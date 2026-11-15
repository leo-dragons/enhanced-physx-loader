# Known issues not caused by the loader

Seen while testing on a Ryzen 7 5700U mini PC (Radeon Vega integrated graphics, AMD legacy driver, Windows 11) with
the Steam version of Mirror's Edge. Each was checked against the loader's logs and crash reports.

## Freeze after closing the Steam overlay (AMD Direct3D 9 driver)

- **Symptom:** the game freezes after opening and closing the Steam overlay (Shift+Tab) and has to be killed.
- **Evidence:** the crash report shows a fault inside `atiumdag.dll`, AMD's Direct3D 9 driver, on the render thread.
  No loader or PhysX code was involved, and physics kept running.
- **Workaround:** turn the Steam overlay off for the game, or run it through DXVK (below).

## Blue screens on the AMD Direct3D 9 path

- Several blue screens were reproduced **with the PhysX replacement switched off completely**, so they belong to the
  AMD DX9 / Windows graphics path. A program running inside the game cannot cause a blue screen by itself.
- They happened after a monitor was disconnected and reconnected (KVM switch) while the game ran in exclusive
  fullscreen, with hardware-monitoring drivers installed.
- After reinstalling the AMD driver as "Driver Only", removing the monitoring tools and not switching displays
  during play, they stopped.
- **Advice:** don't hot-plug or switch displays while the game is fullscreen; prefer the AMD "Driver Only" install.

## DXVK on AMD Vega / Polaris legacy drivers

- DXVK 3.x and 2.7 or newer don't start (the legacy AMD Vulkan driver lacks features they require). DXVK 2.4.1 works.
- With 2.4.1 there is stutter the first time each effect appears, because the driver lacks graphics pipeline
  libraries; it is shader compilation, not physics (the loader's profiler showed no slow physics frames).

## Not a problem: weak weapons don't tear tarps

The game gives each weapon its own impact strength. The weakest never stretches the plastic past the game's tear
limit, however many shots hit it; stronger weapons tear it. The loader applies impacts and tearing with PhysX's own
rules, so this is the game's tuning.
