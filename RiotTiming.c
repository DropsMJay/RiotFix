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
 * 0x43BEC8 to 0x402570; cdecl, no arguments, returns int). The call is redirected to FrameHook, which runs the
 * registered RiotFix callbacks and then the frame function.
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotTiming.h"
#include <string.h>

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
#define MaximumFrameCallbacks 8
static RiotFrameCallback FrameCallbacks[MaximumFrameCallbacks];
static int FrameCallbackCount;
typedef int (__cdecl *FrameFunctionType)(void);
static FrameFunctionType OriginalFrame;

void RiotTimingOnFrame(RiotFrameCallback Callback) {
    if (FrameCallbackCount < MaximumFrameCallbacks) FrameCallbacks[FrameCallbackCount++] = Callback;
}

static int __cdecl FrameHook(void) {
    for (int Index = 0; Index < FrameCallbackCount; Index++) FrameCallbacks[Index]();
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

    if (FrameCallbackCount) {
        OriginalFrame = (FrameFunctionType)A->FrameFunction;
        Log("Frame hook (%d RiotFix callbacks): %s", FrameCallbackCount,
            RedirectCall(A->FrameCall, A->FrameFunction, (DWORD)FrameHook) ? "ok" : "FAILED (unexpected bytes)");
    }
}
