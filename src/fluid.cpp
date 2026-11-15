// Replacement for PhysX 2.8.0 NxFluid (NX_F_NO_PARTICLE_INTERACTION) for Mirror's Edge.
//
// Why: PhysX 2.8.0 runs software fluids through NfFluidPPUInterfaceSwRef, a single-threaded software
// reference of the AGEIA PPU pipeline that tests every particle against every shape in a 10 m "packet"
// with x87 code (~285 box clips per particle per frame, ~100 ms/frame for ~3k particles).
//
// What: createFluid returns this object instead. It exposes the same 81-slot NxFluid vtable as the game's
// 2.8.0 runtime (userData at +4), honours the buffer contract the game uses (addParticles with creation
// ids, updateParticles delete flags, removeAllParticles, write data + deletion ids at fetchResults,
// getWorldBounds), and simulates independent particles (gravity, external acceleration, damping, motion
// limit, restitution/friction against static and dynamic shapes) on a thread pool that runs while PhysX
// simulates the rigid bodies.
//
// Collision geometry is copied out of the scene (boxes, convexes, spheres, capsules, triangle meshes),
// filtered exactly like PhysX filters fluid-vs-shape pairs (shape flags, group pair table, 128-bit groups
// mask with the scene's filter ops incl. Ubisoft's SWAP_AND, static/dynamic collision method), and put in
// one BVH per filter class.
//
// Slot numbers are the game's 2.8.0.7 PhysXCore layouts (verified via RTTI), not the 2.8.4 headers.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string.h>
#include <xmmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>        // _ReturnAddress
#endif
#include "fluid.h"

namespace fluidrep {

// ---------------------------------------------------------------------------
// basics
// ---------------------------------------------------------------------------
static LogFn  g_log = 0;
static bool   g_diag = true;   // per-frame diagnostic counters; proxy turns them off when logging is off
void setDiagnostics(bool on) { g_diag = on; }
static int    g_enabled = -1;
static int    g_threadsWanted = 0;
static bool   g_replaceSph = true;
static double g_msPerTick = 0;

#define LOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

static void* zalloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void  zfree(void* p)   { if (p) HeapFree(GetProcessHeap(), 0, p); }
template <class T> static T* allocN(size_t n) { return (T*)zalloc(sizeof(T) * n); }
template <class T> static void grow(T*& p, int& cap, int need) {
    if (need <= cap) return;
    int nc = cap ? cap : 64;
    while (nc < need) nc *= 2;
    T* q = allocN<T>(nc);
    if (p) { memcpy(q, p, sizeof(T) * cap); zfree(p); }
    p = q; cap = nc;
}

static LONGLONG qpc() { LARGE_INTEGER li; QueryPerformanceCounter(&li); return li.QuadPart; }

struct V3 { float x, y, z; };
struct B3 { V3 mn, mx; };
static inline V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static inline V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 operator*(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float fsqrt(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x > 0 ? x : 0))); }
static inline float fabs_(float x) { return x < 0 ? -x : x; }
static inline float fmin_(float a, float b) { return a < b ? a : b; }
static inline float fmax_(float a, float b) { return a > b ? a : b; }
static inline float len(V3 a) { return fsqrt(dot(a, a)); }
static inline V3 norm(V3 a) { float l = len(a); return l > 1e-20f ? a * (1.0f / l) : v3(0, 0, 0); }
static const float BIG = 3.402823466e+38f;
static inline void bEmpty(B3& b) { b.mn = v3(BIG, BIG, BIG); b.mx = v3(-BIG, -BIG, -BIG); }
static inline void bAdd(B3& b, V3 p) {
    b.mn.x = fmin_(b.mn.x, p.x); b.mn.y = fmin_(b.mn.y, p.y); b.mn.z = fmin_(b.mn.z, p.z);
    b.mx.x = fmax_(b.mx.x, p.x); b.mx.y = fmax_(b.mx.y, p.y); b.mx.z = fmax_(b.mx.z, p.z);
}
static inline void bAddB(B3& b, const B3& o) { bAdd(b, o.mn); bAdd(b, o.mx); }
static inline bool bOverlap(const B3& a, const B3& b) {
    return a.mn.x <= b.mx.x && a.mx.x >= b.mn.x && a.mn.y <= b.mx.y && a.mx.y >= b.mn.y && a.mn.z <= b.mx.z && a.mx.z >= b.mn.z;
}

template <class F> static F vfn(void* obj, int slot) { return (F)((*(void***)obj)[slot]); }

// ---------------------------------------------------------------------------
// PhysX 2.8.0.7 slots used here
// ---------------------------------------------------------------------------
enum {
    SC_getGravity = 6, SC_getGroupCollisionFlag = 48, SC_getFilterOps = 59, SC_getFilterBool = 60,
    SC_getFilterConstant0 = 61, SC_getFilterConstant1 = 62, SC_getNbActors = 63, SC_getActors = 64,
    AC_getNbShapes = 19, AC_getShapes = 20, AC_readActorFlag = 27, AC_isDynamic = 29,
    SH_getGroup = 3, SH_getWorldBounds = 4, SH_getFlag = 6, SH_getGlobalPose = 16, SH_getType = 23, SH_getGroupsMask = 36,
    BOX_getDimensions = 40, SPH_getRadius = 40, CAP_getRadius = 41, CAP_getHeight = 43, CVX_getMesh = 40, TRI_getMesh = 40,
    MESH_getCount = 2, MESH_getFormat = 3, MESH_getBase = 4, MESH_getStride = 5
};
enum { ST_PLANE = 0, ST_SPHERE, ST_BOX, ST_CAPSULE, ST_WHEEL, ST_CONVEX, ST_MESH, ST_HEIGHTFIELD };
enum { SF_TRIGGER = 7, SF_DISABLE_COLLISION = 1 << 4, SF_FLUID_DRAIN = 1 << 8, SF_FLUID_DISABLE_COLLISION = 1 << 10,
       SF_CLOTH_DISABLE_COLLISION = 1 << 16 };
enum { FF_DISABLE_GRAVITY = 1 << 1, FF_COLLISION_TWOWAY = 1 << 2, FF_ENABLED = 1 << 3, FF_PROJECT_TO_PLANE = 1 << 6 };
enum { FP_COLL_STATIC = 1, FP_COLL_DYNAMIC = 2, FP_MOTION_LIMIT = 8 };
enum { FM_FORCE = 0, FM_IMPULSE, FM_VELOCITY_CHANGE, FM_SMOOTH_IMPULSE, FM_SMOOTH_VELOCITY_CHANGE, FM_ACCELERATION };

typedef bool   (__fastcall *FN_bool)(void*, void*);
typedef NxU32  (__fastcall *FN_u32)(void*, void*);
typedef unsigned short (__fastcall *FN_u16)(void*, void*);
typedef float  (__fastcall *FN_f)(void*, void*);
typedef void*  (__fastcall *FN_ptr)(void*, void*);
typedef NxU32  (__fastcall *FN_flagMask)(void*, void*, NxU32);
typedef bool   (__fastcall *FN_groupFlag)(void*, void*, unsigned short, unsigned short);
typedef void   (__fastcall *FN_filterOps)(void*, void*, int*, int*, int*);
typedef GMask* (__fastcall *FN_retMask)(void*, void*, GMask*);
typedef V3*    (__fastcall *FN_retVec)(void*, void*, V3*);
typedef void*  (__fastcall *FN_retPose)(void*, void*, float*);
typedef void   (__fastcall *FN_bounds)(void*, void*, B3*);
typedef void   (__fastcall *FN_vecOut)(void*, void*, V3*);
typedef NxU32  (__fastcall *FN_meshU32)(void*, void*, NxU32, NxU32);
typedef const void* (__fastcall *FN_meshPtr)(void*, void*, NxU32, NxU32);

// ---------------------------------------------------------------------------
// collision filtering (PhysX 2.8.0 semantics, read from PhysXCore 0x1005e5xx / op table 0x102815bc)
// ---------------------------------------------------------------------------
static void maskOp(int op, const GMask& a, const GMask& b, GMask& out) {
    for (int w = 0; w < 4; ++w) {
        NxU32 x = a.b[w], y = b.b[w];
        switch (op) {
            case 0: out.b[w] = x & y; break;          // AND
            case 1: out.b[w] = x | y; break;          // OR
            case 2: out.b[w] = x ^ y; break;          // XOR
            case 3: out.b[w] = ~(x & y); break;       // NAND
            case 4: out.b[w] = ~(x | y); break;       // NOR
            case 5: out.b[w] = ~(x ^ y); break;       // NXOR
            case 6: out.b[w] = x & b.b[(w + 2) & 3]; break;   // SWAP_AND: {a0&b2, a1&b3, a2&b0, a3&b1}
            default: out.b[w] = x & y; break;
        }
    }
}
bool groupsMaskPass(const GMask& g0, const GMask& g1, int op0, int op1, int op2, const GMask& k0, const GMask& k1, bool fb) {
    GMask t0, t1, r;
    maskOp(op0, g0, k0, t0);
    maskOp(op1, g1, k1, t1);
    maskOp(op2, t0, t1, r);
    bool nz = (r.b[0] | r.b[1] | r.b[2] | r.b[3]) != 0;
    return nz == fb;
}

// ---------------------------------------------------------------------------
// particle API structs (2.8.x layouts)
// ---------------------------------------------------------------------------
struct PData   { NxU32* num; float* pos; float* vel; float* life; float* dens; NxU32* id; NxU32* flag; float* nrm;
                 NxU32 sPos, sVel, sLife, sDens, sId, sFlag, sNrm; const char* name; };
struct PIdData { NxU32* num; NxU32* id; NxU32 sId; const char* name; };
struct PUpdate { NxU32 forceMode; NxU32 num; float* force; NxU32* flag; NxU32* id; NxU32 sForce, sFlag, sId; };
struct Plane4  { V3 n; float d; };

template <class T> static inline T* at(void* base, NxU32 stride, NxU32 i) { return (T*)((char*)base + (size_t)stride * i); }
template <class T> static inline const T* atc(const void* base, NxU32 stride, NxU32 i) { return (const T*)((const char*)base + (size_t)stride * i); }

// ---------------------------------------------------------------------------
// geometry
// ---------------------------------------------------------------------------
enum { GT_BOX = 1, GT_CONVEX, GT_SPHERE, GT_CAPSULE, GT_TRI };

struct Geom {
    unsigned char type, dynamic, drain, pad;      // pad: bit0 = no fluid collision, bit1 = no cloth collision
    unsigned short group, pad2;
    GMask mask;
    B3 aabb;
    V3 c, ax[3], e;       // box: center, world axes, half extents; sphere: c; capsule: c=a
    V3 b;                 // capsule end b
    float r;              // sphere / capsule radius
    int p0, pc;           // convex planes in the owning plane array
    V3 t0, t1, t2, tn;    // triangle
    void* shape;          // dynamic geoms only: shape pointer for pose refresh (validated every step)
    NxU32 dbgShapeFlags, dbgActorFlags;   // diagnostics only (never dereferenced later)
    void* actor; void* actorShapes; NxU32 actorNs;
    int localPlanes;      // dynamic convex: index into hull cache
};

struct BNode { B3 b; int left, count; };
struct BVH {
    BNode* nodes; int nNodes, capNodes;
    int* items; int nItems, capItems;
};

struct HullEntry { void* mesh; NxU32 nv; float v0[3]; int start, count; };
static HullEntry* g_hulls = 0; static int g_nHulls = 0, g_capHulls = 0;
static Plane4* g_hullPlanes = 0; static int g_nHullPlanes = 0, g_capHullPlanes = 0;

struct FilterClass {
    unsigned short group; GMask mask; NxU32 collMethod; NxU32 kind;     // kind: 0 fluid, 1 cloth
    int* sIdx; int nS, capS; BVH sBvh;     // static geoms passing the filter
    int* dIdx; int nD, capD; BVH dBvh;     // dynamic geoms passing the filter (rebuilt per step)
};

struct World {
    void* scene;
    bool  staticDirty, dynDirty;
    Geom* sg; int nSg, capSg;               // static geoms
    Plane4* sp; int nSp, capSp;             // static convex planes (world)
    Geom* dg; int nDg, capDg;               // dynamic geoms
    Plane4* dp; int nDp, capDp;             // dynamic convex planes (world, per step)
    bool groupPair[32][32];
    int op0, op1, op2; bool fb; GMask k0, k1;
    int transposed;                          // -1 unknown, 0 row-major NxMat33, 1 column-major
    FilterClass cls[16]; int nCls;
    int skippedTypes[8];
    NxU32 lastActorSig, lastActorCount, lastDynSig;
};
static World g_worlds[4];

static World* worldFor(void* scene, bool create) {
    for (int i = 0; i < 4; ++i) if (g_worlds[i].scene == scene) return &g_worlds[i];
    if (!create) return 0;
    for (int i = 0; i < 4; ++i) if (!g_worlds[i].scene) {
        memset(&g_worlds[i], 0, sizeof(World));
        g_worlds[i].scene = scene; g_worlds[i].staticDirty = g_worlds[i].dynDirty = true; g_worlds[i].transposed = -1;
        return &g_worlds[i];
    }
    return 0;
}

// pose: NxMat34 = NxMat33 (9 floats) + translation
static inline V3 poseAxis(const float* m, int i, int transposed) {
    return transposed ? v3(m[3 * i], m[3 * i + 1], m[3 * i + 2]) : v3(m[i], m[3 + i], m[6 + i]);
}
static inline V3 poseApply(const float* m, V3 p, int transposed) {
    V3 a0 = poseAxis(m, 0, transposed), a1 = poseAxis(m, 1, transposed), a2 = poseAxis(m, 2, transposed);
    return v3(m[9], m[10], m[11]) + a0 * p.x + a1 * p.y + a2 * p.z;
}

static void boxAabb(Geom& g) {
    V3 h;
    h.x = fabs_(g.ax[0].x) * g.e.x + fabs_(g.ax[1].x) * g.e.y + fabs_(g.ax[2].x) * g.e.z;
    h.y = fabs_(g.ax[0].y) * g.e.x + fabs_(g.ax[1].y) * g.e.y + fabs_(g.ax[2].y) * g.e.z;
    h.z = fabs_(g.ax[0].z) * g.e.x + fabs_(g.ax[1].z) * g.e.y + fabs_(g.ax[2].z) * g.e.z;
    g.aabb.mn = g.c - h; g.aabb.mx = g.c + h;
}

