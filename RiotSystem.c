/*
 * RiotSystem - small fixes around Windows: debug output, registry flushes, DirectPlay check, free disk space
 *
 * Facts (original GOG executables):
 *
 * Frame debug output. The frame function (PIH 0x48C430 / TTB 0x493BE0) ends with OutputDebugStringA("")
 *   (PIH 0x48C7B9 / TTB 0x494092). Only that call is skipped; every other debug message is kept.
 *
 * Registry flushes. After saving the settings (PIH 0x438940 / TTB 0x43C080) the game calls RegFlushKey on
 *   HKEY_CURRENT_USER (PIH 0x4389AF / TTB 0x43C0EF), and on exit (PIH 0x4B14D0 / TTB 0x4B9B00) on HKEY_CLASSES_ROOT,
 *   HKEY_CURRENT_USER and HKEY_LOCAL_MACHINE (loaded at PIH 0x4B1741 / TTB 0x4B9CEC). Windows writes the registry
 *   to disk by itself; a forced flush of whole root keys only blocks the game. These uses return ERROR_SUCCESS.
 *
 * DirectPlay check. A static constructor (PIH 0x586895 / TTB 0x5DCDF5) calls PIH 0x49D8E0 / TTB 0x4A2270, which
 *   loads dpnhpast.dll (DirectPlay) only to decide whether "DirectX 8.1" is installed and, if it loaded, creates
 *   the Direct3D 9 object (PIH [0x6DD7E8], SDK 0x1F / TTB [0x74126C], SDK 0x20). Without DirectPlay (not installed
 *   by default since Windows 8) no Direct3D object is created and the game stops with "DirectX ... is not
 *   installed" (MessageBox in PIH 0x439B20 / TTB 0x43D0A0). The call is redirected to CreateDirect3D, which creates the Direct3D
 *   object the same way, through the executable's own Direct3DCreate9 import, without loading DirectPlay.
 *   The original also writes "DirectX 8.1 detected." to the engine's debug log; that line is not written.
 *
 * Free disk space. Two functions read GetDiskFreeSpaceExA (PIH 0x407214, 0x4310D8 / TTB 0x4088D4, 0x434B78):
 *   the first keeps only the low 32 bits of the free bytes, the second stores the free kilobytes in 32 bits.
 *   With large free space the value wraps (or turns negative) and the game believes the disk is full. These two
 *   uses go through ClampedGetDiskFreeSpaceEx, which caps each result at 2 GB - 1 (the first function already
 *   caps its result at 300,000,000 bytes, so nothing the game checks needs more).
 *
 * Skip intro (as in FusionFix, ThirteenAG, MIT). The movie constructor (PIH 0x486810 / TTB 0x48C660, thiscall, one
 *   argument; called only at PIH 0x485496 / TTB 0x48B1AE) leaves +0x28 at 0 (base constructor PIH 0x484C40 /
 *   TTB 0x48ACA0); bit 0 of +0x28 asks the movie to end, as the game's own skip does. The call is redirected to
 *   MovieCreated, which runs the constructor and, for the first movie of the session (the start-up logos), sets
 *   that bit.
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotSystem.h"

/* ================================================================ Per-game addresses */
typedef struct {
    DWORD DebugOutputSlot, FrameDebugOutput;
    DWORD RegistryFlushSlot, RegistryFlushes[2];
    DWORD DiskSpaceSlot, DiskSpaceUses[2];
    DWORD DirectPlayCheckCall, DirectPlayCheck, Direct3DObject, Direct3DSlot; UINT Direct3DVersion;
    DWORD MovieConstructorCall, MovieConstructor;
} SystemAddresses;

static const SystemAddresses Addresses[GameCount] = {
    [GamePIH] = { 0x58B144, 0x48C7B9,
                  0x58B00C, { 0x4389AF, 0x4B1741 },
                  0x58B174, { 0x407214, 0x4310D8 },
                  0x586895, 0x49D8E0, 0x6DD7E8, 0x58B420, 0x1F,
                  0x485496, 0x486810 },
    [GameTTB] = { 0x5E5168, 0x494092,
                  0x5E500C, { 0x43C0EF, 0x4B9CEC },
                  0x5E5194, { 0x4088D4, 0x434B78 },
                  0x5DCDF5, 0x4A2270, 0x74126C, 0x5E545C, 0x20,
                  0x48B1AE, 0x48C660 },
};
static const SystemAddresses *A;

/* ================================================================ Frame debug output */
static volatile LONG SkippedDebugOutputs;
static int DebugOutputSkipped;
static void WINAPI SkipOutputDebugString(LPCSTR Text) {
    (void)Text;
    InterlockedIncrement(&SkippedDebugOutputs);
}
static void *const SkipOutputDebugStringSlot = (void *)SkipOutputDebugString;

/* ================================================================ Registry flushes */
static volatile LONG SkippedFlushes;
static int FlushesSkipped;
static LONG WINAPI SkipRegFlushKey(HKEY Key) {
    (void)Key;
    InterlockedIncrement(&SkippedFlushes);
    return ERROR_SUCCESS;
}
static void *const SkipRegFlushKeySlot = (void *)SkipRegFlushKey;

