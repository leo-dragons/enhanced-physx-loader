// Offline test of the cloth collision pre-check (src/clothfast.cpp) on a fake PhysXCore image.
#include "windows.h"
extern "C" void* memcpy(void* d, const void* s, unsigned long n) { char* a = (char*)d; const char* b = (const char*)s; while (n--) *a++ = *b++; return d; }
extern "C" void* memset(void* d, int c, unsigned long n) { char* a = (char*)d; while (n--) *a++ = (char)c; return d; }
extern "C" int memcmp(const void* x, const void* y, unsigned long n) { const unsigned char* a = (const unsigned char*)x; const unsigned char* b = (const unsigned char*)y; for (; n; --n, ++a, ++b) if (*a != *b) return *a - *b; return 0; }
#define PAGE_EXECUTE_READWRITE 0x40
static inline BOOL VirtualProtect(void*, unsigned long, DWORD, DWORD* o) { if (o) *o = 0; return TRUE; }
static inline BOOL FlushInstructionCache(HANDLE, const void*, unsigned long) { return TRUE; }
#include "../src/clothfast.cpp"
using namespace clothfast;

extern "C" long write(int, const void*, unsigned long);
extern "C" void exit_(int);
static int g_fail = 0, g_pass = 0;
static void puts_(const char* s) { unsigned long n = 0; while (s[n]) ++n; long r = 0;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(4), "b"(1), "c"(s), "d"(n) : "memory"); }
#define CHECK(c, msg) do { if (c) { ++g_pass; puts_("  PASS: " msg "\n"); } else { ++g_fail; puts_("  FAIL: " msg "\n"); } } while (0)
static void logf_(const char* f, ...) { puts_("  [log] "); puts_(f); puts_("\n"); }

static unsigned char g_core[0x300000] __attribute__((aligned(4096)));
static int g_realCalls = 0; static const void* g_lastShape = 0;
extern "C" void __attribute__((cdecl)) fakePairTest(const void* S, const float*, int) { ++g_realCalls; g_lastShape = S; }

struct FakeShape { unsigned char m[0x200]; };
static float g_bounds[6 * 4];
static unsigned char g_bp[0x40];

static void mkShape(FakeShape& s, ULONG_PTR vtRva, unsigned short h, void* body) {
    memset(&s, 0, sizeof(s));
    *(ULONG_PTR*)s.m = (ULONG_PTR)g_core + vtRva;
    *(unsigned short*)(s.m + 0x130) = h;
    *(void**)(s.m + 0x128) = g_bp;
    *(void**)(s.m + 0x134) = body;
}

extern "C" __attribute__((force_align_arg_pointer)) void _start() {
    // fake call site: 50 8D 44 24 48 50 56 E8 rel32 83 C4 0C
    unsigned char* site = g_core + 0xE1D96;
    const unsigned char pre[] = { 0x50, 0x8D, 0x44, 0x24, 0x48, 0x50, 0x56 };
    memcpy(site - 7, pre, 7); site[0] = 0xE8;
    LONG rel = (LONG)((g_core + 0xF6E10) - (site + 5)); memcpy(site + 1, &rel, 4);
    site[5] = 0x83; site[6] = 0xC4; site[7] = 0x0C;
    puts_("== install\n");
    bool ok = clothfast::install((ULONG_PTR)g_core, logf_);
    LONG nrel; memcpy(&nrel, site + 1, 4);
    CHECK(ok && (ULONG_PTR)(site + 5 + nrel) == (ULONG_PTR)&clothPairStub, "call site now targets the stub");
    CHECK(g_cfPairTest == (void*)(g_core + 0xF6E10), "original pair test remembered");
    unsigned char bad[16]; memset(bad, 0, sizeof bad);
    // bounds: shape 0 = unit box at origin, shape 1 = inverted, shape 2 = box at x 10..11
    float b0[6] = { 0, 0, 0, 1, 1, 1 }, b1[6] = { 1, 1, 1, 0, 0, 0 }, b2[6] = { 10, 0, 0, 11, 1, 1 };
    memcpy(g_bounds, b0, 24); memcpy(g_bounds + 6, b1, 24); memcpy(g_bounds + 12, b2, 24);
    *(float**)(g_bp + 0x10) = g_bounds;
    static FakeShape sBox, sInv, sFar, sDyn, sUnk, sNoBp;
    int dummyBody;
    mkShape(sBox, 0x27FB2C, 0, 0); mkShape(sInv, 0x2809FC, 1, 0); mkShape(sFar, 0x28744C, 2, 0);
    mkShape(sDyn, 0x27FB2C, 2, &dummyBody); mkShape(sUnk, 0x123456, 2, 0); mkShape(sNoBp, 0x27FB2C, 0xFFFF, 0);
    float segNear[8] = { 0.5f, 0.5f, 1.02f, 0.5f, 0.5f, 1.10f, 0.01f, 0.01f };   // just above the unit box (within margin)
    float segFar[8]  = { 5, 5, 5, 5.1f, 5, 5, 0.01f, 0.01f };
    float segX[8]    = { 9.5f, 0.5f, 0.5f, 10.5f, 0.5f, 0.5f, 0.01f, 0.01f };      // swept into the far box
    puts_("== pre-check\n");
    CHECK(clothPairCull(sBox.m, segNear) == 0, "vertex near a static box is tested");
    CHECK(clothPairCull(sBox.m, segFar) == 1, "vertex far from a static box is skipped");
    CHECK(clothPairCull(sFar.m, segX) == 0, "segment sweeping into a box is tested");
    CHECK(clothPairCull(sFar.m, segNear) == 1, "far box skipped for a vertex near the origin");
    CHECK(clothPairCull(sInv.m, segFar) == 0, "inverted (bogus) box never skipped");
    CHECK(clothPairCull(sDyn.m, segNear) == 0 && clothPairCull(sDyn.m, segFar) == 0, "dynamic shapes are never skipped");
    CHECK(clothPairCull(sUnk.m, segFar) == 0, "unknown shape class never skipped");
    CHECK(clothPairCull(sNoBp.m, segFar) == 0, "shape outside the broadphase never skipped");
    float segNaN[8] = { 5, 5, 5, 5, 5, 5, 0, 0 }; unsigned nan = 0x7FC00000; memcpy(&segNaN[6], &nan, 4);
    CHECK(clothPairCull(sBox.m, segNaN) == 0, "NaN radius never skipped");
    puts_("== stub\n");
    g_cfPairTest = (void*)&fakePairTest;
    typedef void (*Stub)(const void*, const float*, int);
    Stub st = (Stub)&clothPairStub;
    g_realCalls = 0; st(sBox.m, segFar, 1);
    CHECK(g_realCalls == 0, "stub skips an impossible pair");
    st(sBox.m, segNear, 1);
    CHECK(g_realCalls == 1 && g_lastShape == sBox.m, "stub forwards a possible pair with the original arguments");
    st(sDyn.m, segFar, 0);
    CHECK(g_realCalls == 2 && g_lastShape == sDyn.m, "stub forwards dynamic-shape pairs");
    puts_(g_fail ? "RESULT: FAILURES\n" : "RESULT: all checks passed\n");
    __asm__ volatile("int $0x80" :: "a"(1), "b"(g_fail ? 1 : 0));
    for (;;) {}
}
