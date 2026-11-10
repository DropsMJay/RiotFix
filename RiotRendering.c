/*
 * RiotRendering - shadows, level of detail and draw distance
 *
 * Every address was confirmed by reverse engineering (RiotEngineNotes.md, section 9j) and every hook checks the
 * original bytes before changing anything. Addresses of 0 mean the feature is absent or not needed in that game.
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotRendering.h"
#include <string.h>
#include <math.h>
#define COBJMACROS
#include <d3d9.h>

/* ================================================================ Per-game addresses */
typedef struct {
    DWORD ShadowPush, ShadowCompare;                       /* push 0x100 / cmp ebp,0x100 (shadow texture size) */
    DWORD LodCall, LodFunction;                            /* LOD pick for characters */
    DWORD StaticLodCall, StaticLodFunction;                /* LOD pick for static objects */
    DWORD FadeSlot, FadeFunction; int FadeTakesInts;       /* shadow fade distance setter (vtable slot) */
    DWORD CullSites[6]; int CullCount; DWORD CullVariable; /* reads of the cull distance multiplier */
    DWORD OptionAt; unsigned char OptionBytes[6]; int OptionLength;   /* shadow option load in ShadowMgr::Add */
    DWORD DirectionCall, DirectionFunction, DirectionCompare, DirectionSin45;  /* shadow light direction (45 deg) */
    DWORD RenderVtable, RenderBegin, RenderBeginReturn, RenderEnd, RenderEndReturn;  /* render to texture (slots 12/14) */
    DWORD TextureVtable, TextureSize, TextureSizeReturn, TextureManager;  /* texture size (slot 4) and its manager */
} RenderingAddresses;

static const RenderingAddresses Addresses[GameCount] = {
    [GamePIH] = {
        0x492AE7, 0x4CEF36,
        0x492B0D, 0x46B220,
        0x492B23, 0x491800,
        0x58CA68, 0x4084A0, 1,
        {0x41300A, 0x491020, 0x49116B, 0x491A1D, 0x493586, 0x4989DB}, 6, 0x5C6D1C,
        0x4CEEA0, {0x8B,0x15,0x00,0x6D,0x5C,0x00}, 6,
        0x4CDFD6, 0x4CC020, 0x4CC16C, 0x595B54,
        0x593BF8, 0x49AF40, 0x49B03C, 0x49B0F0, 0x49B119,
        0x58FFE0, 0x445100, 0x44516A, 0x5C8BD0 },
    /* TTB already creates the shadow depth buffer itself (0x49FDF0); its texture cache has not been read yet. */
    [GameTTB] = {
        0x496B35, 0x4DC613,
        0x496B5B, 0x470CC0,
        0x496B71, 0x495870,
        0x5E6CE4, 0x409B40, 0,
        {0}, 0, 0,
        0x4DC590, {0xA1,0xE0,0x9E,0x63,0x00}, 5,
        0x4DB757, 0x4D9FC0, 0x4DA10C, 0x5F0DB8,
        0, 0, 0, 0, 0,
        0, 0, 0, 0 },
};
static const RenderingAddresses *A;

/* ================================================================ LOD */
static float LodInverseSquared = 1.0f;   /* 1 / scale^2 */
static volatile LONG LodCalls, StaticLodCalls;

/* Characters: 0x46B220 (distance^2, model). */
typedef int (ThisCall *LodFunctionType)(void *, float, void *);
static LodFunctionType OriginalLod;
static int ThisCall HookLod(void *Self, float Distance2, void *Model) {
    if (TraceEnabled && InterlockedIncrement(&LodCalls) <= 3)
        Log("[trace] Character LOD: object=%p model=%p dist2=%g -> %g", Self, Model, Distance2, Distance2 * LodInverseSquared);
    return OriginalLod(Self, Distance2 * LodInverseSquared, Model);
}

/* Static objects: 0x491800 (model, distance^2). */
typedef int (ThisCall *StaticLodFunctionType)(void *, void *, float);
static StaticLodFunctionType OriginalStaticLod;
static int ThisCall HookStaticLod(void *Self, void *Model, float Distance2) {
    if (TraceEnabled && InterlockedIncrement(&StaticLodCalls) <= 3)
        Log("[trace] Object LOD: object=%p model=%p dist2=%g -> %g", Self, Model, Distance2, Distance2 * LodInverseSquared);
    return OriginalStaticLod(Self, Model, Distance2 * LodInverseSquared);
}

