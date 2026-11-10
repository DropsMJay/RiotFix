/*
 * RiotDisplay - game window, video modes and message loops
 *
 * Facts (original GOG executables; the code is the same in both games):
 *
 * Main message loop (PIH 0x4385D0 / TTB 0x43BD10, loads PeekMessageA into esi at PIH 0x4386FE / TTB 0x43BE3E).
 *   Every frame it first takes only keyboard messages (0x100-0x108) and mouse messages (0x200-0x209), translating
 *   and dispatching them, then takes ONE message of any other kind with PeekMessage(0, 0), dispatches it without
 *   TranslateMessage, and runs the frame (PIH 0x404550). Other messages (window, focus, paint, timers...) wait
 *   one frame each, so a queue of them is emptied slowly. The AiO Readme ties this to the Desktop Window Manager
 *   declaring the window unresponsive and replacing it with a "ghost" window, mostly in windowed mode.
 *   Fix: the three PeekMessage calls of this loop go through UnfilteredPeekMessage, which ignores the message
 *   filter. The first inner loop then empties the whole queue each frame (translate + dispatch + WM_QUIT check),
 *   the two following calls find the queue empty, and the frame runs: a conventional message loop.
 *
 * Configuration dialog loop (PIH 0x439B20 / TTB 0x43D0A0, loads PeekMessageA into ebx at PIH 0x439EF3 /
 *   TTB 0x43D445). It calls PeekMessage(0, 0) in a loop until the dialog sets its result (+0x90), never waiting,
 *   so the dialog keeps one processor core busy. Fix: that call goes through WaitDialogMessage, which waits with
 *   GetMessage. The loop still translates and dispatches every message and checks the result after each one.
 *
 * Video mode list (PIH 0x4B3510 / TTB 0x4BBB00). In full screen the game lists the adapter's 32-bit modes of at
 *   least 640x480 whose width/height ratio is >= 1.2 and < 10.0; both bounds are stored in locals by
 *   "mov dword ptr [ebp-x], imm32" (PIH 0x4B3689 / 0x4B3690, TTB 0x4BBC5D / 0x4BBC64). In windowed mode (bit 0 of
 *   the display flags, PIH 0x5C6CE8 / TTB 0x639EC8, read at PIH 0x4B35D9 / TTB 0x4BBBAD) it skips the adapter and
 *   offers five fixed modes (640x480 to 1600x1200). Fix (AllResolutions): the bounds become 0 and FLT_MAX, and that
 *   one read of the display flags reads a RiotFix variable holding 0, so windowed mode lists the adapter's modes too.
 *   Every other reader of the display flags still sees the real value.
 *
 * Texture::CopyFromFrontBuffer (PIH 0x448980, vtable slot at 0x590088 / TTB 0x44EB40, slot at 0x5EAFF4; thiscall,
 *   two arguments, "ret 8"). It uses the texture's Direct3D texture (PIH +0x64 / TTB +0x68) without checking it;
 *   FusionFix (ThirteenAG, MIT) reports crashes when it is missing in windowed mode. Fix: without a Direct3D texture
 *   the copy reports failure (returns 0), which is what the function itself returns when a step fails.
 *
 * DPI awareness: the process is marked DPI aware, so Windows does not scale the game's window on scaled desktops.
 *
 * Widescreen (method as in FusionFix, ThirteenAG, MIT; reimplemented here from the original executables).
 *   Camera (vtable PIH 0x594C18 / TTB 0x5EFBA8): the lens is LensConstant / tan(angle / 2) (lens at +0x27C, a
 *   second lens at +0x270; TTB +0x280 / +0x274). LensConstant is 48.0 in PIH (0x5945E8) and 0.2025 in TTB
 *   (0x5EF3D0); the projection and the view frustum are both built from the lens with a fixed half width of 64 at
 *   lens distance (PIH 0x4B1A30, 0x4B1BC0), so the horizontal view is the same at every aspect ratio and a wide
 *   screen loses picture at the top and bottom ("Vert-"). Seven camera instructions read LensConstant (PIH
 *   0x4B1F30, 0x4B1F74, 0x4B1F90, 0x4B1FBE, 0x4B221C, 0x4B223E, 0x4B23E7); other code reads the same constant
 *   for unrelated purposes (PIH 0x4AD496; more than 100 instructions in TTB), so only those seven are repointed,
 *   to RiotFix's own value: LensConstant / ((width / height) / (4 / 3)) (Hor+: the 4:3 view keeps its height and a
 *   wider screen shows more at the sides).
 *   Field of view of play: each level sets it from its Level Properties (PIH property 0x10, TTB 0x16) through the
 *   renderer (PIH slots 10 and 12, called at Suffering.rfl+0xB2364/+0xB2370, +0xB29F8/+0xB2A04 and +0xB2FAD/+0xB2FB9; TTB slots 12 and
 *   14, SufferingTTB.rfl+0x15D138/+0x15D144, +0x15DD68/+0x15DD74, +0x15E30D/+0x15E319). Slot 12 / 14 stores the
 *   default that the game restores after cutscenes and special views. With [Widescreen] FieldOfView those calls
 *   get the angle that gives that many horizontal degrees: tan(h / 2) = (FrustumHalfWidth / LensConstant) *
 *   (aspect / (4 / 3)) * tan(angle / 2). TTB also sets a second angle from an object's property 0x17 (56 degrees,
 *   SufferingTTB.rfl+0x1DC80D, restored to the default at +0x16F0C): FirstPersonFieldOfView replaces it.
 *   Cameras built before the value changes get their lens rescaled through the game's own lens setter (slot 2,
 *   PIH 0x4B1F00 / TTB 0x4BA690), for the cameras of the renderer (pointer PIH 0x5C5D7C / TTB 0x6392F4; array at
 *   +0x40, count at +0x48, stride PIH 0x2C0 / TTB 0x2C4). The display mode (width, height, depth) is read at
 *   PIH 0x5C6CEC / TTB 0x639ECC.
 *   HUD: the 2D quad builder (PIH 0x44A390 / TTB 0x451240, cdecl, 14 arguments; called at PIH 0x44A6C0 and
 *   0x44AB58 / TTB 0x4506A0 and 0x450B55) is called through HudQuad, which only changes the quad's x and width:
 *   during play (game flags PIH 0x5C6E70 / TTB 0x63A04C, (flags & 0x81) == 0x80) each element keeps its 4:3 size
 *   and its distance from the side it belongs to; elsewhere (menus, loading) the whole 2D picture stays in a
 *   centred 4:3 box. Full-screen quads that are not plain white keep the full width.
 *   Loading screens: a full-screen picture drawn as 3 x 2 tiles of (width / 3) x (height / 2) goes in the 4:3 box as
 *   one picture (anchoring each tile on its own opened gaps between them).
 *   Text: fonts draw a string one quad per letter through the font's slot 5 (vtable PIH 0x592358 -> 0x48A200,
 *   9 stack arguments; TTB 0x5ED258 -> 0x490400, 10), found with LogHud. During play RiotFix measures the string
 *   first (quads recorded and drawn empty), then draws it with one anchor for all its letters.
 *   Cutscene bars: drawn by PIH 0x4B0B30 / TTB 0x4B93E0 (thiscall on the renderer, +0x34 non-zero while a cutscene
 *   shows its bars; called once per frame at PIH 0x48C729 / TTB 0x493F69).
 *
 * Camera log ([Debug] LogCameras=1, diagnostics). The renderer's cameras use vtable PIH 0x594C18 / TTB 0x5EFBA4 (TTB has
 *   one more slot first). Field of view in degrees: PIH slot 1 (0x4B21F0) / TTB slot 2 (0x4BABB0), lens =
 *   LensConstant / tan(degrees / 2), then the view is rebuilt; lens setter PIH slot 2 / TTB slot 3; second lens: PIH
 *   slot 3 (0x4B2230) / TTB slot 4 (0x4BABF0). All are thiscall with one float ("ret 4"). The renderer (vtable PIH
 *   0x594940 / TTB 0x5EF8D0) passes the angle on: PIH slot 10 (0x4B0D90, every camera) and 12 (0x4B0E10, second lens
 *   of every camera); TTB slot 12 (0x4B9830, current camera) and 14 (0x4B95E0). With LogCameras the four methods log
 *   the degrees and their caller (one line per change), and the frame hook logs every lens change of the renderer's
 *   cameras with its horizontal angle, 2 * atan(FrustumHalfWidth / lens) (FrustumHalfWidth: PIH 64 at 0x4B1AB9, TTB
 *   0.3125 at 0x4BA2F9). In PIH 53 degrees is asked every frame through renderer slot 10: it is the vertical angle
 *   (67.2 degrees across at 4:3).
 * HUD log ([Debug] LogHud=1, diagnostics): see LogHudQuad.
 *
 * The game also refuses a second instance (mutex "FELLOWSHIP_INSTANCE_MUTEX" in PIH 0x4385D0,
 *   "SUFFERING2_INSTANCE_MUTEX" in TTB): a second process exits at once. That is the short second process seen in the RiotFix 0.0.4 test log.
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotDisplay.h"
#include "RiotTiming.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* ================================================================ Per-game addresses */
typedef struct {
    DWORD PeekMessageSlot;
    DWORD MainLoopPeekLoad;     /* mov esi, [PeekMessageA] in the main loop */
    DWORD DialogLoopPeekLoad;   /* mov ebx, [PeekMessageA] in the configuration dialog loop */
    DWORD MinimumRatioStore, MaximumRatioStore;   /* "mov dword ptr [ebp-x], imm32" (C7 45 xx imm32) */
    DWORD WindowedFlagRead, DisplayFlags;          /* "mov edx, [DisplayFlags]" (8B 15 imm32) */
    DWORD CopyFromFrontBufferSlot, CopyFromFrontBuffer, TextureOffset;
    DWORD LensConstant, LensUses[7];            /* fld/fmul/fdivr dword ptr [LensConstant] (2-byte opcode + address) */
    DWORD SetLens, LensOffset, CameraStride, RendererPointer, DisplayMode, GameFlags;
    DWORD HudQuadCalls[2], HudQuad;
    DWORD FontVtable, DrawText;   /* slot 5: draws a string, one quad per letter */
    DWORD ShadowTextCall, MainTextCall;   /* .rfl offsets of the return addresses of the shadow and the text */
    DWORD CutsceneBarsCall, CutsceneBars;
    DWORD CameraVtable;                                           /* vtable of the renderer's cameras */
    int FieldOfViewSlot, SetLensSlot, SecondFieldOfViewSlot;      /* PIH 1, 2, 3; TTB 2, 3, 4 */
    DWORD SetFieldOfView, SetSecondFieldOfView;
    float FrustumHalfWidth;                                       /* half width at lens distance (PIH +0x250, TTB +0x254) */
    DWORD RendererVtable;
    int RendererFieldOfViewSlot, RendererSecondSlot;              /* PIH 10 and 12; TTB 12 and 14 */
    DWORD RendererFieldOfView, RendererSecond;
    DWORD LevelFieldOfViewCalls[8];          /* .rfl offsets of the return addresses: level start sets the play angle */
    DWORD FirstPersonFieldOfViewCall;        /* .rfl offset of the return address (TTB only) */
    DWORD TextureVtable, TextureRender;      /* slot 32: Texture::Render (x, y, u, v, w, h, clip, flags, colours, sx, sy) */
    DWORD CursorRenderCall;                  /* .rfl offset of the return address of the cursor's Render call */
    DWORD CursorPosition;                    /* screen manager field: cursor x and y, 0 to 100 (floats) */
    DWORD CursorScreenSize;                  /* .rfl offset of the screen width and height (floats) the cursor uses */
    DWORD RendererAspect;                    /* renderer slot 41 (+0xA4): height / width ratio, thiscall, float */
    DWORD IconAspectCall, IconTangentOperand; /* .rfl offsets: return address of the 3D icon's +0xA4 call; "fmul [0.5]" */
} DisplayAddresses;

