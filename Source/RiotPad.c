/*
 * RiotPad - the console's pad as a device of the engine's own kind ([Controller] NativeDevice, Docs/PadPort.md).
 *
 * The PC build dropped the pad class and kept everything around it. The engine still keeps an array of input
 * devices (The Suffering: 0x5B5418, two entries of 0x28 bytes, the keyboard then the mouse), still reaches each one
 * through the same 20-slot interface, still keeps a block of samples per device, still looks a binding record's
 * device number up in that array, and still calls the rumble slot on every device in it. So a pad does not need a
 * path of its own: it needs to be the third entry.
 *
 * What a device has to answer, measured from the keyboard and the mouse (slot, what the engine does with it, how
 * many bytes of arguments it pops):
 *
 *   0  destructor                      ret 4      8  where its values are                ret 0
 *   1  become ready, answer 2          ret 0      9  the press threshold of one input     ret 4
 *   2  how many inputs it has          ret 0     10,11,12  "yes" (the keyboard answers 1) ret 0
 *   3  how many may be bound           ret 0     13  unused by the keyboard               ret 4
 *   4  close                           ret 0     14  the row naming one input             ret 4
 *   5  is it open                      ret 0     15  where those rows are                 ret 0
 *   6  open it                         ret 0     16  unused                               ret 4
 *   7  poll (seconds, reset)           ret 8     17  1.0   18  the rumble floor, 0.05     ret 0
 *                                                19  rumble: the two motor levels         ret 0xc
 *
 * Values are plain: the keyboard writes 1.0 for a pressed key and 0.0 for a loose one, with no dead zone and no
 * scaling, because the binding record carries the range, the curve and the inversion. This pad writes the Xbox
 * build's own 27 values in its own order, with its radial dead zone and its trigger threshold, so the console's
 * layout tables can be used as they stand.
 *
 * Slots 13 and 14 are borrowed from the engine rather than written here: 13 because the keyboard and the mouse
 * disagree about how many arguments it takes (4 bytes against 12), so the keyboard's is the one proven safe for a
 * device that is not the mouse, and 14 because it is already generic - it asks this device's own slots 15 and 2 and
 * hands back the row for the input.
 *
 * The array has to move: it has room for two entries and the memory right after it holds other data. So RiotFix
 * puts a three-entry copy in its own memory and repoints the engine's 29 references to it. Every one of those is an
 * address inside an instruction - the operand of a mov or a cmp - which is why no assembly is written here. It all
 * happens at attach, before any game code runs, so the engine then fills in all three entries itself.
 *
 * This stage adds the device and nothing else: no binding record points at it, so the game cannot play differently.
 * [Debug] LogBindings shows the entry the engine built for it and the values it writes.
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotPad.h"
#include "RiotSdl.h"
#include "RiotTiming.h"
#include "RiotInput.h"
#include <string.h>
#include <math.h>

/* The pad's inputs, in the Xbox build's order (Docs/XboxInput.md section 2), so its layout tables fit as they are. */
enum {
    PadStart, PadBack, PadLeftUp, PadLeftDown, PadLeftLeft, PadLeftRight,
    PadRightUp, PadRightDown, PadRightLeft, PadRightRight,
    PadDpadUp, PadDpadDown, PadDpadLeft, PadDpadRight,
    PadNorth, PadWest, PadEast, PadSouth, PadLeftTrigger, PadRightTrigger,
    PadLeftStickButton, PadRightStickButton, PadLeftShoulder, PadRightShoulder,
    PadLeftStickAny, PadRightStickAny, PadDpadAny,
    /* Not a thing on the controller: how far short of running the move stick is asking to be, which is exactly
       what the engine's walk action wants. See the end of ReadPad. */
    PadWalkAmount,
    PadInputCount
};

/* SDL3's own numbering (buttons SOUTH 0 .. DPAD_RIGHT 14, axes LEFTX 0 .. RIGHT_TRIGGER 5). */
enum { SdlSouth, SdlEast, SdlWest, SdlNorth, SdlBack, SdlGuide, SdlStart,
       SdlLeftStick, SdlRightStick, SdlLeftShoulder, SdlRightShoulder,
       SdlDpadUp, SdlDpadDown, SdlDpadLeft, SdlDpadRight };
enum { SdlLeftX, SdlLeftY, SdlRightX, SdlRightY, SdlLeftTrigger, SdlRightTrigger };

/* Every reference the executable makes to the array. Two kinds, and telling them apart is what matters: a Field is
 * a place that names one field of the array (the operand becomes the new array plus that offset), while a Bound is
 * the address one loop stops at, which is its own starting field plus one entry for each device - so a Bound has to
 * grow by a whole entry when a device is added. Reading a bound as if it were a field is how the first attempt at
 * this hung the game: the loop at 0x40BC50, which gives every device its state, its input count and its buffers,
 * kept the old address as its stop and so ran for the keyboard alone, leaving the mouse without any of them. */
typedef struct { DWORD Site, Offset; } PadField;
typedef struct { DWORD Site, Start; } PadBound;

typedef struct {
    DWORD Array; int Entries; DWORD EntrySize;
    const PadField *Fields; const PadBound *Bounds;   /* both ended by a zero Site */
    DWORD BorrowedSlot13, BorrowedSlot14;
    DWORD RecordSize, TableSlot;   /* a binding record's size, and where the manager keeps the table it reads first */
    DWORD Allocate;                /* the game's own allocator, cdecl (flags, size); its memory is the game's to free */
} PadAddresses;

/* The Suffering. Each Site is the first byte of an operand inside an instruction. The list is the whole of what a
 * sweep of the executable's code finds: every four bytes that read as an address inside the array, each one checked
 * against the instruction it belongs to. RiotFix sweeps again after patching and backs the whole thing out if it
 * finds even one left, so a reference missed here cannot reach the game. */
static const PadField PihFields[] = {
    { 0x40C082, 0x00 }, { 0x40C1FC, 0x00 }, { 0x40C203, 0x00 }, { 0x40C236, 0x00 }, { 0x40C2BA, 0x00 },
    { 0x40C49D, 0x00 }, { 0x40C5BE, 0x00 }, { 0x40C634, 0x00 }, { 0x40C664, 0x00 }, { 0x40C69E, 0x00 },
    { 0x40C6A5, 0x00 }, { 0x40CCE7, 0x00 }, { 0x40D40A, 0x00 }, { 0x40E132, 0x00 }, { 0x40E237, 0x00 },
    { 0x40FA22, 0x00 }, { 0x40FA45, 0x00 },
    { 0x40BFB5, 0x04 }, { 0x40C60A, 0x04 }, { 0x40C62A, 0x04 }, { 0x40C65A, 0x04 }, { 0x40C9D7, 0x04 },
    { 0x40BC5F, 0x08 }, { 0x40D400, 0x0C }, { 0, 0 },
};
static const PadBound PihBounds[] = {
    { 0x40C09D, 0x00 }, { 0x40C2E1, 0x00 }, { 0x40FAD2, 0x00 },      /* loops that walk the entries themselves */
    { 0x40BFF6, 0x04 }, { 0x40CBC2, 0x04 },                          /* loops that walk the state field */
    { 0x40BD20, 0x08 },                                              /* the one that gives each device its buffers */
    { 0, 0 },
};

static const PadAddresses Addresses[GameCount] = {
    [GamePIH] = { 0x5B5418, 2, 0x28, PihFields, PihBounds, 0x40FCC0, 0x40E500, 0x14, 0x28, 0x4E4910 },
    [GameTTB] = { 0 },   /* its own numbers come once the first game is proven in play */
};
static const PadAddresses *A;

typedef struct { const void **Vtable; } PadDevice;

