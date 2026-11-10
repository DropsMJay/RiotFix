/*
 * RiotInput - mouse timing and the menu cursor's input
 *
 * Mouse reading (both games, same code): the mouse device's poll (vtable PIH 0x58D298 / TTB 0x5E78A0, slot 7:
 * PIH 0x4100C0 / TTB 0x411680, thiscall (float Seconds, int Reset), "ret 8") reads DIMOUSESTATE2 into +0x10 (x, y,
 * wheel counts). With Reset it first clears the sums; then it adds the counts to +0x24 / +0x28 / +0x2C and Seconds
 * to +0x30 (set to 0.1 once above it) and stores counts / (seconds * 1536) split by sign: +0x34 up, +0x38 down,
 * +0x3C left, +0x40 right. When the summed seconds are 0 it stores nothing, so the values of the last frame stay.
 * The input loop (PIH 0x40C980) polls each device twice per frame (Reset on the first poll) and passes the time since
 * its last poll from GetTickCount, which moves in steps of about 15.6 ms: at 60 frames per second the frames measure
 * 15, 16, 31 or 0 ms (seen with LogMouse), and a 0 ms frame drops its movement and repeats the last one.
 * [Mouse] PreciseTime passes the time measured with QueryPerformanceCounter between consecutive polls instead.
 *
 * Axes: the input manager (vtable PIH 0x58CB38, slot 40 = 0x40C330: positive minus negative action, slot 38 =
 * 0x40D1A0 per action, raw value 0x40C470) keeps the last 10 readings of each device (two per frame) and reads them
 * by mode: 0 the newest, 1 the largest, 5 the average. The .rfl asks mode 1 for every axis, so a movement keeps
 * pushing for about 5 frames after it stops. [Cursor] Direct makes the menu cursor (PIH Suffering.rfl+0x4FEB0:
 * actions 0x407 / 0x408 and 0x405 / 0x406; TTB SufferingTTB.rfl around +0x12C77D: 0x425 / 0x426 and 0x423 / 0x424)
 * ask mode 0, by changing the "push 1" (6A 01) before those two calls to "push 0".
 *
 * Cursor speed: the cursor update (PIH Suffering.rfl+0x4FEB0, vtable entry +0x1514A4; TTB SufferingTTB.rfl+0x12C540,
 * vtable entry +0x18FAF0; thiscall, no arguments, plain ret) moves the cursor by axis * frame time * speed, with the
 * frame time read from the game object's +4. That time comes from GetTickCount: LogMouse showed TTB menu frames of
 * 15, 16 and, about one frame in 16, 31-32 ms while every frame measured 16.4-16.7 ms, so those frames move the
 * cursor twice as far. [Cursor] PreciseTime puts the frame time measured with QueryPerformanceCounter in the game
 * object's +4 only while the cursor update runs, and restores the game's value after it.
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotInput.h"
#include "RiotTiming.h"

typedef struct {
    DWORD MouseVtable, MousePoll;
    DWORD InputVtable; int AxisSlot; DWORD Axis;   /* input manager: positive minus negative action, (set, +, -, mode) */
    DWORD CursorModePushes[2];                 /* .rfl offsets of "push 1" (6A 01), each followed by "push imm32" */
    unsigned char CursorActions[2][4];         /* the imm32 after each push: the first action of the axis */
    DWORD GameObject;                          /* .rfl offset of the pointer to the exe's game object (+4: frame time) */
    DWORD CursorUpdateEntry, CursorUpdate;     /* .rfl offsets: vtable entry of the cursor update, the function */
} InputAddresses;

static const InputAddresses Addresses[GameCount] = {
    [GamePIH] = { 0x58D298, 0x4100C0, 0x58CB38, 40, 0x40C330, { 0x50078, 0x500B7 }, { { 0x08, 0x04, 0, 0 }, { 0x06, 0x04, 0, 0 } }, 0x17BF40, 0x1514A4, 0x4FEB0 },
    [GameTTB] = { 0x5E78A0, 0x411680, 0x5E7130, 41, 0x40D810, { 0x12C77D, 0x12C7BC }, { { 0x26, 0x04, 0, 0 }, { 0x24, 0x04, 0, 0 } }, 0x1B2078, 0x18FAF0, 0x12C540 },
};
static const InputAddresses *A;

/* ================================================================ Mouse poll */
typedef int (ThisCall *MousePollFunctionType)(void *, float, int);
static MousePollFunctionType OriginalMousePoll;
static int PreciseTime, LogMouse;
static LARGE_INTEGER Frequency, LastPoll;
static volatile LONG MouseLogLines;
static int MouseQuietPolls = 1000;

