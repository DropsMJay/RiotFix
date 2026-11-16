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
 * pushing for about 5 frames after it stops. [Mouse] CursorDirect makes the menu cursor (PIH Suffering.rfl+0x4FEB0:
 * actions 0x407 / 0x408 and 0x405 / 0x406; TTB SufferingTTB.rfl around +0x12C77D: 0x425 / 0x426 and 0x423 / 0x424)
 * ask mode 0, by changing the "push 1" (6A 01) before those two calls to "push 0".
 *
 * Cursor speed: the cursor update (PIH Suffering.rfl+0x4FEB0, vtable entry +0x1514A4; TTB SufferingTTB.rfl+0x12C540,
 * vtable entry +0x18FAF0; thiscall, no arguments, plain ret) moves the cursor by axis * frame time * speed, with the
 * frame time read from the game object's +4. That time comes from GetTickCount: LogMouse showed TTB menu frames of
 * 15, 16 and, about one frame in 16, 31-32 ms while every frame measured 16.4-16.7 ms, so those frames move the
 * cursor twice as far. [Mouse] CursorPreciseTime puts the frame time measured with QueryPerformanceCounter in the game
 * object's +4 only while the cursor update runs, and restores the game's value after it.
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotInput.h"
#include "RiotTiming.h"
#include "RiotSdl.h"
#include "RiotPad.h"
#include <string.h>

typedef struct {
    DWORD MouseVtable, MousePoll;
    DWORD InputVtable; int AxisSlot; DWORD Axis;   /* input manager: positive minus negative action, (set, +, -, mode) */
    DWORD CursorModePushes[2];                 /* .rfl offsets of "push 1" (6A 01), each followed by "push imm32" */
    unsigned char CursorActions[2][4];         /* the imm32 after each push: the first action of the axis */
    DWORD GameObject;                          /* .rfl offset of the pointer to the exe's game object (+4: frame time) */
    DWORD CursorUpdateEntry, CursorUpdate;     /* .rfl offsets: vtable entry of the cursor update, the function */
    /* The input manager's per-action query ([Debug] LogActions, and the way a controller will be fed in). The game
     * asks it what one action is doing and it answers a float in st(0); 3 stack arguments, measured with
     * Scripts/Args.py (PIH ret 0xc at 0x40D2E5). The axis query above is only a wrapper that calls this one through
     * the vtable twice - once for the positive action, once for the negative - and subtracts (PIH 0x40C348 calls
     * [vtable+0x98], which is this slot), so a hook here covers both paths. */
    int ActionSlot; DWORD ActionQuery;
    /* The bindings, as the engine itself reads them. The action query first calls a lookup (PIH 0x40C3D0, TTB
     * 0x40D8B0) that walks two tables held by the input manager, the one at +0x28 and then the one at +0x2C, and
     * stops at the first record of the asked action; the query then walks on while the records keep that action.
     * Records are {action, device, code, ...} ended by an action of -1, 20 bytes each in PIH and 24 in TTB (the
     * lookup's stride). Device 0 is the keyboard with DirectInput scan codes; device 1 the mouse, codes 0..5 its
     * axes and wheel, 6 onwards its buttons. The table in Suffering.rfl at 0x10167520 is only the defaults: with it
     * Use came out as LAlt while the player's Use was E. */
    DWORD ControlRecordSize;
    /* The device array: the engine's own list of input devices, entries of 0x28 bytes (both games), the first field
     * the device object. Two entries in each game, the keyboard then the mouse. Docs/PadPort.md section 1. */
    DWORD DeviceArray; int DeviceCount;
} InputAddresses;

static const InputAddresses Addresses[GameCount] = {
    [GamePIH] = { 0x58D298, 0x4100C0, 0x58CB38, 40, 0x40C330, { 0x50078, 0x500B7 }, { { 0x08, 0x04, 0, 0 }, { 0x06, 0x04, 0, 0 } }, 0x17BF40, 0x1514A4, 0x4FEB0, 38, 0x40D1A0, 0x14, 0x5B5418, 2 },
    [GameTTB] = { 0x5E78A0, 0x411680, 0x5E7130, 41, 0x40D810, { 0x12C77D, 0x12C7BC }, { { 0x26, 0x04, 0, 0 }, { 0x24, 0x04, 0, 0 } }, 0x1B2078, 0x18FAF0, 0x12C540, 39, 0x40E130, 0x18, 0x6156E8, 2 },
};
static const InputAddresses *A;

typedef float (ThisCall *ActionFunctionType)(void *, int, int, int);