static PadDevice Pad;
static const void *PadVtable[20];
static float PadValues[PadInputCount];
static unsigned short PadNames[PadInputCount][3];   /* rows of (0xE000 + input, string id, extra), as the keyboard's */
static char MovedArray[0x28 * 8];                   /* the array's new home, room to spare */
static float SdlNow[PadInputCount];   /* the pad as RiotFix reads it every frame, for the watch below */
static int SwapSticks, InvertMoveX, InvertMoveY, InvertLookX, InvertLookY;
static float PadDeadZone = 6000.0f / 32767.0f;
static int PadOn, Watching, MenuExtras = 1, Bindings = 1, BindGroups = 31, AppendRows, AnalogWalk = 1;
static unsigned NameStringId = 0xFFFF;   /* [Controller] NameStringId: see BuildVtable */

/* What the Xbox build multiplies a motor level by before it hands it to XInput (the float at its 0x2A7F80), instead
 * of the 65535 a full motor would be. See SlotRumble. */
#define XboxRumbleCeiling 45874.5f

/* ======================================================================== The values
 *
 * The Xbox's own shaping: each axis over 32767, an axis inside the dead zone zeroed, then the stick's length taken
 * and, past the dead zone, what is left of it stretched back over the whole range. Each stick then goes in as its
 * four halves, which is how the console's records read it. */
static void StickHalves(int AxisX, int AxisY, float *Out) {
    float X = (float)RiotSdlAxis(AxisX) / 32767.0f, Y = (float)RiotSdlAxis(AxisY) / 32767.0f;
    if (X > -PadDeadZone && X < PadDeadZone) X = 0.0f;
    if (Y > -PadDeadZone && Y < PadDeadZone) Y = 0.0f;
    float Length = __builtin_sqrtf(X * X + Y * Y);
    if (Length <= PadDeadZone || PadDeadZone >= 1.0f) { Out[0] = Out[1] = Out[2] = Out[3] = 0.0f; return; }
    const float Scale = (Length - PadDeadZone) / (1.0f - PadDeadZone) / Length;
    X *= Scale; Y *= Scale;
    if (X > 1.0f) X = 1.0f;
    if (X < -1.0f) X = -1.0f;
    if (Y > 1.0f) Y = 1.0f;
    if (Y < -1.0f) Y = -1.0f;
    Out[0] = Y < 0.0f ? -Y : 0.0f;    /* up */
    Out[1] = Y > 0.0f ?  Y : 0.0f;    /* down */
    Out[2] = X < 0.0f ? -X : 0.0f;    /* left */
    Out[3] = X > 0.0f ?  X : 0.0f;    /* right */
}

static float Largest(const float *From, int Count) {
    float Best = 0.0f;
    for (int Index = 0; Index < Count; Index++) if (From[Index] > Best) Best = From[Index];
    return Best;
}

static void ReadPad(float *Values) {
    static const unsigned char Buttons[][2] = {
        { PadStart, SdlStart }, { PadBack, SdlBack },
        { PadDpadUp, SdlDpadUp }, { PadDpadDown, SdlDpadDown },
        { PadDpadLeft, SdlDpadLeft }, { PadDpadRight, SdlDpadRight },
        { PadNorth, SdlNorth }, { PadWest, SdlWest }, { PadEast, SdlEast }, { PadSouth, SdlSouth },
        { PadLeftStickButton, SdlLeftStick }, { PadRightStickButton, SdlRightStick },
        { PadLeftShoulder, SdlLeftShoulder }, { PadRightShoulder, SdlRightShoulder },
    };
    if (!RiotSdlHasPad()) { memset(Values, 0, sizeof PadValues); return; }
    for (size_t Index = 0; Index < sizeof Buttons / sizeof *Buttons; Index++)
        Values[Buttons[Index][0]] = RiotSdlButton(Buttons[Index][1]) ? 1.0f : 0.0f;
    StickHalves(SdlLeftX, SdlLeftY, &Values[PadLeftUp]);
    StickHalves(SdlRightX, SdlRightY, &Values[PadRightUp]);
    Values[PadLeftTrigger] = (float)RiotSdlAxis(SdlLeftTrigger) / 32767.0f;
    Values[PadRightTrigger] = (float)RiotSdlAxis(SdlRightTrigger) / 32767.0f;
    Values[PadLeftStickAny] = Largest(&Values[PadLeftUp], 4);
    Values[PadRightStickAny] = Largest(&Values[PadRightUp], 4);
    Values[PadDpadAny] = Largest(&Values[PadDpadUp], 4);
    /* Walking by pushing the stick part way, which is how the console does it and what the keyboard does with
     * its walk key. The engine already has the machinery: the player's speed is 1 minus the value of the walk
     * action (Suffering.rfl+0x10045E13, 'fsubr 1.0'), and if that action is bound to nothing it falls back to a
     * default. So the pad hands it 1 minus how far the move stick is pushed, and the subtraction gives the push
     * back as the speed: all the way is 1 and runs, half way is 0.5 and walks.
     *
     * At rest it must report 0, not 1. The engine takes the largest value among an action's records, so a resting
     * stick reporting 1 would hold the speed at zero for the keyboard as well, and walking on foot would stop
     * working the moment a pad was plugged in. Zero at rest leaves the keyboard's own walk key deciding. */
    const float *const Move = SwapSticks ? &Values[PadRightUp] : &Values[PadLeftUp];
    const float Across = Move[2] + Move[3], Along = Move[0] + Move[1];   /* only one of each pair is ever set */
    float Push = sqrtf(Across * Across + Along * Along);
    if (Push > 1.0f) Push = 1.0f;
    Values[PadWalkAmount] = Push > 0.0f ? 1.0f - Push : 0.0f;
}

/* ======================================================================== The layouts
 *
 * A layout is a list of (action, input) pairs, which is what the console's own layout tables are: the five of them
 * in the Xbox executable have exactly this shape, with the actions renumbered (Docs/XboxInput.md section 6). The
 * names of the actions below are the PC build's own ids, read out of the bindings in force in play.
 *
 * An action appears more than once on purpose. Cross is Jump in play and Accept in a menu; the d-pad moves between
 * menu items, cycles the weapons and opens the inventory. The engine asks about whichever action its own state
 * calls for, so the same button does the right thing in each place, exactly as it does on the console. */
/* A row's Role says whether it is one of the stick directions that [Controller] SwapSticks and the inversions act
 * on: the menu rows use the left stick too, and a player who inverts the look does not mean to invert a menu. */
enum { RolePlain, RoleMove, RoleLook, RoleMenuExtra };
typedef struct { DWORD Action; unsigned char Input, Role; } PadBinding;

/* The layouts, as the two consoles lay them out. A layout is nine gameplay buttons and the way the sticks move the
 * player; everything else - the menus, the d-pad, the inventory - is the same in all of them, so it is written once
 * in CommonRows below and added to each.
 *
 * The five PS2 ones come from the game's own Controller Settings screens, and the five Xbox ones from the tables in
 * the Xbox executable (Docs/XboxInput.md section 3). They are not the same: the PS2 puts Use on cross and Attack on
 * R1, the Xbox puts Jump on A and Attack on the right trigger, so a layout of one console played on the other feels
 * like several buttons at once are wrong. The PS2 ones come first, and Double Stick is the default, because that is
 * the console The Suffering was played on. */
enum { NoInput = 0xFF };

typedef struct {
    const char *Name;
    unsigned char Attack, Jump, Throw, Reload, Use, Health, FirstPerson, Insanity, Flashlight;
    unsigned char StrafeLeft, StrafeRight;   /* set only where the triggers strafe and the left stick turns */
} PadLayout;