static int ThisCall MousePoll(void *Self, float Seconds, int Reset) {
    if (PreciseTime) {
        LARGE_INTEGER Now; QueryPerformanceCounter(&Now);
        if (LastPoll.QuadPart) Seconds = (float)((double)(Now.QuadPart - LastPoll.QuadPart) / (double)Frequency.QuadPart);
        LastPoll = Now;
    }
    int Result = OriginalMousePoll(Self, Seconds, Reset);
    if (LogMouse) {
        /* Every poll with movement and the 12 after it, up to 400 lines. */
        const char *Mouse = (const char *)Self;
        const int CountX = *(const int *)(Mouse + 0x10), CountY = *(const int *)(Mouse + 0x14);
        if (CountX || CountY) MouseQuietPolls = 0; else MouseQuietPolls++;
        if (MouseQuietPolls <= 12 && InterlockedIncrement(&MouseLogLines) <= 400) {
            const float *Out = (const float *)(Mouse + 0x34);
            Log("[mouse] tick %lu result %d seconds %.4f reset %d counts %d,%d sums %d,%d time %.4f up %.4f down %.4f left %.4f right %.4f",
                (unsigned long)GetTickCount(), Result, Seconds, Reset, CountX, CountY, *(const int *)(Mouse + 0x24),
                *(const int *)(Mouse + 0x28), *(const float *)(Mouse + 0x30), Out[0], Out[1], Out[2], Out[3]);
        }
    }
    return Result;
}

/* ================================================================ Axis log (diagnostics)
   [Debug] LogAxes=1: every caller of the input manager's axis method (PIH slot 40, 0x40C330; TTB slot 41, 0x40D810;
   thiscall (set, positive action, negative action, mode), "ret 10h", float in st0) is logged with its actions and
   mode the first 3 times its value is not 0 (up to 200 lines), to find which code reads the mouse for the camera. */
typedef float (ThisCall *AxisFunctionType)(void *, void *, int, int, int);
static AxisFunctionType OriginalAxis;
typedef struct { DWORD Caller; int Positive, Negative, Mode, Times; } AxisCaller;
static AxisCaller AxisCallers[128]; static int AxisCallerCount;
static volatile LONG AxisLogLines;
static float ThisCall LoggedAxis(void *Self, void *Set, int Positive, int Negative, int Mode) {
    float Value = OriginalAxis(Self, Set, Positive, Negative, Mode);
    if (Value != 0.0f && AxisLogLines < 200) {
        const DWORD Caller = (DWORD)__builtin_return_address(0);
        AxisCaller *Known = NULL;
        for (int Index = 0; Index < AxisCallerCount; Index++)
            if (AxisCallers[Index].Caller == Caller && AxisCallers[Index].Positive == Positive && AxisCallers[Index].Negative == Negative) Known = &AxisCallers[Index];
        if (!Known && AxisCallerCount < 128) { Known = &AxisCallers[AxisCallerCount++]; *Known = (AxisCaller){ Caller, Positive, Negative, Mode, 0 }; }
        if (Known && Known->Times < 3) {
            Known->Times++; InterlockedIncrement(&AxisLogLines);
            char Name[MAX_PATH + 32]; AddressName(Caller, Name, sizeof Name);
            Log("[axis] %s actions %#x - %#x mode %d: %.4f", Name, Positive, Negative, Mode, Value);
        }
    }
    return Value;
}

/* [Debug] LogMouse also logs, once per frame (300 lines), the frame time the .rfl uses for the cursor and the
   camera (the game object's +4: PIH Suffering.rfl+0x17BF40, TTB SufferingTTB.rfl+0x1B2078, set from the first entry
   of the table the exe gives to the .rfl) next to the time measured with QueryPerformanceCounter. */
static volatile LONG FrameLogLines;
static void FrameTimeLog(void) {
    static LARGE_INTEGER Last; LARGE_INTEGER Now; QueryPerformanceCounter(&Now);
    const double Real = Last.QuadPart ? (double)(Now.QuadPart - Last.QuadPart) / (double)Frequency.QuadPart : 0.0;
    Last = Now;
    HMODULE Rfl = GetModuleHandleA(CurrentGame == GameTTB ? "SufferingTTB.rfl" : "Suffering.rfl");
    if (!Rfl) return;
    const char *Game = *(const char *const *)((DWORD)Rfl + A->GameObject);
    if (!Game || IsBadReadPtr(Game + 4, 4)) return;
    if (InterlockedIncrement(&FrameLogLines) <= 300) Log("[frame] game %.4f s, measured %.4f s", *(const float *)(Game + 4), Real);
}

