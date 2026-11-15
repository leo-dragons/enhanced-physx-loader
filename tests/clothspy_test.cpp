// Offline test of the NxCloth census thunks (src/clothspy.cpp) on a fake object with __thiscall methods.
#include "windows.h"
extern "C" void* memcpy(void* d, const void* s, unsigned long n) { char* a = (char*)d; const char* b = (const char*)s; while (n--) *a++ = *b++; return d; }
extern "C" void* memset(void* d, int c, unsigned long n) { char* a = (char*)d; while (n--) *a++ = (char)c; return d; }
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_READWRITE 0x04
static inline void* VirtualAlloc(void*, unsigned long n, DWORD, DWORD) {
    long r; __asm__ volatile("pushl %%ebp\n\tmovl $0, %%ebp\n\tint $0x80\n\tpopl %%ebp" : "=a"(r)
        : "a"(192), "b"(0), "c"(4096), "d"(7 /*RWX*/), "S"(0x22 /*PRIVATE|ANON*/), "D"(-1) : "memory");
    return (void*)r; }
static inline BOOL VirtualProtect(void*, unsigned long, DWORD, DWORD* o) { if (o) *o = 0; return TRUE; }
static inline BOOL FlushInstructionCache(HANDLE, const void*, unsigned long) { return TRUE; }
#include "../src/clothspy.cpp"
using namespace clothspy;
static int g_fail = 0;
static void puts_(const char* s) { unsigned long n = 0; while (s[n]) ++n; long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(4), "b"(1), "c"(s), "d"(n) : "memory"); }
#define CHECK(c, msg) do { if (c) puts_("  PASS: " msg "\n"); else { ++g_fail; puts_("  FAIL: " msg "\n"); } } while (0)
static void logf_(const char*, ...) {}
struct Obj { void** vt; int v; };
typedef int (__attribute__((thiscall)) *F2)(Obj*, int, int);
typedef float (__attribute__((thiscall)) *FF)(Obj*);
static int __attribute__((thiscall)) m_add(Obj* o, int a, int b) { return o->v + a * 10 + b; }
static float __attribute__((thiscall)) m_f(Obj* o) { return (float)o->v * 0.5f; }
static void* g_vt[92];
extern "C" __attribute__((force_align_arg_pointer)) void _start() {
    for (int i = 0; i < 92; ++i) g_vt[i] = (void*)&m_add;
    g_vt[7] = (void*)&m_f;
    Obj o; o.vt = g_vt; o.v = 3;
    CHECK(install(&o, logf_), "installed");
    int r = ((F2)o.vt[5])(&o, 4, 2);
    CHECK(r == 3 + 40 + 2, "thiscall method with 2 args returns the right value through the thunk");
    r = ((F2)o.vt[5])(&o, 1, 1);
    float f = ((FF)o.vt[7])(&o);
    CHECK(f == 1.5f, "float-returning method works through the thunk");
    CHECK(g_count[5] == 2 && g_count[7] == 1, "calls counted per slot");
    CHECK(g_firstArgs[5][0] == 4 && g_firstArgs[5][1] == 2 && g_firstThis[5] == &o, "first-call arguments and this recorded");
    puts_(g_fail ? "RESULT: FAILURES\n" : "RESULT: all checks passed\n");
    __asm__ volatile("int $0x80" :: "a"(1), "b"(g_fail ? 1 : 0));
    for (;;) {}
}
