// Faster cloth collision for PhysX 2.8.0.7 software cloth (Mirror's Edge tearable plastic sheets).
//
// Profiling the game showed 58% of PhysX's time in torn-cloth scenes inside one function: the per-pair test of
// a cloth vertex's swept segment against one collision shape (RVA 0xF6E10). Its caller (RVA 0xE1BE0) runs it
// for EVERY cloth vertex against EVERY shape inside the cloth's bounding box, and each call first rebuilds the
// shape transform before it can reject a pair that is metres away. Torn cloth spreads over the room, its box
// grows to cover hundreds of the level's static shapes, and the cost explodes (100-150 ms per frame).
//
// Fix: the single call site inside that loop (RVA 0xE1D96) is redirected to a stub that first compares the
// vertex segment's box (inflated by the cloth thickness and a margin) with the shape's world box that PhysX's
// own broadphase keeps for every shape. If they can't touch, the pair is skipped - it could not have produced a
// contact, so the result is identical. Only STATIC shapes are culled (their broadphase box never changes);
// pairs with dynamic shapes, shapes outside the broadphase, unknown shape classes or odd boxes always go to
// PhysX unchanged.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "clothfast.h"

namespace clothfast {

static const ULONG_PTR RVA_CALLSITE  = 0xE1D96;   // call 0x100F6E10 inside 0x100E1BE0 (cloth vertex x shape loop)
static const ULONG_PTR RVA_PAIRTEST  = 0xF6E10;   // cdecl void pairTest(Shape*, const float* seg, int flag)
// Core shape class vtables (RVAs) whose base Shape layout we rely on:
//   +0x128 broadphase object (bounds array at +0x10, 24 bytes = min/max per handle), +0x130 u16 handle,
//   +0x134 body (NULL for static shapes). Verified in Shape::getWorldBounds (RVA 0xC5B0) and the pair test.
static const ULONG_PTR SHAPE_VTBL[] = { 0x27FB2C /*Box*/, 0x2809FC /*Convex*/, 0x28744C /*TriangleMesh*/,
                                       0x27FDEC /*Capsule*/, 0x286954 /*Sphere*/, 0x28198C /*HeightField*/ };

extern "C" {
void* g_cfPairTest = 0;                 // original pair test (jumped to by the stub)
volatile LONG g_cfCalls = 0, g_cfCulled = 0;
}
static ULONG_PTR g_base = 0;
static ULONG_PTR g_vt[sizeof(SHAPE_VTBL) / sizeof(SHAPE_VTBL[0])];

static inline float fmin_(float a, float b) { return a < b ? a : b; }
static inline float fmax_(float a, float b) { return a > b ? a : b; }

// seg (PhysX's local struct): [0..2] old position, [3..5] new position, [6] radius. The pair test compares
// these coordinates directly with the shape's world pose (the caller subtracts an origin vector that is zero in
// the paths observed; whatever it is, the pair test sees exactly these numbers, so we compare the same ones).
// Returns 1 if the pair cannot collide and may be skipped, 0 to run PhysX's test.
extern "C" int __cdecl clothPairCull(const unsigned char* S, const float* seg) {
    ++g_cfCalls;
    if (!S || !seg) return 0;
    if (*(void* const*)(S + 0x134)) return 0;                       // dynamic shape: always test
    ULONG_PTR vt = *(const ULONG_PTR*)S;
    bool known = false;
    for (unsigned i = 0; i < sizeof(g_vt) / sizeof(g_vt[0]); ++i) if (vt == g_vt[i]) { known = true; break; }
    if (!known) return 0;
    unsigned short h = *(const unsigned short*)(S + 0x130);
    if (h == 0xFFFF) return 0;
    const unsigned char* bp = *(const unsigned char* const*)(S + 0x128);
    if (!bp) return 0;
    const float* arr = *(const float* const*)(bp + 0x10);
    if (!arr) return 0;
    const float* b = arr + 6u * h;
    float r = seg[6];
    if (!(r >= 0.0f && r < 10.0f)) return 0;
    float m = 2.0f * r + 0.05f;
    for (int k = 0; k < 3; ++k) {
        float bmn = b[k], bmx = b[3 + k];
        if (!(bmn <= bmx) || bmx - bmn > 1.0e6f) return 0;          // not a sane box (NaN, inverted, huge)
        float lo = fmin_(seg[k], seg[3 + k]) - m, hi = fmax_(seg[k], seg[3 + k]) + m;
        if (hi < bmn || lo > bmx) { ++g_cfCulled; return 1; }
    }
    return 0;
}

// Replaces "call pairTest" at the call site. cdecl: [esp+4] shape, [esp+8] seg, [esp+12] flag.
// Only EAX/ECX/EDX are touched (as any cdecl call may); on "test", jumps to PhysX with the stack untouched.
#if defined(_MSC_VER)
extern "C" __declspec(naked) void clothPairStub() {
    __asm {
        mov  eax, [esp + 8]
        mov  ecx, [esp + 4]
        push eax
        push ecx
        call clothPairCull
        add  esp, 8
        test eax, eax
        jnz  skip
        jmp  dword ptr [g_cfPairTest]
    skip:
        ret
    }
}
#else
#if defined(__ELF__)
#define CF_SYM(x) x            /* offline test build (Linux) */
#else
#define CF_SYM(x) "_" x        /* 32-bit Windows C names */
#endif
extern "C" __attribute__((naked)) void clothPairStub() {
    __asm__ volatile(
        "movl 8(%esp), %eax\n\t"
        "movl 4(%esp), %ecx\n\t"
        "pushl %eax\n\t"
        "pushl %ecx\n\t"
        "call " CF_SYM("clothPairCull") "\n\t"
        "addl $8, %esp\n\t"
        "testl %eax, %eax\n\t"
        "jnz 1f\n\t"
        "jmp *" CF_SYM("g_cfPairTest") "\n\t"
        "1:\n\t"
        "ret\n\t");
}
#endif

bool install(ULONG_PTR coreBase, LogFn log) {
    g_base = coreBase;
    for (unsigned i = 0; i < sizeof(g_vt) / sizeof(g_vt[0]); ++i) g_vt[i] = coreBase + SHAPE_VTBL[i];
    unsigned char* site = (unsigned char*)(coreBase + RVA_CALLSITE);
    ULONG_PTR target = coreBase + RVA_PAIRTEST;
    // Expected bytes: 50 8D 44 24 48 50 56 | E8 rel32 | 83 C4 0C
    static const unsigned char pre[] = { 0x50, 0x8D, 0x44, 0x24, 0x48, 0x50, 0x56 };
    if (memcmp(site - sizeof(pre), pre, sizeof(pre)) != 0 || site[0] != 0xE8 || site[5] != 0x83 || site[6] != 0xC4 || site[7] != 0x0C) {
        log("cloth collision speed-up: call site not recognised; not installed");
        return false;
    }
    LONG rel; memcpy(&rel, site + 1, 4);
    if ((ULONG_PTR)(site + 5 + rel) != target) { log("cloth collision speed-up: call target mismatch; not installed"); return false; }
    g_cfPairTest = (void*)target;
    LONG nrel = (LONG)((ULONG_PTR)&clothPairStub - (ULONG_PTR)(site + 5));
    DWORD old;
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) { log("cloth collision speed-up: VirtualProtect failed"); return false; }
    memcpy(site + 1, &nrel, 4);                                // installed before the first simulate()
    VirtualProtect(site + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 8);
    log("cloth collision speed-up installed (static-shape pre-check in PhysX's cloth vertex/shape loop)");
    return true;
}

void stats(long* calls, long* culled) { *calls = g_cfCalls; *culled = g_cfCulled; }

} // namespace clothfast