/* ================================================================ Free disk space */
typedef BOOL (WINAPI *GetDiskFreeSpaceExFunctionType)(LPCSTR, PULARGE_INTEGER, PULARGE_INTEGER, PULARGE_INTEGER);
static GetDiskFreeSpaceExFunctionType RealGetDiskFreeSpaceEx;
static volatile LONG ClampedDiskSpaceCalls;
static void Clamp(PULARGE_INTEGER Value) {
    if (Value && Value->QuadPart > 0x7FFFFFFFull) {
        Value->QuadPart = 0x7FFFFFFFull;
        InterlockedIncrement(&ClampedDiskSpaceCalls);
    }
}
static BOOL WINAPI ClampedGetDiskFreeSpaceEx(LPCSTR Directory, PULARGE_INTEGER FreeToCaller, PULARGE_INTEGER Total, PULARGE_INTEGER Free) {
    BOOL Result = RealGetDiskFreeSpaceEx(Directory, FreeToCaller, Total, Free);
    if (Result) { Clamp(FreeToCaller); Clamp(Total); Clamp(Free); }
    return Result;
}
static void *const ClampedGetDiskFreeSpaceExSlot = (void *)ClampedGetDiskFreeSpaceEx;

/* ================================================================ DirectPlay check */
typedef void *(WINAPI *Direct3DCreate9FunctionType)(UINT);
static void *ThisCall CreateDirect3D(void *Self) {
    void **Object = (void **)A->Direct3DObject;
    if (!*Object) {
        Direct3DCreate9FunctionType Create = *(Direct3DCreate9FunctionType *)A->Direct3DSlot;
        *Object = Create(A->Direct3DVersion);
        Log("Direct3D created without the DirectPlay check: %s", *Object ? "ok" : "FAILED (Direct3DCreate9 returned null)");
    }
    return Self;
}

/* ================================================================ Skip intro */
typedef void *(ThisCall *MovieConstructorFunctionType)(void *, DWORD);
static MovieConstructorFunctionType OriginalMovieConstructor;
static volatile LONG MoviesCreated;
static void *ThisCall MovieCreated(void *Self, DWORD Argument) {
    void *Result = OriginalMovieConstructor(Self, Argument);
    if (InterlockedIncrement(&MoviesCreated) == 1) {
        *(DWORD *)((char *)Self + 0x28) |= 1;
        Log("Start-up movie skipped");
    }
    return Result;
}

/* ================================================================ Apply */
void RiotSystemApply(void) {
    if (CurrentGame == GameUnknown) return;
    A = &Addresses[CurrentGame];

    if (IniInt("System", "SkipFrameDebugOutput", 1)) {
        DebugOutputSkipped = RepointImportUse(A->FrameDebugOutput, A->DebugOutputSlot, &SkipOutputDebugStringSlot);
        Log("Per-frame OutputDebugString skipped: %s", DebugOutputSkipped ? "ok" : "FAILED (unexpected bytes)");
    }

    if (IniInt("System", "SkipRegistryFlush", 1)) {
        int Count = 0;
        for (int Index = 0; Index < 2; Index++) Count += RepointImportUse(A->RegistryFlushes[Index], A->RegistryFlushSlot, &SkipRegFlushKeySlot);
        FlushesSkipped = Count;
        Log("RegFlushKey skipped: %d/2 sites", Count);
    }

    if (IniInt("System", "FixDiskSpace", 1)) {
        RealGetDiskFreeSpaceEx = (GetDiskFreeSpaceExFunctionType)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetDiskFreeSpaceExA");
        int Count = 0;
        if (RealGetDiskFreeSpaceEx)
            for (int Index = 0; Index < 2; Index++) Count += RepointImportUse(A->DiskSpaceUses[Index], A->DiskSpaceSlot, &ClampedGetDiskFreeSpaceExSlot);
        Log("Free disk space capped at 2 GB for the game: %d/2 sites", Count);
    }

    if (IniInt("System", "SkipIntro", 1)) {
        OriginalMovieConstructor = (MovieConstructorFunctionType)A->MovieConstructor;
        Log("Skip intro: %s", RedirectCall(A->MovieConstructorCall, A->MovieConstructor, (DWORD)MovieCreated) ? "ok" : "FAILED (unexpected bytes)");
    }

    if (IniInt("System", "SkipDirectPlayCheck", 1))
        Log("DirectPlay check skipped: %s",
            RedirectCall(A->DirectPlayCheckCall, A->DirectPlayCheck, (DWORD)CreateDirect3D) ? "ok" : "FAILED (unexpected bytes)");
}

void RiotSystemReport(void) {
    if (DebugOutputSkipped) Log("Per-frame OutputDebugString calls skipped this session: %ld", SkippedDebugOutputs);
    if (FlushesSkipped) Log("RegFlushKey calls skipped this session: %ld", SkippedFlushes);
    if (ClampedDiskSpaceCalls) Log("Free disk space values capped this session: %ld", ClampedDiskSpaceCalls);
}
