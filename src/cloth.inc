// ---------------------------------------------------------------------------
// Cloth replacement (NxCloth, PhysX 2.8.0.7 ABI) - included into fluid.cpp, namespace fluidrep.
//
// Why: PhysX 2.8.0's software cloth collides every cloth vertex against every shape in the cloth's bounding
// box. Torn plastic spreads over a room with thousands of shapes and costs 100-150 ms per frame.
// What: createCloth returns this object. It implements the game's 92-slot NpCloth vtable (slots verified
// against PhysXCore: header order of 2.8.4 minus the functions 2.8.0 doesn't have, stack cleanup checked
// for every slot), simulates the cloth with position-based dynamics (stretching constraints, optional
// bending, gravity/external/wind acceleration, attachments, tearing) and collides each vertex only with the
// shapes near it, using the fluid replacement's collision world (BVH per filter class) and thread pool.
//
// Game contract (measured with the NxCloth census): creation + setters; per frame attachVertexToGlobalPosition
// for pinned vertices, getNumberOfParticles/getVelocities/setVelocities (wind), getWorldBounds, getThickness,
// getScene; bullets use raycast + addDirectedForceAtPos; setFlags/getFlags; setMeshData. PhysX writes the
// game's NxMeshData buffers (positions, normals, indices, parent indices, dirty flags) at fetchResults; so do we.
//
// Threading: the step runs on the worker pool between simulate and fetchResults and only touches the
// simulation arrays. API calls (game thread, possibly during the step) read a published copy and queue
// writes; both are swapped/applied on the game thread at beginStep/endStep under the cloth's lock.
// ---------------------------------------------------------------------------

static const NxU32 CLOTH_MAGIC = 0x434C5550;   // 'PULC'
enum { CLF_PRESSURE = 1, CLF_STATIC = 2, CLF_DISABLE_COLLISION = 4, CLF_SELFCOLLISION = 8, CLF_GRAVITY = 0x20,
       CLF_BENDING = 0x40, CLF_DAMPING = 0x100, CLF_COLLISION_TWOWAY = 0x200, CLF_TEARABLE = 0x1000, CLF_HARDWARE = 0x2000,
       CLF_COMDAMPING = 0x4000, CLF_VALIDBOUNDS = 0x8000 };
enum { CMV_ATTACHED = 1, CMV_TEARABLE = 1 << 7, CMV_EMBED = 1 << 6 };   // EMBED: internal (vertex started inside a moving shape)
enum { MDF_16BIT = 1 };
enum { MDD_POS = 1, MDD_NRM = 2, MDD_IDX = 4, MDD_PAR = 8 };

struct MeshDataR {   // NxMeshData (2.8.0): 17 dwords
    void* pos; void* nrm; NxU32 posStride, nrmStride, maxV; NxU32* numV;
    void* idx; NxU32 idxStride, maxI; NxU32* numI;
    void* par; NxU32 parStride, maxP; NxU32* numP;
    NxU32* dirty; NxU32 flags; const char* name;
};
struct CEdge { int a, b; float rest; };

struct ClothR {
    void** vtbl; void* userData;                    // NxCloth ABI: vptr, userData
    NxU32 magic; void* scene; void* mesh; CRITICAL_SECTION cs; int index;
    unsigned char desc[0x118];                       // original descriptor (saveToDesc)
    // parameters
    float bending, stretching, damping, friction, pressure, tearFactor, attachTearFactor, thickness, density, relGrid;
    float collResp, attachResp, fromFluid, toFluid, minAdhere, sleepVel, wakeCounter;
    NxU32 iters, flags; unsigned short group, ffm; GMask mask; V3 extAcc, wind; B3 validBounds;
    const char* name; void* compartment;
    MeshDataR md;
    // simulation state (worker during the step, game thread otherwise)
    int n, capV, nTri; NxU32* tri;
    V3* x; V3* v; V3* nrm; V3* rest; float* invMass; float* mass; int* parent; unsigned char* vflag;
    unsigned char* att; V3* attTgt; V3* attPrev;
    CEdge* edges; int nEdge, capEdge;
    CEdge* bends; int nBend, capBend;
    bool topoDirty, firstOut, sleeping; float sleepTimer;
    // queued from the API (game thread)
    V3* pendVel; bool hasPendVel; V3* pendDv; bool hasPendDv; unsigned char* pendAtt; V3* pendAttPos; bool hasPendAtt;
    bool pendWake, pendSleep;
    // published copy for the API
    V3* pubX; V3* pubV; V3* pubNrm; float* pubMass; int pubN; NxU32* pubTri; B3 pubBounds; bool pubSleeping;
    // step snapshot
    World* sWorld; int cls; bool classDirty; float sH; int sSub; V3 sAccel; bool sActive;
    int tears, tearsTotal; double lastMs;
    V3* sPred; unsigned char* sHit; V3* sHitN;      // step scratch
    // long-range attachments: every free vertex stays within its rest (geodesic) distance of its nearest pinned vertex
    int* lraAnchor; float* lraDist; bool lraDirty; int lraCount;
    float lastDt;                                    // last simulated frame time (force modes scale by it)
    // diagnostics (logged every 600 frames while the cloth is active)
    float dMaxStretch, dMaxSpeed, dMaxImpulse, dMaxDv; int dImpulses, dImpulseVerts, dTears, dPublishes, dUnconsumed, dSteps;
    int dFramesStatic, dFramesAsleep, dFrames, dFlagLogs, dWakes;
    float pose[12]; int poseMode; bool poseChecked; int nOrig;   // creation pose, how it is applied (see clothCheckPose)
    int sinceCreate; bool settleLogged;                         // 1.1.14: one "settle check" line ~1 s after creation
    // 1.1.14 first-steps report: what pushes the cloth during its first 3 simulated steps (worker writes, game thread logs)
    int eSteps; bool eReady; int eInS, eInD, eSweepS, eSweepD; float eDepS, eDepD, eMaxV, eDt; void* eActS; void* eActD;
    int vbOutNow, vbOutMax, vbFirstLogged, vbSetLogs, vbProbes;   // valid bounds (report only in 1.1.14): vertices outside the game's box
    float vbFarthest;
    // 1.1.14: moving shapes that enclose every pinned vertex of this cloth. The cloth hangs INSIDE such a shape (in
    // Ropeburn a 54 x 57 x 23 m moving convex fills the whole atrium), so it is a volume, not an obstacle: the cloth can
    // never be pushed out of it, and trying pushed loose pieces out through its bottom face, i.e. through the floor.
    void* encl[4]; int nEncl, enclAge, enclLogged;
    float pushCap;    // largest push-out of a vertex by a moving shape per substep (0.35 x mean edge): no single push can tear
    bool freshStep;   // first step after creation or after the game unfreezes the cloth: overlaps that exist then are not hits
    int eTypeS, eTypeD; float eAttJump; int eSub; int eCapped, eEmbedded; int eTearsF; float eMaxVAfter; bool eLoggedSetVel; int eSetVelCalls; int eSetPosCalls; float eSetPosMove, eSetVelMax; bool eDone;
};

static ClothR* g_cloths[64];
static int g_nCloths = 0;
static float g_frameDt = 1.0f / 60.0f;
#ifdef NO_LRA
static const bool g_noLra = true;
#else
static const bool g_noLra = false;
#endif
static int g_clothEnabled = 0;
static bool g_clothStartRefit = true;   // [Cloth] StartRefit (1.1.14)
static volatile LONG g_clothSlotCalls[92];
static void jobWait();
static void poolInit();
static bool readable(const void* p, SIZE_T n);

static ClothR* C(void* self) { return (ClothR*)self; }

// k such that 'iters' applications give the overall stiffness ks: 1 - (1 - k)^iters = ks (bisection)
static float perIterStiffness(float ks, NxU32 iters) {
    if (ks >= 1.0f) return 1.0f;
    if (ks <= 0.0f) return 0.0f;
    float lo = 0, hi = 1;
    for (int b = 0; b < 30; ++b) {
        float k = 0.5f * (lo + hi), q = 1.0f;
        for (NxU32 i = 0; i < iters; ++i) q *= (1.0f - k);
        if (1.0f - q < ks) lo = k; else hi = k;
    }
    return 0.5f * (lo + hi);
}

// --- geometry helpers ---
static void clothRebuildEdges(ClothR* c) {
    // unique triangle edges (stretching) and, with bending, the opposite-vertex pair across each shared edge
    int ne = 0, nb = 0;
    grow(c->edges, c->capEdge, c->nTri * 3);
    // simple hash of edges -> (other vertex of the first triangle) for bending pairs
    int hcap = 1; while (hcap < c->nTri * 6) hcap <<= 1;
    struct HE { int a, b, opp; };
    HE* h = allocN<HE>(hcap);
    for (int i = 0; i < hcap; ++i) h[i].a = -1;
    bool wantBend = (c->flags & CLF_BENDING) != 0;
    if (wantBend) grow(c->bends, c->capBend, c->nTri * 3);
    for (int t = 0; t < c->nTri; ++t) {
        int v[3] = { (int)c->tri[3 * t], (int)c->tri[3 * t + 1], (int)c->tri[3 * t + 2] };
        for (int k = 0; k < 3; ++k) {
            int a = v[k], b = v[(k + 1) % 3], o = v[(k + 2) % 3];
            if (a > b) { int s = a; a = b; b = s; }
            NxU32 hh = ((NxU32)a * 73856093u ^ (NxU32)b * 19349663u) & (hcap - 1);
            for (;;) {
                if (h[hh].a < 0) {
                    h[hh].a = a; h[hh].b = b; h[hh].opp = o;
                    c->edges[ne].a = a; c->edges[ne].b = b; c->edges[ne].rest = len(c->rest[a] - c->rest[b]); ++ne;
                    break;
                }
                if (h[hh].a == a && h[hh].b == b) {
                    if (wantBend && nb < c->capBend) {
                        int p = h[hh].opp, q = o;
                        c->bends[nb].a = p; c->bends[nb].b = q; c->bends[nb].rest = len(c->rest[p] - c->rest[q]); ++nb;
                    }
                    break;
                }
                hh = (hh + 1) & (hcap - 1);
            }
        }
    }
    zfree(h);
    c->nEdge = ne; c->nBend = nb;
    { double sum = 0; int k = 0; for (int e = 0; e < ne; ++e) if (c->edges[e].rest > 0) { sum += c->edges[e].rest; ++k; }
      float mean = k ? (float)(sum / k) : 0.1f; c->pushCap = mean * 0.35f; if (c->pushCap < 0.01f) c->pushCap = 0.01f; }
}

static void clothComputeMasses(ClothR* c) {
    for (int i = 0; i < c->n; ++i) c->mass[i] = 0;
    for (int t = 0; t < c->nTri; ++t) {
        int a = c->tri[3 * t], b = c->tri[3 * t + 1], d = c->tri[3 * t + 2];
        float area = 0.5f * len(cross(c->rest[b] - c->rest[a], c->rest[d] - c->rest[a]));
        float m = area * c->density / 3.0f;
        c->mass[a] += m; c->mass[b] += m; c->mass[d] += m;
    }
    for (int i = 0; i < c->n; ++i) {
        if (c->mass[i] < 1e-6f) c->mass[i] = 1e-6f;
        c->invMass[i] = c->att[i] ? 0.0f : 1.0f / c->mass[i];
    }
}

static void clothComputeNormals(ClothR* c) {
    for (int i = 0; i < c->n; ++i) c->nrm[i] = v3(0, 0, 0);
    for (int t = 0; t < c->nTri; ++t) {
        int a = c->tri[3 * t], b = c->tri[3 * t + 1], d = c->tri[3 * t + 2];
        V3 nn = cross(c->x[b] - c->x[a], c->x[d] - c->x[a]);
        c->nrm[a] = c->nrm[a] + nn; c->nrm[b] = c->nrm[b] + nn; c->nrm[d] = c->nrm[d] + nn;
    }
    for (int i = 0; i < c->n; ++i) c->nrm[i] = norm(c->nrm[i]);
}