static const DisplayAddresses Addresses[GameCount] = {
    [GamePIH] = { 0x58B38C, 0x4386FE, 0x439EF3, 0x4B3689, 0x4B3690, 0x4B35D9, 0x5C6CE8, 0x590088, 0x448980, 0x64,
                  0x5945E8, { 0x4B1F30, 0x4B1F74, 0x4B1F90, 0x4B1FBE, 0x4B221C, 0x4B223E, 0x4B23E7 },
                  0x4B1F00, 0x27C, 0x2C0, 0x5C5D7C, 0x5C6CEC, 0x5C6E70,
                  { 0x44A6C0, 0x44AB58 }, 0x44A390, 0x592358, 0x48A200, 0x6AA64, 0x6AAE2, 0x48C729, 0x4B0B30,
                  0x594C18, 1, 2, 3, 0x4B21F0, 0x4B2230, 64.0f,
                  0x594940, 10, 12, 0x4B0D90, 0x4B0E10,
                  { 0xB2367, 0xB2373, 0xB29FB, 0xB2A07, 0xB2FB0, 0xB2FBC, 0, 0 }, 0,
                  0x58FFE0, 0x44A780, 0x4E1DB, 0xCC, 0x17A7B0, 0x4B0A60, 0x687E7, 0x68834 },
    [GameTTB] = { 0x5E53C8, 0x43BE3E, 0x43D445, 0x4BBC5D, 0x4BBC64, 0x4BBBAD, 0x639EC8, 0x5EAFF4, 0x44EB40, 0x68,
                  0x5EF3D0, { 0x4BA6C0, 0x4BA704, 0x4BA720, 0x4BA74E, 0x4BABDC, 0x4BABFE, 0x4BACC9 },
                  0x4BA690, 0x280, 0x2C4, 0x6392F4, 0x639ECC, 0x63A04C,
                  { 0x4506A0, 0x450B55 }, 0x451240, 0x5ED258, 0x490400, 0x12E570, 0x12E5FF, 0x493F69, 0x4B93E0,
                  0x5EFBA4, 2, 3, 4, 0x4BABB0, 0x4BABF0, 0.3125f,
                  0x5EF8D0, 12, 14, 0x4B9830, 0x4B95E0,
                  { 0x15D13B, 0x15D147, 0x15DD6B, 0x15DD77, 0x15E310, 0x15E31C }, 0x1DC810,
                  0x5EAF48, 0x450780, 0x12AC6C, 0xD0, 0x1B0C78, 0, 0, 0 },
};
static const DisplayAddresses *A;

/* ================================================================ Main loop */
static BOOL WINAPI UnfilteredPeekMessage(LPMSG Message, HWND Window, UINT First, UINT Last, UINT Remove) {
    (void)First; (void)Last;
    return PeekMessageA(Message, Window, 0, 0, Remove);
}
static void *const UnfilteredPeekMessageSlot = (void *)UnfilteredPeekMessage;

/* ================================================================ Configuration dialog loop */
static BOOL WINAPI WaitDialogMessage(LPMSG Message, HWND Window, UINT First, UINT Last, UINT Remove) {
    (void)Remove;
    /* GetMessage returns 0 for WM_QUIT and -1 on error. The original PeekMessage returned TRUE for WM_QUIT too
       (the loop then dispatched it to no window), so only an error counts as "no message". */
    return GetMessageA(Message, Window, First, Last) != -1;
}
static void *const WaitDialogMessageSlot = (void *)WaitDialogMessage;

/* ================================================================ Video mode list */
static const DWORD AlwaysFullScreenFlags = 0;   /* read instead of the display flags by the mode list only */

static int ApplyAllResolutions(void) {
    static const unsigned char Minimum[3] = {0xC7, 0x45, 0xD0}, Maximum[3] = {0xC7, 0x45, 0xEC};
    const float OriginalMinimum = 1.2f, OriginalMaximum = 10.0f, NewMinimum = 0.0f, NewMaximum = 3.402823466e+38f;
    unsigned char ExpectedMinimum[7], ExpectedMaximum[7], ExpectedRead[6] = {0x8B, 0x15};
    memcpy(ExpectedMinimum, Minimum, 3); memcpy(ExpectedMinimum + 3, &OriginalMinimum, 4);
    memcpy(ExpectedMaximum, Maximum, 3); memcpy(ExpectedMaximum + 3, &OriginalMaximum, 4);
    if (CurrentGame == GameTTB) { ExpectedMinimum[2] = 0xDC; ExpectedMaximum[2] = 0xF8; }   /* other frame offsets */
    memcpy(ExpectedRead + 2, &A->DisplayFlags, 4);
    if (!BytesAre(A->MinimumRatioStore, ExpectedMinimum, 7) || !BytesAre(A->MaximumRatioStore, ExpectedMaximum, 7) ||
        !BytesAre(A->WindowedFlagRead, ExpectedRead, 6)) return 0;
    DWORD Flags = (DWORD)&AlwaysFullScreenFlags;
    return WriteCode(A->MinimumRatioStore + 3, &NewMinimum, 4) && WriteCode(A->MaximumRatioStore + 3, &NewMaximum, 4) &&
           WriteCode(A->WindowedFlagRead + 2, &Flags, 4);
}

/* ================================================================ Texture::CopyFromFrontBuffer */
typedef int (ThisCall *CopyFromFrontBufferFunctionType)(void *, void *, void *);
static CopyFromFrontBufferFunctionType OriginalCopyFromFrontBuffer;
static volatile LONG MissingTextureCopies;
static int ThisCall CheckedCopyFromFrontBuffer(void *Self, void *Surface, void *Rectangle) {
    if (!*(void **)((char *)Self + A->TextureOffset)) {
        if (InterlockedIncrement(&MissingTextureCopies) <= 3) Log("CopyFromFrontBuffer without a Direct3D texture: skipped");
        return 0;
    }
    return OriginalCopyFromFrontBuffer(Self, Surface, Rectangle);
}

/* ================================================================ Widescreen */
static float OriginalLensConstant, LensConstant, AppliedLensConstant;
static float PlayFieldOfView, FirstPersonFieldOfView;   /* horizontal degrees from the .ini, 0 = the game's */
static int WatchRendererFieldOfView(void);
static float ReadFieldOfView(const char *Key) {
    float Degrees = IniFloat("Widescreen", Key, 0.0f);
    if (Degrees == 0.0f) return 0.0f;
    if (!(Degrees >= 30.0f && Degrees <= 170.0f)) { Log("Widescreen.%s=%g is invalid (use 30 to 170, or 0)", Key, Degrees); return 0.0f; }
    return Degrees;
}
static float ScreenWidth = 640.0f, ScreenHeight = 480.0f, AspectDifference = 1.0f, HudOffset = 0.0f;
static int CutsceneBarsMode = 1, WidescreenHud = 1;
static volatile int CutsceneBarsSeen, InCutscene;
static int LoggedResolution, RescaleLogs;

typedef void (ThisCall *SetLensFunctionType)(void *, float);
static void ResetFrameAnchors(void);

static void RescaleCameras(float Ratio) {
    DWORD Renderer = *(DWORD *)A->RendererPointer;
    if (!Renderer) return;
    DWORD Base = *(DWORD *)(Renderer + 0x40); int Count = *(int *)(Renderer + 0x48), Rescaled = 0;
    for (int Index = 0; Base && Index < Count && Index < 16; Index++) {
        void *Camera = (void *)(Base + A->CameraStride * Index);
        DWORD *Methods = *(DWORD **)Camera;
        if (!Methods || Methods[A->SetLensSlot] != A->SetLens) continue;
        float Lens = *(float *)((char *)Camera + A->LensOffset);
        if (Lens > 0.0f && Lens < 3.0e38f) { ((SetLensFunctionType)A->SetLens)(Camera, Lens * Ratio); Rescaled++; }
    }
    if (RescaleLogs++ < 4) Log("[widescreen] lens constant %g: %d camera(s) rescaled x%.4f", LensConstant, Rescaled, Ratio);
}

/* Once per frame, before the game's frame: reads the display mode and keeps the lens constant in step with it. */
static void WidescreenFrame(void) {
    InCutscene = CutsceneBarsSeen; CutsceneBarsSeen = 0;
    ResetFrameAnchors();
    const int *Mode = (const int *)A->DisplayMode;
    if (Mode[0] <= 0 || Mode[1] <= 0 || Mode[0] > 32767 || Mode[1] > 32767) return;
    ScreenWidth = (float)Mode[0]; ScreenHeight = (float)Mode[1];
    float Aspect = ScreenWidth / ScreenHeight;
    AspectDifference = Aspect / (4.0f / 3.0f);
    HudOffset = (ScreenWidth - ScreenHeight * (4.0f / 3.0f)) * 0.5f;
    float Wanted = OriginalLensConstant / AspectDifference;
    if (!LoggedResolution || Wanted != LensConstant) {
        if (!LoggedResolution) { LoggedResolution = 1; Log("[widescreen] %dx%d, aspect %.4f: lens constant %g -> %g", Mode[0], Mode[1], Aspect, OriginalLensConstant, Wanted); }
        LensConstant = Wanted;
        if (Wanted != AppliedLensConstant) { RescaleCameras(Wanted / AppliedLensConstant); AppliedLensConstant = Wanted; }
    }
}

typedef void (__cdecl *HudQuadFunctionType)(float *, float, float, float, float, float, float, float, float, float, float,
                                             const DWORD *, DWORD *, int);