/* ================================================================ Actions
 *
 * What the engine is asked about input, action by action ([Debug] LogActions, diagnostics).
 *
 * The action ids are the numbers in each game's binding table (PIH Suffering.rfl 0x10167438, rows of a pointer to
 * the action's name and its id; TTB 0x01A36C60, rows of an id and an index into the name list). Those tables are
 * what a controller will eventually drive, but one thing is missing from them on PC: the four Move rows the console
 * build has were dropped, and the game still walks, so movement is asked for in some other way. Rather than guess,
 * this logs every query the engine makes - the set, the action, the mode it asks for and the answer - so pressing
 * each key in turn names its real id.
 *
 * Read-only: the original is called and its answer passed straight back. */
static ActionFunctionType OriginalActionQuery;
static int LogActions;

/* Two records per distinct question the engine asks: the first time it is asked at all (the census, however the
 * answer comes out, so an action that exists but is not pressed still shows up), and every time the answer changes
 * (so holding a key writes one line, not one a frame). The first build of this logged every non-zero answer every
 * frame, which spent 129 of its 600 lines on one held key and never reached the menu actions at all. */
#define ActionsRemembered 256
typedef struct { int Set, Action, Mode; float Value; } ActionSeen;
static ActionSeen ActionsSeen[ActionsRemembered];
static int ActionsSeenCount;
static CRITICAL_SECTION ActionsLock;
static int ActionsLockMade;

static void ReportAction(int Set, int Action, int Mode, float Value) {
    if (!ActionsLockMade) return;
    EnterCriticalSection(&ActionsLock);
    ActionSeen *Found = NULL;
    for (int Index = 0; Index < ActionsSeenCount; Index++)
        if (ActionsSeen[Index].Set == Set && ActionsSeen[Index].Action == Action && ActionsSeen[Index].Mode == Mode) {
            Found = &ActionsSeen[Index]; break;
        }
    if (!Found) {
        if (ActionsSeenCount >= ActionsRemembered) { LeaveCriticalSection(&ActionsLock); return; }
        Found = &ActionsSeen[ActionsSeenCount++];
        Found->Set = Set; Found->Action = Action; Found->Mode = Mode; Found->Value = Value;
        LeaveCriticalSection(&ActionsLock);
        Log("[input] action 0x%03x is asked about (set %08x, mode %d), first answer %.3f",
            (unsigned)Action, (unsigned)Set, Mode, (double)Value);
        return;
    }
    const float Was = Found->Value;
    if (Was == Value) { LeaveCriticalSection(&ActionsLock); return; }
    Found->Value = Value;
    LeaveCriticalSection(&ActionsLock);
    Log("[input] action 0x%03x (set %08x, mode %d): %.3f -> %.3f",
        (unsigned)Action, (unsigned)Set, Mode, (double)Was, (double)Value);
}

/* ================================================================ Controller ([Controller] Enable)
 *
 * The engine asks its input manager what one action is doing and gets a float back; a controller is fed in by
 * answering that question with the pad's own state when the pad is doing more than the keyboard. The action ids are
 * each game's own, measured with [Debug] LogActions rather than taken from the binding table: in The Suffering the
 * four movement ids (0x409 / 0x40A and 0x40F / 0x410) are not in that table at all, since the console build's Move
 * rows were dropped from it, and only watching the engine ask found them.
 *
 * Which of each measured pair is forward and which is back, and which pair is walking and which is strafing, is not
 * settled by the log - it names the ids, not their meaning - so every one of those choices is an .ini switch rather
 * than a guess baked into the build. Answering is max(keyboard, pad), so both work at once and nothing is taken away.
 *
 * Only the ids below are driven. Everything else is passed through untouched. */
static int PadOn, PadDeadZone, PadSwapSticks, PadInvertMoveX, PadInvertMoveY, PadInvertLookX, PadInvertLookY;
static float PadLookSpeed;

/* The axis as 0..1 past the dead zone, in one direction only: Negative asks for the half below centre. */
static float PadStick(int Axis, int Negative) {
    int Raw = RiotSdlAxis(Axis);
    if (Negative) Raw = -Raw;
    if (Raw <= PadDeadZone) return 0.0f;
    const float Span = 32767.0f - (float)PadDeadZone;
    const float Value = ((float)Raw - (float)PadDeadZone) / (Span > 1.0f ? Span : 1.0f);
    return Value > 1.0f ? 1.0f : Value;
}

/* The five layouts the console versions offered, read off their own Controller Settings screen. The PC data still
 * carries the screen and the five names (Interface strings 5:400 to 5:404) but not the button tables, which is why
 * these were taken from the PS2 build rather than from a file. Pad buttons are SDL's: 0 bottom face (cross), 1 right
 * face (circle), 2 left face (square), 3 top face (triangle), 4 back / select, 6 start, 7 and 8 the stick clicks,
 * 9 and 10 the shoulders (L1, R1), 11 to 14 the d-pad; the triggers (L2, R2) are axes 4 and 5.
 *
 * One physical button can serve two actions, because the engine asks by action and only about the actions that make
 * sense where it is: the d-pad drives both the menu cursor and the inventory, and the bottom face button is Use in
 * play and Select in a menu. */
