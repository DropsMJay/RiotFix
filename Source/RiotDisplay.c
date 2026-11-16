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
    /* The camera carries two lenses: the one at LensOffset, which the horizontal angle comes from (PIH +0x27C), and
     * a second at SecondLensOffset for the vertical (PIH +0x270). Widescreen scales only the first, which is what
     * Hor+ means. Anything that decides what is worth drawing from the second one would then be working with the
     * narrower view the game had before, and would leave out what has just come into sight at the sides. */
    DWORD SecondLensOffset;
    DWORD HudQuadCalls[2], HudQuad;
    DWORD CutsceneBarsCall, CutsceneBars;
    DWORD CameraVtable;                                           /* vtable of the renderer's cameras */
    int FieldOfViewSlot, SetLensSlot, SecondFieldOfViewSlot;      /* PIH 1, 2, 3; TTB 2, 3, 4 */
    DWORD SetFieldOfView, SetSecondFieldOfView;
    float FrustumHalfWidth;                                       /* half width at lens distance (PIH +0x250, TTB +0x254) */
    DWORD RendererVtable;
    int RendererFieldOfViewSlot, RendererSecondSlot;              /* PIH 10 and 12; TTB 12 and 14 */
    DWORD RendererFieldOfView, RendererSecond;
    DWORD LevelFieldOfViewCalls[8];          /* .rfl offsets of the return addresses that set the normal play angle:
                                                the level's start, and in the TTB also the return from first person
                                                (+0x16F0F, which puts the renderer's own default back) */
    DWORD FirstPersonFieldOfViewCall;        /* .rfl offset of the return address (TTB only) */
    DWORD RendererAspect;                    /* renderer slot 41 (+0xA4): height / width ratio, thiscall, float */
    DWORD IconAspectCall, IconTangentOperand; /* .rfl offsets: return address of the 3D icon's +0xA4 call; "fmul [0.5]" */
    /* The archive and map picture ([Widescreen] ArchiveScale): .rfl offsets of the GUI class's draw method and of
     * the entry it occupies in that class's vtable (PIH 0x10026B00 at 0x10150808, TTB 0x19ED330 at 0x1A3189C). */
    DWORD ArchiveDraw, ArchiveDrawSlot;
    DWORD Direct3DCreateSlot;                 /* the exe's import slot for d3d9!Direct3DCreate9 */
    /* Whether a scripted sequence currently owns the view ([Widescreen] OriginalCutsceneFraming). SequenceManager is
     * the manager object itself (a static, not a pointer to one); CameraHeldByRun is its method that answers "does
     * any live run hold the view camera", a thiscall with no arguments returning 0 or 1; RunCountOffset is where the
     * run count sits inside the manager, read first so a call is never made against an unbuilt object. */
    DWORD SequenceManager, CameraHeldByRun, RunCountOffset;
} DisplayAddresses;