/* ================================================================ Shadow fade distance
   PIH passes integers (start, end, two more ints); TTB passes floats (start, end). */
static float ShadowDistanceScale = 1.0f;
static volatile LONG FadeCalls;
typedef void (ThisCall *FadeIntFunctionType)(void *, int, int, int, int);
typedef void (ThisCall *FadeFloatFunctionType)(void *, float, float);
static void *OriginalFade;
static void ThisCall HookFadeInt(void *Self, int Start, int End, int A1, int A2) {
    int NewStart = (int)(Start * ShadowDistanceScale + 0.5f), NewEnd = (int)(End * ShadowDistanceScale + 0.5f);
    if (TraceEnabled && InterlockedIncrement(&FadeCalls) <= 5)
        Log("[trace] Shadow fade: start %d end %d (%d, %d) -> %d %d", Start, End, A1, A2, NewStart, NewEnd);
    ((FadeIntFunctionType)OriginalFade)(Self, NewStart, NewEnd, A1, A2);
}
static void ThisCall HookFadeFloat(void *Self, float Start, float End) {
    if (TraceEnabled && InterlockedIncrement(&FadeCalls) <= 5)
        Log("[trace] Shadow fade: start %g end %g -> %g %g", Start, End, Start * ShadowDistanceScale, End * ShadowDistanceScale);
    ((FadeFloatFunctionType)OriginalFade)(Self, Start * ShadowDistanceScale, End * ShadowDistanceScale);
}

/* ================================================================ Shadow light angle
   The game (PIH 0x4CC020 / TTB 0x4D9FC0, thiscall) stores the normalised light - caster vector at this+0xE and,
   when its height (y) is below sin 45 deg, raises it to exactly 45 deg. With MinAngle the original clamp never
   fires (its fcomp reads NeverBelow = -2) and the same adjustment is done here: y = horizontal * tan(angle). */
static float NeverBelow = -2.0f, DirectionSin = 0.70710678f, DirectionTan = 1.0f;
typedef int (ThisCall *DirectionFunctionType)(void *, void *);
static DirectionFunctionType OriginalDirection;
static volatile LONG DirectionCalls;
static int ThisCall HookDirection(void *Self, void *Caster) {
    int Result = OriginalDirection(Self, Caster);
    float V[3]; memcpy(V, (char *)Self + 0xE, sizeof V);
    if ((V[0] != 0.0f || V[1] != 0.0f || V[2] != 0.0f) && V[1] < DirectionSin) {
        float Horizontal = sqrtf(V[0] * V[0] + V[2] * V[2]);
        if (Horizontal > 0.0f) {
            float Y = Horizontal * DirectionTan, Length = sqrtf(Horizontal * Horizontal + Y * Y);
            float Out[3] = { V[0] / Length, Y / Length, V[2] / Length };
            if (TraceEnabled && InterlockedIncrement(&DirectionCalls) <= 3)
                Log("[trace] Shadow light: (%.2f %.2f %.2f) -> (%.2f %.2f %.2f)", V[0], V[1], V[2], Out[0], Out[1], Out[2]);
            memcpy((char *)Self + 0xE, Out, sizeof Out);
        } else {
            float Out[3] = { 0.0f, 1.0f, 0.0f };   /* light straight below */
            memcpy((char *)Self + 0xE, Out, sizeof Out);
        }
    }
    return Result;
}

/* ================================================================ Render to texture (PIH)
   Renderer at [0x6DD650], vtable 0x593BF8. Begin (slot 12, 0x49AF40) saves the current render target and depth
   buffer (this+0xE8, this+0xEC) and switches only the target; end (slot 14, 0x49B0F0) restores the target.
   Direct3D 9 needs a depth buffer at least as large as the target; TTB creates one of the texture size
   (0x49FDF0). The same is done here, only when the shadow is larger than the screen buffer.
   Conventions read in the exe: begin (texture, flag) with "ret 8"; end without arguments, "ret". */