typedef struct {
    const char *Name;
    int Attack, Jump, Throw, Reload, Use, UseHealth, FirstPerson, Insanity, StrafeLeft, StrafeRight, Flashlight;
} PadLayout;

/* -1 is "not on this layout". Triggers are given as 100 + the axis, so 104 is L2 and 105 is R2. */
#define PadTrigger(Axis) (100 + (Axis))
static const PadLayout PadLayouts[] = {
    /*                      Attack Jump  Throw Reload Use UseHealth 1stPerson Insanity StrafeL StrafeR Flash */
    { "doublestick",           10, PadTrigger(5), 9, PadTrigger(4), 0,  1,  2,  3, -1, -1, -1 },
    { "doublestickflashlight", 10, PadTrigger(5), 9, PadTrigger(4), 0,  1,  2,  3, -1, -1,  8 },
    { "throwjumpswap",         10,  9, PadTrigger(5), 2, 0,  1, PadTrigger(4), 3, -1, -1, -1 },
    { "jumpontop",             10,  0,  9,  1,  2, PadTrigger(4), PadTrigger(5), 3, -1, -1, -1 },
    { "singlestick",            2,  0,  1,  8,  3,  7,  9, 10, PadTrigger(4), PadTrigger(5), -1 },
};
static const PadLayout *PadMap = &PadLayouts[0];

/* A button or a trigger, as 0 or 1. */
static float PadPressed(int Which) {
    if (Which < 0) return 0.0f;
    if (Which >= 100) return PadStick(Which - 100, 0) > 0.25f ? 1.0f : 0.0f;
    return RiotSdlButton(Which) ? 1.0f : 0.0f;
}

static float PadValueFor(int Action) {
    const int MoveX = PadSwapSticks ? 2 : 0, MoveY = PadSwapSticks ? 3 : 1;
    const int LookX = PadSwapSticks ? 0 : 2, LookY = PadSwapSticks ? 1 : 3;
    /* Directions read off the control table: 0x409 W / Up, 0x40A S / Down, 0x40F A / Left, 0x410 D / Right,
     * 0x40B Num 4 (turn left), 0x40C Num 6, 0x41C Num 8 (look up), 0x41D Num 2. SDL's sticks run from -32768 at
     * the top and left to 32767 at the bottom and right, so up and left are the negative halves. */
    switch (Action) {
        case 0x409: return PadStick(MoveY, !PadInvertMoveY);
        case 0x40A: return PadStick(MoveY, PadInvertMoveY);
        case 0x40F: return PadStick(MoveX, !PadInvertMoveX) + PadPressed(PadMap->StrafeLeft);
        case 0x410: return PadStick(MoveX, PadInvertMoveX) + PadPressed(PadMap->StrafeRight);
        case 0x40B: return PadStick(LookX, !PadInvertLookX) * PadLookSpeed;
        case 0x40C: return PadStick(LookX, PadInvertLookX) * PadLookSpeed;
        case 0x41C: return PadStick(LookY, !PadInvertLookY) * PadLookSpeed;
        case 0x41D: return PadStick(LookY, PadInvertLookY) * PadLookSpeed;
        default: return 0.0f;
    }
}

/* ---------------------------------------------------------------- Buttons, as keys
 *
 * The engine polls analog actions every frame, but it asks about a button action only once one of its keys has
 * gone down, and some (Reload, measured: R pressed, no question asked at all) it never asks the per-action query
 * about. Every one of those paths reads the same 256-byte keyboard state, though, so a held pad button is turned
 * into the key the player has bound to its action, marked down in that state, and the engine takes it from there
 * exactly as it takes the keyboard - including the actions that act differently when held.
 *
 * Which key is read each frame from the bindings the engine itself reads (see ControlRecordSize), so the player's
 * own bindings are honoured. An action bound only to a mouse button gets that button marked in the mouse's state
 * instead.
 *
 * Menus take Accept and Cancel the console's way, cross and triangle (circle too), with the d-pad and the left
 * stick both on the arrow keys, as on the console: no pad input moves the mouse cursor. The engine asks about the
 * menu cursor (actions 0x405 to 0x408) only while a menu is up, which is how a menu is told from play.
 *
 * The mouse cursor is hidden while the pad is the last thing used, and comes back as soon as the mouse moves or a
 * mouse button is pressed (RiotInputHidesCursor, read where RiotAccessibility draws the cursor). */
typedef struct { DWORD Action, Device, Code; } ControlRecord;
static unsigned char PadKeys[256], PadMouse[8];
static volatile DWORD MenuSeen;
static volatile DWORD PadUsedAt, MouseUsedAt;   /* GetTickCount of the last use of each, 0 = never */

