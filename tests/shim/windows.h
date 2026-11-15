// Minimal freestanding Windows shim for testing fluid.cpp as a 32-bit Linux program (single-threaded).
#pragma once
typedef unsigned long  DWORD; typedef long LONG; typedef long long LONGLONG; typedef unsigned long ULONG;
typedef unsigned long  ULONG_PTR; typedef unsigned long SIZE_T; typedef int BOOL; typedef void* HANDLE; typedef void* LPVOID;
typedef const void* LPCVOID; typedef wchar_t WCHAR; typedef unsigned char BYTE;
typedef __SIZE_TYPE__ size_t;
#define WINAPI __attribute__((stdcall))
#define __fastcall __attribute__((fastcall))
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFF
#define TRUE 1
#define FALSE 0
#define NULL 0
#define HEAP_ZERO_MEMORY 8
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define THREAD_PRIORITY_ABOVE_NORMAL 1
typedef union { struct { DWORD lo; LONG hi; } u; LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { volatile int x; } CRITICAL_SECTION;
typedef struct { DWORD dwNumberOfProcessors; } SYSTEM_INFO;
typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)(LPVOID);
extern "C" void* memcpy(void*, const void*, unsigned long);
extern "C" void* memset(void*, int, unsigned long);
extern "C" int memcmp(const void*, const void*, unsigned long);
void* shim_alloc(unsigned long n);
static inline HANDLE GetProcessHeap() { return (HANDLE)1; }
static inline void* HeapAlloc(HANDLE, DWORD, SIZE_T n) { void* p = shim_alloc(n); memset(p, 0, n); return p; }
static inline BOOL HeapFree(HANDLE, DWORD, void*) { return TRUE; }
static inline void InitializeCriticalSection(CRITICAL_SECTION*) {}
static inline void DeleteCriticalSection(CRITICAL_SECTION*) {}
static inline void EnterCriticalSection(CRITICAL_SECTION* c) { while (__sync_lock_test_and_set(&c->x, 1)) {} }
static inline void LeaveCriticalSection(CRITICAL_SECTION* c) { __sync_lock_release(&c->x); }
static inline LONG InterlockedIncrement(volatile LONG* p) { return __sync_add_and_fetch(p, 1); }
static inline LONG InterlockedDecrement(volatile LONG* p) { return __sync_sub_and_fetch(p, 1); }
static inline LONG InterlockedExchange(volatile LONG* p, LONG v) { return __sync_lock_test_and_set(p, v); }
static inline LONG InterlockedCompareExchange(volatile LONG* p, LONG x, LONG c) { return __sync_val_compare_and_swap(p, c, x); }
static inline LONG InterlockedExchangeAdd(volatile LONG* p, LONG v) { return __sync_fetch_and_add(p, v); }
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER* li) { unsigned lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); li->QuadPart = ((LONGLONG)hi << 32) | lo; return TRUE; }
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* li) { li->QuadPart = 3000000000LL; return TRUE; }