typedef int (ThisCall *RenderBeginFunctionType)(void *, void *, int);
typedef int (ThisCall *RenderEndFunctionType)(void *);
static RenderBeginFunctionType OriginalRenderBegin;
static RenderEndFunctionType OriginalRenderEnd;
static IDirect3DSurface9 *ShadowDepth;
static volatile LONG RenderCalls, RenderFailures;
static int ThisCall HookRenderBegin(void *Self, void *Texture, int Flag) {
    IDirect3DDevice9 *Device = *(IDirect3DDevice9 **)((char *)Self + 0x268);
    LONG Call = InterlockedIncrement(&RenderCalls);
    int Result = OriginalRenderBegin(Self, Texture, Flag);
    if (Result != 0 || !Texture || !Device) return Result;
    IDirect3DSurface9 *ScreenDepth = *(IDirect3DSurface9 **)((char *)Self + 0xEC);
    UINT Width = *(UINT *)((char *)Texture + 0x20), Height = *(UINT *)((char *)Texture + 0x24);
    if (!ScreenDepth) return Result;
    D3DSURFACE_DESC Desc;
    if (FAILED(IDirect3DSurface9_GetDesc(ScreenDepth, &Desc))) return Result;
    int NeedsDepth = Width > Desc.Width || Height > Desc.Height;
    if (Call == 1) {
        Log("Render to texture %ux%u, screen depth %ux%u (format %d, multisample %d)%s", Width, Height, Desc.Width, Desc.Height,
            (int)Desc.Format, (int)Desc.MultiSampleType, NeedsDepth ? ": creating a depth buffer of the texture size" : "");
        if (A->TextureManager)
            Log("Texture cache: %lu of %lu KB in use", *(DWORD *)(A->TextureManager + 8) >> 10, *(DWORD *)(A->TextureManager + 4) >> 10);
    }
    if (!NeedsDepth) return Result;
    if (ShadowDepth) { IDirect3DSurface9_Release(ShadowDepth); ShadowDepth = NULL; }
    HRESULT Hr = IDirect3DDevice9_CreateDepthStencilSurface(Device, Width, Height, Desc.Format, D3DMULTISAMPLE_NONE, 0, TRUE, &ShadowDepth, NULL);
    if (FAILED(Hr)) {
        ShadowDepth = NULL;
        if (InterlockedIncrement(&RenderFailures) <= 3) Log("Depth %ux%u: CreateDepthStencilSurface failed (0x%08lx)", Width, Height, (unsigned long)Hr);
        return Result;
    }
    IDirect3DDevice9_SetDepthStencilSurface(Device, ShadowDepth);
    DWORD ClearFlags = D3DCLEAR_ZBUFFER;
    if (Desc.Format == D3DFMT_D24S8 || Desc.Format == D3DFMT_D24X4S4 || Desc.Format == D3DFMT_D15S1 || Desc.Format == D3DFMT_D24FS8)
        ClearFlags |= D3DCLEAR_STENCIL;
    IDirect3DDevice9_Clear(Device, 0, NULL, ClearFlags, 0, 1.0f, 0);
    return Result;
}
static int ThisCall HookRenderEnd(void *Self) {
    if (ShadowDepth) {
        IDirect3DDevice9 *Device = *(IDirect3DDevice9 **)((char *)Self + 0x268);
        IDirect3DSurface9 *ScreenDepth = *(IDirect3DSurface9 **)((char *)Self + 0xEC);
        if (Device) IDirect3DDevice9_SetDepthStencilSurface(Device, ScreenDepth);
        IDirect3DSurface9_Release(ShadowDepth); ShadowDepth = NULL;
    }
    return OriginalRenderEnd(Self);
}

/* ================================================================ Shadow memory in the texture cache (PIH)
   Every texture of the texture manager (0x5C8BD0) adds to the amount in use the size that slot 4 of its vtable
   (0x445100) reports (pitch * height + 0x6C; callback 0x463650). The cache has a limit (+4; 10.05 MB in the
   levels tested) and, above it, the loader (0x463920) recycles textures already loaded. A 1024 shadow counts
   4 MB (256 counts 256 KB): with two of them the cache fills and the constant recycling exposes the crashes at
   0x449330 and 0x4488A1. Shadows are the only textures created with flags 0x480001 (0x4CB600); for them this hook
   reports the size they would have at 256, so the cache behaves as in the original game. */