#define T PadLeftTrigger
#define U PadRightTrigger
static const PadLayout Layouts[] = {
    /* name                     attack  jump  throw reload  use  health  1st   insanity  flash   strafe l/r */
    { "doublestick",            PadRightShoulder, U, PadLeftShoulder, T, PadSouth, PadEast, PadWest, PadNorth,
      NoInput, NoInput, NoInput },
    { "doublestickflashlight",  PadRightShoulder, U, PadLeftShoulder, T, PadSouth, PadEast, PadWest, PadNorth,
      PadRightStickButton, NoInput, NoInput },
    { "throwjumpswap",          PadRightShoulder, PadLeftShoulder, U, PadWest, PadSouth, PadEast, T, PadNorth,
      NoInput, NoInput, NoInput },
    { "jumpontop",              PadRightShoulder, PadSouth, PadLeftShoulder, PadEast, PadWest, T, U, PadNorth,
      NoInput, NoInput, NoInput },
    { "singlestick",            PadWest, PadSouth, PadEast, PadRightStickButton, PadNorth, PadLeftStickButton,
      PadLeftShoulder, PadRightShoulder, NoInput, T, U },
    /* The Xbox build's own five, with its Black and White buttons on the shoulders of a modern pad. */
    { "xboxdoublestick",        U, PadSouth, T, PadEast, PadWest, PadLeftShoulder, PadRightShoulder, PadNorth,
      NoInput, NoInput, NoInput },
    { "xboxdoublestickflashlight", U, PadSouth, T, PadEast, PadWest, PadLeftShoulder, PadRightShoulder, PadNorth,
      PadRightStickButton, NoInput, NoInput },
    { "xboxthrowjumpswap",      U, T, PadWest, PadEast, PadSouth, PadLeftShoulder, PadRightShoulder, PadNorth,
      NoInput, NoInput, NoInput },
    { "xboxjumpontop",          U, PadRightShoulder, T, PadLeftShoulder, PadSouth, PadEast, PadWest, PadNorth,
      NoInput, NoInput, NoInput },
    { "xboxsinglestick",        PadWest, PadSouth, PadEast, PadRightStickButton, PadNorth, PadLeftStickButton,
      PadRightShoulder, PadLeftShoulder, NoInput, T, U },
};
#undef T
#undef U

/* The rows every layout has: the menus, the d-pad and the inventory. */
static const PadBinding CommonRows[] = {
    { 0x3EA, PadStart, RolePlain },           /* pause */
    { 0x3FF, PadBack, RolePlain },            /* map */
    { 0x402, PadSouth, RolePlain },           /* a menu's accept: the keyboard reaches it with Space and Enter */
    { 0x403, PadNorth, RolePlain },           /* a menu's cancel: the keyboard reaches it with Escape */
    /* Going into a menu item is an action of its own, which the keyboard reaches with Enter and the console with
     * its accept button and Start (both of them, in the Xbox table). Without it the pad could move through a menu
     * and not open anything.
     * These four are the ones 0.3.8 added, and they are the only difference between the last build that played a
     * whole level and the ones that crash in the engine's screen walk. [Controller] MenuExtras=0 leaves them out,
     * which is the one controlled test that tells a record-count problem apart from a record-content one. */
    { 0x3FC, PadSouth, RoleMenuExtra }, { 0x3FC, PadStart, RoleMenuExtra },
    /* The right stick scrolls a long list, where the mouse would use its wheel. */
    { 0x3F2, PadRightUp, RoleMenuExtra }, { 0x3F3, PadRightDown, RoleMenuExtra },
    { 0x3EC, PadDpadUp, RolePlain }, { 0x3ED, PadDpadDown, RolePlain },       /* moving between menu items */
    { 0x3EE, PadDpadLeft, RolePlain }, { 0x3EF, PadDpadRight, RolePlain },
    { 0x3EC, PadLeftUp, RolePlain }, { 0x3ED, PadLeftDown, RolePlain },       /* the left stick moves there too */
    { 0x3EE, PadLeftLeft, RolePlain }, { 0x3EF, PadLeftRight, RolePlain },
    { 0x43F, PadDpadLeft, RolePlain }, { 0x440, PadDpadRight, RolePlain },    /* the weapon in hand */
    { 0x441, PadDpadUp, RolePlain }, { 0x442, PadDpadDown, RolePlain },       /* the thrown weapon */
    { 0x42E, PadDpadAny, RolePlain },         /* the inventory */
    { 0, 0, 0 },
};

static PadBinding BuiltRows[64];
static const PadBinding *PadRows = BuiltRows;
static const PadLayout *PadMap = &Layouts[0];

/* [Controller] BindGroups, a bisecting switch: bit 0 walking, bit 1 the camera, bit 2 the action buttons, bit 3
 * everything the layouts share - the menu, the pause, the map, the d-pad. 15 is all of them, which is what the
 * port installs normally. It exists because a crash was traced as far as "something that is bound", and the way
 * to the rest is to leave out one group at a time rather than to reason about which row looks suspect. */
static void BuildRows(void) {
    const PadLayout *L = PadMap;
    int Out = 0;
    const int Walk = (BindGroups & 1) != 0, Camera = (BindGroups & 2) != 0;
    const int Fight = (BindGroups & 4) != 0, Shared = (BindGroups & 8) != 0;
    const int Sideways = (BindGroups & 16) != 0;   /* strafing and turning, split off from walking */
    const struct { DWORD Action; unsigned char Input; } Buttons[] = {
        { 0x428, L->Attack }, { 0x42A, L->Jump }, { 0x429, L->Throw }, { 0x425, L->Reload }, { 0x42B, L->Use },
        { 0x430, L->Health }, { 0x42C, L->FirstPerson }, { 0x42D, L->Insanity }, { 0x426, L->Flashlight },
    };
    /* Walking and turning. With the triggers on the strafe, the left stick turns instead, which is what the single
     * stick layouts are. */
    const int Turning = L->StrafeLeft != NoInput;
    if (Walk) {
    /* Walking by how far the stick is pushed. The engine's speed is 1 minus this action's value, and it only
       asks when the action is bound at all, so this row is what turns the whole thing on. */
    if (AnalogWalk) BuiltRows[Out++] = (PadBinding){ 0x411, PadWalkAmount, RolePlain };
    BuiltRows[Out++] = (PadBinding){ 0x409, PadLeftUp, RoleMove };
    BuiltRows[Out++] = (PadBinding){ 0x40A, PadLeftDown, RoleMove };
    if (!Sideways) { /* nothing sideways */ }
    else if (Turning) {
        BuiltRows[Out++] = (PadBinding){ 0x40B, PadLeftLeft, RoleMove };
        BuiltRows[Out++] = (PadBinding){ 0x40C, PadLeftRight, RoleMove };
        BuiltRows[Out++] = (PadBinding){ 0x40F, L->StrafeLeft, RolePlain };
        BuiltRows[Out++] = (PadBinding){ 0x410, L->StrafeRight, RolePlain };
    } else {
        BuiltRows[Out++] = (PadBinding){ 0x40F, PadLeftLeft, RoleMove };
        BuiltRows[Out++] = (PadBinding){ 0x410, PadLeftRight, RoleMove };
    }
    }
    if (Camera) {
    BuiltRows[Out++] = (PadBinding){ 0x41C, PadRightUp, RoleLook };
    BuiltRows[Out++] = (PadBinding){ 0x41D, PadRightDown, RoleLook };
    BuiltRows[Out++] = (PadBinding){ 0x40B, PadRightLeft, RoleLook };
    BuiltRows[Out++] = (PadBinding){ 0x40C, PadRightRight, RoleLook };
    }
    if (Fight) for (size_t Index = 0; Index < sizeof Buttons / sizeof *Buttons; Index++)
        if (Buttons[Index].Input != NoInput)
            BuiltRows[Out++] = (PadBinding){ Buttons[Index].Action, Buttons[Index].Input, RolePlain };
    if (Shared) for (const PadBinding *Row = CommonRows; Row->Action; Row++) {
        if (Row->Role == RoleMenuExtra && !MenuExtras) continue;
        BuiltRows[Out++] = *Row;
    }
    BuiltRows[Out].Action = 0;
}