static const DisplayAddresses Addresses[GameCount] = {
    [GamePIH] = { 0x58B38C, 0x4386FE, 0x439EF3, 0x4B3689, 0x4B3690, 0x4B35D9, 0x5C6CE8, 0x590088, 0x448980, 0x64,
                  0x5945E8, { 0x4B1F30, 0x4B1F74, 0x4B1F90, 0x4B1FBE, 0x4B221C, 0x4B223E, 0x4B23E7 },
                  0x4B1F00, 0x27C, 0x2C0, 0x5C5D7C, 0x5C6CEC, 0x5C6E70, 0x270,
                  { 0x44A6C0, 0x44AB58 }, 0x44A390, 0x48C729, 0x4B0B30,
                  0x594C18, 1, 2, 3, 0x4B21F0, 0x4B2230, 64.0f,
                  0x594940, 10, 12, 0x4B0D90, 0x4B0E10,
                  { 0xB2367, 0xB2373, 0xB29FB, 0xB2A07, 0xB2FB0, 0xB2FBC, 0, 0 }, 0,
                  0x4B0A60, 0x687E7, 0x68834, 0x26B00, 0x150808, 0x58B420,
                  0x5C9010, 0x44C6A0, 0x44A },
    [GameTTB] = { 0x5E53C8, 0x43BE3E, 0x43D445, 0x4BBC5D, 0x4BBC64, 0x4BBBAD, 0x639EC8, 0x5EAFF4, 0x44EB40, 0x68,
                  0x5EF3D0, { 0x4BA6C0, 0x4BA704, 0x4BA720, 0x4BA74E, 0x4BABDC, 0x4BABFE, 0x4BACC9 },
                  0x4BA690, 0x280, 0x2C4, 0x6392F4, 0x639ECC, 0x63A04C, 0x274,
                  { 0x4506A0, 0x450B55 }, 0x451240, 0x493F69, 0x4B93E0,
                  0x5EFBA4, 2, 3, 4, 0x4BABB0, 0x4BABF0, 0.3125f,
                  0x5EF8D0, 12, 14, 0x4B9830, 0x4B95E0,
                  { 0x15D13B, 0x15D147, 0x15DD6B, 0x15DD77, 0x15E310, 0x15E31C, 0x16F0F, 0 }, 0x1DC810,
                  0, 0, 0, 0x14D330, 0x19189C, 0x5E545C,
                  0x63C738, 0x452960, 0x462 },
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

/* ================================================================ Full-screen refresh rate
 * In full screen the engine asks Direct3D for the rate the mode list gave it, and on a screen that offers several
 * for the same resolution that is not always the one wanted. With [Display] RefreshRate set, RiotFix writes it into
 * the presentation parameters of every full-screen device the game creates or resets. Windowed modes are left
 * alone, where Direct3D takes only 0. The hook sits on the exe's own Direct3DCreate9 import slot, so it works
 * whether the game creates Direct3D itself or RiotFix does ([System] SkipDirectPlayCheck). */
typedef struct {
    UINT BackBufferWidth, BackBufferHeight; DWORD BackBufferFormat; UINT BackBufferCount;
    DWORD MultiSampleType, MultiSampleQuality, SwapEffect; HWND DeviceWindow; BOOL Windowed;
    BOOL EnableAutoDepthStencil; DWORD AutoDepthStencilFormat, Flags;
    UINT FullScreenRefreshRateInHz, PresentationInterval;
} PresentParameters;
static int RefreshRate;
static void CarryRefreshRate(PresentParameters *Parameters, const char *What) {
    if (!Parameters || Parameters->Windowed) return;
    static int Logged;
    if (Logged < 4) {
        Logged++;
        Log("[display] %s: %ux%u, %u Hz -> %d Hz", What, Parameters->BackBufferWidth, Parameters->BackBufferHeight,
            Parameters->FullScreenRefreshRateInHz, RefreshRate);
    }
    Parameters->FullScreenRefreshRateInHz = (UINT)RefreshRate;
}
typedef HRESULT (STDMETHODCALLTYPE *ResetDeviceFunctionType)(void *, PresentParameters *);
static ResetDeviceFunctionType OriginalResetDevice;
static HRESULT STDMETHODCALLTYPE ResetDevice(void *Self, PresentParameters *Parameters) {
    CarryRefreshRate(Parameters, "device reset");
    return OriginalResetDevice(Self, Parameters);
}
typedef HRESULT (STDMETHODCALLTYPE *MakeDeviceFunctionType)(void *, UINT, DWORD, HWND, DWORD, PresentParameters *, void **);
static MakeDeviceFunctionType OriginalMakeDevice;
/* Slot 16 of IDirect3D9 is CreateDevice, and slot 16 of IDirect3DDevice9 is Reset. */
static HRESULT STDMETHODCALLTYPE MakeDevice(void *Self, UINT Adapter, DWORD Type, HWND Window, DWORD Behaviour,
                                            PresentParameters *Parameters, void **Device) {
    CarryRefreshRate(Parameters, "device created");
    const HRESULT Result = OriginalMakeDevice(Self, Adapter, Type, Window, Behaviour, Parameters, Device);
    if (Result >= 0 && Device && *Device && !OriginalResetDevice) {
        DWORD *const Vtable = *(DWORD **)*Device;
        OriginalResetDevice = (ResetDeviceFunctionType)Vtable[16];
        const DWORD Ours = (DWORD)ResetDevice;
        if (!WriteCode((DWORD)&Vtable[16], &Ours, 4)) {
            OriginalResetDevice = NULL;
            Log("[display] the refresh rate will be lost on a mode change (Reset could not be watched)");
        }
    }
    return Result;
}
typedef void *(WINAPI *Direct3DCreate9FunctionType)(UINT);
static Direct3DCreate9FunctionType OriginalDirect3DCreate9;
static void *WINAPI MakeDirect3D(UINT Version) {
    void *const Object = OriginalDirect3DCreate9(Version);
    if (Object && !OriginalMakeDevice) {
        DWORD *const Vtable = *(DWORD **)Object;
        OriginalMakeDevice = (MakeDeviceFunctionType)Vtable[16];
        const DWORD Ours = (DWORD)MakeDevice;
        if (!WriteCode((DWORD)&Vtable[16], &Ours, 4)) {
            OriginalMakeDevice = NULL;
            Log("[display] refresh rate: FAILED (the Direct3D vtable could not be written)");
        }
    }
    return Object;
}
static int ApplyRefreshRate(void) {
    OriginalDirect3DCreate9 = *(Direct3DCreate9FunctionType *)A->Direct3DCreateSlot;
    if (!OriginalDirect3DCreate9) return 0;
    const DWORD Ours = (DWORD)MakeDirect3D;
    return WriteCode(A->Direct3DCreateSlot, &Ours, 4);
}

/* ================================================================ Widescreen */
static float OriginalLensConstant, LensConstant, AppliedLensConstant;
static int WidenCulling;
static float PlayFieldOfView, FirstPersonFieldOfView;   /* [Widescreen] ThirdPersonFieldOfView and
                                                           FirstPersonFieldOfView: horizontal degrees, 0 = the game's */
static int WatchRendererFieldOfView(void);
static float ReadFieldOfView(const char *Key) {
    float Degrees = IniFloat("Widescreen", Key, 0.0f);
    if (Degrees == 0.0f) return 0.0f;
    if (!(Degrees >= 30.0f && Degrees <= 170.0f)) { Log("Widescreen.%s=%g is invalid (use 30 to 170, or 0)", Key, Degrees); return 0.0f; }
    return Degrees;
}
static float ScreenWidth = 640.0f, ScreenHeight = 480.0f, AspectDifference = 1.0f, HudOffset = 0.0f;
static int PictureThisFrame, PictureLastFrame;   /* a full-screen picture was drawn this frame / the frame before */
static int CurrentScreenTiles;                   /* tiles drawn by the screen being drawn now */
static int CutsceneBarsMode = 1, WidescreenHud;   /* [Widescreen] Hud, only with Enable=1 */
static volatile int CutsceneBarsSeen, InCutscene;

/* Does a scripted sequence own the view right now? ([Widescreen] OriginalCutsceneFraming)
 *
 * This used to be answered with CutsceneBarsSeen - whether the game had drawn the cutscene bars - which was wrong
 * twice over. The engine puts the bars up for any scripted camera moment, not only for a cutscene, so the lens
 * constant went back to the game's 4:3 value for a moment and the view visibly pulled in; and the answer came from
 * the frame before, so every change of state left one frame drawn with the other framing, which showed up as the
 * picture popping at the edges. The right question is not what was drawn, it is whether a sequence has taken the
 * camera, and the engine answers that itself.
 *
 * The sequence manager keeps an array of live runs and a method that walks it: a run with flag 0x2000 is finished
 * and skipped, a run with flag 0x200 holds the view camera, and the method returns 1 at the first one that does.
 * The engine uses that same answer to decide whether to drop the player's input (PIH 0x4BCCEC), so it is exactly
 * "the player is watching, not playing". Taking it from the engine also makes the answer current rather than a
 * frame old. The header's own flag 0x40 answers a different question and is not used here: eight retail sequences
 * carry it with no camera track at all, and they play in the world while the player keeps his view and control.
 *
 * The run count is read before the call so that an unbuilt manager - at start-up, before the first level - cannot
 * send the engine walking an array that is not there yet. A count outside a sane range, or a game with no addresses
 * for this, falls back to what the bars said. */
typedef int (ThisCall *CameraHeldFunctionType)(void *);

static int SequenceHoldsCamera(void) {
    if (!A->SequenceManager || !A->CameraHeldByRun) return CutsceneBarsSeen;
    void *Manager = (void *)A->SequenceManager;
    const LONG Runs = *(const LONG *)(A->SequenceManager + A->RunCountOffset);
    if (Runs < 0 || Runs > 4096) return CutsceneBarsSeen;
    if (!Runs) return 0;
    return ((CameraHeldFunctionType)A->CameraHeldByRun)(Manager) != 0;
}
static int OriginalCutsceneFraming = 1;   /* [Widescreen] OriginalCutsceneFraming */
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
        if (Lens <= 0.0f || Lens >= 3.0e38f) continue;
        float *Second = (float *)((char *)Camera + A->SecondLensOffset);
        if (WidenCulling && *Second > 0.0f && *Second < 3.0e38f) *Second *= Ratio;
        ((SetLensFunctionType)A->SetLens)(Camera, Lens * Ratio);   /* writes the lens and rebuilds the view */
        if (RescaleLogs < 4)
            Log("[widescreen] camera %d: lens %g -> %g, second lens %g", Index, Lens, Lens * Ratio, *Second);
        Rescaled++;
    }
    if (RescaleLogs++ < 8) Log("[widescreen] lens constant %g: %d camera(s) rescaled x%.4f", LensConstant, Rescaled, Ratio);
}

