ENHANCED PHYSX LOADER for Mirror's Edge (PC)                        v1.2.0 
by leo_dragons
Research, Direction & Testing: leo_dragons  -  Programming: Claude (Anthropic)
==============================================================================

Play Mirror's Edge with PhysX effects ON - shattering glass, debris, dust,
smoke, water, flags, tarps and tearable plastic - at full speed on AMD and Intel
GPUs (and on NVIDIA cards where GPU PhysX no longer works).

WHY THE GAME IS SLOW WITHOUT IT
  Without NVIDIA hardware PhysX, the game's particle effects (glass, dust,
  smoke, water) and cloth run through PhysX 2.8.0's software fallback: a
  single-threaded emulation of the old AGEIA PPU card that tests every particle
  against every object in a 10 m cube. Breaking a window could cost ~100 ms per
  frame, and shooting tearable plastic could drop the game to single digits.

WHAT THIS DOES
  Replaces the particle simulation and the cloth simulation with modern
  multithreaded ones, inside the game's own PhysX loader. Rigid bodies and
  everything else are still simulated by the game's PhysX. The game's program
  and PhysX files are not modified. The installer changes two text files: it
  adds an "Enhanced PhysX Loader" entry at the end of the game's credits, and,
  if you say yes when it asks, turns the PhysX switches on in your TdEngine.ini
  settings file. Both are backed up first and undone by the uninstaller.

  Tested through the whole game on a Ryzen 7 5700U mini PC (Radeon Vega
  integrated graphics): glass, debris, water (storm drains) and cloth behave
  like PhysX on an NVIDIA card - cloth bends, tears, reacts to shots, the
  player and NPCs - with no physics-bound slowdowns.

WHAT'S NEW IN 1.2.0
  - Steam and smoke look right: they were thin and flickered ("blinking
    cotton balls") because the loader never told the game how its particles
    are grouped for drawing. Now each steam vent is one dense, steady plume.
  - Tarps no longer start out stretched, torn or warped. Three causes fixed:
    the first frame after the game pins a tarp, a room-sized invisible volume
    the tarps hang inside, and pushes out of walls that turned into speed.
  - Torn-off pieces land on the floor and stay there instead of sinking
    through it and falling forever.
  - A tarp resting against scaffolding no longer shreds itself over time.
  - The previous session's log is kept as EnhancedPhysXLoader.prev.log.
  - Single-file installer (EnhancedPhysXLoader-1.2.0-Setup.cmd), and an
    entry for this project at the very end of the game's credits.
  - With Log=1, the log now explains much more about cloth (first steps,
    settle check, what a tarp collides with).
  Tested for 1.2.0: the training area (steam) and the Ropeburn atrium (tarps)
  on a Ryzen 7 5700U. Earlier versions were played through the whole game.

WHAT WAS NEW IN 1.1.13
  - Glass shards and outdoor debris no longer hang in mid air or go missing:
    particles now pass through objects the level marks as "no fluid collision".
  - Cloth is flexible and tearable again (1.1.0 cloth looked frozen), with
    PhysX's own impulse and tearing rules, and wakes when you run into it.
  - The installer can turn the game's PhysX settings on for you (asks first,
    makes a backup).
  - Crash reports in plain language: they say which component failed (for
    example the graphics driver) instead of only raw addresses.
  - One switch for troubleshooting: Troubleshoot=1 in the INI.

REQUIREMENTS
  - Mirror's Edge for PC (Steam version tested; other versions use the same
    PhysX build and should work)
  - The game's original PhysX files. If you ever applied the "delete the PhysX
    DLLs" fix: Steam > Mirror's Edge > Properties > Installed Files > Verify
    integrity of game files.
  - Start the game at least once before installing, so it creates its settings
    file (the installer uses it to turn PhysX on).

INSTALL
  Two downloads with the same contents:
    EnhancedPhysXLoader-1.2.0-Setup.cmd   one file; double-click it and choose
                                          1 (install). It is a plain text file:
                                          open it in Notepad to see what it does.
    EnhancedPhysXLoader-1.2.0.zip         this folder, with install.bat.
  The steps are the same for both.

  1. Zip only: extract the folder anywhere.
  2. Close the game, then run the Setup.cmd (or install.bat). It finds the game (Steam
     libraries, EA/Origin registry, or asks you) and copies the files. If the
     game is in Program Files, Windows will ask for administrator rights.
     Or: install.bat "D:\SteamLibrary\steamapps\common\mirrors edge"
  3. The installer then checks the game's PhysX settings and, if PhysX is off,
     asks: "Turn PhysX on in TdEngine.ini now? [Y/n]". Press Enter for yes.
     It backs the file up first (TdEngine.ini.epxl-backup-<date>, next to it).
  4. It adds an "Enhanced PhysX Loader" block after the game's own end
     credits, in every language (TdGame\Localization\<language>\
     TdGameCredits.*; each file is backed up as <name>.epxl.bak).
  5. Start the game. PhysX should show as on in the video options; leave it on.

  To confirm it's active: set Log=1 in the INI and look for "SDK hooks
  installed" in Binaries\EnhancedPhysXLoader.log.

  Manual install (what install.bat does):
    copy  Binaries\PhysXLocal\PhysXLoader.dll  ->  Binaries\PhysXLoader_orig.dll
    copy  files\PhysXLoader.dll                ->  Binaries\PhysXLoader.dll
    copy  files\EnhancedPhysXLoader.ini        ->  Binaries\EnhancedPhysXLoader.ini
    and in Documents\EA Games\Mirror's Edge\TdGame\Config\TdEngine.ini:
      [Engine.Engine]    bDisablePhysXHardwareSupport=False
      [SystemSettings]   PhysXEnhanced=True

  Updating from an older version: run install.bat again. Your existing
  EnhancedPhysXLoader.ini is kept; new settings use their defaults until you
  add them (compare with files\EnhancedPhysXLoader.ini).