// Local hull planes for a convex mesh, cached by (mesh pointer, vertex count, first vertex).
static int hullFor(void* mesh) {
    NxU32 nv = vfn<FN_meshU32>(mesh, MESH_getCount)(mesh, 0, 0, 1 /*VERTICES*/);
    NxU32 fmt = vfn<FN_meshU32>(mesh, MESH_getFormat)(mesh, 0, 0, 1);
    const char* base = (const char*)vfn<FN_meshPtr>(mesh, MESH_getBase)(mesh, 0, 0, 1);
    NxU32 stride = vfn<FN_meshU32>(mesh, MESH_getStride)(mesh, 0, 0, 1);
    if (!base || nv < 4 || fmt != 1 /*FLOAT*/ || stride < 12 || nv > 64) return -1;
    const float* f0 = (const float*)base;
    for (int i = 0; i < g_nHulls; ++i) {
        HullEntry& h = g_hulls[i];
        if (h.mesh == mesh && h.nv == nv && h.v0[0] == f0[0] && h.v0[1] == f0[1] && h.v0[2] == f0[2]) return i;
    }
    V3* v = allocN<V3>(nv);
    B3 bb; bEmpty(bb);
    for (NxU32 i = 0; i < nv; ++i) { const float* p = (const float*)(base + (size_t)stride * i); v[i] = v3(p[0], p[1], p[2]); bAdd(bb, v[i]); }
    float ext = len(bb.mx - bb.mn);
    float eps = 1e-4f * (ext > 1e-6f ? ext : 1.0f);
    int start = g_nHullPlanes, count = 0;
    for (NxU32 i = 0; i < nv; ++i) for (NxU32 j = i + 1; j < nv; ++j) for (NxU32 k = j + 1; k < nv; ++k) {
        V3 n = cross(v[j] - v[i], v[k] - v[i]);
        float l = len(n);
        if (l < 1e-12f) continue;
        n = n * (1.0f / l);
        float d = dot(n, v[i]);
        bool anyPos = false, anyNeg = false;
        for (NxU32 m = 0; m < nv && !(anyPos && anyNeg); ++m) {
            float s = dot(n, v[m]) - d;
            if (s > eps) anyPos = true; else if (s < -eps) anyNeg = true;
        }
        if (anyPos && anyNeg) continue;
        if (anyPos) { n = n * -1.0f; d = -d; }          // make all vertices satisfy n.v - d <= 0
        bool dup = false;
        for (int q = start; q < start + count; ++q)
            if (dot(g_hullPlanes[q].n, n) > 0.9999f && fabs_(g_hullPlanes[q].d - d) < eps) { dup = true; break; }
        if (dup) continue;
        grow(g_hullPlanes, g_capHullPlanes, g_nHullPlanes + 1);
        g_hullPlanes[g_nHullPlanes].n = n; g_hullPlanes[g_nHullPlanes].d = d; ++g_nHullPlanes; ++count;
    }
    zfree(v);
    if (count < 4) { g_nHullPlanes = start; return -1; }
    grow(g_hulls, g_capHulls, g_nHulls + 1);
    HullEntry& h = g_hulls[g_nHulls];
    h.mesh = mesh; h.nv = nv; h.v0[0] = f0[0]; h.v0[1] = f0[1]; h.v0[2] = f0[2]; h.start = start; h.count = count;
    return g_nHulls++;
}

// Transform a cached local hull into world planes; returns false if unusable.
static void hullToWorld(int hull, const float* pose, int tr, Plane4*& arr, int& n, int& cap, Geom& g) {
    HullEntry& h = g_hulls[hull];
    V3 a0 = poseAxis(pose, 0, tr), a1 = poseAxis(pose, 1, tr), a2 = poseAxis(pose, 2, tr), t = v3(pose[9], pose[10], pose[11]);
    grow(arr, cap, n + h.count);
    g.p0 = n; g.pc = h.count;
    for (int i = 0; i < h.count; ++i) {
        const Plane4& lp = g_hullPlanes[h.start + i];
        V3 wn = a0 * lp.n.x + a1 * lp.n.y + a2 * lp.n.z;
        arr[n].n = wn; arr[n].d = lp.d + dot(wn, t); ++n;
    }
}

// Actor-level flags of the actor whose shapes are being read (set by the world builders below).
// NxActorFlag NX_AF_DISABLE_COLLISION = 1, NX_AF_FLUID_DISABLE_COLLISION = 8 (NpActor::readActorFlag, vtable slot 27,
// returns ([actor+0x2c] & flag) != 0). PhysX fluids ignore every shape of such actors; 1.1.2 only looked at shape flags,
// so invisible actors marked 'fluids pass through' held glass shards and debris in mid-air. Cloth is not affected.
static NxU32 g_curActorFlags = 0;
typedef bool (__fastcall *FN_readActorFlag)(void*, void*, NxU32);
static NxU32 actorFlagsOf(void* act) {
    NxU32 f = 0;
    if (vfn<FN_readActorFlag>(act, AC_readActorFlag)(act, 0, 1)) f |= 1;
    if (vfn<FN_readActorFlag>(act, AC_readActorFlag)(act, 0, 8)) f |= 8;
    return f;
}

// Read a shape's common data; returns false if fluids can never collide with it.
static bool shapeCommon(void* sh, Geom& g, NxU32& type) {
    NxU32 flags = vfn<FN_flagMask>(sh, SH_getFlag)(sh, 0, 0x1FBFFF);
    if (flags & (SF_TRIGGER | SF_DISABLE_COLLISION)) return false;
    unsigned char no = (unsigned char)(((flags & SF_FLUID_DISABLE_COLLISION) || (g_curActorFlags & 9) ? 1 : 0) | ((flags & SF_CLOTH_DISABLE_COLLISION) ? 2 : 0));
    g.dbgShapeFlags = flags; g.dbgActorFlags = g_curActorFlags;
    if (no == 3) return false;
    type = vfn<FN_u32>(sh, SH_getType)(sh, 0);
    g.group = vfn<FN_u16>(sh, SH_getGroup)(sh, 0) & 31;
    vfn<FN_retMask>(sh, SH_getGroupsMask)(sh, 0, &g.mask);
    g.drain = (flags & SF_FLUID_DRAIN) ? 1 : 0;
    g.pad = no;
    return true;
}

// Detect whether NxMat34 rotation is row- or column-major by comparing our box AABBs with PhysX's.
// Only shapes where exactly one convention reproduces PhysX's world bounds count as a vote: boxes rotated
// about a single axis give identical bounds for R and R^T, convex vertex sets usually don't. Tie -> row-major
// (the SDK default, NxMat33 without TRANSPOSED_MAT33).
static void detectOrientation(World& w, void** actors, NxU32 nActors) {
    int votes[2] = { 0, 0 }, tested = 0;
    for (NxU32 a = 0; a < nActors && votes[0] + votes[1] < 32 && tested < 4000; ++a) {
        void* act = actors[a];
        NxU32 ns = vfn<FN_u32>(act, AC_getNbShapes)(act, 0);
        void** shapes = (void**)vfn<FN_ptr>(act, AC_getShapes)(act, 0);
        for (NxU32 s = 0; s < ns && shapes; ++s) {
            void* sh = shapes[s];
            NxU32 type = vfn<FN_u32>(sh, SH_getType)(sh, 0);
            if (type != ST_BOX && type != ST_CONVEX) continue;
            ++tested;
            float pose[12]; vfn<FN_retPose>(sh, SH_getGlobalPose)(sh, 0, pose);
            B3 ref; vfn<FN_bounds>(sh, SH_getWorldBounds)(sh, 0, &ref);
            float tol = 2e-3f * (len(ref.mx - ref.mn) + 1.0f);
            bool match[2] = { false, false };
            for (int t = 0; t < 2; ++t) {
                B3 bb;
                if (type == ST_BOX) {
                    Geom g; memset(&g, 0, sizeof(g));
                    g.c = v3(pose[9], pose[10], pose[11]);
                    for (int i = 0; i < 3; ++i) g.ax[i] = poseAxis(pose, i, t);
                    vfn<FN_retVec>(sh, BOX_getDimensions)(sh, 0, &g.e);
                    boxAabb(g); bb = g.aabb;
                } else {
                    void* mesh = vfn<FN_ptr>(sh, CVX_getMesh)(sh, 0);
                    if (!mesh) break;
                    NxU32 nv = vfn<FN_meshU32>(mesh, MESH_getCount)(mesh, 0, 0, 1);
                    NxU32 fmt = vfn<FN_meshU32>(mesh, MESH_getFormat)(mesh, 0, 0, 1);
                    const char* base = (const char*)vfn<FN_meshPtr>(mesh, MESH_getBase)(mesh, 0, 0, 1);
                    NxU32 stride = vfn<FN_meshU32>(mesh, MESH_getStride)(mesh, 0, 0, 1);
                    if (!base || !nv || fmt != 1 || stride < 12) break;
                    bEmpty(bb);
                    for (NxU32 i = 0; i < nv; ++i) { const float* q = (const float*)(base + (size_t)stride * i); bAdd(bb, poseApply(pose, v3(q[0], q[1], q[2]), t)); }
                }
                match[t] = len(bb.mn - ref.mn) < tol && len(bb.mx - ref.mx) < tol;
            }
            if (match[0] != match[1]) ++votes[match[1] ? 1 : 0];
        }
    }
    w.transposed = votes[1] > votes[0] ? 1 : 0;
    LOG("FLUIDREP pose orientation: %d shapes checked, discriminating votes row-major %d / column-major %d -> using %s",
        tested, votes[0], votes[1], w.transposed ? "column-major" : "row-major");
}

// Build a geom from a shape at its current pose. Returns false for unsupported/empty shapes.
static bool buildGeom(World& w, void* sh, Geom& g, bool dynamic, Plane4*& planes, int& nPlanes, int& capPlanes, int& outLocalHull) {
    memset(&g, 0, sizeof(g));
    NxU32 type;
    if (!shapeCommon(sh, g, type)) return false;
    g.dynamic = dynamic ? 1 : 0;
    g.shape = sh;
    outLocalHull = -1;
    float pose[12];
    if (type != ST_MESH) vfn<FN_retPose>(sh, SH_getGlobalPose)(sh, 0, pose);
    int tr = w.transposed < 0 ? 0 : w.transposed;
    switch (type) {
    case ST_BOX: {
        g.type = GT_BOX;
        g.c = v3(pose[9], pose[10], pose[11]);
        for (int i = 0; i < 3; ++i) g.ax[i] = poseAxis(pose, i, tr);
        vfn<FN_retVec>(sh, BOX_getDimensions)(sh, 0, &g.e);
        boxAabb(g);
        return true;
    }
    case ST_SPHERE: {
        g.type = GT_SPHERE;
        g.c = v3(pose[9], pose[10], pose[11]);
        g.r = vfn<FN_f>(sh, SPH_getRadius)(sh, 0);
        g.aabb.mn = g.c - v3(g.r, g.r, g.r); g.aabb.mx = g.c + v3(g.r, g.r, g.r);
        return true;
    }
    case ST_CAPSULE: {
        g.type = GT_CAPSULE;
        g.r = vfn<FN_f>(sh, CAP_getRadius)(sh, 0);
        float h = vfn<FN_f>(sh, CAP_getHeight)(sh, 0) * 0.5f;
        V3 ay = poseAxis(pose, 1, tr), c = v3(pose[9], pose[10], pose[11]);
        g.c = c - ay * h; g.b = c + ay * h;
        bEmpty(g.aabb); bAdd(g.aabb, g.c); bAdd(g.aabb, g.b);
        g.aabb.mn = g.aabb.mn - v3(g.r, g.r, g.r); g.aabb.mx = g.aabb.mx + v3(g.r, g.r, g.r);
        return true;
    }
    case ST_CONVEX: {
        void* mesh = vfn<FN_ptr>(sh, CVX_getMesh)(sh, 0);
        if (!mesh) return false;
        int hull = hullFor(mesh);
        if (hull < 0) {
            // too many vertices for the brute-force hull: use an oriented box around the local vertices
            NxU32 nv = vfn<FN_meshU32>(mesh, MESH_getCount)(mesh, 0, 0, 1);
            NxU32 fmt = vfn<FN_meshU32>(mesh, MESH_getFormat)(mesh, 0, 0, 1);
            const char* base = (const char*)vfn<FN_meshPtr>(mesh, MESH_getBase)(mesh, 0, 0, 1);
            NxU32 stride = vfn<FN_meshU32>(mesh, MESH_getStride)(mesh, 0, 0, 1);
            if (!base || !nv || fmt != 1 || stride < 12) return false;
            B3 lb; bEmpty(lb);
            for (NxU32 i = 0; i < nv; ++i) { const float* q = (const float*)(base + (size_t)stride * i); bAdd(lb, v3(q[0], q[1], q[2])); }
            g.type = GT_BOX;
            g.c = poseApply(pose, (lb.mn + lb.mx) * 0.5f, tr);
            for (int i = 0; i < 3; ++i) g.ax[i] = poseAxis(pose, i, tr);
            g.e = (lb.mx - lb.mn) * 0.5f;
            g.b = (lb.mn + lb.mx) * 0.5f;     // local center offset, used when refreshing dynamic poses
            boxAabb(g);
            g.localPlanes = -1;
            return true;
        }
        g.type = GT_CONVEX;
        outLocalHull = hull; g.localPlanes = hull;
        hullToWorld(hull, pose, tr, planes, nPlanes, capPlanes, g);
        vfn<FN_bounds>(sh, SH_getWorldBounds)(sh, 0, &g.aabb);
        return true;
    }
    default:
        if (type < 8) w.skippedTypes[type]++;
        return false;
    }
}

// Triangle meshes (static only): one geom per triangle.
static void addTriangleMesh(World& w, void* sh) {
    Geom proto; memset(&proto, 0, sizeof(proto));
    NxU32 type;
    if (!shapeCommon(sh, proto, type)) return;
    void* mesh = vfn<FN_ptr>(sh, TRI_getMesh)(sh, 0);
    if (!mesh) return;
    float pose[12]; vfn<FN_retPose>(sh, SH_getGlobalPose)(sh, 0, pose);
    int tr = w.transposed < 0 ? 0 : w.transposed;
    NxU32 nt = vfn<FN_meshU32>(mesh, MESH_getCount)(mesh, 0, 0, 0 /*TRIANGLES*/);
    NxU32 tfmt = vfn<FN_meshU32>(mesh, MESH_getFormat)(mesh, 0, 0, 0);
    const char* tb = (const char*)vfn<FN_meshPtr>(mesh, MESH_getBase)(mesh, 0, 0, 0);
    NxU32 tstr = vfn<FN_meshU32>(mesh, MESH_getStride)(mesh, 0, 0, 0);
    NxU32 nv = vfn<FN_meshU32>(mesh, MESH_getCount)(mesh, 0, 0, 1);
    NxU32 vfmt = vfn<FN_meshU32>(mesh, MESH_getFormat)(mesh, 0, 0, 1);
    const char* vb = (const char*)vfn<FN_meshPtr>(mesh, MESH_getBase)(mesh, 0, 0, 1);
    NxU32 vstr = vfn<FN_meshU32>(mesh, MESH_getStride)(mesh, 0, 0, 1);
    if (!tb || !vb || vfmt != 1 || nt == 0 || nt > 2000000) { LOG("FLUIDREP triangle mesh %p unsupported (tris %u fmt %u/%u)", sh, nt, tfmt, vfmt); return; }
    grow(w.sg, w.capSg, w.nSg + (int)nt);
    for (NxU32 t = 0; t < nt; ++t) {
        NxU32 idx[3];
        for (int k = 0; k < 3; ++k) {
            const char* p = tb + (size_t)tstr * t;
            idx[k] = tfmt == 4 ? ((const NxU32*)p)[k] : tfmt == 3 ? ((const unsigned short*)p)[k] : ((const unsigned char*)p)[k];
            if (idx[k] >= nv) idx[k] = 0;
        }
        Geom& g = w.sg[w.nSg];
        g = proto;
        g.type = GT_TRI;
        V3 p0, p1, p2;
        { const float* f = (const float*)(vb + (size_t)vstr * idx[0]); p0 = poseApply(pose, v3(f[0], f[1], f[2]), tr); }
        { const float* f = (const float*)(vb + (size_t)vstr * idx[1]); p1 = poseApply(pose, v3(f[0], f[1], f[2]), tr); }
        { const float* f = (const float*)(vb + (size_t)vstr * idx[2]); p2 = poseApply(pose, v3(f[0], f[1], f[2]), tr); }
        V3 n = cross(p1 - p0, p2 - p0);
        if (len(n) < 1e-12f) continue;
        g.t0 = p0; g.t1 = p1; g.t2 = p2; g.tn = norm(n);
        bEmpty(g.aabb); bAdd(g.aabb, p0); bAdd(g.aabb, p1); bAdd(g.aabb, p2);
        ++w.nSg;
    }
}