static HudQuadFunctionType OriginalHudQuad;
static int LogHud;
static int UiLeafDepth;   /* inside a TTB interface leaf (see HudDrawVertices) */

/* Screens. The .rfl's screen manager (PIH Suffering.rfl+0x4E050, vtable entry +0x151468; TTB SufferingTTB.rfl+0x12AB10,
   entry +0x18FAA8) draws its stack of screens (+0x50 array, +0x58 count): from the top it looks for the first screen
   that covers the others (method +0x18 true and +0x24 != 2), then draws that one and every screen above it whose +0x24
   is above 2 (method +0x20), then tail-calls a method (PIH +0x64, TTB +0x70); it first asks method +0x60 / +0x6C.
   RiotFix draws the stack the same way in C, to know which screen each quad belongs to. A screen that has been drawn
   during play (flags & 0x81 == 0x80) for a second keeps the play layout while the game is paused under a menu, so the
   HUD stays at the sides behind the pause menu; the menu itself goes in the 4:3 box. */
typedef int (ThisCall *ScreenMethodType)(void *);
#define ScreenMethod(Object, Offset) (*(ScreenMethodType *)(*(char *const *)(Object) + (Offset)))
typedef struct { void *Screen; void *Vtable; int Frames; } PlayScreen;
static PlayScreen PlayScreens[16]; static int PlayScreenCount;
static void *CurrentScreen; static int CurrentScreenIsPlay;
static void *ScreenManager;   /* the .rfl's screen manager, kept for the cursor */
static int CursorPass;        /* inside the cursor's Render call: HudQuad leaves the quad as RiotFix placed it */
static DWORD OriginalScreenManager;

static int PlayLayout(void) {
    const DWORD Flags = *(const DWORD *)A->GameFlags;
    return (Flags & 0x81) == 0x80 || ((Flags & 0x80) && CurrentScreenIsPlay);
}
static PlayScreen *FindPlayScreen(void *Screen) {
    for (int Index = 0; Index < PlayScreenCount; Index++)
        if (PlayScreens[Index].Screen == Screen && PlayScreens[Index].Vtable == *(void **)Screen) return &PlayScreens[Index];
    return NULL;
}
static int ThisCall ScreenManagerDraw(void *Self) {
    ScreenManager = Self;
    const int CheckOffset = CurrentGame == GameTTB ? 0x6C : 0x60, TailOffset = CurrentGame == GameTTB ? 0x70 : 0x64;
    int Count = *(int *)((char *)Self + 0x58);
    if (!Count || !ScreenMethod(Self, CheckOffset)(Self)) return 0;
    void **Screens = *(void ***)((char *)Self + 0x50);
    int First = Count - 1;
    while (First > 0) {
        void *Screen = Screens[First];
        if (ScreenMethod(Screen, 0x18)(Screen) && *(int *)((char *)Screen + 0x24) != 2) break;
        First--;
    }
    const int Playing = (*(const DWORD *)A->GameFlags & 0x81) == 0x80;
    for (int Index = First; Index < Count; Index++) {
        void *Screen = Screens[Index];
        if (*(int *)((char *)Screen + 0x24) <= 2) continue;
        PlayScreen *Known = FindPlayScreen(Screen);
        if (Playing) {
            if (!Known && PlayScreenCount < 16) { Known = &PlayScreens[PlayScreenCount++]; *Known = (PlayScreen){ Screen, *(void **)Screen, 0 }; }
            if (Known && Known->Frames < 1000000) Known->Frames++;
        }
        CurrentScreen = Screen; CurrentScreenIsPlay = Known && Known->Frames >= 30;
        ScreenMethod(Screen, 0x20)(Screen);
    }
    CurrentScreen = NULL; CurrentScreenIsPlay = 0;
    return ScreenMethod(Self, TailOffset)(Self);
}
/* The .rfl is loaded after RiotFix: the manager is taken over on the first frame that finds it. */
static int ScreenManagerRegistered;
static void ScreenManagerFrame(void) {
    static int Done;
    if (Done) return;
    HMODULE Rfl = GetModuleHandleA(CurrentGame == GameTTB ? "SufferingTTB.rfl" : "Suffering.rfl");
    if (!Rfl) return;
    Done = 1;
    const DWORD Base = (DWORD)Rfl, Slot = Base + (CurrentGame == GameTTB ? 0x18FAA8 : 0x151468);
    OriginalScreenManager = Base + (CurrentGame == GameTTB ? 0x12AB10 : 0x4E050);
    Log("Widescreen HUD behind menus (screen manager): %s",
        ReplaceVtableSlot(Slot, 0, OriginalScreenManager, (DWORD)ScreenManagerDraw) ? "ok" : "FAILED (unexpected .rfl)");
}
static int ThisCall HudDrawVertices(void *Texture, const float *Vertices, int Count);
static int HookUiScene(void);
static void LogHudQuad(const float *Arguments, float X, float Y, float Width, float Height);
/* Text: the font draws a string one quad per letter (font vtable slot 5). During play each string is drawn twice:
   a measuring pass whose quads are recorded and drawn empty, then the real pass with one anchor for the whole
   string, chosen from the centre of all its letters. The same text drawn again in the same frame (its shadow)
   reuses the anchor. */
enum { AnchorNone, AnchorLeft, AnchorCentre, AnchorRight };
enum { TextIdle, TextMeasuring, TextDrawing };
static int TextPass, TextAnchor;
static float TextLeft, TextRight;
typedef struct { DWORD Hash; float Y; int Anchor; } TextAnchorEntry;
static TextAnchorEntry FrameAnchors[64]; static int FrameAnchorCount;

static int AnchorOf(float Centre) {
    Centre /= ScreenWidth;
    return Centre < 1.0f / 3.0f ? AnchorLeft : Centre > 2.0f / 3.0f ? AnchorRight : AnchorCentre;
}

/* [Widescreen] HudScale: during play every HUD element (and its text) grows by HudScale around a point of the screen
   chosen from where the element is: horizontally the side it is anchored to (left edge, centre, right edge),
   vertically the third of the screen its centre is in (top edge, middle, bottom edge). The crosshair stays in the
   middle and the corner elements grow away from their corners. Menus are not scaled. */
static float HudScale = 1.0f;
static int ForcedAnchor;   /* AnchorNone, or the anchor of a whole inventory cell being drawn (see CellFrameDraw) */
/* Rows of inventory cells seen in the last frame: the copies the row draws while its cells slide (not through the
   cell's draw method) take the anchor of the row they are in. Only for quads that start in the first two thirds. */
typedef struct { float Top, Bottom; int Anchor; } CellRow;
static CellRow CellRows[2][8]; static int CellRowCount[2], CellRowFrame;
static void RememberCellRow(float Top, float Bottom, int Anchor) {
    CellRow *Rows = CellRows[CellRowFrame]; int *Count = &CellRowCount[CellRowFrame];
    for (int Index = 0; Index < *Count; Index++) if (fabsf(Rows[Index].Top - Top) < 1.0f && Rows[Index].Anchor == Anchor) return;
    if (*Count < 8) Rows[(*Count)++] = (CellRow){ Top, Bottom, Anchor };
}
static int CellRowAnchor(float X, float Top, float Bottom) {
    if (X >= ScreenWidth * 2.0f / 3.0f) return AnchorNone;
    const int Last = CellRowFrame ^ 1;
    for (int Frame = 0; Frame < 2; Frame++) {
        const int Which = Frame ? Last : CellRowFrame;
        for (int Index = 0; Index < CellRowCount[Which]; Index++) {
            const CellRow *Row = &CellRows[Which][Index];
            if (Top >= Row->Top - 8.0f && Bottom <= Row->Bottom + 8.0f) return Row->Anchor;
        }
    }
    return AnchorNone;
}
static void ScaleHudPoint(float *X, float *Y, int Anchor, float CentreY) {
    const float PivotX = Anchor == AnchorLeft ? 0.0f : Anchor == AnchorRight ? ScreenWidth : ScreenWidth * 0.5f;
    const float PivotY = CentreY < ScreenHeight / 3.0f ? 0.0f : CentreY > ScreenHeight * 2.0f / 3.0f ? ScreenHeight : ScreenHeight * 0.5f;
    *X = PivotX + (*X - PivotX) * HudScale;
    *Y = PivotY + (*Y - PivotY) * HudScale;
}
static void __cdecl HudQuad(float *Quad, float X, float Y, float Width, float Height, float A6, float A7, float A8, float A9,
                            float A10, float A11, const DWORD *Colors, DWORD *A13, int A14) {
    if (LogHud) LogHudQuad((const float *)&Quad, X, Y, Width, Height);
    const float OriginalX = X, OriginalWidth = Width;
    if (TextPass == TextMeasuring) {
        if (X < TextLeft) TextLeft = X;
        if (X + Width > TextRight) TextRight = X + Width;
        Width = Height = 0.0f;   /* nothing to see in this pass */
    } else if (AspectDifference > 1.0f && !CursorPass) {
        const float TileWidth = ScreenWidth / 3.0f, TileHeight = ScreenHeight / 2.0f;
        if (fabsf(Width - TileWidth) < 0.5f && fabsf(Height - TileHeight) < 0.5f && fabsf(fmodf(X, TileWidth)) < 0.5f) {
            /* One of the six tiles (3 x 2) of a full-screen picture (loading screens): the picture goes in the 4:3 box. */
            X = X / AspectDifference + HudOffset; Width /= AspectDifference;
        } else if (X == 0.0f && Width == ScreenWidth) {
            /* Full-screen quad: plain white ones (pictures) go in the 4:3 box, others (fades) keep the full width. */
            if (Colors && Colors[0] == 0xFFFFFFFF && Colors[1] == 0xFFFFFFFF && Colors[2] == 0xFFFFFFFF && Colors[3] == 0xFFFFFFFF) {
                X = X / AspectDifference + HudOffset; Width /= AspectDifference;
            }
        } else if (PlayLayout()) {
            /* Play: each element keeps its 4:3 size and its distance from its side (left third, right third, centre).
               The letters of a string all follow the anchor of the string. */
            int Anchor = ForcedAnchor;
            if (Anchor == AnchorNone) Anchor = CellRowAnchor(X, Y, Y + Height);
            if (Anchor == AnchorNone) Anchor = TextPass == TextDrawing && TextAnchor != AnchorNone ? TextAnchor : AnchorOf(X + Width * 0.5f);
            float Right = ScreenWidth - X;
            Width /= AspectDifference;
            if (Anchor == AnchorLeft) X = X / AspectDifference;
            else if (Anchor == AnchorRight) X = ScreenWidth - Right / AspectDifference;
            else X = X / AspectDifference + HudOffset;
        } else {
            X = X / AspectDifference + HudOffset; Width /= AspectDifference;
        }
    }
    if (HudScale != 1.0f && TextPass != TextMeasuring && !CursorPass && PlayLayout() && !(OriginalX == 0.0f && OriginalWidth == ScreenWidth) &&
        !(fabsf(OriginalWidth - ScreenWidth / 3.0f) < 0.5f && fabsf(Height - ScreenHeight / 2.0f) < 0.5f)) {
        int Anchor = ForcedAnchor;
        if (Anchor == AnchorNone) Anchor = CellRowAnchor(OriginalX, Y, Y + Height);
        if (Anchor == AnchorNone) Anchor = TextPass == TextDrawing && TextAnchor != AnchorNone ? TextAnchor : AnchorOf(OriginalX + OriginalWidth * 0.5f);
        ScaleHudPoint(&X, &Y, Anchor, Y + Height * 0.5f);
        Width *= HudScale; Height *= HudScale;
    }
    OriginalHudQuad(Quad, X, Y, Width, Height, A6, A7, A8, A9, A10, A11, Colors, A13, A14);
}

