/*
 * RiotTiming - threads, locks and frame timing
 *
 * Loading deadlock (fix described by FusionFix, ThirteenAG, MIT; reimplemented here and checked in the original
 * GOG executables).
 * Facts:
 * - The "model lock" (critical section PIH 0x5DC590 / TTB 0x6400E8) is used by one routine only: it is entered at
 *   PIH 0x458A1C / TTB 0x45DB6C ("push -1; mov ecx, Lock; call Enter") and left at PIH 0x458A36 / TTB 0x45DB86
 *   ("push Lock; call LeaveCriticalSection"); its only other references are its creation and deletion.
 * - While holding it, that routine calls PIH 0x4582D0, which reaches PIH 0x46D270, which enters the "resource lock"
 *   (PIH 0x6DCBC0 / TTB 0x74075C, entered for example at PIH 0x472600 / TTB 0x4789A0). So this path takes the
 *   model lock and then the resource lock.
 * - FusionFix reports that the streaming thread takes the same two locks in the opposite order, which can hang a
 *   level load on multi-core processors. That opposite path has not been traced here yet (hypothesis).
 * Fix: the routine enters and leaves the resource lock instead of the model lock (only the lock address in the two
 * instructions changes). Both are recursive critical sections. Because the model lock guards nothing else, the
 * routine stays mutually exclusive, and every thread now takes the resource lock first: the two locks can no longer
 * be taken in opposite orders.
 *
 * Frame hook. The main loop calls the frame function once per pass (PIH call at 0x438788 to 0x404550 / TTB call at
 * 0x43BEC8 to 0x402570; cdecl, no arguments, returns int). The call is redirected to FrameHook, which waits for the
 * frame rate limit, runs the registered RiotFix callbacks and then the frame function.
 *
 * Frame rate limit ([Timing] MaxFrameRate). The games misbehave above 60 frames per second (the physics and some
 * animations run from the frame time without a cap), so the hook holds each pass until the frame's share of a second
 * has gone by. The wait sleeps while more than two milliseconds are left (with the system timer at 1 ms, asked of
 * winmm when it is there) and spins for the rest, which keeps the pace without burning a core. The next frame is
 * counted from the time the last one was due, not from when it started, so the rate does not drift; after a pause
 * longer than two frames (a level load) the count starts again from now.
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotTiming.h"
#include <string.h>

/* ================================================================ Frame rate limit */
static double FrameSeconds;              /* 1 / MaxFrameRate, 0 = no limit */
static LARGE_INTEGER Frequency, FrameDue;
typedef DWORD (WINAPI *TimePeriodFunctionType)(UINT);
static TimePeriodFunctionType BeginTimePeriod;

static void WaitForFrame(void) {
    if (!FrameSeconds) return;
    LARGE_INTEGER Now; QueryPerformanceCounter(&Now);
    const LONGLONG Interval = (LONGLONG)(FrameSeconds * (double)Frequency.QuadPart);
    if (!FrameDue.QuadPart || Now.QuadPart > FrameDue.QuadPart + 2 * Interval) FrameDue.QuadPart = Now.QuadPart;
    FrameDue.QuadPart += Interval;
    for (;;) {
        QueryPerformanceCounter(&Now);
        const LONGLONG Left = FrameDue.QuadPart - Now.QuadPart;
        if (Left <= 0) return;
        const double Milliseconds = (double)Left * 1000.0 / (double)Frequency.QuadPart;
        if (Milliseconds > 2.0) Sleep((DWORD)(Milliseconds - 1.5)); else Sleep(0);
    }
}

typedef struct {
    DWORD EnterModelLock;     /* "mov ecx, ModelLock" (B9 imm32) */
    DWORD LeaveModelLock;     /* "push ModelLock" (68 imm32) */
    DWORD ModelLock, ResourceLock;
    DWORD FrameCall, FrameFunction;
} TimingAddresses;

static const TimingAddresses Addresses[GameCount] = {
    [GamePIH] = { 0x458A1E, 0x458A36, 0x5DC590, 0x6DCBC0, 0x438788, 0x404550 },
    [GameTTB] = { 0x45DB6E, 0x45DB86, 0x6400E8, 0x74075C, 0x43BEC8, 0x402570 },
};

/* ================================================================ Frame hook */
#define MaximumFrameCallbacks 24
static RiotFrameCallback FrameCallbacks[MaximumFrameCallbacks];
static int FrameCallbackCount;
typedef int (__cdecl *FrameFunctionType)(void);
static FrameFunctionType OriginalFrame;

/* A full table used to drop callbacks without a word, and one of them was the screen-text watcher: the option was
 * on, the ini was right, and nothing ran or said why. A diagnostic that disappears quietly is worse than one that
 * is missing, so the table is larger and a refusal is now written down. */
void RiotTimingOnFrame(RiotFrameCallback Callback) {
    if (FrameCallbackCount < MaximumFrameCallbacks) { FrameCallbacks[FrameCallbackCount++] = Callback; return; }
    Log("Frame hook: no room for another per-frame callback (the limit is %d), so one will not run",
        MaximumFrameCallbacks);
}

static int __cdecl FrameHook(void) {
    WaitForFrame();
    for (int Index = 0; Index < FrameCallbackCount; Index++) if (FrameCallbacks[Index]) FrameCallbacks[Index]();
    return OriginalFrame();
}

void RiotTimingApply(void) {
    if (CurrentGame == GameUnknown) return;
    const TimingAddresses *A = &Addresses[CurrentGame];

    if (IniInt("Timing", "FixLoadingDeadlock", 1)) {
        unsigned char Enter[5] = {0xB9}, Leave[5] = {0x68};
        memcpy(Enter + 1, &A->ModelLock, 4); memcpy(Leave + 1, &A->ModelLock, 4);
        int Ok = BytesAre(A->EnterModelLock, Enter, 5) && BytesAre(A->LeaveModelLock, Leave, 5) &&
                 WriteCode(A->EnterModelLock + 1, &A->ResourceLock, 4) && WriteCode(A->LeaveModelLock + 1, &A->ResourceLock, 4);
        Log("Loading deadlock (one lock order for models and resources): %s", Ok ? "ok" : "FAILED (unexpected bytes)");
    }

    const int MaxFrameRate = IniInt("Timing", "MaxFrameRate", 60);
    if (MaxFrameRate > 0) {
        if (MaxFrameRate < 10 || MaxFrameRate > 1000) Log("Timing.MaxFrameRate=%d is invalid (10 to 1000, or 0)", MaxFrameRate);
        else {
            QueryPerformanceFrequency(&Frequency);
            FrameSeconds = 1.0 / (double)MaxFrameRate;
            HMODULE Winmm = LoadLibraryA("winmm.dll");
            BeginTimePeriod = Winmm ? (TimePeriodFunctionType)GetProcAddress(Winmm, "timeBeginPeriod") : NULL;
            if (BeginTimePeriod) BeginTimePeriod(1);
            Log("Frame rate limited to %d per second (system timer %s)", MaxFrameRate, BeginTimePeriod ? "at 1 ms" : "as it is");
        }
    }

    if (FrameCallbackCount || FrameSeconds) {
        OriginalFrame = (FrameFunctionType)A->FrameFunction;
        Log("Frame hook (%d RiotFix callbacks%s): %s", FrameCallbackCount, FrameSeconds ? ", frame rate limit" : "",
            RedirectCall(A->FrameCall, A->FrameFunction, (DWORD)FrameHook) ? "ok" : "FAILED (unexpected bytes)");
    }
}