int RiotInputHidesCursor(void) {
    const int Hidden = PadOn && PadUsedAt && (LONG)(PadUsedAt - MouseUsedAt) > 0;
    static int Was, Changes;
    if (Hidden != Was && Changes < 20) {   /* a few, so a mouse that never reads as still shows up in the log */
        Changes++;
        Log("Controller: mouse cursor %s", Hidden ? "hidden (the pad was used last)" : "shown (the mouse was used)");
    }
    Was = Hidden;
    return Hidden;
}

/* Any button, a trigger past a quarter or a stick past its dead zone. */
static int PadTouched(void) {
    for (int Button = 0; Button <= 14; Button++) if (RiotSdlButton(Button)) return 1;
    for (int Axis = 0; Axis < 4; Axis++) if (PadStick(Axis, 0) > 0.0f || PadStick(Axis, 1) > 0.0f) return 1;
    return PadPressed(PadTrigger(4)) > 0.0f || PadPressed(PadTrigger(5)) > 0.0f;
}
static void *volatile InputManager;   /* "this" of the action query, kept on its first call */
static int NativePad;                 /* 1 while RiotPad gives the game a pad of its own, which this path leaves alone */

void *RiotInputManager(void) { return InputManager; }

static const ControlRecord *RecordAt(const char *Table, int Index) {
    return (const ControlRecord *)(Table + Index * A->ControlRecordSize);
}

/* The first record of an action, found the way the engine's own lookup finds it: table +0x28, then table +0x2C. */
static const char *FirstRecordOf(int Action) {
    const char *Manager = (const char *)InputManager;
    if (!Manager) return NULL;
    for (int Which = 0; Which < 2; Which++) {
        const char *Table = *(const char *const *)(Manager + 0x28 + Which * 4);
        if (!Table) continue;
        for (int Index = 0; Index < 1024 && RecordAt(Table, Index)->Action != 0xFFFFFFFF; Index++)
            if ((int)RecordAt(Table, Index)->Action == Action) return (const char *)RecordAt(Table, Index);
    }
    return NULL;
}

/* Of the records the engine reads for an action, the first key (device 0), or failing that the first mouse button
 * (device 1, as a button index). */
static int BindingOf(int Action, int *Device, int *Code) {
    const char *First = FirstRecordOf(Action);
    if (!First) return 0;
    int Found = 0;
    for (int Index = 0; Index < 16 && (int)RecordAt(First, Index)->Action == Action; Index++) {
        const ControlRecord *Record = RecordAt(First, Index);
        if (Record->Device == 0 && Record->Code < 256) { *Device = 0; *Code = (int)Record->Code; return 1; }
        if (Record->Device == 1 && Record->Code >= 6 && Record->Code < 14 && !Found) {
            *Device = 1; *Code = (int)Record->Code - 6; Found = 1;
        }
    }
    return Found;
}

static void PressAction(int Action) {
    int Device = 0, Code = 0;
    if (!BindingOf(Action, &Device, &Code)) return;
    if (Device == 0) PadKeys[Code] = 0x80; else PadMouse[Code] = 0x80;
    static unsigned char Told[0x80];
    const int Slot = Action - 0x3E0;
    if (Slot >= 0 && Slot < 0x80 && !Told[Slot]) {
        Told[Slot] = 1;
        Log("[pad] action 0x%03x goes in as %s 0x%02x", (unsigned)Action, Device ? "mouse button" : "key", (unsigned)Code);
    }
}

static void BuildPadKeys(void) {
    memset(PadKeys, 0, sizeof PadKeys); memset(PadMouse, 0, sizeof PadMouse);
    if (!PadOn) return;
    if (GetTickCount() - MenuSeen < 300) {
        if (RiotSdlButton(0)) PadKeys[0x1C] = 0x80;                                   /* cross: Enter */
        if (RiotSdlButton(1) || RiotSdlButton(3) || RiotSdlButton(6)) PadKeys[0x01] = 0x80;  /* Esc */
        if (RiotSdlButton(11)) PadKeys[0xC8] = 0x80;                                  /* the arrow keys */
        if (RiotSdlButton(12)) PadKeys[0xD0] = 0x80;
        if (RiotSdlButton(13)) PadKeys[0xCB] = 0x80;
        if (RiotSdlButton(14)) PadKeys[0xCD] = 0x80;
        /* The left stick past half way, on its stronger axis, is the d-pad's arrow for that way. */
        const float Up = PadStick(1, 1), Down = PadStick(1, 0), Left = PadStick(0, 1), Right = PadStick(0, 0);
        const float Vertical = Up > Down ? Up : Down, Horizontal = Left > Right ? Left : Right;
        if (Vertical > 0.5f && Vertical >= Horizontal) PadKeys[Up > Down ? 0xC8 : 0xD0] = 0x80;
        else if (Horizontal > 0.5f) PadKeys[Left > Right ? 0xCB : 0xCD] = 0x80;
        return;
    }
    const struct { int Action, Button; } Wanted[] = {
        { 0x428, PadMap->Attack }, { 0x42A, PadMap->Jump }, { 0x429, PadMap->Throw }, { 0x425, PadMap->Reload },
        { 0x42B, PadMap->Use }, { 0x430, PadMap->UseHealth }, { 0x42C, PadMap->FirstPerson },
        { 0x42D, PadMap->Insanity }, { 0x426, PadMap->Flashlight },
        { 0x3FF, 4 }, { 0x3EA, 6 },                 /* map on select, pause on start, on every layout */
        { 0x43F, 13 }, { 0x440, 14 },               /* d-pad left / right: previous / next weapon */
        { 0x441, 11 }, { 0x442, 12 },               /* d-pad up / down: previous / next thrown */
    };
    for (size_t Index = 0; Index < sizeof Wanted / sizeof *Wanted; Index++)
        if (PadPressed(Wanted[Index].Button) > 0.0f) PressAction(Wanted[Index].Action);
}

