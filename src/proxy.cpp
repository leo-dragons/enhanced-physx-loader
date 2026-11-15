// Enhanced PhysX Loader for Mirror's Edge (PhysX 2.8.0, 32-bit)
// by leo_dragons
//
// Drop-in PhysXLoader.dll. It forwards every loader export to the game's original loader and, when the loaded
// PhysXCore is the exact build shipped with the game (2.8.0.7), replaces PhysX's software fluid simulation
// (glass, dust, smoke, mist) with a multithreaded implementation, and moves software cloth into a PhysX cloth
// compartment so it is simulated on its own thread, in parallel with the rigid bodies.
// Everything else (rigid bodies, cloth solving itself, ...) is still simulated by PhysX.
//
// Files next to this DLL:
//   PhysXLoader_orig.dll       the game's original Binaries\PhysXLocal\PhysXLoader.dll (copied by the installer;
//                              if missing, PhysXLocal\PhysXLoader.dll is used directly)
//   EnhancedPhysXLoader.ini    settings (all optional)
//   EnhancedPhysXLoader.log    only if [Logging] Log=1, or after a crash if CrashLog=1
//
// The game loads "PhysXLoader.dll" from its exe folder (Binaries) before trying PhysXLocal, which is why this
// DLL lives in Binaries.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <tlhelp32.h>
#include "slots_280.h"
#include "fluid.h"
#include "clothfast.h"
#include "clothspy.h"

#if !defined(_M_IX86) && !defined(__i386__)
#error "Must be built as 32-bit (x86). Mirror's Edge is a 32-bit game."
#endif

#define EPXL_VERSION "1.2.0"

typedef unsigned int NxU32;
using namespace slots280;

// ---------------------------------------------------------------------------
// configuration (EnhancedPhysXLoader.ini)
// ---------------------------------------------------------------------------
struct Config {
    int fluidReplacement;   // [General] FluidReplacement   1
    int fluidThreads;       // [General] FluidThreads       0 = auto
    int replaceSph;         // [General] ReplaceSPH         1 (SPH fluids: mist, steam)
    int nativePacketScale;  // [General] NativePacketScale  1 (only when FluidReplacement=0)
    int log;                // [Logging] Log                0
    int frameCsv;           // [Logging] FrameCsv           0
    int crashLog;           // [Logging] CrashLog           1
    int clothThread;        // [General] ClothThread        1 = plain cloth on its own thread, 2 = also tearable, 0 = off
    int clothIters;         // [Cloth] SolverIterations          0 = game's value (5)
    int tearIters;          // [Cloth] TearableSolverIterations  0 = game's value (5)
    int profile;            // [Logging] Profile             0 (1 = sample PhysX during heavy frames)
    int fastCollision;      // [Cloth] FastCollision         1 (skip impossible cloth vertex / static shape pairs)
    int clothSpy;           // [Logging] ClothSpy            0 (1 = log which NxCloth methods the game uses)
    int clothReplace;       // [Cloth] Replace               1 (our cloth simulation instead of PhysX's)
    int crashVerbose;       // [Logging] CrashVerbose        0 (1 = registers, stack and loaded graphics modules in crash reports)
    int troubleshoot;       // [Logging] Troubleshoot        0 (1 = Log, FrameCsv, Profile and CrashVerbose all on)
    NxU32 forceHW;          // [Diag] ForceHWVersion         0 (experiment: report this PhysX hardware version to the game)
    int clothStartRefit;    // [Cloth] StartRefit            1 (fit a cloth placed far from its pins to the pins)
};
static Config  g_cfg = { 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1 };
static int     g_localD3d9 = 0;   // d3d9.dll next to the game (DXVK); checked once at startup, read by the crash report
static wchar_t g_dir[MAX_PATH];        // folder of this DLL, with trailing backslash

static void loadConfig() {
    wchar_t ini[MAX_PATH];
    lstrcpynW(ini, g_dir, MAX_PATH); lstrcatW(ini, L"EnhancedPhysXLoader.ini");
    g_cfg.fluidReplacement  = GetPrivateProfileIntW(L"General", L"FluidReplacement", 1, ini) != 0;
    g_cfg.fluidThreads      = GetPrivateProfileIntW(L"General", L"FluidThreads", 0, ini);
    g_cfg.replaceSph        = GetPrivateProfileIntW(L"General", L"ReplaceSPH", 1, ini) != 0;
    g_cfg.nativePacketScale = GetPrivateProfileIntW(L"General", L"NativePacketScale", 1, ini);
    g_cfg.log               = GetPrivateProfileIntW(L"Logging", L"Log", 0, ini) != 0;
    g_cfg.frameCsv          = GetPrivateProfileIntW(L"Logging", L"FrameCsv", 0, ini) != 0;
    g_cfg.crashLog          = GetPrivateProfileIntW(L"Logging", L"CrashLog", 1, ini) != 0;
    g_cfg.clothThread       = GetPrivateProfileIntW(L"General", L"ClothThread", 1, ini);
    if (g_cfg.clothThread < 0 || g_cfg.clothThread > 2) g_cfg.clothThread = 1;
    g_cfg.profile           = GetPrivateProfileIntW(L"Logging", L"Profile", 0, ini) != 0;
    g_cfg.clothReplace      = GetPrivateProfileIntW(L"Cloth", L"Replace", 1, ini) != 0;
    g_cfg.clothStartRefit   = GetPrivateProfileIntW(L"Cloth", L"StartRefit", 1, ini) != 0;
    g_cfg.clothSpy          = GetPrivateProfileIntW(L"Logging", L"ClothSpy", 0, ini) != 0;
    g_cfg.fastCollision     = GetPrivateProfileIntW(L"Cloth", L"FastCollision", 1, ini) != 0;
    g_cfg.clothIters        = GetPrivateProfileIntW(L"Cloth", L"SolverIterations", 0, ini);
    g_cfg.tearIters         = GetPrivateProfileIntW(L"Cloth", L"TearableSolverIterations", 0, ini);
    if (g_cfg.clothIters < 0 || g_cfg.clothIters > 32) g_cfg.clothIters = 0;
    if (g_cfg.tearIters < 0 || g_cfg.tearIters > 32) g_cfg.tearIters = 0;
    if (g_cfg.fluidThreads < 0 || g_cfg.fluidThreads > 16) g_cfg.fluidThreads = 0;
    int k = g_cfg.nativePacketScale;
    if (k != 1 && k != 2 && k != 4 && k != 8 && k != 16) g_cfg.nativePacketScale = 1;
    g_cfg.crashVerbose      = GetPrivateProfileIntW(L"Logging", L"CrashVerbose", 0, ini) != 0;
    g_cfg.troubleshoot      = GetPrivateProfileIntW(L"Logging", L"Troubleshoot", 0, ini) != 0;
    g_cfg.forceHW           = (NxU32)GetPrivateProfileIntW(L"Diag", L"ForceHWVersion", 0, ini);
    if (g_cfg.troubleshoot) { g_cfg.log = g_cfg.frameCsv = g_cfg.profile = g_cfg.crashVerbose = g_cfg.crashLog = 1; }
}

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------
static CRITICAL_SECTION g_cs;
static HANDLE   g_log = INVALID_HANDLE_VALUE;
static HANDLE   g_csv = INVALID_HANDLE_VALUE;
static double   g_msPerTick = 0.0;
static LONGLONG g_t0 = 0;
static char     g_csvBuf[64 * 1024];
static int      g_csvLen = 0;
static volatile LONG g_heavy = 0;      // last frame waited > 30 ms for PhysX (profiler samples only then)