// ---------------------------------------------------------------------------
// BVH
// ---------------------------------------------------------------------------
static int bvhBuildRec(BVH& b, const Geom* geoms, int* items, int start, int count, int depth) {
    int ni = b.nNodes;
    grow(b.nodes, b.capNodes, b.nNodes + 1);
    ++b.nNodes;
    B3 bb; bEmpty(bb);
    B3 cb; bEmpty(cb);
    for (int i = start; i < start + count; ++i) {
        const B3& gb = geoms[items[i]].aabb;
        bAddB(bb, gb);
        bAdd(cb, (gb.mn + gb.mx) * 0.5f);
    }
    b.nodes[ni].b = bb;
    if (count <= 4 || depth > 40) { b.nodes[ni].left = start; b.nodes[ni].count = count; return ni; }
    V3 ext = cb.mx - cb.mn;
    int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
    float split = axis == 0 ? (cb.mn.x + cb.mx.x) * 0.5f : axis == 1 ? (cb.mn.y + cb.mx.y) * 0.5f : (cb.mn.z + cb.mx.z) * 0.5f;
    int mid = start;
    for (int i = start; i < start + count; ++i) {
        const B3& gb = geoms[items[i]].aabb;
        float c = axis == 0 ? (gb.mn.x + gb.mx.x) * 0.5f : axis == 1 ? (gb.mn.y + gb.mx.y) * 0.5f : (gb.mn.z + gb.mx.z) * 0.5f;
        if (c < split) { int t = items[i]; items[i] = items[mid]; items[mid] = t; ++mid; }
    }
    if (mid == start || mid == start + count) mid = start + count / 2;
    b.nodes[ni].count = 0;
    int l = bvhBuildRec(b, geoms, items, start, mid - start, depth + 1);
    int r = bvhBuildRec(b, geoms, items, mid, start + count - mid, depth + 1);
    (void)r;
    b.nodes[ni].left = l;   // right child is always the node built right after the left subtree; store explicitly:
    b.nodes[ni].count = -r; // negative count = internal node, -count = right child index
    return ni;
}
static void bvhBuild(BVH& b, const Geom* geoms, const int* idx, int n) {
    b.nNodes = 0;
    grow(b.items, b.capItems, n);
    memcpy(b.items, idx, sizeof(int) * n);
    b.nItems = n;
    if (n) bvhBuildRec(b, geoms, b.items, 0, n, 0);
}

// ---------------------------------------------------------------------------
// narrow phase: moving sphere (radius r) from p0 along D (t in [0,1]) vs geom
// ---------------------------------------------------------------------------
struct Hit { float t; V3 n; float depth; bool inside; };

static bool hitBox(const Geom& g, V3 p0, V3 D, float r, Hit& h) {
    V3 d0 = p0 - g.c;
    float lp[3] = { dot(d0, g.ax[0]), dot(d0, g.ax[1]), dot(d0, g.ax[2]) };
    float ld[3] = { dot(D, g.ax[0]), dot(D, g.ax[1]), dot(D, g.ax[2]) };
    float E[3] = { g.e.x + r, g.e.y + r, g.e.z + r };
    if (fabs_(lp[0]) <= E[0] && fabs_(lp[1]) <= E[1] && fabs_(lp[2]) <= E[2]) {
        int best = 0; float bestPen = BIG;
        for (int i = 0; i < 3; ++i) { float pen = E[i] - fabs_(lp[i]); if (pen < bestPen) { bestPen = pen; best = i; } }
        h.inside = true; h.t = 0; h.depth = bestPen; h.n = g.ax[best] * (lp[best] >= 0 ? 1.0f : -1.0f);
        return true;
    }
    float tmin = 0, tmax = 1; int axis = -1; float sgn = 1;
    for (int i = 0; i < 3; ++i) {
        if (fabs_(ld[i]) < 1e-12f) { if (fabs_(lp[i]) > E[i]) return false; continue; }
        float inv = 1.0f / ld[i];
        float t1 = (-E[i] - lp[i]) * inv, t2 = (E[i] - lp[i]) * inv;
        float s = -1.0f;                       // entering through the -E face when moving +
        if (t1 > t2) { float t = t1; t1 = t2; t2 = t; s = 1.0f; }
        if (t1 > tmin) { tmin = t1; axis = i; sgn = s; }
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return false;
    }
    if (axis < 0) return false;
    h.inside = false; h.t = tmin; h.depth = 0; h.n = g.ax[axis] * sgn;
    return true;
}

static bool hitConvex(const Plane4* planes, const Geom& g, V3 p0, V3 D, float r, Hit& h) {
    float tE = 0, tX = 1, maxS0 = -BIG; int best = -1, maxIdx = -1;
    for (int i = g.p0; i < g.p0 + g.pc; ++i) {
        float d = planes[i].d + r;
        float s0 = dot(planes[i].n, p0) - d;
        float s1 = s0 + dot(planes[i].n, D);
        if (s0 > maxS0) { maxS0 = s0; maxIdx = i; }
        if (s0 > 0 && s1 > 0) return false;
        if (s0 > 0) { float t = s0 / (s0 - s1); if (t > tE) { tE = t; best = i; } }
        else if (s1 > 0) { float t = s0 / (s0 - s1); if (t < tX) tX = t; }
        if (tE > tX) return false;
    }
    if (best < 0) {
        if (maxIdx < 0) return false;
        h.inside = true; h.t = 0; h.depth = -maxS0; h.n = planes[maxIdx].n;
        return true;
    }
    h.inside = false; h.t = tE; h.depth = 0; h.n = planes[best].n;
    return true;
}

static bool hitSphereAt(V3 c, float R, V3 p0, V3 D, Hit& h) {
    V3 q = p0 - c;
    float qq = dot(q, q);
    if (qq < R * R) {
        float l = fsqrt(qq);
        h.inside = true; h.t = 0; h.depth = R - l; h.n = l > 1e-12f ? q * (1.0f / l) : v3(0, 0, 1);
        return true;
    }
    float a = dot(D, D), b = dot(q, D), cc = qq - R * R;
    if (b > 0 || a < 1e-20f) return false;
    float disc = b * b - a * cc;
    if (disc < 0) return false;
    float t = (-b - fsqrt(disc)) / a;
    if (t < 0 || t > 1) return false;
    h.inside = false; h.t = t; h.depth = 0; h.n = norm(q + D * t);
    return true;
}

static bool hitCapsule(const Geom& g, V3 p0, V3 D, float r, Hit& h) {
    float R = g.r + r;
    V3 ab = g.b - g.c;
    float abab = dot(ab, ab);
    // inside test
    float s = abab > 1e-12f ? dot(p0 - g.c, ab) / abab : 0;
    s = s < 0 ? 0 : s > 1 ? 1 : s;
    V3 cp = g.c + ab * s;
    V3 q = p0 - cp;
    if (dot(q, q) < R * R) {
        float l = len(q);
        h.inside = true; h.t = 0; h.depth = R - l; h.n = l > 1e-12f ? q * (1.0f / l) : v3(0, 0, 1);
        return true;
    }
    bool any = false; Hit best; best.t = BIG;
    // infinite cylinder
    if (abab > 1e-12f) {
        V3 m = p0 - g.c;
        float md = dot(m, ab), nd = dot(D, ab), dd = abab;
        float nn = dot(D, D), mn = dot(m, D);
        float a = dd * nn - nd * nd;
        float k = dot(m, m) - R * R;
        float c = dd * k - md * md;
        float b = dd * mn - nd * md;
        if (fabs_(a) > 1e-12f) {
            float disc = b * b - a * c;
            if (disc >= 0) {
                float t = (-b - fsqrt(disc)) / a;
                if (t >= 0 && t <= 1) {
                    float sp = (md + t * nd) / dd;
                    if (sp >= 0 && sp <= 1) {
                        V3 hp = p0 + D * t;
                        best.inside = false; best.t = t; best.depth = 0; best.n = norm(hp - (g.c + ab * sp)); any = true;
                    }
                }
            }
        }
    }
    Hit hs;
    if (hitSphereAt(g.c, R, p0, D, hs) && hs.t < best.t) { best = hs; any = true; }
    if (hitSphereAt(g.b, R, p0, D, hs) && hs.t < best.t) { best = hs; any = true; }
    if (any) h = best;
    return any;
}

static bool hitTri(const Geom& g, V3 p0, V3 D, float r, Hit& h) {
    float s0 = dot(g.tn, p0 - g.t0);
    float side = s0 >= 0 ? 1.0f : -1.0f;
    V3 n = g.tn * side;
    float a0 = s0 * side - r, a1 = a0 + dot(n, D);
    float t;
    if (a0 < 0) {
        if (a0 < -2 * r) return false;      // well behind: ignore (two-sided thin surface)
        t = 0;
    } else {
        if (a1 >= 0) return false;
        t = a0 / (a0 - a1);
    }
    V3 p = p0 + D * t - n * (r + (a0 < 0 ? a0 : 0));
    // barycentric inside test
    V3 e0 = g.t1 - g.t0, e1 = g.t2 - g.t0, vp = p - g.t0;
    float d00 = dot(e0, e0), d01 = dot(e0, e1), d11 = dot(e1, e1), d20 = dot(vp, e0), d21 = dot(vp, e1);
    float den = d00 * d11 - d01 * d01;
    if (fabs_(den) < 1e-20f) return false;
    float v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den;
    if (v < 0 || w < 0 || v + w > 1) return false;
    h.inside = a0 < 0; h.t = t; h.depth = a0 < 0 ? -a0 : 0; h.n = n;
    return true;
}

// ---------------------------------------------------------------------------
// fluid object
// ---------------------------------------------------------------------------
static const NxU32 MAGIC = 0x464C5550;   // 'FLUP'

struct Particle { V3 p, v, n; float life; NxU32 id; NxU32 flags; NxU32 dead; float rho; int lastGeom; };
struct PendForce { NxU32 id; V3 f; NxU32 mode; };

struct Fluid {
    void** vtbl;          // +0  NxFluid ABI
    void*  userData;      // +4  NxFluid::userData
    NxU32  magic;
    void*  scene;
    int    index;
    CRITICAL_SECTION cs;

    // parameters (mutable through setters)
    NxU32 maxParticles, numReserve, curLimit, psm, simMethod, collMethod, flags;
    float rppm, restDensity, krm, mlMul, cdMul, stiffness, viscosity, surfaceTension, damping, fadeIn, collResp;
    float restS, dynFS, statFS, attrS, restD, dynFD, statFD, attrD;
    V3 extAcc; Plane4 projPlane;
    unsigned short group, ffMaterial; GMask mask;
    const char* name;
    void* compartment;
    PData write; PIdData delW, creW; NxU32 packetData[2];
    bool classDirty; int cls;

    // particles
    Particle* parts; NxU32 n;
    Particle* pend; NxU32 nPend;
    NxU32* freeIds; NxU32 nFree;
    int* idToIdx;
    NxU32* delReport; NxU32 nDelReport;
    NxU32* pendDel; NxU32 nPendDel;
    PendForce* pendForce; int nPendForce, capPendForce;
    // fluid packets (1.1.14): output order grouped by spatial cell + the NxFluidPacket list the game draws sprites from
    unsigned long long* pkKey; NxU32* pkIdx; NxU32* pkTmp; unsigned long long* pkKeyTmp; int capPk;
    NxU32 dPackets, dPacketsMax, dPacketsCapped;
    // diagnostics (Log=1): what the game asks for and where particles go
    unsigned long dReq, dAdded, dAddCalls, dGameDel, dLifeDel, dDrainDel, dRemoveAll, dUpdForces, dFrames, dSteps;
    float dMaxSpeed, dMaxLife; NxU32 dPeak; bool dSeen;
    bool pendRemoveAll;
    B3 bounds;

    // step snapshot (read by workers)
    V3 sAccel; float sDamping, sMotionLimit, sRadius, sH, sRestThresh; int sSub; NxU32 sFlags, sCollMethod;
    float sRestS, sDynFS, sStatFS, sRestD, sDynFD, sStatFD; Plane4 sPlane;
    World* sWorld;

    // SPH (simulationMethod SPH or mixed): snapshot taken in beginStep, read-only for the workers
    bool sph;
    V3* snapP; V3* snapV; float* snapRho;
    int* gridHead; int* gridNext; NxU32 gridMask;
    float kH, kH2, kInvH, kPoly6, kSpiky, kVisc, kMass, sStiff, sVisc, sRho0;
};

static Fluid* g_fluids[64];
static int g_nFluids = 0;
static volatile LONG g_slotCalls[81];

static Fluid* F(void* self) { return (Fluid*)self; }

static void reportDelete(Fluid* f, NxU32 id) { if (f->nDelReport < f->maxParticles) f->delReport[f->nDelReport++] = id; }

// --- vtable functions (thiscall emulated with fastcall + dummy edx) ---
#if defined(_MSC_VER)
#define RETADDR() _ReturnAddress()
#else
#define RETADDR() __builtin_return_address(0)
#endif
static void describeAddr(const void* a, char* out, int n) {
    HMODULE m = 0; wchar_t path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m)
        GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* base = path; for (const wchar_t* q = path; *q; ++q) if (*q == L'\\' || *q == L'/') base = q + 1;
    int k = 0; for (; base[k] && k < n - 24; ++k) out[k] = (char)base[k];
    out[k] = 0;
    ULONG_PTR off = m ? (ULONG_PTR)a - (ULONG_PTR)m : (ULONG_PTR)a;
    const char hex[] = "0123456789abcdef"; char t[16]; int ti = 0;
    do { t[ti++] = hex[off & 15]; off >>= 4; } while (off && ti < 8);
    out[k++] = '+'; out[k++] = '0'; out[k++] = 'x'; while (ti) out[k++] = t[--ti]; out[k] = 0;
}
static void firstCall(int slot, const void* caller) {
    char where[MAX_PATH + 32]; describeAddr(caller, where, sizeof(where));
    LOG("FLUIDREP first call of NxFluid slot %d from %s", slot, where);
}
#define CNT(i) do { if (InterlockedIncrement(&g_slotCalls[i]) == 1) firstCall(i, RETADDR()); } while (0)
static void* __fastcall s00_dtor(void* self, void*, NxU32) { CNT(0); return self; }
static void* __fastcall s01_createEmitter(void*, void*, const void*) { CNT(1); LOG("FLUIDREP createEmitter called: emitters are not supported"); return 0; }
static void  __fastcall s02_releaseEmitter(void*, void*, void*) { CNT(2); }
static NxU32 __fastcall s03_getNbEmitters(void*, void*) { CNT(3); return 0; }
static void* __fastcall s04_getEmitters(void*, void*) { CNT(4); return 0; }