/* ======================================================================== The 20 slots */
static void *ThisCall SlotDestroy(void *Self, int Flags) { (void)Flags; return Self; }
/* The engine keeps a state per device and only polls a device whose state is 2. The state is whatever slots 1 and 6
 * return, so both have to answer 2, not merely "yes":
 *
 *   state 0: it calls slot 5 (is it open); anything but zero moves the device to state 1
 *   state 1: it calls slot 6 (open it) and makes the ANSWER the new state, so an answer of 1 leaves it at state 1
 *            and it is asked again the next frame, for ever
 *   state 2: it polls the device (slot 7) twice a frame
 *
 * The first build of this answered 1 from slot 6. The pad started at state 2, because slot 1 answers 2, and worked
 * in the menus; then loading a level put the devices back to state 0, the pad climbed to state 1 and stayed there,
 * and from that moment the engine never read it again - the pad was dead in play and nowhere else. */
static int ThisCall SlotStart(void *Self) { (void)Self; return 2; }   /* 2 = ready, the answer the keyboard gives */
static int ThisCall SlotInputs(void *Self) { (void)Self; return PadInputCount; }
static int ThisCall SlotBindable(void *Self) { (void)Self; return PadInputCount; }
static void ThisCall SlotClose(void *Self) { (void)Self; }
static int ThisCall SlotIsOpen(void *Self) { (void)Self; return 1; }
static int ThisCall SlotOpen(void *Self) { (void)Self; return 2; }   /* 2 = ready, and nothing less will do */
static volatile DWORD PollCount;   /* how often the engine asks the pad for its values */
static int ThisCall SlotPoll(void *Self, float Seconds, int Reset) {
    (void)Self; (void)Seconds; (void)Reset;
    PollCount++;
    ReadPad(PadValues);
    return 2;
}
static float *ThisCall SlotValues(void *Self) { (void)Self; return PadValues; }
static int ThisCall SlotYes(void *Self) { (void)Self; return 1; }
static const void *ThisCall SlotNameRows(void *Self) { (void)Self; return PadNames; }
static void ThisCall SlotNothing(void *Self, int Argument) { (void)Self; (void)Argument; }
static float ThisCall SlotOne(void *Self) { (void)Self; return 1.0f; }
static float ThisCall SlotRumbleFloor(void *Self) { (void)Self; return 0.05f; }
/* Rumble. The engine's mixer (0x40F7D0) works out the two motor levels for the frame, zeroes either one that falls
 * below the floor slot 18 gives out, copies them into an array of its own and hands that array to this slot on every
 * device in the array (0x40FAC1). What arrives is one of the engine's growable arrays, passed whole on the stack:
 * {the floats, how many fit, how many there are} - which is why the slot clears 12 bytes on return. The three
 * parameters below are those three words; taking them one by one keeps the layout in plain sight and off the
 * compiler's judgement about passing a struct by value.
 *
 * The two levels are the Xbox's own, in its own order: measured in that build's rumble slot (0x1B0F00), level 0 goes
 * to XINPUT_FEEDBACK's left motor and level 1 to its right, which are SDL's low and high frequency ones. That build
 * also scales both by 45874.5 rather than by 65535, holding its motors to seven tenths of what they can do; this is
 * the console's own ceiling, so RiotFix keeps it. */
static volatile DWORD RumbleCount;         /* how often the mixer reaches us, and the loudest it has asked for */
static volatile float RumbleLow, RumbleHigh;
static void ThisCall SlotRumble(void *Self, const float *Levels, int Capacity, int Count) {
    (void)Self; (void)Capacity;
    if (!Levels || Count < 2) return;
    RumbleCount++;
    float Low = Levels[0], High = Levels[1];
    if (Low > RumbleLow) RumbleLow = Low;
    if (High > RumbleHigh) RumbleHigh = High;
    if (!(Low > 0.0f)) Low = 0.0f;                     /* also catches a NaN, which would cast to anything */
    if (!(High > 0.0f)) High = 0.0f;
    if (Low > 1.0f) Low = 1.0f;
    if (High > 1.0f) High = 1.0f;
    RiotSdlRumble((unsigned)(Low * XboxRumbleCeiling), (unsigned)(High * XboxRumbleCeiling));
}

/* The Xbox's thresholds: a button counts as pressed past a fifth, a trigger past 0.55, a stick half at any value. */
static float ThisCall SlotThreshold(void *Self, int Input) {
    (void)Self;
    if (Input == PadLeftTrigger || Input == PadRightTrigger) return 0.55f;
    if (Input >= PadLeftUp && Input <= PadRightRight) return 0.0f;
    if (Input >= PadLeftStickAny) return 0.0f;
    return 0.2f;
}

static void BuildVtable(void) {
    PadVtable[0] = (const void *)SlotDestroy;      PadVtable[10] = (const void *)SlotYes;
    PadVtable[1] = (const void *)SlotStart;        PadVtable[11] = (const void *)SlotYes;
    PadVtable[2] = (const void *)SlotInputs;       PadVtable[12] = (const void *)SlotYes;
    PadVtable[3] = (const void *)SlotBindable;     PadVtable[13] = (const void *)(size_t)A->BorrowedSlot13;
    PadVtable[4] = (const void *)SlotClose;        PadVtable[14] = (const void *)(size_t)A->BorrowedSlot14;
    PadVtable[5] = (const void *)SlotIsOpen;       PadVtable[15] = (const void *)SlotNameRows;
    PadVtable[6] = (const void *)SlotOpen;         PadVtable[16] = (const void *)SlotNothing;
    PadVtable[7] = (const void *)SlotPoll;         PadVtable[17] = (const void *)SlotOne;
    PadVtable[8] = (const void *)SlotValues;       PadVtable[18] = (const void *)SlotRumbleFloor;
    PadVtable[9] = (const void *)SlotThreshold;    PadVtable[19] = (const void *)SlotRumble;
    /* A name row is the code in the first word and then a 32-bit string id, not two words: the reader at 0x40C690
     * takes a dword from the row's second byte (`mov eax, [eax+2]`) and treats -1 as "this input has no name",
     * which is why the keyboard's own nameless row 0 carries 0xFFFF in both of its last words. Writing 0xFFFF in
     * the first of them alone leaves the id 0x0000FFFF - a number the game accepts, looks up in a string table
     * that has no such entry, and then calls a method on what it finds, which is how a pad-bound menu action
     * crashed the screen drawing. Both words, or neither. */
    /* Our inputs have no names, and the game's two devices name every one of theirs. That difference is not idle:
     * with -1 the reader above hands its caller a null string instead of a string, and only a screen that shows
     * the name of a bound input ever asks - which is why a crash can wait in one room and not another. Until the
     * console's own names are read out (stage 6 of Docs/PadPort.md), [Controller] NameStringId answers with real
     * strings: input 0 gets that id, input 1 the next, and so on up the list.
     *
     * Running ids rather than one repeated id is what makes this a measurement. The game resolves these names
     * correctly for its own devices - a hint reads "Use Mouse4, Enter or E to open lockers" - and the numbers
     * below are the ones it uses, measured in the running game rather than deduced from a file.
     *
     * How a name is found, all of it confirmed in memory: the row's 32-bit field is an index into an array the
     * string manager keeps at [manager + 8], reached as [[[0x5B5110]+0xA0]]; the lookup is that array's element,
     * nothing more (0x45E27F is "return base + Id*4"). The element is not the text - it is the string's own
     * composite id, (table << 16) | entry, which is exactly the number SurrealEd shows: the keyboard's row for
     * the left arrow holds 0xE2, the array's entry 0xE2 holds 0x000200A8, and 2_00a8 in the editor is
     * "Lft Arrow". Two days of trying to match the editor's numbering against the row's failed because they are
     * two different numbers with a table in between.
     *
     * The array already carries the controller's names, twice over - the console had two pads - and the block
     * below is the first copy. Four indices in the middle of it, 0x13 to 0x16, are empty, and they fall exactly
     * where the four face buttons belong: on the console those are drawn as icons rather than spelled, so there
     * is no text for them. They are left nameless here, as is the walk input, which is not a control at all. */
    static const unsigned short NameIds[PadInputCount] = {
        0x05,   /* Start                 "START button"              */
        0x06,   /* Back                  "SELECT button"             */
        0x07, 0x08, 0x09, 0x0A,   /* left stick   up, down, left, right   */
        0x0B, 0x0C, 0x0D, 0x0E,   /* right stick  up, down, left, right   */
        0x0F, 0x10, 0x11, 0x12,   /* d-pad        up, down, left, right   */
        0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,   /* the four face buttons: icons on the console, no text here */
        0x18,   /* left trigger          "L2 button"                 */
        0x1B,   /* right trigger         "R2 button"                 */
        0x19,   /* left stick button     "L3 button"                 */
        0x1C,   /* right stick button    "R3 button"                 */
        0x17,   /* left shoulder         "L1 button"                 */
        0x1A,   /* right shoulder        "R1 button"                 */
        0x1D,   /* left stick, any way   "left analog stick"         */
        0x20,   /* right stick, any way  "right analog stick"        */
        0x23,   /* d-pad, any way        "directional button"        */
        0xFFFF  /* the walk amount is not a control and is never named */
    };
    for (int Input = 0; Input < PadInputCount; Input++) {
        unsigned Id = NameIds[Input];
        if (NameStringId != 0xFFFF) Id = NameStringId + (unsigned)Input;   /* for trying a block by hand */
        if (Id == 0xFFFF) Id = 0xFFFFFFFFu;
        PadNames[Input][0] = (unsigned short)(0xE000 + Input);
        PadNames[Input][1] = (unsigned short)(Id & 0xFFFF);
        PadNames[Input][2] = (unsigned short)(Id >> 16);
    }
    Pad.Vtable = PadVtable;
}