static LONGLONG now() { LARGE_INTEGER li; QueryPerformanceCounter(&li); return li.QuadPart; }
static double   ms(LONGLONG a, LONGLONG b) { return (double)(b - a) * g_msPerTick; }

static HANDLE openOut(const wchar_t* name, bool append) {
    wchar_t path[MAX_PATH];
    lstrcpynW(path, g_dir, MAX_PATH); lstrcatW(path, name);
    DWORD disp = append ? OPEN_ALWAYS : CREATE_ALWAYS;
    // full sharing: the log can be opened (and even deleted) while the game runs
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE h = CreateFileW(path, GENERIC_WRITE, share, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD n = GetTempPathW(MAX_PATH, path);
        if (n && n + lstrlenW(name) < MAX_PATH) {
            lstrcatW(path, name);
            h = CreateFileW(path, GENERIC_WRITE, share, NULL, disp, FILE_ATTRIBUTE_NORMAL, NULL);
        }
    }
    if (h != INVALID_HANDLE_VALUE && append) SetFilePointer(h, 0, NULL, FILE_END);
    return h;
}

static void writeAll(HANDLE h, const char* s, int n) { DWORD w; if (h != INVALID_HANDLE_VALUE && n > 0) WriteFile(h, s, (DWORD)n, &w, NULL); }

static void plog(const char* fmt, ...) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char buf[2048];
    int n = _snprintf(buf, sizeof(buf), "[%10.3f ms] ", ms(g_t0, now()));
    if (n < 0) n = 0;
    va_list ap; va_start(ap, fmt);
    int m = _vsnprintf(buf + n, sizeof(buf) - n - 3, fmt, ap);
    va_end(ap);
    if (m < 0) m = (int)sizeof(buf) - n - 3;
    n += m; buf[n++] = '\r'; buf[n++] = '\n';
    EnterCriticalSection(&g_cs);
    writeAll(g_log, buf, n);
    LeaveCriticalSection(&g_cs);
}