static NxU32 __fastcall s05_addParticles(void* self, void*, const void* pdv, bool appendIds) {
    CNT(5);
    Fluid* f = F(self);
    const PData* pd = (const PData*)pdv;
    NxU32 req = (pd && pd->num) ? *pd->num : 0;
    EnterCriticalSection(&f->cs);
    NxU32 limit = f->curLimit < f->maxParticles ? f->curLimit : f->maxParticles;
    NxU32 used = f->n + f->nPend;
    NxU32 room = used < limit ? limit - used : 0;
    NxU32 cnt = req < room ? req : room;
    if (cnt > f->nFree) cnt = f->nFree;
    NxU32 creStart = 0;
    if (f->creW.num) creStart = appendIds ? *f->creW.num : 0;
    // PhysX 2.8.0 contract (release notes: "NxFluid::addParticles() now appends new particle data to the user
    // particle write buffer"): the new particles are appended to the write buffer at *numParticlesPtr and the
    // count is increased. Mirror's Edge relies on it: right after addParticles it reads each new particle's id
    // from the write buffer at (old count + i) to build its particle records. Without this it read stale ids,
    // its records and id table drifted apart, and the game eventually crashed in its particle code.
    const PData& wd = f->write;
    NxU32 wBase = wd.num ? *wd.num : 0;
    if (wBase > f->maxParticles) wBase = f->n + f->nPend;           // never trust a corrupt count
    for (NxU32 i = 0; i < cnt; ++i) {
        Particle& p = f->pend[f->nPend++];
        memset(&p, 0, sizeof(p));
        NxU32 id = f->freeIds[--f->nFree];
        p.id = id;
        if (pd->pos) { const float* s = atc<float>(pd->pos, pd->sPos, i); p.p = v3(s[0], s[1], s[2]); }
        if (pd->vel) { const float* s = atc<float>(pd->vel, pd->sVel, i); p.v = v3(s[0], s[1], s[2]); }
        p.life = pd->life ? *atc<float>(pd->life, pd->sLife, i) : -1.0f;
        if (p.life == 0.0f) p.life = -1.0f;            // 0 = no lifetime
        f->idToIdx[id] = -1;
        if (f->creW.num && f->creW.id && creStart + i < f->maxParticles) *at<NxU32>(f->creW.id, f->creW.sId, creStart + i) = id;
        NxU32 k = wBase + i;
        if (wd.num && k < f->maxParticles) {
            if (wd.pos)  { float* o = at<float>(wd.pos, wd.sPos, k); o[0] = p.p.x; o[1] = p.p.y; o[2] = p.p.z; }
            if (wd.vel)  { float* o = at<float>(wd.vel, wd.sVel, k); o[0] = p.v.x; o[1] = p.v.y; o[2] = p.v.z; }
            if (wd.life) { *at<float>(wd.life, wd.sLife, k) = p.life < 0 ? 0.0f : p.life; }
            if (wd.dens) { *at<float>(wd.dens, wd.sDens, k) = f->restDensity; }
            if (wd.id)   { *at<NxU32>(wd.id, wd.sId, k) = id; }
            if (wd.flag) { *at<NxU32>(wd.flag, wd.sFlag, k) = 0; }
            if (wd.nrm)  { float* o = at<float>(wd.nrm, wd.sNrm, k); o[0] = 0; o[1] = 0; o[2] = 0; }
            bAdd(f->bounds, p.p);
        }
    }
    ++f->dAddCalls; f->dReq += req; f->dAdded += cnt;
    for (NxU32 i = 0; i < cnt && pd && pd->life; ++i) { float lf = *atc<float>(pd->life, pd->sLife, i); if (lf > f->dMaxLife) f->dMaxLife = lf; }
    if (wd.num && cnt) { NxU32 t = wBase + cnt; *wd.num = t < f->maxParticles ? t : f->maxParticles; }
    if (f->creW.num) { NxU32 total = creStart + cnt; *f->creW.num = total < f->maxParticles ? total : f->maxParticles; }
    LeaveCriticalSection(&f->cs);
    return cnt;
}

static void __fastcall s06_removeAllParticles(void* self, void*) { CNT(6); Fluid* f = F(self); EnterCriticalSection(&f->cs); f->pendRemoveAll = true; LeaveCriticalSection(&f->cs); }
static void  __fastcall s07_setWrite(void* self, void*, const PData* d) { CNT(7); F(self)->write = *d; }
static PData* __fastcall s08_getWrite(void* self, void*, PData* out) { CNT(8); *out = F(self)->write; return out; }
static void  __fastcall s09_setDel(void* self, void*, const PIdData* d) { CNT(9); F(self)->delW = *d; }
static PIdData* __fastcall s10_getDel(void* self, void*, PIdData* out) { CNT(10); *out = F(self)->delW; return out; }
static void  __fastcall s11_setCre(void* self, void*, const PIdData* d) { CNT(11); F(self)->creW = *d; }
static PIdData* __fastcall s12_getCre(void* self, void*, PIdData* out) { CNT(12); *out = F(self)->creW; return out; }
static NxU32 __fastcall s13_getReserve(void* self, void*) { CNT(13); return F(self)->numReserve; }
static void  __fastcall s14_setReserve(void* self, void*, NxU32 v) { CNT(14); F(self)->numReserve = v; }
static NxU32 __fastcall s15_getLimit(void* self, void*) { CNT(15); return F(self)->curLimit; }
static void  __fastcall s16_setLimit(void* self, void*, NxU32 v) { CNT(16); F(self)->curLimit = v < F(self)->maxParticles ? v : F(self)->maxParticles; }

static void __fastcall s17_updateParticles(void* self, void*, const PUpdate* u) {
    CNT(17);
    Fluid* f = F(self);
    if (!u || !u->id) return;
    EnterCriticalSection(&f->cs);
    for (NxU32 i = 0; i < u->num; ++i) {
        NxU32 id = *atc<NxU32>(u->id, u->sId, i);
        if (id >= f->maxParticles) continue;
        if (u->flag && (*atc<NxU32>(u->flag, u->sFlag, i) & 1)) {
            if (f->nPendDel < f->maxParticles) f->pendDel[f->nPendDel++] = id;
        } else if (u->force) {
            const float* fv = atc<float>(u->force, u->sForce, i);
            grow(f->pendForce, f->capPendForce, f->nPendForce + 1);
            PendForce& pf = f->pendForce[f->nPendForce++];
            pf.id = id; pf.f = v3(fv[0], fv[1], fv[2]); pf.mode = u->forceMode;
        }
    }
    LeaveCriticalSection(&f->cs);
}

static void  __fastcall s18_setPacket(void* self, void*, const NxU32* d) { CNT(18); F(self)->packetData[0] = d[0]; F(self)->packetData[1] = d[1]; }
static NxU32* __fastcall s19_getPacket(void* self, void*, NxU32* out) { CNT(19); out[0] = F(self)->packetData[0]; out[1] = F(self)->packetData[1]; return out; }
static NxU32 __fastcall s20_getSim(void* self, void*) { CNT(20); return F(self)->simMethod; }
static void  __fastcall s21_setSim(void* self, void*, NxU32 v) { CNT(21); if (v != 2) LOG("FLUIDREP setSimulationMethod(%u) ignored (only no-interaction is replaced)", v); }
#define FGET(slot, field) static float __fastcall s##slot##_get(void* self, void*) { CNT(slot); return F(self)->field; }
#define FSET(slot, field) static void  __fastcall s##slot##_set(void* self, void*, float v) { CNT(slot); F(self)->field = v; }
FGET(22, stiffness) FSET(23, stiffness) FGET(24, viscosity) FSET(25, viscosity) FGET(26, surfaceTension) FSET(27, surfaceTension)
FGET(28, damping) FSET(29, damping) FGET(30, fadeIn) FSET(31, fadeIn)
static V3*   __fastcall s32_getExt(void* self, void*, V3* out) { CNT(32); *out = F(self)->extAcc; return out; }
static void  __fastcall s33_setExt(void* self, void*, const V3* v) { CNT(33); F(self)->extAcc = *v; }
static Plane4* __fastcall s34_getPlane(void* self, void*, Plane4* out) { CNT(34); *out = F(self)->projPlane; return out; }
static void  __fastcall s35_setPlane(void* self, void*, const Plane4* p) { CNT(35); F(self)->projPlane = *p; }
static NxU32 __fastcall s36_getCollMethod(void* self, void*) { CNT(36); return F(self)->collMethod; }
static void  __fastcall s37_setCollMethod(void* self, void*, NxU32 v) { CNT(37); F(self)->collMethod = v; F(self)->classDirty = true; }
FGET(38, restS) FSET(39, restS) FGET(40, dynFS) FSET(41, dynFS) FGET(42, statFS) FSET(43, statFS) FGET(44, attrS) FSET(45, attrS)
FGET(46, restD) FSET(47, restD) FGET(48, dynFD) FSET(49, dynFD) FGET(50, statFD) FSET(51, statFD) FGET(52, attrD) FSET(53, attrD)
static void  __fastcall s54_setCollResp(void* self, void*, float v) { CNT(54); F(self)->collResp = v; }
static float __fastcall s55_getCollResp(void* self, void*) { CNT(55); return F(self)->collResp; }
static void  __fastcall s56_setFlag(void* self, void*, NxU32 flag, bool val) { CNT(56); Fluid* f = F(self); if (val) f->flags |= flag; else f->flags &= ~flag; }
static NxU32 __fastcall s57_getFlag(void* self, void*, NxU32 flag) { CNT(57); return F(self)->flags & flag; }
static void* __fastcall s58_getScene(void* self, void*) { CNT(58); return F(self)->scene; }
static NxU32 __fastcall s59_getMax(void* self, void*) { CNT(59); return F(self)->maxParticles; }
static float __fastcall s60_getKrm(void* self, void*) { CNT(60); return F(self)->krm; }
static float __fastcall s61_getMl(void* self, void*) { CNT(61); return F(self)->mlMul; }
static float __fastcall s62_getCd(void* self, void*) { CNT(62); return F(self)->cdMul; }
static NxU32 __fastcall s63_getPsm(void* self, void*) { CNT(63); return F(self)->psm; }
static float __fastcall s64_getRppm(void* self, void*) { CNT(64); return F(self)->rppm; }
static float __fastcall s65_getRho(void* self, void*) { CNT(65); return F(self)->restDensity; }
static float __fastcall s66_getRestDist(void* self, void*) { CNT(66); return 1.0f / F(self)->rppm; }
static float __fastcall s67_getMass(void* self, void*) { CNT(67); Fluid* f = F(self); return f->restDensity / (f->rppm * f->rppm * f->rppm); }
static void  __fastcall s68_setGroup(void* self, void*, NxU32 g) { CNT(68); F(self)->group = (unsigned short)(g & 31); F(self)->classDirty = true; }
static NxU32 __fastcall s69_getGroup(void* self, void*) { CNT(69); return F(self)->group; }
static void  __fastcall s70_setMask(void* self, void*, const GMask* m) { CNT(70); F(self)->mask = *m; F(self)->classDirty = true; }
static GMask* __fastcall s71_getMask(void* self, void*, GMask* out) { CNT(71); *out = F(self)->mask; return out; }
static void  __fastcall s72_getBounds(void* self, void*, B3* out) { CNT(72); *out = F(self)->bounds; }
static NxU32 __fastcall s73_load(void*, void*, const void*) { CNT(73); return 0; }
static NxU32 __fastcall s74_save(void*, void*, void*) { CNT(74); return 0; }
static NxU32 __fastcall s75_ppuTime(void*, void*) { CNT(75); return 0; }
static void  __fastcall s76_setName(void* self, void*, const char* n) { CNT(76); F(self)->name = n; }
static const char* __fastcall s77_getName(void* self, void*) { CNT(77); return F(self)->name; }
static void* __fastcall s78_getCompartment(void* self, void*) { CNT(78); return F(self)->compartment; }
static NxU32 __fastcall s79_getFfm(void* self, void*) { CNT(79); return F(self)->ffMaterial; }
static void  __fastcall s80_setFfm(void* self, void*, NxU32 v) { CNT(80); F(self)->ffMaterial = (unsigned short)v; }

static void* g_vtbl[81] = {
    (void*)s00_dtor, (void*)s01_createEmitter, (void*)s02_releaseEmitter, (void*)s03_getNbEmitters, (void*)s04_getEmitters,
    (void*)s05_addParticles, (void*)s06_removeAllParticles, (void*)s07_setWrite, (void*)s08_getWrite, (void*)s09_setDel,
    (void*)s10_getDel, (void*)s11_setCre, (void*)s12_getCre, (void*)s13_getReserve, (void*)s14_setReserve,
    (void*)s15_getLimit, (void*)s16_setLimit, (void*)s17_updateParticles, (void*)s18_setPacket, (void*)s19_getPacket,
    (void*)s20_getSim, (void*)s21_setSim, (void*)s22_get, (void*)s23_set, (void*)s24_get, (void*)s25_set, (void*)s26_get,
    (void*)s27_set, (void*)s28_get, (void*)s29_set, (void*)s30_get, (void*)s31_set, (void*)s32_getExt, (void*)s33_setExt,
    (void*)s34_getPlane, (void*)s35_setPlane, (void*)s36_getCollMethod, (void*)s37_setCollMethod, (void*)s38_get,
    (void*)s39_set, (void*)s40_get, (void*)s41_set, (void*)s42_get, (void*)s43_set, (void*)s44_get, (void*)s45_set,
    (void*)s46_get, (void*)s47_set, (void*)s48_get, (void*)s49_set, (void*)s50_get, (void*)s51_set, (void*)s52_get,
    (void*)s53_set, (void*)s54_setCollResp, (void*)s55_getCollResp, (void*)s56_setFlag, (void*)s57_getFlag,
    (void*)s58_getScene, (void*)s59_getMax, (void*)s60_getKrm, (void*)s61_getMl, (void*)s62_getCd, (void*)s63_getPsm,
    (void*)s64_getRppm, (void*)s65_getRho, (void*)s66_getRestDist, (void*)s67_getMass, (void*)s68_setGroup,
    (void*)s69_getGroup, (void*)s70_setMask, (void*)s71_getMask, (void*)s72_getBounds, (void*)s73_load, (void*)s74_save,
    (void*)s75_ppuTime, (void*)s76_setName, (void*)s77_getName, (void*)s78_getCompartment, (void*)s79_getFfm, (void*)s80_setFfm };

// ---------------------------------------------------------------------------
// filter classes
// ---------------------------------------------------------------------------
static bool geomPasses(const World& w, const FilterClass& c, const Geom& g) {
    if (!(c.collMethod & (g.dynamic ? 2u : 1u))) return false;
    if (g.pad & (c.kind ? 2 : 1)) return false;
    if (!w.groupPair[c.group & 31][g.group & 31]) return false;
    return groupsMaskPass(c.mask, g.mask, w.op0, w.op1, w.op2, w.k0, w.k1, w.fb);
}

static void rebuildClassStatic(World& w, FilterClass& c) {
    c.nS = 0;
    grow(c.sIdx, c.capS, w.nSg);
    for (int i = 0; i < w.nSg; ++i) if (geomPasses(w, c, w.sg[i])) c.sIdx[c.nS++] = i;
    bvhBuild(c.sBvh, w.sg, c.sIdx, c.nS);
}

static int classFor(World& w, unsigned short group, const GMask& mask, NxU32 collMethod, NxU32 kind) {
    for (int i = 0; i < w.nCls; ++i) {
        FilterClass& c = w.cls[i];
        if (c.group == group && c.collMethod == collMethod && c.kind == kind && memcmp(&c.mask, &mask, sizeof(GMask)) == 0) return i;
    }
    if (w.nCls >= 16) return 0;
    FilterClass& c = w.cls[w.nCls];
    memset(&c, 0, sizeof(c));
    c.group = group; c.mask = mask; c.collMethod = collMethod; c.kind = kind;
    rebuildClassStatic(w, c);
    w.dynDirty = true;
    LOG("FLUIDREP filter class %d (%s): group %u mask %08x %08x %08x %08x method %u -> %d of %d static geoms collide",
        w.nCls, kind ? "cloth" : "fluid", c.group, c.mask.b[0], c.mask.b[1], c.mask.b[2], c.mask.b[3], c.collMethod, c.nS, w.nSg);
    return w.nCls++;
}
static int classFor(World& w, Fluid* f) { return classFor(w, f->group, f->mask, f->collMethod, 0); }