typedef DWORD (ThisCall *TextureSizeFunctionType)(void *);
static TextureSizeFunctionType OriginalTextureSize;
static DWORD ThisCall HookTextureSize(void *Self) {
    DWORD Result = OriginalTextureSize(Self);
    char *Texture = (char *)Self;
    DWORD Flags = *(DWORD *)(Texture + 0x1C), Width = *(DWORD *)(Texture + 0x20), Height = *(DWORD *)(Texture + 0x24);
    DWORD Pitch = *(DWORD *)(Texture + 0x30);
    if ((Flags & 0x480000) == 0x480000 && *(DWORD *)(Texture + 0x44) == 0 && *(DWORD *)(Texture + 0x64) != 0 &&
        Width > 256 && Height > 256 && Pitch) {
        DWORD RealBytes = Pitch * Height, Bytes256 = (Pitch / Width) * 256 * 256;
        if (Result >= RealBytes) Result = Result - RealBytes + Bytes256;
    }
    return Result;
}

/* ================================================================ Draw distance */
static float CullValue = 1.0f;

/* ================================================================ Apply settings */
static void ApplyShadowResolution(void) {
    int Resolution = IniInt("Shadows", "Resolution", 256);
    if (Resolution == 256) return;
    if (Resolution < 64 || Resolution > 2048 || (Resolution & (Resolution - 1))) {
        Log("Shadows.Resolution=%d is invalid (use 128..2048, a power of 2)", Resolution);
        return;
    }
    unsigned char Push[5] = {0x68}; memcpy(Push + 1, &Resolution, 4);
    unsigned char Compare[6] = {0x81, 0xFD}; memcpy(Compare + 2, &Resolution, 4);
    int Ok = BytesAre(A->ShadowPush, (const unsigned char *)"\x68\x00\x01\x00\x00", 5) &&
             BytesAre(A->ShadowCompare, (const unsigned char *)"\x81\xFD\x00\x01\x00\x00", 6) &&
             WriteCode(A->ShadowPush, Push, 5) && WriteCode(A->ShadowCompare, Compare, 6);
    Log("Shadow %dx%d: %s", Resolution, Resolution, Ok ? "ok" : "FAILED (unexpected bytes)");
    if (!Ok) return;

    if (A->RenderVtable) {
        DWORD *Vtable = (DWORD *)A->RenderVtable;
        if (Vtable[12] == A->RenderBegin && Vtable[14] == A->RenderEnd &&
            BytesAre(A->RenderBeginReturn, (const unsigned char *)"\xC2\x08\x00", 3) && BytesAre(A->RenderEndReturn, (const unsigned char *)"\xC3", 1)) {
            OriginalRenderBegin = (RenderBeginFunctionType)A->RenderBegin; OriginalRenderEnd = (RenderEndFunctionType)A->RenderEnd;
            int Hooked = ReplaceVtableSlot(A->RenderVtable, 12, A->RenderBegin, (DWORD)HookRenderBegin) &&
                         ReplaceVtableSlot(A->RenderVtable, 14, A->RenderEnd, (DWORD)HookRenderEnd);
            Log("Depth buffer for shadows larger than the screen: %s", Hooked ? "ok" : "FAILED");
        } else Log("Shadow depth buffer: unexpected vtable (%08lx %08lx)", Vtable[12], Vtable[14]);
    }
    if (A->TextureVtable) {
        DWORD *Vtable = (DWORD *)A->TextureVtable;
        if (Vtable[4] == A->TextureSize && BytesAre(A->TextureSizeReturn, (const unsigned char *)"\xC3", 1)) {
            OriginalTextureSize = (TextureSizeFunctionType)A->TextureSize;
            Log("Shadows counted as 256 in the texture cache: %s",
                ReplaceVtableSlot(A->TextureVtable, 4, A->TextureSize, (DWORD)HookTextureSize) ? "ok" : "FAILED");
        } else Log("Texture cache: unexpected vtable (%08lx)", Vtable[4]);
    }
}

static void ApplyShadowDistance(void) {
    ShadowDistanceScale = IniFloat("Shadows", "DistanceScale", 1.0f);
    if (ShadowDistanceScale == 1.0f || ShadowDistanceScale <= 0.0f) return;
    DWORD Current = *(DWORD *)A->FadeSlot;
    if (Current != A->FadeFunction) { Log("Shadow distance: unexpected vtable (%08lx)", Current); return; }
    OriginalFade = (void *)Current;
    DWORD Hook = A->FadeTakesInts ? (DWORD)HookFadeInt : (DWORD)HookFadeFloat;
    Log("Shadow distance x%.2f: %s", ShadowDistanceScale, WriteCode(A->FadeSlot, &Hook, 4) ? "ok" : "FAILED");
}