/* ================================================================ Cursor */
/* The .rfl is loaded after RiotFix: the cursor's axes are changed on the first frame that finds it. */
static void CursorDirectFrame(void) {
    static int Done;
    if (Done) return;
    HMODULE Rfl = GetModuleHandleA(CurrentGame == GameTTB ? "SufferingTTB.rfl" : "Suffering.rfl");
    if (!Rfl) return;
    Done = 1;
    int Count = 0;
    for (int Index = 0; Index < 2; Index++) {
        const DWORD At = (DWORD)Rfl + A->CursorModePushes[Index];
        unsigned char Expected[7] = { 0x6A, 0x01, 0x68 };
        for (int Byte = 0; Byte < 4; Byte++) Expected[3 + Byte] = A->CursorActions[Index][Byte];
        static const unsigned char NewestMode = 0x00;
        if (BytesAre(At, Expected, sizeof Expected) && WriteCode(At + 1, &NewestMode, 1)) Count++;
    }
    Log("Cursor follows the newest mouse reading: %d/2 axes%s", Count, Count == 2 ? "" : " (unexpected .rfl)");
}

/* ================================================================ Cursor time */
static float MeasuredFrameSeconds;             /* 0 until two frames were measured */
static void MeasureFrame(void) {
    static LARGE_INTEGER Last; LARGE_INTEGER Now; QueryPerformanceCounter(&Now);
    MeasuredFrameSeconds = Last.QuadPart ? (float)((double)(Now.QuadPart - Last.QuadPart) / (double)Frequency.QuadPart) : 0.0f;
    Last = Now;
}

typedef void (ThisCall *CursorUpdateFunctionType)(void *);
static CursorUpdateFunctionType OriginalCursorUpdate;
static DWORD CursorGameObject;                 /* address of the pointer to the game object */
static void ThisCall CursorUpdate(void *Self) {
    float *Time = NULL, Saved = 0.0f;
    char *Game = *(char **)CursorGameObject;
    /* Above 0.1 s (a load or a pause) the game's own value is kept. */
    if (Game && MeasuredFrameSeconds > 0.0f && MeasuredFrameSeconds <= 0.1f) {
        Time = (float *)(Game + 4); Saved = *Time; *Time = MeasuredFrameSeconds;
    }
    OriginalCursorUpdate(Self);
    if (Time) *Time = Saved;
}

static void CursorTimeFrame(void) {
    static int Done;
    if (Done) return;
    HMODULE Rfl = GetModuleHandleA(CurrentGame == GameTTB ? "SufferingTTB.rfl" : "Suffering.rfl");
    if (!Rfl) return;
    Done = 1;
    CursorGameObject = (DWORD)Rfl + A->GameObject;
    OriginalCursorUpdate = (CursorUpdateFunctionType)((DWORD)Rfl + A->CursorUpdate);
    const int Hooked = ReplaceVtableSlot((DWORD)Rfl + A->CursorUpdateEntry, 0, (DWORD)OriginalCursorUpdate, (DWORD)CursorUpdate);
    Log("Cursor speed from the measured frame time: %s", Hooked ? "ok" : "FAILED (unexpected .rfl)");
}

/* ================================================================ Apply */
void RiotInputApply(void) {
    if (CurrentGame == GameUnknown) return;
    A = &Addresses[CurrentGame];

    PreciseTime = IniInt("Mouse", "PreciseTime", 1);
    LogMouse = IniInt("Debug", "LogMouse", 0);
    if (PreciseTime || LogMouse) {
        QueryPerformanceFrequency(&Frequency);
        OriginalMousePoll = (MousePollFunctionType)A->MousePoll;
        int Hooked = ReplaceVtableSlot(A->MouseVtable, 7, A->MousePoll, (DWORD)MousePoll);
        if (!Hooked) PreciseTime = LogMouse = 0;
        if (PreciseTime || !Hooked) Log("Mouse time from QueryPerformanceCounter: %s", Hooked ? "ok" : "FAILED (unexpected vtable)");
        if (LogMouse) { Log("Mouse log (polls with movement, up to 400 lines): on"); RiotTimingOnFrame(FrameTimeLog); }
    }
    if (IniInt("Debug", "LogAxes", 0) || IniInt("Debug", "LogAxis", 0)) {
        OriginalAxis = (AxisFunctionType)A->Axis;
        Log("Axis log (callers of the input axes, up to 200 lines): %s",
            ReplaceVtableSlot(A->InputVtable, A->AxisSlot, A->Axis, (DWORD)LoggedAxis) ? "ok" : "FAILED (unexpected vtable)");
    }
    if (IniInt("Cursor", "Direct", 1)) RiotTimingOnFrame(CursorDirectFrame);
    if (IniInt("Cursor", "PreciseTime", 1)) {
        QueryPerformanceFrequency(&Frequency);
        RiotTimingOnFrame(MeasureFrame);
        RiotTimingOnFrame(CursorTimeFrame);
    }
}