/* ======================================================================== Moving the array
 *
 * Nothing here is written unless every site holds what it is expected to hold, and nothing is left written unless
 * the executable's code is afterwards free of any address inside the old array. A reference left behind would mean
 * one loop still reading the old two entries while the rest read the new three, which is the kind of fault that
 * shows up as a hang rather than as a message, so it is checked rather than trusted. */
static DWORD Expected(DWORD Offset) { return A->Array + Offset; }
static DWORD ExpectedBound(DWORD Start) { return A->Array + Start + A->EntrySize * (DWORD)A->Entries; }

/* Any four bytes in the game's own code that still read as an address inside the old array. */
static int LeftoverReferences(DWORD *Where, int Room) {
    const char *Module = (const char *)GetModuleHandleA(NULL);
    const IMAGE_DOS_HEADER *Dos = (const IMAGE_DOS_HEADER *)Module;
    const IMAGE_NT_HEADERS *Headers = (const IMAGE_NT_HEADERS *)(Module + Dos->e_lfanew);
    const IMAGE_SECTION_HEADER *Section = IMAGE_FIRST_SECTION(Headers);
    const DWORD Low = A->Array, High = A->Array + A->EntrySize * (DWORD)(A->Entries + 1);
    int Found = 0;
    for (int Index = 0; Index < Headers->FileHeader.NumberOfSections; Index++, Section++) {
        if (!(Section->Characteristics & IMAGE_SCN_CNT_CODE)) continue;
        const unsigned char *At = (const unsigned char *)(Module + Section->VirtualAddress);
        for (DWORD Offset = 0; Offset + 4 <= Section->Misc.VirtualSize; Offset++) {
            DWORD Value;
            memcpy(&Value, At + Offset, sizeof Value);
            if (Value < Low || Value >= High) continue;
            if (Found < Room) Where[Found] = (DWORD)(size_t)(At + Offset);
            Found++;
        }
    }
    return Found;
}

static int MoveArray(void) {
    const size_t Size = (size_t)A->EntrySize * A->Entries;
    memcpy(MovedArray, (const void *)(size_t)A->Array, Size);
    memset(MovedArray + Size, 0, A->EntrySize);
    *(void **)(MovedArray + Size) = &Pad;
    const DWORD New = (DWORD)(size_t)MovedArray;

    /* Every site has to hold the address it is expected to hold before anything is written. */
    for (const PadField *Field = A->Fields; Field->Site; Field++)
        if (*(const DWORD *)(size_t)Field->Site != Expected(Field->Offset)) {
            Log("Controller: %#lx holds %#lx, not the expected %#lx, so the device array is left alone",
                Field->Site, *(const DWORD *)(size_t)Field->Site, Expected(Field->Offset));
            return 0;
        }
    for (const PadBound *Bound = A->Bounds; Bound->Site; Bound++)
        if (*(const DWORD *)(size_t)Bound->Site != ExpectedBound(Bound->Start)) {
            Log("Controller: %#lx holds %#lx, not the expected end %#lx, so the device array is left alone",
                Bound->Site, *(const DWORD *)(size_t)Bound->Site, ExpectedBound(Bound->Start));
            return 0;
        }

    int Written = 0;
    for (const PadField *Field = A->Fields; Field->Site; Field++) {
        const DWORD Value = New + Field->Offset;
        if (WriteCode(Field->Site, &Value, sizeof Value)) Written++;
    }
    for (const PadBound *Bound = A->Bounds; Bound->Site; Bound++) {
        const DWORD Value = New + Bound->Start + A->EntrySize * (DWORD)(A->Entries + 1);
        if (WriteCode(Bound->Site, &Value, sizeof Value)) Written++;
    }

    DWORD Left[8];
    const int Leftovers = LeftoverReferences(Left, 8);
    if (Leftovers) {
        for (int Index = 0; Index < Leftovers && Index < 8; Index++)
            Log("Controller: %#lx still points into the old device array", Left[Index]);
        for (const PadField *Field = A->Fields; Field->Site; Field++) {
            const DWORD Value = Expected(Field->Offset); WriteCode(Field->Site, &Value, sizeof Value);
        }
        for (const PadBound *Bound = A->Bounds; Bound->Site; Bound++) {
            const DWORD Value = ExpectedBound(Bound->Start); WriteCode(Bound->Site, &Value, sizeof Value);
        }
        Log("Controller: %d reference(s) to the old device array were left over, so it was put back as it was",
            Leftovers);
        return 0;
    }
    Log("Controller: the device array moved from %#lx to %p with room for %d devices: %d references, none left over",
        A->Array, (void *)MovedArray, A->Entries + 1, Written);
    return 1;
}

/* ======================================================================== Putting the pad in the bindings
 *
 * The manager reads two tables, the one at +0x28 first. Both are in use, so the pad's rows go into a copy of the
 * first with its own rows added, and the copy is installed in its place. Where a row goes matters: the engine finds
 * the first row of an action and then walks forward only while the rows keep that action, so a row added at the end
 * of the table would never be reached. Each pad row therefore goes right after the game's last row for the same
 * action, and an action the game binds to nothing at all gets a group of its own at the end.
 *
 * The table belongs to the game, which builds a new one whenever the bindings are loaded or changed, so this is
 * checked every frame: when the manager is reading a table that is not ours, ours is built again from it. That way
 * a player who edits the controls in the game's own menu keeps both their keys and the pad. */
typedef struct { DWORD Action, Device, Code; float Low, High; } ControlRecord;

typedef void *(__cdecl *AllocateType)(int, size_t);

/* The table is built in the game's own memory rather than in RiotFix's, because the game may well free the table it
 * finds at +0x28 when it builds a new one, and memory from the game's allocator is memory it may free. */
static ControlRecord *OurRecords;
static int OurRecordCount, FirstPadRow = -1;

/* The player's own [Controller] choices, applied where they belong: to the stick a role reads from, and to the pair
 * of halves within it. Everything else about a row is left alone. */

/* [Controller] ForceCode: every row is given this input code instead of its own. The crash jumps to an address
 * equal to the code of the row bound to walking backwards - 3 with the left stick, 7 with the sticks swapped -
 * so the engine is reaching our code field and treating it as somewhere to go. One code of our choosing settles
 * whether that reading is right: the jump should land on whatever is put here. 0xFF means "leave the rows as
 * they are". */
