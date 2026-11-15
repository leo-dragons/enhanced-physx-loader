// Freestanding 32-bit Linux test for fluid.cpp: fake PhysX scene + scenario checks.
#include <windows.h>
#include <stdarg.h>
#include "../src/fluid.h"

// ---- runtime ----------------------------------------------------------------
extern "C" void* memcpy(void* d, const void* s, unsigned long n) { char* a = (char*)d; const char* b = (const char*)s; while (n--) *a++ = *b++; return d; }
extern "C" void* memset(void* d, int c, unsigned long n) { char* a = (char*)d; while (n--) *a++ = (char)c; return d; }
extern "C" int memcmp(const void* x, const void* y, unsigned long n) { const unsigned char* a = (const unsigned char*)x; const unsigned char* b = (const unsigned char*)y; for (; n; --n, ++a, ++b) if (*a != *b) return *a - *b; return 0; }
static char g_heap[64 << 20]; static unsigned long g_heapUsed = 0;
void* shim_alloc(unsigned long n) { n = (n + 15) & ~15ul; unsigned long at = __sync_fetch_and_add(&g_heapUsed, n); if (at + n > sizeof(g_heap)) { for (;;) {} } return g_heap + at; }
#ifdef SHIM_MT
extern "C" void shim_thread_entry(ShimThreadStart* st) { st->fn(st->arg); }
#endif
static long sys(long n, long a, long b, long c) { long r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory"); return r; }
static void out(const char* s, int n) { sys(4, 1, (long)s, n); }
static int slen(const char* s) { int n = 0; while (s[n]) ++n; return n; }

struct Buf { char b[2048]; int n; void c(char ch) { if (n < 2046) b[n++] = ch; } void s(const char* t) { while (t && *t) c(*t++); } };
static void putU(Buf& o, unsigned long v, int base, int width, char pad) {
    char t[24]; int n = 0; do { int d = (int)(v % base); t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v);
    while (n < width) t[n++] = pad;
    while (n) o.c(t[--n]);
}
static void putF(Buf& o, double v, int prec) {
    if (v != v) { o.s("nan"); return; }
    if (v < 0) { o.c('-'); v = -v; }
    if (v > 1e15) { o.s("big"); return; }
    double r = 0.5; for (int i = 0; i < prec; ++i) r /= 10; v += r;
    unsigned long ip = (unsigned long)v; putU(o, ip, 10, 0, ' ');
    if (prec) { o.c('.'); double fr = v - ip; for (int i = 0; i < prec; ++i) { fr *= 10; int d = (int)fr; o.c((char)('0' + d)); fr -= d; } }
}
static void vfmt(Buf& o, const char* f, va_list ap) {
    for (; *f; ++f) {
        if (*f != '%') { o.c(*f); continue; }
        ++f; char pad = ' '; int width = 0, prec = 3;
        if (*f == '-') ++f;
        if (*f == '0') { pad = '0'; ++f; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') { ++f; prec = 0; while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
        bool l = false, ll = false; while (*f == 'l') { if (l) ll = true; l = true; ++f; }
        switch (*f) {
            case 'd': case 'i': { long long v = ll ? va_arg(ap, long long) : (long long)va_arg(ap, long); if (v < 0) { o.c('-'); v = -v; } putU(o, (unsigned long)v, 10, width, pad); break; }
            case 'u': { unsigned long long v = ll ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned long); putU(o, (unsigned long)v, 10, width, pad); break; }
            case 'x': putU(o, va_arg(ap, unsigned long), 16, width, pad); break;
            case 'p': o.s("0x"); putU(o, (unsigned long)va_arg(ap, void*), 16, 8, '0'); break;
            case 's': if (l) { const wchar_t* w = va_arg(ap, const wchar_t*); while (w && *w) o.c((char)*w++); } else o.s(va_arg(ap, const char*)); break;
            case 'f': case 'g': putF(o, va_arg(ap, double), *f == 'g' ? 3 : prec); break;
            case '%': o.c('%'); break;
            default: o.c('?');
        }
    }
}
static void say(const char* f, ...) { Buf o; o.n = 0; va_list ap; va_start(ap, f); vfmt(o, f, ap); va_end(ap); o.c('\n'); out(o.b, o.n); }
static bool g_quietLog = false;
static void logfn(const char* f, ...) { if (g_quietLog) return; Buf o; o.n = 0; o.s("  [log] "); va_list ap; va_start(ap, f); vfmt(o, f, ap); va_end(ap); o.c('\n'); out(o.b, o.n); }
unsigned long g_shimNoAccessLo = 0, g_shimNoAccessHi = 0;
static int g_fail = 0; static int g_inSlab = 0, g_inSlabShown = 0;
#define CHECK(cond, ...) do { if (cond) say("  PASS: " __VA_ARGS__); else { say("  FAIL: " __VA_ARGS__); ++g_fail; } } while (0)

// ---- math -----------------------------------------------------------------------
struct V3 { float x, y, z; };
static float fsq(float x) { float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }
static float cosA(float a) { float r; __asm__("fcos" : "=t"(r) : "0"(a)); return r; }
static float sinA(float a) { float r; __asm__("fsin" : "=t"(r) : "0"(a)); return r; }
struct GMask { unsigned b[4]; };
struct B3 { V3 mn, mx; };

// ---- fake PhysX objects ------------------------------------------------------------
#define FC __fastcall
struct Shape { void** vt; int type; float pose[12]; V3 dims; float radius, height; unsigned short group; GMask mask; unsigned flags; void* mesh; };
struct Mesh  { void** vt; int nv; float* verts; };
struct Actor { void** vt; int dynamic; int ns; Shape** shapes; unsigned flags; };
struct Scene { void** vt; int na; Actor** actors; V3 g; };

// row-major pose: rotation about Z by angle a, translation t
static void setPose(float* m, float a, V3 t) {
    float c = cosA(a), s = sinA(a);
    float r[9] = { c, -s, 0, s, c, 0, 0, 0, 1 };
    for (int i = 0; i < 9; ++i) m[i] = r[i];
    m[9] = t.x; m[10] = t.y; m[11] = t.z;
}
static V3 axisOf(const float* m, int i) { V3 v = { m[i], m[3 + i], m[6 + i] }; return v; }

static unsigned FC sh_getGroup(void* s, void*) { return ((Shape*)s)->group; }
static void FC sh_getWorldBounds(void* sv, void*, B3* b) {
    Shape* s = (Shape*)sv;
    V3 c = { s->pose[9], s->pose[10], s->pose[11] };
    V3 h;
    if (s->type == 2) {
        V3 a0 = axisOf(s->pose, 0), a1 = axisOf(s->pose, 1), a2 = axisOf(s->pose, 2);
        #define AB(x) ((x) < 0 ? -(x) : (x))
        h.x = AB(a0.x) * s->dims.x + AB(a1.x) * s->dims.y + AB(a2.x) * s->dims.z;
        h.y = AB(a0.y) * s->dims.x + AB(a1.y) * s->dims.y + AB(a2.y) * s->dims.z;
        h.z = AB(a0.z) * s->dims.x + AB(a1.z) * s->dims.y + AB(a2.z) * s->dims.z;
    } else if (s->type == 5) {
        Mesh* m = (Mesh*)s->mesh; B3 bb = { { 1e30f, 1e30f, 1e30f }, { -1e30f, -1e30f, -1e30f } };
        for (int i = 0; i < m->nv; ++i) {
            float* p = m->verts + 3 * i;
            V3 w = { s->pose[9] + s->pose[0] * p[0] + s->pose[1] * p[1] + s->pose[2] * p[2],
                     s->pose[10] + s->pose[3] * p[0] + s->pose[4] * p[1] + s->pose[5] * p[2],
                     s->pose[11] + s->pose[6] * p[0] + s->pose[7] * p[1] + s->pose[8] * p[2] };
            if (w.x < bb.mn.x) bb.mn.x = w.x; if (w.y < bb.mn.y) bb.mn.y = w.y; if (w.z < bb.mn.z) bb.mn.z = w.z;
            if (w.x > bb.mx.x) bb.mx.x = w.x; if (w.y > bb.mx.y) bb.mx.y = w.y; if (w.z > bb.mx.z) bb.mx.z = w.z;
        }
        *b = bb; return;
    } else { float r = s->radius + s->height; h.x = h.y = h.z = r; }
    b->mn.x = c.x - h.x; b->mn.y = c.y - h.y; b->mn.z = c.z - h.z; b->mx.x = c.x + h.x; b->mx.y = c.y + h.y; b->mx.z = c.z + h.z;
}
static unsigned FC sh_getFlag(void* s, void*, unsigned mask) { return ((Shape*)s)->flags & mask; }
static float* FC sh_getGlobalPose(void* s, void*, float* out) { for (int i = 0; i < 12; ++i) out[i] = ((Shape*)s)->pose[i]; return out; }
static unsigned FC sh_getType(void* s, void*) { return ((Shape*)s)->type; }
static GMask* FC sh_getGroupsMask(void* s, void*, GMask* out) { *out = ((Shape*)s)->mask; return out; }
static V3* FC sh_getDims(void* s, void*, V3* out) { *out = ((Shape*)s)->dims; return out; }       // box slot 40
static void* FC sh_getMesh(void* s, void*) { return ((Shape*)s)->mesh; }                             // convex slot 40
static float FC sh_capRadius(void* s, void*) { return ((Shape*)s)->radius; }
static float FC sh_capHeight(void* s, void*) { return ((Shape*)s)->height; }
static void* g_boxVt[64]; static void* g_cvxVt[64]; static void* g_capVt[64];

static unsigned FC m_getCount(void* m, void*, unsigned, unsigned arr) { return arr == 1 ? ((Mesh*)m)->nv : 0; }
static unsigned FC m_getFormat(void*, void*, unsigned, unsigned) { return 1; }
static const void* FC m_getBase(void* m, void*, unsigned, unsigned) { return ((Mesh*)m)->verts; }
static unsigned FC m_getStride(void*, void*, unsigned, unsigned) { return 12; }
static void* g_meshVt[16];

static unsigned FC a_getNbShapes(void* a, void*) { return ((Actor*)a)->ns; }
static void* FC a_getShapes(void* a, void*) { return ((Actor*)a)->shapes; }
static bool FC a_isDynamic(void* a, void*) { return ((Actor*)a)->dynamic != 0; }
static bool FC a_readActorFlag(void* a, void*, unsigned f) { return (((Actor*)a)->flags & f) != 0; }   // NxActor slot 27
static void* g_actorVt[110];

static void FC s_getGravity(void* s, void*, V3* g) { *g = ((Scene*)s)->g; }
static bool FC s_groupFlag(void*, void*, unsigned short, unsigned short) { return true; }
static void FC s_filterOps(void*, void*, int* a, int* b, int* c) { *a = 1; *b = 1; *c = 6; }   // OR, OR, SWAP_AND (as in the game)
static bool FC s_filterBool(void*, void*) { return true; }
static GMask* FC s_filterK(void*, void*, GMask* o) { memset(o, 0, 16); return o; }
static unsigned FC s_getNbActors(void* s, void*) { return ((Scene*)s)->na; }
static void* FC s_getActors(void* s, void*) { return ((Scene*)s)->actors; }
static void* g_sceneVt[160];

static void setupVtables() {
    void* common[] = { 0, 0, 0, (void*)sh_getGroup, (void*)sh_getWorldBounds, 0, (void*)sh_getFlag };
    void** vts[3] = { g_boxVt, g_cvxVt, g_capVt };
    for (int k = 0; k < 3; ++k) {
        for (int i = 0; i < 7; ++i) vts[k][i] = common[i];
        vts[k][16] = (void*)sh_getGlobalPose; vts[k][23] = (void*)sh_getType; vts[k][36] = (void*)sh_getGroupsMask;
    }
    g_boxVt[40] = (void*)sh_getDims; g_cvxVt[40] = (void*)sh_getMesh;
    g_capVt[41] = (void*)sh_capRadius; g_capVt[43] = (void*)sh_capHeight;
    g_meshVt[2] = (void*)m_getCount; g_meshVt[3] = (void*)m_getFormat; g_meshVt[4] = (void*)m_getBase; g_meshVt[5] = (void*)m_getStride;
    g_actorVt[19] = (void*)a_getNbShapes; g_actorVt[20] = (void*)a_getShapes; g_actorVt[29] = (void*)a_isDynamic; g_actorVt[27] = (void*)a_readActorFlag;
    g_sceneVt[6] = (void*)s_getGravity; g_sceneVt[48] = (void*)s_groupFlag; g_sceneVt[59] = (void*)s_filterOps;
    g_sceneVt[60] = (void*)s_filterBool; g_sceneVt[61] = (void*)s_filterK; g_sceneVt[62] = (void*)s_filterK;
    g_sceneVt[63] = (void*)s_getNbActors; g_sceneVt[64] = (void*)s_getActors;
}

// ---- game-side buffers (32-bit layout, as Mirror's Edge uses them) ----------------------
struct PData   { unsigned* num; float* pos; float* vel; float* life; float* dens; unsigned* id; unsigned* flag; float* nrm;
                 unsigned sPos, sVel, sLife, sDens, sId, sFlag, sNrm; const char* name; };
struct PIdData { unsigned* num; unsigned* id; unsigned sId; const char* name; };
struct PUpdate { unsigned forceMode; unsigned num; float* force; unsigned* flag; unsigned* id; unsigned sForce, sFlag, sId; };
struct Rec32 { float p[3], v[3]; unsigned id, pad; };
static const int MAXP = 64;
static Rec32 g_out[MAXP]; static float g_nrm[MAXP * 3]; static unsigned g_outNum, g_delNum, g_creNum, g_del[MAXP], g_cre[MAXP];

typedef unsigned (FC *FN_add)(void*, void*, const PData*, bool);
typedef void (FC *FN_upd)(void*, void*, const PUpdate*);
typedef void (FC *FN_v)(void*, void*);
typedef void (FC *FN_bounds)(void*, void*, B3*);
template <class F> static F slot(void* o, int i) { return (F)((*(void***)o)[i]); }

static void buildDesc(unsigned char* d, float damping, V3 ext, unsigned flags) {
    memset(d, 0, 0x160);
    unsigned u; float f;
    u = MAXP; memcpy(d + 0x40, &u, 4); u = 0; memcpy(d + 0x44, &u, 4);
    f = 0.78125f; memcpy(d + 0x48, &f, 4); f = 1000; memcpy(d + 0x4C, &f, 4); f = 2; memcpy(d + 0x50, &f, 4);
    f = 0.25f; memcpy(d + 0x54, &f, 4); f = 0.03125f; memcpy(d + 0x58, &f, 4); u = 4; memcpy(d + 0x5C, &u, 4);
    f = damping; memcpy(d + 0x6C, &f, 4); memcpy(d + 0x74, &ext, 12);
    f = 0.2f; memcpy(d + 0x90, &f, 4); f = 0.05f; memcpy(d + 0x94, &f, 4); memcpy(d + 0x98, &f, 4);
    f = 0.5f; memcpy(d + 0xA0, &f, 4); memcpy(d + 0xA4, &f, 4); memcpy(d + 0xA8, &f, 4);
    u = 2; memcpy(d + 0xB4, &u, 4); u = 3; memcpy(d + 0xB8, &u, 4);
    GMask fm = { { 0x40, 0, 0x1025, 0 } }; memcpy(d + 0xC0, &fm, 16);
    PData w; memset(&w, 0, sizeof(w));
    w.num = &g_outNum; w.pos = g_out[0].p; w.vel = g_out[0].v; w.id = &g_out[0].id; w.nrm = g_nrm;
    w.sPos = w.sVel = w.sId = 32; w.sNrm = 12;
    memcpy(d + 0xD4, &w, sizeof(w));
    PIdData del = { &g_delNum, g_del, 4, 0 }, cre = { &g_creNum, g_cre, 4, 0 };
    memcpy(d + 0x114, &del, 16); memcpy(d + 0x124, &cre, 16);
    memcpy(d + 0x13C, &flags, 4);
    void* ud = (void*)0x1234; memcpy(d + 0x140, &ud, 4);
}

static Rec32* findId(unsigned id) { for (unsigned i = 0; i < g_outNum; ++i) if (g_out[i].id == id) return &g_out[i]; return 0; }

static int run() {
    setupVtables();
    fluidrep::init(logfn, true, 0, true);
    // --- scene: floor (rotated 30 deg, non-cube), a filtered box, a convex wedge, a dynamic capsule ---
    GMask collide = { { 0x1, 0, 0x40, 0 } }, ignore = { { 0x2, 0, 0, 0 } };
    static Shape floor_ = { g_boxVt, 2 }; setPose(floor_.pose, 0.5236f, (V3){ 0, 0, -0.5f }); floor_.dims = (V3){ 20, 10, 0.5f }; floor_.mask = collide;
    static Shape ghost  = { g_boxVt, 2 }; setPose(ghost.pose, 0, (V3){ 5, 0, 1 }); ghost.dims = (V3){ 1, 1, 0.2f }; ghost.mask = ignore;
    static float wedgeV[] = { -1, -1, 0,  1, -1, 0,  1, 1, 0,  -1, 1, 0,  -1, -1, 1,  -1, 1, 1 };  // ramp rising toward -x
    static Mesh wedgeM = { g_meshVt, 6, wedgeV };
    static Shape wedge  = { g_cvxVt, 5 }; setPose(wedge.pose, 0.7f, (V3){ -5, 0, 0 }); wedge.mask = collide; wedge.mesh = &wedgeM;
    static Shape cap    = { g_capVt, 3 }; setPose(cap.pose, 0, (V3){ 10, 0, 1 }); cap.radius = 0.3f; cap.height = 1.0f; cap.mask = collide;
    // capsule along local Y: rotate so it stands upright? keep along Y (lying) for the test
    static Shape* sFloor[] = { &floor_ }, *sGhost[] = { &ghost }, *sWedge[] = { &wedge }, *sCap[] = { &cap };
    static Actor aFloor = { g_actorVt, 0, 1, sFloor }, aGhost = { g_actorVt, 0, 1, sGhost }, aWedge = { g_actorVt, 0, 1, sWedge }, aCap = { g_actorVt, 1, 1, sCap };
    static Actor* actors[] = { &aFloor, &aGhost, &aWedge, &aCap };
    static Scene scene = { g_sceneVt, 4, actors, { 0, 0, -9.81f } };

    say("== create fluid");
    static unsigned char desc[0x160];
    buildDesc(desc, 0.1f, (V3){ 0, 0, 0 }, 0x9);
    void* fl = fluidrep::tryCreate(&scene, desc);
    CHECK(fl != 0, "tryCreate returned our fluid");
    if (!fl) return 1;
    CHECK(((void**)fl)[1] == (void*)0x1234, "userData copied to NxFluid::userData (+4)");

    say("== add particles");
    struct In { float p[3], v[3]; } in[6] = {
        { { 0, 0, 2 }, { 1, 0, 0 } },        // falls on the floor, slides
        { { 5, 0, 3 }, { 0, 0, 0 } },        // falls through the filtered 'ghost' box to the floor
        { { -5.5f, 0, 3 }, { 0, 0, 0 } },    // lands on the wedge slope
        { { 0, 3, -0.3f }, { 0, 0, 0 } },    // starts inside the floor: must be pushed out
        { { 10, 0, 3 }, { 0, 0, 0 } },       // lands on the dynamic capsule
        { { 2, -2, 0.5f }, { 0, 0, -30 } },  // fast: motion limit must stop tunnelling
    };
    unsigned n6 = 6;
    PData ip; memset(&ip, 0, sizeof(ip)); ip.num = &n6; ip.pos = in[0].p; ip.vel = in[0].v; ip.sPos = ip.sVel = 24;
    unsigned added = slot<FN_add>(fl, 5)(fl, 0, &ip, false);
    CHECK(added == 6, "addParticles added 6 (got %u)", added);
    CHECK(g_creNum == 6, "creation ids reported: %u", g_creNum);
    unsigned ids[6]; bool uniq = true;
    for (int i = 0; i < 6; ++i) { ids[i] = g_cre[i]; for (int j = 0; j < i; ++j) if (ids[i] == ids[j]) uniq = false; if (ids[i] >= (unsigned)MAXP) uniq = false; }
    CHECK(uniq, "creation ids unique and < maxParticles (%u %u %u %u %u %u)", ids[0], ids[1], ids[2], ids[3], ids[4], ids[5]);

    say("== simulate 4 s at 60 Hz");
    g_quietLog = false;
    float minZ[6]; for (int i = 0; i < 6; ++i) minZ[i] = 1e9f;
    for (int f = 0; f < 240; ++f) {
        fluidrep::beginStep(&scene, 1.0f / 60.0f);
        fluidrep::endStep(&scene);
        g_quietLog = true;
        for (int i = 0; i < 6; ++i) { Rec32* r = findId(ids[i]); if (r && r->p[2] < minZ[i]) minZ[i] = r->p[2]; }
    }
    g_quietLog = false;
    CHECK(g_outNum == 6, "6 particles written back (got %u)", g_outNum);
    // floor top surface is z = 0 (rotation is about Z so it stays flat); collision distance = 0.04
    for (int i = 0; i < 6; ++i) {
        Rec32* r = findId(ids[i]);
        if (!r) { CHECK(false, "particle %d present", i); continue; }
        float sp = fsq(r->v[0] * r->v[0] + r->v[1] * r->v[1] + r->v[2] * r->v[2]);
        say("  particle %d id %u: pos (%.3f %.3f %.3f) vel (%.3f %.3f %.3f) |v| %.3f minZ %.3f nrm (%.2f %.2f %.2f)", i, r->id,
            r->p[0], r->p[1], r->p[2], r->v[0], r->v[1], r->v[2], sp, minZ[i], g_nrm[3 * (r - g_out)], g_nrm[3 * (r - g_out) + 1], g_nrm[3 * (r - g_out) + 2]);
    }
    Rec32* p0 = findId(ids[0]); Rec32* p1 = findId(ids[1]); Rec32* p2 = findId(ids[2]); Rec32* p3 = findId(ids[3]); Rec32* p4 = findId(ids[4]); Rec32* p5 = findId(ids[5]);
    CHECK(p0 && p0->p[2] > 0.035f && p0->p[2] < 0.06f, "particle 0 rests on the floor at ~0.04");
    CHECK(p0 && minZ[0] > 0.0f, "particle 0 never went below the floor surface (min %.4f)", minZ[0]);
    CHECK(p0 && p0->p[0] > 0.2f, "particle 0 slid along +x (x=%.3f)", p0 ? p0->p[0] : 0);
    CHECK(p1 && p1->p[2] < 0.1f, "particle 1 passed through the filtered box and reached the floor (z=%.3f)", p1 ? p1->p[2] : 0);
    CHECK(p2 && (p2->p[0] + 5.5f) * (p2->p[0] + 5.5f) + p2->p[1] * p2->p[1] > 0.5f && minZ[2] > 0.0f, "particle 2 landed on the wedge and slid off it (pos %.3f %.3f)", p2 ? p2->p[0] : 0, p2 ? p2->p[1] : 0);
    CHECK(p3 && p3->p[2] > 0.035f, "particle 3 was pushed out of the floor (z=%.3f)", p3 ? p3->p[2] : 0);
    CHECK(p4 && (p4->p[2] > 1.2f || p4->p[2] < 0.1f) && minZ[4] > 0.0f, "particle 4 hit the capsule (min z %.3f, now %.3f)", minZ[4], p4 ? p4->p[2] : 0);
    CHECK(p5 && minZ[5] > 0.0f, "fast particle 5 did not tunnel through the floor (min z %.4f)", minZ[5]);

    B3 b; slot<FN_bounds>(fl, 72)(fl, 0, &b);
    bool inside = true; for (unsigned i = 0; i < g_outNum; ++i) for (int k = 0; k < 3; ++k) if (g_out[i].p[k] < (&b.mn.x)[k] || g_out[i].p[k] > (&b.mx.x)[k]) inside = false;
    CHECK(inside && b.mn.x < b.mx.x, "getWorldBounds contains every particle (x %.2f..%.2f)", b.mn.x, b.mx.x);
    float maxRestV = 0; for (unsigned i = 0; i < g_outNum; ++i) { float s2 = g_out[i].v[0]*g_out[i].v[0]+g_out[i].v[1]*g_out[i].v[1]+g_out[i].v[2]*g_out[i].v[2]; if (s2 > maxRestV) maxRestV = s2; }
    CHECK(maxRestV < 1e-4f, "resting particles have ~zero velocity (max |v|^2 %.6f)", maxRestV);
    CHECK(g_delNum == 0, "no deletions reported yet (%u)", g_delNum);

    say("== updateParticles: delete 2 particles");
    struct UD { unsigned id, flag; } ud[2] = { { ids[1], 1 }, { ids[4], 1 } };
    PUpdate up; memset(&up, 0, sizeof(up)); up.num = 2; up.id = &ud[0].id; up.flag = &ud[0].flag; up.sId = up.sFlag = 8;
    slot<FN_upd>(fl, 17)(fl, 0, &up);
    fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
    CHECK(g_outNum == 4, "4 particles remain (got %u)", g_outNum);
    CHECK(g_delNum == 2 && ((g_del[0] == ids[1] && g_del[1] == ids[4]) || (g_del[0] == ids[4] && g_del[1] == ids[1])),
          "deletion ids report exactly the 2 deleted (%u: %u %u)", g_delNum, g_del[0], g_del[1]);
    CHECK(!findId(ids[1]) && !findId(ids[4]), "deleted ids gone from write data");
    fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
    CHECK(g_delNum == 0, "deletion count cleared next frame (%u)", g_delNum);

    say("== id reuse after deletion");
    unsigned n1 = 1; In one = { { 0, 0, 1 }, { 0, 0, 0 } };
    PData ip1 = ip; ip1.num = &n1; ip1.pos = one.p; ip1.vel = one.v;
    unsigned before = g_outNum;
    slot<FN_add>(fl, 5)(fl, 0, &ip1, false);
    CHECK(g_creNum == 1 && g_cre[0] < (unsigned)MAXP, "new particle got id %u (creation count %u)", g_cre[0], g_creNum);
    unsigned newId = g_cre[0];
    bool clash = false; for (unsigned i = 0; i < before; ++i) if (g_out[i].id == newId) clash = true;
    CHECK(!clash, "new id does not clash with a live particle");
    // PhysX 2.8.0: addParticles appends the new particle to the write buffer (the game reads its id there)
    CHECK(g_outNum == before + 1 && g_out[before].id == newId && g_out[before].p[2] == 1.0f,
          "addParticles appended the new particle to the write buffer (count %u -> %u, id %u)", before, g_outNum, g_out[before].id);

    say("== removeAllParticles");
    slot<FN_v>(fl, 6)(fl, 0);
    fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
    CHECK(g_outNum == 0, "no particles written after removeAll (got %u)", g_outNum);
    CHECK(g_delNum == 5, "removeAll reported all 5 ids (4 live + 1 pending) as deleted (got %u)", g_delNum);

    say("== capacity: add more than maxParticles");
    static In many[100]; for (int i = 0; i < 100; ++i) { many[i].p[0] = (float)(i % 10); many[i].p[1] = (float)(i / 10); many[i].p[2] = 1; many[i].v[0] = many[i].v[1] = many[i].v[2] = 0; }
    unsigned n100 = 100; PData ipm = ip; ipm.num = &n100; ipm.pos = many[0].p; ipm.vel = many[0].v;
    unsigned got = slot<FN_add>(fl, 5)(fl, 0, &ipm, false);
    CHECK(got == (unsigned)MAXP, "addParticles capped at maxParticles (%u)", got);
    fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
    CHECK(g_outNum == (unsigned)MAXP, "write data holds %u particles", g_outNum);

    say("== smoke-like fluid: external acceleration + damping give terminal velocity");
    static unsigned char desc2[0x160];
    // separate output buffers would be needed for a real second fluid; reuse after releasing the first
    fluidrep::release(&scene, fl);
    buildDesc(desc2, 2.5f, (V3){ 0, 0, 19.81f }, 0x9);     // net +10 up, damping 2.5 -> terminal ~4 units/s
    void* smoke = fluidrep::tryCreate(&scene, desc2);
    unsigned n1b = 1; In s1 = { { 0, 0, 1 }, { 0, 0, 0 } };
    PData ips = ip; ips.num = &n1b; ips.pos = s1.p; ips.vel = s1.v;
    slot<FN_add>(smoke, 5)(smoke, 0, &ips, false);
    for (int f = 0; f < 180; ++f) { fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene); }
    CHECK(g_outNum == 1 && g_out[0].v[2] > 3.0f && g_out[0].v[2] < 4.5f, "terminal rise velocity ~4 (got %.3f)", g_out[0].v[2]);


    say("== SPH mist (game parameters: rppm 10, krm 2, stiffness 20, viscosity 6, cd 12, ext +16.2)");
    {
        static unsigned char ds[0x160]; static Rec32 so[6000]; static float sn[6000 * 3]; static unsigned sNum, sdel, scre, sdelb[6000], screb[6000];
        buildDesc(ds, 0.0f, (V3){ 0, 0, 16.2f }, 0x9);
        unsigned u; float fv;
        u = 1; memcpy(ds + 0xB4, &u, 4);                       // SPH
        u = 6000; memcpy(ds + 0x40, &u, 4);
        fv = 10.0f; memcpy(ds + 0x48, &fv, 4); fv = 2.0f; memcpy(ds + 0x50, &fv, 4);
        fv = 0.8f; memcpy(ds + 0x54, &fv, 4); fv = 12.0f; memcpy(ds + 0x58, &fv, 4); u = 16; memcpy(ds + 0x5C, &u, 4);
        fv = 20.0f; memcpy(ds + 0x60, &fv, 4); fv = 6.0f; memcpy(ds + 0x64, &fv, 4);
        fv = 0.5f; memcpy(ds + 0x90, &fv, 4);
        PData w; memset(&w, 0, sizeof(w)); w.num = &sNum; w.pos = so[0].p; w.vel = so[0].v; w.id = &so[0].id; w.nrm = sn; w.sPos = w.sVel = w.sId = 32; w.sNrm = 12;
        memcpy(ds + 0xD4, &w, sizeof(w));
        PIdData del = { &sdel, sdelb, 4, 0 }, cre = { &scre, screb, 4, 0 }; memcpy(ds + 0x114, &del, 16); memcpy(ds + 0x124, &cre, 16);
        // gravity-dominated variant for the "floor mist" case: no upward acceleration
        float zero = 0; memcpy(ds + 0x7C, &zero, 4);
        void* sf = fluidrep::tryCreate(&scene, ds);
        CHECK(sf != 0, "SPH fluid is replaced");
        // a compressed block: 8x8x8 at spacing 0.05 (half the rest spacing) -> pressure must spread it
        static In blk[512]; int nb = 0;
        for (int x = 0; x < 8; ++x) for (int y = 0; y < 8; ++y) for (int z = 0; z < 8; ++z) {
            blk[nb].p[0] = -2 + x * 0.05f; blk[nb].p[1] = 2 + y * 0.05f; blk[nb].p[2] = 2.0f + z * 0.05f;
            blk[nb].v[0] = blk[nb].v[1] = blk[nb].v[2] = 0; ++nb; }
        unsigned n512 = 512; PData ip; memset(&ip, 0, sizeof(ip)); ip.num = &n512; ip.pos = blk[0].p; ip.vel = blk[0].v; ip.sPos = ip.sVel = 24;
        slot<FN_add>(sf, 5)(sf, 0, &ip, false);
        float ext0 = 0; bool nan = false; float maxV = 0, minZ = 1e9f;
        for (int f = 0; f < 240; ++f) {
            fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
            for (unsigned i = 0; i < sNum; ++i) {
                for (int k = 0; k < 3; ++k) if (so[i].p[k] != so[i].p[k] || so[i].v[k] != so[i].v[k]) nan = true;
                float v2 = so[i].v[0]*so[i].v[0] + so[i].v[1]*so[i].v[1] + so[i].v[2]*so[i].v[2]; if (v2 > maxV) maxV = v2;
                if (so[i].p[2] < minZ) minZ = so[i].p[2];
            }
            if (f == 0) { float mn = 1e9f, mx = -1e9f; for (unsigned i = 0; i < sNum; ++i) { if (so[i].p[0] < mn) mn = so[i].p[0]; if (so[i].p[0] > mx) mx = so[i].p[0]; } ext0 = mx - mn; }
        }
        float mn = 1e9f, mx = -1e9f, mny = 1e9f, mxy = -1e9f, sumZ = 0;
        for (unsigned i = 0; i < sNum; ++i) { if (so[i].p[0] < mn) mn = so[i].p[0]; if (so[i].p[0] > mx) mx = so[i].p[0];
                                              if (so[i].p[1] < mny) mny = so[i].p[1]; if (so[i].p[1] > mxy) mxy = so[i].p[1]; sumZ += so[i].p[2]; }
        say("  SPH: %u particles, x extent %.3f -> %.3f, y extent %.3f, mean z %.3f, min z %.3f, max |v| %.3f", sNum, ext0, mx - mn, mxy - mny, sumZ / sNum, minZ, fsq(maxV));
        CHECK(!nan, "SPH stays finite (no NaN)");
        CHECK(mx - mn > ext0 * 1.5f, "pressure spreads the compressed block (%.3f -> %.3f)", ext0, mx - mn);
        CHECK(minZ > 1.1f, "mist hovers at its 1.2 collision distance above the floor (min z %.3f)", minZ);
        CHECK(fsq(maxV) < 10.0f, "speeds stay bounded (max %.3f)", fsq(maxV));
        fluidrep::release(&scene, sf);
        // performance: 5000 SPH particles
        void* sf2 = fluidrep::tryCreate(&scene, ds);
        static In many[5000]; unsigned seed = 7;
        for (int i = 0; i < 5000; ++i) { seed = seed * 1103515245u + 12345u; float a = (seed >> 8 & 0xFFFF) / 65535.0f;
            seed = seed * 1103515245u + 12345u; float b = (seed >> 8 & 0xFFFF) / 65535.0f; seed = seed * 1103515245u + 12345u; float c = (seed >> 8 & 0xFFFF) / 65535.0f;
            many[i].p[0] = -3 + a * 2.5f; many[i].p[1] = 1 + b * 2.5f; many[i].p[2] = 1.3f + c * 1.0f; many[i].v[0] = many[i].v[1] = many[i].v[2] = 0; }
        unsigned n5 = 5000; PData ip5 = ip; ip5.num = &n5; ip5.pos = many[0].p; ip5.vel = many[0].v;
        slot<FN_add>(sf2, 5)(sf2, 0, &ip5, false);
        double sum = 0, mxs = 0; int frames = 60;
        for (int f = 0; f < frames; ++f) { fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
            fluidrep::FrameStats st = fluidrep::lastStats(); if (f) { sum += st.stepMs; if (st.stepMs > mxs) mxs = st.stepMs; } }
        say("  SPH perf: %u particles, avg step CPU %.3f ms (max %.3f)", sNum, sum / (frames - 1), mxs);
        bool nan2 = false; for (unsigned i = 0; i < sNum; ++i) for (int k = 0; k < 3; ++k) if (so[i].p[k] != so[i].p[k]) nan2 = true;
        CHECK(!nan2 && sNum == 5000, "5000-particle SPH run finite and complete");
        fluidrep::release(&scene, sf2);
    }
    say("== fluid packets: write buffer grouped by cell, NxFluidPacket list covers every particle (steam fix)");
    {
        struct Pk { float mn[3], mx[3]; unsigned first, num, id; };
        static unsigned char dp[0x160]; static Rec32 po[3000]; static unsigned pNum, pdel, pcre, pdelb[3000], pcreb[3000];
        static Pk pk[924 + 8]; static unsigned pkNum = 0xDEAD;
        buildDesc(dp, 1.0f, (V3){ 0, 0, 20.0f }, 0x9);          // the game's steam: SPH, rppm 10, krm 2, psm 16, ext +20
        unsigned u; float fv;
        u = 1; memcpy(dp + 0xB4, &u, 4); u = 3000; memcpy(dp + 0x40, &u, 4);
        fv = 10.0f; memcpy(dp + 0x48, &fv, 4); fv = 2.0f; memcpy(dp + 0x50, &fv, 4); fv = 0.4f; memcpy(dp + 0x54, &fv, 4);
        fv = 12.0f; memcpy(dp + 0x58, &fv, 4); u = 16; memcpy(dp + 0x5C, &u, 4); fv = 20.0f; memcpy(dp + 0x60, &fv, 4); fv = 6.0f; memcpy(dp + 0x64, &fv, 4);
        PData w; memset(&w, 0, sizeof(w)); w.num = &pNum; w.pos = po[0].p; w.vel = po[0].v; w.id = &po[0].id; w.sPos = w.sVel = w.sId = 32;
        memcpy(dp + 0xD4, &w, sizeof(w));
        PIdData del = { &pdel, pdelb, 4, 0 }, cre = { &pcre, pcreb, 4, 0 }; memcpy(dp + 0x114, &del, 16); memcpy(dp + 0x124, &cre, 16);
        Pk* pkp = pk; unsigned* pkn = &pkNum; memcpy(dp + 0x134, &pkp, 4); memcpy(dp + 0x138, &pkn, 4);
        void* pf = fluidrep::tryCreate(&scene, dp);
        CHECK(pf != 0, "steam-like SPH fluid with a packet buffer is replaced");
        // three puffs far apart plus a scattered trail: several packets (edge 16 x 0.2 = 3.2 m)
        static In pin[600]; unsigned seed = 11;
        for (int i = 0; i < 600; ++i) {
            seed = seed * 1103515245u + 12345u; float a = (seed >> 8 & 0xFFFF) / 65535.0f;
            seed = seed * 1103515245u + 12345u; float b = (seed >> 8 & 0xFFFF) / 65535.0f;
            seed = seed * 1103515245u + 12345u; float c = (seed >> 8 & 0xFFFF) / 65535.0f;
            float cx = (i % 4 == 0) ? -20 : (i % 4 == 1) ? 0 : (i % 4 == 2) ? 25 : -40 + a * 80;
            pin[i].p[0] = cx + a * 1.5f; pin[i].p[1] = 3 + b * 1.5f; pin[i].p[2] = 3 + c * 2.0f; pin[i].v[0] = pin[i].v[1] = pin[i].v[2] = 0; }
        unsigned n600 = 600; PData ip; memset(&ip, 0, sizeof(ip)); ip.num = &n600; ip.pos = pin[0].p; ip.vel = pin[0].v; ip.sPos = ip.sVel = 24;
        slot<FN_add>(pf, 5)(pf, 0, &ip, false);
        bool allOk = true; unsigned maxPk = 0, minPk = 1u << 30;
        for (int f = 0; f < 90; ++f) {
            // the game deletes ~1 particle every other frame (like the steam emitters)
            if (f % 2 == 1 && pNum > 10) { unsigned did = po[f % pNum].id, one = 1, fl1 = 1; PUpdate up; memset(&up, 0, sizeof(up));
                up.num = one; up.id = &did; up.sId = 4; up.flag = &fl1; up.sFlag = 4; slot<FN_upd>(pf, 17)(pf, 0, &up); }
            fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
            // checks every frame
            bool ok = pkNum > 0 && pkNum <= 924;
            unsigned next = 0;
            for (unsigned k = 0; ok && k < pkNum; ++k) {
                if (pk[k].first != next || pk[k].num == 0) { ok = false; break; }
                int cell0[3];
                for (unsigned j = pk[k].first; j < pk[k].first + pk[k].num; ++j) {
                    for (int a = 0; a < 3; ++a) {
                        if (po[j].p[a] < pk[k].mn[a] || po[j].p[a] > pk[k].mx[a]) ok = false;
                        float cf = po[j].p[a] / 3.2f; int ci = (int)cf; if ((float)ci > cf) --ci;
                        if (j == pk[k].first) cell0[a] = ci; else if (ci != cell0[a]) ok = false;
                    }
                }
                next += pk[k].num;
            }
            if (next != pNum) ok = false;
            // ids unique
            static unsigned char seen[3000]; memset(seen, 0, sizeof(seen));
            for (unsigned j = 0; j < pNum; ++j) { if (po[j].id >= 3000 || seen[po[j].id]) ok = false; else seen[po[j].id] = 1; }
            if (!ok) { if (allOk) say("  packet check failed at frame %d: %u packets, %u particles", f, pkNum, pNum); allOk = false; }
            if (pkNum > maxPk) maxPk = pkNum; if (pkNum < minPk) minPk = pkNum;
        }
        say("  packets per frame %u..%u for %u particles", minPk, maxPk, pNum);
        CHECK(allOk, "every frame: packets contiguous, cover all particles exactly once, one cell per packet, AABBs contain their particles, ids unique");
        CHECK(minPk >= 3, "puffs far apart end up in separate packets (min %u)", minPk);
        // packet limit: 1200 particles each in its own cell -> 924 packets, the rest merged into the last one
        fluidrep::release(&scene, pf);
        void* pf2 = fluidrep::tryCreate(&scene, dp);
        static In grid[1200];
        for (int i = 0; i < 1200; ++i) { grid[i].p[0] = (float)(i % 40) * 4.0f - 80; grid[i].p[1] = (float)(i / 40) * 4.0f - 60; grid[i].p[2] = 5; grid[i].v[0] = grid[i].v[1] = grid[i].v[2] = 0; }
        unsigned n1200 = 1200; PData ig = ip; ig.num = &n1200; ig.pos = grid[0].p; ig.vel = grid[0].v;
        slot<FN_add>(pf2, 5)(pf2, 0, &ig, false);
        fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
        unsigned covered = 0; for (unsigned k = 0; k < pkNum; ++k) covered += pk[k].num;
        CHECK(pkNum == 924 && covered == pNum && pNum == 1200, "packet limit: %u packets (max 924) still cover all %u particles (%u covered)", pkNum, pNum, covered);
        // an unwritable packet buffer is left alone (no crash, count set to 0)
        g_shimNoAccessLo = (unsigned long)pk & ~0xFFFUL; g_shimNoAccessHi = g_shimNoAccessLo + 0x10000;
        pkNum = 77;
        fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
        g_shimNoAccessLo = g_shimNoAccessHi = 0;
        CHECK(pkNum == 0 || pkNum == 77, "unwritable packet buffer is not written (count %u)", pkNum);
        fluidrep::release(&scene, pf2);
        // a fluid without a packet buffer keeps the natural order (no packet code runs)
    }
    say("== ReplaceSPH=0 leaves SPH fluids to PhysX");
    static unsigned char desc3[0x160]; buildDesc(desc3, 0, (V3){ 0, 0, 0 }, 0x9); unsigned sph = 1; memcpy(desc3 + 0xB4, &sph, 4);
    fluidrep::init(logfn, true, 0, false);
    CHECK(fluidrep::tryCreate(&scene, desc3) == 0, "SPH descriptor left to PhysX when ReplaceSPH=0");
    fluidrep::init(logfn, true, 0, true);


    say("== performance: 1500 boxes + 900 convexes, 3000 particles, single thread");
    {
        static Shape boxes[1500], cvxs[900]; static Shape* sp[2400]; static Actor acts[2400]; static Actor* aps[2401];
        unsigned seed = 12345; 
        #define RND() ((seed = seed * 1103515245u + 12345u) >> 8 & 0xFFFF) / 65535.0f
        for (int i = 0; i < 1500; ++i) { Shape& b = boxes[i]; memset(&b, 0, sizeof(b)); b.vt = g_boxVt; b.type = 2; b.mask = collide;
            setPose(b.pose, RND() * 3.14f, (V3){ RND() * 40 - 20, RND() * 40 - 20, RND() * 6 });
            b.dims = (V3){ 0.1f + RND() * 1.5f, 0.1f + RND() * 1.5f, 0.05f + RND() * 0.5f }; sp[i] = &b; }
        for (int i = 0; i < 900; ++i) { Shape& c = cvxs[i]; memset(&c, 0, sizeof(c)); c.vt = g_cvxVt; c.type = 5; c.mask = collide; c.mesh = &wedgeM;
            setPose(c.pose, RND() * 3.14f, (V3){ RND() * 40 - 20, RND() * 40 - 20, RND() * 6 }); sp[1500 + i] = &c; }
        for (int i = 0; i < 2400; ++i) { acts[i].vt = g_actorVt; acts[i].dynamic = 0; acts[i].ns = 1; acts[i].shapes = &sp[i]; aps[i] = &acts[i]; }
        aps[2400] = &aFloor;
        static Scene big = { g_sceneVt, 2401, aps, { 0, 0, -9.81f } };
        static unsigned char d4[0x160]; static Rec32 big_out[4096]; static float big_nrm[4096 * 3]; static unsigned bigNum, bdel, bcre, bdelb[4096], bcreb[4096];
        buildDesc(d4, 0.1f, (V3){ 0, 0, 0 }, 0x9);
        unsigned m4 = 4096; memcpy(d4 + 0x40, &m4, 4);
        PData w; memset(&w, 0, sizeof(w)); w.num = &bigNum; w.pos = big_out[0].p; w.vel = big_out[0].v; w.id = &big_out[0].id; w.nrm = big_nrm; w.sPos = w.sVel = w.sId = 32; w.sNrm = 12;
        memcpy(d4 + 0xD4, &w, sizeof(w));
        PIdData del = { &bdel, bdelb, 4, 0 }, cre = { &bcre, bcreb, 4, 0 }; memcpy(d4 + 0x114, &del, 16); memcpy(d4 + 0x124, &cre, 16);
        g_quietLog = false;
        void* bf = fluidrep::tryCreate(&big, d4);
        static In pin[3000];
        for (int i = 0; i < 3000; ++i) { pin[i].p[0] = RND() * 30 - 15; pin[i].p[1] = RND() * 30 - 15; pin[i].p[2] = 3 + RND() * 4;
                                         pin[i].v[0] = RND() * 6 - 3; pin[i].v[1] = RND() * 6 - 3; pin[i].v[2] = RND() * 4 - 2; }
        unsigned n3 = 3000; PData ipp; memset(&ipp, 0, sizeof(ipp)); ipp.num = &n3; ipp.pos = pin[0].p; ipp.vel = pin[0].v; ipp.sPos = ipp.sVel = 24;
        slot<FN_add>(bf, 5)(bf, 0, &ipp, false);
        g_quietLog = true;
        double sumStep = 0, maxStep = 0, sumTests = 0; int frames = 120;
        for (int f = 0; f < frames; ++f) {
            fluidrep::beginStep(&big, 1.0f / 60.0f); fluidrep::endStep(&big);
            fluidrep::FrameStats st = fluidrep::lastStats();
            if (f > 0) { sumStep += st.stepMs; if (st.stepMs > maxStep) maxStep = st.stepMs; sumTests += st.narrowTests; }
            V3 a0 = axisOf(floor_.pose, 0), a1 = axisOf(floor_.pose, 1);
            for (unsigned i = 0; i < bigNum; ++i) {
                float lx = big_out[i].p[0] * a0.x + big_out[i].p[1] * a0.y, ly = big_out[i].p[0] * a1.x + big_out[i].p[1] * a1.y;
                bool foot = (lx < 0 ? -lx : lx) < floor_.dims.x - 0.05f && (ly < 0 ? -ly : ly) < floor_.dims.y - 0.05f;
                if (foot && big_out[i].p[2] < -0.005f && big_out[i].p[2] > -0.95f) ++g_inSlab;
                if (foot && big_out[i].p[2] < -0.005f && big_out[i].p[2] > -0.95f && g_inSlabShown < 3) { ++g_inSlabShown;
                    say("    INSIDE FLOOR SLAB frame %d: id %u z %.4f v (%.2f %.2f %.2f)", f, big_out[i].id, big_out[i].p[2], big_out[i].v[0], big_out[i].v[1], big_out[i].v[2]); }
            }
        }
        g_quietLog = false;
        say("  particles %u, avg step %.3f ms (max %.3f) single-threaded at an assumed 3 GHz TSC, avg narrow tests/frame %.0f (%.1f per particle)",
            bigNum, sumStep / (frames - 1), maxStep, sumTests / (frames - 1), sumTests / (frames - 1) / 3000.0);
        int below = 0, shown = 0;
        int offEdge = 0;
        for (unsigned i = 0; i < bigNum; ++i) if (big_out[i].p[2] < 0.0f) {
            { V3 a0 = axisOf(floor_.pose, 0), a1 = axisOf(floor_.pose, 1);
              float lx = big_out[i].p[0] * a0.x + big_out[i].p[1] * a0.y, ly = big_out[i].p[0] * a1.x + big_out[i].p[1] * a1.y;
              if ((lx < 0 ? -lx : lx) > floor_.dims.x || (ly < 0 ? -ly : ly) > floor_.dims.y) { ++offEdge; continue; } }
            ++below;
            if (shown < 6) {
                ++shown;
                int inBox = -1; float bz0 = 0, bz1 = 0;
                for (int k = 0; k < 1500; ++k) {
                    Shape& b = boxes[k]; float dx = big_out[i].p[0] - b.pose[9], dy = big_out[i].p[1] - b.pose[10], dz = big_out[i].p[2] - b.pose[11];
                    V3 a0 = axisOf(b.pose, 0), a1 = axisOf(b.pose, 1);
                    float lx = dx * a0.x + dy * a0.y, ly = dx * a1.x + dy * a1.y;
                    if ((lx < 0 ? -lx : lx) < b.dims.x + 0.05f && (ly < 0 ? -ly : ly) < b.dims.y + 0.05f && (dz < 0 ? -dz : dz) < b.dims.z + 0.05f) { inBox = k; bz0 = b.pose[11] - b.dims.z; bz1 = b.pose[11] + b.dims.z; break; }
                }
                say("    below floor: z %.4f v (%.3f %.3f %.3f) near box %d spanning z %.3f..%.3f", big_out[i].p[2], big_out[i].v[0], big_out[i].v[1], big_out[i].v[2], inBox, bz0, bz1);
            }
        }
        { unsigned h = 2166136261u; for (unsigned i = 0; i < bigNum; ++i) { const unsigned* w = (const unsigned*)&big_out[i]; for (int k = 0; k < 7; ++k) h = (h ^ w[k]) * 16777619u; }
          say("  CHECKSUM of final particle state: %x (%u particles)", h, bigNum); }
        say("    %d particles slid off the floor's edge (expected: the floor is smaller than the spawn area)", offEdge);
        CHECK(g_inSlab == 0, "no particle was ever inside the floor slab during 120 frames (%d particle-frames)", g_inSlab);
        if (below) say("    note: %d particle(s) below the floor inside its footprint without ever entering the slab (came from beyond the edge)", below);
    }
    // ------------------------------------------------------------------ cloth replacement
    {
        say("== cloth: setup");
        fluidrep::clothInit(true);
        const int G = 11; const float SP = 0.2f;                     // 11x11 vertices, 2 m x 2 m
        static float cpts[G * G * 3]; static unsigned ctri[(G - 1) * (G - 1) * 2 * 3];
        for (int j = 0; j < G; ++j) for (int i = 0; i < G; ++i) { float* q = &cpts[3 * (j * G + i)]; q[0] = i * SP; q[1] = j * SP; q[2] = 0; }
        int nt = 0;
        for (int j = 0; j < G - 1; ++j) for (int i = 0; i < G - 1; ++i) {
            unsigned a = j * G + i, b = a + 1, c = a + G, d = c + 1;
            ctri[3 * nt] = a; ctri[3 * nt + 1] = b; ctri[3 * nt + 2] = d; ++nt;
            ctri[3 * nt] = a; ctri[3 * nt + 1] = d; ctri[3 * nt + 2] = c; ++nt;
        }
        struct FakeClothMesh { void** vt; };
        struct Saver { static bool FC save(void*, void*, unsigned char* md) {
            unsigned u; void* p;
            u = 121; memcpy(md + 0, &u, 4); u = 200; memcpy(md + 4, &u, 4); u = 12; memcpy(md + 8, &u, 4); u = 12; memcpy(md + 12, &u, 4);
            p = cpts; memcpy(md + 0x10, &p, 4); p = ctri; memcpy(md + 0x14, &p, 4); u = 0x100; memcpy(md + 0x18, &u, 4);   // TEARABLE mesh
            return true; } };
        static void* meshVt[4] = { 0, (void*)&Saver::save, 0, 0 };
        static FakeClothMesh cmesh = { meshVt };
        // game-side render buffers (twice the vertices for tearing, like the game's)
        static float bPos[242 * 3], bNrm[242 * 3]; static unsigned bIdx[600], bPar[600], bNumV, bNumI, bNumP, bDirty;
        struct MD { void* pos; void* nrm; unsigned ps, ns, maxV; unsigned* numV; void* idx; unsigned is, maxI; unsigned* numI; void* par; unsigned pst, maxP; unsigned* numP; unsigned* dirty; unsigned flags; const char* name; };
        MD md = { bPos, bNrm, 12, 12, 242, &bNumV, bIdx, 4, 600, &bNumI, bPar, 4, 600, &bNumP, &bDirty, 0, 0 };
        auto mkDesc = [&](unsigned char* d, const float* pose, unsigned flags) {
            memset(d, 0, 0x118);
            void* m = &cmesh; memcpy(d, &m, 4); memcpy(d + 4, pose, 48);
            float f;
            f = 0.05f; memcpy(d + 0x34, &f, 4); f = 1; memcpy(d + 0x38, &f, 4); memcpy(d + 0x3C, &f, 4); memcpy(d + 0x40, &f, 4);
            f = 0.5f; memcpy(d + 0x44, &f, 4); memcpy(d + 0x48, &f, 4); f = 1; memcpy(d + 0x4C, &f, 4); f = 1.5f; memcpy(d + 0x50, &f, 4);
            f = 0.2f; memcpy(d + 0x54, &f, 4); memcpy(d + 0x58, &f, 4); f = 1.5f; memcpy(d + 0x5C, &f, 4);
            unsigned it = 5; memcpy(d + 0x6C, &it, 4); f = 0.4f; memcpy(d + 0x88, &f, 4); f = -1; memcpy(d + 0x8C, &f, 4);
            memcpy(d + 0x90, &md, sizeof(md));
            GMask cm = { { 0x40, 0, 0x1025, 0 } }; memcpy(d + 0xD8, &cm, 16);
            f = 1; memcpy(d + 0x104, &f, 4); memcpy(d + 0x108, &flags, 4);
            void* ud = (void*)0x5678; memcpy(d + 0x10C, &ud, 4);
        };
        typedef void (FC *FN_att)(void*, void*, unsigned, const V3*);
        typedef unsigned (FC *FN_n)(void*, void*);
        typedef void (FC *FN_vel)(void*, void*, void*, unsigned);
        typedef bool (FC *FN_ray)(void*, void*, const float*, V3*, unsigned*);
        typedef void (FC *FN_dforce)(void*, void*, const V3*, const V3*, float, unsigned);
        typedef void (FC *FN_rb)(void*, void*, B3*);
        // hanging sheet: local y -> world z (the game's own pose layout), top row pinned every frame
        float hp[12] = { 1, 0, 0,  0, 0, -1,  0, 1, 0,  0, 0, 4 };
        static unsigned char cd[0x118]; mkDesc(cd, hp, 0x1020);
        void* cl = fluidrep::clothTryCreate(&scene, cd);
        CHECK(cl != 0, "clothTryCreate returned our cloth");
        if (cl) {
            CHECK(((void**)cl)[1] == (void*)0x5678, "userData copied to NxCloth::userData (+4)");
            CHECK(bNumV == 121 && bNumI == 600 && (bDirty & 4), "mesh data written at creation (vertices %u, indices %u, dirty 0x%x)", bNumV, bNumI, bDirty);
            CHECK(bPos[3 * 120 + 2] > 5.99f && bPos[3 * 120 + 2] < 6.01f, "initial positions are posed (top corner z %.3f)", bPos[3 * 120 + 2]);
            V3 pinned[G];
            for (int i = 0; i < G; ++i) pinned[i] = (V3){ i * SP, 0, 6 };
            for (int f = 0; f < 180; ++f) {
                for (int i = 0; i < G; ++i) slot<FN_att>(cl, 28)(cl, 0, (unsigned)(10 * G + i), &pinned[i]);
                fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
            }
            float maxStretch = 0; bool nan = false;
            for (int k = 0; k < (int)bNumI; k += 3) for (int e = 0; e < 3; ++e) {
                unsigned a = bIdx[k + e], b = bIdx[k + (e + 1) % 3];
                float dx = bPos[3 * a] - bPos[3 * b], dy = bPos[3 * a + 1] - bPos[3 * b + 1], dz = bPos[3 * a + 2] - bPos[3 * b + 2];
                float l = fsq(dx * dx + dy * dy + dz * dz);
                float rx = cpts[3 * a] - cpts[3 * b], ry = cpts[3 * a + 1] - cpts[3 * b + 1];
                float rl = fsq(rx * rx + ry * ry);
                if (l != l) nan = true;
                if (rl > 0 && l / rl > maxStretch) maxStretch = l / rl;
            }
            say("  hanging sheet after 3 s: top-left (%.3f %.3f %.3f) bottom-left (%.3f %.3f %.3f), max edge stretch %.3f",
                bPos[3 * 110], bPos[3 * 110 + 1], bPos[3 * 110 + 2], bPos[0], bPos[1], bPos[2], maxStretch);
            CHECK(!nan, "hanging sheet stays finite");
            CHECK(bPos[3 * 110 + 2] > 5.99f && bPos[3 * 110 + 2] < 6.01f, "pinned row stays at its global position");
            CHECK(bPos[2] > 3.8f && bPos[2] < 4.2f, "the sheet hangs its full height below the pins (bottom z %.3f)", bPos[2]);
            CHECK(maxStretch < 1.12f, "edges keep their length (max stretch %.3f)", maxStretch);
            // raycast through the sheet (plane y ~ 0)
            float ray[6] = { 1.0f, -5.0f, 5.0f, 0, 1, 0 }; V3 hit; unsigned vid = 999;
            bool hr = slot<FN_ray>(cl, 36)(cl, 0, ray, &hit, &vid);
            CHECK(hr && hit.y > -0.2f && hit.y < 0.2f && vid < 121, "raycast hits the sheet (y %.3f, vertex %u)", hit.y, vid);
            // velocities round trip (the game's wind code: get, modify, set)
            unsigned nP = slot<FN_n>(cl, 53)(cl, 0);
            static float vb[242 * 3];
            slot<FN_vel>(cl, 52)(cl, 0, vb, 12);
            for (unsigned i = 0; i < nP; ++i) vb[3 * i] += 1.0f;
            slot<FN_vel>(cl, 50)(cl, 0, vb, 12);
            fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
            CHECK(nP == 121, "getNumberOfParticles = 121 (%u)", nP);
            CHECK(bPos[3 * 60] > 1.0f + 0.005f, "setVelocities moved the sheet (centre x %.3f)", bPos[3 * 60]);
            // bullet: strong impulse at the centre must tear the sheet
            V3 at = { bPos[3 * 60], bPos[3 * 60 + 1], bPos[3 * 60 + 2] }, imp = { 0, 400, 0 };
            slot<FN_dforce>(cl, 84)(cl, 0, &at, &imp, 0.5f, 1 /*impulse*/);
            for (int f = 0; f < 30; ++f) {
                for (int i = 0; i < G; ++i) slot<FN_att>(cl, 28)(cl, 0, (unsigned)(10 * G + i), &pinned[i]);
                fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
            }
            bool idxOk = true, parOk = bNumP == bNumV;
            for (unsigned k = 0; k < bNumI; ++k) if (bIdx[k] >= bNumV) idxOk = false;
            for (unsigned k = 0; k < bNumP; ++k) if (bPar[k] >= 121) parOk = false;
            say("  after the impulse: %u vertices, %u indices, %u parent indices, dirty 0x%x", bNumV, bNumI, bNumP, bDirty);
            CHECK(bNumV > 121, "the impulse tore the sheet (vertices %u)", bNumV);
            CHECK(idxOk, "all indices point at written vertices");
            CHECK(parOk, "parent indices cover every vertex and point at original vertices");
            fluidrep::clothRelease(&scene, cl);
            CHECK(!fluidrep::clothIsOurs(cl), "cloth released");
        }
        {   // start pose stored transposed: the first pin targets must decide how the sheet is placed (1.1.1 started every
            // tarp upside down in game and it exploded)
            float hpT[12] = { 1, 0, 0,  0, 0, 1,  0, -1, 0,  0, 0, 4 };     // transpose of hp's rotation
            static unsigned char cd4[0x118]; mkDesc(cd4, hpT, 0x1020);
            void* c4 = fluidrep::clothTryCreate(&scene, cd4);
            CHECK(c4 != 0, "cloth with transposed start pose created");
            if (c4) {
                float startTop = bPos[3 * 120 + 2];
                V3 pinned4[G]; for (int i = 0; i < G; ++i) pinned4[i] = (V3){ i * SP, 0, 6 };
                float maxSt = 0;
                for (int f = 0; f < 120; ++f) {
                    for (int i = 0; i < G; ++i) slot<FN_att>(c4, 28)(c4, 0, (unsigned)(10 * G + i), &pinned4[i]);
                    fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
                    for (int k = 0; k < (int)bNumI; k += 3) for (int e = 0; e < 3; ++e) {
                        unsigned a = bIdx[k + e], b = bIdx[k + (e + 1) % 3];
                        float dx = bPos[3*a]-bPos[3*b], dy = bPos[3*a+1]-bPos[3*b+1], dz = bPos[3*a+2]-bPos[3*b+2];
                        float rx = cpts[3*bPar[a]]-cpts[3*bPar[b]], ry = cpts[3*bPar[a]+1]-cpts[3*bPar[b]+1];
                        float l = fsq(dx*dx+dy*dy+dz*dz), rl = fsq(rx*rx+ry*ry); if (rl > 0 && l / rl > maxSt) maxSt = l / rl; }
                }
                say("  transposed start pose: top-row vertex started at z %.3f; after 2 s vertices %u, bottom z %.3f, max stretch %.3f",
                    startTop, bNumV, bPos[2], maxSt);
                CHECK(bNumV == 121, "no tearing from a wrong start pose (vertices %u)", bNumV);
                CHECK(bPos[2] > 3.8f && bPos[2] < 4.2f, "sheet hangs below its pins (bottom z %.3f)", bPos[2]);
                CHECK(maxSt < 1.1f, "edges keep their length (max stretch %.3f)", maxSt);
                fluidrep::clothRelease(&scene, c4);
            }
        }
        // falling sheet onto the floor (top surface z = 0)
        float fp[12] = { 1, 0, 0,  0, 1, 0,  0, 0, 1,  -1, -1, 1 };
        bDirty = 0;
        static unsigned char cd2[0x118]; mkDesc(cd2, fp, 0x20);
        void* c2 = fluidrep::clothTryCreate(&scene, cd2);
        CHECK(c2 != 0, "second cloth created");
        if (c2) {
            float minZ = 1e9f;
            for (int f = 0; f < 180; ++f) {
                fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
                for (unsigned i = 0; i < bNumV; ++i) if (bPos[3 * i + 2] < minZ) minZ = bPos[3 * i + 2];
            }
            float maxZ = -1e9f; for (unsigned i = 0; i < bNumV; ++i) if (bPos[3 * i + 2] > maxZ) maxZ = bPos[3 * i + 2];
            say("  falling sheet: min z ever %.4f, final max z %.4f", minZ, maxZ);
            CHECK(minZ > 0.0f, "falling sheet never goes through the floor (min z %.4f)", minZ);
            CHECK(maxZ < 0.3f, "falling sheet lies on the floor (max z %.4f)", maxZ);
            B3 bb; slot<FN_rb>(c2, 23)(c2, 0, &bb);
            CHECK(bb.mn.z <= bb.mx.z && bb.mx.z < 0.3f, "getWorldBounds matches the sheet");
            // PhysX impulse semantics: the impulse is shared by the TOTAL mass of the vertices in the radius (2 m x 2 m,
            // density 1 -> 4 kg), weighted by 1 - d^2/r^2; the centre vertex gets 10 / 4 = 2.5 m/s (minus one step of gravity)
            V3 ctr = { bPos[3 * 60], bPos[3 * 60 + 1], bPos[3 * 60 + 2] }, up = { 0, 0, 10 };
            slot<FN_dforce>(c2, 84)(c2, 0, &ctr, &up, 100.0f, 1 /*impulse*/);
            fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
            static float vb2[242 * 3]; slot<FN_vel>(c2, 52)(c2, 0, vb2, 12);
            say("  impulse 10 N s over the whole 4 kg sheet: centre vz %.3f, corner vz %.3f", vb2[3 * 60 + 2], vb2[2]);
            CHECK(vb2[3 * 60 + 2] > 2.2f && vb2[3 * 60 + 2] < 2.5f, "impulse divided by the region's total mass (centre vz %.3f, expected ~2.34)", vb2[3 * 60 + 2]);
            // game component check (UE3 USkeletalMeshComponent: +0x35c NxCloth, +0x364 render positions)
            static unsigned char comp[0x400]; memset(comp, 0, sizeof(comp));
            void* cp = c2; memcpy(comp + 0x35c, &cp, 4); void* bp = bPos; memcpy(comp + 0x364, &bp, 4);
            ((void**)c2)[1] = comp;
            fluidrep::clothRelease(&scene, c2);
        }
        {   // 1.1.14: tarp hanging inside a room-sized moving volume whose bottom is just below the floor (Ropeburn atrium:
            // loose pieces were pushed out through the volume's bottom face = through the floor, and fell forever)
            say("== cloth: tarp inside a room-sized moving volume");
            static Shape vol = { g_boxVt, 2 }; setPose(vol.pose, 0, (V3){ 60, 1, 3.3f }); vol.dims = (V3){ 8, 8, 3.3f }; vol.mask = collide;   // z 0..6.6, beyond the floor box (the game floor is thin triangles the pushed vertices tunnel through)
            static Shape* sVol[] = { &vol }; static Actor aVol = { g_actorVt, 1, 1, sVol };
            static Actor* actors5[] = { &aFloor, &aGhost, &aWedge, &aCap, &aVol };
            scene.na = 5; scene.actors = actors5;
            // sheet starts at z 1.2..3.2, deep inside the volume, pinned by one top corner: it swings down until its far
            // corner hangs about 0.4 m above the volume's bottom, where the old code started pushing it out downwards
            float vp[12] = { 1, 0, 0,  0, 0, -1,  0, 1, 0,  59, 1, 1.2f };
            static unsigned char cdV[0x118]; mkDesc(cdV, vp, 0x1020);
            void* cv = fluidrep::clothTryCreate(&scene, cdV);
            V3 pinsV[1] = { { 59, 1, 3.2f } };
            float maxV = 0, minZ = 1e9f; unsigned nv = 0;
            for (int f = 0; f < 480; ++f) {
                slot<FN_att>(cv, 28)(cv, 0, (unsigned)(10 * G), &pinsV[0]);
                fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
                static float vbV[242 * 3]; slot<FN_vel>(cv, 52)(cv, 0, vbV, 12);
                for (unsigned v = 0; v < bNumV; ++v) { float q = fsq(vbV[3*v]*vbV[3*v] + vbV[3*v+1]*vbV[3*v+1] + vbV[3*v+2]*vbV[3*v+2]); if (f > 5 && q > maxV) maxV = q;
                    if (bPos[3 * v + 2] < minZ) minZ = bPos[3 * v + 2]; }
                nv = bNumV;
            }
            say("  inside a moving volume: lowest vertex z %.3f (volume bottom 0), max speed %.2f m/s, vertices %u", minZ, maxV, nv);
            CHECK(minZ > 0.05f && nv == 121 && maxV < 8.0f, "the sheet hangs inside the volume and is not pushed out through its bottom (lowest z %.3f, speed %.2f, vertices %u)", minZ, maxV, nv);
            fluidrep::clothRelease(&scene, cv);
            scene.na = 4; scene.actors = actors;
        }
        {   // 1.1.14: a tarp hanging into level geometry, long loading frame first: pushes must not launch it
            say("== cloth: tarp hanging into the floor (pushes out of level geometry give no speed)");
            float fl[12] = { 1, 0, 0,  0, 0, -1,  0, 1, 0,  2, 1, -0.2f };      // sheet z -0.2..1.8: bottom rows inside the floor slab
            static unsigned char cdH[0x118]; mkDesc(cdH, fl, 0x1120);
            void* ch = fluidrep::clothTryCreate(&scene, cdH);
            V3 pinsH[G]; for (int i = 0; i < G; ++i) pinsH[i] = (V3){ 2 + i * SP, 1, 1.8f };
            float maxV = 0; unsigned v0 = 0;
            for (int f = 0; f < 120; ++f) {
                for (int i = 0; i < G; ++i) slot<FN_att>(ch, 28)(ch, 0, (unsigned)(10 * G + i), &pinsH[i]);
                fluidrep::beginStep(&scene, f == 0 ? 0.1f : 1.0f / 60.0f); fluidrep::endStep(&scene);
                static float vbH[242 * 3]; slot<FN_vel>(ch, 52)(ch, 0, vbH, 12);
                for (unsigned v = 0; v < bNumV; ++v) { float q = fsq(vbH[3*v]*vbH[3*v] + vbH[3*v+1]*vbH[3*v+1] + vbH[3*v+2]*vbH[3*v+2]); if (q > maxV) maxV = q; }
                v0 = bNumV;
            }
            say("  hanging into the floor: max speed %.2f m/s over 2 s, vertices %u", maxV, v0);
            CHECK(maxV < 4.0f && v0 == 121, "no launch and no tears from resting against level geometry (speed %.2f, vertices %u)", maxV, v0);
            fluidrep::clothRelease(&scene, ch);
        }
        {   // 1.1.14: a moving shape already deep inside a tarp on its first step (Ropeburn: box 0.7 m inside -> 44 m/s, 40 tears)
            say("== cloth: moving shape overlapping the tarp at creation");
            float savedPose[12]; for (int i = 0; i < 12; ++i) savedPose[i] = cap.pose[i];
            setPose(cap.pose, 0, (V3){ 10, 0, 1 });                                    // capsule along y through the sheet
            float ov[12] = { 1, 0, 0,  0, 0, -1,  0, 1, 0,  9, 0, 0.2f };              // sheet in plane y = 0, x 9..11, z 0.2..2.2
            static unsigned char cdO[0x118]; mkDesc(cdO, ov, 0x1120);                   // tearable
            void* co = fluidrep::clothTryCreate(&scene, cdO);
            V3 pinsO[G]; for (int i = 0; i < G; ++i) pinsO[i] = (V3){ 9 + i * SP, 0, 2.2f };
            float maxV = 0, maxS = 0;
            for (int f = 0; f < 30; ++f) {
                for (int i = 0; i < G; ++i) slot<FN_att>(co, 28)(co, 0, (unsigned)(10 * G + i), &pinsO[i]);
                fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
                static float vbO[242 * 3]; slot<FN_vel>(co, 52)(co, 0, vbO, 12);
                for (unsigned v = 0; v < bNumV; ++v) { float q = fsq(vbO[3*v]*vbO[3*v] + vbO[3*v+1]*vbO[3*v+1] + vbO[3*v+2]*vbO[3*v+2]); if (q > maxV) maxV = q; }
                for (int k = 0; k < (int)bNumI; k += 3) for (int e = 0; e < 3; ++e) { unsigned a = bIdx[k + e], b = bIdx[k + (e + 1) % 3];
                    if (a >= 121 || b >= 121) continue;
                    float dx = bPos[3 * a] - bPos[3 * b], dy = bPos[3 * a + 1] - bPos[3 * b + 1], dz = bPos[3 * a + 2] - bPos[3 * b + 2];
                    float rx = cpts[3 * a] - cpts[3 * b], ry = cpts[3 * a + 1] - cpts[3 * b + 1];
                    float l = fsq(dx * dx + dy * dy + dz * dz) / fsq(rx * rx + ry * ry); if (l > maxS) maxS = l; }
            }
            say("  overlap at creation: max speed %.2f m/s, max edge %.2f x rest, vertices %u (121 = no tears)", maxV, maxS, bNumV);
            CHECK(bNumV == 121 && maxV < 8.0f && maxS < 1.5f, "an overlapping moving shape neither launches nor tears the tarp (speed %.2f, stretch %.2f, vertices %u)", maxV, maxS, bNumV);
            fluidrep::clothRelease(&scene, co);
            for (int i = 0; i < 12; ++i) cap.pose[i] = savedPose[i];
        }
        {   // 1.1.14: pins applied and simulated on the very same frame (Ropeburn load 1: tarps torn and warped in one step)
            say("== cloth: first pins + first step on the same frame");
            float far_[12] = { 1, 0, 0,  0, 0, -1,  0, 1, 0,  -53, -240, 122 };   // far from the origin, like the game's tarps
            static unsigned char cdF[0x118]; mkDesc(cdF, far_, 0x1120);              // + tearable
            void* cf = fluidrep::clothTryCreate(&scene, cdF);
            V3 pinsF[G]; for (int i = 0; i < G; ++i) pinsF[i] = (V3){ -53 + i * SP, -240, 124 };   // exactly where the top row is placed
            float maxS = 0, maxV = 0;
            for (int f = 0; f < 3; ++f) {
                for (int i = 0; i < G; ++i) slot<FN_att>(cf, 28)(cf, 0, (unsigned)(10 * G + i), &pinsF[i]);
                fluidrep::beginStep(&scene, f == 0 ? 0.1f : 1.0f / 60.0f); fluidrep::endStep(&scene);   // first frame: a long (loading) frame
                for (int k = 0; k < (int)bNumI; k += 3) for (int e = 0; e < 3; ++e) { unsigned a = bIdx[k + e], b = bIdx[k + (e + 1) % 3];
                    if (a >= 121 || b >= 121) continue;
                    float dx = bPos[3 * a] - bPos[3 * b], dy = bPos[3 * a + 1] - bPos[3 * b + 1], dz = bPos[3 * a + 2] - bPos[3 * b + 2];
                    float rx = cpts[3 * a] - cpts[3 * b], ry = cpts[3 * a + 1] - cpts[3 * b + 1];
                    float l = fsq(dx * dx + dy * dy + dz * dz) / fsq(rx * rx + ry * ry); if (l > maxS) maxS = l; }
                static float vbF[242 * 3]; slot<FN_vel>(cf, 52)(cf, 0, vbF, 12);
                for (unsigned v = 0; v < bNumV; ++v) { float q = fsq(vbF[3*v]*vbF[3*v] + vbF[3*v+1]*vbF[3*v+1] + vbF[3*v+2]*vbF[3*v+2]); if (q > maxV) maxV = q; }
            }
            say("  first 3 frames: max edge %.2f x rest, max speed %.2f m/s, vertices %u (121 = no tears)", maxS, maxV, bNumV);
            CHECK(bNumV == 121 && maxS < 1.3f && maxV < 3.0f, "newly pinned vertices start from where they are, not from the origin (stretch %.2f, speed %.2f, vertices %u)", maxS, maxV, bNumV);
            fluidrep::clothRelease(&scene, cf);
        }
        {   // 1.1.14: a tarp created far from where the game pins it (about 1 load in 10 in game: stretched across a room)
            say("== cloth: start mismatch -> refit to the pins");
            // case A: wrong translation, pins on one row (collinear) -> translation-only fit
            float bad[12] = { 1, 0, 0,  0, 0, -1,  0, 1, 0,  30, -12, 9 };    // the game pins it at x 0..2, y 0, z 6 (hp)
            static unsigned char cdA[0x118]; mkDesc(cdA, bad, 0x1020);
            void* ca = fluidrep::clothTryCreate(&scene, cdA);
            V3 pinsA[G]; for (int i = 0; i < G; ++i) pinsA[i] = (V3){ i * SP, 0, 6 };
            for (int f = 0; f < 90; ++f) {
                for (int i = 0; i < G; ++i) slot<FN_att>(ca, 28)(ca, 0, (unsigned)(10 * G + i), &pinsA[i]);
                fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
            }
            float maxD = 0, maxS = 0;
            for (unsigned v = 0; v < bNumV && v < 121; ++v) { float dx = bPos[3 * v] - cpts[3 * v], dy = bPos[3 * v + 1], dz = bPos[3 * v + 2] - 4.0f;
                (void)dz; float d = dx < 0 ? -dx : dx; if (d > maxD) maxD = d; float ay = dy < 0 ? -dy : dy; if (ay > maxD) maxD = ay; }
            for (int k = 0; k < (int)bNumI; k += 3) for (int e = 0; e < 3; ++e) { unsigned a = bIdx[k + e], b = bIdx[k + (e + 1) % 3];
                float dx = bPos[3 * a] - bPos[3 * b], dy = bPos[3 * a + 1] - bPos[3 * b + 1], dz = bPos[3 * a + 2] - bPos[3 * b + 2];
                float l = fsq(dx * dx + dy * dy + dz * dz) / SP; if (l > maxS) maxS = l; }
            say("  A: max x/y offset from the pinned sheet %.3f m, max edge length %.2f x rest", maxD, maxS);
            CHECK(maxD < 0.6f && maxS < 1.6f, "translation-only refit: sheet hangs from its pins, not stretched across the scene (offset %.3f, stretch %.2f)", maxD, maxS);
            fluidrep::clothRelease(&scene, ca);
            // case B: wrong rotation + translation, pins on two rows (top row and the middle row's ends) -> rigid fit
            float badB[12] = { 0, 1, 0,  1, 0, 0,  0, 0, -1,  -7, 22, 3 };
            static unsigned char cdB[0x118]; mkDesc(cdB, badB, 0x1020);
            void* cb = fluidrep::clothTryCreate(&scene, cdB);
            // correct placement = hp: world = (x, 0, 4 + y)
            unsigned pv[G + 2]; V3 pinsB[G + 2]; int npb = 0;
            for (int i = 0; i < G; ++i) { pv[npb] = 10 * G + i; pinsB[npb] = (V3){ i * SP, 0, 6 }; ++npb; }
            pv[npb] = 5 * G + 0;  pinsB[npb] = (V3){ 0, 0, 5 }; ++npb;
            pv[npb] = 5 * G + 10; pinsB[npb] = (V3){ 2, 0, 5 }; ++npb;
            for (int f = 0; f < 90; ++f) {
                for (int i = 0; i < npb; ++i) slot<FN_att>(cb, 28)(cb, 0, pv[i], &pinsB[i]);
                fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
            }
            float maxDB = 0;
            for (unsigned v = 0; v < 121; ++v) { float ex = cpts[3 * v], ez = 4.0f + cpts[3 * v + 1];
                float dx = bPos[3 * v] - ex, dy = bPos[3 * v + 1], dz = bPos[3 * v + 2] - ez; float d = fsq(dx * dx + dy * dy + dz * dz); if (d > maxDB) maxDB = d; }
            say("  B: max distance from the pinned placement %.3f m", maxDB);
            CHECK(maxDB < 0.6f, "rigid refit: rotated and moved cloth lands on its pins (max %.3f m)", maxDB);
            fluidrep::clothRelease(&scene, cb);
            // case C: a correctly placed cloth is left alone (no refit)
            static unsigned char cdC[0x118]; mkDesc(cdC, hp, 0x1020);
            void* cc = fluidrep::clothTryCreate(&scene, cdC);
            for (int f = 0; f < 70; ++f) { for (int i = 0; i < G; ++i) slot<FN_att>(cc, 28)(cc, 0, (unsigned)(10 * G + i), &pinsA[i]);
                fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene); }
            CHECK(bPos[3 * 120 + 2] > 5.9f && bPos[3 * 120 + 2] < 6.1f, "correct placement untouched (top corner z %.3f)", bPos[3 * 120 + 2]);
            fluidrep::clothRelease(&scene, cc);
        }
        {   // a resting tarp wakes up and gets pushed when a moving (kinematic) shape runs into it
            say("== cloth: wake by contact");
            float savedPose[12]; for (int i = 0; i < 12; ++i) savedPose[i] = cap.pose[i];
            cap.pose[10] = -4.0f;                                                   // capsule out of the way
            float hp2[12] = { 1, 0, 0,  0, 0, -1,  0, 1, 0,  9, 0.5f, 0.2f };      // plane y = 0.5, x 9..11, z 0.2..2.2
            static unsigned char cd3[0x118]; mkDesc(cd3, hp2, 0x1020);
            void* c3 = fluidrep::clothTryCreate(&scene, cd3);
            CHECK(c3 != 0, "third cloth created");
            if (c3) {
                typedef bool (FC *FN_b)(void*, void*); typedef void (FC *FN_v)(void*, void*);
                V3 pins[G]; for (int i = 0; i < G; ++i) pins[i] = (V3){ 9 + i * SP, 0.5f, 2.2f };
                slot<FN_v>(c3, 78)(c3, 0);                                           // the game puts new cloth to sleep
                for (int f = 0; f < 30; ++f) {
                    for (int i = 0; i < G; ++i) slot<FN_att>(c3, 28)(c3, 0, (unsigned)(10 * G + i), &pins[i]);
                    fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
                }
                bool slept = slot<FN_b>(c3, 74)(c3, 0);
                float y0 = bPos[3 * 60 + 1];
                static float before[121 * 3]; for (int k = 0; k < 121 * 3; ++k) before[k] = bPos[k];
                cap.pose[10] = 0.5f;                                                 // the capsule moves into the tarp
                for (int f = 0; f < 10; ++f) {
                    for (int i = 0; i < G; ++i) slot<FN_att>(c3, 28)(c3, 0, (unsigned)(10 * G + i), &pins[i]);
                    fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
                }
                float moved = 0;
                for (unsigned i = 0; i < 121; ++i) { float dx = bPos[3*i]-before[3*i], dy = bPos[3*i+1]-before[3*i+1], dz = bPos[3*i+2]-before[3*i+2];
                    float d = fsq(dx*dx+dy*dy+dz*dz); if (d > moved) moved = d; }
                say("  asleep before contact %d, centre y %.3f; after the capsule (lying across the sheet) moved in: max displacement %.3f, vertices %u", slept ? 1 : 0, y0, moved, bNumV);
                CHECK(slept, "resting tarp was asleep before the contact");
                CHECK(moved > 0.2f, "the capsule pushed the tarp (max displacement %.3f)", moved);
                fluidrep::clothRelease(&scene, c3);
            }
            for (int i = 0; i < 12; ++i) cap.pose[i] = savedPose[i];
        }
    }
    {   // actor-level NX_AF_FLUID_DISABLE_COLLISION (8): fluids pass through every shape of that actor (glass and debris
        // were held in mid-air by such invisible actors in 1.1.3); a box on an ordinary actor still stops particles
        say("== fluid: actor flag 'fluids pass through'");
        GMask collide2 = { { 0x1, 0, 0x40, 0 } };
        static Shape fl2 = { g_boxVt, 2 }; setPose(fl2.pose, 0, (V3){ 100, 0, -0.5f }); fl2.dims = (V3){ 10, 10, 0.5f }; fl2.mask = collide2;
        static Shape pass = { g_boxVt, 2 }; setPose(pass.pose, 0, (V3){ 98, 0, 1 }); pass.dims = (V3){ 1, 1, 0.2f }; pass.mask = collide2;
        static Shape solid = { g_boxVt, 2 }; setPose(solid.pose, 0, (V3){ 102, 0, 1 }); solid.dims = (V3){ 1, 1, 0.2f }; solid.mask = collide2;
        static Shape *sF2[] = { &fl2 }, *sP[] = { &pass }, *sS[] = { &solid };
        static Actor aF2 = { g_actorVt, 0, 1, sF2, 0 }, aP = { g_actorVt, 0, 1, sP, 8 }, aS = { g_actorVt, 0, 1, sS, 0 };
        static Actor* actors2[] = { &aF2, &aP, &aS };
        static Scene scene2 = { g_sceneVt, 3, actors2, { 0, 0, -9.81f } };
        static unsigned char d2[0x160]; buildDesc(d2, 0.1f, (V3){ 0, 0, 0 }, 0x9);
        void* f2 = fluidrep::tryCreate(&scene2, d2);
        struct In2 { float p[3], v[3]; } in2[2] = { { { 98, 0, 3 }, { 0, 0, 0 } }, { { 102, 0, 3 }, { 0, 0, 0 } } };
        unsigned n2 = 2;
        PData ip2; memset(&ip2, 0, sizeof(ip2)); ip2.num = &n2; ip2.pos = in2[0].p; ip2.vel = in2[0].v; ip2.sPos = ip2.sVel = 24;
        slot<FN_add>(f2, 5)(f2, 0, &ip2, false);
        unsigned id0 = g_cre[0], id1 = g_cre[1];
        for (int fr = 0; fr < 120; ++fr) { fluidrep::beginStep(&scene2, 1.0f / 60.0f); fluidrep::endStep(&scene2); }
        Rec32* r0 = findId(id0); Rec32* r1 = findId(id1);
        say("  over the pass-through actor: z %.3f; over the ordinary box: z %.3f", r0 ? r0->p[2] : -99.0f, r1 ? r1->p[2] : -99.0f);
        CHECK(r0 && r0->p[2] < 0.5f, "particle fell through the actor flagged 'fluids pass through' (z %.3f)", r0 ? r0->p[2] : -99.0f);
        CHECK(r1 && r1->p[2] > 1.15f, "particle rests on the ordinary box (z %.3f)", r1 ? r1->p[2] : -99.0f);
        fluidrep::releaseScene(&scene2);
    }
