// Replacement for PhysX 2.8.0 NxFluid (NX_F_NO_PARTICLE_INTERACTION only), as used by Mirror's Edge.
#pragma once

typedef unsigned int NxU32;

namespace fluidrep {

typedef void (*LogFn)(const char* fmt, ...);

void  init(LogFn log, bool enable, int threads, bool replaceSph);   // threads: 0 = automatic
bool  enabled();

// Returns our NxFluid* (vtable-compatible with the game's 2.8.0 NxFluid), or NULL to let PhysX create it.
void* tryCreate(void* scene, const void* desc);
bool  isOurs(void* fluid);
void  release(void* scene, void* fluid);

void  beginStep(void* scene, float dt);   // game thread, before PhysX NxScene::simulate
void  endStep(void* scene);               // game thread, after NxScene::fetchResults(RIGID_BODY_FINISHED) returned true
void  releaseScene(void* scene);
void  onActorsChanged(void* scene, bool staticActor);   // call from NxScene::createActor / releaseActor hooks

struct FrameStats {
    int    fluids, particles;
    int    staticGeoms, dynamicGeoms;
    double prepMs, waitMs, stepMs;     // prep: game-thread work in beginStep; wait: time endStep blocked; step: worker CPU time
    double narrowTests;                // narrow-phase shape tests this frame
};
FrameStats lastStats();
void  logSummary(const char* when);

// Cloth replacement (NxCloth, 2.8.0.7 ABI); shares the collision world and thread pool with the fluids.
void  clothInit(bool enable);
bool  clothEnabled();
void* clothTryCreate(void* scene, const void* desc);   // NULL: let PhysX create it
bool  clothIsOurs(void* cloth);
void  clothRelease(void* scene, void* cloth);
void  clothSummary();
void  clothSetStartRefit(bool on);   // [Cloth] StartRefit: fit a cloth placed far from its pins to the pins (1.1.14)
void  clothPeriodic();
void  setDiagnostics(bool on);   // per-frame diagnostic counters (on with [Logging] Log=1; default on)
void  fluidPeriodic();

// Collision filtering shared with the census (2.8.0 semantics, incl. Ubisoft SWAP_AND).
struct GMask { NxU32 b[4]; };
bool  groupsMaskPass(const GMask& g0, const GMask& g1, int op0, int op1, int op2, const GMask& k0, const GMask& k1, bool fb);

} // namespace fluidrep
