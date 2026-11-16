/*
 * RiotAccessibility - interface size and text outline
 *
 * Interface size ([Accessibility] InterfaceScale): during play every HUD element (and its text) grows around a point
 * of the screen chosen from where the element is: horizontally the side it is anchored to (left edge, centre, right
 * edge), vertically the third of the screen its centre is in (top edge, middle, bottom edge). The crosshair stays in
 * the middle and the corner elements grow away from their corners. Menus keep their size. RiotDisplay applies this to
 * each quad (it owns the layout); this module keeps the size and the arithmetic.
 *
 * Mouse cursor: the screen manager's last method (PIH Suffering.rfl+0x4E0C0, TTB SufferingTTB.rfl+0x12AB80) draws the
 * cursor texture with Texture::Render (texture vtable slot 32, PIH 0x44A780 / TTB 0x450780; TTB has a 12th argument)
 * at x = (int)(ScreenWidth * CursorX * 0.01) - HotspotX (y likewise), scale 1. The cursor position (manager +0xCC /
 * +0xD0 in PIH, +0xD0 / +0xD4 in TTB) goes from 0 to 100. Render filters the texture linearly whenever the scale is
 * not 1 (with scale 1 it samples points). The cursor texture is drawn for a 480-line screen, so its size is the
 * screen height / 480 times InterfaceScale; it grows around its hotspot, and in the 4:3 menu box the point moves with
 * the box and the cursor keeps its proportions (RiotDisplay leaves the cursor's quad alone).
 *
 * Text outline ([Accessibility] Outline): the game draws a shadow, the same string in black 2 pixels off, through one
 * call (PIH Suffering.rfl+0x6AA61, TTB SufferingTTB.rfl+0x12E56D, colours alpha << 24) followed by the text itself
 * (PIH +0x6AADF, TTB +0x12E5FC). The shadow call is held back; when the text comes, the string is drawn eight times
 * around it in the outline colour (with the shadow's alpha), then the shadow if kept, then the text.
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotAccessibility.h"
#include "RiotDisplay.h"
#include "RiotInput.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    DWORD FontVtable, DrawText;              /* slot 5: draws a string, one quad per letter */
    DWORD ShadowTextCall, MainTextCall;      /* .rfl offsets of the return addresses of the shadow and the text */
    DWORD TextureVtable, TextureRender;      /* slot 32: Texture::Render (x, y, u, v, w, h, clip, flags, colours, sx, sy) */
    DWORD CursorRenderCall;                  /* .rfl offset of the return address of the cursor's Render call */
    DWORD CursorPosition;                    /* screen manager field: cursor x and y, 0 to 100 (floats) */
    DWORD CursorScreenSize;                  /* .rfl offset of the screen width and height (floats) the cursor uses */
} AccessibilityAddresses;

static const AccessibilityAddresses Addresses[GameCount] = {
    [GamePIH] = { 0x592358, 0x48A200, 0x6AA64, 0x6AAE2, 0x58FFE0, 0x44A780, 0x4E1DB, 0xCC, 0x17A7B0 },
    [GameTTB] = { 0x5ED258, 0x490400, 0x12E570, 0x12E5FF, 0x5EAF48, 0x450780, 0x12AC6C, 0xD0, 0x1B0C78 },
};
static const AccessibilityAddresses *A;

/* ================================================================ Interface size */
static float InterfaceScale = 1.0f;

float RiotInterfaceScale(void) { return InterfaceScale; }

void RiotScalePoint(float *X, float *Y, int Anchor, float CentreY) {
    const float Width = RiotScreenWidth(), Height = RiotScreenHeight();
    const float PivotX = Anchor == AnchorLeft ? 0.0f : Anchor == AnchorRight ? Width : Width * 0.5f;
    const float PivotY = CentreY < Height / 3.0f ? 0.0f : CentreY > Height * 2.0f / 3.0f ? Height : Height * 0.5f;
    *X = PivotX + (*X - PivotX) * InterfaceScale;
    *Y = PivotY + (*Y - PivotY) * InterfaceScale;
}

/* ================================================================ Text */
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

static int TextOutline, TextOutlineShadow, TextOutlineAll;
static float TextOutlineSize = 2.0f;
static DWORD TextOutlineColour;
static TextCall PendingShadow; static DWORD PendingShadowColours[4]; static int HasPendingShadow;
/* [Accessibility] OutlineSkipFonts: resource ids of fonts drawn without an outline. A font is a resource like the
   textures; its id is at +4 (the resource release, PIH 0x466660, passes [this + 4] to the manager; the TTB text leaf
   0x43F7A0 gets the font from the manager by id). LogHud logs the id of every font with its first string. Font
   00050003 (Interface package, 5_0003) is the menus' hand-drawn font in both games; 00050002 is the HUD's. */