// Long-range attachments (Kim, Chentanez, Mueller 2012). PBD with a few iterations lets a large hanging sheet stretch
// 20-30% under its own weight (measured on a 40x42 tarp: max edge stretch 1.31 with 5 iterations), which is close to the
// game's tearFactor 1.5, so wind alone tore the plastic and used up the tear reserve. Each free vertex gets a
// unilateral constraint: distance to its nearest pinned vertex <= rest geodesic distance (multi-source Dijkstra over the
// rest-length edge graph). Vertices with no path to a pinned vertex (torn-off pieces) are unconstrained.
static void clothComputeLra(ClothR* c) {
    int n = c->n;
    c->lraCount = 0;
    c->lraDirty = false;
    bool anyAtt = false;
    for (int i = 0; i < n; ++i) { c->lraAnchor[i] = -1; c->lraDist[i] = BIG; if (c->att[i]) anyAtt = true; }
    if (!anyAtt) return;
    // CSR adjacency from the stretching edges
    int* deg = allocN<int>(n + 1); int* adj = allocN<int>(2 * c->nEdge + 1); float* w = allocN<float>(2 * c->nEdge + 1);
    for (int e = 0; e < c->nEdge; ++e) { ++deg[c->edges[e].a + 1]; ++deg[c->edges[e].b + 1]; }
    for (int i = 0; i < n; ++i) deg[i + 1] += deg[i];
    int* fill = allocN<int>(n);
    for (int e = 0; e < c->nEdge; ++e) {
        int a = c->edges[e].a, b = c->edges[e].b; float r = c->edges[e].rest;
        adj[deg[a] + fill[a]] = b; w[deg[a] + fill[a]++] = r;
        adj[deg[b] + fill[b]] = a; w[deg[b] + fill[b]++] = r;
    }
    // binary heap of (dist, vertex), lazy deletion
    int cap = 2 * c->nEdge + n + 1, hn = 0;
    float* hk = allocN<float>(cap); int* hv = allocN<int>(cap);
    auto push = [&](float k, int v) {
        if (hn >= cap) return;
        int i = hn++;
        while (i > 0) { int p = (i - 1) >> 1; if (hk[p] <= k) break; hk[i] = hk[p]; hv[i] = hv[p]; i = p; }
        hk[i] = k; hv[i] = v;
    };
    for (int i = 0; i < n; ++i) if (c->att[i]) { c->lraDist[i] = 0; c->lraAnchor[i] = i; push(0, i); }
    while (hn) {
        float k = hk[0]; int v = hv[0];
        float lk = hk[--hn]; int lv = hv[hn];
        int i = 0;
        for (;;) { int l = 2 * i + 1; if (l >= hn) break; int r = l + 1; int m = (r < hn && hk[r] < hk[l]) ? r : l; if (hk[m] >= lk) break; hk[i] = hk[m]; hv[i] = hv[m]; i = m; }
        if (hn) { hk[i] = lk; hv[i] = lv; }
        if (k > c->lraDist[v]) continue;
        for (int q = deg[v]; q < deg[v + 1]; ++q) {
            int u = adj[q]; float nd = k + w[q];
            if (nd < c->lraDist[u]) { c->lraDist[u] = nd; c->lraAnchor[u] = c->lraAnchor[v]; push(nd, u); }
        }
    }
    for (int i = 0; i < n; ++i) if (!c->att[i] && c->lraAnchor[i] >= 0) ++c->lraCount;
    zfree(deg); zfree(adj); zfree(w); zfree(fill); zfree(hk); zfree(hv);
}

// Split vertex vi along the plane through it with normal pn: the triangles on the +pn side move to a copy.
static bool clothSplitVertex(ClothR* c, int vi, V3 pn) {
    if (c->n >= c->capV) return false;
    int nv = c->n, moved = 0, stay = 0;
    for (int t = 0; t < c->nTri; ++t) {
        NxU32* tv = &c->tri[3 * t];
        int k = tv[0] == (NxU32)vi ? 0 : tv[1] == (NxU32)vi ? 1 : tv[2] == (NxU32)vi ? 2 : -1;
        if (k < 0) continue;
        V3 cen = (c->x[tv[0]] + c->x[tv[1]] + c->x[tv[2]]) * (1.0f / 3.0f);
        if (dot(cen - c->x[vi], pn) > 0) ++moved; else ++stay;
    }
    if (!moved || !stay) return false;
    for (int t = 0; t < c->nTri; ++t) {
        NxU32* tv = &c->tri[3 * t];
        int k = tv[0] == (NxU32)vi ? 0 : tv[1] == (NxU32)vi ? 1 : tv[2] == (NxU32)vi ? 2 : -1;
        if (k < 0) continue;
        V3 cen = (c->x[tv[0]] + c->x[tv[1]] + c->x[tv[2]]) * (1.0f / 3.0f);
        if (dot(cen - c->x[vi], pn) > 0) tv[k] = (NxU32)nv;
    }
    c->x[nv] = c->x[vi]; c->v[nv] = c->v[vi]; c->rest[nv] = c->rest[vi]; c->parent[nv] = c->parent[vi];
    c->vflag[nv] = c->vflag[vi]; c->att[nv] = 0; c->attTgt[nv] = c->x[vi]; c->attPrev[nv] = c->x[vi];
    c->nrm[nv] = c->nrm[vi];
    c->n = nv + 1;
    return true;
}

// --- the step (worker thread, one cloth per job) ---
static void clothFindEnclosing(ClothR* c, const World& w, const FilterClass* fc) {
    c->enclAge = 0;
    void* found[4]; int nf = 0;
    int pins = 0; for (int i = 0; i < c->n; ++i) if (c->att[i]) ++pins;
    if (pins == 0) return;                       // nothing pinned (any more): keep what was found while it was
#ifdef EPXL_TEST_NO_ENCL
    pins = 0;
#endif
    if (fc && pins > 0) {
        for (int k = 0; k < fc->nD && nf < 4; ++k) {
            const Geom& g = w.dg[fc->dIdx[k]];
            if (g.type != GT_CONVEX && g.type != GT_BOX) continue;
            bool all = true; float minDepth = BIG;
            for (int i = 0; i < c->n && all; ++i) {
                if (!c->att[i]) continue;
                Hit h; bool hit = g.type == GT_CONVEX ? hitConvex(w.dp, g, c->x[i], v3(0, 0, 0), 0, h) : hitBox(g, c->x[i], v3(0, 0, 0), 0, h);
                if (!hit || !h.inside || h.depth < 0.05f) all = false; else if (h.depth < minDepth) minDepth = h.depth;
            }
            if (!all) continue;
            found[nf++] = g.shape;
            bool known = false; for (int j = 0; j < c->nEncl; ++j) if (c->encl[j] == g.shape) known = true;
            if (!known && c->enclLogged < 6) {
                ++c->enclLogged;
                NxU32 af = 0; if (g.actor && readable((const char*)g.actor + 0x2c, 4)) af = *(const NxU32*)((const char*)g.actor + 0x2c);
                LOG("CLOTHREP cloth %p hangs inside a moving %s (actor %p, %.1f x %.1f x %.1f m, z %.2f..%.2f): all %d pinned vertices are inside it "
                    "(at least %.2f m deep) -> treated as a volume, not an obstacle. Shape flags 0x%x, actor flags 0x%x, group %u",
                    c, g.type == GT_CONVEX ? "convex" : "box", g.actor, g.aabb.mx.x - g.aabb.mn.x, g.aabb.mx.y - g.aabb.mn.y, g.aabb.mx.z - g.aabb.mn.z,
                    g.aabb.mn.z, g.aabb.mx.z, pins, minDepth, g.dbgShapeFlags, af, g.group);
            }
        }
    }
    for (int j = 0; j < nf; ++j) c->encl[j] = found[j];
    c->nEncl = nf;
}

