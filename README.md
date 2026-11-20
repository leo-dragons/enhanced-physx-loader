# Enhanced PhysX Loader for Mirror's Edge

Play Mirror's Edge (PC) with PhysX effects **on**, at full speed, on AMD and Intel GPUs, and on NVIDIA cards
where 32-bit GPU PhysX no longer works: shattering glass, debris, dust, smoke and steam, flags, tarps and
tearable plastic.

**Research, Direction & Testing:** leo_dragons
**Programming:** Claude (Anthropic)
MIT License

> **For Mirror's Edge (PC) only.** It is built for, and has only been tested with, the PhysX 2.8.0.7 runtime that
> ships with Mirror's Edge. It will not help other PhysX games, and is not meant to be dropped into them.

> Download the latest version from the [Releases](../../releases) page. This page is about the project and its
> source; the README inside the download explains installing and settings.

## The story

This started as a crazy idea backed by years of research. Mirror's Edge was designed with PhysX in mind: glass that
bursts into shards, plastic sheets that tear when you run through them, smoke that hangs in the air. For years the
only way to see it at full speed was an NVIDIA card, and on everything else the effects either crawled or had to
be switched off. Wanting to play it the way it was meant to be seen, on whatever hardware I had, never went away.

The first serious attempt went through ZLUDA, to run NVIDIA's GPU code on other hardware; on my machine it wasn't
worth it, and it was dropped. The approach that worked was different: leave the game and NVIDIA's runtime alone,
and replace only the slow software simulation underneath, at the one point where the game loads PhysX.

The idea, the direction, years of digging into how this game uses PhysX, and every hour of play-testing are mine.
The code was written by Claude (Anthropic), working from that direction and from the game's own PhysX runtime. No
grudge against NVIDIA here: PhysX gave this game something worth waiting for, and this project is about keeping it
alive for everyone.

## Why the game is slow without it

Without NVIDIA hardware PhysX, Mirror's Edge runs its particle effects (glass, dust, smoke, water) and its cloth
through PhysX 2.8.0's software fallback: a single-threaded emulation of the old AGEIA PPU card. Breaking a window
could cost about 100 ms per frame, and shooting tearable plastic could drop the game to single digits.

## What it does

The game loads PhysX through `PhysXLoader.dll`. This project replaces that DLL with one that loads the game's own
PhysX as usual and then takes over the two expensive parts:

- **Fluids** (glass shards, debris, dust, smoke, steam, water): a multithreaded particle and SPH simulation with
  its own collision world.
- **Cloth** (flags, banners, tarps, tearable plastic): a multithreaded cloth solver that follows PhysX's own rules
  for impulses, tearing and attachments, worked out from the game's PhysX runtime.

Rigid bodies and everything else stay with the game's PhysX. The game's program and PhysX files are not modified.

Tested on:

- a Ryzen 7 5700U mini PC (Radeon Vega integrated graphics), played through the whole game;
- an RTX 5080 with a driver that has no 32-bit GPU PhysX, played through the whole game;
- an MSI Claw A1M handheld (Intel Core Ultra 7 155H, Intel Arc graphics).

No physics slowdowns and no crashes from the loader.

Reported working by players:

- AMD Radeon RX 7900 XTX — thanks to [@RRBernal](https://github.com/RRBernal) for the first report from a player
  ([#1](https://github.com/leo-dragons/enhanced-physx-loader/issues/1)).

Tried it on other hardware? Tell us in [Discussions](https://github.com/leo-dragons/enhanced-physx-loader/discussions),
whether it worked or not.

## Install

1. Download `EnhancedPhysXLoader-<version>-Setup.cmd` from [Releases](../../releases).
2. Close the game and double-click it. Choose **1** to install. It finds the game (Steam libraries, EA/Origin, or
   asks you), asks for administrator rights if the game is in Program Files, and offers to turn PhysX on in the
   game's settings file.
3. Start the game. PhysX should show as on in the video options.

The Setup.cmd is a plain text file: open it in Notepad to read exactly what it does. The `.zip` on the same page
has the same contents with a classic `install.bat`. To uninstall, run the Setup.cmd again and choose **2**.

Linux / Proton: not tested yet. The installers are Windows-only; a manual install is described in
`package/README.txt`.

## Feedback and bug reports

- **It works on my hardware / questions / ideas:** [Discussions](https://github.com/leo-dragons/enhanced-physx-loader/discussions).
- **Something is wrong:** open an [issue](https://github.com/leo-dragons/enhanced-physx-loader/issues). The loader can
  write logs that show what went wrong: set `Troubleshoot=1` in `Binaries\EnhancedPhysXLoader.ini`, play until the
  problem happens, quit through the game's menu, and attach `EnhancedPhysXLoader.log`, `EnhancedPhysXLoader_frames.csv`
  and `EnhancedPhysXLoader_profile.txt` from the `Binaries` folder (crash details are written into the `.log`). Set it
  back to `0` afterwards, as the CSV grows large. Please mention your GPU, CPU, driver version and where in the game
  it happened.

## Roadmap

Everything below is a plan, not a promise.

- **AVX2 with SSE2 fallback.** First an experiment: build the same code with AVX2 and compare physics step times
  against the SSE2 build in the heaviest scenes (Ropeburn tarps, glass rooms). If AVX2 is clearly faster, the DLL
  picks the path at startup from the CPU's features, with an INI switch to force SSE2, and the tests run on both.
- **PhysX effects on the Xbox 360 version of Mirror's Edge.** A proof of concept for a modded (RGH) console:
  bring the PC-only cloth and particle effects to the 360, starting with one tarp or one glass effect, using the
  simulation code from this project. Development first in the Xenia emulator, then on real hardware.
- **Possibly Mafia II and Borderlands 2,** as separate projects built on what was learned here (they use their own
  PhysX builds and NVIDIA's APEX on top, so this DLL will not work in them as it is).
- More chapters played through with 1.2.0, and testing on more GPUs and on Linux with Proton.

## Repository layout

| Path | What |
|---|---|
| `src/proxy.cpp` | loader exports and forwarding, INI, logging, PhysX scene hooks, crash reports |
| `src/fluid.cpp`, `fluid.h` | the `NxFluid` replacement: particle contract, collision world, BVHs, thread pool, fluid packets |
| `src/cloth.inc` | the `NxCloth` replacement (included by `fluid.cpp`) |
| `src/clothfast.*`, `src/clothspy.*` | faster PhysX cloth collision when cloth replacement is off; cloth call logger |
| `src/slots_280.h` | verified vtable slots of the game's PhysXCore 2.8.0.7 |
| `src/PhysXLoader.def`, `src/version.rc` | exports and version resource |
| `files/EnhancedPhysXLoader.ini` | default settings, each one explained |
| `installer/` | `install.ps1` (find game, copy, PhysX settings, uninstall) and `credits.ps1` (end-credits entry) |
| `install.bat`, `uninstall.bat` | launchers for the zip package |
| `package/` | the end-user README, license and notice shipped in the download |
| `tests/` | offline tests against a fake PhysX scene |
| `tools/` | Linux build, sysroot setup, Setup.cmd builder, RTTI vtable dumper |
| `KNOWN_ISSUES.md` | problems seen during testing that are not caused by the loader |

## Building

The DLL must be 32-bit. No PhysX SDK headers are needed or used.

**Visual Studio 2022:**
```
cmake -S . -B build -A Win32
cmake --build build --config Release
```

**Linux (clang + lld), how the releases are built:**
```
sh tools/make_sysroot.sh      # once: mingw-w64 headers and import libraries
sh tools/build_linux.sh       # -> build-check/PhysXLoader.dll
```

**Release package:** put the DLL into `files/`, assemble the folder as in the download, then
`python3 tools/make_setup.py <folder> <version> EnhancedPhysXLoader-<version>-Setup.cmd`.

## Tests

```
sh tests/build_and_run.sh
```
Builds `fluid.cpp` unchanged for 32-bit Linux (freestanding, no libc) against a small Windows shim and a fake
PhysX scene, and checks fluids (landing, sliding, push-out, filtering, tunnelling, the particle id and deletion
contract, packet output) and cloth (pinning, tearing, impulses, waking, start-up on a long frame, moving shapes
overlapping a new tarp, resting against level geometry, room-sized enclosing volumes), single- and multi-threaded.

## Safety design

- Hooks are installed only if the loaded `PhysXCore.dll` is exactly 2.8.0.7 and every patched vtable entry points
  into it; otherwise the DLL is a plain pass-through loader.
- Every replacement can be switched off in the INI, and logging costs nothing while off.
- Buffers the game hands over are checked before they are written.

## Legal

This project contains only original code. It does not contain, and must not be distributed together with, any
NVIDIA PhysX binaries, SDK headers or SDK code, or any file from Mirror's Edge. At runtime it loads the PhysX files
that ship with the user's own copy of the game. Interface layouts were determined by inspecting the game's PhysX
2.8.0 runtime and from the publicly documented PhysX 2.8 API, for interoperability.

PhysX is a trademark of NVIDIA Corporation. Mirror's Edge is a trademark of Electronic Arts Inc. This project is
not affiliated with or endorsed by NVIDIA, Electronic Arts or DICE.