static int TextNeedsAnchor(void) {
    return OriginalHudQuad && TextPass == TextIdle && (AspectDifference > 1.0f || HudScale != 1.0f) &&
           (PlayLayout() || (UiLeafDepth && (*(const DWORD *)A->GameFlags & 0x80)));
}
static DWORD TextHash(const wchar_t *Text) {
    DWORD Hash = 2166136261u;
    for (int Index = 0; Text && Text[Index] && Index < 4096; Index++) Hash = (Hash ^ Text[Index]) * 16777619u;
    return Hash;
}
/* Looks for the anchor of the same text near the same height in this frame (a shadow is drawn a few pixels off). */
static int RememberedAnchor(DWORD Hash, float Y) {
    for (int Index = 0; Index < FrameAnchorCount; Index++)
        if (FrameAnchors[Index].Hash == Hash && fabsf(FrameAnchors[Index].Y - Y) <= 8.0f) return FrameAnchors[Index].Anchor;
    return AnchorNone;
}
static void RememberAnchor(DWORD Hash, float Y, int Anchor) {
    if (FrameAnchorCount < 64) FrameAnchors[FrameAnchorCount++] = (TextAnchorEntry){ Hash, Y, Anchor };
}
static void ResetFrameAnchors(void) { FrameAnchorCount = 0; CellRowFrame ^= 1; CellRowCount[CellRowFrame] = 0; }
static void BeginMeasure(void) { TextPass = TextMeasuring; TextLeft = 3.0e38f; TextRight = -3.0e38f; }
static void EndMeasure(void) {
    TextAnchor = TextLeft <= TextRight ? AnchorOf((TextLeft + TextRight) * 0.5f) : AnchorNone;
    TextPass = TextDrawing;
}

/* One call of the font's slot 5. Stack arguments (both games, from the calls at PIH Suffering.rfl+0x6AA5E and TTB
   SufferingTTB.rfl+0x12E56A): 0 font data, 1 text, 2 x, 3 y, 4 out pointer, 5 scale x, 6 scale y, 7 colours (four
   ARGB corners), 8 flags, 9 matrix (TTB only: the interface's text leaf 0x43F7A0 passes 0 as flags and its scaled
   4 x 4 matrix as the 10th argument). PIH has 9, TTB 10 ("ret 24h" / "ret 28h"). */