static void clothStep(ClothR* c, int& tests) {
    LONGLONG t0 = qpc();
    const World& w = *c->sWorld;
    const FilterClass* fc = (!(c->flags & CLF_DISABLE_COLLISION) && c->cls >= 0 && c->cls < w.nCls) ? &w.cls[c->cls] : 0;
    float h = c->sH;
    int n = c->n;
    V3* pred = c->sPred; unsigned char* hit = c->sHit; V3* hitN = c->sHitN;      // scratch, sized capV
    float kS = c->stretching; if (kS > 1) kS = 1; if (kS < 0) kS = 0;
    float kB = c->bending; if (kB > 1) kB = 1; if (kB < 0) kB = 0;
    NxU32 iters = c->iters ? c->iters : 1; if (iters > 32) iters = 32;
    // per-iteration stiffness so that 'iters' iterations give the requested overall stiffness
    float kSi = perIterStiffness(kS, iters), kBi = perIterStiffness(kB, iters);
    float r = c->thickness > 0 ? c->thickness : 0.01f;
    if (c->lraDirty) clothComputeLra(c);
    if (c->freshStep || c->enclAge++ >= 120) clothFindEnclosing(c, w, fc);
    float eV0 = 0;
    if (c->eSteps < 3) {
        for (int i = 0; i < n; ++i) {
            if (c->att[i]) { float j = len(c->attTgt[i] - c->attPrev[i]); if (j > c->eAttJump) c->eAttJump = j; }
            else { float q = len(c->v[i]); if (q > eV0) eV0 = q; }
        }
        if (eV0 > c->eMaxV) c->eMaxV = eV0;         // eMaxV first holds the speed before the steps (see clothFirstStepsReport)
        if (h * c->sSub > c->eDt) { c->eDt = h * c->sSub; c->eSub = c->sSub; }
    }
    for (int s = 0; s < c->sSub; ++s) {
        float a = (float)(s + 1) / (float)c->sSub;
        // integrate
        for (int i = 0; i < n; ++i) {
            if (c->att[i]) { pred[i] = c->attPrev[i] + (c->attTgt[i] - c->attPrev[i]) * a; continue; }
            V3 vv = c->v[i] + c->sAccel * h;
            if (c->flags & CLF_DAMPING) { float d = c->damping * h * 10.0f; if (d > 1) d = 1; vv = vv * (1.0f - d); }
            c->v[i] = vv;
            pred[i] = c->x[i] + vv * h;
            hit[i] = 0;
        }
        if (c->flags & CLF_COMDAMPING) {
            V3 vc = v3(0, 0, 0); float m = 0;
            for (int i = 0; i < n; ++i) if (!c->att[i]) { vc = vc + c->v[i] * c->mass[i]; m += c->mass[i]; }
            if (m > 0) {
                vc = vc * (1.0f / m);
                float d = c->damping * h * 10.0f; if (d > 1) d = 1;
                for (int i = 0; i < n; ++i) if (!c->att[i]) { pred[i] = pred[i] - (c->v[i] - vc) * (d * h); }
            }
        }
        // constraints
        for (NxU32 it = 0; it < iters; ++it) {
            for (int e = 0; e < c->nEdge; ++e) {
                int ia = c->edges[e].a, ib = c->edges[e].b;
                float wa = c->invMass[ia], wb = c->invMass[ib], ws = wa + wb;
                if (ws <= 0) continue;
                V3 d = pred[ib] - pred[ia];
                float l = len(d);
                if (l < 1e-9f) continue;
                float k = (l - c->edges[e].rest) / (l * ws) * kSi;
                pred[ia] = pred[ia] + d * (k * wa);
                pred[ib] = pred[ib] - d * (k * wb);
            }
            for (int e = 0; e < c->nBend; ++e) {
                int ia = c->bends[e].a, ib = c->bends[e].b;
                float wa = c->invMass[ia], wb = c->invMass[ib], ws = wa + wb;
                if (ws <= 0) continue;
                V3 d = pred[ib] - pred[ia];
                float l = len(d);
                if (l < 1e-9f || l > c->bends[e].rest) continue;      // bending resists folding only
                float k = (l - c->bends[e].rest) / (l * ws) * kBi;
                pred[ia] = pred[ia] + d * (k * wa);
                pred[ib] = pred[ib] - d * (k * wb);
            }
            if (c->lraCount && !g_noLra) {
                for (int i = 0; i < n; ++i) {
                    int an = c->lraAnchor[i];
                    if (an < 0 || c->att[i] || !c->att[an]) continue;
                    V3 d = pred[i] - pred[an];
                    float l2 = dot(d, d), L = c->lraDist[i];
                    if (l2 > L * L) pred[i] = pred[an] + d * (L / fsqrt(l2));
                }
            }
        }
        // collision: swept sphere (radius = thickness) from x to pred against the shapes near the vertex
        if (fc) {
            for (int i = 0; i < n; ++i) {
                if (c->att[i]) continue;
                V3 p0 = c->x[i], p1 = pred[i];
                for (int itc = 0; itc < 2; ++itc) {
                    QueryCtx q;
                    q.w = &w; q.p0 = p0; q.D = p1 - p0; q.r = r; q.bestGeom = -1; q.tests = 0; q.bestDyn = false; q.skip = 0; q.nSkip = 0;
                    // a vertex deep inside a solid (more than this step's motion) started there: PhysX's swept test never
                    // sees it, and pushing it out across the solid tore sheets apart
                    q.maxInside = r + len(q.D) + 0.02f;
                    B3 box; bEmpty(box); bAdd(box, p0); bAdd(box, p1);
                    box.mn = box.mn - v3(r, r, r); box.mx = box.mx + v3(r, r, r);
                    q.geoms = w.sg; q.planes = w.sp;
                    queryBvh(fc->sBvh, box, q, false);
                    if (fc->nD) {
                        QueryCtx qd = q; qd.geoms = w.dg; qd.planes = w.dp; qd.tests = 0; qd.bestGeom = -1;
                        qd.skip = c->encl; qd.nSkip = c->nEncl;
                        // moving shapes push into the cloth during a step; but on the cloth's first step (after creation or
                        // unfreezing) an overlap is where things started, not a hit (Ropeburn: a moving box already 0.7 m
                        // inside two tarps launched them at 44 m/s and tore them on their first step)
                        qd.maxInside = r + len(q.D) + 0.5f;
                        queryBvh(fc->dBvh, box, qd, true);
                        if (qd.bestGeom >= 0 && qd.best.inside) {
                            if (qd.best.depth > q.maxInside && (c->freshStep || (c->vflag[i] & CMV_EMBED))) {
                                c->vflag[i] |= CMV_EMBED;            // started inside it: stays put until it is out (like static)
                                if (c->eSteps < 3) { ++c->eInD; if (qd.best.depth > c->eDepD) { c->eDepD = qd.best.depth; c->eActD = qd.geoms[qd.bestGeom].actor; c->eTypeD = qd.geoms[qd.bestGeom].type; } ++c->eEmbedded; }
                                qd.bestGeom = -1;
                            }
                        } else if (itc == 0) c->vflag[i] &= ~CMV_EMBED;
                        q.tests += qd.tests;
                        if (qd.bestGeom >= 0 && (q.bestGeom < 0 ||
                            (qd.best.inside && (!q.best.inside || qd.best.depth > q.best.depth)) ||
                            (!qd.best.inside && !q.best.inside && qd.best.t < q.best.t))) { q.best = qd.best; q.bestGeom = qd.bestGeom; q.bestDyn = true; }
                    }
                    tests += q.tests;
                    if (q.bestGeom < 0) break;
                    V3 nn = q.best.n;
                    if (hit[i] != 2) hit[i] = q.bestDyn ? 2 : 1;      // 1 = level geometry, 2 = a moving shape
                    hitN[i] = nn;
                    if (c->eSteps < 3) {
                        const Geom& g = (q.bestDyn ? w.dg : w.sg)[q.bestGeom];
                        if (q.bestDyn) { if (q.best.inside) { ++c->eInD; if (q.best.depth > c->eDepD) { c->eDepD = q.best.depth; c->eActD = g.actor; c->eTypeD = g.type; } } else ++c->eSweepD; }
                        else           { if (q.best.inside) { ++c->eInS; if (q.best.depth > c->eDepS) { c->eDepS = q.best.depth; c->eActS = g.actor; c->eTypeS = g.type; } } else ++c->eSweepS; }
                    }
                    if (q.best.inside) {
                        // a moving shape deep in the cloth pushes it out gradually (at most pushCap per substep), so a single
                        // push can never stretch an edge past its tear limit
                        float dep = q.best.depth;
                        if (q.bestDyn && dep > c->pushCap) { dep = c->pushCap; if (c->eSteps < 3) ++c->eCapped; }
                        p0 = p0 + nn * (dep + 1e-4f);
                        V3 rem = p1 - q.p0; float rn = dot(rem, nn); if (rn < 0) rem = rem - nn * rn;
                        p1 = p0 + rem;
                    } else {
                        V3 hp = q.p0 + q.D * q.best.t + nn * 1e-4f;
                        V3 rem = p1 - hp; float rn = dot(rem, nn); if (rn < 0) rem = rem - nn * rn;
                        p0 = hp; p1 = hp + rem;
                    }
                }
                pred[i] = p1;
            }
        }
        // velocities, friction, commit
        float invH = h > 0 ? 1.0f / h : 0;
        for (int i = 0; i < n; ++i) {
            if (c->att[i]) { c->v[i] = (pred[i] - c->x[i]) * invH; c->x[i] = pred[i]; continue; }
            V3 vv = (pred[i] - c->x[i]) * invH;
            if (hit[i]) {
                V3 nn = hitN[i];
                float vn = dot(vv, nn);
                V3 vt = vv - nn * vn;
                if (vn < 0) vn = 0;
                // A push out of a solid is a position correction, not a launch: level geometry gives the cloth no speed away
                // from it, a moving shape at most a running speed. Without this a vertex pushed 0.3 m out of a box in one
                // 16 ms step left at 18 m/s, and faster vertices were allowed deeper pushes: tarps tore by themselves.
                float vnMax = hit[i] == 2 ? 10.0f : 0.5f;
                if (vn > vnMax) vn = vnMax;
                vv = nn * vn + vt * (1.0f - (c->friction > 1 ? 1 : c->friction < 0 ? 0 : c->friction));
            }
            c->v[i] = vv; c->x[i] = pred[i];
        }
    }
    // tearing: edges stretched beyond tearFactor split their (tearable) endpoint
    // Edges touching a pinned vertex never tear (PhysX tears attachments only with NX_CLOTH_ATTACHMENT_TEARABLE, which
    // the game doesn't use); a vertex is split at most once per step.
    float maxStretch = 0;
    for (int e = 0; e < c->nEdge; ++e) {
        float rl = c->edges[e].rest;
        if (rl <= 0) continue;
        V3 d = c->x[c->edges[e].b] - c->x[c->edges[e].a];
        float q = dot(d, d) / (rl * rl);
        if (q > maxStretch) maxStretch = q;
    }
    maxStretch = fsqrt(maxStretch);
    if (maxStretch > c->dMaxStretch) c->dMaxStretch = maxStretch;
    if ((c->flags & CLF_TEARABLE) && c->tearFactor > 1.0f && c->sSub && maxStretch > c->tearFactor && c->n < c->capV) {
        // PhysXCore's tear pass (RVA 0xEE0B0, after the step): every stretching constraint longer than tearFactor x rest
        // splits one of its tearable endpoints; when both are tearable the endpoint alternates with the step parity.
        const int MAXT = 64;
        int tornNow = 0; int done[MAXT]; int parity = c->tearsTotal + c->dSteps;
        for (int e = 0; e < c->nEdge && tornNow < MAXT; ++e) {
            int ia = c->edges[e].a, ib = c->edges[e].b;
            float rl = c->edges[e].rest;
            if (rl <= 0) continue;
            if (c->att[ia] || c->att[ib]) continue;          // never tear the cloth off its pins
            V3 d = c->x[ib] - c->x[ia];
            float l = len(d);
            if (l <= c->tearFactor * rl) continue;
            bool ta = (c->vflag[ia] & CMV_TEARABLE) != 0, tb = (c->vflag[ib] & CMV_TEARABLE) != 0;
            int vi = ta && tb ? ((parity++ & 1) ? ib : ia) : ta ? ia : tb ? ib : -1;
            if (vi < 0) continue;
            bool dup = false;
            for (int k = 0; k < tornNow; ++k) if (done[k] == vi) dup = true;
            if (dup) continue;
            V3 pn = norm(vi == ia ? d : d * -1.0f);
            if (clothSplitVertex(c, vi, pn)) done[tornNow++] = vi;
        }
        if (tornNow) {
            clothRebuildEdges(c);
            clothComputeMasses(c);
            clothComputeLra(c);
            c->topoDirty = true; c->tears += tornNow; c->tearsTotal += tornNow; c->dTears += tornNow;
            n = c->n;
        }
    }
    clothComputeNormals(c);
    // sleep detection (PhysX: all vertices slower than sleepLinearVelocity for wakeUpCounter seconds)
    float sv = c->sleepVel > 0 ? c->sleepVel : 0.15f;
    float maxV2 = 0;
    for (int i = 0; i < n; ++i) if (!c->att[i]) { float q = dot(c->v[i], c->v[i]); if (q > maxV2) maxV2 = q; }
    if (fsqrt(maxV2) > c->dMaxSpeed) c->dMaxSpeed = fsqrt(maxV2);
    ++c->dSteps;
    c->freshStep = false;
    if (c->eSteps < 3) {
        c->eTearsF += c->tears; if (fsqrt(maxV2) > c->eMaxVAfter) c->eMaxVAfter = fsqrt(maxV2);
        if (++c->eSteps == 3) c->eReady = true;
    }
    if (maxV2 < sv * sv) { c->sleepTimer += h * c->sSub; if (c->sleepTimer > (c->wakeCounter > 0 ? c->wakeCounter : 0.4f)) c->sleeping = true; }
    else c->sleepTimer = 0;
    c->lastMs = (qpc() - t0) * g_msPerTick;
}

// ---------------------------------------------------------------------------
// output to the game's NxMeshData (game thread)
// ---------------------------------------------------------------------------
static void clothWriteMeshData(ClothR* c, bool topo) {
    const MeshDataR& m = c->md;
    if (!m.pos || !m.numV) return;
    int n = c->pubN;
    NxU32 maxV = m.maxV ? m.maxV : (NxU32)n;
    NxU32 nw = (NxU32)n < maxV ? (NxU32)n : maxV;
    for (NxU32 i = 0; i < nw; ++i) {
        float* o = at<float>(m.pos, m.posStride ? m.posStride : 12, i); o[0] = c->pubX[i].x; o[1] = c->pubX[i].y; o[2] = c->pubX[i].z;
        if (m.nrm) { float* q = at<float>(m.nrm, m.nrmStride ? m.nrmStride : 12, i); q[0] = c->pubNrm[i].x; q[1] = c->pubNrm[i].y; q[2] = c->pubNrm[i].z; }
    }
    *m.numV = nw;
    NxU32 dirty = MDD_POS | (m.nrm ? MDD_NRM : 0);
    if (topo) {
        if (m.idx && m.numI) {
            NxU32 ni = (NxU32)c->nTri * 3; if (m.maxI && ni > m.maxI) ni = m.maxI - m.maxI % 3;
            bool b16 = (m.flags & MDF_16BIT) != 0;
            NxU32 st = m.idxStride ? m.idxStride : (b16 ? 2 : 4);
            for (NxU32 k = 0; k < ni; ++k) {
                NxU32 vi = c->pubTri[k]; if (vi >= nw) vi = 0;          // never point past what we wrote
                if (b16) *at<unsigned short>(m.idx, st, k) = (unsigned short)vi; else *at<NxU32>(m.idx, st, k) = vi;
            }
            *m.numI = ni;
            dirty |= MDD_IDX;
        }
        if (m.par && m.numP) {
            NxU32 np = nw; if (m.maxP && np > m.maxP) np = m.maxP;
            bool b16 = (m.flags & MDF_16BIT) != 0;
            NxU32 st = m.parStride ? m.parStride : (b16 ? 2 : 4);
            for (NxU32 k = 0; k < np; ++k) {
                NxU32 p = (NxU32)c->parent[k];
                if (b16) *at<unsigned short>(m.par, st, k) = (unsigned short)p; else *at<NxU32>(m.par, st, k) = p;
            }
            *m.numP = np;
            dirty |= MDD_PAR;
        }
    }
    if (m.dirty) *m.dirty |= dirty;
}

static void clothPublishOne(ClothR* c) {
    EnterCriticalSection(&c->cs);
    bool topo = c->topoDirty || c->firstOut;
    c->pubN = c->n;
    memcpy(c->pubX, c->x, sizeof(V3) * c->n);
    memcpy(c->pubV, c->v, sizeof(V3) * c->n);
    memcpy(c->pubNrm, c->nrm, sizeof(V3) * c->n);
    memcpy(c->pubMass, c->mass, sizeof(float) * c->n);
    if (topo) memcpy(c->pubTri, c->tri, sizeof(NxU32) * 3 * c->nTri);
    B3 b; bEmpty(b);
    for (int i = 0; i < c->n; ++i) bAdd(b, c->x[i]);
    c->pubBounds = b;
    c->pubSleeping = c->sleeping;
    // diagnostics: does the game consume (clear) the dirty flags between our writes?
    if (g_diag && c->md.dirty && readable(c->md.dirty, 4)) { ++c->dPublishes; if (*c->md.dirty & MDD_POS) ++c->dUnconsumed; }
    clothWriteMeshData(c, topo);
    c->topoDirty = false; c->firstOut = false;
    LeaveCriticalSection(&c->cs);
}