#ifdef CLOTH_EXP
    {   // realistic tarp: 40 x 42 vertices, 0.1 m spacing, top row pinned every frame, no bullets
        say("== EXP: big hanging tarp");
        const int W = 40, H = 42; const float SP = 0.1f;
        static float pts[W * H * 3]; static unsigned tri[(W - 1) * (H - 1) * 6];
        for (int j = 0; j < H; ++j) for (int i = 0; i < W; ++i) { float* q = &pts[3 * (j * W + i)]; q[0] = i * SP; q[1] = j * SP; q[2] = 0; }
        int nt = 0;
        for (int j = 0; j < H - 1; ++j) for (int i = 0; i < W - 1; ++i) {
            unsigned a = j * W + i, b = a + 1, c = a + W, d = c + 1;
            tri[3 * nt] = a; tri[3 * nt + 1] = b; tri[3 * nt + 2] = d; ++nt; tri[3 * nt] = a; tri[3 * nt + 1] = d; tri[3 * nt + 2] = c; ++nt;
        }
        static int s_nt; s_nt = nt;
        struct Sv { static bool FC save(void*, void*, unsigned char* md) { unsigned u; void* p;
            u = W * H; memcpy(md, &u, 4); u = s_nt; memcpy(md + 4, &u, 4); u = 12; memcpy(md + 8, &u, 4); memcpy(md + 12, &u, 4);
            p = pts; memcpy(md + 0x10, &p, 4); p = tri; memcpy(md + 0x14, &p, 4); u = 0x100; memcpy(md + 0x18, &u, 4); return true; } };
        static void* mvt[4] = { 0, (void*)&Sv::save, 0, 0 }; static struct { void** vt; } cm = { mvt };
        const int MAXV = W * H + 512;
        static float pos[MAXV * 3], nrm[MAXV * 3]; static unsigned idx[(W - 1) * (H - 1) * 6 * 3 + 64], par[MAXV], nV, nI, nP, dirty;
        struct MD { void* pos; void* nrm; unsigned ps, ns, maxV; unsigned* numV; void* idx; unsigned is, maxI; unsigned* numI; void* par; unsigned pst, maxP; unsigned* numP; unsigned* dirty; unsigned flags; const char* name; };
        MD md = { pos, nrm, 12, 12, (unsigned)MAXV, &nV, idx, 4, (unsigned)(nt * 3), &nI, par, 4, (unsigned)(nt * 3), &nP, &dirty, 0, 0 };
        static unsigned char d[0x118]; memset(d, 0, sizeof(d));
        float pose[12] = { 1, 0, 0,  0, 0, -1,  0, 1, 0,  50, 0, 8 };   // local y -> world z, far from the test geometry
        void* m = &cm; memcpy(d, &m, 4); memcpy(d + 4, pose, 48);
        float f; f = 0.01f; memcpy(d + 0x34, &f, 4); f = 1; memcpy(d + 0x38, &f, 4); memcpy(d + 0x3C, &f, 4); memcpy(d + 0x40, &f, 4);
        f = 0.5f; memcpy(d + 0x44, &f, 4); memcpy(d + 0x48, &f, 4); f = 1; memcpy(d + 0x4C, &f, 4); f = 1.5f; memcpy(d + 0x50, &f, 4);
        f = 0.2f; memcpy(d + 0x54, &f, 4); memcpy(d + 0x58, &f, 4); f = 1.5f; memcpy(d + 0x5C, &f, 4);
        unsigned it = 5; memcpy(d + 0x6C, &it, 4); f = 0.4f; memcpy(d + 0x88, &f, 4); f = -1; memcpy(d + 0x8C, &f, 4);
        memcpy(d + 0x90, &md, sizeof(md)); GMask cmk = { { 0x800, 0, 5, 0 } }; memcpy(d + 0xD8, &cmk, 16);
        f = 1; memcpy(d + 0x104, &f, 4); unsigned fl = 0x1020; memcpy(d + 0x108, &fl, 4);
        void* cl = fluidrep::clothTryCreate(&scene, d);
        typedef void (FC *FN_att)(void*, void*, unsigned, const V3*);
        typedef void (FC *FN_dforce)(void*, void*, const V3*, const V3*, float, unsigned);
        static V3 pin[W]; for (int i = 0; i < W; ++i) { pin[i].x = 50 + i * SP; pin[i].y = 0; pin[i].z = 8 + (H - 1) * SP; }
        auto stretch = [&]() { float mx = 0; for (unsigned k = 0; k + 2 < nI; k += 3) for (int e = 0; e < 3; ++e) {
            unsigned a = idx[k + e], b = idx[k + (e + 1) % 3]; unsigned pa = par[a], pb = par[b];
            float dx = pos[3*a]-pos[3*b], dy = pos[3*a+1]-pos[3*b+1], dz = pos[3*a+2]-pos[3*b+2];
            float rx = pts[3*pa]-pts[3*pb], ry = pts[3*pa+1]-pts[3*pb+1];
            float l = fsq(dx*dx+dy*dy+dz*dz), r = fsq(rx*rx+ry*ry); if (r > 0 && l / r > mx) mx = l / r; } return mx; };
        for (int fr = 0; fr < 300; ++fr) {
            for (int i = 0; i < W; ++i) slot<FN_att>(cl, 28)(cl, 0, (unsigned)((H - 1) * W + i), &pin[i]);
            fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
            if (fr % 60 == 59) say("  t=%.1f s: vertices %u, max stretch %.3f, bottom-centre z %.3f", (fr + 1) / 60.0, nV, stretch(), pos[3 * (W / 2) + 2]);
        }
        static const float imps[] = { 0.5f, 1, 2, 5, 10, 20, 50 };
        for (int k = 0; k < 7; ++k) {
            unsigned before = nV;
            int vc = 20 * W + 5 + 5 * k; if (vc >= W * H) vc = 20 * W;
            V3 at = { pos[3 * vc], pos[3 * vc + 1], pos[3 * vc + 2] };
            V3 imp = { 0, imps[k], 0 };
            slot<FN_dforce>(cl, 84)(cl, 0, &at, &imp, 0.5f, 1);
            for (int fr = 0; fr < 60; ++fr) {
                for (int i = 0; i < W; ++i) slot<FN_att>(cl, 28)(cl, 0, (unsigned)((H - 1) * W + i), &pin[i]);
                fluidrep::beginStep(&scene, 1.0f / 60.0f); fluidrep::endStep(&scene);
            }
            say("  impulse %.1f N s at vertex %d: +%u vertices (now %u), max stretch now %.3f", imps[k], vc, nV - before, nV, stretch());
        }
        fluidrep::clothSummary();
        fluidrep::clothRelease(&scene, cl);
    }
#endif
    fluidrep::FrameStats st = fluidrep::lastStats();
    say("stats: fluids %d particles %d static %d dynamic %d tests %.0f", st.fluids, st.particles, st.staticGeoms, st.dynamicGeoms, st.narrowTests);
    say(g_fail ? "RESULT: %d FAILURES" : "RESULT: all checks passed", g_fail);
    return g_fail;
}

extern "C" void _start() {
    __asm__ volatile("andl $-16, %esp");
    int r = run();
    sys(252, r, 0, 0);   // exit_group: also ends worker threads
}