/* Who casts shadows: 1 = player only, 2 = characters, 3 = everything (the game reads the menu option in
   ShadowMgr::Add; option 0 in the menu still turns shadows off). */
static void ApplyShadowCasters(void) {
    int Casters = IniInt("Shadows", "Casters", 0);
    if (!Casters) return;
    if (Casters < 1 || Casters > 3) { Log("Shadows.Casters=%d is invalid (use 1, 2 or 3)", Casters); return; }
    unsigned char Code[6];
    if (A->OptionLength == 6) { Code[0] = 0xBA; memcpy(Code + 1, &Casters, 4); Code[5] = 0x90; }   /* mov edx, imm32; nop */
    else { Code[0] = 0xB8; memcpy(Code + 1, &Casters, 4); }                                         /* mov eax, imm32 */
    int Ok = BytesAre(A->OptionAt, A->OptionBytes, A->OptionLength) && WriteCode(A->OptionAt, Code, A->OptionLength);
    Log("Shadow casters %s: %s", Casters == 1 ? "player only" : Casters == 2 ? "characters" : "everything",
        Ok ? "ok" : "FAILED (unexpected bytes)");
}

static void ApplyShadowAngle(void) {
    float Angle = IniFloat("Shadows", "MinAngle", 45.0f);
    if (Angle == 45.0f) return;
    if (Angle < 5.0f || Angle > 85.0f) { Log("Shadows.MinAngle=%g is invalid (use 5 to 85)", Angle); return; }
    DirectionSin = sinf(Angle * 3.14159265f / 180.0f); DirectionTan = tanf(Angle * 3.14159265f / 180.0f);
    unsigned char Fcomp[6] = {0xD8, 0x1D}; memcpy(Fcomp + 2, &A->DirectionSin45, 4);
    OriginalDirection = (DirectionFunctionType)A->DirectionFunction;
    int Ok = BytesAre(A->DirectionCompare, Fcomp, 6) && RedirectCall(A->DirectionCall, A->DirectionFunction, (DWORD)HookDirection);
    if (Ok) { DWORD Address = (DWORD)&NeverBelow; Ok = WriteCode(A->DirectionCompare + 2, &Address, 4); }
    Log("Shadow light minimum angle %.0f deg: %s", Angle, Ok ? "ok" : "FAILED (unexpected bytes)");
}

static void ApplyLod(void) {
    float Scale = IniFloat("LOD", "DistanceScale", 1.0f);
    if (Scale == 1.0f) return;
    LodInverseSquared = Scale <= 0.0f ? 0.0f : 1.0f / (Scale * Scale);
    OriginalLod = (LodFunctionType)A->LodFunction; OriginalStaticLod = (StaticLodFunctionType)A->StaticLodFunction;
    const char *Note = Scale <= 0 ? " (always LOD 0)" : "";
    Log("Character LOD x%.2f%s: %s", Scale, Note, RedirectCall(A->LodCall, A->LodFunction, (DWORD)HookLod) ? "ok" : "FAILED");
    Log("Object LOD x%.2f%s: %s", Scale, Note, RedirectCall(A->StaticLodCall, A->StaticLodFunction, (DWORD)HookStaticLod) ? "ok" : "FAILED");
}

/* Draw distance multiplier (PIH only). */
static void ApplyCullDistance(void) {
    float Multiplier = IniFloat("Draw", "CullDistanceMultiplier", 0.0f);
    if (Multiplier <= 0.0f) return;
    if (!A->CullCount) { Log("Draw.CullDistanceMultiplier: not present in this engine version"); return; }
    CullValue = Multiplier; int Patched = 0;
    unsigned char Fld[6] = {0xD9, 0x05}; memcpy(Fld + 2, &A->CullVariable, 4);
    for (int Index = 0; Index < A->CullCount; Index++)
        if (BytesAre(A->CullSites[Index], Fld, 6)) { DWORD Address = (DWORD)&CullValue; Patched += WriteCode(A->CullSites[Index] + 2, &Address, 4); }
    Log("Draw distance x%.2f: %d/%d sites", Multiplier, Patched, A->CullCount);
}

void RiotRenderingApply(void) {
    if (CurrentGame == GameUnknown) return;
    A = &Addresses[CurrentGame];
    ApplyShadowResolution();
    ApplyShadowDistance();
    ApplyShadowCasters();
    ApplyShadowAngle();
    ApplyLod();
    ApplyCullDistance();
}