// ---------------------------------------------------------------------------
// beginStep / endStep integration (game thread)
// ---------------------------------------------------------------------------
static bool clothAnyInScene(void* scene) {
    for (int i = 0; i < g_nCloths; ++i) if (g_cloths[i]->scene == scene) return true;
    return false;
}

// Where the cloth starts. 1.1.1 in game: every tarp started mirrored top-to-bottom relative to where the game pins it
// (first simulated frame: edges next to the pins stretched 43x = sheet height / edge length, then the sheet exploded).
// Instead of trusting one reading of NxClothDesc::globalPose, the first batch of attachVertexToGlobalPosition targets
// decides: candidate placements of the rest mesh are compared with the game's pin positions and the closest one wins.
static V3 clothPlace(const float* m, V3 p, int mode) {
    switch (mode) {
    case 1: return poseApply(m, p, 1);                                  // rotation transposed (column-major NxMat33)
    case 2: return v3(m[9] + p.x, m[10] + p.y, m[11] + p.z);            // translation only
    case 3: return p;                                                   // mesh already in world space
    default: return poseApply(m, p, 0);                                 // NxMat34 as PhysX documents it (row-major)
    }
}
// Rigid fit (Horn 1987, unit quaternions): rotation R and translation t minimising sum |R a_i + t - b_i|^2.
static void hornFit(const V3* a, const V3* b, int n, float R[9], V3& t) {
    V3 ca = v3(0, 0, 0), cb = v3(0, 0, 0);
    for (int i = 0; i < n; ++i) { ca = ca + a[i]; cb = cb + b[i]; }
    ca = ca * (1.0f / n); cb = cb * (1.0f / n);
    double S[3][3] = { { 0 } };
    for (int i = 0; i < n; ++i) {
        V3 p = a[i] - ca, q = b[i] - cb;
        double pp[3] = { p.x, p.y, p.z }, qq[3] = { q.x, q.y, q.z };
        for (int r = 0; r < 3; ++r) for (int k = 0; k < 3; ++k) S[r][k] += pp[r] * qq[k];
    }
    double N[4][4] = {
        { S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0] },
        { S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2] },
        { S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1] },
        { S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2] } };
    double shift = 0; for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) shift += N[r][k] < 0 ? -N[r][k] : N[r][k];
    for (int r = 0; r < 4; ++r) N[r][r] += shift;                    // make it positive definite for power iteration
    double q[4] = { 1, 0.01, 0.02, 0.03 };
    for (int it = 0; it < 200; ++it) {
        double nq[4] = { 0, 0, 0, 0 };
        for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) nq[r] += N[r][k] * q[k];
        double l = 0; for (int r = 0; r < 4; ++r) l += nq[r] * nq[r];
        l = l > 0 ? 1.0 / fsqrt((float)l) : 0; for (int r = 0; r < 4; ++r) q[r] = nq[r] * l;
    }
    double w = q[0], x = q[1], y = q[2], z = q[3];
    R[0] = (float)(w*w + x*x - y*y - z*z); R[1] = (float)(2*(x*y - w*z));         R[2] = (float)(2*(x*z + w*y));
    R[3] = (float)(2*(x*y + w*z));         R[4] = (float)(w*w - x*x + y*y - z*z); R[5] = (float)(2*(y*z - w*x));
    R[6] = (float)(2*(x*z - w*y));         R[7] = (float)(2*(y*z + w*x));         R[8] = (float)(w*w - x*x - y*y + z*z);
    V3 rc = v3(R[0]*ca.x + R[1]*ca.y + R[2]*ca.z, R[3]*ca.x + R[4]*ca.y + R[5]*ca.z, R[6]*ca.x + R[7]*ca.y + R[8]*ca.z);
    t = cb - rc;
}
static V3 rotApply(const float R[9], V3 p) { return v3(R[0]*p.x + R[1]*p.y + R[2]*p.z, R[3]*p.x + R[4]*p.y + R[5]*p.z, R[6]*p.x + R[7]*p.y + R[8]*p.z); }

static void clothPlaceAll(ClothR* c, const float* m, int mode) {
    for (int i = 0; i < c->n; ++i) {
        c->x[i] = clothPlace(m, c->rest[i], mode); c->v[i] = v3(0, 0, 0);
        c->attTgt[i] = c->attPrev[i] = c->x[i];
        c->pubX[i] = c->x[i]; c->pubV[i] = v3(0, 0, 0);
    }
    c->topoDirty = true;
}

static void clothCheckPose(ClothR* c) {
    c->poseChecked = true;
    double err[4] = { 0, 0, 0, 0 }; int cnt = 0;
    for (int i = 0; i < c->nOrig && i < c->n; ++i) {
        if (c->pendAtt[i] != 1) continue;
        for (int k = 0; k < 4; ++k) err[k] += len(clothPlace(c->pose, c->rest[i], k) - c->pendAttPos[i]);
        ++cnt;
    }
    if (cnt < 2) { c->poseChecked = cnt == 0 ? false : true; return; }
    int best = 0;
    for (int k = 1; k < 4; ++k) if (err[k] < err[best]) best = k;
    static const char* names[4] = { "row-major", "transposed", "translation only", "world" };
    LOG("CLOTHREP cloth %p start pose check against %d pinned vertices: mean distance row-major %.3f, transposed %.3f, "
        "translation only %.3f, world %.3f -> %s", c, cnt, err[0] / cnt, err[1] / cnt, err[2] / cnt, err[3] / cnt, names[best]);
    // 1.1.14: even the best reading can be far from where the game pins the cloth (about 1 load in 10 in game, a tarp
    // started stretched across a whole room). Then no reading is trusted and the placement is fitted to the pins.
    B3 rb; bEmpty(rb); for (int i = 0; i < c->nOrig && i < c->n; ++i) bAdd(rb, c->rest[i]);
    float extent = len(rb.mx - rb.mn);
    double tol = extent * 0.05f > 0.25f ? extent * 0.05f : 0.25f;
    if (err[best] / cnt <= tol) {
        if (best != c->poseMode && err[best] <= 0.5 * err[c->poseMode]) {   // switch only when clearly better
            c->poseMode = best;
            if (c->n == c->nOrig) clothPlaceAll(c, c->pose, best);          // already torn: too late to move it
        }
        return;
    }
    double errMode = err[c->poseMode] / cnt;
    V3* a = allocN<V3>(cnt); V3* b = allocN<V3>(cnt); int m = 0;
    for (int i = 0; i < c->nOrig && i < c->n && m < cnt; ++i) if (c->pendAtt[i] == 1) { a[m] = c->rest[i]; b[m] = c->pendAttPos[i]; ++m; }
    V3 pinC = v3(0, 0, 0), placedC = v3(0, 0, 0);
    for (int i = 0; i < m; ++i) { pinC = pinC + b[i]; placedC = placedC + clothPlace(c->pose, a[i], c->poseMode); }
    pinC = pinC * (1.0f / m); placedC = placedC * (1.0f / m);
    LOG("CLOTHREP cloth %p START MISMATCH: pinned vertices are %.3f m (mean) from where the cloth was placed (cloth size %.2f m); "
        "pose translation (%.2f %.2f %.2f), placed pin centre (%.2f %.2f %.2f), game's pin centre (%.2f %.2f %.2f)%s",
        c, errMode, extent, c->pose[9], c->pose[10], c->pose[11], placedC.x, placedC.y, placedC.z, pinC.x, pinC.y, pinC.z,
        c->n != c->nOrig ? "; already torn, left as is" : "");
    if (c->n == c->nOrig) {
        // are the pins spread in 3D (not all on one line)?  far pair + distance of the others from that line
        int i0 = 0, i1 = 0; float best2 = -1;
        for (int i = 0; i < m; ++i) { float d = len(a[i] - a[0]); if (d > best2) { best2 = d; i1 = i; } }
        best2 = -1; for (int i = 0; i < m; ++i) { float d = len(a[i] - a[i1]); if (d > best2) { best2 = d; i0 = i; } }
        V3 axis = a[i1] - a[i0]; float span = len(axis); float off = 0;
        if (span > 1e-6f) { axis = axis * (1.0f / span); for (int i = 0; i < m; ++i) { V3 d = a[i] - a[i0]; V3 perp = d - axis * dot(d, axis); float l = len(perp); if (l > off) off = l; } }
        float R[9]; V3 t; bool rigid = m >= 3 && off > 0.05f * span;
        if (rigid) hornFit(a, b, m, R, t);
        else {                                                           // too few / collinear pins: keep the documented (row-major)
            V3 e0 = poseAxis(c->pose, 0, 0), e1 = poseAxis(c->pose, 1, 0), e2 = poseAxis(c->pose, 2, 0);   // orientation, move only
            R[0] = e0.x; R[1] = e1.x; R[2] = e2.x; R[3] = e0.y; R[4] = e1.y; R[5] = e2.y; R[6] = e0.z; R[7] = e1.z; R[8] = e2.z;
            V3 ca = v3(0, 0, 0); for (int i = 0; i < m; ++i) ca = ca + a[i]; ca = ca * (1.0f / m);
            t = pinC - rotApply(R, ca);
        }
        double res = 0; for (int i = 0; i < m; ++i) res += len(rotApply(R, a[i]) + t - b[i]); res /= m;
        bool ok = res < 0.5 * errMode && res <= tol;
        if (ok && g_clothStartRefit) {
            float np[12] = { R[0], R[1], R[2], R[3], R[4], R[5], R[6], R[7], R[8], t.x, t.y, t.z };
            memcpy(c->pose, np, 48); c->poseMode = 0;
            clothPlaceAll(c, c->pose, 0);
        }
        LOG("CLOTHREP cloth %p start refit (%s fit to %d pins): residual %.3f m -> %s", c, rigid ? "rigid" : "translation-only", m, res,
            !ok ? "REJECTED (the pins do not match any rigid placement of the cloth)" : g_clothStartRefit ? "APPLIED" : "not applied ([Cloth] StartRefit=0)");
    }
    zfree(a); zfree(b);
}

static void clothFirstStepsReport(ClothR* c) {
    c->eReady = false; c->eDone = true;
    static const char* gt[] = { "?", "box", "convex", "sphere", "capsule", "triangle", "?", "?", "?" };   // GT_* in fluid.cpp
    LOG("CLOTHREP cloth %p first %d simulated step(s): longest frame %.4f s in %d substeps; speed before %.2f m/s, after %.2f m/s; pins moved up to %.3f m; "
        "%d tears; setVelocity(ies) calls %d (fastest %.2f m/s); setPosition(s) calls %d (moved up to %.3f m); pushed out of static %d times (deepest %.3f m, %s, actor %p), of moving shapes %d times (deepest %.3f m, %s, actor %p); "
        "swept hits static %d / moving %d; %d moving-shape pushes limited to %.3f m; %d hits ignored (vertex started inside a moving shape)",
        c, c->eSteps, c->eDt, c->eSub, c->eMaxV, c->eMaxVAfter, c->eAttJump, c->eTearsF, c->eSetVelCalls, c->eSetVelMax, c->eSetPosCalls, c->eSetPosMove,
        c->eInS, c->eDepS, c->eInS ? gt[c->eTypeS < 9 ? c->eTypeS : 8] : "-", c->eActS,
        c->eInD, c->eDepD, c->eInD ? gt[c->eTypeD < 9 ? c->eTypeD : 8] : "-", c->eActD, c->eSweepS, c->eSweepD, c->eCapped, c->pushCap, c->eEmbedded);
}