#ifdef SHIM_MT
// --- real threads via clone(2), spin-based sync objects ---
struct ShimObj { int type; volatile long count; volatile long flag; int manual; };
static inline long shim_sys(long n, long a, long b, long c) { long r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory"); return r; }
static inline void shim_yield() { shim_sys(158, 0, 0, 0); }
struct ShimThreadStart { LPTHREAD_START_ROUTINE fn; LPVOID arg; };
extern "C" void shim_thread_entry(ShimThreadStart* st);
static inline HANDLE CreateThread(void*, SIZE_T, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD, DWORD*) {
    ShimThreadStart* st = (ShimThreadStart*)shim_alloc(sizeof(ShimThreadStart)); st->fn = fn; st->arg = arg;
    const unsigned long SZ = 256 * 1024; char* stack = (char*)shim_alloc(SZ);
    // after "pop fn" esp points at st; it must be 16-byte aligned at the call (i386 SysV ABI, SSE spills)
    unsigned* top = (unsigned*)(((unsigned long)(stack + SZ - 64)) & ~15ul);   // shim_alloc is not 16-byte aligned
    unsigned* sp = top - 5;            // sp[0] = fn, sp[1] = st  -> &sp[1] == top - 4 (16-byte aligned)
    sp[0] = (unsigned)&shim_thread_entry; sp[1] = (unsigned)st;
    long flags = 0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000;   // VM FS FILES SIGHAND THREAD SYSVSEM
    long ret;
    __asm__ volatile("int $0x80\n\t"
                     "test %%eax, %%eax\n\t"
                     "jnz 1f\n\t"
                     "popl %%eax\n\t"
                     "call *%%eax\n\t"
                     "movl $1, %%eax\n\t"
                     "xorl %%ebx, %%ebx\n\t"
                     "int $0x80\n\t"
                     "1:\n\t"
                     : "=a"(ret) : "a"(120), "b"(flags), "c"(sp), "d"(0), "S"(0), "D"(0) : "memory");
    return ret > 0 ? (HANDLE)ret : 0;
}
static inline HANDLE CreateSemaphoreW(void*, LONG init, LONG, const WCHAR*) { ShimObj* o = (ShimObj*)shim_alloc(sizeof(ShimObj)); o->type = 1; o->count = init; return o; }
static inline BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG*) { __sync_fetch_and_add(&((ShimObj*)h)->count, n); return TRUE; }
static inline HANDLE CreateEventW(void*, BOOL manual, BOOL init, const WCHAR*) { ShimObj* o = (ShimObj*)shim_alloc(sizeof(ShimObj)); o->type = 2; o->flag = init; o->manual = manual; return o; }
static inline BOOL SetEvent(HANDLE h) { __sync_lock_test_and_set(&((ShimObj*)h)->flag, 1); return TRUE; }
static inline BOOL ResetEvent(HANDLE h) { __sync_lock_test_and_set(&((ShimObj*)h)->flag, 0); return TRUE; }
static inline DWORD WaitForSingleObject(HANDLE h, DWORD) {
    ShimObj* o = (ShimObj*)h;
    for (;;) {
        if (o->type == 1) { long c = o->count; if (c > 0 && __sync_bool_compare_and_swap(&o->count, c, c - 1)) return 0; }
        else if (o->flag) return 0;
        shim_yield();
    }
}
static inline void GetSystemInfo(SYSTEM_INFO* si) { si->dwNumberOfProcessors = 6; }
typedef struct { volatile long lock; } SHIM_CS;
#else
static inline HANDLE CreateSemaphoreW(void*, LONG, LONG, const WCHAR*) { return (HANDLE)2; }
static inline BOOL ReleaseSemaphore(HANDLE, LONG, LONG*) { return TRUE; }
static inline HANDLE CreateEventW(void*, BOOL, BOOL, const WCHAR*) { return (HANDLE)3; }
static inline BOOL SetEvent(HANDLE) { return TRUE; }
static inline BOOL ResetEvent(HANDLE) { return TRUE; }
static inline DWORD WaitForSingleObject(HANDLE, DWORD) { return 0; }
static inline HANDLE CreateThread(void*, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, DWORD*) { return 0; }  // no workers
static inline void GetSystemInfo(SYSTEM_INFO* si) { si->dwNumberOfProcessors = 1; }
#endif
static inline BOOL SetThreadPriority(HANDLE, int) { return TRUE; }
static inline HANDLE GetCurrentProcess() { return (HANDLE)-1; }
static inline BOOL ReadProcessMemory(HANDLE, LPCVOID src, void* dst, SIZE_T n, SIZE_T* got) { if ((unsigned long)src < 0x10000) { if (got) *got = 0; return FALSE; } memcpy(dst, src, n); if (got) *got = n; return TRUE; }   // low addresses: unreadable, like Windows
static inline WCHAR* lstrcpynW(WCHAR* d, const WCHAR* s, int n) { int i = 0; for (; i < n - 1 && s[i]; ++i) d[i] = s[i]; d[i] = 0; return d; }
static inline WCHAR* lstrcatW(WCHAR* d, const WCHAR* s) { int i = 0; while (d[i]) ++i; int j = 0; while (s[j]) d[i++] = s[j++]; d[i] = 0; return d; }
static inline DWORD GetFileAttributesW(const WCHAR*) { return INVALID_FILE_ATTRIBUTES; }
typedef void* HMODULE; typedef const WCHAR* LPCWSTR;
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
static inline BOOL GetModuleHandleExW(DWORD, LPCWSTR, HMODULE* m) { *m = 0; return FALSE; }
static inline DWORD GetModuleFileNameW(HMODULE, WCHAR* p, DWORD) { p[0] = 0; return 0; }
// VirtualQuery (1.1.14 packet output checks the game's buffers): addresses below 64 KB are unmapped, the rest is RW
typedef struct { void* BaseAddress; void* AllocationBase; DWORD AllocationProtect; SIZE_T RegionSize; DWORD State, Protect, Type; } MEMORY_BASIC_INFORMATION;
#define MEM_COMMIT 0x1000
#define PAGE_NOACCESS 0x01
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100
extern unsigned long g_shimNoAccessLo, g_shimNoAccessHi;   // tests can mark one range unwritable (defined in test.cpp)
static inline SIZE_T VirtualQuery(const void* p, MEMORY_BASIC_INFORMATION* m, SIZE_T) {
    unsigned long a = (unsigned long)p;
    memset(m, 0, sizeof(*m));
    if (a < 0x10000) { m->BaseAddress = 0; m->RegionSize = 0x10000; m->State = 0x10000; m->Protect = PAGE_NOACCESS; return sizeof(*m); }
    if (g_shimNoAccessHi && a >= g_shimNoAccessLo && a < g_shimNoAccessHi) { m->BaseAddress = (void*)g_shimNoAccessLo; m->RegionSize = g_shimNoAccessHi - g_shimNoAccessLo; m->State = MEM_COMMIT; m->Protect = PAGE_NOACCESS; return sizeof(*m); }
    unsigned long base = a & ~0xFFFUL, end = (g_shimNoAccessHi && g_shimNoAccessLo > a) ? g_shimNoAccessLo : base + 0x100000;
    m->BaseAddress = (void*)base; m->RegionSize = end - base; m->State = MEM_COMMIT; m->Protect = PAGE_READWRITE; return sizeof(*m);
}