UNINSTALL
  Run the Setup.cmd and choose 2, or double-click uninstall.bat (or delete
  Binaries\PhysXLoader.dll, Binaries\PhysXLoader_orig.dll and
  Binaries\EnhancedPhysXLoader.ini). It offers to restore the TdEngine.ini
  backup (the default is to keep your current game settings) and removes
  the credits entry.

SETTINGS  (Binaries\EnhancedPhysXLoader.ini)
  FluidReplacement=1   0 = use the original (slow) PhysX particle simulation
  ReplaceSPH=1         0 = leave mist/steam (SPH fluids) to PhysX
  FluidThreads=0       worker threads, 0 = automatic (try 2-4 while streaming)
  ClothThread=1        only with [Cloth] Replace=0: PhysX cloth on its own thread
  NativePacketScale=1  speed-up for the original simulation when FluidReplacement=0
  [Cloth] Replace=1    our own cloth solver; 0 = PhysX's cloth (the settings below then apply)
  [Cloth] FastCollision=1  skip impossible cloth collision tests (big win with torn plastic)
  [Cloth] SolverIterations=0 / TearableSolverIterations=0
                       cloth solver iterations (0 = game's 5)
  [Logging] Troubleshoot=0  1 = all diagnostics below on at once
  [Logging] Log=0           1 = write EnhancedPhysXLoader.log
  [Logging] FrameCsv=0      1 = per-frame physics timings
  [Logging] Profile=0       1 = where PhysX spends time in heavy frames
  [Logging] CrashLog=1      short plain-language crash report in the log
  [Logging] CrashVerbose=0  1 = technical details in crash reports

TROUBLESHOOTING
  Reporting a problem:
    1. In Binaries\EnhancedPhysXLoader.ini set Troubleshoot=1.
    2. Play until the problem happens, then quit through the game's menu if
       you can (the profile file is written on exit).
    3. Send these files from the Binaries folder:
         EnhancedPhysXLoader.log
         EnhancedPhysXLoader_frames.csv
         EnhancedPhysXLoader_profile.txt
       and TdGame\Logs\Launch.log from Documents\EA Games\Mirror's Edge.
    4. Set Troubleshoot=0 again (the CSV grows large).

  Reading a crash report: open EnhancedPhysXLoader.log and look for
  "crash report". It says where the fault happened, for example:
      The game hit an invalid memory access in atiumdag.dll.
      What it means: the fault happened inside the AMD graphics driver ...
  "inside the Enhanced PhysX Loader" is the only verdict that points at this
  mod - please report those. If the game kept running after the report, it
  recovered on its own.

  Quick checks:
  - Effects look like PhysX is off (few glass shards, no water): make sure
    PhysX is on in the video options, or run install.bat again and answer yes
    to the TdEngine.ini question.
  - No speed-up: set Log=1 and check EnhancedPhysXLoader.log. "PhysXCore is
    not the game's 2.8.0.7 build" means the original PhysX files are missing:
    verify the game files in Steam.
  - Something looks wrong with particles: set FluidReplacement=0 to compare
    with the original simulation.
  - Cloth hangs, falls or tears oddly: set [Cloth] Replace=0 and tell us where.

KNOWN ISSUES (not caused by this loader)
  These showed up during testing on an AMD Radeon Vega (integrated) with AMD's
  legacy driver, and also happen with PhysX turned off or without the loader.

  1. Freeze after closing the Steam overlay (AMD Direct3D 9 driver)
     After Shift+Tab, closing the overlay and resuming, the game can freeze and
     has to be ended in Task Manager. The crash report shows the fault inside
     atiumdag.dll (AMD's Direct3D 9 driver); physics keeps running normally.
     Workarounds: turn the Steam overlay off for the game, or use DXVK (3).

  2. Blue screens / instant reboots (AMD Direct3D 9 driver, Windows graphics)
     Reproduced with PhysX completely disabled, so they come from the AMD
     Direct3D 9 / Windows graphics path. A program inside the game (like this
     loader) cannot cause a blue screen. What helped on the test machine:
       - reinstalling the AMD driver as "Driver Only" (after DDU),
       - removing hardware monitoring tools that install kernel drivers
         (HWiNFO, CPU-Z/CPUID SDK) while playing,
       - not switching a KVM or plugging/unplugging displays while the game
         runs in exclusive fullscreen.

  3. DXVK on older AMD GPUs (Vega/Polaris legacy driver)
     DXVK (Direct3D 9 over Vulkan) avoids the driver problems above, but:
       - DXVK 3.x and 2.7+ do not start on the legacy AMD driver (missing
         Vulkan features); use DXVK 2.4.1 (32-bit d3d9.dll into Binaries).
       - 2.4.1 stutters, with dips to 0 fps and audio cut-outs, the first time
         effects appear (the driver lacks graphics pipeline libraries). This is
         shader compilation, not physics.
     Crash reports note when a d3d9.dll (DXVK) is next to the game.

  4. Remaining frame dips in heavy scenes are rendering, not physics: the
     profiler measured no frames slowed by PhysX in those sessions.

KNOWN LIMITATIONS
  - Particles and cloth don't push rigid bodies (two-way coupling); the game
    doesn't use it. Cloth doesn't collide with itself.
  - Very dense effects (helicopter dust) can still cost GPU time to draw - that
    part is rendering, not physics.

Not affiliated with or endorsed by NVIDIA, Electronic Arts or DICE.
PhysX is a trademark of NVIDIA Corporation. Mirror's Edge is a trademark of
Electronic Arts Inc. See LICENSE.txt and NOTICE.txt.