/* Armed on a frame, never at start-up. SDL is deliberately started on the first frame rather than from DllMain,
 * where LoadLibrary deadlocks on the loader lock, so at the moment RiotInputApply runs there is no pad open yet and
 * asking for one answers no. The first build of this asked there and switched the controller off for the whole
 * session. The question is now asked every frame until a pad turns up, which also picks up a pad plugged in while
 * the game is already running. */
static int PadWanted;
static void ReadPadThisFrame(void) {
    if (!PadOn) {
        if (!PadWanted || !RiotSdlHasPad()) return;
        PadOn = 1;
        Log("Controller: on, layout \"%s\" (dead zone %d, look speed %.2f%s%s%s%s%s)", PadMap->Name, PadDeadZone,
            (double)PadLookSpeed, PadSwapSticks ? ", sticks swapped" : "", PadInvertMoveX ? ", move x inverted" : "",
            PadInvertMoveY ? ", move y inverted" : "", PadInvertLookX ? ", look x inverted" : "",
            PadInvertLookY ? ", look y inverted" : "");
        Log("Controller: %s", NativePad ? "the engine reads the pad itself; this path only hides the mouse cursor"
                                        : "buttons go in as the keys bound to their actions, read from the input manager");
    }
    RiotSdlReadPad();
    /* Which of the two was used last decides whether the mouse cursor is drawn, so it is noticed in both paths:
     * when the engine reads the pad itself this is all that is left of this one. */
    if (PadOn && PadTouched()) PadUsedAt = GetTickCount() | 1;
    if (!NativePad) BuildPadKeys();
}

/* ============================================= Bindings and devices ([Debug] LogBindings)
 *
 * Stage 0 of Docs/PadPort.md. Porting the console's pad as a real device needs three things measured in play rather
 * than read out of the files: what Low and High really are in the live records (the table in Suffering.rfl reads
 * -1.0 and 1.0, which would make a loose key answer 0.5 while LogActions measures 0.0), which fields the manager
 * fills in each device entry, and whether the manager's second record table (+0x2C) is free for records of our own.
 * This logs all three once, on the first frame on which the input manager is known, then names each input of each
 * device the first time it reads anything, which shows the raw values a device is expected to write. It changes
 * nothing. */
typedef struct {
    void *Device; DWORD State, Inputs, FieldsC;
    const float *Samples;   /* +0x10: the rows the value fetch reads, [Samples + Inputs * row + input] */
    DWORD Fields14, Rows, Fields1C, Polled, Fields24;
} DeviceEntry;

typedef int (ThisCall *DeviceIntType)(void *);

static int LogBindings;

static void LogRecordTable(const char *Which, const char *Table) {
    if (!Table) { Log("Bindings: the %s table is empty, so it is free for records of our own", Which); return; }
    int Count = 0;
    while (Count < 500 && RecordAt(Table, Count)->Action != 0xFFFFFFFF) Count++;
    Log("Bindings: the %s table at %p holds %d records of %lu bytes", Which, (const void *)Table, Count,
        A->ControlRecordSize);
    for (int Index = 0; Index < Count; Index++) {
        const ControlRecord *Record = RecordAt(Table, Index);
        const float *Range = (const float *)((const char *)Record + 12);
        Log("  action 0x%03lx device %lu code 0x%02lx low %.3f high %.3f", Record->Action, Record->Device,
            Record->Code, (double)Range[0], (double)Range[1]);
    }
}

/* The array the engine is using: the game's own, or the larger one RiotPad moved it into. */
static const DeviceEntry *LiveDeviceArray(int *Count) {
    const void *Moved = RiotPadArray();
    *Count = Moved ? RiotPadDeviceCount() : A->DeviceCount;
    return Moved ? (const DeviceEntry *)Moved : (const DeviceEntry *)(size_t)A->DeviceArray;
}

