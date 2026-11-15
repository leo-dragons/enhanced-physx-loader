// Virtual table slot indices for the PhysXCore.dll shipped with Mirror's Edge
// (Epic build "2.8.0_FC7", file version 2.8.0.7, timestamp 2008-01-30).
//
// These were NOT taken from the 2.8.4 headers. They were verified against the
// game's own PhysXCore.dll via RTTI (NpScene / NpPhysicsSDK vtables) and by
// checking each slot's stack cleanup (ret N) and the error strings it references.
//
// Key difference vs 2.8.4 headers: 2.8.0 has no NxScene::linearOBBSpecificShapeSweep,
// so every NxScene slot from 120 onward is one LOWER than the 2.8.4 header order.
// Using 2.8.4 header order would call the wrong function (e.g. checkResults instead of simulate).
#pragma once

namespace slots280 {

// NpPhysicsSDK primary vtable: 37 entries
enum Sdk {
    SDK_createScene  = 4,   // NxScene* createScene(const NxSceneDesc&)      ret 4, "PhysicsSDK::createScene: desc.isValid() is false!"
    SDK_releaseScene = 5,   // void releaseScene(NxScene&)                    ret 4, "PhysicsSDK::releaseScene: double deletion detected!"
    SDK_VTABLE_SIZE  = 37
};

// NpScene primary vtable: 156 entries
enum Scene {
    SC_getFlags          = 2,
    SC_getSimType        = 3,
    SC_getNbActors       = 63,
    SC_createCompartment = 38,   // NxCompartment* createCompartment(const NxCompartmentDesc&)  ret 4, validates type<=2, deviceCode, timeScale
    SC_setTiming         = 82,   // void setTiming(NxReal maxTimestep, NxU32 maxIter, NxTimeStepMethod)  ret 0xC, rejects NX_TIMESTEP_INHERIT
    SC_getTiming         = 83,   // void getTiming(NxReal&, NxU32&, NxTimeStepMethod&, NxU32* numSubSteps) const  ret 0x10
    SC_getStats2         = 87,   // const NxSceneStats2* getStats2() const
    SC_createFluid       = 126,  // header 127
    SC_releaseFluid      = 127,  // header 128
    SC_getNbFluids       = 128,  // header 129
    SC_createCloth       = 131,  // header 132
    SC_releaseCloth      = 132,  // header 133
    SC_getNbCloths       = 133,  // header 134
    SC_createSoftBody    = 135,  // header 136
    SC_releaseSoftBody   = 136,  // header 137
    SC_getNbSoftBodies   = 137,  // header 138
    SC_simulate          = 140,  // header 141, "Scene::simulate: The elapsed time must be non-negative!"
    SC_checkResults      = 141,  // header 142
    SC_fetchResults      = 142,  // header 143, "Scene::fetchResults: simulate() was not called or aborted"
    SC_VTABLE_SIZE       = 156
};

// NpCompartment vtable: 14 entries (same order as the 2.8.4 NxCompartment header)
enum Compartment {
    CO_setTiming = 6,            // void setTiming(NxReal, NxU32, NxTimeStepMethod)  ret 0xC
    CO_VTABLE_SIZE = 14
};

// NxCompartmentDesc (2.8.0, checked in NpScene::createCompartment): 0x1C bytes
//   +0 type, +4 deviceCode, +8 gridHashCellSize, +C gridHashTablePower, +10 flags, +14 threadMask, +18 timeScale
// NxClothDesc as passed by the game (2.8.0; NpScene::createCloth validates it and copies it field by field
// into an internal descriptor, which is where Scene::createCloth reads flags at +124 / compartment at +130):
//   +0 clothMesh, +4 globalPose (48 bytes), +34 thickness, +38 density, +3C bendingStiffness,
//   +40 stretchingStiffness, +4C pressure, +50 tearFactor, +5C attachmentTearFactor, +6C solverIterations,
//   +108 flags (NX_CLF_HARDWARE = 0x2000), +10C userData, +110 name, +114 compartment.
//   The compartment must be NX_SCT_CLOTH; "Attempt to create HW cloth in SW compartment!" is the only device
//   check, so software cloth in a CPU compartment is valid.
enum ClothDescOff {
    CD_thickness = 0x34, CD_density = 0x38, CD_bending = 0x3C, CD_stretching = 0x40, CD_pressure = 0x4C,
    CD_tearFactor = 0x50, CD_solverIterations = 0x6C, CD_flags = 0x108, CD_compartment = 0x114
};
static const unsigned NX_CLF_HARDWARE = 0x2000, NX_CLF_TEARABLE = 0x1000;

// Only hook a core whose version matches exactly.
static const unsigned VER_MAJOR = 2, VER_MINOR = 8, VER_BUGFIX = 0, VER_BUILD = 7;

} // namespace slots280