// ---------------------------------------------------------------------------
// world building (game thread, scene not simulating)
// ---------------------------------------------------------------------------
static void readFilter(World& w) {
    void* sc = w.scene;
    for (int a = 0; a < 32; ++a) for (int b = 0; b < 32; ++b)
        w.groupPair[a][b] = vfn<FN_groupFlag>(sc, SC_getGroupCollisionFlag)(sc, 0, (unsigned short)a, (unsigned short)b);
    vfn<FN_filterOps>(sc, SC_getFilterOps)(sc, 0, &w.op0, &w.op1, &w.op2);
    w.fb = vfn<FN_bool>(sc, SC_getFilterBool)(sc, 0);
    vfn<FN_retMask>(sc, SC_getFilterConstant0)(sc, 0, &w.k0);
    vfn<FN_retMask>(sc, SC_getFilterConstant1)(sc, 0, &w.k1);
}

static void rebuildStatic(World& w) {
    LONGLONG t0 = qpc();
    void* sc = w.scene;
    NxU32 nActors = vfn<FN_u32>(sc, SC_getNbActors)(sc, 0);
    void** actors = (void**)vfn<FN_ptr>(sc, SC_getActors)(sc, 0);
    readFilter(w);
    if (w.transposed < 0 && actors) detectOrientation(w, actors, nActors);
    w.nSg = 0; w.nSp = 0;
    memset(w.skippedTypes, 0, sizeof(w.skippedTypes));
    int meshes = 0;
    for (NxU32 a = 0; a < nActors && actors; ++a) {
        void* act = actors[a];
        if (!act || vfn<FN_bool>(act, AC_isDynamic)(act, 0)) continue;
        g_curActorFlags = actorFlagsOf(act);
        NxU32 ns = vfn<FN_u32>(act, AC_getNbShapes)(act, 0);
        void** shapes = (void**)vfn<FN_ptr>(act, AC_getShapes)(act, 0);
        for (NxU32 s = 0; s < ns && shapes; ++s) {
            void* sh = shapes[s];
            if (!sh) continue;
            if (vfn<FN_u32>(sh, SH_getType)(sh, 0) == ST_MESH) { addTriangleMesh(w, sh); ++meshes; continue; }
            grow(w.sg, w.capSg, w.nSg + 1);
            int hull;
            if (buildGeom(w, sh, w.sg[w.nSg], false, w.sp, w.nSp, w.capSp, hull)) { w.sg[w.nSg].shape = 0; ++w.nSg; }
        }
    }
    g_curActorFlags = 0;
    int noFluidActor = 0; for (int i = 0; i < w.nSg; ++i) if (w.sg[i].dbgActorFlags & 9) ++noFluidActor;
    for (int i = 0; i < w.nCls; ++i) rebuildClassStatic(w, w.cls[i]);
    w.staticDirty = false;
    w.dynDirty = true;
    LOG("FLUIDREP static world rebuilt: %u actors -> %d geoms (%d convex planes, %d tri meshes; %d geoms on actors fluids pass through), skipped types plane %d heightfield %d wheel %d, %.2f ms",
        nActors, w.nSg, w.nSp, meshes, noFluidActor, w.skippedTypes[ST_PLANE], w.skippedTypes[ST_HEIGHTFIELD], w.skippedTypes[ST_WHEEL],
        (qpc() - t0) * g_msPerTick);
    for (int i = 0; i < w.nCls; ++i) LOG("FLUIDREP   class %d: %d static geoms collide", i, w.cls[i].nS);
}

static bool anyClassNeedsDynamic(World& w) {
    for (int i = 0; i < w.nCls; ++i) if (w.cls[i].collMethod & 2) return true;
    return false;
}

// Dynamic shapes: the shape list is re-enumerated when actors change; poses are refreshed every step.
static void refreshDynamic(World& w) {
    if (!anyClassNeedsDynamic(w)) { w.nDg = 0; for (int i = 0; i < w.nCls; ++i) w.cls[i].nD = 0; return; }
    int tr = w.transposed < 0 ? 0 : w.transposed;
    // Validate cached dynamic shapes: their actor's shape list must be unchanged (actors themselves are
    // covered by the actor-set signature and the create/releaseActor hooks).
    if (!w.dynDirty) {
        void* lastActor = 0;
        for (int i = 0; i < w.nDg && !w.dynDirty; ++i) {
            Geom& g = w.dg[i];
            if (g.actor == lastActor) continue;
            lastActor = g.actor;
            if (vfn<FN_u32>(g.actor, AC_getNbShapes)(g.actor, 0) != g.actorNs ||
                vfn<FN_ptr>(g.actor, AC_getShapes)(g.actor, 0) != g.actorShapes) w.dynDirty = true;
        }
    }
    if (w.dynDirty) {
        void* sc = w.scene;
        NxU32 nActors = vfn<FN_u32>(sc, SC_getNbActors)(sc, 0);
        void** actors = (void**)vfn<FN_ptr>(sc, SC_getActors)(sc, 0);
        w.nDg = 0; w.nDp = 0;
        for (NxU32 a = 0; a < nActors && actors; ++a) {
            void* act = actors[a];
            if (!act || !vfn<FN_bool>(act, AC_isDynamic)(act, 0)) continue;
            g_curActorFlags = actorFlagsOf(act);
            NxU32 ns = vfn<FN_u32>(act, AC_getNbShapes)(act, 0);
            void** shapes = (void**)vfn<FN_ptr>(act, AC_getShapes)(act, 0);
            for (NxU32 s = 0; s < ns && shapes; ++s) {
                void* sh = shapes[s];
                if (!sh || vfn<FN_u32>(sh, SH_getType)(sh, 0) == ST_MESH) continue;
                grow(w.dg, w.capDg, w.nDg + 1);
                int hull;
                if (!buildGeom(w, sh, w.dg[w.nDg], true, w.dp, w.nDp, w.capDp, hull)) continue;
                w.dg[w.nDg].actor = act; w.dg[w.nDg].actorShapes = shapes; w.dg[w.nDg].actorNs = ns;
                // keep only shapes some class can collide with
                bool keep = false;
                for (int c = 0; c < w.nCls && !keep; ++c) keep = geomPasses(w, w.cls[c], w.dg[w.nDg]);
                if (keep) ++w.nDg;
            }
        }
        g_curActorFlags = 0;
        w.dynDirty = false;
    } else {
        // refresh poses in place
        w.nDp = 0;
        for (int i = 0; i < w.nDg; ++i) {
            Geom& g = w.dg[i];
            void* sh = g.shape;
            float pose[12]; vfn<FN_retPose>(sh, SH_getGlobalPose)(sh, 0, pose);
            V3 c = v3(pose[9], pose[10], pose[11]);
            switch (g.type) {
            case GT_BOX:
                g.c = g.localPlanes == -1 ? poseApply(pose, g.b, tr) : c;     // -1: convex approximated by a box
                for (int k = 0; k < 3; ++k) g.ax[k] = poseAxis(pose, k, tr);
                boxAabb(g); break;
            case GT_SPHERE: g.c = c; g.aabb.mn = c - v3(g.r, g.r, g.r); g.aabb.mx = c + v3(g.r, g.r, g.r); break;
            case GT_CAPSULE: {
                float h = len(g.b - g.c) * 0.5f;
                V3 ay = poseAxis(pose, 1, tr);
                g.c = c - ay * h; g.b = c + ay * h;
                bEmpty(g.aabb); bAdd(g.aabb, g.c); bAdd(g.aabb, g.b);
                g.aabb.mn = g.aabb.mn - v3(g.r, g.r, g.r); g.aabb.mx = g.aabb.mx + v3(g.r, g.r, g.r);
                break;
            }
            case GT_CONVEX:
                hullToWorld(g.localPlanes, pose, tr, w.dp, w.nDp, w.capDp, g);
                vfn<FN_bounds>(sh, SH_getWorldBounds)(sh, 0, &g.aabb);
                break;
            }
        }
    }
    for (int c = 0; c < w.nCls; ++c) {
        FilterClass& fc = w.cls[c];
        fc.nD = 0;
        grow(fc.dIdx, fc.capD, w.nDg);
        for (int i = 0; i < w.nDg; ++i) if (geomPasses(w, fc, w.dg[i])) fc.dIdx[fc.nD++] = i;
        bvhBuild(fc.dBvh, w.dg, fc.dIdx, fc.nD);
    }
}

// ---------------------------------------------------------------------------
// particle step (worker threads)
// ---------------------------------------------------------------------------
struct QueryCtx { const World* w; const Geom* geoms; const Plane4* planes; V3 p0, D; float r; Hit best; int bestGeom; bool bestDyn; bool dynSet; int tests; float maxInside; void* const* skip; int nSkip; };   // skip: moving shapes to ignore (by shape pointer)

static void queryBvh(const BVH& b, const B3& box, QueryCtx& q, bool dyn) {
    if (!b.nNodes) return;
    int stack[64]; int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const BNode& nd = b.nodes[stack[--sp]];
        if (!bOverlap(nd.b, box)) continue;
        if (nd.count > 0) {
            for (int i = nd.left; i < nd.left + nd.count; ++i) {
                int gi = b.items[i];
                const Geom& g = q.geoms[gi];
                if (!bOverlap(g.aabb, box)) continue;
                if (q.nSkip) { bool sk = false; for (int k = 0; k < q.nSkip; ++k) if (g.shape == q.skip[k]) sk = true; if (sk) continue; }
                ++q.tests;
                Hit h; bool hit = false;
                switch (g.type) {
                case GT_BOX: hit = hitBox(g, q.p0, q.D, q.r, h); break;
                case GT_CONVEX: hit = hitConvex(q.planes, g, q.p0, q.D, q.r, h); break;
                case GT_SPHERE: hit = hitSphereAt(g.c, g.r + q.r, q.p0, q.D, h); break;
                case GT_CAPSULE: hit = hitCapsule(g, q.p0, q.D, q.r, h); break;
                case GT_TRI: hit = hitTri(g, q.p0, q.D, q.r, h); break;
                }
                if (!hit || (h.inside && h.depth > q.maxInside)) continue;
                // prefer deepest penetration, otherwise earliest hit
                bool better = q.bestGeom < 0 ||
                              (h.inside && (!q.best.inside || h.depth > q.best.depth)) ||
                              (!h.inside && !q.best.inside && h.t < q.best.t);
                if (better) { q.best = h; q.bestGeom = gi; q.bestDyn = dyn; }
            }
        } else if (sp < 62) {
            stack[sp++] = nd.left;
            stack[sp++] = -nd.count;
        }
    }
}

// Is point p (sphere of radius r) inside any collidable geom other than 'skip'? Used to choose a push-out
// direction that doesn't move a particle from one solid into an overlapping one (e.g. a crate sunk into the floor).
static bool geomContains(const Geom& g, const Plane4* planes, V3 p, float r) {
    Hit h; V3 zero = v3(0, 0, 0);
    switch (g.type) {
    case GT_BOX: return hitBox(g, p, zero, r, h) && h.inside;
    case GT_CONVEX: return hitConvex(planes, g, p, zero, r, h) && h.inside;
    case GT_SPHERE: return hitSphereAt(g.c, g.r + r, p, zero, h) && h.inside;
    case GT_CAPSULE: return hitCapsule(g, p, zero, r, h) && h.inside;
    default: return false;      // thin triangles never "contain" a point
    }
}
static bool insideAnyBvh(const BVH& b, const Geom* geoms, const Plane4* planes, V3 p, float r, const Geom* skip) {
    if (!b.nNodes) return false;
    B3 box; box.mn = p - v3(r, r, r); box.mx = p + v3(r, r, r);
    int stack[64]; int sp = 0; stack[sp++] = 0;
    while (sp) {
        const BNode& nd = b.nodes[stack[--sp]];
        if (!bOverlap(nd.b, box)) continue;
        if (nd.count > 0) {
            for (int i = nd.left; i < nd.left + nd.count; ++i) {
                const Geom& g = geoms[b.items[i]];
                if (&g == skip || !bOverlap(g.aabb, box)) continue;
                if (geomContains(g, planes, p, r)) return true;
            }
        } else if (sp < 62) { stack[sp++] = nd.left; stack[sp++] = -nd.count; }
    }
    return false;
}
static bool insideAny(const World& w, const FilterClass& fc, V3 p, float r, const Geom* skip) {
    return insideAnyBvh(fc.sBvh, w.sg, w.sp, p, r, skip) || (fc.nD && insideAnyBvh(fc.dBvh, w.dg, w.dp, p, r, skip));
}

// Particle starts inside geom g: pick the shallowest face whose exit point is free; fall back to the shallowest.
static void chooseExit(const World& w, const FilterClass& fc, const Geom& g, const Plane4* planes, V3 p, float r, Hit& h) {
    const int MAXF = 32;
    V3 fn[MAXF]; float fd[MAXF]; int nf = 0;
    if (g.type == GT_BOX) {
        V3 d0 = p - g.c;
        float lp[3] = { dot(d0, g.ax[0]), dot(d0, g.ax[1]), dot(d0, g.ax[2]) };
        float E[3] = { g.e.x + r, g.e.y + r, g.e.z + r };
        for (int i = 0; i < 3; ++i) {
            fn[nf] = g.ax[i];        fd[nf++] = E[i] - lp[i];
            fn[nf] = g.ax[i] * -1.0f; fd[nf++] = E[i] + lp[i];
        }
    } else if (g.type == GT_CONVEX) {
        for (int i = g.p0; i < g.p0 + g.pc && nf < MAXF; ++i) { fn[nf] = planes[i].n; fd[nf++] = planes[i].d + r - dot(planes[i].n, p); }
    } else {
        return;   // spheres/capsules: radial push-out is the only sensible exit
    }
    // selection order by depth (small n: simple selection sort)
    for (int a = 0; a < nf; ++a) {
        int m = a;
        for (int b = a + 1; b < nf; ++b) if (fd[b] < fd[m]) m = b;
        V3 tn = fn[a]; fn[a] = fn[m]; fn[m] = tn; float td = fd[a]; fd[a] = fd[m]; fd[m] = td;
        if (fd[a] < 0) continue;
        V3 dest = p + fn[a] * (fd[a] + 1e-4f);
        if (!insideAny(w, fc, dest, r, &g)) { h.n = fn[a]; h.depth = fd[a]; return; }
    }
    // every exit blocked: keep the shallowest (h unchanged)
}

static void respond(V3& v, V3 n, float rest, float dynF, float statF, float restThresh) {
    float vn = dot(v, n);
    if (vn >= 0) return;
    if (-vn < restThresh) rest = 0;      // resting contact: no micro-bounces
    V3 vnv = n * vn, vt = v - vnv;
    float vtl = len(vt);
    if (vtl <= statF * -vn) vt = v3(0, 0, 0);
    else vt = vt * (1.0f - dynF);
    v = vt - vnv * rest;
}

// ---- SPH (Mueller et al. 2003 kernels; density normalised to the fluid's rest spacing) ----
static inline int ffloor(float x) { int i = (int)x; return (x < (float)i) ? i - 1 : i; }
static inline NxU32 cellHash(int x, int y, int z) { return ((NxU32)x * 73856093u) ^ ((NxU32)y * 19349663u) ^ ((NxU32)z * 83492791u); }