// 1.1.14: when cloth goes below its valid bounds, list the solids in the column around that point: does the floor exist
// in our collision world, what is it, and does this cloth's collision filter include it?
static void clothFloorProbe(ClothR* c, V3 p) {
    World* w = worldFor(c->scene, false);
    if (!w) { LOG("CLOTHREP   floor probe: no collision world"); return; }
    const FilterClass* fc = (c->cls >= 0 && c->cls < w->nCls) ? &w->cls[c->cls] : 0;
    static const char* gt[] = { "?", "box", "convex", "sphere", "capsule", "triangle" };
    int shown = 0, total = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const Geom* arr = pass ? w->dg : w->sg; int cnt = pass ? w->nDg : w->nSg;
        for (int i = 0; i < cnt; ++i) {
            const Geom& g = arr[i];
            if (p.x < g.aabb.mn.x - 0.5f || p.x > g.aabb.mx.x + 0.5f || p.y < g.aabb.mn.y - 0.5f || p.y > g.aabb.mx.y + 0.5f) continue;
            if (g.aabb.mx.z < p.z - 15.0f || g.aabb.mn.z > p.z + 15.0f) continue;
            ++total;
            if (shown >= 12) continue;
            ++shown;
            bool in = fc && geomPasses(*w, *fc, g);
            if (g.type == GT_TRI)
                LOG("CLOTHREP   floor probe: %s triangle z %.2f..%.2f normal (%.2f %.2f %.2f), group %u -> %s", pass ? "moving" : "static",
                    g.aabb.mn.z, g.aabb.mx.z, g.tn.x, g.tn.y, g.tn.z, g.group, in ? "collides with this cloth" : "FILTERED OUT for this cloth");
            else
                LOG("CLOTHREP   floor probe: %s %s z %.2f..%.2f (x %.2f..%.2f, y %.2f..%.2f), group %u, actor %p -> %s", pass ? "moving" : "static",
                    gt[g.type < 6 ? g.type : 0], g.aabb.mn.z, g.aabb.mx.z, g.aabb.mn.x, g.aabb.mx.x, g.aabb.mn.y, g.aabb.mx.y, g.group, g.actor,
                    in ? "collides with this cloth" : "FILTERED OUT for this cloth");
        }
    }
    LOG("CLOTHREP   floor probe at (%.2f %.2f %.2f): %d solids within 15 m above/below (%d listed); cloth group %u, filter class %d, thickness %.3f",
        p.x, p.y, p.z, total, shown, c->group, c->cls, c->thickness);
}

// ~1 s after creation: how far the cloth is from its rest shape (catches tarps that start or end up stretched)
static void clothSettleCheck(ClothR* c) {
    c->settleLogged = true;
    if (!c->eDone && c->eSteps > 0) clothFirstStepsReport(c);    // frozen by the game before its 3rd step
    B3 rb, xb; bEmpty(rb); bEmpty(xb);
    for (int i = 0; i < c->n; ++i) { bAdd(xb, c->x[i]); if (i < c->nOrig) bAdd(rb, c->rest[i]); }
    float restExt = len(rb.mx - rb.mn), curExt = len(xb.mx - xb.mn), maxStretch = 0, sumStretch = 0; int ne = 0;
    for (int e = 0; e < c->nEdge; ++e) {
        const CEdge& E = c->edges[e]; if (E.rest <= 1e-6f) continue;
        float r = len(c->x[E.a] - c->x[E.b]) / E.rest; if (r > maxStretch) maxStretch = r; sumStretch += r; ++ne;
    }
    int pins = 0; for (int i = 0; i < c->n; ++i) if (c->att[i]) ++pins;
    bool warped = curExt > restExt * 2.0f + 1.0f || maxStretch > 4.0f;
    LOG("CLOTHREP cloth %p settle check after %d steps: size %.2f m (rest %.2f m), edge stretch mean %.2f max %.2f, %d pinned, %d torn vertices -> %s",
        c, c->sinceCreate, curExt, restExt, ne ? sumStretch / ne : 0.0f, maxStretch, pins, c->n - c->nOrig, warped ? "WARPED" : "ok");
}

// Apply queued API writes; returns the number of cloths that need a step.
static int clothPrepare(void* scene) {
    int active = 0;
    for (int ci = 0; ci < g_nCloths; ++ci) {
        ClothR* c = g_cloths[ci];
        if (c->scene != scene) continue;
        EnterCriticalSection(&c->cs);
        bool wake = c->pendWake;
        for (int i = 0; i < c->n; ++i) if (c->att[i]) c->attPrev[i] = c->x[i];
        c->lastDt = g_frameDt;
        if (c->hasPendAtt && !c->poseChecked) clothCheckPose(c);
        if (c->hasPendAtt) {
            bool massDirty = false;
            for (int i = 0; i < c->n; ++i) {
                if (c->pendAtt[i] == 1) {
                    V3 p = c->pendAttPos[i];
                    // a newly pinned vertex moves to its target from where it IS: attPrev was never set for it (zero =
                    // the world origin), and on a frame with substeps the pin swept in from the origin, tearing and
                    // warping the tarp (Ropeburn, 1.1.13: the game pinned and simulated on the same frame)
                    if (!c->att[i]) { c->att[i] = 1; c->attPrev[i] = c->x[i]; massDirty = true; wake = true; }
                    else if (len(p - c->attTgt[i]) > 1e-4f) wake = true;
                    c->attTgt[i] = p;
                } else if (c->pendAtt[i] == 2 && c->att[i]) { c->att[i] = 0; massDirty = true; wake = true; }
                c->pendAtt[i] = 0;
            }
            if (massDirty) { for (int i = 0; i < c->n; ++i) c->invMass[i] = c->att[i] ? 0.0f : 1.0f / c->mass[i]; c->lraDirty = true; }
            c->hasPendAtt = false;
        }
        if (g_diag && (c->flags & CLF_VALIDBOUNDS)) {   // with Log=1, report only: vertices PhysX would remove (outside the game's valid bounds)
            const B3& vb = c->validBounds; int out = 0; float far_ = 0; V3 fp = v3(0, 0, 0);
            for (int i = 0; i < c->n; ++i) {
                const V3& p = c->x[i];
                float dx = p.x < vb.mn.x ? vb.mn.x - p.x : p.x > vb.mx.x ? p.x - vb.mx.x : 0;
                float dy = p.y < vb.mn.y ? vb.mn.y - p.y : p.y > vb.mx.y ? p.y - vb.mx.y : 0;
                float dz = p.z < vb.mn.z ? vb.mn.z - p.z : p.z > vb.mx.z ? p.z - vb.mx.z : 0;
                float d = dx > dy ? (dx > dz ? dx : dz) : (dy > dz ? dy : dz);
                if (d > 0) { ++out; if (d > far_) { far_ = d; fp = p; } }
            }
            c->vbOutNow = out; if (out > c->vbOutMax) c->vbOutMax = out; if (far_ > c->vbFarthest) c->vbFarthest = far_;
            static const int kVbSteps[3] = { 1, 10, 100 };           // log when 1, 10 and 100 vertices are first outside
            if (c->vbFirstLogged < 3 && out >= kVbSteps[c->vbFirstLogged]) {
                while (c->vbFirstLogged < 3 && out >= kVbSteps[c->vbFirstLogged]) ++c->vbFirstLogged;
                LOG("CLOTHREP cloth %p VALID BOUNDS: %d of %d vertices are outside the game's box (farthest %.2f m out, at %.2f %.2f %.2f); "
                    "PhysX would remove them (report only, nothing removed)", c, out, c->n, far_, fp.x, fp.y, fp.z);
                if (c->vbProbes < 2) { ++c->vbProbes; clothFloorProbe(c, fp); }
            }
        }
        if (c->eReady && !c->eDone) clothFirstStepsReport(c);
        if (!c->settleLogged && ++c->sinceCreate >= 60) clothSettleCheck(c);
        if (c->hasPendVel) {
            int m = c->pubN < c->n ? c->pubN : c->n;
            for (int i = 0; i < m; ++i) { if (len(c->pendVel[i] - c->v[i]) > 1e-5f) wake = true; c->v[i] = c->pendVel[i]; }
            c->hasPendVel = false;
        }
        if (c->hasPendDv) {
            for (int i = 0; i < c->n; ++i) { c->v[i] = c->v[i] + c->pendDv[i]; c->pendDv[i] = v3(0, 0, 0); }
            c->hasPendDv = false; wake = true;
        }
        if (c->pendSleep) { c->sleeping = true; c->sleepTimer = 0; for (int i = 0; i < c->n; ++i) c->v[i] = v3(0, 0, 0); }
        if (wake) { c->sleeping = false; c->sleepTimer = 0; }
        c->pendWake = c->pendSleep = false;
        LeaveCriticalSection(&c->cs);
        c->sActive = !c->sleeping && !(c->flags & CLF_STATIC);
        ++c->dFrames; if (c->flags & CLF_STATIC) ++c->dFramesStatic; else if (c->sleeping) ++c->dFramesAsleep;
        if (c->sActive) ++active;
    }
    return active;
}


// cloths that sleep but are not frozen by the game: they need the wake-by-contact test
static int clothWakeable(void* scene) {
    int k = 0;
    for (int i = 0; i < g_nCloths; ++i) { ClothR* c = g_cloths[i]; if (c->scene == scene && !c->sActive && !(c->flags & CLF_STATIC)) ++k; }
    return k;
}

static void clothWakeByContact(void* scene, const World& w) {
    for (int ci = 0; ci < g_nCloths; ++ci) {
        ClothR* c = g_cloths[ci];
        if (c->scene != scene || c->sActive || (c->flags & CLF_STATIC) || (c->flags & CLF_DISABLE_COLLISION)) continue;
        if (c->cls < 0 || c->cls >= w.nCls || c->pubN <= 0) continue;
        const FilterClass& fc = w.cls[c->cls];
        B3 b = c->pubBounds;
        float r = (c->thickness > 0 ? c->thickness : 0.01f) + 0.05f;
        b.mn = b.mn - v3(r, r, r); b.mx = b.mx + v3(r, r, r);
        bool touch = false;
        for (int k = 0; k < fc.nD && !touch; ++k) touch = bOverlap(w.dg[fc.dIdx[k]].aabb, b);
        if (!touch) continue;
        c->sleeping = false; c->sleepTimer = 0; c->sActive = true; ++c->dWakes;
    }
}

static void clothPublishScene(void* scene) {
    for (int ci = 0; ci < g_nCloths; ++ci) {
        ClothR* c = g_cloths[ci];
        if (c->scene != scene) continue;
        if (!c->sActive && !c->topoDirty && !c->firstOut && !c->hasPendAtt) continue;   // asleep: buffers unchanged
        clothPublishOne(c);
    }
}

// ---------------------------------------------------------------------------
// NxCloth vtable (92 slots, 2.8.0.7 order; thiscall emulated with fastcall + dummy edx)
// ---------------------------------------------------------------------------
static volatile LONG g_clothFirst[92];
static void clothCnt(int slot, const void* caller) {
    InterlockedIncrement(&g_clothSlotCalls[slot]);
    if (InterlockedCompareExchange(&g_clothFirst[slot], 1, 0) == 0) {
        // slots the game was seen using are expected; anything else is worth knowing about
        static const unsigned char seen[] = { 2,3,5,7,9,11,13,15,17,18,22,23,28,36,41,42,43,50,52,53,60,62,68,76,77,78,79,80,84,86 };
        for (unsigned i = 0; i < sizeof(seen); ++i) if (seen[i] == slot) return;
        char where[MAX_PATH + 32]; describeAddr(caller, where, sizeof(where));
        LOG("CLOTHREP first call of NxCloth slot %d from %s", slot, where);
    }
}
#define CC(i) clothCnt(i, RETADDR())
#define CGETF(slot, field) static float __fastcall c##slot(void* s, void*) { CC(slot); return C(s)->field; }
#define CSETF(slot, field) static void __fastcall c##slot(void* s, void*, float v) { CC(slot); C(s)->field = v; }

