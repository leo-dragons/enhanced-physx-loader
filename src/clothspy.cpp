// Cloth API census ([Logging] ClothSpy=1): which NxCloth methods the game calls, how often, from where and with
// which first arguments. Diagnostics for the cloth replacement; does not change behaviour.
//
// Every entry of the NpCloth vtable (92 slots in PhysXCore 2.8.0.7) is redirected to a 10-byte thunk
// "push slot; jmp common". The common stub records the call and jumps to the original function with the
// stack and ECX untouched (EAX is free at entry of a __thiscall method).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "clothspy.h"

namespace clothspy {

static const int NSLOT = 92;
static LogFn g_log = 0;
static void* g_orig[NSLOT];
static volatile LONG g_count[NSLOT];
static volatile LONG g_firstDone[NSLOT];
static ULONG_PTR g_firstRet[NSLOT];
static ULONG g_firstArgs[NSLOT][6];
static void* g_firstThis[NSLOT];
static unsigned char* g_thunks = 0;
static volatile LONG g_installed = 0;

extern "C" void* __cdecl clothSpyRecord(ULONG* frame, void* self) {
    // frame[0] = slot, frame[1] = return address, frame[2..] = stack arguments
    ULONG slot = frame[0];
    if (slot >= (ULONG)NSLOT) return 0;
    InterlockedIncrement(&g_count[slot]);
    if (InterlockedCompareExchange(&g_firstDone[slot], 1, 0) == 0) {
        g_firstRet[slot] = frame[1];
        g_firstThis[slot] = self;
        for (int i = 0; i < 6; ++i) g_firstArgs[slot][i] = frame[2 + i];
        InterlockedExchange(&g_firstDone[slot], 2);
    }
    return g_orig[slot];
}

#if defined(_MSC_VER)
extern "C" __declspec(naked) void clothSpyCommon() {
    __asm {
        push ecx
        push edx
        push ecx
        lea  eax, [esp + 12]
        push eax
        call clothSpyRecord
        add  esp, 8
        pop  edx
        pop  ecx
        add  esp, 4
        jmp  eax
    }
}
#else
#if defined(__ELF__)
#define CS_SYM(x) x
#else
#define CS_SYM(x) "_" x
#endif
extern "C" __attribute__((naked)) void clothSpyCommon() {
    __asm__ volatile(
        "pushl %ecx\n\t"
        "pushl %edx\n\t"
        "pushl %ecx\n\t"                 // self
        "leal 12(%esp), %eax\n\t"        // frame: [slot][ret][args...]
        "pushl %eax\n\t"
        "call " CS_SYM("clothSpyRecord") "\n\t"
        "addl $8, %esp\n\t"
        "popl %edx\n\t"
        "popl %ecx\n\t"
        "addl $4, %esp\n\t"              // drop slot
        "jmp *%eax\n\t");
}
#endif

static void describe(ULONG_PTR a, char* out, int n) {
    HMODULE m = 0; wchar_t path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m)
        GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* base = path; for (const wchar_t* q = path; *q; ++q) if (*q == L'\\' || *q == L'/') base = q + 1;
    int k = 0; for (; base[k] && k < n - 16; ++k) out[k] = (char)base[k];
    out[k] = 0;
    ULONG_PTR off = m ? a - (ULONG_PTR)m : a;
    char t[16]; int ti = 0; const char hex[] = "0123456789abcdef";
    do { t[ti++] = hex[off & 15]; off >>= 4; } while (off && ti < 8);
    out[k++] = '+'; out[k++] = '0'; out[k++] = 'x'; while (ti) out[k++] = t[--ti]; out[k] = 0;
}

bool install(void* cloth, LogFn log) {
    if (!cloth || InterlockedCompareExchange(&g_installed, 1, 0) != 0) return false;
    g_log = log;
    void** vt = *(void***)cloth;
    g_thunks = (unsigned char*)VirtualAlloc(NULL, NSLOT * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_thunks) { log("clothspy: VirtualAlloc failed"); return false; }
    for (int i = 0; i < NSLOT; ++i) {
        unsigned char* t = g_thunks + i * 16;
        t[0] = 0x68; ULONG s = (ULONG)i; memcpy(t + 1, &s, 4);                 // push slot
        t[5] = 0xE9; LONG rel = (LONG)((ULONG_PTR)&clothSpyCommon - (ULONG_PTR)(t + 10)); memcpy(t + 6, &rel, 4);
        g_orig[i] = vt[i];
    }
    DWORD old;
    if (!VirtualProtect(vt, NSLOT * sizeof(void*), PAGE_READWRITE, &old)) { log("clothspy: VirtualProtect failed"); return false; }
    for (int i = 0; i < NSLOT; ++i) vt[i] = g_thunks + i * 16;
    VirtualProtect(vt, NSLOT * sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_thunks, NSLOT * 16);
    log("clothspy: NpCloth vtable %p instrumented (%d slots)", vt, NSLOT);
    return true;
}

void report(const char* when) {
    if (!g_log || !g_installed) return;
    g_log("clothspy %s: NxCloth calls by vtable slot", when);
    for (int i = 0; i < NSLOT; ++i) {
        if (!g_count[i]) continue;
        char where[MAX_PATH + 32] = "?";
        if (g_firstDone[i] == 2) describe(g_firstRet[i], where, sizeof(where));
        g_log("  slot %2d: %8ld calls; first from %s this %p args %08lx %08lx %08lx %08lx %08lx %08lx", i, g_count[i], where,
              g_firstThis[i], g_firstArgs[i][0], g_firstArgs[i][1], g_firstArgs[i][2], g_firstArgs[i][3], g_firstArgs[i][4], g_firstArgs[i][5]);
    }
}

} // namespace clothspy