static void LogDeviceEntries(void) {
    int Count; const DeviceEntry *Array = LiveDeviceArray(&Count);
    for (int Index = 0; Index < Count; Index++) {
        const DeviceEntry *Entry = &Array[Index];
        void **Vtable = Entry->Device ? *(void ***)Entry->Device : NULL;
        Log("Devices: %d device %p vtable %p state %lu inputs %lu samples %p rows %lu polled %lu (+0xc %#lx "
            "+0x14 %#lx +0x1c %#lx +0x24 %#lx)", Index, Entry->Device, (void *)Vtable, Entry->State, Entry->Inputs,
            (const void *)Entry->Samples, Entry->Rows, Entry->Polled, Entry->FieldsC, Entry->Fields14,
            Entry->Fields1C, Entry->Fields24);
        if (Vtable)
            Log("  the device itself says %d inputs, %d of them bindable",
                ((DeviceIntType)Vtable[2])(Entry->Device), ((DeviceIntType)Vtable[3])(Entry->Device));
    }
}

/* The newest row of samples, input by input, each one named the first time it reads anything. */
static void LogLiveSamples(void) {
    static unsigned char Seen[4][512];
    int Count; const DeviceEntry *Array = LiveDeviceArray(&Count);
    for (int Index = 0; Index < Count && Index < 4; Index++) {
        const DeviceEntry *Entry = &Array[Index];
        if (!Entry->Samples || !Entry->Inputs || !Entry->Rows) continue;
        const float *Row = Entry->Samples + (size_t)Entry->Inputs * (Entry->Rows - 1);
        for (DWORD Input = 0; Input < Entry->Inputs && Input < 512; Input++) {
            if (Row[Input] == 0.0f || Row[Input] != Row[Input] || Seen[Index][Input]) continue;
            Seen[Index][Input] = 1;
            Log("Devices: %d input 0x%02lx reads %.3f", Index, Input, (double)Row[Input]);
        }
    }
}

static void LogBindingsFrame(void) {
    static int Told;
    if (!InputManager || !A->DeviceArray) return;
    if (!Told) {
        Told = 1;
        Log("Bindings: the input manager is at %p; its tables are at +0x28 %p and +0x2C %p", InputManager,
            *(void **)((char *)InputManager + 0x28), *(void **)((char *)InputManager + 0x2C));
        LogRecordTable("first (+0x28)", *(const char **)((char *)InputManager + 0x28));
        LogRecordTable("second (+0x2C)", *(const char **)((char *)InputManager + 0x2C));
        LogDeviceEntries();
    }
    LogLiveSamples();
}

/* The per-action query, hooked for the controller's analog part and for [Debug] LogActions. Answering is
 * max(keyboard, pad), so both work at once. */
static float ThisCall ActionQuery(void *Self, int Set, int Action, int Mode) {
    if (!InputManager) InputManager = Self;
    float Value = OriginalActionQuery(Self, Set, Action, Mode);
    if (Action >= 0x405 && Action <= 0x408) MenuSeen = GetTickCount();
    if (PadOn && !NativePad) {
        const float FromPad = PadValueFor(Action);
        if (FromPad > Value) Value = FromPad;
    }
    if (LogActions) ReportAction(Set, Action, Mode, Value);
    if (LogBindings && NativePad) RiotPadWatchQuery(Set, (DWORD)Action, Mode, Value);
    return Value;
}