static void* __fastcall c0(void* s, void*, NxU32) { CC(0); return s; }                       // destructor: PhysX never owns ours
static void  __fastcall c1(void* s, void*, void* desc) { CC(1); if (desc) memcpy(desc, C(s)->desc, sizeof(C(s)->desc)); }
static void* __fastcall c2(void* s, void*) { CC(2); return C(s)->mesh; }
CSETF(3, bending) CGETF(4, bending) CSETF(5, stretching) CGETF(6, stretching) CSETF(7, damping) CGETF(8, damping)
CSETF(9, friction) CGETF(10, friction) CSETF(11, pressure) CGETF(12, pressure) CSETF(13, tearFactor) CGETF(14, tearFactor)
CSETF(15, attachTearFactor) CGETF(16, attachTearFactor) CSETF(17, thickness) CGETF(18, thickness)
CGETF(19, density) CGETF(20, relGrid)
static NxU32 __fastcall c21(void* s, void*) { CC(21); return C(s)->iters; }
static void  __fastcall c22(void* s, void*, NxU32 v) { CC(22); C(s)->iters = v ? v : 1; }
static void  __fastcall c23(void* s, void*, B3* b) { CC(23); ClothR* c = C(s); EnterCriticalSection(&c->cs); *b = c->pubBounds; LeaveCriticalSection(&c->cs); }
static void  __fastcall c24(void* s, void*, void*, NxU32) { CC(24); }                       // attachToShape: not used by the game
static void  __fastcall c25(void* s, void*, NxU32) { CC(25); }                              // attachToCollidingShapes
static void  __fastcall c26(void* s, void*, void*) { CC(26); }                              // detachFromShape
static void  __fastcall c27(void* s, void*, NxU32, void*, const V3*, NxU32) { CC(27); }     // attachVertexToShape
static void  __fastcall c28(void* s, void*, NxU32 id, const V3* pos) {                      // attachVertexToGlobalPosition
    CC(28); ClothR* c = C(s);
    if (!pos || (int)id >= c->capV) return;
    EnterCriticalSection(&c->cs); c->pendAtt[id] = 1; c->pendAttPos[id] = *pos; c->hasPendAtt = true; LeaveCriticalSection(&c->cs);
}
static void  __fastcall c29(void* s, void*, NxU32 id) {                                     // freeVertex
    CC(29); ClothR* c = C(s);
    if ((int)id >= c->capV) return;
    EnterCriticalSection(&c->cs); c->pendAtt[id] = 2; c->hasPendAtt = true; LeaveCriticalSection(&c->cs);
}
static void  __fastcall c30(void* s, void*, NxU32, float, float) { CC(30); }                // dominateVertex
static NxU32 __fastcall c31(void* s, void*, NxU32 id) { CC(31); ClothR* c = C(s); return ((int)id < c->n && c->att[id]) ? 1u : 0u; }
static void* __fastcall c32(void* s, void*, NxU32) { CC(32); return 0; }
static V3*   __fastcall c33(void* s, void*, V3* ret, NxU32 id) { CC(33); ClothR* c = C(s); *ret = ((int)id < c->n) ? c->attTgt[id] : v3(0, 0, 0); return ret; }
static void  __fastcall c34(void* s, void*, void*, float, float, float) { CC(34); }        // attachToCore
static bool  __fastcall c35(void* s, void*, NxU32 id, const V3* nrm) {                     // tearVertex
    CC(35); ClothR* c = C(s);
    (void)id; (void)nrm; (void)c;
    return false;
}
// raycast(const NxRay&, NxVec3& hit, NxU32& vertexId): nearest triangle hit on the published mesh
static bool  __fastcall c36(void* s, void*, const float* ray, V3* hitOut, NxU32* vid) {
    CC(36); ClothR* c = C(s);
    if (!ray) return false;
    V3 o = v3(ray[0], ray[1], ray[2]), d = v3(ray[3], ray[4], ray[5]);
    EnterCriticalSection(&c->cs);
    float bestT = BIG; int bestTri = -1;
    for (int t = 0; t < c->nTri; ++t) {
        NxU32 ia = c->pubTri[3 * t], ib = c->pubTri[3 * t + 1], ic = c->pubTri[3 * t + 2];
        if ((int)ia >= c->pubN || (int)ib >= c->pubN || (int)ic >= c->pubN) continue;
        V3 a = c->pubX[ia], e1 = c->pubX[ib] - a, e2 = c->pubX[ic] - a;
        V3 pv = cross(d, e2);
        float det = dot(e1, pv);
        if (fabs_(det) < 1e-12f) continue;
        float inv = 1.0f / det;
        V3 tv = o - a;
        float u = dot(tv, pv) * inv; if (u < 0 || u > 1) continue;
        V3 qv = cross(tv, e1);
        float vv = dot(d, qv) * inv; if (vv < 0 || u + vv > 1) continue;
        float tt = dot(e2, qv) * inv;
        if (tt >= 0 && tt < bestT) { bestT = tt; bestTri = t; }
    }
    bool ok = bestTri >= 0;
    if (ok) {
        V3 hp = o + d * bestT;
        if (hitOut) *hitOut = hp;
        if (vid) {
            NxU32 best = c->pubTri[3 * bestTri]; float bd = BIG;
            for (int k = 0; k < 3; ++k) { NxU32 vi = c->pubTri[3 * bestTri + k]; V3 dd = c->pubX[vi] - hp; float q = dot(dd, dd); if (q < bd) { bd = q; best = vi; } }
            *vid = best;
        }
    }
    LeaveCriticalSection(&c->cs);
    return ok;
}
static void  __fastcall c37(void* s, void*, NxU32 g) { CC(37); C(s)->group = (unsigned short)(g & 31); C(s)->classDirty = true; }
static NxU32 __fastcall c38(void* s, void*) { CC(38); return C(s)->group; }
static void  __fastcall c39(void* s, void*, const GMask* m) { CC(39); if (m) { C(s)->mask = *m; C(s)->classDirty = true; } }
static GMask* __fastcall c40(void* s, void*, GMask* ret) { CC(40); *ret = C(s)->mask; return ret; }
static void  __fastcall c41(void* s, void*, const MeshDataR* m) {                          // setMeshData
    CC(41); ClothR* c = C(s);
    if (!m) return;
    EnterCriticalSection(&c->cs); c->md = *m; clothWriteMeshData(c, true); LeaveCriticalSection(&c->cs);   // fill the new buffers right away
}
static MeshDataR* __fastcall c42(void* s, void*, MeshDataR* ret) { CC(42); *ret = C(s)->md; return ret; }
static void  __fastcall c43(void* s, void*, const B3* b) {                               // setValidBounds
    CC(43); ClothR* c = C(s);
    if (!b) return;
    c->validBounds = *b;
    if (c->vbSetLogs < 4) { ++c->vbSetLogs; LOG("CLOTHREP cloth %p setValidBounds (%.2f %.2f %.2f)-(%.2f %.2f %.2f), size %.1f x %.1f x %.1f m", c,
        b->mn.x, b->mn.y, b->mn.z, b->mx.x, b->mx.y, b->mx.z, b->mx.x - b->mn.x, b->mx.y - b->mn.y, b->mx.z - b->mn.z); }
}
static void  __fastcall c44(void* s, void*, B3* b) { CC(44); if (b) *b = C(s)->validBounds; }
static void  __fastcall c45(void* s, void*, const V3* p, NxU32 id) {                       // setPosition
    CC(45); ClothR* c = C(s);
    if (!p || (int)id >= c->n) return;
    EnterCriticalSection(&c->cs);
    if (c->eSteps < 3) { ++c->eSetPosCalls; float mv = len(*p - c->x[id]); if (mv > c->eSetPosMove) c->eSetPosMove = mv; }
    c->x[id] = *p; c->pubX[id] = *p; c->pendWake = true; LeaveCriticalSection(&c->cs);
}
static void  __fastcall c46(void* s, void*, const void* buf, NxU32 stride) {               // setPositions
    CC(46); ClothR* c = C(s);
    if (!buf) return;
    if (!stride) stride = 12;
    EnterCriticalSection(&c->cs);
    bool early = c->eSteps < 3; if (early) ++c->eSetPosCalls;
    for (int i = 0; i < c->pubN && i < c->n; ++i) {
        const float* f = atc<float>(buf, stride, i); V3 np = v3(f[0], f[1], f[2]);
        if (early) { float mv = len(np - c->x[i]); if (mv > c->eSetPosMove) c->eSetPosMove = mv; }
        c->x[i] = np; c->pubX[i] = c->x[i];
    }
    c->pendWake = true;
    LeaveCriticalSection(&c->cs);
}
static V3*   __fastcall c47(void* s, void*, V3* ret, NxU32 id) { CC(47); ClothR* c = C(s); EnterCriticalSection(&c->cs); *ret = (int)id < c->pubN ? c->pubX[id] : v3(0, 0, 0); LeaveCriticalSection(&c->cs); return ret; }
static void  __fastcall c48(void* s, void*, void* buf, NxU32 stride) {                     // getPositions
    CC(48); ClothR* c = C(s);
    if (!buf) return;
    if (!stride) stride = 12;
    EnterCriticalSection(&c->cs);
    for (int i = 0; i < c->pubN; ++i) { float* f = at<float>(buf, stride, i); f[0] = c->pubX[i].x; f[1] = c->pubX[i].y; f[2] = c->pubX[i].z; }
    LeaveCriticalSection(&c->cs);
}
static void  __fastcall c49(void* s, void*, const V3* v, NxU32 id) {                       // setVelocity
    CC(49); ClothR* c = C(s);
    if (!v || (int)id >= c->pubN) return;
    EnterCriticalSection(&c->cs);
    if (!c->hasPendVel) { memcpy(c->pendVel, c->pubV, sizeof(V3) * c->pubN); c->hasPendVel = true; }
    c->pendVel[id] = *v;
    if (c->eSteps < 3) { ++c->eSetVelCalls; float q = len(*v); if (q > c->eSetVelMax) c->eSetVelMax = q; }
    LeaveCriticalSection(&c->cs);
}
static void  __fastcall c50(void* s, void*, const void* buf, NxU32 stride) {               // setVelocities
    CC(50); ClothR* c = C(s);
    if (!buf) return;
    if (!stride) stride = 12;
    EnterCriticalSection(&c->cs);
    bool early = c->eSteps < 3; if (early) ++c->eSetVelCalls;
    for (int i = 0; i < c->pubN; ++i) {
        const float* f = atc<float>(buf, stride, i); c->pendVel[i] = v3(f[0], f[1], f[2]);
        if (early) { float q = len(c->pendVel[i]); if (q > c->eSetVelMax) c->eSetVelMax = q; }
    }
    c->hasPendVel = true;
    LeaveCriticalSection(&c->cs);
}
static V3*   __fastcall c51(void* s, void*, V3* ret, NxU32 id) { CC(51); ClothR* c = C(s); EnterCriticalSection(&c->cs); *ret = (int)id < c->pubN ? c->pubV[id] : v3(0, 0, 0); LeaveCriticalSection(&c->cs); return ret; }
static void  __fastcall c52(void* s, void*, void* buf, NxU32 stride) {                     // getVelocities
    CC(52); ClothR* c = C(s);
    if (!buf) return;
    if (!stride) stride = 12;
    EnterCriticalSection(&c->cs);
    for (int i = 0; i < c->pubN; ++i) { float* f = at<float>(buf, stride, i); f[0] = c->pubV[i].x; f[1] = c->pubV[i].y; f[2] = c->pubV[i].z; }
    LeaveCriticalSection(&c->cs);
}
static NxU32 __fastcall c53(void* s, void*) { CC(53); return (NxU32)C(s)->pubN; }
static NxU32 __fastcall c54(void* s, void*) { CC(54); return 0; }                           // queryShapePointers
static NxU32 __fastcall c55(void* s, void*) { CC(55); return 0; }                           // getStateByteSize
static void  __fastcall c56(void* s, void*, void**, NxU32*) { CC(56); }
static void  __fastcall c57(void* s, void*, void**, NxU32) { CC(57); }
static void  __fastcall c58(void* s, void*, void*, bool) { CC(58); }
static void  __fastcall c59(void* s, void*, void*) { CC(59); }
CSETF(60, collResp) CGETF(61, collResp) CSETF(62, attachResp) CGETF(63, attachResp)
CSETF(64, fromFluid) CGETF(65, fromFluid) CSETF(66, toFluid) CGETF(67, toFluid)
static void  __fastcall c68(void* s, void*, float x, float y, float z) { CC(68); C(s)->extAcc = v3(x, y, z); }
static V3*   __fastcall c69(void* s, void*, V3* ret) { CC(69); *ret = C(s)->extAcc; return ret; }
CSETF(70, minAdhere) CGETF(71, minAdhere)
static void  __fastcall c72(void* s, void*, float x, float y, float z) { CC(72); C(s)->wind = v3(x, y, z); }
static V3*   __fastcall c73(void* s, void*, V3* ret) { CC(73); *ret = C(s)->wind; return ret; }
static bool  __fastcall c74(void* s, void*) { CC(74); return C(s)->pubSleeping; }
CGETF(75, sleepVel) CSETF(76, sleepVel)
static void  __fastcall c77(void* s, void*, float) { CC(77); ClothR* c = C(s); EnterCriticalSection(&c->cs); c->pendWake = true; c->pendSleep = false; LeaveCriticalSection(&c->cs); }
static void  __fastcall c78(void* s, void*) { CC(78); ClothR* c = C(s); EnterCriticalSection(&c->cs); c->pendSleep = true; c->pendWake = false; LeaveCriticalSection(&c->cs); }
static void  __fastcall c79(void* s, void*, NxU32 f) {                                     // setFlags
    CC(79); ClothR* c = C(s);
    NxU32 nf = (f & ~CLF_HARDWARE) | (c->flags & CLF_HARDWARE);
    bool bendChange = ((nf ^ c->flags) & CLF_BENDING) != 0;
    jobWait();                                            // topology arrays may be rebuilt below
    if (nf != c->flags && c->dFlagLogs < 40) {
        ++c->dFlagLogs;
        char where[MAX_PATH + 32]; describeAddr(RETADDR(), where, sizeof(where));
        LOG("CLOTHREP cloth %p setFlags 0x%x -> 0x%x (%s) from %s", c, c->flags, nf,
            ((nf ^ c->flags) & CLF_STATIC) ? ((nf & CLF_STATIC) ? "game FROZE the cloth" : "game UNFROZE the cloth") : "other flags", where);
    }
    if ((c->flags & CLF_STATIC) && !(nf & CLF_STATIC)) c->freshStep = true;   // unfrozen: shapes may have moved into it meanwhile
    c->flags = nf;
    if (bendChange) clothRebuildEdges(c);
}
static NxU32 __fastcall c80(void* s, void*) { CC(80); return C(s)->flags; }
static void  __fastcall c81(void* s, void*, const char* n) { CC(81); C(s)->name = n; }
static void clothQueueDv(ClothR* c, NxU32 id, V3 f, NxU32 mode, float scale) {
    float m = c->pubMass[id] > 1e-6f ? c->pubMass[id] : 1e-6f;
    V3 dv;
    switch (mode) {
    case FM_IMPULSE: dv = f * (scale / m); break;
    case FM_VELOCITY_CHANGE: dv = f * scale; break;
    case FM_ACCELERATION: dv = f * (scale / 60.0f); break;
    default: dv = f * (scale / (m * 60.0f)); break;         // FORCE (and smooth variants): one 60 Hz frame
    }
    c->pendDv[id] = c->pendDv[id] + dv;
    c->hasPendDv = true;
}
static void  __fastcall c82(void* s, void*, const V3* f, NxU32 id, NxU32 mode) {           // addForceAtVertex
    CC(82); ClothR* c = C(s);
    if (!f || (int)id >= c->pubN) return;
    EnterCriticalSection(&c->cs); clothQueueDv(c, id, *f, mode, 1.0f); LeaveCriticalSection(&c->cs);
}
// addForceAtPos / addDirectedForceAtPos, as PhysXCore 2.8.0.7 does it (NfCloth, RVA 0x193140): the free vertices within
// 'radius' get weight w = 1 - d^2/r^2; the magnitude is shared by the region: for FORCE and IMPULSE it is divided by the
// TOTAL mass of those vertices (not each vertex's own mass), VELOCITY_CHANGE and ACCELERATION use it as is, FORCE and
// ACCELERATION are then scaled by the time step; each vertex gets dv = direction * value * w. The smooth modes do nothing.
// Direction: the given force's direction (directed) or away from 'pos' (radial).
static void clothRegionImpulse(ClothR* c, const V3& pos, const V3* dirForce, float magnitude, float radius, NxU32 mode) {
    if (radius <= 0 || mode == FM_SMOOTH_IMPULSE || mode == FM_SMOOTH_VELOCITY_CHANGE || mode > FM_ACCELERATION) return;
    float r2 = radius * radius, M = 0; int cnt = 0;
    for (int i = 0; i < c->pubN; ++i) {
        if (c->att[i]) continue;
        V3 d = c->pubX[i] - pos;
        if (dot(d, d) >= r2) continue;
        M += c->pubMass[i]; ++cnt;
    }
    if (!cnt) return;
    float value = magnitude;
    if (mode == FM_FORCE || mode == FM_IMPULSE) value = M > 1e-9f ? magnitude / M : 0;
    if (mode == FM_FORCE || mode == FM_ACCELERATION) value *= c->lastDt > 0 ? c->lastDt : 1.0f / 60.0f;
    V3 dir = dirForce ? norm(*dirForce) : v3(0, 0, 0);
    float maxDv = 0;
    for (int i = 0; i < c->pubN; ++i) {
        if (c->att[i]) continue;
        V3 d = c->pubX[i] - pos; float d2 = dot(d, d);
        if (d2 >= r2) continue;
        float w = 1.0f - d2 / r2;
        V3 dv = (dirForce ? dir : norm(d)) * (value * w);
        c->pendDv[i] = c->pendDv[i] + dv;
        if (value * w > maxDv) maxDv = value * w;
    }
    c->hasPendDv = true;
    ++c->dImpulses; c->dImpulseVerts += cnt;
    if (magnitude > c->dMaxImpulse) c->dMaxImpulse = magnitude;
    if (maxDv > c->dMaxDv) c->dMaxDv = maxDv;
}
static void  __fastcall c83(void* s, void*, const V3* pos, float magnitude, float radius, NxU32 mode) {   // addForceAtPos
    CC(83); ClothR* c = C(s);
    if (!pos) return;
    EnterCriticalSection(&c->cs); clothRegionImpulse(c, *pos, 0, magnitude, radius, mode); LeaveCriticalSection(&c->cs);
}
static void  __fastcall c84(void* s, void*, const V3* pos, const V3* force, float radius, NxU32 mode) {   // addDirectedForceAtPos
    CC(84); ClothR* c = C(s);
    if (!pos || !force) return;
    EnterCriticalSection(&c->cs); clothRegionImpulse(c, *pos, force, len(*force), radius, mode); LeaveCriticalSection(&c->cs);
}
static bool  __fastcall c85(void* s, void*, const B3*, NxU32* nb, const NxU32** idx) { CC(85); if (nb) *nb = 0; if (idx) *idx = 0; return false; }
static void* __fastcall c86(void* s, void*) { CC(86); return C(s)->scene; }
static const char* __fastcall c87(void* s, void*) { CC(87); return C(s)->name; }
static void* __fastcall c88(void* s, void*) { CC(88); return C(s)->compartment; }
static NxU32 __fastcall c89(void* s, void*) { CC(89); return 0; }
static NxU32 __fastcall c90(void* s, void*) { CC(90); return C(s)->ffm; }
static void  __fastcall c91(void* s, void*, NxU32 m) { CC(91); C(s)->ffm = (unsigned short)m; }

