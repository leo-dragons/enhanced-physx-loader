---
name: Bug report
about: Something doesn't work or looks wrong with the loader in Mirror's Edge
labels: bug
---

**What happened**
(What you saw, and where in the game: chapter, room, what you were doing.)

**Your PC**
- GPU and driver version:
- CPU:
- Windows version (or Linux / Proton version):
- Mirror's Edge version (Steam, EA app / Origin, GOG):
- Enhanced PhysX Loader version:
- Other mods in the game's `Binaries` folder (for example a `d3d9.dll` or `dinput8.dll`):

**Logs**
Set `Troubleshoot=1` in `Binaries\EnhancedPhysXLoader.ini`, play until the problem happens, quit through the game's
menu, and attach `EnhancedPhysXLoader.log`, `EnhancedPhysXLoader_frames.csv` and `EnhancedPhysXLoader_profile.txt`
from the `Binaries` folder. Set it back to `0` afterwards.