// The 27 neighbour buckets of the cell containing p, without duplicates (hash collisions would double count).
static int neighbourBuckets(const Fluid* f, V3 p, NxU32* out) {
    int cx = ffloor(p.x * f->kInvH), cy = ffloor(p.y * f->kInvH), cz = ffloor(p.z * f->kInvH), n = 0;
    for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) for (int dz = -1; dz <= 1; ++dz) {
        NxU32 b = cellHash(cx + dx, cy + dy, cz + dz) & f->gridMask;
        bool dup = false;
        for (int k = 0; k < n; ++k) if (out[k] == b) { dup = true; break; }
        if (!dup) out[n++] = b;
    }
    return n;
}

static V3 sphAccel(Fluid* f, NxU32 idx, float& rhoOut) {
    V3 pi = f->snapP[idx], vi = f->snapV[idx];
    NxU32 buckets[27];
    int nb = neighbourBuckets(f, pi, buckets);
    float h = f->kH, h2 = f->kH2;
    float rho = 0;
    for (int b = 0; b < nb; ++b)
        for (int j = f->gridHead[buckets[b]]; j >= 0; j = f->gridNext[j]) {
            V3 d = pi - f->snapP[j];
            float r2 = dot(d, d);
            if (r2 < h2) { float w = h2 - r2; rho += w * w * w; }
        }
    rho *= f->kPoly6 * f->kMass;
    if (rho < 1e-6f) rho = f->sRho0;
    rhoOut = rho;
    float pres = f->sStiff * (rho - f->sRho0); if (pres < 0) pres = 0;   // no attraction: stable, gas-like
    V3 fp = v3(0, 0, 0), fv = v3(0, 0, 0);
    for (int b = 0; b < nb; ++b)
        for (int j = f->gridHead[buckets[b]]; j >= 0; j = f->gridNext[j]) {
            if ((NxU32)j == idx) continue;
            V3 d = pi - f->snapP[j];
            float r2 = dot(d, d);
            if (r2 >= h2 || r2 < 1e-12f) continue;
            float r = fsqrt(r2), hr = h - r;
            float rhoj = f->snapRho[j];
            float pj = f->sStiff * (rhoj - f->sRho0); if (pj < 0) pj = 0;
            fp = fp + d * (f->kMass * (pres + pj) / (2.0f * rhoj) * f->kSpiky * hr * hr / r);
            fv = fv + (f->snapV[j] - vi) * (f->kMass / rhoj * f->kVisc * hr);
        }
    return (fp + fv * f->sVisc) * (1.0f / rho);
}

static void stepParticle(Fluid* f, NxU32 idx, int& tests) {
    Particle& p = f->parts[idx];
    const World& w = *f->sWorld;
    const FilterClass* fc = (f->cls >= 0 && f->cls < w.nCls) ? &w.cls[f->cls] : 0;
    float h = f->sH;
    p.flags = 0;
    p.n = v3(0, 0, 0);
    V3 aSph = v3(0, 0, 0);
    if (f->sph && f->sSub) {
        float rho;
        aSph = sphAccel(f, idx, rho);
        p.rho = rho;
        float aMax = f->sMotionLimit / (h * h), al = len(aSph);   // never more than one motion limit per substep
        if (al > aMax) aSph = aSph * (aMax / al);
    }
    for (int s = 0; s < f->sSub; ++s) {
        if (p.life > 0) { p.life -= h; if (p.life <= 0) { p.dead = 1; return; } }
        p.v = p.v + (f->sAccel + aSph) * h;
        float damp = f->sDamping * h; if (damp > 1) damp = 1;
        p.v = p.v * (1.0f - damp);
        V3 d = p.v * h;
        float dl = len(d);
        if (dl > f->sMotionLimit && dl > 0) { d = d * (f->sMotionLimit / dl); p.v = d * (1.0f / h); p.flags |= FP_MOTION_LIMIT; }
        V3 p0 = p.p, p1 = p.p + d;
        if (fc) {
            for (int it = 0; it < 3; ++it) {
                QueryCtx q;
                q.w = &w; q.p0 = p0; q.D = p1 - p0; q.r = f->sRadius; q.bestGeom = -1; q.tests = 0; q.maxInside = BIG; q.skip = 0; q.nSkip = 0;
                B3 box; bEmpty(box); bAdd(box, p0); bAdd(box, p1);
                box.mn = box.mn - v3(q.r, q.r, q.r); box.mx = box.mx + v3(q.r, q.r, q.r);
                q.geoms = w.sg; q.planes = w.sp;
                queryBvh(fc->sBvh, box, q, false);
                if (fc->nD) {
                    QueryCtx qd = q; qd.geoms = w.dg; qd.planes = w.dp; qd.tests = 0; qd.bestGeom = -1;
                    queryBvh(fc->dBvh, box, qd, true);
                    q.tests += qd.tests;
                    if (qd.bestGeom >= 0 && (q.bestGeom < 0 ||
                        (qd.best.inside && (!q.best.inside || qd.best.depth > q.best.depth)) ||
                        (!qd.best.inside && !q.best.inside && qd.best.t < q.best.t))) { q.best = qd.best; q.bestGeom = qd.bestGeom; q.bestDyn = true; }
                }
                tests += q.tests;
                if (q.bestGeom < 0) break;
                const Geom& g = q.bestDyn ? w.dg[q.bestGeom] : w.sg[q.bestGeom];
                if (g.drain) { p.dead = 2; return; }
                if (q.best.inside) chooseExit(w, *fc, g, q.bestDyn ? w.dp : w.sp, q.p0, q.r, q.best);
                V3 n = q.best.n;
                p.flags |= q.bestDyn ? FP_COLL_DYNAMIC : FP_COLL_STATIC;
                p.lastGeom = q.bestDyn ? -(q.bestGeom + 2) : q.bestGeom;
                p.n = n;
                if (q.bestDyn) respond(p.v, n, f->sRestD, f->sDynFD, f->sStatFD, f->sRestThresh);
                else           respond(p.v, n, f->sRestS, f->sDynFS, f->sStatFS, f->sRestThresh);
                const float eps = 1e-4f;
                if (q.best.inside) {
                    p0 = p0 + n * (q.best.depth + eps);
                    V3 rem = p1 - q.p0;
                    float rn = dot(rem, n); if (rn < 0) rem = rem - n * rn;
                    p1 = p0 + rem;
                } else {
                    V3 hp = q.p0 + q.D * q.best.t + n * eps;
                    float remT = 1.0f - q.best.t;
                    p0 = hp;
                    p1 = hp + p.v * (h * remT);
                }
            }
        }
        p.p = p1;
        if (f->sFlags & FF_PROJECT_TO_PLANE) {
            float dist = dot(f->sPlane.n, p.p) + f->sPlane.d;
            p.p = p.p - f->sPlane.n * dist;
            p.v = p.v - f->sPlane.n * dot(p.v, f->sPlane.n);
        }
    }
}

#include "cloth.inc"

// ---------------------------------------------------------------------------
// thread pool
// ---------------------------------------------------------------------------
struct Chunk { Fluid* f; NxU32 start, count; ClothR* cloth; };
static const int MAX_CHUNKS = 8192;
static Chunk g_chunks[MAX_CHUNKS];
static volatile LONG g_jobNext = 0, g_jobDone = 0, g_jobTotal = 0, g_jobTests = 0;
static volatile LONG g_jobTicks = 0;
static HANDLE g_sem = 0, g_doneEvt = 0;
static HANDLE g_threads[16]; static int g_nThreads = 0;
static void* g_jobScene = 0;
static bool  g_jobInFlight = false;

// Chunk claims use compare-and-swap so g_jobNext never runs past g_jobTotal: a worker waking late from the
// previous job can then only ever claim a chunk that is genuinely next in the current job.
static void runChunks() {
    for (;;) {
        LONG i = g_jobNext;
        if (i >= g_jobTotal) break;
        if (InterlockedCompareExchange(&g_jobNext, i + 1, i) != i) continue;
        LONGLONG t0 = qpc();
        Chunk& c = g_chunks[i];
        int tests = 0;
        if (c.cloth) clothStep(c.cloth, tests);
        else for (NxU32 k = c.start; k < c.start + c.count; ++k) stepParticle(c.f, k, tests);
        InterlockedExchangeAdd(&g_jobTests, tests);
        InterlockedExchangeAdd(&g_jobTicks, (LONG)(qpc() - t0));
        if (InterlockedIncrement(&g_jobDone) == g_jobTotal) SetEvent(g_doneEvt);
    }
}

static DWORD WINAPI workerMain(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_sem, INFINITE);
        runChunks();
    }
}

static void poolInit() {
    if (g_sem) return;
    SYSTEM_INFO si; GetSystemInfo(&si);
    // Default: half the logical CPUs, at most 8. The fluid step is short (a few ms of CPU per frame in the
    // worst scenes), so more threads add little and compete with the game, the driver and capture/streaming.
    int n = g_threadsWanted > 0 ? g_threadsWanted : (int)si.dwNumberOfProcessors / 2;
    if (n < 1) n = 1;
    if (n > (g_threadsWanted > 0 ? 16 : 8)) n = g_threadsWanted > 0 ? 16 : 8;
    g_sem = CreateSemaphoreW(NULL, 0, 1 << 20, NULL);
    g_doneEvt = CreateEventW(NULL, TRUE, TRUE, NULL);
    for (int i = 0; i < n; ++i) {
        g_threads[i] = CreateThread(NULL, 64 * 1024, workerMain, NULL, 0, NULL);
        if (g_threads[i]) ++g_nThreads;              // normal priority
    }
    LOG("Fluid thread pool: %d workers (%lu logical CPUs)", g_nThreads, si.dwNumberOfProcessors);
}

static void jobWait() {
    if (!g_jobInFlight) return;
    runChunks();                                   // the waiting thread helps
    WaitForSingleObject(g_doneEvt, INFINITE);
    g_jobInFlight = false;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
static FrameStats g_stats;
static LONGLONG g_prepTicks = 0;

void init(LogFn log, bool enable, int threads, bool replaceSph) {
    g_log = log;
    g_enabled = enable ? 1 : 0;
    g_threadsWanted = threads;
    g_replaceSph = replaceSph;
    LARGE_INTEGER fr; QueryPerformanceFrequency(&fr); g_msPerTick = 1000.0 / (double)fr.QuadPart;
    LOG("Fluid replacement %s", enable ? "enabled" : "disabled (PhysX simulates fluids)");
}

bool enabled() { return g_enabled == 1; }

static bool readable(const void* p, SIZE_T n) {
    if (!p) return false;
    unsigned char tmp[64]; SIZE_T got = 0;
    if (n > sizeof(tmp)) n = sizeof(tmp);
    return ReadProcessMemory(GetCurrentProcess(), p, tmp, n, &got) && got == n;
}

void* tryCreate(void* scene, const void* descv) {
    const unsigned char* d = (const unsigned char*)descv;
    if (!d) return 0;
    NxU32 maxP, sim, psm, flags;
    memcpy(&maxP, d + 0x40, 4); memcpy(&sim, d + 0xB4, 4); memcpy(&psm, d + 0x5C, 4); memcpy(&flags, d + 0x13C, 4);
    float rppm; memcpy(&rppm, d + 0x48, 4);
    if (sim != 2 && !((sim == 1 || sim == 4) && g_replaceSph)) { LOG("FLUIDREP fluid with simulationMethod %u -> left to PhysX", sim); return 0; }
    if (maxP == 0 || maxP > 1000000 || rppm <= 0 || psm < 4 || (psm & (psm - 1)) || (flags & ~0xFFu)) {
        LOG("FLUIDREP descriptor failed sanity check (max %u rppm %f psm %u flags 0x%x) -> left to PhysX", maxP, rppm, psm, flags);
        return 0;
    }
    PData w; memcpy(&w, d + 0xD4, sizeof(w));
    PIdData dl, cr; memcpy(&dl, d + 0x114, sizeof(dl)); memcpy(&cr, d + 0x124, sizeof(cr));
    if ((w.num && !readable(w.num, 4)) || (w.pos && !readable(w.pos, 4)) || (dl.num && !readable(dl.num, 4)) || (cr.num && !readable(cr.num, 4))) {
        LOG("FLUIDREP write/id buffers not readable -> left to PhysX");
        return 0;
    }
    if (g_nFluids >= 64) return 0;
    poolInit();

    Fluid* f = (Fluid*)zalloc(sizeof(Fluid));
    f->vtbl = g_vtbl; f->magic = MAGIC; f->scene = scene;
    InitializeCriticalSection(&f->cs);
    f->maxParticles = maxP; f->curLimit = maxP; f->psm = psm; f->simMethod = sim; f->flags = flags;
    memcpy(&f->numReserve, d + 0x44, 4); f->rppm = rppm; memcpy(&f->restDensity, d + 0x4C, 4);
    memcpy(&f->krm, d + 0x50, 4); memcpy(&f->mlMul, d + 0x54, 4); memcpy(&f->cdMul, d + 0x58, 4);
    memcpy(&f->stiffness, d + 0x60, 4); memcpy(&f->viscosity, d + 0x64, 4); memcpy(&f->surfaceTension, d + 0x68, 4);
    memcpy(&f->damping, d + 0x6C, 4); memcpy(&f->fadeIn, d + 0x70, 4); memcpy(&f->extAcc, d + 0x74, 12);
    memcpy(&f->projPlane, d + 0x80, 16);
    memcpy(&f->restS, d + 0x90, 4); memcpy(&f->dynFS, d + 0x94, 4); memcpy(&f->statFS, d + 0x98, 4); memcpy(&f->attrS, d + 0x9C, 4);
    memcpy(&f->restD, d + 0xA0, 4); memcpy(&f->dynFD, d + 0xA4, 4); memcpy(&f->statFD, d + 0xA8, 4); memcpy(&f->attrD, d + 0xAC, 4);
    memcpy(&f->collResp, d + 0xB0, 4); memcpy(&f->collMethod, d + 0xB8, 4);
    unsigned short grp; memcpy(&grp, d + 0xBC, 2); f->group = grp & 31;
    memcpy(&f->mask, d + 0xC0, 16); memcpy(&f->ffMaterial, d + 0xD0, 2);
    f->write = w; f->delW = dl; f->creW = cr; memcpy(f->packetData, d + 0x134, 8);
    memcpy(&f->userData, d + 0x140, 4); memcpy(&f->name, d + 0x144, 4); memcpy(&f->compartment, d + 0x148, 4);

    f->parts = allocN<Particle>(maxP); f->pend = allocN<Particle>(maxP);
    f->sph = (sim == 1 || sim == 4);
    if (f->sph) {
        f->snapP = allocN<V3>(maxP); f->snapV = allocN<V3>(maxP); f->snapRho = allocN<float>(maxP); f->gridNext = allocN<int>(maxP);
        NxU32 hs = 1024; while (hs < maxP * 2) hs <<= 1;
        f->gridHead = allocN<int>(hs); f->gridMask = hs - 1;
        f->kH = f->krm / rppm; f->kH2 = f->kH * f->kH; f->kInvH = 1.0f / f->kH;
        const float PI = 3.14159265f;
        float h3 = f->kH2 * f->kH, h6 = h3 * h3, h9 = h6 * h3;
        f->kPoly6 = 315.0f / (64.0f * PI * h9);
        f->kSpiky = 45.0f / (PI * h6);
        f->kVisc = 45.0f / (PI * h6);
        f->kMass = f->restDensity / (rppm * rppm * rppm);
        // normalise: a cubic lattice at the rest spacing must give exactly the rest density (zero pressure)
        float a = 1.0f / rppm, sum = 0; int R = (int)(f->kH / a) + 1;
        for (int x = -R; x <= R; ++x) for (int y = -R; y <= R; ++y) for (int z = -R; z <= R; ++z) {
            float r2 = (x * x + y * y + z * z) * a * a;
            if (r2 < f->kH2) { float ww = f->kH2 - r2; sum += ww * ww * ww; }
        }
        float lattice = sum * f->kPoly6 * f->kMass;
        if (lattice > 0) f->kPoly6 *= f->restDensity / lattice;
    }
    f->freeIds = allocN<NxU32>(maxP); f->idToIdx = allocN<int>(maxP);
    f->delReport = allocN<NxU32>(maxP); f->pendDel = allocN<NxU32>(maxP);
    for (NxU32 i = 0; i < maxP; ++i) { f->freeIds[i] = maxP - 1 - i; f->idToIdx[i] = -1; }
    f->nFree = maxP;
    bEmpty(f->bounds);
    f->classDirty = true; f->cls = -1;
    f->index = g_nFluids;
    g_fluids[g_nFluids++] = f;

    // initial particles
    PData init; memcpy(&init, d, sizeof(init));
    if (init.num && readable(init.num, 4) && *init.num) s05_addParticles(f, 0, &init, false);

    World* wld = worldFor(scene, true);
    if (wld) wld->staticDirty = wld->staticDirty || wld->nSg == 0;

    LOG("FLUIDREP created fluid %p (scene %p): max %u, collisionDistance %.4f, motionLimit %.4f, damping %.3f, extAcc (%.2f %.2f %.2f), "
        "restitution S %.2f D %.2f, friction S %.2f/%.2f D %.2f/%.2f, method %u, flags 0x%x, group %u mask %08x %08x %08x %08x",
        f, scene, maxP, f->cdMul / rppm, f->mlMul / rppm, f->damping, f->extAcc.x, f->extAcc.y, f->extAcc.z,
        f->restS, f->restD, f->dynFS, f->statFS, f->dynFD, f->statFD, f->collMethod, f->flags, f->group,
        f->mask.b[0], f->mask.b[1], f->mask.b[2], f->mask.b[3]);
    if (f->name && readable(f->name, 1)) LOG("FLUIDREP   name \"%s\"", f->name);
    { static int dumped = 0; if (dumped < 12) { ++dumped; for (int o = 0; o < 0x150; o += 32) { const NxU32* q = (const NxU32*)(d + o);
        LOG("FLUIDREP   desc +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", o, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]); } } }
    if (f->sph) LOG("FLUIDREP   SPH fluid: kernel radius %.4f, stiffness %.2f, viscosity %.2f, particle mass %.4f", f->kH, f->stiffness, f->viscosity, f->kMass);
    if (f->flags & FF_COLLISION_TWOWAY) LOG("FLUIDREP   note: two-way collision requested; not simulated (particles don't push rigid bodies)");
    if (f->packetData[0] && f->packetData[1])
        LOG("FLUIDREP   packet output on: buffer %08x, count at %08x, packet edge %.2f m (packetSizeMultiplier %u x kernel radius)",
            f->packetData[0], f->packetData[1], f->rppm > 0 ? (float)(f->psm ? f->psm : 16) * f->krm / f->rppm : 1.0f, f->psm);
    return f;
}