static void* g_clothVtbl[92] = {
    (void*)c0, (void*)c1, (void*)c2, (void*)c3, (void*)c4, (void*)c5, (void*)c6, (void*)c7, (void*)c8, (void*)c9,
    (void*)c10, (void*)c11, (void*)c12, (void*)c13, (void*)c14, (void*)c15, (void*)c16, (void*)c17, (void*)c18, (void*)c19,
    (void*)c20, (void*)c21, (void*)c22, (void*)c23, (void*)c24, (void*)c25, (void*)c26, (void*)c27, (void*)c28, (void*)c29,
    (void*)c30, (void*)c31, (void*)c32, (void*)c33, (void*)c34, (void*)c35, (void*)c36, (void*)c37, (void*)c38, (void*)c39,
    (void*)c40, (void*)c41, (void*)c42, (void*)c43, (void*)c44, (void*)c45, (void*)c46, (void*)c47, (void*)c48, (void*)c49,
    (void*)c50, (void*)c51, (void*)c52, (void*)c53, (void*)c54, (void*)c55, (void*)c56, (void*)c57, (void*)c58, (void*)c59,
    (void*)c60, (void*)c61, (void*)c62, (void*)c63, (void*)c64, (void*)c65, (void*)c66, (void*)c67, (void*)c68, (void*)c69,
    (void*)c70, (void*)c71, (void*)c72, (void*)c73, (void*)c74, (void*)c75, (void*)c76, (void*)c77, (void*)c78, (void*)c79,
    (void*)c80, (void*)c81, (void*)c82, (void*)c83, (void*)c84, (void*)c85, (void*)c86, (void*)c87, (void*)c88, (void*)c89,
    (void*)c90, (void*)c91 };

// ---------------------------------------------------------------------------
// creation / release
// ---------------------------------------------------------------------------
typedef bool (__fastcall *FN_meshSave)(void*, void*, void*);

static void clothReportOne(ClothR* c, const char* when);
static void destroyCloth(ClothR* c) {
    DeleteCriticalSection(&c->cs);
    zfree(c->tri); zfree(c->x); zfree(c->v); zfree(c->nrm); zfree(c->rest); zfree(c->invMass); zfree(c->mass); zfree(c->parent);
    zfree(c->vflag); zfree(c->att); zfree(c->attTgt); zfree(c->attPrev); zfree(c->edges); zfree(c->bends);
    zfree(c->pendVel); zfree(c->pendDv); zfree(c->pendAtt); zfree(c->pendAttPos); zfree(c->pubX); zfree(c->pubV); zfree(c->pubNrm); zfree(c->pubMass); zfree(c->pubTri);
    zfree(c->sPred); zfree(c->sHit); zfree(c->sHitN); zfree(c->lraAnchor); zfree(c->lraDist);
    c->magic = 0;
    zfree(c);
}

void clothInit(bool enable) { g_clothEnabled = enable ? 1 : 0; LOG("Cloth replacement %s", enable ? "enabled" : "disabled (PhysX simulates cloth)"); }
bool clothEnabled() { return g_clothEnabled == 1; }