static DWORD SkipFonts[16]; static int SkipFontCount;
static int FontSkipped(const TextCall *Call) {
    if (!SkipFontCount || !Call->Arguments[0]) return 0;
    DWORD Id = ((const DWORD *)Call->Arguments[0])[1];
    for (int Index = 0; Index < SkipFontCount; Index++) if (SkipFonts[Index] == Id) return 1;
    return 0;
}
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
        float Size = TextOutlineSize * RiotScreenHeight() / 1080.0f, X = TextFloat(Call, 2), Y = TextFloat(Call, 3);
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
static DWORD TextHash(const wchar_t *Text) {
    DWORD Hash = 2166136261u;
    for (int Index = 0; Text && Text[Index] && Index < 4096; Index++) Hash = (Hash ^ Text[Index]) * 16777619u;
    return Hash;
}
/* Draws one string (and its outline), with one anchor for all its letters during play (RiotDisplay keeps the passes). */
static DWORD DrawString(TextCall *Call, const TextCall *Shadow, int Outline) {
    if (!RiotTextNeedsAnchor()) return DrawPrepared(Call, Shadow, Outline);
    const wchar_t *Text = (const wchar_t *)Call->Arguments[1];
    float Y = TextFloat(Call, 3);
    DWORD Hash = TextHash(Text);
    int Anchor = RiotTextAnchorFor(Hash, Y);
    if (Anchor == AnchorNone) {
        RiotTextMeasureBegin();
        CallDrawText(Call);
        RiotTextAnchorRemember(Hash, Y, RiotTextMeasureEnd());
    } else RiotTextUseAnchor(Anchor);
    DWORD Result = DrawPrepared(Call, Shadow, Outline);
    RiotTextIdle();
    return Result;
}
static DWORD HandleText(TextCall *Call, DWORD Caller) {
    if (RiotLogHudOn() && RiotTextIdleNow()) RiotLogFont(Call, Caller);
    if (TextOutline && RiotTextIdleNow() && FontSkipped(Call)) {
        /* A font without an outline: a held shadow goes first, then this string as the game draws it. */
        if (HasPendingShadow) {
            HasPendingShadow = 0;
            if (RflOffset(Caller) == A->MainTextCall && Call->Arguments[1] == PendingShadow.Arguments[1]) return DrawString(Call, &PendingShadow, 0);
            DrawString(&PendingShadow, NULL, 0);
        }
        return DrawString(Call, NULL, 0);
    }
    if (TextOutline && RiotTextIdleNow()) {
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
int RiotTextHook(void) {
    if (TextHooked) return 1;
    if (CurrentGame == GameUnknown) return 0;
    A = &Addresses[CurrentGame];
    if (CurrentGame == GameTTB) { OriginalDrawText10 = (DrawText10FunctionType)A->DrawText; TextHooked = ReplaceVtableSlot(A->FontVtable, 5, A->DrawText, (DWORD)DrawText10); }
    else { OriginalDrawText9 = (DrawText9FunctionType)A->DrawText; TextHooked = ReplaceVtableSlot(A->FontVtable, 5, A->DrawText, (DWORD)DrawText9); }
    return TextHooked;
}

/* ================================================================ Mouse cursor */
typedef int (ThisCall *Render11FunctionType)(void *, int, int, int, int, int, int, void *, DWORD, void *, float, float);
typedef int (ThisCall *Render12FunctionType)(void *, int, int, int, int, int, int, void *, DWORD, void *, float, float, int);
static Render11FunctionType OriginalRender11;
static Render12FunctionType OriginalRender12;

static void PlaceCursor(int *X, int *Y, float *Scale) {
    void *Manager = RiotScreenManagerObject();
    HMODULE Rfl = RflModule();
    if (!Rfl || !Manager) return;
    const float *Size = (const float *)((DWORD)Rfl + A->CursorScreenSize);
    const float *Position = (const float *)((char *)Manager + A->CursorPosition);
    /* The game's own point (same operations, truncated like its _ftol) gives the hotspot back. */
    const int PointX = (int)((long double)Size[0] * Position[0] * 0.01f), PointY = (int)((long double)Size[1] * Position[1] * 0.01f);
    const int HotX = PointX - *X, HotY = PointY - *Y;
    float NewScale = Size[1] / 480.0f * InterfaceScale;   /* the cursor texture is drawn for a 480-line screen */
    if (!(NewScale > 0.0f)) NewScale = 1.0f;
    float Offset, Difference, X2 = (float)PointX;
    if (RiotMenuBox(&Offset, &Difference) && !RiotPlayLayout()) X2 = X2 / Difference + Offset;
    *X = (int)lroundf(X2 - HotX * NewScale);
    *Y = (int)lroundf((float)PointY - HotY * NewScale);
    *Scale = NewScale;
}
static int ThisCall Render11(void *Self, int X, int Y, int U, int V, int W, int H, void *Clip, DWORD Flags, void *Colours, float Sx, float Sy) {
    if (RflOffset((DWORD)__builtin_return_address(0)) != A->CursorRenderCall) return OriginalRender11(Self, X, Y, U, V, W, H, Clip, Flags, Colours, Sx, Sy);
    if (RiotInputHidesCursor()) return 0;   /* the caller does not read the result (PIH .rfl+0x4E1DB, TTB +0x12AC6C) */
    PlaceCursor(&X, &Y, &Sx);
    RiotCursorPass(1);
    int Result = OriginalRender11(Self, X, Y, U, V, W, H, Clip, Flags, Colours, Sx, Sy);
    RiotCursorPass(0);
    return Result;
}
static int ThisCall Render12(void *Self, int X, int Y, int U, int V, int W, int H, void *Clip, DWORD Flags, void *Colours, float Sx, float Sy,
                             int Last) {
    if (RflOffset((DWORD)__builtin_return_address(0)) != A->CursorRenderCall)
        return OriginalRender12(Self, X, Y, U, V, W, H, Clip, Flags, Colours, Sx, Sy, Last);
    if (RiotInputHidesCursor()) return 0;
    PlaceCursor(&X, &Y, &Sx);
    RiotCursorPass(1);
    int Result = OriginalRender12(Self, X, Y, U, V, W, H, Clip, Flags, Colours, Sx, Sy, Last);
    RiotCursorPass(0);
    return Result;
}
static int HookCursor(void) {
    if (CurrentGame == GameTTB) { OriginalRender12 = (Render12FunctionType)A->TextureRender; return ReplaceVtableSlot(A->TextureVtable, 32, A->TextureRender, (DWORD)Render12); }
    OriginalRender11 = (Render11FunctionType)A->TextureRender;
    return ReplaceVtableSlot(A->TextureVtable, 32, A->TextureRender, (DWORD)Render11);
}

/* ================================================================ Apply */
void RiotAccessibilityRead(void) {
    if (CurrentGame == GameUnknown) return;
    A = &Addresses[CurrentGame];
    InterfaceScale = IniFloat("Accessibility", "InterfaceScale", 1.0f);
    if (!(InterfaceScale >= 0.5f && InterfaceScale <= 3.0f)) {
        Log("Accessibility.InterfaceScale=%g is invalid (use 0.5 to 3)", InterfaceScale);
        InterfaceScale = 1.0f;
    }
    if (InterfaceScale != 1.0f) Log("Interface size x%g (HUD and mouse cursor)", InterfaceScale);
}

void RiotAccessibilityApply(void) {
    if (CurrentGame == GameUnknown) return;
    A = &Addresses[CurrentGame];

    if (IniInt("Accessibility", "Outline", 1)) {
        TextOutlineSize = IniFloat("Accessibility", "OutlineSize", 2.0f);
        if (!(TextOutlineSize > 0.0f && TextOutlineSize <= 8.0f)) { Log("Accessibility.OutlineSize=%g is invalid (use 0.5 to 8)", TextOutlineSize); TextOutlineSize = 2.0f; }
        char Colour[32]; GetPrivateProfileStringA("Accessibility", "OutlineColor", "000000", Colour, sizeof Colour, IniPath);
        TextOutlineColour = strtoul(Colour, NULL, 16) & 0xFFFFFF;   /* RRGGBB */
        TextOutlineShadow = IniInt("Accessibility", "OutlineShadow", 0);
        TextOutlineAll = IniInt("Accessibility", "OutlineAll", 1);
        char Fonts[256]; GetPrivateProfileStringA("Accessibility", "OutlineSkipFonts", "00050003", Fonts, sizeof Fonts, IniPath);
        for (char *Next = Fonts; *Next && SkipFontCount < 16;) {
            char *End; DWORD Id = strtoul(Next, &End, 16);
            if (End == Next) { Next++; continue; }
            SkipFonts[SkipFontCount++] = Id; Next = End;
        }
        if (SkipFontCount) Log("Text outline skips %d font(s) ([Accessibility] OutlineSkipFonts)", SkipFontCount);
        TextOutline = RiotTextHook();
        Log("Text outline (%g px at 1080p, colour %06lX, shadow %s, %s): %s", TextOutlineSize, (unsigned long)TextOutlineColour,
            TextOutlineShadow ? "kept" : "replaced", TextOutlineAll ? "all text" : "text with a shadow only", TextOutline ? "ok" : "FAILED (unexpected vtable)");
    }

    /* The cursor is always placed by RiotFix: the game draws its texture at its 480-line size whatever the screen is. */
    RiotNeedScreenManager();
    Log("Mouse cursor size (screen height / 480 x %g): %s", InterfaceScale, HookCursor() ? "ok" : "FAILED (unexpected vtable)");
}