static void csvLine(const char* fmt, ...) {
    if (g_csv == INVALID_HANDLE_VALUE) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)sizeof(buf) - 1;
    EnterCriticalSection(&g_cs);
    if (g_csvLen + n > (int)sizeof(g_csvBuf)) { writeAll(g_csv, g_csvBuf, g_csvLen); g_csvLen = 0; }
    memcpy(g_csvBuf + g_csvLen, buf, n); g_csvLen += n;
    LeaveCriticalSection(&g_cs);
}
static void csvFlush() {
    if (g_csv == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_cs); writeAll(g_csv, g_csvBuf, g_csvLen); g_csvLen = 0; LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------------------
// original loader
// ---------------------------------------------------------------------------
typedef void* (__cdecl *PFN_CreateSDK)(NxU32, void*, void*, const void*, void*);
typedef void* (__cdecl *PFN_CreateSDKWithID)(NxU32, char*, char*, char*, char*, void*, void*, const void*, void*);
typedef void  (__cdecl *PFN_ReleaseSDK)(void*);
typedef void* (__cdecl *PFN_Void)();
typedef void* (__cdecl *PFN_Cooking)(NxU32);
typedef void* (__cdecl *PFN_CookingWithID)(NxU32, char*, char*, char*, char*);

static struct {
    HMODULE mod;
    PFN_CreateSDK createSDK; PFN_CreateSDKWithID createSDKWithID; PFN_ReleaseSDK releaseSDK;
    PFN_Void getSDK, getAllocator, getFoundation, getUtil;
    PFN_Cooking getCooking; PFN_CookingWithID getCookingWithID;
} g_orig;

static volatile LONG g_initState = 0;

static void initOnce() {
    if (g_initState == 2) return;
    if (InterlockedCompareExchange(&g_initState, 1, 0) != 0) { while (g_initState != 2) Sleep(0); return; }

    loadConfig();
    if (g_cfg.log) {
        // keep the previous run's log (1.1.14): problems noticed only after relaunching still have their evidence
        wchar_t cur[MAX_PATH], prev[MAX_PATH];
        lstrcpynW(cur, g_dir, MAX_PATH); lstrcatW(cur, L"EnhancedPhysXLoader.log");
        lstrcpynW(prev, g_dir, MAX_PATH); lstrcatW(prev, L"EnhancedPhysXLoader.prev.log");
        if (GetFileAttributesW(cur) != INVALID_FILE_ATTRIBUTES) MoveFileExW(cur, prev, MOVEFILE_REPLACE_EXISTING);
        g_log = openOut(L"EnhancedPhysXLoader.log", false);
    }
    if (g_cfg.frameCsv) {
        g_csv = openOut(L"EnhancedPhysXLoader_frames.csv", false);
        csvLine("frame,t_ms,dt_ms,physx_wait_ms,fluids,particles,fluid_prep_ms,fluid_wait_ms,fluid_cpu_ms,collision_tests\r\n");
    }
    wchar_t exe[MAX_PATH] = L"?";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    plog("Enhanced PhysX Loader " EPXL_VERSION " by leo_dragons. host=%ls", exe);
    plog("config: FluidReplacement=%d ReplaceSPH=%d FluidThreads=%d ClothThread=%d NativePacketScale=%d Log=%d FrameCsv=%d CrashLog=%d",
         g_cfg.fluidReplacement, g_cfg.replaceSph, g_cfg.fluidThreads, g_cfg.clothThread, g_cfg.nativePacketScale,
         g_cfg.log, g_cfg.frameCsv, g_cfg.crashLog);
    plog("config: [Cloth] Replace=%d StartRefit=%d FastCollision=%d SolverIterations=%d TearableSolverIterations=%d (0 = game's value); ClothSpy=%d Profile=%d CrashVerbose=%d Troubleshoot=%d",
         g_cfg.clothReplace, g_cfg.clothStartRefit, g_cfg.fastCollision, g_cfg.clothIters, g_cfg.tearIters, g_cfg.clothSpy, g_cfg.profile, g_cfg.crashVerbose, g_cfg.troubleshoot);
    {   // DXVK replaces Direct3D 9 when its d3d9.dll sits next to the game; worth knowing when reading a crash report
        wchar_t d9[MAX_PATH]; lstrcpynW(d9, g_dir, MAX_PATH); lstrcatW(d9, L"d3d9.dll");
        g_localD3d9 = GetFileAttributesW(d9) != INVALID_FILE_ATTRIBUTES;
        plog("graphics: %ls", g_localD3d9 ?L"d3d9.dll found next to the game (DXVK or another Direct3D 9 replacement)" : L"Windows Direct3D 9 (no d3d9.dll next to the game)");
    }

    wchar_t path[MAX_PATH];
    lstrcpynW(path, g_dir, MAX_PATH); lstrcatW(path, L"PhysXLoader_orig.dll");
    g_orig.mod = LoadLibraryW(path);
    if (!g_orig.mod) {
        lstrcpynW(path, g_dir, MAX_PATH); lstrcatW(path, L"PhysXLocal\\PhysXLoader.dll");
        g_orig.mod = LoadLibraryW(path);
    }
    if (!g_orig.mod) {
        plog("FATAL: original PhysXLoader not found (PhysXLoader_orig.dll or PhysXLocal\\PhysXLoader.dll). Run install.bat again.");
    } else {
        plog("original loader: %ls", path);
#define GETP(field, type, name) g_orig.field = (type)GetProcAddress(g_orig.mod, name);
        GETP(createSDK, PFN_CreateSDK, "NxCreatePhysicsSDK")
        GETP(createSDKWithID, PFN_CreateSDKWithID, "NxCreatePhysicsSDKWithID")
        GETP(releaseSDK, PFN_ReleaseSDK, "NxReleasePhysicsSDK")
        GETP(getSDK, PFN_Void, "NxGetPhysicsSDK")
        GETP(getAllocator, PFN_Void, "NxGetPhysicsSDKAllocator")
        GETP(getFoundation, PFN_Void, "NxGetFoundationSDK")
        GETP(getUtil, PFN_Void, "NxGetUtilLib")
        GETP(getCooking, PFN_Cooking, "NxGetCookingLib")
        GETP(getCookingWithID, PFN_CookingWithID, "NxGetCookingLibWithID")
#undef GETP
    }
    InterlockedExchange(&g_initState, 2);
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static HMODULE moduleFromAddress(const void* p) {
    HMODULE m = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)p, &m);
    return m;
}
static bool moduleRange(HMODULE m, ULONG_PTR* lo, ULONG_PTR* hi) {
    if (!m) return false;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)m;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)((const char*)m + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *lo = (ULONG_PTR)m; *hi = (ULONG_PTR)m + nt->OptionalHeader.SizeOfImage;
    return true;
}
static bool fileVersion(const wchar_t* path, unsigned v[4]) {
    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(path, &dummy);
    if (!sz) return false;
    void* buf = HeapAlloc(GetProcessHeap(), 0, sz);
    if (!buf) return false;
    bool ok = false; VS_FIXEDFILEINFO* ffi = NULL; UINT len = 0;
    if (GetFileVersionInfoW(path, 0, sz, buf) && VerQueryValueW(buf, L"\\", (void**)&ffi, &len) && ffi) {
        v[0] = HIWORD(ffi->dwFileVersionMS); v[1] = LOWORD(ffi->dwFileVersionMS);
        v[2] = HIWORD(ffi->dwFileVersionLS); v[3] = LOWORD(ffi->dwFileVersionLS);
        ok = true;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return ok;
}
static bool patchSlot(void** vt, int slot, void* hook, void** orig) {
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = vt[slot]; vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    return true;
}
static ULONG_PTR g_coreLo = 0, g_coreHi = 0;
static bool vtableLooksSane(void** vt, int count) {
    if ((ULONG_PTR)vt < g_coreLo || (ULONG_PTR)vt >= g_coreHi) return false;
    for (int i = 0; i < count; ++i) { ULONG_PTR f = (ULONG_PTR)vt[i]; if (f < g_coreLo || f >= g_coreHi) return false; }
    return true;
}
template <class F> static F vfn(void* obj, int slot) { return (F)((*(void***)obj)[slot]); }

// ---------------------------------------------------------------------------
// crash logger (first-chance vectored handler; logs real faults, then lets the game handle them as usual)
// ---------------------------------------------------------------------------
static volatile LONG g_crashCount = 0;
static HANDLE g_crashFile = INVALID_HANDLE_VALUE;

static void crashWrite(const char* s) {
    HANDLE h = g_log != INVALID_HANDLE_VALUE ? g_log : g_crashFile;
    if (h == INVALID_HANDLE_VALUE) { g_crashFile = openOut(L"EnhancedPhysXLoader.log", true); h = g_crashFile; }
    DWORD w; if (h != INVALID_HANDLE_VALUE) WriteFile(h, s, (DWORD)lstrlenA(s), &w, NULL);
}
static void describeAddr(ULONG_PTR a, char* out, int n) {
    HMODULE m = 0; wchar_t path[MAX_PATH] = L"";
    if (a > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* b = path; for (const wchar_t* q = path; *q; ++q) if (*q == L'\\' || *q == L'/') b = q + 1;
        _snprintf(out, n, "%ls+0x%lx", b, (unsigned long)(a - (ULONG_PTR)m));
    } else {
        _snprintf(out, n, "%08lx", (unsigned long)a);
    }
}
// plain-language crash verdict: which component the fault happened in, in words a player can act on
static bool modIs(const wchar_t* name, const wchar_t* prefix) {   // case-insensitive prefix match
    for (; *prefix; ++name, ++prefix) {
        wchar_t a = *name, b = *prefix;
        if (a >= L'A' && a <= L'Z') a += 32;
        if (b >= L'A' && b <= L'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}
static const char* faultVerdict(const wchar_t* mod) {
    if (!mod || !*mod)                   return "in code that belongs to no loaded module (often a driver or overlay that already unloaded, or corrupted memory). Not identifiable from here; turn on CrashVerbose=1 for details.";
    if (modIs(mod, L"physxloader"))      return "inside the Enhanced PhysX Loader. This one is ours: please report it with this log.";
    if (modIs(mod, L"physxcore") || modIs(mod, L"physxcooking") || modIs(mod, L"nxcooking"))
                                         return "inside NVIDIA PhysX (the game's own PhysX runtime that the loader works with). Please report it with this log.";
    if (modIs(mod, L"atiumd") || modIs(mod, L"aticfx") || modIs(mod, L"amdx") || modIs(mod, L"atidxx") || modIs(mod, L"amd"))
                                         return "inside the AMD graphics driver (Direct3D 9). This is a known AMD driver problem with this game and also happens with PhysX off; see Known issues in the README.";
    if (modIs(mod, L"nvd3dum") || modIs(mod, L"nvwgf") || modIs(mod, L"nvumd"))
                                         return "inside the NVIDIA graphics driver.";
    if (modIs(mod, L"igd") || modIs(mod, L"igc") || modIs(mod, L"ig9") || modIs(mod, L"igx"))
                                         return "inside the Intel graphics driver.";
    if (modIs(mod, L"d3d9.dll"))         return g_localD3d9 ? "inside DXVK (the d3d9.dll next to the game), not in the loader."
                                                            : "inside Microsoft Direct3D 9 (usually reached through the graphics driver).";
    if (modIs(mod, L"dxgi") || modIs(mod, L"d3d11") || modIs(mod, L"vulkan"))
                                         return "inside the graphics stack (DXGI/Direct3D 11/Vulkan; with DXVK this is where it draws).";
    if (modIs(mod, L"mirrorsedge"))      return "inside the game itself (MirrorsEdge.exe).";
    if (modIs(mod, L"gameoverlayrenderer") || modIs(mod, L"rtss") || modIs(mod, L"rivatuner") || modIs(mod, L"discord") || modIs(mod, L"overlay"))
                                         return "inside an overlay (Steam/Discord/RivaTuner or similar). Try with the overlay off.";
    if (modIs(mod, L"ntdll") || modIs(mod, L"kernel") || modIs(mod, L"kernelbase") || modIs(mod, L"ucrtbase") || modIs(mod, L"msvc"))
                                         return "inside a Windows system library; the cause is usually whoever called it (turn on CrashVerbose=1 to see the call stack).";
    return "inside a third-party module (see the name above).";
}
static void moduleBase(ULONG_PTR a, wchar_t* out, int n) {   // file name only, empty if not in a module
    HMODULE m = 0; wchar_t path[MAX_PATH] = L""; out[0] = 0;
    if (a > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* b = path; for (const wchar_t* q = path; *q; ++q) if (*q == L'\\' || *q == L'/') b = q + 1;
        lstrcpynW(out, b, n);
    }
}
static const char* exceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:     return "an invalid memory access";
    case EXCEPTION_ILLEGAL_INSTRUCTION:  return "an illegal instruction";
    case EXCEPTION_STACK_OVERFLOW:       return "a stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:   return "a division by zero";
    case EXCEPTION_PRIV_INSTRUCTION:     return "a privileged instruction";
    case EXCEPTION_IN_PAGE_ERROR:        return "a failed read from disk (in-page error)";
    }
    return "an error";
}
static LONG CALLBACK crashHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_crashCount) > 4) return EXCEPTION_CONTINUE_SEARCH;
    char buf[1024], where[MAX_PATH + 32];
    CONTEXT* c = ep->ContextRecord;
    {   // plain-language report (always)
        wchar_t mod[MAX_PATH];
        moduleBase((ULONG_PTR)ep->ExceptionRecord->ExceptionAddress, mod, MAX_PATH);
        _snprintf(buf, sizeof(buf),
                  "\r\n*** Enhanced PhysX Loader " EPXL_VERSION " crash report\r\n"
                  "    The game hit %s in %ls.\r\n"
                  "    What it means: the fault happened %s\r\n"
                  "    Graphics: %s\r\n"
                  "    If the game kept running after this, it recovered on its own and this can be ignored.%s",
                  exceptionName(code), mod[0] ? mod : L"unknown code", faultVerdict(mod),
                  g_localD3d9 ? "DXVK or another d3d9.dll next to the game" : "Windows Direct3D 9 with the installed driver",
                  g_cfg.crashVerbose ? "" : "\r\n    (Set CrashVerbose=1 under [Logging] in EnhancedPhysXLoader.ini for technical details.)\r\n");
        crashWrite(buf);
    }
    if (!g_cfg.crashVerbose) {
        HANDLE h = g_log != INVALID_HANDLE_VALUE ? g_log : g_crashFile;
        if (h != INVALID_HANDLE_VALUE) FlushFileBuffers(h);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    describeAddr((ULONG_PTR)ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    _snprintf(buf, sizeof(buf), "\r\n    technical: EXCEPTION 0x%08lx at %s (thread %lu)", code, where, GetCurrentThreadId());
    crashWrite(buf);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
        _snprintf(buf, sizeof(buf), " - %s address %08lx", kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
                  (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
        crashWrite(buf);
    }
    _snprintf(buf, sizeof(buf), "\r\n    eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx\r\n    stack:",
              c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    crashWrite(buf);
    int found = 0;
    for (int i = 0; i < 2048 && found < 24; ++i) {
        ULONG_PTR v = 0; SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(c->Esp + 4 * i), &v, 4, &got) || got != 4) break;
        if (v < 0x10000) continue;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)v, &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE ||
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) continue;
        describeAddr(v, where, sizeof(where));
        _snprintf(buf, sizeof(buf), "\r\n      [esp+%03x] %s", i * 4, where);
        crashWrite(buf);
        ++found;
    }
    crashWrite("\r\n    graphics/physics modules loaded:");
    {
        static const wchar_t* const names[] = { L"d3d9.dll", L"dxgi.dll", L"d3d11.dll", L"vulkan-1.dll", L"atiumdag.dll", L"atiumdva.dll",
            L"aticfx32.dll", L"amdxx32.dll", L"nvd3dum.dll", L"igdumdim32.dll", L"igd9dxva32.dll", L"PhysXCore.dll",
            L"PhysXLoader_orig.dll", L"GameOverlayRenderer.dll", L"RTSSHooks.dll" };
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            HMODULE m = GetModuleHandleW(names[i]);
            if (!m) continue;
            wchar_t path[MAX_PATH] = L""; GetModuleFileNameW(m, path, MAX_PATH);
            _snprintf(buf, sizeof(buf), "\r\n      %08lx %ls", (unsigned long)(ULONG_PTR)m, path);
            crashWrite(buf);
        }
    }
    crashWrite("\r\n");
    HANDLE h = g_log != INVALID_HANDLE_VALUE ? g_log : g_crashFile;
    if (h != INVALID_HANDLE_VALUE) FlushFileBuffers(h);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------