static void WatchActions(void) {
    NativePad = RiotPadIsNative();
    LogActions = IniInt("Debug", "LogActions", 0);
    LogBindings = IniInt("Debug", "LogBindings", 0);
    if (LogBindings) {
        if (NativePad) RiotPadWatch();
        if (A->DeviceArray) RiotTimingOnFrame(LogBindingsFrame);
        else Log("Bindings: this game's device array is not known yet, so nothing is logged");
    }
    PadWanted = IniInt("Controller", "Enable", 1);
    if (PadWanted) {
        PadDeadZone = IniInt("Controller", "DeadZone", 6000);
        if (PadDeadZone < 0) PadDeadZone = 0;
        if (PadDeadZone > 30000) PadDeadZone = 30000;
        PadLookSpeed = IniFloat("Controller", "LookSpeed", 2.0f);
        PadSwapSticks = IniInt("Controller", "SwapSticks", 0);
        PadInvertMoveX = IniInt("Controller", "InvertMoveX", 0);
        PadInvertMoveY = IniInt("Controller", "InvertMoveY", 0);
        PadInvertLookX = IniInt("Controller", "InvertLookX", 0);
        PadInvertLookY = IniInt("Controller", "InvertLookY", 0);
        char Wanted[64]; GetPrivateProfileStringA("Controller", "Layout", "doublestick", Wanted, sizeof Wanted, IniPath);
        for (char *At = Wanted; *At; At++) {
            if (*At >= 'A' && *At <= 'Z') *At += 32;
            if (*At == ' ' || *At == '+' || *At == '/') { memmove(At, At + 1, strlen(At)); At--; }
        }
        PadMap = &PadLayouts[0];
        for (size_t Index = 0; Index < sizeof PadLayouts / sizeof *PadLayouts; Index++)
            if (!strcmp(Wanted, PadLayouts[Index].Name)) { PadMap = &PadLayouts[Index]; break; }
        Log("Controller: asked for, waiting for a pad (SDL opens one on the first frame)");
        RiotTimingOnFrame(ReadPadThisFrame);
    }
    if (!LogActions && !LogBindings && !PadWanted && !NativePad) return;
    if (!A->ActionQuery) { Log("Input: the per-action query is not known in this game yet"); return; }
    InitializeCriticalSection(&ActionsLock); ActionsLockMade = 1;
    OriginalActionQuery = (ActionFunctionType)A->ActionQuery;
    Log("Action queries watched (each action once when first asked, then on every change): %s",
        ReplaceVtableSlot(A->InputVtable, A->ActionSlot, A->ActionQuery, (DWORD)ActionQuery) ? "ok" : "FAILED");
}

/* ================================================================ The keyboard as the game reads it
 *
 * ([Debug] LogKeyboard, diagnostics, and the ground for feeding controller buttons in.)
 *
 * The engine polls analog actions every frame but asks about a button action only once one of its keys has gone
 * down: in a LogActions session the movement and look actions are first asked about with the answer 0, while Jump
 * and Throw are first asked about with the answer 1, at the moment their key is already held. So answering the
 * per-action query cannot press a button - nothing asks until a key goes down - and a controller's buttons have to
 * reach the game as keys. This watches how the keyboard is read, to know where to put them: the whole state each
 * frame (GetDeviceState, 256 bytes) or a queue of events (GetDeviceData), and which scan code each key is.
 *
 * Only in use when RiotFix is loaded as dinput8.dll, since that is the only way it sees DirectInput8Create. COM
 * methods are stdcall with the object first. IDirectInput8A slot 3 is CreateDevice; IDirectInputDevice8A slot 9 is
 * GetDeviceState and slot 10 GetDeviceData. The device vtable is shared by every device dinput8 makes, so the
 * keyboard is told apart by the pointer CreateDevice returned for GUID_SysKeyboard. Read-only. */
typedef HRESULT (WINAPI *CreateDeviceFunctionType)(void *, const GUID *, void **, void *);
typedef HRESULT (WINAPI *GetDeviceStateFunctionType)(void *, DWORD, void *);
typedef HRESULT (WINAPI *GetDeviceDataFunctionType)(void *, DWORD, void *, DWORD *, DWORD);
static CreateDeviceFunctionType OriginalCreateDevice;
static GetDeviceStateFunctionType OriginalGetDeviceState;
static GetDeviceDataFunctionType OriginalGetDeviceData;
static void *KeyboardDevice;
static int LogKeyboard;
static volatile LONG KeyboardLines;
static unsigned char KeysSeen[256];
static const GUID KeyboardDeviceId = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID MouseDeviceId = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
#define KeyboardRoom() (InterlockedIncrement(&KeyboardLines) <= 500)

static void *MouseDevice;
static HRESULT WINAPI KeyboardState(void *Self, DWORD Size, void *Data) {
    const HRESULT Result = OriginalGetDeviceState(Self, Size, Data);
    /* The mouse shares this method (dinput8 gives every device one vtable): its buttons start at byte 12, after
     * three LONG axes, 4 of them in a DIMOUSESTATE (16 bytes) and 8 in a DIMOUSESTATE2 (20 bytes). */
    if (PadOn && Self == MouseDevice && SUCCEEDED(Result) && Data && Size >= 16) {
        unsigned char *Buttons = (unsigned char *)Data + 12;
        const LONG *Axes = (const LONG *)Data;   /* x, y and wheel, before the pad's buttons are added */
        int Used = Axes[0] || Axes[1] || Axes[2];
        for (DWORD Index = 0; Index < (Size >= 20 ? 8u : 4u); Index++) if (Buttons[Index] & 0x80) Used = 1;
        if (Used) MouseUsedAt = GetTickCount() | 1;
        for (DWORD Index = 0; Index < (Size >= 20 ? 8u : 4u); Index++) Buttons[Index] |= PadMouse[Index];
    }
    if (LogKeyboard && Self == KeyboardDevice && SUCCEEDED(Result) && Data) {
        static int Announced;
        if (!Announced++) Log("[keyboard] read as a whole state: GetDeviceState, %lu bytes", (unsigned long)Size);
        const unsigned char *Keys = (const unsigned char *)Data;
        for (DWORD Key = 0; Key < Size && Key < 256; Key++) {
            const unsigned char Down = Keys[Key] & 0x80;
            if (Down != KeysSeen[Key]) {
                KeysSeen[Key] = Down;
                if (KeyboardRoom()) Log("[keyboard] state: key 0x%02lx %s", (unsigned long)Key, Down ? "down" : "up");
            }
        }
    }
    /* After the log, so LogKeyboard keeps showing what the real keyboard did. */
    if (PadOn && Self == KeyboardDevice && SUCCEEDED(Result) && Data && Size >= 256) {
        unsigned char *Keys = (unsigned char *)Data;
        for (int Key = 0; Key < 256; Key++) Keys[Key] |= PadKeys[Key];
    }
    return Result;
}