static int ForceCode = 0xFF;
static unsigned char InputFor(const PadBinding *Row) {
    if (ForceCode != 0xFF) return (unsigned char)ForceCode;
    if (Row->Role == RolePlain) return Row->Input;
    int Half = Row->Input - (Row->Role == RoleMove ? PadLeftUp : PadRightUp);   /* 0 up, 1 down, 2 left, 3 right */
    const int InvertY = Row->Role == RoleMove ? InvertMoveY : InvertLookY;
    const int InvertX = Row->Role == RoleMove ? InvertMoveX : InvertLookX;
    if (Half < 2) { if (InvertY) Half ^= 1; } else if (InvertX) Half ^= 1;
    int Stick = Row->Role == RoleMove ? PadLeftUp : PadRightUp;
    if (SwapSticks) Stick = Stick == PadLeftUp ? PadRightUp : PadLeftUp;
    return (unsigned char)(Stick + Half);
}

static int RowsOfAction(DWORD Action, ControlRecord *Out, int Room) {
    int Added = 0;
    for (const PadBinding *Row = PadRows; Row->Action; Row++) {
        if (Row->Action != Action || Added >= Room) continue;
        Out[Added].Action = Action;
        Out[Added].Device = (DWORD)A->Entries;   /* the pad is the entry after the game's own devices */
        Out[Added].Code = InputFor(Row);
        Out[Added].Low = 0.0f;
        Out[Added].High = 1.0f;
        Added++;
    }
    return Added;
}

static int TableHolds(const ControlRecord *Table, int Count, DWORD Action) {
    for (int Index = 0; Index < Count; Index++) if (Table[Index].Action == Action) return 1;
    return 0;
}

/* Whether the table the manager is reading is the one we built. The address alone would not say so: if the game
 * freed ours and its allocator handed the same block back for a table of its own, the address would match while the
 * contents would not. So the pad row we know the position of is checked as well. */
static int TableIsOurs(const ControlRecord *Table) {
    return Table && Table == OurRecords && FirstPadRow >= 0 && FirstPadRow < OurRecordCount &&
           Table[FirstPadRow].Device == (DWORD)A->Entries && Table[OurRecordCount].Action == 0xFFFFFFFF;
}

static void InstallRecords(void) {
    void *Manager = RiotInputManager();
    if (!Manager) return;
    const ControlRecord **Slot = (const ControlRecord **)((char *)Manager + A->TableSlot);
    const ControlRecord *Live = *Slot;
    if (!Live || TableIsOurs(Live)) return;   /* nothing to do while the manager is reading ours */

    int Count = 0;
    while (Count < 400 && Live[Count].Action != 0xFFFFFFFF) Count++;
    int Ours = 0;
    for (const PadBinding *Row = PadRows; Row->Action; Row++) Ours++;
    const int Room = Count + Ours;
    ControlRecord *Built = (ControlRecord *)((AllocateType)(size_t)A->Allocate)(0, (size_t)(Room + 1) * A->RecordSize);
    if (!Built) { Log("Controller: no memory for the bindings table, so the pad is bound to nothing"); return; }
    OurRecords = Built;
    FirstPadRow = -1;
    int Out = 0, GroupStart = 0;
    for (int Index = 0; Index < Count && Out < Room; Index++) {
        /* The rows of an action come in groups, and the engine reads the first group only. So the test is whether
         * this group is the action's first - which means looking at what comes before the group, not before the row:
         * the rows of this same group are of this same action, and counting them as an earlier mention is what left
         * every action with more than one key - move, attack, use, accept - without the pad. */
        if (Index == 0 || Live[Index].Action != Live[Index - 1].Action) GroupStart = Index;
        Built[Out++] = Live[Index];
        const int LastOfGroup = Index + 1 == Count || Live[Index + 1].Action != Live[Index].Action;
        if (!AppendRows && LastOfGroup && !TableHolds(Live, GroupStart, Live[Index].Action)) {
            const int Added = RowsOfAction(Live[Index].Action, &Built[Out], Room - Out);
            if (Added && FirstPadRow < 0) FirstPadRow = Out;
            Out += Added;
        }
    }
    /* [Controller] AppendRows puts every row after the game's table instead of inside its action's first group.
     * The engine reads only an action's first group, so a bound action whose group is earlier in the table will
     * not answer this way - the pad goes half dead on purpose. What it tests is the other half: inserting rows
     * moves every record after them, and if anything in the engine remembers where an action's records were, that
     * memory is now wrong. A crash that goes away here is caused by the moving, not by the rows. */
    const int AfterTheGames = Out;   /* where our own rows begin, for the append path's own duplicate test */
    for (const PadBinding *Row = PadRows; Row->Action && Out < Room; Row++)
        if (AppendRows ? !TableHolds(&Built[AfterTheGames], Out - AfterTheGames, Row->Action)
                       : (!TableHolds(Live, Count, Row->Action) && !TableHolds(Built, Out, Row->Action))) {
            const int Added = RowsOfAction(Row->Action, &Built[Out], Room - Out);
            if (Added && FirstPadRow < 0) FirstPadRow = Out;
            Out += Added;
        }
    Built[Out].Action = 0xFFFFFFFF;
    OurRecordCount = Out;
    *Slot = Built;

    /* Every row of the layout has to be in the table, and in the action's first group. A row that is not there is a
     * button that does nothing, which is quiet enough to be missed in play, so it is counted here instead. */
    static int Missing = 0;   /* kept across calls: this runs every frame, and 8 lines in all is the point */
    for (const PadBinding *Row = PadRows; Row->Action; Row++) {
        int Present = 0;
        for (int Index = 0; Index < Out; Index++)
            if (Built[Index].Action == Row->Action && Built[Index].Device == (DWORD)A->Entries &&
                Built[Index].Code == InputFor(Row)) { Present = 1; break; }
        if (!Present && Missing++ < 8) Log("Controller: action %#lx did not get its row, so that button does nothing",
                                           Row->Action);
    }

    static int Told;
    if (!Told) {
        Told = 1;
        Log("Controller: the pad is bound with %d records beside the game's %d%s", Out - Count, Count,
            Missing ? ", but some rows did not get in (see above)" : ", all of the layout's rows in place");
    }
}

static void ReportWatch(void);

/* ================================================================ Letting the pad shake
 * The engine mixes rumble every frame in the input manager's slot 29 (SUFFERING.EXE+0x40F7D0), and for every
 * device in the array it asks whether that device is in a short list the manager keeps inline:
 *
 *     0040FA51  mov  edx, [ebx+0x74]   ; how long the list is
 *     0040FA5A  lea  ecx, [ebx+0x6c]   ; the list itself, two slots
 *     0040FA60  cmp  edi, [ecx]        ; is this device in it?
 *
 * The list holds device indices, is two slots long, and the manager's constructor (0x40DB60) sets both of them
 * to -1. Nothing in the PC build ever puts anything there - the one write to the length is that constructor -
 * which is why the PC version of the game has no rumble at all: the mixer runs, finds no device listed, and
 * never reaches a rumble slot. Measured, not assumed: with the pad in play the engine read it 240 times in two
 * seconds and asked for rumble not once.
 *
 * So the pad puts itself in the list. The engine clears the list back to -1 (0x40C140) when it resets, so this
 * is checked every frame rather than once, in the same place the records are. Nothing else is disturbed: the
 * slots were empty, and the mixer skips a slot of -1 on its own. */
static int RumbleWanted = 1, RumbleListed, RumbleSaid;