// PhysX hooks
// ---------------------------------------------------------------------------
typedef void* (__fastcall *FN_createScene)(void*, void*, const void*);
typedef void  (__fastcall *FN_releaseScene)(void*, void*, void*);
typedef void  (__fastcall *FN_simulate)(void*, void*, float);
typedef bool  (__fastcall *FN_fetchResults)(void*, void*, int, bool, NxU32*);
typedef void* (__fastcall *FN_createObj)(void*, void*, const void*);
typedef void  (__fastcall *FN_releaseObj)(void*, void*, void*);
typedef NxU32 (__fastcall *FN_u32)(void*, void*);
typedef bool  (__fastcall *FN_bool)(void*, void*);

static FN_createScene  o_createScene;
static FN_releaseScene o_releaseScene;
static FN_simulate     o_simulate;
static FN_fetchResults o_fetchResults;
static FN_createObj    o_createFluid, o_createActor;
static FN_releaseObj   o_releaseFluid, o_releaseActor;
static volatile LONG   g_sdkPatched = 0, g_scenePatched = 0;
static NxU32           g_frame = 0;
static float           g_simDt = 0;

static void __fastcall hk_simulate(void* self, void*, float dt) {
    fluidrep::beginStep(self, dt);
    g_simDt = dt;
    o_simulate(self, 0, dt);
}

static bool __fastcall hk_fetchResults(void* self, void*, int status, bool block, NxU32* err) {
    LONGLONG t0 = now();
    bool r = o_fetchResults(self, 0, status, block, err);
    LONGLONG t1 = now();
    if (r && (status & 1)) {             // NX_RIGID_BODY_FINISHED
        fluidrep::endStep(self);
        ++g_frame;
        if (g_csv != INVALID_HANDLE_VALUE) {
            fluidrep::FrameStats fs = fluidrep::lastStats();
            csvLine("%u,%.3f,%.3f,%.3f,%d,%d,%.3f,%.3f,%.3f,%.0f\r\n", g_frame, ms(g_t0, t1), g_simDt * 1000.0, ms(t0, t1),
                    fs.fluids, fs.particles, fs.prepMs, fs.waitMs, fs.stepMs, fs.narrowTests);
        }
        InterlockedExchange(&g_heavy, ms(t0, t1) > 30.0 ? 1 : 0);
        if (g_cfg.log && g_frame % 3600 == 0) {
            fluidrep::logSummary("periodic");
            if (g_cfg.clothReplace) fluidrep::clothSummary();
            if (g_cfg.clothSpy) clothspy::report("periodic");
        }
        if (g_cfg.log && g_frame % 600 == 0) {
            if (g_cfg.clothReplace) fluidrep::clothPeriodic();
            if (g_cfg.fluidReplacement) fluidrep::fluidPeriodic();
            static long lastCalls = 0, lastCulled = 0;
            long c, k; clothfast::stats(&c, &k);
            if (c != lastCalls) plog("cloth collision: %ld vertex/shape pairs in the last 600 frames, %ld skipped (%.1f%%)",
                                     c - lastCalls, k - lastCulled, c > lastCalls ? 100.0 * (k - lastCulled) / (c - lastCalls) : 0.0);
            lastCalls = c; lastCulled = k;
        }
        if (g_frame % 300 == 0) csvFlush();      // ~5 s: a crash can't lose more than that
    }
    return r;
}