static HRESULT WINAPI KeyboardData(void *Self, DWORD ObjectSize, void *Data, DWORD *Count, DWORD Flags) {
    const HRESULT Result = OriginalGetDeviceData(Self, ObjectSize, Data, Count, Flags);
    if (LogKeyboard && Self == KeyboardDevice && SUCCEEDED(Result) && Data && Count) {
        static int Announced;
        if (!Announced++) Log("[keyboard] read as events: GetDeviceData, %lu-byte records, flags %lx",
                              (unsigned long)ObjectSize, (unsigned long)Flags);
        for (DWORD Index = 0; Index < *Count; Index++) {
            const DWORD *Record = (const DWORD *)((const char *)Data + Index * ObjectSize);
            if (KeyboardRoom()) Log("[keyboard] event: key 0x%02lx %s%s", (unsigned long)Record[0],
                                    (Record[1] & 0x80) ? "down" : "up", (Flags & 1) ? " (peek)" : "");
        }
    }
    return Result;
}

static HRESULT WINAPI CreateDevice(void *Self, const GUID *Which, void **Device, void *Outer) {
    const HRESULT Result = OriginalCreateDevice(Self, Which, Device, Outer);
    if (FAILED(Result) || !Device || !*Device || !Which) return Result;
    const int IsKeyboard = !memcmp(Which, &KeyboardDeviceId, sizeof(GUID));
    if (LogKeyboard)
        Log("[keyboard] device made: %s at %p", IsKeyboard ? "keyboard" : !memcmp(Which, &MouseDeviceId, sizeof(GUID)) ? "mouse" : "other", *Device);
    if (!memcmp(Which, &MouseDeviceId, sizeof(GUID))) {
        MouseDevice = *Device;
        if (OriginalGetDeviceState && ((const DWORD *)*(const DWORD *)*Device)[9] != (DWORD)KeyboardState)
            Log("[pad] the mouse does not share the keyboard's methods: buttons bound only to the mouse will not work");
    }
    if (!IsKeyboard) return Result;
    KeyboardDevice = *Device;
    const DWORD Vtable = *(const DWORD *)*Device;
    if (!OriginalGetDeviceState) {
        OriginalGetDeviceState = (GetDeviceStateFunctionType)((const DWORD *)Vtable)[9];
        OriginalGetDeviceData = (GetDeviceDataFunctionType)((const DWORD *)Vtable)[10];
        const int State = ReplaceVtableSlot(Vtable, 9, (DWORD)OriginalGetDeviceState, (DWORD)KeyboardState);
        const int Events = ReplaceVtableSlot(Vtable, 10, (DWORD)OriginalGetDeviceData, (DWORD)KeyboardData);
        Log("Keyboard reads watched (GetDeviceState %s, GetDeviceData %s)", State ? "ok" : "FAILED", Events ? "ok" : "FAILED");
    }
    return Result;
}

void RiotInputWatchDirectInput(void *DirectInput) {
    if ((!LogKeyboard && !PadWanted) || !DirectInput || OriginalCreateDevice) return;
    const DWORD Vtable = *(const DWORD *)DirectInput;
    OriginalCreateDevice = (CreateDeviceFunctionType)((const DWORD *)Vtable)[3];
    Log("DirectInput devices watched: %s",
        ReplaceVtableSlot(Vtable, 3, (DWORD)OriginalCreateDevice, (DWORD)CreateDevice) ? "ok" : "FAILED");
}

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

    LogKeyboard = IniInt("Debug", "LogKeyboard", 0);
    WatchActions();

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
    if (IniInt("Mouse", "CursorDirect", 1)) RiotTimingOnFrame(CursorDirectFrame);
    if (IniInt("Mouse", "CursorPreciseTime", 1)) {
        QueryPerformanceFrequency(&Frequency);
        RiotTimingOnFrame(MeasureFrame);
        RiotTimingOnFrame(CursorTimeFrame);
    }
}