/* Once per frame, before the game's frame: reads the display mode and, with the widescreen field of view on, keeps
   the lens constant in step with it. The screen size is read even with the widescreen off: the interface size and the
   cursor need it. */
static int WidescreenOn;
static void ArchiveFrameStart(void);
static void DisplayFrame(void) {
    ArchiveFrameStart();
    const int HoldsNow = SequenceHoldsCamera();
    if (HoldsNow != InCutscene) {
        static int Changes;
        if (Changes++ < 20)
            Log("[widescreen] a sequence %s the view: framing goes %s", HoldsNow ? "takes" : "gives back",
                OriginalCutsceneFraming ? (HoldsNow ? "back to the game's 4:3" : "to widescreen") : "nowhere (option off)");
    }
    InCutscene = HoldsNow; CutsceneBarsSeen = 0;
    PictureLastFrame = PictureThisFrame; PictureThisFrame = 0; CurrentScreenTiles = 0;
    ResetFrameAnchors();
    const int *Mode = (const int *)A->DisplayMode;
    if (Mode[0] <= 0 || Mode[1] <= 0 || Mode[0] > 32767 || Mode[1] > 32767) return;
    ScreenWidth = (float)Mode[0]; ScreenHeight = (float)Mode[1];
    float Aspect = ScreenWidth / ScreenHeight;
    AspectDifference = Aspect / (4.0f / 3.0f);
    HudOffset = (ScreenWidth - ScreenHeight * (4.0f / 3.0f)) * 0.5f;
    if (!WidescreenOn) return;
    /* [Widescreen] OriginalCutsceneFraming: during a cutscene the lens constant goes back to the game's, so the
       picture keeps the horizontal angle it was framed with; what a wide screen loses is the top and the bottom,
       where the cutscene's own black bars already are. */
    float Wanted = OriginalCutsceneFraming && InCutscene ? OriginalLensConstant : OriginalLensConstant / AspectDifference;
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
/* Screens that draw a full-screen picture in tiles (loading screens, menu backgrounds) are never the HUD, even while
   the game counts as running: their own elements (the loading bar) keep the menu layout and the game's size. The
   class is remembered by its vtable, so the next level's loading screen is known from its first frame. */
static void *PictureVtables[8]; static int PictureVtableCount;
static int PictureScreen(void *Vtable) {
    for (int Index = 0; Index < PictureVtableCount; Index++) if (PictureVtables[Index] == Vtable) return 1;
    return 0;
}
static void TileDrawn(void) {
    if (++CurrentScreenTiles >= 3) PictureThisFrame = 1;
    if (!CurrentScreen || CurrentScreenTiles < 3) return;   /* a picture is six tiles (3 x 2) */
    CurrentScreenIsPlay = 0;
    void *Vtable = *(void **)CurrentScreen;
    if (!PictureScreen(Vtable) && PictureVtableCount < 8) {
        PictureVtables[PictureVtableCount++] = Vtable;
        Log("[widescreen] screen class %p draws full-screen pictures: menu layout", Vtable);
    }
}
static void *ScreenManager;   /* the .rfl's screen manager, kept for the cursor */
static int CursorPass;        /* inside the cursor's Render call: HudQuad leaves the quad as RiotFix placed it */
static DWORD OriginalScreenManager;

/* 1 while the HUD keeps its 4:3 sizes and the menus are drawn inside a centred 4:3 box. */
static int MenuBox(void) { return OriginalHudQuad && WidescreenHud && AspectDifference > 1.0f; }
static int PlayLayout(void) {
    const DWORD Flags = *(const DWORD *)A->GameFlags;
    /* A quad the play screen itself drew is the HUD, whatever else is on the screen. This has to come first, and
       ahead of the picture test below, because of the cross-fade at the start of a level: the loading picture
       fades out over live gameplay, so for as long as it lasts both screens draw in the same frame, the play
       screen first. The picture test is a per-frame flag and cannot tell those two apart - it held the HUD in the
       4:3 box for the whole fade and let it jump to its real place the moment the picture stopped. Letting the
       picture screen's own quads through is not a risk: CurrentScreenIsPlay is cleared for any screen that draws
       a picture, both by its remembered class and by the tile count of the frame in hand. */
    if ((Flags & 0x80) && CurrentScreenIsPlay) return 1;
    /* Otherwise, while a full-screen picture is on the screen (a loading screen), nothing is the HUD: the TTB draws
       its loading screen outside the screen manager, so the picture itself is the only sign, and it lasts more than
       one frame. */
    if (PictureThisFrame || PictureLastFrame) return 0;
    return (Flags & 0x81) == 0x80;
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
        CurrentScreen = Screen; CurrentScreenTiles = 0;
        CurrentScreenIsPlay = Known && Known->Frames >= 30 && !PictureScreen(*(void **)Screen);
        /* The play screen takes 30 drawn frames to be believed, which is half a second in which its HUD is placed
           as a menu's. That matters at the start of a level, where it lands inside the fade, so it is worth saying
           out loud when it happens rather than being read off a screenshot. */
        if (Known && !PictureScreen(*(void **)Screen)) {
            static int Warmups, LastFrames;
            if (Known->Frames < 30) LastFrames = Known->Frames;
            else if (LastFrames && Warmups++ < 8) {
                Log("[widescreen] the play screen %p was believed after %d frame(s); until then its HUD was placed "
                    "as a menu's", Screen, Known->Frames);
                LastFrames = 0;
            }
        }
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
enum { TextIdle, TextMeasuring, TextDrawing };
static int TextPass, TextAnchor;
static float TextLeft, TextRight;
typedef struct { DWORD Hash; float Y; int Anchor; } TextAnchorEntry;
static TextAnchorEntry FrameAnchors[64]; static int FrameAnchorCount;

static int AnchorOf(float Centre) {
    Centre /= ScreenWidth;
    return Centre < 1.0f / 3.0f ? AnchorLeft : Centre > 2.0f / 3.0f ? AnchorRight : AnchorCentre;
}

/* The interface size ([Accessibility] InterfaceScale, kept by RiotAccessibility) is applied to every HUD element and
   its text; menus are not scaled. */
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
/* ================================================================ Archive and map picture
 * The archive screen (notes, photos and maps) draws its picture tile by tile, each tile at the size of its own
 * texture in pixels: the HUD log shows 128.0 x 128.0 quads at every resolution, and the same picture measures 508
 * pixels tall at 1024x768 and at 1920x1080. So the picture keeps the size it was drawn for, around 640x480, and
 * takes less and less of the screen as the resolution grows, until a note cannot be read. The engine does cap the
 * scale at 1.0 (PIH 0x10026971, TTB 0x19EBEC7, both comparing against the shared 1.0 constant), but that cap is
 * reached only by items the level marks as zoomable (Map Item p57); notes take another limit. Instead of patching
 * either, RiotFix hooks the draw method in the class vtable and scales the quads it draws, which covers notes,
 * photos and maps in both games.
 *
 * Scaling has to be around one point common to every tile, or the tiles separate. That point is the centre of the
 * picture's bounding box in the frame before: the screen is still, so the box does not move, and the first frame
 * after it opens is drawn at the game's own size. The inventory rows above keep a two-frame memory the same way.
 *
 * The same hook settles the horizontal squeeze. The 4:3 menu box divides X and the width and leaves Y and the
 * height alone, which is right for everything laid out in fractions of the screen, but this picture's size is in
 * pixels: dividing only its width left it 0.75 as wide as it should be on a 16:9 screen (measured: 423 pixels wide
 * with Hud=0 against 317 with Hud=1). Here the whole picture is placed at once, so its shape is kept. */
static float ArchiveScale;              /* [Widescreen] ArchiveScale: how much of the screen height it takes. 0 = off */
static int ArchiveDrawing;              /* inside the class's draw method */
typedef struct { float Left, Right, Top, Bottom; int Seen; } ArchiveBox;
static ArchiveBox ArchiveBoxes[2]; static int ArchiveBoxFrame;
typedef int (ThisCall *ArchiveDrawFunctionType)(void *);
static ArchiveDrawFunctionType OriginalArchiveDraw;

static int ThisCall ArchiveDraw(void *Self) {
    ArchiveDrawing++;
    const int Result = OriginalArchiveDraw(Self);
    ArchiveDrawing--;
    return Result;
}
static void RememberArchiveQuad(float X, float Y, float Width, float Height) {
    ArchiveBox *Box = &ArchiveBoxes[ArchiveBoxFrame];
    if (!Box->Seen) { *Box = (ArchiveBox){ X, X + Width, Y, Y + Height, 1 }; return; }
    if (X < Box->Left) Box->Left = X;
    if (X + Width > Box->Right) Box->Right = X + Width;
    if (Y < Box->Top) Box->Top = Y;
    if (Y + Height > Box->Bottom) Box->Bottom = Y + Height;
}
/* How much to grow the picture, and the centre to grow it around, both from the frame before. 0 = leave it as it
   is (the option is off, or the picture has not been seen yet). */
static float ArchiveFactor(float *CentreX, float *CentreY) {
    const ArchiveBox *Box = &ArchiveBoxes[ArchiveBoxFrame ^ 1];
    if (!Box->Seen || ArchiveScale <= 0.0f) return 0.0f;
    const float Width = Box->Right - Box->Left, Height = Box->Bottom - Box->Top;
    if (!(Width > 1.0f) || !(Height > 1.0f) || !(ScreenHeight > 1.0f)) return 0.0f;
    *CentreX = (Box->Left + Box->Right) * 0.5f; *CentreY = (Box->Top + Box->Bottom) * 0.5f;
    float Factor = ScreenHeight * ArchiveScale / Height;
    /* A picture wider than it is tall must still fit the 4:3 box, where the rest of this screen lives. */
    const float Room = ScreenHeight * (4.0f / 3.0f) * 0.9f;
    if (Width * Factor > Room) Factor = Room / Width;
    return Factor > 1.0f ? Factor : 0.0f;
}
/* Called once a frame from DisplayFrame: starts a new box and, as soon as the game has loaded its .rfl, puts the
   hook in the class vtable. The entry is only changed when it still holds the address it is expected to hold. */
static void ArchiveFrameStart(void) {
    ArchiveBoxFrame ^= 1; ArchiveBoxes[ArchiveBoxFrame].Seen = 0;
    if (ArchiveScale <= 0.0f || OriginalArchiveDraw || !A->ArchiveDrawSlot) return;
    const DWORD Rfl = (DWORD)RflModule();
    if (!Rfl) return;
    DWORD *const Slot = (DWORD *)(Rfl + A->ArchiveDrawSlot);
    const DWORD Expected = Rfl + A->ArchiveDraw;
    static int Logged;
    if (*Slot != Expected) {
        if (!Logged++) Log("[widescreen] archive picture: FAILED (unexpected vtable entry %p)", (void *)*Slot);
        OriginalArchiveDraw = (ArchiveDrawFunctionType)-1;   /* do not look again */
        return;
    }
    const DWORD Ours = (DWORD)ArchiveDraw;
    OriginalArchiveDraw = (ArchiveDrawFunctionType)Expected;
    if (!WriteCode((DWORD)Slot, &Ours, 4)) {
        OriginalArchiveDraw = (ArchiveDrawFunctionType)-1;
        if (!Logged++) Log("[widescreen] archive picture: FAILED (the vtable could not be written)");
        return;
    }
    Log("[widescreen] archive and map picture: %g of the screen height", ArchiveScale);
}

static void __cdecl HudQuad(float *Quad, float X, float Y, float Width, float Height, float A6, float A7, float A8, float A9,
                            float A10, float A11, const DWORD *Colors, DWORD *A13, int A14) {
    if (LogHud) LogHudQuad((const float *)&Quad, X, Y, Width, Height);
    const float OriginalX = X, OriginalWidth = Width;
    const float TileWidth = ScreenWidth / 3.0f, TileHeight = ScreenHeight / 2.0f;
    /* One of the six tiles (3 x 2) a full-screen picture is drawn in (loading screens, menu backgrounds). */
    const int Tile = fabsf(Width - TileWidth) < 0.5f && fabsf(Height - TileHeight) < 0.5f && fabsf(fmodf(X, TileWidth)) < 0.5f;
    if (Tile && !CursorPass && TextPass != TextMeasuring) TileDrawn();
    if (ArchiveDrawing && TextPass != TextMeasuring && !CursorPass) RememberArchiveQuad(X, Y, Width, Height);
    if (TextPass == TextMeasuring) {
        if (X < TextLeft) TextLeft = X;
        if (X + Width > TextRight) TextRight = X + Width;
        Width = Height = 0.0f;   /* nothing to see in this pass */
    } else if (ArchiveDrawing && !CursorPass) {
        /* The whole picture at once, around its own centre, so its shape is kept and the tiles stay together. */
        float CentreX = 0.0f, CentreY = 0.0f;
        const float Factor = ArchiveFactor(&CentreX, &CentreY);
        if (Factor > 0.0f) {
            /* The factor is worked out from the asked size, so it gives the wanted height on its own: the 4:3 box
               moves the picture's centre but must not divide its size again, or its shape would be lost. */
            const float Placed = MenuBox() ? CentreX / AspectDifference + HudOffset : CentreX;
            X = Placed + (X - CentreX) * Factor; Y = CentreY + (Y - CentreY) * Factor;
            Width *= Factor; Height *= Factor;
        } else if (MenuBox()) {
            X = X / AspectDifference + HudOffset; Width /= AspectDifference;
        }
    } else if (MenuBox() && !CursorPass) {
        if (Tile) {
            /* The picture goes in the 4:3 box. */
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
    if (RiotInterfaceScale() != 1.0f && TextPass != TextMeasuring && !CursorPass && PlayLayout() && !(OriginalX == 0.0f && OriginalWidth == ScreenWidth) &&
        !Tile) {
        int Anchor = ForcedAnchor;
        if (Anchor == AnchorNone) Anchor = CellRowAnchor(OriginalX, Y, Y + Height);
        if (Anchor == AnchorNone) Anchor = TextPass == TextDrawing && TextAnchor != AnchorNone ? TextAnchor : AnchorOf(OriginalX + OriginalWidth * 0.5f);
        RiotScalePoint(&X, &Y, Anchor, Y + Height * 0.5f);
        Width *= RiotInterfaceScale(); Height *= RiotInterfaceScale();
    }
    OriginalHudQuad(Quad, X, Y, Width, Height, A6, A7, A8, A9, A10, A11, Colors, A13, A14);
}

static int TextNeedsAnchor(void) {
    return OriginalHudQuad && TextPass == TextIdle && (MenuBox() || RiotInterfaceScale() != 1.0f) &&
           (PlayLayout() || (UiLeafDepth && (*(const DWORD *)A->GameFlags & 0x80)));
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
   frame is drawn from, and HudQuad moves that frame (anchor, interface size). The cell's draw method (vtable entry
   Suffering.rfl+0x152728 -> +0x68630, thiscall, no arguments) is called with the rectangle moved the same way, so the
   icon and its depth clear follow the frame; the rectangle is put back afterwards. */
typedef int (ThisCall *IconCellDrawFunctionType)(void *);
static IconCellDrawFunctionType OriginalIconCellDraw;
static int ThisCall IconCellDraw(void *Cell) {
    float *Rect = (float *)((char *)Cell + 0x38);
    const float Saved[4] = { Rect[0], Rect[1], Rect[2], Rect[3] };
    if (OriginalHudQuad && PlayLayout() && (MenuBox() || RiotInterfaceScale() != 1.0f)) {
        float X = Rect[0], Y = Rect[1], Width = Rect[2], Height = Rect[3];
        const int Anchor = ForcedAnchor != AnchorNone ? ForcedAnchor : AnchorOf(X);   /* as its cell (see CellFrameDraw) */
        if (AspectDifference > 1.0f) {
            const float Right = ScreenWidth - X;
            Width /= AspectDifference;
            if (Anchor == AnchorLeft) X = X / AspectDifference;
            else if (Anchor == AnchorRight) X = ScreenWidth - Right / AspectDifference;
            else X = X / AspectDifference + HudOffset;
        }
        if (RiotInterfaceScale() != 1.0f) { RiotScalePoint(&X, &Y, Anchor, Y + Height * 0.5f); Width *= RiotInterfaceScale(); Height *= RiotInterfaceScale(); }
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
    WidescreenOn = 1;
    PlayFieldOfView = ReadFieldOfView("ThirdPersonFieldOfView");
    FirstPersonFieldOfView = ReadFieldOfView("FirstPersonFieldOfView");
    if (FirstPersonFieldOfView && !A->FirstPersonFieldOfViewCall) {
        Log("Widescreen.FirstPersonFieldOfView: not available in this game yet"); FirstPersonFieldOfView = 0.0f;
    }
    if (PlayFieldOfView || FirstPersonFieldOfView) {
        WatchRendererFieldOfView();
        Log("Widescreen horizontal field of view: third person %g, first person %g (0 = the game's)", PlayFieldOfView, FirstPersonFieldOfView);
    }
    WidescreenHud = IniInt("Widescreen", "Hud", 1);
    if (A->RendererAspect && IniInt("Widescreen", "FixInventoryIcons", 1)) {
        OriginalRendererAspect = (RendererAspectFunctionType)A->RendererAspect;
        FixIcons = ReplaceVtableSlot(A->RendererVtable, 41, A->RendererAspect, (DWORD)IconAspect);
        if (FixIcons) RiotTimingOnFrame(IconFrame); else Log("Inventory 3D icons: FAILED (unexpected vtable)");
    }
    WidenCulling = IniInt("Widescreen", "WidenCulling", 0);
    OriginalCutsceneFraming = IniInt("Widescreen", "OriginalCutsceneFraming", 1);
    Log("Cutscenes keep their original framing: %s", OriginalCutsceneFraming ? "on" : "off (widened like the game)");
    if (OriginalCutsceneFraming)
        Log("Cutscene framing asks the engine whether a sequence holds the view: %s",
            A->SequenceManager && A->CameraHeldByRun ? "yes" : "NO ADDRESSES, falling back to the cutscene bars");
    CutsceneBarsMode = IniInt("Widescreen", "CutsceneBars", 1);
    OriginalCutsceneBars = (CutsceneBarsFunctionType)A->CutsceneBars;
    Log("Cutscene bars %s: %s", CutsceneBarsMode ? "on" : "off",
        RedirectCall(A->CutsceneBarsCall, A->CutsceneBars, (DWORD)CutsceneBars) ? "ok" : "FAILED (unexpected bytes)");
}

/* The HUD quads pass through RiotFix when the widescreen layout (4:3 sizes, anchors, menu box) or an interface size
   other than the game's is wanted. The interface size works on its own, with the widescreen off. */
static void ApplyHudLayout(void) {
    if (!WidescreenHud && RiotInterfaceScale() == 1.0f && ArchiveScale <= 0.0f) return;
    OriginalHudQuad = (HudQuadFunctionType)A->HudQuad;
    int Count = RedirectCall(A->HudQuadCalls[0], A->HudQuad, (DWORD)HudQuad) + RedirectCall(A->HudQuadCalls[1], A->HudQuad, (DWORD)HudQuad);
    Log("HUD and menus placed by RiotFix (%s%s): %d/2 calls",
        WidescreenHud ? "widescreen 4:3" : RiotInterfaceScale() != 1.0f ? "interface size only" : "archive picture only",
        WidescreenHud && RiotInterfaceScale() != 1.0f ? ", interface size" : "", Count);
    Log("Text anchored per string: %s", RiotTextHook() ? "ok" : "FAILED (unexpected vtable)");
    if (!ScreenManagerRegistered++) RiotTimingOnFrame(ScreenManagerFrame);
    if (CurrentGame == GameTTB) Log("Interface scene placed per element: %s", HookUiScene() == 3 ? "ok" : "FAILED (unexpected vtable)");
}

/* ================================================================ Camera log (diagnostics) */
typedef void (ThisCall *SetFieldOfViewFunctionType)(void *, float);
static SetFieldOfViewFunctionType OriginalSetFieldOfView, OriginalSetSecondFieldOfView, OriginalRendererFieldOfView, OriginalRendererSecond;
static volatile LONG CameraLogLines;
#define MaximumCameraLogLines 200

/* One line per change: the same value asked again by the same caller is not logged again. */
typedef struct { void *Object; float Degrees; DWORD Caller; } LastFieldOfView;
static LastFieldOfView LastAsked[4];

void CallChain(const void *From, DWORD *Chain, int Count);
static void LogFieldOfView(int Kind, const char *Which, void *Object, float Degrees, DWORD Caller) {
    LastFieldOfView *Last = &LastAsked[Kind];
    if (Last->Object == Object && Last->Degrees == Degrees && Last->Caller == Caller) return;
    Last->Object = Object; Last->Degrees = Degrees; Last->Caller = Caller;
    if (InterlockedIncrement(&CameraLogLines) > MaximumCameraLogLines) return;
    DWORD Chain[3]; CallChain(&Degrees, Chain, 3);
    char Names[3][MAX_PATH + 32];
    for (int Index = 0; Index < 3; Index++) { if (Chain[Index]) AddressName(Chain[Index], Names[Index], sizeof Names[Index]); else strcpy(Names[Index], "-"); }
    (void)Caller;
    Log("[camera] %p %s %.3f deg (asked by %s <- %s <- %s)", Object, Which, Degrees, Names[0], Names[1], Names[2]);
}
static void ThisCall LoggedSetFieldOfView(void *Camera, float Degrees) {
    LogFieldOfView(0, "camera field of view", Camera, Degrees, (DWORD)__builtin_return_address(0));
    OriginalSetFieldOfView(Camera, Degrees);
}
/* [Widescreen] WidenCulling, after the engine has written the lens, not before: the engine works the second lens out
 * from the angle on every call, so a value written anywhere else is back to the game's on the very next set. The first
 * build of this option scaled it inside RescaleCameras, which runs only when the lens constant changes - at start-up
 * and at the ends of a cutscene - so it was overwritten within a frame or two and the option looked like it did
 * nothing. It was never really tested. Whether the second lens is what the culling reads is still a hypothesis; the
 * picture losing whole rooms at the sides while the first lens is widened and the second is not is what suggests it. */
static void WidenSecondLens(void *Camera) {
    if (!WidenCulling || !A->SecondLensOffset || OriginalLensConstant <= 0.0f || LensConstant <= 0.0f) return;
    float *Second = (float *)((char *)Camera + A->SecondLensOffset);
    if (*Second > 0.0f && *Second < 3.0e38f) *Second *= LensConstant / OriginalLensConstant;
}

static void ThisCall LoggedSetSecondFieldOfView(void *Camera, float Degrees) {
    LogFieldOfView(1, "camera second field of view", Camera, Degrees, (DWORD)__builtin_return_address(0));
    OriginalSetSecondFieldOfView(Camera, Degrees);
    WidenSecondLens(Camera);
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
    WidenSecondLens(Renderer);
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
       game is running (flag 0x80), paused or not. While a full-screen picture is up (a loading screen) it goes in the
       4:3 box like the picture, at the game's size. */
    int Anchor = AnchorNone;
    if ((*(const DWORD *)A->GameFlags & 0x80) && !PictureThisFrame && !PictureLastFrame) {
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
    if (RiotInterfaceScale() != 1.0f && Anchor != AnchorNone)
        for (int Index = 0; Index < Count; Index++) RiotScalePoint(&Moved[Index * 8], &Moved[Index * 8 + 1], Anchor, (Top + Bottom) * 0.5f);
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

/* The return addresses on the stack above a hook's arguments: who asked for this call, and who asked them. */
void CallChain(const void *From, DWORD *Chain, int Count) {
    if (!RflScanned) {
        HMODULE Rfl = RflModule();
        if (Rfl) { AddCodeRanges(Rfl); RflScanned = 1; }
    }
    int Found = 0;
    const DWORD *Slot = (const DWORD *)From, *Top = StackTop();
    for (int Index = 0; Index < 1024 && Slot + Index < Top && Found < Count; Index++)
        if (IsReturnAddress(Slot[Index])) Chain[Found++] = Slot[Index];
    while (Found < Count) Chain[Found++] = 0;
}
static void LogHudDraw(const void *Arguments, const char *What, float X, float Y, float Width, float Height) {
    DWORD Chain[4];
    CallChain(Arguments, Chain, 4);
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

/* ================================================================ What RiotAccessibility asks of the layout */
float RiotScreenWidth(void) { return ScreenWidth; }
float RiotScreenHeight(void) { return ScreenHeight; }
int RiotPlayLayout(void) { return A && PlayLayout(); }
int RiotMenuBox(float *Offset, float *Difference) {
    if (!A || !MenuBox()) return 0;
    if (Offset) *Offset = HudOffset;
    if (Difference) *Difference = AspectDifference;
    return 1;
}
void *RiotScreenManagerObject(void) { return ScreenManager; }
void RiotNeedScreenManager(void) { if (A && !ScreenManagerRegistered++) RiotTimingOnFrame(ScreenManagerFrame); }
void RiotCursorPass(int On) { CursorPass = On; }

int RiotTextNeedsAnchor(void) { return TextNeedsAnchor(); }
int RiotTextIdleNow(void) { return TextPass == TextIdle; }
int RiotTextAnchorFor(DWORD Hash, float Y) { return RememberedAnchor(Hash, Y); }
void RiotTextAnchorRemember(DWORD Hash, float Y, int Anchor) { RememberAnchor(Hash, Y, Anchor); }
void RiotTextMeasureBegin(void) { BeginMeasure(); }
int RiotTextMeasureEnd(void) { EndMeasure(); return TextAnchor; }
void RiotTextUseAnchor(int Anchor) { TextAnchor = Anchor; TextPass = TextDrawing; }
void RiotTextIdle(void) { TextPass = TextIdle; }

int RiotLogHudOn(void) { return LogHud; }
void RiotLogFont(const TextCall *Call, DWORD Caller) { LogFont(Call, Caller); }

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

    /* [Widescreen] ArchiveScale: how much of the screen height the archive and map picture takes. The game draws it
       at the size it was made for, around 640x480, whatever the resolution. 0 = the game's own size. */
    ArchiveScale = IniFloat("Widescreen", "ArchiveScale", 0.84f);
    if (ArchiveScale != 0.0f && !(ArchiveScale >= 0.1f && ArchiveScale <= 1.0f)) {
        Log("Widescreen.ArchiveScale=%g is invalid (use 0.1 to 1, or 0)", ArchiveScale);
        ArchiveScale = 0.0f;
    }

    /* [Display] RefreshRate: the rate asked for in full screen, in Hz. 0 = the one the game picks. */
    RefreshRate = IniInt("Display", "RefreshRate", 0);
    if (RefreshRate && !(RefreshRate >= 20 && RefreshRate <= 1000)) {
        Log("Display.RefreshRate=%d is invalid (use 20 to 1000, or 0)", RefreshRate);
        RefreshRate = 0;
    }
    if (RefreshRate)
        Log("Full-screen refresh rate asked as %d Hz: %s", RefreshRate,
            ApplyRefreshRate() ? "ok" : "FAILED (unexpected import slot)");

    RiotTimingOnFrame(DisplayFrame);
    if (IniInt("Widescreen", "Enable", 1)) ApplyWidescreen();
    else if (IniFloat("Widescreen", "ThirdPersonFieldOfView", 0.0f) || IniFloat("Widescreen", "FirstPersonFieldOfView", 0.0f))
        Log("Widescreen.ThirdPersonFieldOfView and FirstPersonFieldOfView need [Widescreen] Enable=1: not applied");
    ApplyHudLayout();

    if (IniInt("Debug", "LogCameras", 0)) ApplyCameraLog();
    if (IniInt("Debug", "LogHud", 0)) {
        AddCodeRanges(GetModuleHandleA(NULL)); LogHud = 1;
        if (CurrentGame == GameTTB) Log("HUD log: vertex drawing %s", HookUiScene() ? "watched" : "FAILED (unexpected vtable)");
        Log("HUD log (first %d call chains): %s", MaximumHudLogLines, OriginalHudQuad ? "on" : "off (needs [Widescreen] Hud=1 or an interface size)");
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