// Fluids: our replacement first; PhysX (optionally with scaled packets) for anything we don't handle.
static void* __fastcall hk_createFluid(void* self, void*, const void* desc) {
    if (g_cfg.log) { char where[MAX_PATH + 32]; describeAddr((ULONG_PTR)__builtin_return_address(0), where, sizeof(where)); plog("createFluid called from %s", where); }
    if (fluidrep::enabled()) {
        void* mine = fluidrep::tryCreate(self, desc);
        if (mine) return mine;
    }
    int k = g_cfg.nativePacketScale;
    if (desc && k > 1) {
        // Shrink PhysX's collision packets by k while keeping physical distances and mass unchanged.
        unsigned char* d = (unsigned char*)desc;
        float rppm, rho, krm, ml, cd; NxU32 psm, sim;
        memcpy(&rppm, d + 0x48, 4); memcpy(&rho, d + 0x4C, 4); memcpy(&krm, d + 0x50, 4);
        memcpy(&ml, d + 0x54, 4); memcpy(&cd, d + 0x58, 4); memcpy(&psm, d + 0x5C, 4); memcpy(&sim, d + 0xB4, 4);
        bool plausible = psm >= 4 && psm <= 128 && !(psm & (psm - 1)) && rppm > 0 && rppm < 10000 && krm >= 1 && krm <= 5 && ml > 0 && cd > 0 && rho > 0;
        float limit = (float)psm * krm;
        if (plausible && sim == 2 && ml * k <= limit && cd * k <= limit) {
            float nr = rppm * k, nml = ml * k, ncd = cd * k, nrho = rho * (float)(k * k * k);
            memcpy(d + 0x48, &nr, 4); memcpy(d + 0x4C, &nrho, 4); memcpy(d + 0x54, &nml, 4); memcpy(d + 0x58, &ncd, 4);
            void* r = o_createFluid(self, 0, desc);
            memcpy(d + 0x48, &rppm, 4); memcpy(d + 0x4C, &rho, 4); memcpy(d + 0x54, &ml, 4); memcpy(d + 0x58, &cd, 4);
            if (r) { plog("PhysX fluid created with packets scaled by %d", k); return r; }
        }
    }
    return o_createFluid(self, 0, desc);
}
static void __fastcall hk_releaseFluid(void* self, void*, void* obj) {
    if (fluidrep::isOurs(obj)) { fluidrep::release(self, obj); return; }
    o_releaseFluid(self, 0, obj);
}
static void* __fastcall hk_createActor(void* self, void*, const void* desc) {
    void* a = o_createActor(self, 0, desc);
    if (a) fluidrep::onActorsChanged(self, !vfn<FN_bool>(a, 29)(a, 0));
    return a;
}
static void __fastcall hk_releaseActor(void* self, void*, void* actor) {
    if (actor) fluidrep::onActorsChanged(self, !vfn<FN_bool>(actor, 29)(actor, 0));   // before its shapes are freed
    o_releaseActor(self, 0, actor);
}

// ---------------------------------------------------------------------------
// Cloth: run software cloth on its own thread.
//
// Mirror's Edge creates software cloth without a compartment, so PhysX simulates it inside the primary scene,
// one after the other with the rigid bodies, on the single PhysX simulation thread. PhysX 2.8 can instead put
// cloth in a "compartment" (NX_SCT_CLOTH, device NX_DC_CPU); every compartment has its own simulation thread,
// which simulate() starts together with the primary scene and fetchResults(NX_RIGID_BODY_FINISHED) waits for.
// That is exactly how cloth ran on PhysX hardware, so the game already copes with it: cloth sees the rigid bodies
// through PhysX's own mirroring, and the cloth itself is still PhysX's.
//
// One compartment per scene, created lazily on the first eligible cloth. Its timing mirrors the scene's
// (setTiming hook), so cloth substeps exactly as before. If anything fails, the cloth goes to the primary scene.
// ---------------------------------------------------------------------------
typedef void* (__fastcall *FN_createComp)(void*, void*, const void*);
typedef void  (__fastcall *FN_setTiming)(void*, void*, float, NxU32, int);
typedef void  (__fastcall *FN_getTiming)(void*, void*, float*, NxU32*, int*, NxU32*);
static FN_createObj    o_createCloth;
static FN_setTiming    o_setTiming;
static FN_releaseObj   o_releaseCloth;

struct SceneCloth { void* scene; void* comp; bool failed; int cloths, inComp; };
static SceneCloth g_sc[8];

static SceneCloth* sceneCloth(void* scene, bool create) {
    for (int i = 0; i < 8; ++i) if (g_sc[i].scene == scene) return &g_sc[i];
    if (!create) return NULL;
    for (int i = 0; i < 8; ++i) if (!g_sc[i].scene) { memset(&g_sc[i], 0, sizeof(g_sc[i])); g_sc[i].scene = scene; return &g_sc[i]; }
    return NULL;
}

static void* clothCompartment(void* scene) {
    SceneCloth* sc = sceneCloth(scene, true);
    if (!sc || sc->failed) return NULL;
    if (sc->comp) return sc->comp;
    struct { NxU32 type, deviceCode; float cell; NxU32 power, flags, threadMask; float timeScale; } cd =
        { 2 /*NX_SCT_CLOTH*/, 0xFFFF0000u /*NX_DC_CPU*/, 100.0f, 8, 8 /*NX_CF_INHERIT_SETTINGS*/, 0, 1.0f };
    void* c = vfn<FN_createComp>(scene, SC_createCompartment)(scene, 0, &cd);
    if (!c || !vtableLooksSane(*(void***)c, CO_VTABLE_SIZE)) {
        sc->failed = true;
        plog("cloth: could not create a CPU cloth compartment (%p); cloth stays in the primary scene", c);
        return NULL;
    }
    float ts = 1.0f / 60.0f; NxU32 it = 8, sub = 0; int method = 0;
    vfn<FN_getTiming>(scene, SC_getTiming)(scene, 0, &ts, &it, &method, &sub);
    vfn<FN_setTiming>(c, CO_setTiming)(c, 0, ts, it, method);
    sc->comp = c;
    plog("cloth: compartment %p created for scene %p (timing %.5f x %u, method %d); cloth now runs on its own thread",
         c, scene, ts, it, method);
    return c;
}