void* clothTryCreate(void* scene, const void* descv) {
    if (g_clothEnabled != 1 || !descv) return 0;
    const unsigned char* d = (const unsigned char*)descv;
    NxU32 flags; memcpy(&flags, d + 0x108, 4);
    void* mesh; memcpy(&mesh, d, 4);
    if ((flags & CLF_HARDWARE) || !mesh) return 0;
    if (g_nCloths >= 64) { LOG("CLOTHREP too many cloths -> left to PhysX"); return 0; }
    // the cooked mesh, via NxClothMesh::saveToDesc (vtable slot 1; fills a 0x30-byte NxClothMeshDesc)
    unsigned char mdsc[0x40]; memset(mdsc, 0, sizeof(mdsc));
    bool okSave = vfn<FN_meshSave>(mesh, 1)(mesh, 0, mdsc);
    NxU32 nv, nt, pst, tst, mfl, mst, fst; const char* pts; const char* tris; const char* masses; const char* vfl;
    memcpy(&nv, mdsc + 0x00, 4); memcpy(&nt, mdsc + 0x04, 4); memcpy(&pst, mdsc + 0x08, 4); memcpy(&tst, mdsc + 0x0C, 4);
    memcpy(&pts, mdsc + 0x10, 4); memcpy(&tris, mdsc + 0x14, 4); memcpy(&mfl, mdsc + 0x18, 4);
    memcpy(&mst, mdsc + 0x1C, 4); memcpy(&fst, mdsc + 0x20, 4); memcpy(&masses, mdsc + 0x24, 4); memcpy(&vfl, mdsc + 0x28, 4);
    bool b16 = (mfl & 2) != 0;            // NX_MF_16_BIT_INDICES
    if (!okSave || nv < 3 || nv > 65535 || nt < 1 || nt > 200000 || pst < 12 || tst < (b16 ? 6u : 12u) || !pts || !tris ||
        !readable(pts, 12) || !readable(tris, 6) || (masses && !readable(masses, 4)) || (vfl && !readable(vfl, 4))) {
        LOG("CLOTHREP cloth mesh %p not usable (save %d, %u vertices, %u triangles, strides %u/%u, flags 0x%x) -> left to PhysX",
            mesh, okSave ? 1 : 0, nv, nt, pst, tst, mfl);
        return 0;
    }
    MeshDataR md; memcpy(&md, d + 0x90, sizeof(md));
    if ((md.pos && !readable(md.pos, 12)) || (md.numV && !readable(md.numV, 4)) || (md.idx && !readable(md.idx, 4)) ||
        (md.numI && !readable(md.numI, 4)) || (md.par && !readable(md.par, 4)) || (md.numP && !readable(md.numP, 4)) ||
        (md.dirty && !readable(md.dirty, 4)) || (md.nrm && !readable(md.nrm, 12))) {
        LOG("CLOTHREP mesh data buffers not readable -> left to PhysX");
        return 0;
    }
    poolInit();
    ClothR* c = (ClothR*)zalloc(sizeof(ClothR));
    c->vtbl = g_clothVtbl; c->magic = CLOTH_MAGIC; c->scene = scene; c->mesh = mesh;
    InitializeCriticalSection(&c->cs);
    memcpy(c->desc, d, sizeof(c->desc));
    memcpy(&c->thickness, d + 0x34, 4); memcpy(&c->density, d + 0x38, 4); memcpy(&c->bending, d + 0x3C, 4);
    memcpy(&c->stretching, d + 0x40, 4); memcpy(&c->damping, d + 0x44, 4); memcpy(&c->friction, d + 0x48, 4);
    memcpy(&c->pressure, d + 0x4C, 4); memcpy(&c->tearFactor, d + 0x50, 4); memcpy(&c->collResp, d + 0x54, 4);
    memcpy(&c->attachResp, d + 0x58, 4); memcpy(&c->attachTearFactor, d + 0x5C, 4); memcpy(&c->toFluid, d + 0x60, 4);
    memcpy(&c->fromFluid, d + 0x64, 4); memcpy(&c->minAdhere, d + 0x68, 4); memcpy(&c->iters, d + 0x6C, 4);
    memcpy(&c->extAcc, d + 0x70, 12); memcpy(&c->wind, d + 0x7C, 12); memcpy(&c->wakeCounter, d + 0x88, 4);
    memcpy(&c->sleepVel, d + 0x8C, 4);
    unsigned short grp; memcpy(&grp, d + 0xD4, 2); c->group = grp & 31;
    memcpy(&c->mask, d + 0xD8, 16); memcpy(&c->ffm, d + 0xE8, 2); memcpy(&c->validBounds, d + 0xEC, 24);
    memcpy(&c->relGrid, d + 0x104, 4); c->flags = flags;
    memcpy(&c->userData, d + 0x10C, 4); memcpy(&c->name, d + 0x110, 4); memcpy(&c->compartment, d + 0x114, 4);
    if (c->density <= 0) c->density = 1.0f;
    if (!c->iters) c->iters = 1;
    c->md = md;
    // capacity: the game's buffers bound how many vertices tearing may create
    int cap = (int)nv;
    if (md.maxV > nv) cap = (int)md.maxV;
    if (cap > 65535) cap = 65535;
    c->capV = cap; c->n = (int)nv; c->nTri = (int)nt;
    c->tri = allocN<NxU32>(3 * nt); c->pubTri = allocN<NxU32>(3 * nt);
    c->x = allocN<V3>(cap); c->v = allocN<V3>(cap); c->nrm = allocN<V3>(cap); c->rest = allocN<V3>(cap);
    c->invMass = allocN<float>(cap); c->mass = allocN<float>(cap); c->parent = allocN<int>(cap); c->vflag = allocN<unsigned char>(cap);
    c->att = allocN<unsigned char>(cap); c->attTgt = allocN<V3>(cap); c->attPrev = allocN<V3>(cap);
    c->pendVel = allocN<V3>(cap); c->pendDv = allocN<V3>(cap); c->pendAtt = allocN<unsigned char>(cap); c->pendAttPos = allocN<V3>(cap);
    c->pubX = allocN<V3>(cap); c->pubV = allocN<V3>(cap); c->pubNrm = allocN<V3>(cap); c->pubMass = allocN<float>(cap);
    c->sPred = allocN<V3>(cap); c->sHit = allocN<unsigned char>(cap); c->sHitN = allocN<V3>(cap);
    c->lraAnchor = allocN<int>(cap); c->lraDist = allocN<float>(cap); c->lraDirty = true; c->lastDt = 1.0f / 60.0f;
    float pose[12]; memcpy(pose, d + 4, 48);
    memcpy(c->pose, pose, 48); c->poseMode = 0; c->poseChecked = false; c->nOrig = (int)nv;
    for (NxU32 i = 0; i < nv; ++i) {
        const float* p = (const float*)(pts + (size_t)pst * i);
        V3 lp = v3(p[0], p[1], p[2]);
        c->rest[i] = lp;
        c->x[i] = poseApply(pose, lp, 0);
        c->parent[i] = (int)i;
        NxU32 vf = vfl ? *(const NxU32*)(vfl + (size_t)(fst ? fst : 4) * i) : (NxU32)CMV_TEARABLE;
        c->vflag[i] = (unsigned char)(((vf & CMV_TEARABLE) ? CMV_TEARABLE : 0) | ((vf & CMV_ATTACHED) ? CMV_ATTACHED : 0));
        if (vf & CMV_ATTACHED) { c->att[i] = 1; c->attTgt[i] = c->x[i]; c->attPrev[i] = c->x[i]; }
    }
    for (NxU32 t = 0; t < nt; ++t) {
        const char* q = tris + (size_t)tst * t;
        for (int k = 0; k < 3; ++k) {
            NxU32 vi = b16 ? ((const unsigned short*)q)[k] : ((const NxU32*)q)[k];
            c->tri[3 * t + k] = vi < nv ? vi : 0;
        }
    }
    clothRebuildEdges(c);
    clothComputeMasses(c);
    if (masses) for (NxU32 i = 0; i < nv; ++i) {
        float m = *(const float*)(masses + (size_t)(mst ? mst : 4) * i);
        if (m > 0) { c->mass[i] = m; c->invMass[i] = c->att[i] ? 0.0f : 1.0f / m; }
    }
    clothComputeNormals(c);
    c->firstOut = true; c->cls = -1; c->classDirty = true; c->freshStep = true;
    c->index = g_nCloths;
    g_cloths[g_nCloths++] = c;
    clothPublishOne(c);
    World* wld = worldFor(scene, true);
    if (wld) wld->staticDirty = wld->staticDirty || wld->nSg == 0;
    { B3 rb; bEmpty(rb); for (NxU32 i = 0; i < nv; ++i) bAdd(rb, c->rest[i]);
      LOG("CLOTHREP cloth %p creation pose: translation (%.2f %.2f %.2f), rest size %.2f m, first frame dt %.4f", c, pose[9], pose[10], pose[11],
          len(rb.mx - rb.mn), g_frameDt);
      const B3& vb = c->validBounds; B3 wb; bEmpty(wb); for (NxU32 i = 0; i < nv; ++i) bAdd(wb, c->x[i]);
      LOG("CLOTHREP cloth %p valid bounds from the game: (%.2f %.2f %.2f)-(%.2f %.2f %.2f), size %.1f x %.1f x %.1f m; the tarp itself spans "
          "(%.2f %.2f %.2f)-(%.2f %.2f %.2f); flag %s", c, vb.mn.x, vb.mn.y, vb.mn.z, vb.mx.x, vb.mx.y, vb.mx.z,
          vb.mx.x - vb.mn.x, vb.mx.y - vb.mn.y, vb.mx.z - vb.mn.z, wb.mn.x, wb.mn.y, wb.mn.z, wb.mx.x, wb.mx.y, wb.mx.z,
          (flags & CLF_VALIDBOUNDS) ? "on" : "off (the game may turn it on later)"); }
    LOG("CLOTHREP created cloth %p (scene %p): %u vertices (room for %d), %u triangles, %d edges, flags 0x%x, iterations %u, "
        "thickness %.3f, tearFactor %.2f, mesh flags 0x%x%s%s, buffers maxV %u maxI %u maxP %u",
        c, scene, nv, cap, nt, c->nEdge, flags, c->iters, c->thickness, c->tearFactor, mfl,
        vfl ? ", vertex flags" : "", masses ? ", vertex masses" : "", md.maxV, md.maxI, md.maxP);
    return c;
}

void clothSetStartRefit(bool on) { g_clothStartRefit = on; }

bool clothIsOurs(void* cloth) {
    for (int i = 0; i < g_nCloths; ++i) if (g_cloths[i] == cloth) return true;
    return false;
}

void clothRelease(void* scene, void* cloth) {
    (void)scene;
    jobWait();
    for (int i = 0; i < g_nCloths; ++i) if (g_cloths[i] == cloth) {
        ClothR* c = g_cloths[i];
        g_cloths[i] = g_cloths[--g_nCloths];
        clothReportOne(c, "at release");
        LOG("CLOTHREP released cloth %p (%d vertices, %d tears)", c, c->n, c->tearsTotal);
        destroyCloth(c);
        return;
    }
}

static void clothReleaseScene(void* scene) {
    for (int i = 0; i < g_nCloths;) {
        if (g_cloths[i]->scene == scene) { ClothR* c = g_cloths[i]; g_cloths[i] = g_cloths[--g_nCloths]; clothReportOne(c, "at scene release"); destroyCloth(c); }
        else ++i;
    }
}

static void clothReportOne(ClothR* c, const char* when) {
    LOG("CLOTHREP cloth %p %s: %d/%d vertices, %d pinned-reachable, %d tears (%d total), max stretch %.2f, max speed %.1f m/s, "
        "%d impulses (max %.2f, %.1f vertices each, max dv %.1f m/s), game consumed %d of %d mesh updates, step %.2f ms",
        c, when, c->n, c->capV, c->lraCount, c->dTears, c->tearsTotal, c->dMaxStretch, c->dMaxSpeed, c->dImpulses, c->dMaxImpulse,
        c->dImpulses ? (double)c->dImpulseVerts / c->dImpulses : 0.0, c->dMaxDv, c->dPublishes - c->dUnconsumed, c->dPublishes, c->lastMs);
    if (g_diag && (c->flags & CLF_VALIDBOUNDS))
        LOG("CLOTHREP   valid bounds: %d vertices outside now, at most %d, farthest %.2f m out (report only)", c->vbOutNow, c->vbOutMax, c->vbFarthest);
    c->vbOutMax = 0; c->vbFarthest = 0;
    int pinned = 0; for (int i = 0; i < c->n; ++i) if (c->att[i]) ++pinned;
    V3 p0 = c->pubN > 0 ? c->pubX[0] : v3(0, 0, 0), pm = c->pubN > 0 ? c->pubX[c->pubN / 2] : v3(0, 0, 0);
    LOG("CLOTHREP   frames %d: simulated %d, frozen by game %d, asleep %d, woken by contact %d; flags 0x%x; pinned %d; bounds (%.2f %.2f %.2f)-(%.2f %.2f %.2f); "
        "v0 (%.3f %.3f %.3f) vmid (%.3f %.3f %.3f)", c->dFrames, c->dSteps, c->dFramesStatic, c->dFramesAsleep, c->dWakes, c->flags, pinned,
        c->pubBounds.mn.x, c->pubBounds.mn.y, c->pubBounds.mn.z, c->pubBounds.mx.x, c->pubBounds.mx.y, c->pubBounds.mx.z,
        p0.x, p0.y, p0.z, pm.x, pm.y, pm.z);
    // What the game sees: the buffer we write vs. what the game's render data reads (UE3 USkeletalMeshComponent, found by
    // disassembly: +0x364 cloth position TArray (data, num), +0x388 NumClothMeshVerts, +0x324 cloth flags (4 = frozen),
    // +0x34c blend weight, +0x35c the NxCloth). userData is assumed to be the component; checked against +0x35c.
    const unsigned char* comp = (const unsigned char*)c->userData;
    if (comp && readable(comp + 0x320, 64) && readable(comp + 0x360, 64)) {
        void* clothPtr = *(void* const*)(comp + 0x35c);
        if (clothPtr == (void*)c) {
            const float* gp = *(const float* const*)(comp + 0x364);
            float g0[3] = { 0, 0, 0 };
            if (gp && readable(gp, 12)) { g0[0] = gp[0]; g0[1] = gp[1]; g0[2] = gp[2]; }
            LOG("CLOTHREP   game component %p: render buffer %p (%s ours %p), num %d, NumClothMeshVerts %d, flags 0x%x, blend %.2f, "
                "render v0 (%.3f %.3f %.3f)", comp, gp, gp == c->md.pos ? "same as" : "DIFFERENT from", c->md.pos,
                *(const int*)(comp + 0x368), *(const int*)(comp + 0x388), *(const unsigned*)(comp + 0x324), *(const float*)(comp + 0x34c),
                g0[0], g0[1], g0[2]);
        } else LOG("CLOTHREP   userData %p is not the game component we expected (its +0x35c is %p)", comp, clothPtr);
    }
    c->dMaxStretch = c->dMaxSpeed = c->dMaxImpulse = c->dMaxDv = 0;
    c->dImpulses = c->dImpulseVerts = c->dTears = c->dPublishes = c->dUnconsumed = c->dSteps = 0;
    c->dFrames = c->dFramesStatic = c->dFramesAsleep = c->dWakes = 0;
}

// every 600 frames: one line per cloth that simulated since the last report
void clothPeriodic() {
    for (int i = 0; i < g_nCloths; ++i) clothReportOne(g_cloths[i], "last 600 frames");
}

void clothSummary() {
    jobWait();
    int nc = 0, verts = 0, tears = 0, asleep = 0; double ms = 0;
    for (int i = 0; i < g_nCloths; ++i) { ClothR* c = g_cloths[i]; ++nc; verts += c->n; tears += c->tearsTotal; ms += c->lastMs; if (c->sleeping) ++asleep; }
    LOG("CLOTHREP summary: %d cloths (%d asleep), %d vertices, %d tears so far, last step %.2f ms CPU", nc, asleep, verts, tears, ms);
    for (int i = 0; i < 92; ++i) if (g_clothSlotCalls[i]) LOG("CLOTHREP   NxCloth slot %d called %ld times", i, g_clothSlotCalls[i]);
}