typedef struct { void *Self; DWORD Arguments[10]; } TextCall;
typedef DWORD (ThisCall *DrawText9FunctionType)(void *, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
typedef DWORD (ThisCall *DrawText10FunctionType)(void *, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
static DrawText9FunctionType OriginalDrawText9;
static DrawText10FunctionType OriginalDrawText10;
static DWORD CallDrawText(const TextCall *Call) {
    const DWORD *V = Call->Arguments;
    if (CurrentGame == GameTTB) return OriginalDrawText10(Call->Self, V[0], V[1], V[2], V[3], V[4], V[5], V[6], V[7], V[8], V[9]);
    return OriginalDrawText9(Call->Self, V[0], V[1], V[2], V[3], V[4], V[5], V[6], V[7], V[8]);
}
static float TextFloat(const TextCall *Call, int Index) { float Value; memcpy(&Value, &Call->Arguments[Index], 4); return Value; }
static void SetTextFloat(TextCall *Call, int Index, float Value) { memcpy(&Call->Arguments[Index], &Value, 4); }

/* Text outline ([Text] Outline): the game draws a shadow, the same string in black 2 pixels off, through one call
   (PIH Suffering.rfl+0x6AA61, TTB SufferingTTB.rfl+0x12E56D, colours alpha << 24) followed by the text itself (PIH
   +0x6AADF, TTB +0x12E5FC). The shadow call is held back; when the text comes, the string is drawn eight times around
   it in the outline colour (with the shadow's alpha), then the shadow if kept, then the text. */
static int TextOutline, TextOutlineShadow, TextOutlineAll;
static float TextOutlineSize = 2.0f;
static DWORD TextOutlineColour;
static TextCall PendingShadow; static DWORD PendingShadowColours[4]; static int HasPendingShadow;
/* [Text] OutlineSkipFonts: resource ids of fonts drawn without an outline. A font is a resource like the textures; its
   id is at +4 (the resource release, PIH 0x466660, passes [this + 4] to the manager; the TTB text leaf 0x43F7A0 gets
   the font from the manager by id). LogHud logs the id of every font with its first string. Font 00050003 (Interface
   package, 5_0003) is the menus' hand-drawn font in both games ("Quit", "New Game"); 00050002 is the HUD's. */
static DWORD SkipFonts[16]; static int SkipFontCount;
static int FontSkipped(const TextCall *Call) {
    if (!SkipFontCount || !Call->Arguments[0]) return 0;
    DWORD Id = ((const DWORD *)Call->Arguments[0])[1];
    for (int Index = 0; Index < SkipFontCount; Index++) if (SkipFonts[Index] == Id) return 1;
    return 0;
}
static void LogFont(const TextCall *Call, DWORD Caller);

static int BlackText(const TextCall *Call) {
    const DWORD *Colours = (const DWORD *)Call->Arguments[7];
    if (!Colours) return 1;
    for (int Index = 0; Index < 4; Index++) if (Colours[Index] & 0xFFFFFF) return 0;
    return 1;
}
static DWORD DrawPrepared(TextCall *Call, const TextCall *Shadow, int Outline) {
    if (Outline) {
        DWORD Colours[4];
        const DWORD *Source = (const DWORD *)(Shadow ? Shadow : Call)->Arguments[7];
        for (int Index = 0; Index < 4; Index++) Colours[Index] = (Source[Index] & 0xFF000000) | TextOutlineColour;
        float Size = TextOutlineSize * ScreenHeight / 1080.0f, X = TextFloat(Call, 2), Y = TextFloat(Call, 3);
        if (CurrentGame == GameTTB && Call->Arguments[9]) {
            /* Text placed with a matrix (TTB interface): x and y are in the matrix's units, so the size is divided by its scale. */
            const float *Matrix = (const float *)Call->Arguments[9];
            float Scale = sqrtf(Matrix[0] * Matrix[0] + Matrix[1] * Matrix[1]);
            if (Scale > 0.0001f) Size /= Scale;
        }
        static const signed char Directions[8][2] = { {-1,-1}, {0,-1}, {1,-1}, {-1,0}, {1,0}, {-1,1}, {0,1}, {1,1} };
        TextCall Copy = *Call;
        Copy.Arguments[7] = (DWORD)Colours;
        for (int Index = 0; Index < 8; Index++) {
            SetTextFloat(&Copy, 2, X + Directions[Index][0] * Size); SetTextFloat(&Copy, 3, Y + Directions[Index][1] * Size);
            CallDrawText(&Copy);
        }
        if (Shadow && TextOutlineShadow) CallDrawText(Shadow);
    } else if (Shadow) CallDrawText(Shadow);
    return CallDrawText(Call);
}
/* Draws one string (and its outline), with one anchor for all its letters during play. */
static DWORD DrawString(TextCall *Call, const TextCall *Shadow, int Outline) {
    if (!TextNeedsAnchor()) return DrawPrepared(Call, Shadow, Outline);
    const wchar_t *Text = (const wchar_t *)Call->Arguments[1];
    float Y = TextFloat(Call, 3);
    DWORD Hash = TextHash(Text);
    int Anchor = RememberedAnchor(Hash, Y);
    if (Anchor == AnchorNone) {
        BeginMeasure(); CallDrawText(Call); EndMeasure();
        RememberAnchor(Hash, Y, TextAnchor);
    } else { TextAnchor = Anchor; TextPass = TextDrawing; }
    DWORD Result = DrawPrepared(Call, Shadow, Outline);
    TextPass = TextIdle;
    return Result;
}
static DWORD RflOffset(DWORD Address) {
    static DWORD Base;
    if (!Base) Base = (DWORD)GetModuleHandleA(CurrentGame == GameTTB ? "SufferingTTB.rfl" : "Suffering.rfl");
    return Base ? Address - Base : 0;
}
/* ================================================================ Cursor
   The screen manager's last method (PIH Suffering.rfl+0x4E0C0, TTB SufferingTTB.rfl+0x12AB80) draws the cursor
   texture with Texture::Render (texture vtable slot 32, PIH 0x44A780 / TTB 0x450780; TTB has a 12th argument) at
   x = (int)(ScreenWidth * CursorX * 0.01) - HotspotX (y likewise), scale 1. The cursor position (manager +0xCC / +0xD0
   in PIH, +0xD0 / +0xD4 in TTB) goes from 0 to 100. Render filters the texture linearly whenever the scale is not 1
   (with scale 1 it samples points). [Cursor] Scale enlarges it around its hotspot; in the 4:3 menu box the point
   moves with the box and the cursor keeps its proportions (HudQuad leaves it alone). */
typedef int (ThisCall *Render11FunctionType)(void *, int, int, int, int, int, int, void *, DWORD, void *, float, float);
typedef int (ThisCall *Render12FunctionType)(void *, int, int, int, int, int, int, void *, DWORD, void *, float, float, int);
static Render11FunctionType OriginalRender11;
static Render12FunctionType OriginalRender12;
static float CursorScale = 1.0f; static int CursorAutomatic;

static void PlaceCursor(int *X, int *Y, float *Scale) {
    const DWORD Rfl = (DWORD)GetModuleHandleA(CurrentGame == GameTTB ? "SufferingTTB.rfl" : "Suffering.rfl");
    if (!Rfl || !ScreenManager) return;
    const float *Size = (const float *)(Rfl + A->CursorScreenSize);
    const float *Position = (const float *)((char *)ScreenManager + A->CursorPosition);
    /* The game's own point (same operations, truncated like its _ftol) gives the hotspot back. */
    const int PointX = (int)((long double)Size[0] * Position[0] * 0.01f), PointY = (int)((long double)Size[1] * Position[1] * 0.01f);
    const int HotX = PointX - *X, HotY = PointY - *Y;
    float NewScale = CursorAutomatic ? Size[1] / 480.0f : CursorScale;
    if (!(NewScale > 0.0f)) NewScale = 1.0f;
    float X2 = (float)PointX;
    if (OriginalHudQuad && AspectDifference > 1.0f && !PlayLayout()) X2 = X2 / AspectDifference + HudOffset;
    *X = (int)lroundf(X2 - HotX * NewScale);
    *Y = (int)lroundf((float)PointY - HotY * NewScale);
    *Scale = NewScale;
}
static int ThisCall Render11(void *Self, int X, int Y, int U, int V, int W, int H, void *Clip, DWORD Flags, void *Colours, float Sx, float Sy) {
    if (RflOffset((DWORD)__builtin_return_address(0)) != A->CursorRenderCall) return OriginalRender11(Self, X, Y, U, V, W, H, Clip, Flags, Colours, Sx, Sy);
    PlaceCursor(&X, &Y, &Sx);
    CursorPass = 1;
    int Result = OriginalRender11(Self, X, Y, U, V, W, H, Clip, Flags, Colours, Sx, Sy);
    CursorPass = 0;
    return Result;
}
static int ThisCall Render12(void *Self, int X, int Y, int U, int V, int W, int H, void *Clip, DWORD Flags, void *Colours, float Sx, float Sy,
                             int Last) {
    if (RflOffset((DWORD)__builtin_return_address(0)) != A->CursorRenderCall)
        return OriginalRender12(Self, X, Y, U, V, W, H, Clip, Flags, Colours, Sx, Sy, Last);
    PlaceCursor(&X, &Y, &Sx);
    CursorPass = 1;
    int Result = OriginalRender12(Self, X, Y, U, V, W, H, Clip, Flags, Colours, Sx, Sy, Last);
    CursorPass = 0;
    return Result;
}
static int HookCursor(void) {
    if (CurrentGame == GameTTB) { OriginalRender12 = (Render12FunctionType)A->TextureRender; return ReplaceVtableSlot(A->TextureVtable, 32, A->TextureRender, (DWORD)Render12); }
    OriginalRender11 = (Render11FunctionType)A->TextureRender;
    return ReplaceVtableSlot(A->TextureVtable, 32, A->TextureRender, (DWORD)Render11);
}

/* ================================================================ 3D inventory icons (PIH)
   The inventory cell (Suffering.rfl+0x68630) draws the item's model in the 3D scene: it sets the renderer's angle
   to the template's value f (renderer slot 10), takes Fx = (width / 2) / tan(f * 0.5) (the 0.5 at +0x68834) and
   Fy = (height / 2) / tan(A * f * 0.5) with A from renderer slot 41 (+0xA4, height / width), places the model so
   the centre of the cell (+0x38 / +0x3C position, +0x40 / +0x44 size) falls on it, clears the depth inside the
   cell's rectangle (slot 9, +0x24) and draws. The engine's angle is the vertical one and its horizontal tangent is
   FrustumHalfWidth / lens (the screen keeps square pixels with the widescreen lens), so the icon lands off the cell,
   and the part outside the cell is hidden by the scene's depth. With [Widescreen] FixInventoryIcons the 0.5 of Fx
   becomes Kx = atan(horizontal tangent) / f, and the A returned to that call becomes 2 * atan(vertical tangent) / f,
   so both match the camera the model is drawn with. */
typedef float (ThisCall *RendererAspectFunctionType)(void *);
static RendererAspectFunctionType OriginalRendererAspect;
static float IconTangentFactorX = 0.5f;
static int FixIcons;
static volatile LONG IconLogLines;
static float ThisCall IconAspect(void *Renderer) {
    const float Aspect = OriginalRendererAspect(Renderer);
    if (!FixIcons || RflOffset((DWORD)__builtin_return_address(0)) != A->IconAspectCall) return Aspect;
    /* The angle the cell has just set: lens = LensConstant / tan(f / 2) for the current camera. */
    const char *R = (const char *)Renderer;
    const DWORD Base = *(const DWORD *)(R + 0x40); const int Index = *(const int *)(R + 0x4C);
    if (!Base || Index < 0) return Aspect;
    const float Lens = *(const float *)(Base + A->CameraStride * (DWORD)Index + A->LensOffset);
    const float Constant = AppliedLensConstant > 0.0f ? AppliedLensConstant : *(const float *)A->LensConstant;
    if (!(Lens > 0.0f) || !(Constant > 0.0f) || !(ScreenWidth > 0.0f)) return Aspect;
    const double Angle = 2.0 * atan(Constant / Lens);
    /* The renderer's own ratio (slot 41) is the one between its vertical and horizontal tangents; the game used it
       as a ratio of angles. */
    const double Horizontal = A->FrustumHalfWidth / Lens, Vertical = Horizontal * Aspect;
    IconTangentFactorX = (float)(atan(Horizontal) / Angle);
    const float NewAspect = (float)(2.0 * atan(Vertical) / Angle);
    if (InterlockedIncrement(&IconLogLines) <= 10)
        Log("[icon] angle %.2f deg, lens %g, tangents %.4f x %.4f, game aspect %.4f -> %.4f, x factor 0.5 -> %.4f",
            Angle * 57.29577951, Lens, Horizontal, Vertical, Aspect, NewAspect, IconTangentFactorX);
    return NewAspect;
}
/* The cell's rectangle (+0x38 x, +0x3C y, +0x40 width, +0x44 height, floats, in the game's layout) is the one its
   frame is drawn from, and HudQuad moves that frame (anchor, HudScale). The cell's draw method (vtable entry
   Suffering.rfl+0x152728 -> +0x68630, thiscall, no arguments) is called with the rectangle moved the same way, so the
   icon and its depth clear follow the frame; the rectangle is put back afterwards. */
typedef int (ThisCall *IconCellDrawFunctionType)(void *);
static IconCellDrawFunctionType OriginalIconCellDraw;
static int ThisCall IconCellDraw(void *Cell) {
    float *Rect = (float *)((char *)Cell + 0x38);
    const float Saved[4] = { Rect[0], Rect[1], Rect[2], Rect[3] };
    if (OriginalHudQuad && PlayLayout() && (AspectDifference > 1.0f || HudScale != 1.0f)) {
        float X = Rect[0], Y = Rect[1], Width = Rect[2], Height = Rect[3];
        const int Anchor = ForcedAnchor != AnchorNone ? ForcedAnchor : AnchorOf(X);   /* as its cell (see CellFrameDraw) */
        if (AspectDifference > 1.0f) {
            const float Right = ScreenWidth - X;
            Width /= AspectDifference;
            if (Anchor == AnchorLeft) X = X / AspectDifference;
            else if (Anchor == AnchorRight) X = ScreenWidth - Right / AspectDifference;
            else X = X / AspectDifference + HudOffset;
        }
        if (HudScale != 1.0f) { ScaleHudPoint(&X, &Y, Anchor, Y + Height * 0.5f); Width *= HudScale; Height *= HudScale; }
        Rect[0] = X; Rect[1] = Y; Rect[2] = Width; Rect[3] = Height;
    }
    int Result = OriginalIconCellDraw(Cell);
    Rect[0] = Saved[0]; Rect[1] = Saved[1]; Rect[2] = Saved[2]; Rect[3] = Saved[3];
    return Result;
}
/* A whole inventory cell (frame, item, ammo count, icon) takes one anchor, so cells that slide across a third of the
   screen during the switch animation are not split. The anchor comes from the cell's left edge: the weapon row
   starts at the left and cuts its cells where the row ends (at about a third of the screen), so every cell still
   shown during the slide starts in the left third. The cell class's draw method is the vtable entry
   Suffering.rfl+0x1526E8 -> +0x66F40 (thiscall, no arguments); it ends in the generic widget draw, which draws the
   frame and the children. (Its +0x68 is the cell itself, not its row: seen with LogHud.) */
static IconCellDrawFunctionType OriginalCellFrameDraw;
static volatile LONG CellLogLines;
static int ThisCall CellFrameDraw(void *Cell) {
    const int Previous = ForcedAnchor;
    const float *Rect = (const float *)((char *)Cell + 0x38);
    if (Rect[2] > 0.0f && Rect[2] < 100000.0f) { ForcedAnchor = AnchorOf(Rect[0]); RememberCellRow(Rect[1], Rect[1] + Rect[3], ForcedAnchor); }
    if (LogHud && InterlockedIncrement(&CellLogLines) <= 12)
        Log("[cell] %p %.1f,%.1f %.1fx%.1f -> anchor %d", Cell, Rect[0], Rect[1], Rect[2], Rect[3], ForcedAnchor);
    int Result = OriginalCellFrameDraw(Cell);
    ForcedAnchor = Previous;
    return Result;
}
static void IconFrame(void) {
    static int Done;
    if (Done) return;
    HMODULE Rfl = GetModuleHandleA("Suffering.rfl");
    if (!Rfl) return;
    Done = 1;
    const DWORD At = (DWORD)Rfl + A->IconTangentOperand, Half = (DWORD)Rfl + 0x14B6DC;
    unsigned char Expected[6] = { 0xD8, 0x0D }; memcpy(Expected + 2, &Half, 4);
    const DWORD Factor = (DWORD)&IconTangentFactorX;
    int Patched = BytesAre(At, Expected, 6) && WriteCode(At + 2, &Factor, 4);
    if (!Patched) FixIcons = 0;
    Log("Inventory 3D icons placed with the camera's angles: %s", Patched ? "ok" : "FAILED (unexpected .rfl)");
    if (OriginalHudQuad) {
        OriginalIconCellDraw = (IconCellDrawFunctionType)((DWORD)Rfl + 0x68630);
        Log("Inventory 3D icons follow their HUD frame: %s",
            ReplaceVtableSlot((DWORD)Rfl + 0x152728, 0, (DWORD)Rfl + 0x68630, (DWORD)IconCellDraw) ? "ok" : "FAILED (unexpected .rfl)");
        OriginalCellFrameDraw = (IconCellDrawFunctionType)((DWORD)Rfl + 0x66F40);
        Log("Inventory cells anchored as one: %s",
            ReplaceVtableSlot((DWORD)Rfl + 0x1526E8, 0, (DWORD)Rfl + 0x66F40, (DWORD)CellFrameDraw) ? "ok" : "FAILED (unexpected .rfl)");
    }
}

static DWORD HandleText(TextCall *Call, DWORD Caller) {
    if (LogHud && TextPass == TextIdle) LogFont(Call, Caller);
    if (TextOutline && TextPass == TextIdle && FontSkipped(Call)) {
        /* A font without an outline: a held shadow goes first, then this string as the game draws it. */
        if (HasPendingShadow) {
            HasPendingShadow = 0;
            if (RflOffset(Caller) == A->MainTextCall && Call->Arguments[1] == PendingShadow.Arguments[1]) return DrawString(Call, &PendingShadow, 0);
            DrawString(&PendingShadow, NULL, 0);
        }
        return DrawString(Call, NULL, 0);
    }
    if (TextOutline && TextPass == TextIdle) {
        DWORD Offset = RflOffset(Caller);
        if (Offset == A->ShadowTextCall && Call->Arguments[7]) {
            if (HasPendingShadow) DrawString(&PendingShadow, NULL, 0);
            PendingShadow = *Call;
            memcpy(PendingShadowColours, (const void *)Call->Arguments[7], sizeof PendingShadowColours);
            PendingShadow.Arguments[7] = (DWORD)PendingShadowColours;
            HasPendingShadow = 1;
            return 0;   /* the game does not use the result of this call */
        }
        if (HasPendingShadow) {
            HasPendingShadow = 0;
            if (Offset == A->MainTextCall && Call->Arguments[1] == PendingShadow.Arguments[1]) return DrawString(Call, &PendingShadow, 1);
            DrawString(&PendingShadow, NULL, 0);
        }
    }
    /* Other text: outlined too with OutlineAll, except black text (shadows drawn by other code, dark labels). */
    return DrawString(Call, NULL, TextOutline && TextOutlineAll && !BlackText(Call));
}
static DWORD ThisCall DrawText9(void *Self, DWORD A0, DWORD A1, DWORD A2, DWORD A3, DWORD A4, DWORD A5, DWORD A6, DWORD A7, DWORD A8) {
    TextCall Call = { Self, { A0, A1, A2, A3, A4, A5, A6, A7, A8, 0 } };
    return HandleText(&Call, (DWORD)__builtin_return_address(0));
}
static DWORD ThisCall DrawText10(void *Self, DWORD A0, DWORD A1, DWORD A2, DWORD A3, DWORD A4, DWORD A5, DWORD A6, DWORD A7, DWORD A8,
                                 DWORD A9) {
    TextCall Call = { Self, { A0, A1, A2, A3, A4, A5, A6, A7, A8, A9 } };
    return HandleText(&Call, (DWORD)__builtin_return_address(0));
}
static int TextHooked;
static int HookText(void) {
    if (TextHooked) return 1;
    if (CurrentGame == GameTTB) { OriginalDrawText10 = (DrawText10FunctionType)A->DrawText; TextHooked = ReplaceVtableSlot(A->FontVtable, 5, A->DrawText, (DWORD)DrawText10); }
    else { OriginalDrawText9 = (DrawText9FunctionType)A->DrawText; TextHooked = ReplaceVtableSlot(A->FontVtable, 5, A->DrawText, (DWORD)DrawText9); }
    return TextHooked;
}

typedef int (ThisCall *CutsceneBarsFunctionType)(void *);
static CutsceneBarsFunctionType OriginalCutsceneBars;
static int ThisCall CutsceneBars(void *Renderer) {
    if (*(int *)((char *)Renderer + 0x34)) CutsceneBarsSeen = 1;
    return CutsceneBarsMode ? OriginalCutsceneBars(Renderer) : 0;
}

static void ApplyWidescreen(void) {
    OriginalLensConstant = *(const float *)A->LensConstant;
    LensConstant = AppliedLensConstant = OriginalLensConstant;
    int Repointed = 0;
    for (int Index = 0; Index < 7; Index++) {
        DWORD At = A->LensUses[Index];
        if (*(DWORD *)(At + 2) != A->LensConstant) continue;
        DWORD Variable = (DWORD)&LensConstant;
        Repointed += WriteCode(At + 2, &Variable, 4);
    }
    Log("Widescreen field of view (Hor+): %d/7 camera instructions", Repointed);
    if (Repointed != 7) return;
    RiotTimingOnFrame(WidescreenFrame);
    PlayFieldOfView = ReadFieldOfView("FieldOfView");
    FirstPersonFieldOfView = ReadFieldOfView("FirstPersonFieldOfView");
    if (FirstPersonFieldOfView && !A->FirstPersonFieldOfViewCall) {
        Log("Widescreen.FirstPersonFieldOfView: not available in this game yet"); FirstPersonFieldOfView = 0.0f;
    }
    if (PlayFieldOfView || FirstPersonFieldOfView) {
        WatchRendererFieldOfView();
        Log("Widescreen horizontal field of view: play %g, first person %g (0 = the game's)", PlayFieldOfView, FirstPersonFieldOfView);
    }

    WidescreenHud = IniInt("Widescreen", "Hud", 1);
    HudScale = IniFloat("Widescreen", "HudScale", 1.0f);
    if (!(HudScale >= 0.5f && HudScale <= 3.0f)) { Log("Widescreen.HudScale=%g is invalid (use 0.5 to 3)", HudScale); HudScale = 1.0f; }
    if (HudScale != 1.0f && WidescreenHud) Log("Widescreen HUD scale x%g", HudScale);
    if (WidescreenHud) {
        OriginalHudQuad = (HudQuadFunctionType)A->HudQuad;
        int Count = RedirectCall(A->HudQuadCalls[0], A->HudQuad, (DWORD)HudQuad) + RedirectCall(A->HudQuadCalls[1], A->HudQuad, (DWORD)HudQuad);
        Log("Widescreen HUD and menus in 4:3: %d/2 calls", Count);
        Log("Widescreen text anchored per string: %s", HookText() ? "ok" : "FAILED (unexpected vtable)");
        if (!ScreenManagerRegistered++) RiotTimingOnFrame(ScreenManagerFrame);
        if (CurrentGame == GameTTB) Log("Widescreen interface scene anchored per element: %s", HookUiScene() == 3 ? "ok" : "FAILED (unexpected vtable)");
    }
    if (A->RendererAspect && IniInt("Widescreen", "FixInventoryIcons", 1)) {
        OriginalRendererAspect = (RendererAspectFunctionType)A->RendererAspect;
        FixIcons = ReplaceVtableSlot(A->RendererVtable, 41, A->RendererAspect, (DWORD)IconAspect);
        if (FixIcons) RiotTimingOnFrame(IconFrame); else Log("Inventory 3D icons: FAILED (unexpected vtable)");
    }
    CutsceneBarsMode = IniInt("Widescreen", "CutsceneBars", 1);
    OriginalCutsceneBars = (CutsceneBarsFunctionType)A->CutsceneBars;
    Log("Cutscene bars %s: %s", CutsceneBarsMode ? "on" : "off",
        RedirectCall(A->CutsceneBarsCall, A->CutsceneBars, (DWORD)CutsceneBars) ? "ok" : "FAILED (unexpected bytes)");
}

/* ================================================================ Camera log (diagnostics) */
typedef void (ThisCall *SetFieldOfViewFunctionType)(void *, float);
static SetFieldOfViewFunctionType OriginalSetFieldOfView, OriginalSetSecondFieldOfView, OriginalRendererFieldOfView, OriginalRendererSecond;
static volatile LONG CameraLogLines;
#define MaximumCameraLogLines 200

/* One line per change: the same value asked again by the same caller is not logged again. */
typedef struct { void *Object; float Degrees; DWORD Caller; } LastFieldOfView;
static LastFieldOfView LastAsked[4];

static void LogFieldOfView(int Kind, const char *Which, void *Object, float Degrees, DWORD Caller) {
    LastFieldOfView *Last = &LastAsked[Kind];
    if (Last->Object == Object && Last->Degrees == Degrees && Last->Caller == Caller) return;
    Last->Object = Object; Last->Degrees = Degrees; Last->Caller = Caller;
    if (InterlockedIncrement(&CameraLogLines) > MaximumCameraLogLines) return;
    char Name[MAX_PATH + 32]; AddressName(Caller, Name, sizeof Name);
    Log("[camera] %p %s %.3f deg (asked by %s)", Object, Which, Degrees, Name);
}
static void ThisCall LoggedSetFieldOfView(void *Camera, float Degrees) {
    LogFieldOfView(0, "camera field of view", Camera, Degrees, (DWORD)__builtin_return_address(0));
    OriginalSetFieldOfView(Camera, Degrees);
}
static void ThisCall LoggedSetSecondFieldOfView(void *Camera, float Degrees) {
    LogFieldOfView(1, "camera second field of view", Camera, Degrees, (DWORD)__builtin_return_address(0));
    OriginalSetSecondFieldOfView(Camera, Degrees);
}
/* The renderer's two angle setters: log (LogCameras) and the angles chosen in the .ini (FieldOfView). */
static int LogCamerasOn, RendererWatched;
static DWORD RflBase;
static float AngleFor(DWORD Caller, float Degrees) {
    if (!PlayFieldOfView && !FirstPersonFieldOfView) return Degrees;
    if (!RflBase) RflBase = (DWORD)GetModuleHandleA(CurrentGame == GameTTB ? "SufferingTTB.rfl" : "Suffering.rfl");
    if (!RflBase) return Degrees;
    DWORD Offset = Caller - RflBase;
    float Horizontal = 0.0f;
    if (Offset == A->FirstPersonFieldOfViewCall && A->FirstPersonFieldOfViewCall) Horizontal = FirstPersonFieldOfView;
    else for (int Index = 0; Index < 8; Index++) if (A->LevelFieldOfViewCalls[Index] && Offset == A->LevelFieldOfViewCalls[Index]) Horizontal = PlayFieldOfView;
    if (!Horizontal) return Degrees;
    /* tan(h / 2) = (FrustumHalfWidth / OriginalLensConstant) * AspectDifference * tan(angle / 2) */
    double Ratio = A->FrustumHalfWidth / OriginalLensConstant * AspectDifference;
    return (float)(2.0 * atan(tan(Horizontal * 0.5 / 57.29577951) / Ratio) * 57.29577951);
}
static void ThisCall WatchedRendererFieldOfView(void *Renderer, float Degrees) {
    DWORD Caller = (DWORD)__builtin_return_address(0);
    float Angle = AngleFor(Caller, Degrees);
    if (LogCamerasOn) {
        char Which[96];
        if (Angle != Degrees) snprintf(Which, sizeof Which, "renderer field of view (RiotFix, game asked %.3f)", Degrees);
        else snprintf(Which, sizeof Which, "renderer field of view");
        LogFieldOfView(2, Which, Renderer, Angle, Caller);
    }
    OriginalRendererFieldOfView(Renderer, Angle);
}
static void ThisCall WatchedRendererSecond(void *Renderer, float Degrees) {
    DWORD Caller = (DWORD)__builtin_return_address(0);
    float Angle = AngleFor(Caller, Degrees);
    if (LogCamerasOn) {
        char Which[96];
        if (Angle != Degrees) snprintf(Which, sizeof Which, "renderer second field of view (RiotFix, game asked %.3f)", Degrees);
        else snprintf(Which, sizeof Which, "renderer second field of view");
        LogFieldOfView(3, Which, Renderer, Angle, Caller);
    }
    OriginalRendererSecond(Renderer, Angle);
}
static int WatchRendererFieldOfView(void) {
    if (RendererWatched) return 2;
    OriginalRendererFieldOfView = (SetFieldOfViewFunctionType)A->RendererFieldOfView;
    OriginalRendererSecond = (SetFieldOfViewFunctionType)A->RendererSecond;
    RendererWatched = ReplaceVtableSlot(A->RendererVtable, A->RendererFieldOfViewSlot, A->RendererFieldOfView, (DWORD)WatchedRendererFieldOfView) +
                      ReplaceVtableSlot(A->RendererVtable, A->RendererSecondSlot, A->RendererSecond, (DWORD)WatchedRendererSecond);
    if (RendererWatched != 2) Log("Renderer field of view: %d/2 methods watched (unexpected vtable)", RendererWatched);
    return RendererWatched;
}

static float LastLens[16];
static void CameraLogFrame(void) {
    DWORD Renderer = *(DWORD *)A->RendererPointer;
    if (!Renderer) return;
    DWORD Base = *(DWORD *)(Renderer + 0x40); int Count = *(int *)(Renderer + 0x48);
    for (int Index = 0; Base && Index < Count && Index < 16; Index++) {
        char *Camera = (char *)(Base + A->CameraStride * Index);
        if (*(DWORD *)Camera != A->CameraVtable) continue;
        float Lens = *(float *)(Camera + A->LensOffset);
        if (Lens == LastLens[Index] || !(Lens > 0.0f)) continue;
        LastLens[Index] = Lens;
        if (InterlockedIncrement(&CameraLogLines) > MaximumCameraLogLines) return;
        const int *Mode = (const int *)A->DisplayMode;
        Log("[camera] renderer camera %d (%p): lens %g, horizontal %.2f deg, original-constant angle %.2f deg (screen %dx%d)",
            Index, Camera, Lens, 2.0 * atan(A->FrustumHalfWidth / Lens) * 57.29577951,
            2.0 * atan(OriginalLensConstant / Lens) * 57.29577951, Mode[0], Mode[1]);
    }
}

static void ApplyCameraLog(void) {
    if (!OriginalLensConstant) OriginalLensConstant = *(const float *)A->LensConstant;
    OriginalSetFieldOfView = (SetFieldOfViewFunctionType)A->SetFieldOfView;
    OriginalSetSecondFieldOfView = (SetFieldOfViewFunctionType)A->SetSecondFieldOfView;
    LogCamerasOn = 1;
    int Hooked = ReplaceVtableSlot(A->CameraVtable, A->FieldOfViewSlot, A->SetFieldOfView, (DWORD)LoggedSetFieldOfView) +
                 ReplaceVtableSlot(A->CameraVtable, A->SecondFieldOfViewSlot, A->SetSecondFieldOfView, (DWORD)LoggedSetSecondFieldOfView) +
                 WatchRendererFieldOfView();
    RiotTimingOnFrame(CameraLogFrame);
    Log("Camera log (first %d changes): %d/4 camera and renderer methods watched", MaximumCameraLogLines, Hooked);
}

/* ================================================================ HUD log (diagnostics) */
/* [Debug] LogHud=1: for every new chain of callers that draws a 2D quad, logs the quad and up to four return
   addresses found on the stack above it (values that point just after a call instruction in the exe or the .rfl).
   Used to find the code that draws text letter by letter. */
typedef struct { DWORD Start, End; } CodeRange;
static CodeRange CodeRanges[16]; static int CodeRangeCount, RflScanned, LogHud;
static volatile LONG HudLogLines;
#define MaximumHudLogLines 300
static DWORD SeenChains[1024]; static int SeenChainCount;

static void AddCodeRanges(HMODULE Module) {
    if (!Module) return;
    IMAGE_NT_HEADERS *Headers = (IMAGE_NT_HEADERS *)((char *)Module + ((IMAGE_DOS_HEADER *)Module)->e_lfanew);
    IMAGE_SECTION_HEADER *Section = IMAGE_FIRST_SECTION(Headers);
    for (int Index = 0; Index < Headers->FileHeader.NumberOfSections && CodeRangeCount < 16; Index++, Section++)
        if (Section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            DWORD Start = (DWORD)Module + Section->VirtualAddress;
            CodeRanges[CodeRangeCount++] = (CodeRange){ Start, Start + Section->Misc.VirtualSize };
        }
}
static int InCode(DWORD Value, DWORD Before) {
    for (int Index = 0; Index < CodeRangeCount; Index++)
        if (Value >= CodeRanges[Index].Start + Before && Value < CodeRanges[Index].End) return 1;
    return 0;
}
static int IsReturnAddress(DWORD Value) {
    if (!InCode(Value, 7)) return 0;
    const unsigned char *Code = (const unsigned char *)Value;
    if (Code[-5] == 0xE8) return 1;                                                   /* call rel32 */
    if (Code[-6] == 0xFF && (Code[-5] & 0x38) == 0x10 && ((Code[-5] & 0xC0) == 0x80 || (Code[-5] & 0xC7) == 0x05)) return 1;
    if (Code[-3] == 0xFF && (Code[-2] & 0xF8) == 0x50) return 1;                      /* call [reg + disp8] */
    if (Code[-7] == 0xFF && Code[-6] == 0x94) return 1;                               /* call [reg + reg + disp32] */
    if (Code[-4] == 0xFF && Code[-3] == 0x54) return 1;                               /* call [reg + reg + disp8] */
    if (Code[-2] == 0xFF && ((Code[-1] & 0xF8) == 0xD0 || (Code[-1] & 0xF8) == 0x10)) return 1;   /* call reg, call [reg] */
    return 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"   /* fs-relative read of the thread's stack base */
static const DWORD *StackTop(void) { return (const DWORD *)((NT_TIB *)NtCurrentTeb())->StackBase; }
#pragma GCC diagnostic pop

static void LogHudDraw(const void *Arguments, const char *What, float X, float Y, float Width, float Height);
static void LogHudQuad(const float *Arguments, float X, float Y, float Width, float Height) {
    LogHudDraw(Arguments, "quad", X, Y, Width, Height);
}

/* TTB interface scene (health bar, ammunition, weapon panel...): leaf nodes draw through texture slot 34 (vtable
   0x5EAF48 -> 0x450550, thiscall, "ret 8"), pre-transformed vertices (x, y, z, rhw, colour, specular, u, v: 32 bytes
   each). Sprite leaves (vtable 0x5EA748, slot 3 0x43F120) and text leaves (vtable 0x5EA798, slot 3 0x43F7A0, through
   the font with a matrix) scale the 640 x 448 design uniformly, Scale = min(width / 640, height / 448) of the current
   camera's viewport (+0x2A4, +0x2A8), from its origin (+0x28C, +0x294): on a wide screen the design sits at the left.
   While a game runs (flag 0x80, paused too) each leaf (a text: the whole string) is moved by 0, Extra / 2 or Extra
   (Extra = width - 640 * Scale) after the third of the design it is in; otherwise by Extra / 2. Only vertices drawn inside those leaves move. */
typedef int (ThisCall *DrawVerticesFunctionType)(void *, const float *, int);
typedef void (ThisCall *UiLeafDrawFunctionType)(void *, DWORD);
static DrawVerticesFunctionType OriginalDrawVertices;
static UiLeafDrawFunctionType OriginalUiSprite, OriginalUiText;
static void ThisCall UiSprite(void *Node, DWORD State) { UiLeafDepth++; OriginalUiSprite(Node, State); UiLeafDepth--; }
static void ThisCall UiText(void *Node, DWORD State) { UiLeafDepth++; OriginalUiText(Node, State); UiLeafDepth--; }

static int UiRegion(float *Origin, float *Width, float *Extra) {
    DWORD Renderer = *(DWORD *)A->RendererPointer;
    if (!Renderer || !*(DWORD *)(Renderer + 0x40)) return 0;
    const char *Camera = (const char *)(*(DWORD *)(Renderer + 0x40) + A->CameraStride * *(int *)(Renderer + 0x4C));
    float ViewWidth = (float)*(const int *)(Camera + 0x2A4), ViewHeight = (float)*(const int *)(Camera + 0x2A8);
    if (!(ViewWidth > 0.0f && ViewHeight > 0.0f)) return 0;
    float Scale = fminf(ViewWidth / 640.0f, ViewHeight / 448.0f);
    *Origin = *(const float *)(Camera + 0x28C); *Width = 640.0f * Scale; *Extra = ViewWidth - *Width;
    return *Extra > 0.5f;
}
static int ThisCall HudDrawVertices(void *Texture, const float *Vertices, int Count) {
    if (LogHud && Vertices && Count > 0 && Count < 4096) {
        float Left = 3.0e38f, Right = -3.0e38f, Top = 3.0e38f, Bottom = -3.0e38f;
        for (int Index = 0; Index < Count; Index++) {
            const float *Vertex = Vertices + Index * 8;
            if (Vertex[0] < Left) Left = Vertex[0];
            if (Vertex[0] > Right) Right = Vertex[0];
            if (Vertex[1] < Top) Top = Vertex[1];
            if (Vertex[1] > Bottom) Bottom = Vertex[1];
        }
        char What[48]; snprintf(What, sizeof What, "%d vertices%s", Count, UiLeafDepth ? " (interface)" : "");
        LogHudDraw(&Vertices, What, Left, Top, Right - Left, Bottom - Top);
    }
    if (TextPass == TextMeasuring && (!UiLeafDepth || !Vertices || Count <= 0 || Count > 256)) return 1;   /* drawn in the next pass */
    float Origin, Width, Extra;
    if (!UiLeafDepth || !OriginalHudQuad || !Vertices || Count <= 0 || Count > 256 || !UiRegion(&Origin, &Width, &Extra))
        return TextPass == TextMeasuring ? 1 : OriginalDrawVertices(Texture, Vertices, Count);
    float Left = 3.0e38f, Right = -3.0e38f;
    for (int Index = 0; Index < Count; Index++) {
        float X = Vertices[Index * 8];
        if (X < Left) Left = X;
        if (X > Right) Right = X;
    }
    /* Positions in the design, as fractions of the screen width, so the anchor rules of the rest of the HUD apply. */
    float VirtualLeft = (Left - Origin) / Width * ScreenWidth, VirtualRight = (Right - Origin) / Width * ScreenWidth;
    if (TextPass == TextMeasuring) {
        if (VirtualLeft < TextLeft) TextLeft = VirtualLeft;
        if (VirtualRight > TextRight) TextRight = VirtualRight;
        return 1;   /* nothing to see in this pass */
    }
    float Offset = Extra * 0.5f;
    /* The interface scene holds only the in-game HUD (menus are .rfl screens), so it keeps the play layout whenever a
       game is running (flag 0x80), paused or not. */
    int Anchor = AnchorNone;
    if (*(const DWORD *)A->GameFlags & 0x80) {
        Anchor = TextPass == TextDrawing && TextAnchor != AnchorNone ? TextAnchor : AnchorOf((VirtualLeft + VirtualRight) * 0.5f);
        Offset = Anchor == AnchorLeft ? 0.0f : Anchor == AnchorRight ? Extra : Extra * 0.5f;
    }
    float Moved[256 * 8];
    memcpy(Moved, Vertices, (size_t)Count * 32);
    float Top = 3.0e38f, Bottom = -3.0e38f;
    for (int Index = 0; Index < Count; Index++) {
        Moved[Index * 8] += Offset;
        if (Moved[Index * 8 + 1] < Top) Top = Moved[Index * 8 + 1];
        if (Moved[Index * 8 + 1] > Bottom) Bottom = Moved[Index * 8 + 1];
    }
    if (HudScale != 1.0f && Anchor != AnchorNone)
        for (int Index = 0; Index < Count; Index++) ScaleHudPoint(&Moved[Index * 8], &Moved[Index * 8 + 1], Anchor, (Top + Bottom) * 0.5f);
    return OriginalDrawVertices(Texture, Moved, Count);
}
static int UiSceneHooked;
static int HookUiScene(void) {
    if (UiSceneHooked || CurrentGame != GameTTB) return UiSceneHooked;
    OriginalDrawVertices = (DrawVerticesFunctionType)0x450550;
    OriginalUiSprite = (UiLeafDrawFunctionType)0x43F120;
    OriginalUiText = (UiLeafDrawFunctionType)0x43F7A0;
    UiSceneHooked = ReplaceVtableSlot(0x5EAF48, 34, 0x450550, (DWORD)HudDrawVertices) +
                    ReplaceVtableSlot(0x5EA748, 3, 0x43F120, (DWORD)UiSprite) + ReplaceVtableSlot(0x5EA798, 3, 0x43F7A0, (DWORD)UiText);
    return UiSceneHooked;
}

static void LogHudDraw(const void *Arguments, const char *What, float X, float Y, float Width, float Height) {
    if (!RflScanned) {
        HMODULE Rfl = GetModuleHandleA(CurrentGame == GameTTB ? "SufferingTTB.rfl" : "Suffering.rfl");
        if (Rfl) { AddCodeRanges(Rfl); RflScanned = 1; }
    }
    DWORD Chain[4] = {0}; int Found = 0;
    const DWORD *Slot = (const DWORD *)Arguments, *Top = StackTop();
    for (int Index = 0; Index < 1024 && Slot + Index < Top && Found < 4; Index++)
        if (IsReturnAddress(Slot[Index])) Chain[Found++] = Slot[Index];
    const DWORD Flags = *(const DWORD *)A->GameFlags;
    DWORD Hash = (2166136261u ^ Flags) * 16777619u;
    for (int Index = 0; Index < 4; Index++) Hash = (Hash ^ Chain[Index]) * 16777619u;
    for (int Index = 0; Index < SeenChainCount; Index++) if (SeenChains[Index] == Hash) return;
    if (SeenChainCount < 1024) SeenChains[SeenChainCount++] = Hash;
    if (InterlockedIncrement(&HudLogLines) > MaximumHudLogLines) return;
    char Names[4][MAX_PATH + 32];
    for (int Index = 0; Index < 4; Index++) { if (Chain[Index]) AddressName(Chain[Index], Names[Index], sizeof Names[Index]); else strcpy(Names[Index], "-"); }
    Log("[hud] %s (flags %08lx) %s %.1f,%.1f %.1fx%.1f by %s <- %s <- %s <- %s", PlayLayout() ? "play" : "menu",
        (unsigned long)Flags, What, X, Y, Width, Height, Names[0], Names[1], Names[2], Names[3]);
}

/* Logs every font once (up to 32) with its resource id, its first string and the caller of the draw. */
static void LogFont(const TextCall *Call, DWORD Caller) {
    static DWORD Seen[32]; static int SeenCount;
    const DWORD Font = Call->Arguments[0];
    if (!Font || SeenCount >= 32) return;
    for (int Index = 0; Index < SeenCount; Index++) if (Seen[Index] == Font) return;
    Seen[SeenCount++] = Font;
    char Text[48] = {0}; const wchar_t *Source = (const wchar_t *)Call->Arguments[1];
    for (int Index = 0; Source && Source[Index] && Index < 40; Index++) Text[Index] = Source[Index] < 128 && Source[Index] >= 32 ? (char)Source[Index] : '?';
    char Name[MAX_PATH + 32]; AddressName(Caller, Name, sizeof Name);
    Log("[font] %08lx id %08lx: \"%s\" drawn by %s", (unsigned long)Font, (unsigned long)((const DWORD *)Font)[1], Text, Name);
}

/* ================================================================ Apply */
void RiotDisplayApply(void) {
    if (IniInt("Display", "DpiAware", 1)) {
        typedef BOOL (WINAPI *SetProcessDPIAwareFunctionType)(void);
        SetProcessDPIAwareFunctionType SetAware = (SetProcessDPIAwareFunctionType)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDPIAware");
        Log("DPI aware: %s", SetAware && SetAware() ? "ok" : "FAILED");
    }
    if (CurrentGame == GameUnknown) return;
    A = &Addresses[CurrentGame];

    if (IniInt("Display", "AllResolutions", 1))
        Log("All adapter resolutions listed (full screen and windowed): %s", ApplyAllResolutions() ? "ok" : "FAILED (unexpected bytes)");

    if (IniInt("Widescreen", "Enable", 1)) ApplyWidescreen();
    if (IniInt("Text", "Outline", 1)) {
        TextOutlineSize = IniFloat("Text", "OutlineSize", 2.0f);
        if (!(TextOutlineSize > 0.0f && TextOutlineSize <= 8.0f)) { Log("Text.OutlineSize=%g is invalid (use 0.5 to 8)", TextOutlineSize); TextOutlineSize = 2.0f; }
        char Colour[32]; GetPrivateProfileStringA("Text", "OutlineColor", "000000", Colour, sizeof Colour, IniPath);
        TextOutlineColour = strtoul(Colour, NULL, 16) & 0xFFFFFF;   /* RRGGBB */
        TextOutlineShadow = IniInt("Text", "OutlineShadow", 0);
        TextOutlineAll = IniInt("Text", "OutlineAll", 1);
        char Fonts[256]; GetPrivateProfileStringA("Text", "OutlineSkipFonts", "00050003", Fonts, sizeof Fonts, IniPath);
        for (char *Next = Fonts; *Next && SkipFontCount < 16;) {
            char *End; DWORD Id = strtoul(Next, &End, 16);
            if (End == Next) { Next++; continue; }
            SkipFonts[SkipFontCount++] = Id; Next = End;
        }
        if (SkipFontCount) Log("Text outline skips %d font(s) ([Text] OutlineSkipFonts)", SkipFontCount);
        TextOutline = HookText();
        Log("Text outline (%g px at 1080p, colour %06lX, shadow %s, %s): %s", TextOutlineSize, (unsigned long)TextOutlineColour,
            TextOutlineShadow ? "kept" : "replaced", TextOutlineAll ? "all text" : "text with a shadow only", TextOutline ? "ok" : "FAILED (unexpected vtable)");
    }
    {
        char Scale[32]; GetPrivateProfileStringA("Cursor", "Scale", "0", Scale, sizeof Scale, IniPath);
        CursorScale = (float)atof(Scale); CursorAutomatic = CursorScale == 0.0f;
        if (CursorScale < 0.0f || CursorScale > 8.0f) { Log("Cursor.Scale=%s is invalid (0 = automatic, or 0.5 to 8)", Scale); CursorScale = 0.0f; CursorAutomatic = 1; }
        if (CursorAutomatic || CursorScale != 1.0f || (OriginalHudQuad && IniInt("Widescreen", "Enable", 1))) {
            if (!ScreenManagerRegistered++) RiotTimingOnFrame(ScreenManagerFrame);
            if (CursorAutomatic) Log("Cursor scale automatic (screen height / 480): %s", HookCursor() ? "ok" : "FAILED (unexpected vtable)");
            else Log("Cursor scale x%g: %s", CursorScale, HookCursor() ? "ok" : "FAILED (unexpected vtable)");
        }
    }
    if (IniInt("Debug", "LogCameras", 0)) ApplyCameraLog();
    if (IniInt("Debug", "LogHud", 0)) {
        AddCodeRanges(GetModuleHandleA(NULL)); LogHud = 1;
        if (CurrentGame == GameTTB) Log("HUD log: vertex drawing %s", HookUiScene() ? "watched" : "FAILED (unexpected vtable)");
        Log("HUD log (first %d call chains): %s", MaximumHudLogLines, OriginalHudQuad ? "on" : "off (needs [Widescreen] Enable=1 and Hud=1)");
    }

    if (IniInt("Display", "FixFrontBufferCopy", 1)) {
        OriginalCopyFromFrontBuffer = (CopyFromFrontBufferFunctionType)A->CopyFromFrontBuffer;
        Log("CopyFromFrontBuffer checks its texture: %s",
            ReplaceVtableSlot(A->CopyFromFrontBufferSlot, 0, A->CopyFromFrontBuffer, (DWORD)CheckedCopyFromFrontBuffer) ? "ok" : "FAILED (unexpected vtable)");
    }

    if (IniInt("Display", "ConventionalMessageLoop", 1))
        Log("Main message loop without message filters: %s",
            RepointImportUse(A->MainLoopPeekLoad, A->PeekMessageSlot, &UnfilteredPeekMessageSlot) ? "ok" : "FAILED (unexpected bytes)");

    if (IniInt("Display", "WaitInConfigurationDialog", 1))
        Log("Configuration dialog waits for messages: %s",
            RepointImportUse(A->DialogLoopPeekLoad, A->PeekMessageSlot, &WaitDialogMessageSlot) ? "ok" : "FAILED (unexpected bytes)");
}