static void* __fastcall hk_createCloth(void* self, void*, const void* desc) {
    if (!desc) return o_createCloth(self, 0, desc);
    unsigned char* d = (unsigned char*)desc;
    NxU32 flags, iters; void* comp; float thick, dens, bend, stretch, tear;
    memcpy(&flags, d + CD_flags, 4); memcpy(&comp, d + CD_compartment, 4); memcpy(&iters, d + CD_solverIterations, 4);
    memcpy(&thick, d + CD_thickness, 4); memcpy(&dens, d + CD_density, 4); memcpy(&bend, d + CD_bending, 4);
    memcpy(&stretch, d + CD_stretching, 4); memcpy(&tear, d + CD_tearFactor, 4);
    if (fluidrep::clothEnabled() && !(flags & NX_CLF_HARDWARE)) {
        void* mine = fluidrep::clothTryCreate(self, desc);
        if (mine) return mine;
    }
    SceneCloth* sc = sceneCloth(self, true);
    void* target = NULL;
    // Optional solver-iteration override (the solver is iterated this many times per substep; fewer = cheaper,
    // slightly stretchier cloth). Written into the game's descriptor for the call only, then restored.
    NxU32 useIters = iters;
    int ov = (flags & NX_CLF_TEARABLE) ? g_cfg.tearIters : g_cfg.clothIters;
    if (ov > 0 && iters >= 1) { useIters = (NxU32)ov; memcpy(d + CD_solverIterations, &useIters, 4); }
    // Tearable cloth stays in the primary scene unless ClothThread=2: measured in the game, 5 tearable sheets
    // cost 43-89 ms per frame in a compartment versus 5-11 ms in the primary scene (plain cloth is cheaper there).
    bool eligible = g_cfg.clothThread == 2 || (g_cfg.clothThread == 1 && !(flags & NX_CLF_TEARABLE));
    if (eligible && !comp && !(flags & NX_CLF_HARDWARE)) target = clothCompartment(self);
    void* r = NULL;
    if (target) {
        memcpy(d + CD_compartment, &target, 4);
        r = o_createCloth(self, 0, desc);
        memcpy(d + CD_compartment, &comp, 4);
        if (!r) {
            if (sc) sc->failed = true;
            plog("cloth: creation in the compartment failed; using the primary scene from now on");
        }
    }
    if (!r) { r = o_createCloth(self, 0, desc); target = NULL; }
    memcpy(d + CD_solverIterations, &iters, 4);
    if (r && sc) { ++sc->cloths; if (target) ++sc->inComp; }
    if (r && g_cfg.clothSpy) {
        clothspy::install(r, plog);
        static int dumpedTear = 0, dumpedPlain = 0;
        int& dumped = (flags & NX_CLF_TEARABLE) ? dumpedTear : dumpedPlain;
        if (dumped < 2) {
            ++dumped;
            plog("clothspy: descriptor of cloth %p (%s), clothMesh %p (vtable %p):", r, (flags & NX_CLF_TEARABLE) ? "tearable" : "plain",
                 *(void**)d, *(void**)d ? **(void***)d : 0);
            for (int o = 0; o < 0x118; o += 32) {
                const NxU32* w = (const NxU32*)(d + o);
                plog("  +%03x: %08x %08x %08x %08x %08x %08x %08x %08x", o, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
    }
    if (g_cfg.log)
        plog("cloth %p created%s: flags 0x%x%s, solverIterations %u (game %u), thickness %.3f, density %.3f, bending %.2f, stretching %.2f, tearFactor %.2f",
             r, target ? " in compartment" : (comp ? " in game's compartment" : " in primary scene"), flags,
             (flags & NX_CLF_TEARABLE) ? " (tearable)" : "", useIters, iters, thick, dens, bend, stretch, tear);
    return r;
}
static void __fastcall hk_releaseCloth(void* self, void*, void* cloth) {
    if (fluidrep::clothIsOurs(cloth)) { fluidrep::clothRelease(self, cloth); return; }
    o_releaseCloth(self, 0, cloth);
}
static void __fastcall hk_setTiming(void* self, void*, float maxTs, NxU32 maxIter, int method) {
    o_setTiming(self, 0, maxTs, maxIter, method);
    SceneCloth* sc = sceneCloth(self, false);
    if (sc && sc->comp && method != 2) vfn<FN_setTiming>(sc->comp, CO_setTiming)(sc->comp, 0, maxTs, maxIter, method);
}


// ---------------------------------------------------------------------------
// 1.1.14 diagnostics: why does the game skip some PhysX content (e.g. the training-area steam) with the loader?
// Logs PhysX hardware queries, the game's PhysXEnhanced flag, scene/compartment types and the AGEIA registry keys.
// ---------------------------------------------------------------------------
static bool memReadable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (const char*)p + n <= (const char*)mbi.BaseAddress + mbi.RegionSize;
}
static const char* physxEnhancedText() {
    // MirrorsEdge.exe (Steam build) GSystemSettings.PhysXEnhanced at 0x1ffa8a8; the exe has no relocation at runtime
    HMODULE exe = GetModuleHandleW(NULL);
    if ((ULONG_PTR)exe != 0x400000) return "unknown (exe not at 0x400000)";
    const int* p = (const int*)0x1ffa8a8;
    if (!memReadable(p, 4)) return "unknown (not readable)";
    return *p ? "True" : "False";
}
static void* g_sdk = NULL;
static FN_u32 o_getHWVersion;
static NxU32 __fastcall hk_getHWVersion(void* self, void*) {
    NxU32 real = o_getHWVersion(self, 0);
    return real ? real : g_cfg.forceHW;
}
static void logHardware(const char* when) {
    if (!g_sdk || !g_cfg.log) return;
    NxU32 seen = vfn<FN_u32>(g_sdk, 31)(g_sdk, 0);                  // what the game gets (NpPhysicsSDK slot 31 = getHWVersion)
    NxU32 hw   = o_getHWVersion ? o_getHWVersion(g_sdk, 0) : seen;     // what PhysX itself reports
    NxU32 ppu  = vfn<FN_u32>(g_sdk, 32)(g_sdk, 0);                     // getNbPPUs (slot 32)
    void* mgr  = memReadable((char*)g_sdk + 0xb8, 4) ? *(void**)((char*)g_sdk + 0xb8) : (void*)-1;
    const char* enh = physxEnhancedText();
    plog("diag %s: PhysX getHWVersion=0x%08x (game sees 0x%08x) getNbPPUs=%u hwManager=%p PhysXEnhanced=%s -> enhanced-content gate %s",
         when, hw, seen, ppu, mgr, enh, (seen && lstrcmpA(enh, "True") == 0) ? "OPEN" : "CLOSED");
}
typedef LONG (WINAPI *PFN_RegOpenKeyExW)(HKEY, LPCWSTR, DWORD, REGSAM, HKEY*);
typedef LONG (WINAPI *PFN_RegEnumValueW)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG (WINAPI *PFN_RegCloseKey)(HKEY);
static void logAgeiaRegistry() {
    if (!g_cfg.log) return;
    HMODULE adv = LoadLibraryW(L"advapi32.dll");
    if (!adv) return;
    PFN_RegOpenKeyExW op = (PFN_RegOpenKeyExW)GetProcAddress(adv, "RegOpenKeyExW");
    PFN_RegEnumValueW en = (PFN_RegEnumValueW)GetProcAddress(adv, "RegEnumValueW");
    PFN_RegCloseKey   cl = (PFN_RegCloseKey)GetProcAddress(adv, "RegCloseKey");
    if (!op || !en || !cl) return;
    struct { HKEY root; const char* name; } roots[] = { { HKEY_CURRENT_USER, "HKCU" }, { HKEY_LOCAL_MACHINE, "HKLM" } };
    for (int r = 0; r < 2; ++r) {
        HKEY k;
        if (op(roots[r].root, L"Software\\AGEIA Technologies", 0, KEY_READ, &k) != 0) { plog("diag registry %s\\Software\\AGEIA Technologies: (no key)", roots[r].name); continue; }
        for (DWORD i = 0; i < 64; ++i) {
            wchar_t name[128]; DWORD nl = 128, type = 0, dl = 512; BYTE data[512];
            if (en(k, i, name, &nl, NULL, &type, data, &dl) != 0) break;
            if (type == REG_DWORD && dl >= 4) plog("diag registry %s ...\\%ls = dword %lu", roots[r].name, name, *(DWORD*)data);
            else if ((type == REG_SZ || type == REG_EXPAND_SZ) && dl >= 2) { data[dl < 510 ? dl : 510] = 0; data[(dl < 510 ? dl : 510) + 1] = 0; plog("diag registry %s ...\\%ls = \"%ls\"", roots[r].name, name, (wchar_t*)data); }
            else plog("diag registry %s ...\\%ls = (type %lu, %lu bytes)", roots[r].name, name, type, dl);
        }
        cl(k);
    }
}
static FN_createComp o_createCompartment;
static void* __fastcall hk_createCompartment(void* self, void*, const void* desc) {
    void* c = o_createCompartment(self, 0, desc);
    if (g_cfg.log && desc) {
        NxU32 t[3]; memcpy(t, desc, 8); memcpy(&t[2], (const char*)desc + 16, 4);
        char where[MAX_PATH + 32]; describeAddr((ULONG_PTR)__builtin_return_address(0), where, sizeof(where));
        plog("diag createCompartment(type %u%s, deviceCode 0x%08x, flags 0x%x) from %s -> %p", t[0],
             t[0] == 0 ? " rigid" : t[0] == 1 ? " fluid" : t[0] == 2 ? " cloth" : "", t[1], t[2], where, c);
    }
    return c;
}

static void patchSceneVtable(void* scene) {
    if (InterlockedCompareExchange(&g_scenePatched, 1, 0) != 0) return;
    void** vt = *(void***)scene;
    if (!vtableLooksSane(vt, SC_VTABLE_SIZE)) { plog("scene vtable failed sanity check; fluid replacement off"); return; }
    bool ok = true;
    ok &= patchSlot(vt, SC_simulate,     (void*)hk_simulate,     (void**)&o_simulate);
    ok &= patchSlot(vt, SC_fetchResults, (void*)hk_fetchResults, (void**)&o_fetchResults);
    ok &= patchSlot(vt, SC_createFluid,  (void*)hk_createFluid,  (void**)&o_createFluid);
    ok &= patchSlot(vt, SC_releaseFluid, (void*)hk_releaseFluid, (void**)&o_releaseFluid);
    ok &= patchSlot(vt, 7 /*createActor*/,  (void*)hk_createActor,  (void**)&o_createActor);
    ok &= patchSlot(vt, 8 /*releaseActor*/, (void*)hk_releaseActor, (void**)&o_releaseActor);
    ok &= patchSlot(vt, SC_createCloth,  (void*)hk_createCloth,  (void**)&o_createCloth);
    ok &= patchSlot(vt, SC_releaseCloth, (void*)hk_releaseCloth, (void**)&o_releaseCloth);
    ok &= patchSlot(vt, SC_setTiming,    (void*)hk_setTiming,    (void**)&o_setTiming);
    ok &= patchSlot(vt, SC_createCompartment, (void*)hk_createCompartment, (void**)&o_createCompartment);
    plog("scene hooks %s", ok ? "installed" : "PARTIALLY FAILED");
}
static void* __fastcall hk_createScene(void* self, void*, const void* desc) {
    void* scene = o_createScene(self, 0, desc);
    if (scene) {
        patchSceneVtable(scene);
        plog("scene %p created", scene);
        if (g_cfg.log) {
            NxU32 simType = vfn<FN_u32>(scene, SC_getSimType)(scene, 0), flags = vfn<FN_u32>(scene, SC_getFlags)(scene, 0);
            plog("diag scene %p: simType %u (%s), flags 0x%x", scene, simType, simType == 0 ? "software" : simType == 1 ? "hardware" : "?", flags);
            logHardware("at scene creation");
        }
    }
    return scene;
}
static void __fastcall hk_releaseScene(void* self, void*, void* scene) {
    SceneCloth* sc = sceneCloth(scene, false);
    if (sc) {
        plog("scene %p released (%d cloths, %d of them on the cloth thread)", scene, sc->cloths, sc->inComp);
        sc->scene = NULL;
    } else plog("scene %p released", scene);
    if (g_cfg.log && g_cfg.clothReplace) fluidrep::clothSummary();
    fluidrep::releaseScene(scene);
    csvFlush();
    o_releaseScene(self, 0, scene);
}

// ---------------------------------------------------------------------------
// Optional sampling profiler ([Logging] Profile=1). While the game is waiting more than 30 ms per frame for
// PhysX, a background thread suspends the game's threads about once per millisecond and records where they
// are inside PhysXCore (leaf address + nearest PhysXCore caller from the EBP chain). Written at exit to
// EnhancedPhysXLoader_profile.txt as RVAs; mapped to functions offline. Diagnostics only.
// ---------------------------------------------------------------------------
static const int HASH_SIZE = 1 << 16;
struct HEntry { ULONG key, key2, count; };
static HEntry g_leaf[HASH_SIZE], g_pair[HASH_SIZE];
static volatile LONG g_samplerStop = 0, g_samplesWritten = 0;
static ULONG g_ticks = 0, g_threadSamples = 0, g_coreSamples = 0, g_dropped = 0;
static HANDLE g_samplerThread = NULL;
struct SThread { DWORD tid; HANDLE h; ULONG samples, hits; bool dead; };
static const int MAX_THREADS = 256;
static SThread g_thr[MAX_THREADS];
static int g_nThr = 0;

static inline bool inCore(ULONG_PTR a) { return a >= g_coreLo && a < g_coreHi; }
static void hadd(HEntry* t, ULONG k, ULONG k2) {
    ULONG h = (k * 2654435761u) ^ (k2 * 40503u);
    for (int probe = 0; probe < 64; ++probe) {
        HEntry& e = t[(h + probe) & (HASH_SIZE - 1)];
        if (e.count == 0) { e.key = k; e.key2 = k2; e.count = 1; return; }
        if (e.key == k && e.key2 == k2) { ++e.count; return; }
    }
    ++g_dropped;
}
static void refreshThreads(DWORD pid, DWORD selfTid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == selfTid) continue;
        bool known = false;
        for (int i = 0; i < g_nThr; ++i) if (g_thr[i].tid == te.th32ThreadID) { known = true; break; }
        if (known || g_nThr >= MAX_THREADS) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (h) { SThread& t = g_thr[g_nThr++]; t.tid = te.th32ThreadID; t.h = h; t.samples = t.hits = 0; t.dead = false; }
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}
static void sampleOne(SThread& t) {
    // Nothing between Suspend and Resume may allocate or take locks (the target may hold the heap lock).
    if (SuspendThread(t.h) == (DWORD)-1) { t.dead = true; return; }
    CONTEXT ctx; memset(&ctx, 0, sizeof(ctx)); ctx.ContextFlags = CONTEXT_CONTROL;
    BOOL ok = GetThreadContext(t.h, &ctx);
    ULONG_PTR eip = ok ? ctx.Eip : 0, ebp = ok ? ctx.Ebp : 0, caller = 0;
    if (ok && inCore(eip)) {
        HANDLE me = GetCurrentProcess();
        for (int d = 0; d < 8 && ebp; ++d) {
            ULONG_PTR fr[2]; SIZE_T got = 0;
            if (!ReadProcessMemory(me, (LPCVOID)ebp, fr, sizeof(fr), &got) || got != sizeof(fr)) break;
            if (inCore(fr[1])) { caller = fr[1]; break; }
            if (fr[0] <= ebp) break;
            ebp = fr[0];
        }
    }
    ResumeThread(t.h);
    if (!ok) { t.dead = true; return; }
    ++t.samples; ++g_threadSamples;
    if (inCore(eip)) {
        ++t.hits; ++g_coreSamples;
        ULONG leaf = (ULONG)(eip - g_coreLo);
        hadd(g_leaf, leaf, 0);
        hadd(g_pair, leaf, caller ? (ULONG)(caller - g_coreLo) : 0xFFFFFFFFu);
    }
}
static DWORD WINAPI samplerMain(LPVOID) {
    DWORD pid = GetCurrentProcessId(), selfTid = GetCurrentThreadId();
    LONGLONG lastRefresh = 0;
    while (!g_samplerStop) {
        if (!g_heavy) { Sleep(20); continue; }
        LONGLONG t = now();
        if (lastRefresh == 0 || ms(lastRefresh, t) > 500.0) { refreshThreads(pid, selfTid); lastRefresh = t; }
        ++g_ticks;
        for (int i = 0; i < g_nThr; ++i) {
            SThread& th = g_thr[i];
            if (th.dead) continue;
            if (th.hits == 0 && (g_ticks % 50) != 0) continue;    // threads never seen in PhysX: rarely
            sampleOne(th);
        }
        Sleep(1);
    }
    return 0;
}
static void startSampler() {
    if (!g_cfg.profile || g_samplerThread) return;
    g_samplerThread = CreateThread(NULL, 0, samplerMain, NULL, 0, NULL);
    plog("profiler %s (PhysXCore %p..%p); samples only while PhysX costs > 30 ms per frame",
         g_samplerThread ? "started" : "FAILED to start", (void*)g_coreLo, (void*)g_coreHi);
}
static void writeSamples() {
    if (!g_samplerThread || InterlockedExchange(&g_samplesWritten, 1) != 0) return;
    InterlockedExchange(&g_samplerStop, 1);
    WaitForSingleObject(g_samplerThread, 500);
    HANDLE f = openOut(L"EnhancedPhysXLoader_profile.txt", false);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[256]; int n;
    n = _snprintf(buf, sizeof(buf), "# PhysXCore base %p size 0x%lx\r\n# ticks %lu thread_samples %lu core_samples %lu dropped %lu\r\n",
                  (void*)g_coreLo, (unsigned long)(g_coreHi - g_coreLo), g_ticks, g_threadSamples, g_coreSamples, g_dropped);
    writeAll(f, buf, n);
    for (int i = 0; i < g_nThr; ++i) if (g_thr[i].hits) {
        n = _snprintf(buf, sizeof(buf), "THREAD %lu samples %lu in_core %lu\r\n", g_thr[i].tid, g_thr[i].samples, g_thr[i].hits);
        writeAll(f, buf, n);
    }
    for (int i = 0; i < HASH_SIZE; ++i) if (g_leaf[i].count) {
        n = _snprintf(buf, sizeof(buf), "LEAF %08lx %lu\r\n", g_leaf[i].key, g_leaf[i].count); writeAll(f, buf, n);
    }
    for (int i = 0; i < HASH_SIZE; ++i) if (g_pair[i].count) {
        n = _snprintf(buf, sizeof(buf), "PAIR %08lx %08lx %lu\r\n", g_pair[i].key, g_pair[i].key2, g_pair[i].count); writeAll(f, buf, n);
    }
    CloseHandle(f);
    plog("EnhancedPhysXLoader_profile.txt written: %lu PhysXCore samples", g_coreSamples);
}

static void onSdkCreated(void* sdk) {
    if (!sdk) return;
    static volatile LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) {
        if (g_cfg.crashLog) AddVectoredExceptionHandler(1, crashHandler);
    }
    void** vt = *(void***)sdk;
    HMODULE core = moduleFromAddress(vt);
    wchar_t corePath[MAX_PATH] = L"?";
    if (core) GetModuleFileNameW(core, corePath, MAX_PATH);
    unsigned v[4] = { 0, 0, 0, 0 };
    bool haveVer = core && fileVersion(corePath, v);
    plog("PhysXCore: %ls (version %u.%u.%u.%u)", corePath, v[0], v[1], v[2], v[3]);
    g_sdk = sdk;
    if (g_cfg.log) {
        HMODULE dev = GetModuleHandleW(L"PhysXDevice.dll");
        wchar_t dp[MAX_PATH] = L"(not loaded)"; if (dev) GetModuleFileNameW(dev, dp, MAX_PATH);
        plog("diag PhysXDevice: %ls", dp);
        logAgeiaRegistry();
        logHardware("at SDK creation");
    }
    if (!haveVer || v[0] != VER_MAJOR || v[1] != VER_MINOR || v[2] != VER_BUGFIX || v[3] != VER_BUILD) {
        plog("PhysXCore is not the game's 2.8.0.7 build: running as a plain loader (no fluid replacement). "
             "Restore the original PhysX files (Steam: Verify integrity of game files) to enable it.");
        return;
    }
    if (InterlockedCompareExchange(&g_sdkPatched, 1, 0) != 0) return;
    if (!moduleRange(core, &g_coreLo, &g_coreHi) || !vtableLooksSane(vt, SDK_VTABLE_SIZE)) { plog("SDK vtable failed sanity check"); return; }
    fluidrep::init(plog, g_cfg.fluidReplacement != 0, g_cfg.fluidThreads, g_cfg.replaceSph != 0);
    fluidrep::clothInit(g_cfg.clothReplace != 0);
    fluidrep::clothSetStartRefit(g_cfg.clothStartRefit != 0);
    fluidrep::setDiagnostics(g_cfg.log != 0);   // the cloth consumed-buffer check (VirtualQuery per cloth per frame) runs only when logging
    if (g_cfg.fastCollision) clothfast::install(g_coreLo, plog);
    startSampler();
    if (g_cfg.forceHW) {
        bool hk = patchSlot(vt, 31, (void*)hk_getHWVersion, (void**)&o_getHWVersion);
        plog("diag ForceHWVersion=0x%08x: getHWVersion hook %s (used only when PhysX itself reports no hardware)", g_cfg.forceHW, hk ? "installed" : "FAILED");
    }
    bool ok = patchSlot(vt, SDK_createScene,  (void*)hk_createScene,  (void**)&o_createScene) &&
              patchSlot(vt, SDK_releaseScene, (void*)hk_releaseScene, (void**)&o_releaseScene);
    plog("SDK hooks %s", ok ? "installed" : "FAILED");
}