bool isOurs(void* fluid) {
    for (int i = 0; i < g_nFluids; ++i) if (g_fluids[i] == fluid) return true;
    return false;
}

static void destroyFluid(Fluid* f) {
    DeleteCriticalSection(&f->cs);
    zfree(f->parts); zfree(f->pend); zfree(f->freeIds); zfree(f->idToIdx); zfree(f->delReport); zfree(f->pendDel); zfree(f->pendForce); zfree(f->pkKey); zfree(f->pkIdx); zfree(f->pkTmp); zfree(f->pkKeyTmp);
    zfree(f->snapP); zfree(f->snapV); zfree(f->snapRho); zfree(f->gridHead); zfree(f->gridNext);
    f->magic = 0;
    zfree(f);
}

static void fluidRestingReport(Fluid* f) {
    const World* w = f->sWorld;
    if (!w || !f->n) return;
    int keys[16], cnt[16], nk = 0, resting = 0;
    for (NxU32 k = 0; k < f->n; ++k) {
        const Particle& p = f->parts[k];
        if (!(p.flags & (FP_COLL_STATIC | FP_COLL_DYNAMIC)) || dot(p.v, p.v) > 0.05f * 0.05f) continue;
        ++resting;
        int j = 0; while (j < nk && keys[j] != p.lastGeom) ++j;
        if (j == nk) { if (nk == 16) continue; keys[nk] = p.lastGeom; cnt[nk++] = 0; }
        ++cnt[j];
    }
    if (!resting) return;
    LOG("FLUIDREP   %d of %u particles resting on something:", resting, f->n);
    for (int r = 0; r < 3 && nk; ++r) {
        int b = 0; for (int j = 1; j < nk; ++j) if (cnt[j] > cnt[b]) b = j;
        int gi = keys[b]; bool dyn = gi < 0; int idx = dyn ? -gi - 2 : gi;
        const Geom* g = dyn ? (idx < w->nDg ? &w->dg[idx] : 0) : (idx < w->nSg ? &w->sg[idx] : 0);
        if (g) LOG("FLUIDREP     %d on %s geom %d: type %d, box (%.2f %.2f %.2f)-(%.2f %.2f %.2f), group %u, shape flags 0x%x, actor flags 0x%x",
                   cnt[b], dyn ? "dynamic" : "static", idx, g->type, g->aabb.mn.x, g->aabb.mn.y, g->aabb.mn.z, g->aabb.mx.x, g->aabb.mx.y, g->aabb.mx.z,
                   g->group, g->dbgShapeFlags, g->dbgActorFlags);
        cnt[b] = -1;
    }
}

static void fluidReport(Fluid* f, const char* when) {
    fluidRestingReport(f);
    LOG("FLUIDREP fluid %p %s: %s, now %u particles (peak %u); game asked to add %lu in %lu calls, %lu accepted; removed: %lu by the game "
        "(%lu removeAll), %lu lifetime, %lu drains; %lu forces; max speed %.1f m/s, longest lifetime asked %.1f s; %lu frames",
        f, when, f->name && readable(f->name, 1) ? f->name : "(unnamed)", f->n, f->dPeak, f->dReq, f->dAddCalls, f->dAdded,
        f->dGameDel, f->dRemoveAll, f->dLifeDel, f->dDrainDel, f->dUpdForces, fsqrt(f->dMaxSpeed), f->dMaxLife, f->dFrames);
    if (f->packetData[0] && f->packetData[1])
        LOG("FLUIDREP fluid %p packets: %.1f per frame on average, peak %u%s", f, f->dFrames ? (double)f->dPackets / f->dFrames : 0.0,
            f->dPacketsMax, f->dPacketsCapped ? " (packet limit reached: extra particles merged into the last packet)" : "");
    f->dPackets = f->dPacketsMax = f->dPacketsCapped = 0;
    f->dReq = f->dAdded = f->dAddCalls = f->dGameDel = f->dLifeDel = f->dDrainDel = f->dRemoveAll = f->dUpdForces = f->dFrames = 0;
    f->dMaxSpeed = 0; f->dMaxLife = 0; f->dPeak = 0;
}

void fluidPeriodic() {
    for (int i = 0; i < g_nFluids; ++i) if (g_fluids[i]->dFrames && (g_fluids[i]->dAddCalls || g_fluids[i]->n)) fluidReport(g_fluids[i], "last 600 frames");
}

void release(void* scene, void* fluid) {
    (void)scene;
    jobWait();
    for (int i = 0; i < g_nFluids; ++i) if (g_fluids[i] == fluid) {
        Fluid* f = g_fluids[i];
        g_fluids[i] = g_fluids[--g_nFluids];
        fluidReport(f, "at release");
        LOG("FLUIDREP released fluid %p (%u particles)%s", f, f->n, f->dSeen ? "" : " - never had a particle");
        destroyFluid(f);
        return;
    }
}

void releaseScene(void* scene) {
    jobWait();
    clothReleaseScene(scene);
    for (int i = 0; i < g_nFluids;) {
        if (g_fluids[i]->scene == scene) { Fluid* f = g_fluids[i]; g_fluids[i] = g_fluids[--g_nFluids]; fluidReport(f, "at scene release"); destroyFluid(f); }
        else ++i;
    }
    World* w = worldFor(scene, false);
    if (w) {
        zfree(w->sg); zfree(w->sp); zfree(w->dg); zfree(w->dp);
        for (int c = 0; c < w->nCls; ++c) {
            zfree(w->cls[c].sIdx); zfree(w->cls[c].dIdx);
            zfree(w->cls[c].sBvh.nodes); zfree(w->cls[c].sBvh.items); zfree(w->cls[c].dBvh.nodes); zfree(w->cls[c].dBvh.items);
        }
        memset(w, 0, sizeof(World));
    }
}

void onActorsChanged(void* scene, bool staticActor) {
    World* w = worldFor(scene, false);
    if (!w) return;
    w->dynDirty = true;                 // cached dynamic shape pointers may be about to dangle
    if (staticActor) w->staticDirty = true;
}

static void removeAt(Fluid* f, NxU32 idx) {
    NxU32 last = f->n - 1;
    if (idx != last) { f->parts[idx] = f->parts[last]; f->idToIdx[f->parts[idx].id] = (int)idx; }
    --f->n;
}

static void applyPending(Fluid* f) {
    EnterCriticalSection(&f->cs);
    if (f->pendRemoveAll) {
        ++f->dRemoveAll; f->dGameDel += f->n + f->nPend;
        for (NxU32 i = 0; i < f->n; ++i) { reportDelete(f, f->parts[i].id); f->idToIdx[f->parts[i].id] = -1; }
        for (NxU32 i = 0; i < f->nPend; ++i) reportDelete(f, f->pend[i].id);
        f->n = 0; f->nPend = 0; f->nPendDel = 0; f->nPendForce = 0; f->pendRemoveAll = false;
    }
    for (NxU32 k = 0; k < f->nPendDel; ++k) {
        NxU32 id = f->pendDel[k];
        int idx = f->idToIdx[id];
        if (idx >= 0) { ++f->dGameDel; reportDelete(f, id); f->idToIdx[id] = -1; removeAt(f, (NxU32)idx); continue; }
        for (NxU32 i = 0; i < f->nPend; ++i) if (f->pend[i].id == id) { reportDelete(f, id); f->pend[i] = f->pend[--f->nPend]; break; }
    }
    f->nPendDel = 0;
    for (NxU32 i = 0; i < f->nPend; ++i) {
        if (f->n >= f->maxParticles) { reportDelete(f, f->pend[i].id); continue; }
        f->parts[f->n] = f->pend[i];
        f->idToIdx[f->pend[i].id] = (int)f->n;
        ++f->n;
    }
    f->nPend = 0;
    float mass = f->restDensity / (f->rppm * f->rppm * f->rppm);
    for (int k = 0; k < f->nPendForce; ++k) {
        PendForce& pf = f->pendForce[k];
        int idx = pf.id < f->maxParticles ? f->idToIdx[pf.id] : -1;
        if (idx < 0) continue;
        V3 dv = pf.f;
        switch (pf.mode) {
            case FM_FORCE: case FM_SMOOTH_IMPULSE: dv = pf.f * (1.0f / (mass > 0 ? mass : 1)) * (1.0f / 60.0f); break;
            case FM_IMPULSE: dv = pf.f * (1.0f / (mass > 0 ? mass : 1)); break;
            case FM_ACCELERATION: dv = pf.f * (1.0f / 60.0f); break;
            default: break;
        }
        f->parts[idx].v = f->parts[idx].v + dv;
    }
    f->dUpdForces += f->nPendForce;
    f->nPendForce = 0;
    LeaveCriticalSection(&f->cs);
}