static int Readable(const void *At, size_t Size) {
    MEMORY_BASIC_INFORMATION Info;
    if (!At || !VirtualQuery(At, &Info, sizeof Info) || Info.State != MEM_COMMIT) return 0;
    if (Info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return (size_t)((const char *)Info.BaseAddress + Info.RegionSize - (const char *)At) >= Size;
}

/* What a name row carries, read out of the executable's own tables.
 *
 * Four attempts at this were wrong before the useful part was separated out, and the failures are worth keeping
 * because each was a different trap. Asking the string manager at the main menu answered "????" for every id,
 * which is the game's not-found text: the tables are not loaded that early. Hooking the input manager's vtable
 * slot 14 caught nothing, because the hint expander asks the DEVICE's slot 14 (0x40E244 calls it on the device
 * at 0x5B5418 + index*0x28) - one slot number, two classes. Reading the answer as eight-bit text reported every
 * real string as one character, because the game's screen text is sixteen bit (the expander advances its output
 * two bytes a character at 0x40E2B2). And the object reached through [[[0x5B5110]+0xA0]] is not the string
 * manager: hooking its slot 2 returned pieces of Windows DLL names, so that chain belongs to something else.
 *
 * What is settled: the row is {a code, then a 32-bit id}, six bytes, and the device's slot 14 returns
 * "table + input*6" bounded by the input count (0x40E500). The id is not the number the editor shows for the
 * same text, and the step between them is still unknown - so this logs the ids as they are and leaves the
 * resolving to the game. [Debug] DumpStrings. */
static int DumpStrings;

static void DumpDeviceNames(const char *What, DWORD Table, int Rows) {
    const unsigned char *const At = (const unsigned char *)Table;
    if (!Readable(At, (size_t)Rows * 6)) {
        Log("Names: %s has no readable name table at 0x%lX", What, (unsigned long)Table);
        return;
    }
    for (int Row = 0; Row < Rows; Row++) {
        const unsigned Code = *(const unsigned short *)(At + Row * 6);
        const unsigned Id = *(const unsigned *)(At + Row * 6 + 2);
        if (Id == 0xFFFFFFFF) Log("Names: %s row %3d code 0x%04X has no name", What, Row, Code);
        else Log("Names: %s row %3d code 0x%04X id 0x%03X", What, Row, Code, Id);
    }
}

static void DumpStringTableOnce(void) {
    if (!DumpStrings) return;
    DumpStrings = 0;
    Log("Names: the name rows the executable holds, as they are");
    DumpDeviceNames("keyboard", 0x58CD00, 253);
    DumpDeviceNames("mouse", 0x58D2F0, 14);
    for (int Input = 0; Input < PadInputCount; Input++)
        Log("Names: pad      row %3d code 0x%04X id 0x%04X%04X", Input, PadNames[Input][0],
            PadNames[Input][2], PadNames[Input][1]);
}

static void KeepPadInRumbleList(void) {
    if (!RumbleWanted || !PadOn) return;
    unsigned char *const Manager = (unsigned char *)RiotInputManager();
    if (!Manager) return;
    int *const Slots = (int *)(Manager + 0x6C);
    int *const Length = (int *)(Manager + 0x74);
    if (!RumbleSaid) {
        RumbleSaid = 1;
        Log("Controller: the engine's rumble list holds %d slot(s), and they are %d and %d", *Length,
            Slots[0], Slots[1]);
    }
    if (*Length < 1 || *Length > 2) *Length = 1;      /* the class has room for two and no more */
    if (Slots[0] == (int)A->Entries) return;          /* already ours */
    if (Slots[0] == -1 || Slots[0] == (int)A->Entries) {
        Slots[0] = (int)A->Entries;
        if (!RumbleListed++) Log("Controller: the pad is listed with the engine's rumble mixer, which lists "
                                 "nothing of its own on the PC: ok");
    }
}

static void KeepSdlReading(void) {
    RiotSdlReadPad();
    ReadPad(SdlNow);       /* the pad as it is now, whether or not the engine is reading it */
    KeepPadInRumbleList();
    DumpStringTableOnce();
    if (Bindings) InstallRecords();
    if (Watching) ReportWatch();
}

/* ================================================================ The hint that names a controller
 * The game's hints carry markers like <LadderMove>, and the expander at SUFFERING.EXE+0x40DEA0 replaces each
 * with the keys bound to it. For every record of the action it appends that record's code to a list kept per
 * device, and those lists live in one stack array built a few instructions earlier with a count of **two** -
 * the keyboard and the mouse, which are all the devices the PC build can have:
 *
 *     0040E059  push 2                      ; two devices
 *     0040E05B  push 0xC                    ; twelve bytes each
 *     0040E0B8  mov  eax, [edi+4]           ; the record's device
 *     0040E0BB  cmp  eax, -1 / je           ; the only one it refuses
 *     0040E0C0  lea  esi, [eax+eax*2]       ; device * 3 dwords
 *     0040E0F9  mov  ecx, [esp+eax*4+0x80]  ; that device's list, read past the end for device 2
 *     0040E100  mov  [ecx+ebp*4], edx       ; and the code written through it
 *
 * Our pad is device 2. Reading one element past a two-element array hands it whatever the frame holds there, and
 * the code of the record is then written through that. The crash is a jump to an address equal to one of our own
 * input codes, which is exactly the value this line stores, and it needs two actions because <LadderMove> covers
 * 0x409 and 0x40A: the second pass is the one that reaches the return address.
 *
 * It is the game's own bug, uncovered rather than caused - the PC build simply never had a third device. The fix
 * is to make the test match the array that follows it: 'cmp eax, 2' with an unsigned 'jae' refuses device 2 and
 * everything above it, and still refuses -1, which is what the original line was for. Two byte values change and
 * no instruction moves. A hint then names the keyboard, as it did before the pad existed.
 *
 * The branch has to be re-aimed as well, and this is worth spelling out because aiming it wrong hangs the game
 * outright. The original jumps to 0x40E10A, which is **past** the 'add edi, 0x14' that steps to the next record:
 *
 *     0040E107  add edi, 0x14     ; step
 *     0040E10A  cmp [edi], ebp    ; same action? then round again
 *     0040E10C  je  0x40E0B8
 *
 * Refusing a record and landing at 0x40E10A reads the same record again, refuses it again, and never leaves -
 * harmless in the original, where no record carries the -1 it tests for, and an endless loop the moment the test
 * matches something real. So the jump is re-aimed at 0x40E107 (0x4A becomes 0x47), which steps past the refused
 * record first.
 *
 * The address is The Suffering's; Ties That Bind needs its own, so it is left alone until measured. */
static int FixHintDeviceList = 1;

static void FixHintDeviceListNow(void) {
    if (!FixHintDeviceList || CurrentGame != GamePIH) return;
    unsigned char *const At = (unsigned char *)0x40E0BB;
    static const unsigned char Expected[5] = { 0x83, 0xF8, 0xFF, 0x74, 0x4A };   /* cmp eax,-1 / je +0x4A */
    if (memcmp(At, Expected, sizeof Expected) != 0) {
        Log("Controller: the device test at SUFFERING.EXE+0x40E0BB is not the one expected, so the hint that "
            "names a controller is NOT fixed - avoid binding both walking actions");
        return;
    }
    DWORD Protect = 0;
    if (!VirtualProtect(At, sizeof Expected, PAGE_EXECUTE_READWRITE, &Protect)) return;
    At[2] = 0x02;   /* cmp eax, 2  - the number of device lists the expander's stack array really holds */
    At[3] = 0x73;   /* jae         - above or equal, so -1 is still refused along with 2 and up */
    At[4] = 0x47;   /* and to 0x40E107, the 'add edi, 0x14' - see below */
    VirtualProtect(At, sizeof Expected, Protect, &Protect);
    Log("Hints that name a bound key count only the devices their list has room for (the pad no longer writes "
        "past it): ok");
}

void RiotPadApply(void) {
    A = &Addresses[CurrentGame];
    if (!IniInt("Controller", "NativeDevice", 1)) {
        Log("Controller: the pad is not given to the game as a device of its own ([Controller] NativeDevice=0)");
        return;
    }
    if (!A->Array) { Log("Controller: the device array is not known in this game yet, so no native pad"); return; }
    PadDeadZone = (float)IniInt("Controller", "DeadZone", 6000) / 32767.0f;
    if (PadDeadZone < 0.0f || PadDeadZone > 0.9f) PadDeadZone = 6000.0f / 32767.0f;
    char Wanted[64];
    GetPrivateProfileStringA("Controller", "Layout", Layouts[0].Name, Wanted, sizeof Wanted, IniPath);
    for (size_t Index = 0; Index < sizeof Layouts / sizeof *Layouts; Index++)
        if (!_stricmp(Wanted, Layouts[Index].Name)) PadMap = &Layouts[Index];
    if (_stricmp(Wanted, PadMap->Name))
        Log("Controller: there is no layout called \"%s\", so \"%s\" is used", Wanted, PadMap->Name);
    Bindings = IniInt("Controller", "Bindings", 1);
    if (!Bindings) Log("Controller: the pad is a device of the engine but nothing is bound to it ([Controller] "
                       "Bindings=0), which is the port as it stood before any record was installed");
    AppendRows = IniTestInt("Controller", "AppendRows", 0);
    RumbleWanted = IniInt("Controller", "Rumble", 1);
    AnalogWalk = IniInt("Controller", "AnalogWalk", 1);
    DumpStrings = IniInt("Debug", "DumpStrings", 0);
    if (!AnalogWalk) Log("Controller: the stick always runs ([Controller] AnalogWalk=0)");
    FixHintDeviceList = IniInt("Controller", "FixHintDeviceList", 1);
    FixHintDeviceListNow();
    ForceCode = IniInt("Controller", "ForceCode", 0xFF) & 0xFF;
    if (ForceCode != 0xFF) Log("Controller: every row is given input code %#x ([Controller] ForceCode) - a test",
                               ForceCode);
    if (AppendRows) Log("Controller: the pad's rows are put after the game's table instead of inside each action's "
                        "first group ([Controller] AppendRows), so most of them will not answer - this is a test");
    BindGroups = IniInt("Controller", "BindGroups", 31) & 31;
    if (BindGroups != 31)
        Log("Controller: only part of the bindings is installed ([Controller] BindGroups=%d): forward and back %s, "
            "camera %s, buttons %s, shared %s, sideways %s", BindGroups, (BindGroups & 1) ? "yes" : "no",
            (BindGroups & 2) ? "yes" : "no", (BindGroups & 4) ? "yes" : "no", (BindGroups & 8) ? "yes" : "no",
            (BindGroups & 16) ? "yes" : "no");
    MenuExtras = IniTestInt("Controller", "MenuExtras", 1);
    NameStringId = (unsigned)IniTestInt("Controller", "NameStringId", 0xFFFF);
    if (NameStringId != 0xFFFF)
        Log("Controller: every input is named with string %u, which is wrong text on purpose ([Controller] "
            "NameStringId) - ids run upwards from there, one per input", NameStringId);
    if (!MenuExtras) Log("Controller: going into a menu item and scrolling a list are left unbound ([Controller] "
                         "MenuExtras=0), which takes the pad back to the rows of 0.3.7");
    BuildRows();
    SwapSticks = IniInt("Controller", "SwapSticks", 0);
    InvertMoveX = IniInt("Controller", "InvertMoveX", 0);
    InvertMoveY = IniInt("Controller", "InvertMoveY", 0);
    InvertLookX = IniInt("Controller", "InvertLookX", 0);
    InvertLookY = IniInt("Controller", "InvertLookY", 0);
    BuildVtable();
    if (!MoveArray()) { Log("Controller: NOT all references moved, so the pad is not in the array"); return; }
    PadOn = 1;
    RiotTimingOnFrame(KeepSdlReading);
    Log("Controller: a pad is device %d of the engine, with %d inputs, layout \"%s\"", A->Entries, PadInputCount,
        PadMap->Name);
}

/* Following a button through the three steps it has to pass, counted over a couple of seconds rather than logged
 * one line at a time: how the pad itself reads (SdlNow, read every frame by RiotFix), what the engine last took
 * from the pad (PadValues, written only when the engine polls), and what the engine answers the game. A button that
 * does nothing is one of three faults, and the three numbers tell them apart: the pad moving while the polled value
 * stays at rest means the engine has stopped reading the device; both moving while the answer stays at zero means
 * the record is not being read; all three moving means the action is answered and the game is listening to another
 * one. The count of questions matters as much as the values: an action the engine never asks about in play can
 * never work, however well the pad reads. */
static struct { DWORD Action; int Input, Asked, Mode; float Pad, Polled, Answer; } Watch[48];
static int WatchCount;
static DWORD WatchSince;

void RiotPadWatchQuery(int Set, DWORD Action, int Mode, float Answer) {
    (void)Set;
    if (!PadOn) return;
    int Slot = -1;
    for (int Index = 0; Index < WatchCount; Index++) if (Watch[Index].Action == Action) { Slot = Index; break; }
    if (Slot < 0) {
        int Input = -1;
        for (const PadBinding *Row = PadRows; Row->Action; Row++)
            if (Row->Action == Action) { Input = InputFor(Row); break; }
        if (Input < 0 || WatchCount >= (int)(sizeof Watch / sizeof *Watch)) return;
        Slot = WatchCount++;
        Watch[Slot].Action = Action; Watch[Slot].Input = Input;
    }
    const int Input = Watch[Slot].Input;
    Watch[Slot].Asked++;
    Watch[Slot].Mode = Mode;
    if (SdlNow[Input] > Watch[Slot].Pad) Watch[Slot].Pad = SdlNow[Input];
    if (PadValues[Input] > Watch[Slot].Polled) Watch[Slot].Polled = PadValues[Input];
    if (Answer > Watch[Slot].Answer) Watch[Slot].Answer = Answer;
}

static void ReportWatch(void) {
    static DWORD LastPolls;
    const DWORD Now = GetTickCount();
    if (!WatchSince) { WatchSince = Now; return; }
    if (Now - WatchSince < 2000) return;
    const DWORD Polls = PollCount - LastPolls;
    LastPolls = PollCount;
    WatchSince = Now;
    int Interesting = 0;
    for (int Index = 0; Index < WatchCount; Index++)
        if (Watch[Index].Asked && (Watch[Index].Pad > 0.01f || Watch[Index].Answer > 0.01f)) Interesting = 1;
    static DWORD LastRumbles;
    const DWORD Rumbles = RumbleCount - LastRumbles;
    LastRumbles = RumbleCount;
    if (Interesting)
        Log("[pad] the last couple of seconds: the engine read the pad %lu times", Polls);
    /* Rumble answers a separate question - whether the mixer reaches our slot at all, and with what - so it is
     * reported whether or not an action was interesting. */
    if (Rumbles || RumbleLow > 0.0f || RumbleHigh > 0.0f) {
        Log("[pad] the engine asked for rumble %lu time(s) in the last couple of seconds, up to %.3f on the low "
            "motor and %.3f on the high one", Rumbles, (double)RumbleLow, (double)RumbleHigh);
        RumbleLow = RumbleHigh = 0.0f;
    }
    for (int Index = 0; Index < WatchCount; Index++) {
        const int Asked = Watch[Index].Asked;
        if (Interesting && (Asked || Watch[Index].Pad > 0.01f))
            Log("[pad]   action %#05lx (input %#04x, mode %d) asked %d time(s): the pad reads up to %.3f, the engine "
                "took up to %.3f from it and answered up to %.3f", Watch[Index].Action, (unsigned)Watch[Index].Input,
                Watch[Index].Mode, Asked, (double)Watch[Index].Pad, (double)Watch[Index].Polled,
                (double)Watch[Index].Answer);
        Watch[Index].Asked = 0; Watch[Index].Pad = Watch[Index].Polled = Watch[Index].Answer = 0.0f;
    }
}

void RiotPadWatch(void) { Watching = 1; }
int RiotPadIsNative(void) { return PadOn; }
const void *RiotPadArray(void) { return PadOn ? MovedArray : NULL; }
int RiotPadDeviceCount(void) { return PadOn ? A->Entries + 1 : 0; }