// ---------------------------------------------------------------------------
// exports (cdecl, as the 2.8.0 loader)
// ---------------------------------------------------------------------------
extern "C" {

void* __cdecl NxCreatePhysicsSDK(NxU32 sdkVersion, void* allocator, void* outputStream, const void* desc, void* errorCode) {
    initOnce();
    if (!g_orig.createSDK) return NULL;
    void* sdk = g_orig.createSDK(sdkVersion, allocator, outputStream, desc, errorCode);
    plog("NxCreatePhysicsSDK(0x%08x) -> %p", sdkVersion, sdk);
    onSdkCreated(sdk);
    return sdk;
}
void* __cdecl NxCreatePhysicsSDKWithID(NxU32 sdkVersion, char* company, char* app, char* appVer, char* user,
                                       void* allocator, void* outputStream, const void* desc, void* errorCode) {
    initOnce();
    if (!g_orig.createSDKWithID) return NULL;
    void* sdk = g_orig.createSDKWithID(sdkVersion, company, app, appVer, user, allocator, outputStream, desc, errorCode);
    onSdkCreated(sdk);
    return sdk;
}
void __cdecl NxReleasePhysicsSDK(void* sdk) {
    initOnce();
    plog("NxReleasePhysicsSDK");
    if (g_cfg.clothSpy) clothspy::report("at exit");
    writeSamples();
    csvFlush();
    if (g_orig.releaseSDK) g_orig.releaseSDK(sdk);
}
void* __cdecl NxGetPhysicsSDK()          { initOnce(); return g_orig.getSDK ? g_orig.getSDK() : NULL; }
void* __cdecl NxGetPhysicsSDKAllocator() { initOnce(); return g_orig.getAllocator ? g_orig.getAllocator() : NULL; }
void* __cdecl NxGetFoundationSDK()       { initOnce(); return g_orig.getFoundation ? g_orig.getFoundation() : NULL; }
void* __cdecl NxGetUtilLib()             { initOnce(); return g_orig.getUtil ? g_orig.getUtil() : NULL; }
void* __cdecl NxGetCookingLib(NxU32 v)   { initOnce(); return g_orig.getCooking ? g_orig.getCooking(v) : NULL; }
void* __cdecl NxGetCookingLibWithID(NxU32 v, char* c, char* a, char* av, char* u) {
    initOnce();
    return g_orig.getCookingWithID ? g_orig.getCookingWithID(v, c, a, av, u) : NULL;
}

} // extern "C"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        g_msPerTick = 1000.0 / (double)f.QuadPart;
        g_t0 = now();
        DWORD n = GetModuleFileNameW(inst, g_dir, MAX_PATH);
        while (n > 0 && g_dir[n - 1] != L'\\' && g_dir[n - 1] != L'/') --n;
        g_dir[n] = 0;
    } else if (reason == DLL_PROCESS_DETACH) {
        csvFlush();
    }
    return TRUE;
}