void beginStep(void* scene, float dt) {
    if (!g_nFluids && !g_nCloths) return;
    LONGLONG t0 = qpc();
    jobWait();          // a previous step that was never fetched
    bool any = false;
    for (int i = 0; i < g_nFluids; ++i) if (g_fluids[i]->scene == scene) { any = true; break; }
    if (!any && !clothAnyInScene(scene)) return;
    World* w = worldFor(scene, true);
    if (!w) return;

    InterlockedExchange(&g_jobTotal, 0);     // no worker may claim while chunks are rewritten
    // Apply queued adds/deletes first. With no live particle there is nothing to simulate and no reason to
    // touch the collision world: level streaming costs nothing until particles appear.
    NxU32 live = 0;
    for (int i = 0; i < g_nFluids; ++i) if (g_fluids[i]->scene == scene) { applyPending(g_fluids[i]); live += g_fluids[i]->n; }
    g_frameDt = dt > 0 ? (dt > 0.1f ? 0.1f : dt) : g_frameDt;
    int clothActive = clothPrepare(scene);
    int clothSleepers = clothWakeable(scene);
    if (!live && !clothActive && !clothSleepers) {
        g_jobTotal = 0; g_jobScene = scene; g_jobInFlight = false; SetEvent(g_doneEvt);
        g_prepTicks = qpc() - t0;
        g_stats.particles = 0; g_jobTicks = 0; g_jobTests = 0;
        return;
    }

    // actor set signature: rebuild if actors were added/removed without passing through our hooks
    NxU32 nActors = vfn<FN_u32>(scene, SC_getNbActors)(scene, 0);
    void** actors = (void**)vfn<FN_ptr>(scene, SC_getActors)(scene, 0);
    // Separate signatures: dynamic actors (ragdolls, debris) come and go constantly and must not force a
    // rebuild of the static geometry.
    NxU32 sSig = 2166136261u, dSig = 2166136261u, nStatic = 0;
    for (NxU32 a = 0; a < nActors && actors; ++a) {
        void* act = actors[a];
        if (!act) continue;
        if (vfn<FN_bool>(act, AC_isDynamic)(act, 0)) dSig = (dSig ^ (NxU32)(ULONG_PTR)act) * 16777619u;
        else { sSig = (sSig ^ (NxU32)(ULONG_PTR)act) * 16777619u; ++nStatic; }
    }
    if (sSig != w->lastActorSig || nStatic != w->lastActorCount) { w->staticDirty = true; w->lastActorSig = sSig; w->lastActorCount = nStatic; }
    if (dSig != w->lastDynSig) { w->dynDirty = true; w->lastDynSig = dSig; }
    if (w->staticDirty) rebuildStatic(*w);

    V3 gravity = v3(0, 0, 0);
    vfn<FN_vecOut>(scene, SC_getGravity)(scene, 0, &gravity);

    if (dt < 0) dt = 0;
    if (dt > 0.1f) dt = 0.1f;
    int sub = 1;
    while (dt / sub > (1.0f / 50.0f) && sub < 4) ++sub;

    int nChunks = 0, particles = 0;
    for (int i = 0; i < g_nFluids; ++i) {
        Fluid* f = g_fluids[i];
        if (f->scene != scene) continue;
        if (f->classDirty || f->cls < 0) { f->cls = classFor(*w, f); f->classDirty = false; }
        f->sWorld = w;
        f->sAccel = f->extAcc + ((f->flags & FF_DISABLE_GRAVITY) ? v3(0, 0, 0) : gravity);
        f->sDamping = f->damping; f->sMotionLimit = f->mlMul / f->rppm; f->sRadius = f->cdMul / f->rppm;
        f->sH = sub ? dt / sub : 0; f->sSub = (dt > 0 && (f->flags & FF_ENABLED)) ? sub : 0;
        f->sRestThresh = 2.0f * len(f->sAccel) * f->sH;   // ~2 steps of acceleration
        f->sFlags = f->flags; f->sCollMethod = f->collMethod; f->sPlane = f->projPlane;
        f->sRestS = f->restS; f->sDynFS = f->dynFS; f->sStatFS = f->statFS;
        f->sRestD = f->restD; f->sDynFD = f->dynFD; f->sStatFD = f->statFD;
        if (f->sph && f->sSub) {
            f->sStiff = f->stiffness; f->sVisc = f->viscosity; f->sRho0 = f->restDensity;
            memset(f->gridHead, 0xFF, sizeof(int) * (f->gridMask + 1));
            for (NxU32 k = 0; k < f->n; ++k) {
                const Particle& pk = f->parts[k];
                f->snapP[k] = pk.p; f->snapV[k] = pk.v; f->snapRho[k] = pk.rho > 0 ? pk.rho : f->restDensity;
                NxU32 b = cellHash(ffloor(pk.p.x * f->kInvH), ffloor(pk.p.y * f->kInvH), ffloor(pk.p.z * f->kInvH)) & f->gridMask;
                f->gridNext[k] = f->gridHead[b]; f->gridHead[b] = (int)k;
            }
        }
        particles += (int)f->n;
        if (!f->sSub) continue;
        for (NxU32 s = 0; s < f->n && nChunks < MAX_CHUNKS; s += 64) {
            g_chunks[nChunks].f = f; g_chunks[nChunks].start = s; g_chunks[nChunks].count = (f->n - s) < 64 ? (f->n - s) : 64; g_chunks[nChunks].cloth = 0;
            ++nChunks;
        }
    }
    // cloth collision classes before the dynamic shapes are gathered (only classes that exist collect them)
    for (int ci = 0; ci < g_nCloths; ++ci) {
        ClothR* c = g_cloths[ci];
        if (c->scene != scene || (c->flags & CLF_STATIC)) continue;
        if (c->classDirty || c->cls < 0) { c->cls = classFor(*w, c->group, c->mask, 3, 1); c->classDirty = false; }
    }
    refreshDynamic(*w);
    // A sleeping cloth wakes when a moving shape it collides with comes near it (PhysX wakes cloth on contact): without
    // this, running into a resting tarp did nothing.
    if (clothSleepers) clothWakeByContact(scene, *w);
    // cloths first: one job per cloth, the longest jobs start earliest
    int nFluidChunks = nChunks, nClothJobs = 0;
    for (int ci = 0; ci < g_nCloths; ++ci) if (g_cloths[ci]->scene == scene && g_cloths[ci]->sActive) ++nClothJobs;
    if (nClothJobs > MAX_CHUNKS - nFluidChunks) nClothJobs = MAX_CHUNKS - nFluidChunks;
    if (nClothJobs) for (int k = nFluidChunks - 1; k >= 0; --k) g_chunks[k + nClothJobs] = g_chunks[k];   // make room in front
    nChunks = 0;
    for (int ci = 0; ci < g_nCloths && nChunks < nClothJobs; ++ci) {
        ClothR* c = g_cloths[ci];
        if (c->scene != scene || !c->sActive) continue;
        c->sWorld = w;
        c->sAccel = c->extAcc + c->wind + ((c->flags & CLF_GRAVITY) ? gravity : v3(0, 0, 0));
        c->sH = sub ? dt / sub : 0; c->sSub = dt > 0 ? sub : 0;
        c->tears = 0;
        g_chunks[nChunks].f = 0; g_chunks[nChunks].start = 0; g_chunks[nChunks].count = 0; g_chunks[nChunks].cloth = c;
        ++nChunks;
    }
    if (nChunks < nClothJobs) for (int k = 0; k < nFluidChunks; ++k) g_chunks[nChunks + k] = g_chunks[nClothJobs + k];
    nChunks += nFluidChunks;

    // publish order: no claims possible while total is 0; counters reset; then the new total
    g_jobDone = 0; g_jobTests = 0; g_jobTicks = 0; g_jobScene = scene;
    ResetEvent(g_doneEvt);
    InterlockedExchange(&g_jobNext, 0);
    InterlockedExchange(&g_jobTotal, nChunks);
    if (nChunks) {
        g_jobInFlight = true;
        ReleaseSemaphore(g_sem, g_nThreads, NULL);
    } else {
        SetEvent(g_doneEvt);
    }
    g_prepTicks = qpc() - t0;
    g_stats.particles = particles;
    g_stats.staticGeoms = w->nSg; g_stats.dynamicGeoms = w->nDg;
}


// ---------------------------------------------------------------------------
// Fluid packets (1.1.14)
// PhysX 2.8 groups fluid particles into "packets": cubes with an edge of packetSizeMultiplier * kernelRadius. The
// write buffer is ordered packet by packet and, when the game supplies an NxFluidPacketData, PhysX fills it with
// one NxFluidPacket { NxBounds3 aabb; NxU32 firstParticleIndex, numParticles, packetID; } (36 bytes) per packet.
// Mirror's Edge draws its sprite fluids (steam, smoke, dust) by walking that packet list (it sorts the packets by
// distance and renders the particles each one points at). Up to 1.1.13 the list was never written, so the game
// walked a stale list: most particles were never drawn and the ones that were changed every frame (blinking
// "cotton balls"). The limit is NX_CONSTANT_FLUID_MAX_PACKETS, 924 in PhysXCore 2.8.0.7; the game sizes its
// buffer for it.
// ---------------------------------------------------------------------------
static const NxU32 kMaxPackets = 924;
struct FluidPacket { float mn[3], mx[3]; NxU32 first, num, id; };

static bool writableRange(void* p, size_t bytes) {
    MEMORY_BASIC_INFORMATION mbi;
    char* c = (char*)p;
    while (bytes) {
        if (!VirtualQuery(c, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        if (!(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        size_t left = (size_t)((char*)mbi.BaseAddress + mbi.RegionSize - c);
        if (left >= bytes) return true;
        bytes -= left; c += left;
    }
    return true;
}

// Fills f->pkIdx[0..n) with particle indices grouped by packet cell (stable LSD radix sort on a 48-bit cell key).
// Returns false when packets are not wanted (no packet buffer from the game) and the natural order should be used.
static bool packetOrder(Fluid* f, NxU32 n, float cell) {
    if ((NxU32)f->capPk < n) {
        zfree(f->pkKey); zfree(f->pkIdx); zfree(f->pkTmp); zfree(f->pkKeyTmp);
        int nc = 64; while ((NxU32)nc < n) nc *= 2;
        f->pkKey = allocN<unsigned long long>(nc); f->pkKeyTmp = allocN<unsigned long long>(nc);
        f->pkIdx = allocN<NxU32>(nc); f->pkTmp = allocN<NxU32>(nc);
        f->capPk = nc;
    }
    float inv = 1.0f / cell;
    for (NxU32 k = 0; k < n; ++k) {
        const V3& p = f->parts[k].p;
        float c[3] = { p.x * inv, p.y * inv, p.z * inv };
        unsigned long long key = 0;
        for (int a = 0; a < 3; ++a) {
            float v = c[a]; int iv;
            if (!(v == v)) iv = 0; else if (v > 32000.0f) iv = 32000; else if (v < -32000.0f) iv = -32000;
            else { iv = (int)v; if ((float)iv > v) --iv; }          // floor
            key = (key << 16) | (unsigned long long)(NxU32)(iv + 32768);
        }
        f->pkKey[k] = key; f->pkIdx[k] = k;
    }
    unsigned long long *ks = f->pkKey, *kd = f->pkKeyTmp; NxU32 *is = f->pkIdx, *id = f->pkTmp;
    for (int pass = 0; pass < 6; ++pass) {
        NxU32 cnt[257]; memset(cnt, 0, sizeof(cnt));
        int sh = pass * 8;
        for (NxU32 k = 0; k < n; ++k) ++cnt[((ks[k] >> sh) & 255) + 1];
        if (cnt[((ks[0] >> sh) & 255) + 1] == n) continue;           // every key has this byte equal: skip pass
        for (int b = 0; b < 256; ++b) cnt[b + 1] += cnt[b];
        for (NxU32 k = 0; k < n; ++k) { NxU32 d = cnt[(ks[k] >> sh) & 255]++; kd[d] = ks[k]; id[d] = is[k]; }
        unsigned long long* t = ks; ks = kd; kd = t; NxU32* ti = is; is = id; id = ti;
    }
    if (ks != f->pkKey) { memcpy(f->pkKey, ks, sizeof(*ks) * n); memcpy(f->pkIdx, is, sizeof(*is) * n); }
    return true;
}

// Writes the packet list for the sorted order in f->pkIdx/f->pkKey. Particles beyond the packet limit join the last packet.
static void writePackets(Fluid* f, NxU32 n) {
    FluidPacket* buf = (FluidPacket*)(ULONG_PTR)f->packetData[0];
    NxU32* numPtr = (NxU32*)(ULONG_PTR)f->packetData[1];
    if (!buf || !numPtr || !writableRange(numPtr, 4)) return;
    NxU32 np = 0, checked = 0;                                       // packets known to fit in the game's buffer
    for (NxU32 k = 0; k < n; ++k) {
        bool newCell = (k == 0) || f->pkKey[k] != f->pkKey[k - 1];
        if (newCell && np < kMaxPackets) {
            if (np == checked) {                                         // check the game's buffer 64 packets at a time
                NxU32 want = kMaxPackets - checked < 64 ? kMaxPackets - checked : 64;
                if (writableRange(buf + checked, sizeof(FluidPacket) * want)) checked += want;
            }
        }
        if (newCell && np < checked) {
            FluidPacket& pk = buf[np++];
            pk.first = k; pk.num = 0; pk.id = (NxU32)(f->pkKey[k] ^ (f->pkKey[k] >> 21) ^ (f->pkKey[k] >> 42));
            pk.mn[0] = pk.mn[1] = pk.mn[2] = BIG; pk.mx[0] = pk.mx[1] = pk.mx[2] = -BIG;
        } else if (newCell) {
            ++f->dPacketsCapped;                                         // over the limit: the last packet takes the rest
        }
        if (np == 0) { *numPtr = 0; return; }                            // game buffer not writable at all
        FluidPacket& pk = buf[np - 1];
        const V3& p = f->parts[f->pkIdx[k]].p;
        if (p.x < pk.mn[0]) pk.mn[0] = p.x;
        if (p.y < pk.mn[1]) pk.mn[1] = p.y;
        if (p.z < pk.mn[2]) pk.mn[2] = p.z;
        if (p.x > pk.mx[0]) pk.mx[0] = p.x;
        if (p.y > pk.mx[1]) pk.mx[1] = p.y;
        if (p.z > pk.mx[2]) pk.mx[2] = p.z;
        pk.num = k - pk.first + 1;
    }
    for (NxU32 i = 0; i < np; ++i)
        for (int a = 0; a < 3; ++a) { buf[i].mn[a] -= f->sRadius; buf[i].mx[a] += f->sRadius; }
    *numPtr = np;
    f->dPackets += np; if (np > f->dPacketsMax) f->dPacketsMax = np;
}

void endStep(void* scene) {
    bool any = false;
    for (int i = 0; i < g_nFluids; ++i) if (g_fluids[i]->scene == scene) { any = true; break; }
    bool anyCloth = clothAnyInScene(scene);
    if (!any && !anyCloth) return;
    LONGLONG t0 = qpc();
    if (g_jobScene == scene) jobWait();
    double waitMs = (qpc() - t0) * g_msPerTick;
    int fluids = 0, particles = 0;
    for (int i = 0; i < g_nFluids; ++i) {
        Fluid* f = g_fluids[i];
        if (f->scene != scene) continue;
        ++fluids;
        EnterCriticalSection(&f->cs);
        // drop dead particles (lifetime / drains)
        for (NxU32 k = 0; k < f->n;) {
            if (f->parts[k].dead) { if (f->parts[k].dead == 2) ++f->dDrainDel; else ++f->dLifeDel; reportDelete(f, f->parts[k].id); f->idToIdx[f->parts[k].id] = -1; removeAt(f, k); }
            else ++k;
        }
        // write data
        B3 bb; bEmpty(bb);
        const PData& wd = f->write;
        NxU32 n = f->n;
        bool packets = f->packetData[0] && f->packetData[1] && n > 0;
        if (packets) {
            float cell = f->rppm > 0 ? (float)(f->psm ? f->psm : 16) * f->krm / f->rppm : 1.0f;
            if (!(cell > 1e-4f)) cell = 1.0f;
            packets = packetOrder(f, n, cell);
        }
        for (NxU32 k = 0; k < n; ++k) {
            const Particle& p = f->parts[packets ? f->pkIdx[k] : k];
            bAdd(bb, p.p);
            if (wd.pos)  { float* o = at<float>(wd.pos, wd.sPos, k); o[0] = p.p.x; o[1] = p.p.y; o[2] = p.p.z; }
            if (wd.vel)  { float* o = at<float>(wd.vel, wd.sVel, k); o[0] = p.v.x; o[1] = p.v.y; o[2] = p.v.z; }
            if (wd.life) { *at<float>(wd.life, wd.sLife, k) = p.life < 0 ? 0.0f : p.life; }
            if (wd.dens) { *at<float>(wd.dens, wd.sDens, k) = f->restDensity; }
            if (wd.id)   { *at<NxU32>(wd.id, wd.sId, k) = p.id; }
            if (wd.flag) { *at<NxU32>(wd.flag, wd.sFlag, k) = p.flags; }
            if (wd.nrm)  { float* o = at<float>(wd.nrm, wd.sNrm, k); o[0] = p.n.x; o[1] = p.n.y; o[2] = p.n.z; }
        }
        if (wd.num) *wd.num = n;
        if (packets) writePackets(f, n);
        else if (f->packetData[1] && n == 0 && writableRange((void*)(ULONG_PTR)f->packetData[1], 4)) *(NxU32*)(ULONG_PTR)f->packetData[1] = 0;
        ++f->dFrames; if (n > f->dPeak) f->dPeak = n; if (n) f->dSeen = true;
        for (NxU32 k = 0; k < n; k += 7) { float q = dot(f->parts[k].v, f->parts[k].v); if (q > f->dMaxSpeed) f->dMaxSpeed = q; }
        if (n) { V3 r = v3(f->sRadius, f->sRadius, f->sRadius); bb.mn = bb.mn - r; bb.mx = bb.mx + r; }
        f->bounds = bb;
        // deletion ids, then free them
        NxU32 nd = f->nDelReport;
        if (f->delW.id) for (NxU32 k = 0; k < nd; ++k) *at<NxU32>(f->delW.id, f->delW.sId, k) = f->delReport[k];
        if (f->delW.num) *f->delW.num = f->delW.id ? nd : 0;
        for (NxU32 k = 0; k < nd; ++k) if (f->nFree < f->maxParticles) f->freeIds[f->nFree++] = f->delReport[k];
        f->nDelReport = 0;
        if (f->creW.num) *f->creW.num = 0;
        particles += (int)n;
        LeaveCriticalSection(&f->cs);
    }
    if (anyCloth) clothPublishScene(scene);
    g_stats.fluids = fluids;
    g_stats.particles = particles;
    g_stats.prepMs = g_prepTicks * g_msPerTick;
    g_stats.waitMs = waitMs;
    g_stats.stepMs = g_jobTicks * g_msPerTick;
    g_stats.narrowTests = g_jobTests;
}

FrameStats lastStats() { return g_stats; }

void logSummary(const char* when) {
    LOG("FLUIDREP summary %s: %d fluids, %d particles, static geoms %d, dynamic geoms %d", when, g_stats.fluids, g_stats.particles,
        g_stats.staticGeoms, g_stats.dynamicGeoms);
    for (int i = 0; i < 81; ++i) if (g_slotCalls[i] && i != 72 && i != 58) LOG("FLUIDREP   NxFluid slot %d called %ld times", i, g_slotCalls[i]);
}

} // namespace fluidrep
