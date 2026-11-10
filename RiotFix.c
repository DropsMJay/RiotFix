/*
 * RiotFix - runtime patches for The Suffering (2004) and The Suffering: Ties That Bind (2005)
 *
 * dinput8.dll proxy (or .asi plugin). On start-up it applies in memory the settings read from RiotFix.ini.
 * No game file is changed. Every address was confirmed by reverse engineering (RiotEngineNotes.md, section 9j)
 * and every hook checks the original bytes before changing anything.
 *
 * This file is only the entry point: the dinput8 proxy, the location of RiotFix.ini / RiotFix.log, and the order
 * in which the modules are applied. Each module owns its own per-game address table.
 *
 *   RiotCore         log, settings, game detection, memory helpers
 *   RiotMemory       resource cache budgets
 *   RiotRendering    shadows, level of detail, draw distance
 *   RiotSystem       debug output, registry flushes, DirectPlay check, free disk space
 *   RiotDisplay      game window, video modes, message loops
 *   RiotInput        mouse timing, menu cursor input
 *   RiotTiming       threads, locks, frame timing
 *   RiotDiagnostics  crash log, minidump, memory report
 *
 * Build: see Build.sh
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotMemory.h"
#include "RiotRendering.h"
#include "RiotSystem.h"
#include "RiotDisplay.h"
#include "RiotInput.h"
#include "RiotTiming.h"
#include "RiotDiagnostics.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* ================================================================ dinput8 proxy
   Both games import only DirectInput8Create from dinput8.dll. */
typedef HRESULT (WINAPI *DirectInput8CreateFunctionType)(HINSTANCE, DWORD, const void *, void **, void *);
static DirectInput8CreateFunctionType RealDirectInput8Create;
static HMODULE RealDinput8;

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE Instance, DWORD Version, const void *Riid, void **Out, void *Outer) {
    if (!RealDirectInput8Create) return E_FAIL;
    return RealDirectInput8Create(Instance, Version, Riid, Out, Outer);
}

static void LoadRealDinput8(void) {
    char SystemDinput8[MAX_PATH]; GetSystemDirectoryA(SystemDinput8, MAX_PATH); strcat(SystemDinput8, "\\dinput8.dll");
    RealDinput8 = LoadLibraryA(SystemDinput8);
    if (RealDinput8) RealDirectInput8Create = (DirectInput8CreateFunctionType)GetProcAddress(RealDinput8, "DirectInput8Create");
}

/* ================================================================ Paths
   RiotFix.ini and RiotFix.log live next to the DLL (the exe folder, or the .asi loader's "scripts" folder);
   without an .ini there, the exe folder is tried. */
static void FindPaths(HINSTANCE Instance, char *Folder) {
    GetModuleFileNameA(Instance, Folder, MAX_PATH);
    char *Slash = strrchr(Folder, '\\'); if (Slash) Slash[1] = 0;
    snprintf(IniPath, MAX_PATH, "%sRiotFix.ini", Folder);
    if (GetFileAttributesA(IniPath) == INVALID_FILE_ATTRIBUTES) {
        char ExeFolder[MAX_PATH]; GetModuleFileNameA(NULL, ExeFolder, MAX_PATH);
        Slash = strrchr(ExeFolder, '\\'); if (Slash) Slash[1] = 0;
        char Alternative[MAX_PATH]; snprintf(Alternative, MAX_PATH, "%sRiotFix.ini", ExeFolder);
        if (GetFileAttributesA(Alternative) != INVALID_FILE_ATTRIBUTES) strcpy(IniPath, Alternative);
    }
    snprintf(LogPath, MAX_PATH, "%sRiotFix.log", Folder);
}

/* ================================================================ Start-up */
static void Start(HINSTANCE Instance) {
    LogInitialize();
    LoadRealDinput8();

    char Folder[MAX_PATH];
    FindPaths(Instance, Folder);
    /* One named mutex per folder: when another game process with RiotFix is already running from this folder,
       this one writes RiotFix.<process id>.log and leaves the shared log and its rotation alone. */
    int OtherInstance = 0;
    {
        char Name[MAX_PATH + 32] = "Local\\RiotFix_"; DWORD Hash = 2166136261u;
        for (const char *Character = Folder; *Character; Character++) Hash = (Hash ^ (unsigned char)tolower((unsigned char)*Character)) * 16777619u;   /* FNV-1a */
        snprintf(Name + strlen(Name), 16, "%08lx", Hash);
        CreateMutexA(NULL, FALSE, Name);   /* kept open for the life of the process */
        OtherInstance = GetLastError() == ERROR_ALREADY_EXISTS;
    }
    if (OtherInstance) snprintf(LogPath, MAX_PATH, "%sRiotFix.%lu.log", Folder, GetCurrentProcessId());
    else RotateLogs(Folder);
    LogOpen();
    Log("RiotFix " RiotFixVersion "  ini: %s", IniPath);
    if (OtherInstance) Log("Another game process with RiotFix is running from this folder: this one logs here.");
    if (GetFileAttributesA(IniPath) == INVALID_FILE_ATTRIBUTES) Log("RiotFix.ini not found: using the original values.");
    TraceEnabled = IniInt("Debug", "Trace", 0);
    { char Exe[MAX_PATH]; GetModuleFileNameA(NULL, Exe, MAX_PATH); Log("exe: %s", Exe); }
    Log("DLL loaded at %p", Instance);
    RiotDiagnosticsLogStartup();

    CurrentGame = DetectGame();
    if (CurrentGame == GameUnknown) Log("Unknown executable; nothing applied.");
    else {
        Log("Game: %s", GameName(CurrentGame));
        RiotMemoryApply();
        RiotRenderingApply();
        RiotSystemApply();
        RiotDisplayApply();
        RiotInputApply();
        RiotTimingApply();
    }

    RiotDiagnosticsApply();
    LogClose();
}

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID Reserved) {
    (void)Reserved;
    if (Reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(Instance);
        Start(Instance);
    } else if (Reason == DLL_PROCESS_DETACH) {
        RiotSystemReport();
        RiotDiagnosticsLogMemory("at exit");
        Log("Game closed normally.");
    }
    return TRUE;
}
