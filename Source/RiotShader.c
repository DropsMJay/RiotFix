/*
 * RiotShader - the pixel shader the scene is drawn through
 *
 * Two features share one slot, which is why they share a file: black and white, which is a shader the game
 * already carries and never reaches on the PC, and a colour shader of RiotFix's own. Both work by the same
 * mechanism - the engine loads every pixel shader it knows by name and draws the scene through index 0 when
 * one field in the renderer is set - so turning one on means the other cannot be on.
 *
 * Nothing here assembles a shader and nothing here writes to the game's folder. The engine reads its shaders
 * as text and assembles them itself every run, so a shader of ours is a file RiotFix writes elsewhere and a
 * name in the engine's own table pointed at it. The two failed attempts at doing the assembling ourselves are
 * described below, where the route that replaced them is.
 *
 * Every address was confirmed by reverse engineering and the game was the final test for each of them.
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotTiming.h"
#include "RiotShader.h"
#include <string.h>

/* ================================================================ Where these settings are read from
 * [Shader] is their home. They began life in [Display], because the first of them arrived as one more display
 * switch, and an .ini written before this release still has them there - so each one is looked for in [Shader]
 * first and in [Display] only if [Shader] does not answer. Nobody's file is broken by the move, and the log
 * says once that the old section is still in use, which is the only hint anyone needs to tidy it.
 *
 * "Does not answer" has to mean absent rather than empty or zero, so the integer reader asks twice with two
 * different defaults: a key that is really there gives the same number both times. */
static int OldSectionSeen;

static void NoteOldSection(const char *Key) {
    if (OldSectionSeen) return;
    OldSectionSeen = 1;
    Log("Shader: reading %s from the old [Display] section. These settings now live in [Shader]; [Display] "
        "still works and will keep working.", Key);
}

static int ShaderInt(const char *Key, int Default) {
    if (IniInt("Shader", Key, 0) == IniInt("Shader", Key, 1)) return IniInt("Shader", Key, 0);
    if (IniInt("Display", Key, 0) != IniInt("Display", Key, 1)) return Default;
    NoteOldSection(Key);
    return IniInt("Display", Key, Default);
}

static float ShaderFloat(const char *Key, float Default) {
    const float Wanted = IniFloat("Shader", Key, -1.0e30f);
    if (Wanted != -1.0e30f) return Wanted;
    const float Old = IniFloat("Display", Key, -1.0e30f);
    if (Old == -1.0e30f) return Default;
    NoteOldSection(Key);
    return Old;
}

static void ShaderString(const char *Key, char *Out, DWORD Room) {
    GetPrivateProfileStringA("Shader", Key, "", Out, Room, IniPath);
    if (Out[0]) return;
    GetPrivateProfileStringA("Display", Key, "", Out, Room, IniPath);
    if (Out[0]) NoteOldSection(Key);
}

/* ================================================================ Black and white (PIH)
 * The engine draws the whole frame through a black-and-white pixel shader when one field is set, and that path
 * is alive in this build even though nothing in the menus reaches it. Measured:
 *
 *   the shader table at 0x58F158..0x58F1A7 holds twenty names; the last two are pixel shaders, loaded by the
 *   loop at 0x435650 into the manager at 0x5CBF20: index 0 is "xvsh\xpsBW_PC.psh" and index 1
 *   "xvsh\xpsSetAlpha00.psh". 0x435EA0 is "turn on pixel shader N" - it reads the handle at the manager plus
 *   0x1016C + N*4 and hands it to the device's SetPixelShader. The scene draw at 0x49D62A reads
 *   [renderer + 0x134], and when that is not zero it takes the branch that turns on index 0.
 *
 * So the switch is a single word in the renderer. On the console this is the Murnau Mode cheat, whose code is a
 * controller button sequence; whatever reads that sequence does not answer RiotFix's pad, so the PC has had the
 * shader loaded and unreachable since release. The field is written every frame because the engine owns it and
 * may clear it; one write at startup would not hold. */
static int BlackAndWhiteWanted;
static void KeepBlackAndWhite(void) {
    void **const Renderer = *(void ***)0x6DD650;
    if (!Renderer) return;
    *(int *)((char *)Renderer + 0x134) = BlackAndWhiteWanted ? 1 : 0;
}

static void ApplyBlackAndWhite(void) {
    BlackAndWhiteWanted = ShaderInt("BlackAndWhite", 0);
    if (!BlackAndWhiteWanted) return;
    if (CurrentGame != GamePIH) { Log("Shader.BlackAndWhite: the field has not been measured in this game yet"); return; }
    Log("Shader: the picture is drawn in black and white, through the shader the game already carries "
        "([Shader] BlackAndWhite)");
    RiotTimingOnFrame(KeepBlackAndWhite);
}

/* ================================================================ A pixel shader of our own, over the scene
 * The engine reads its own pixel shaders as text and assembles them every run - there is no cache in that path
 * (the file is opened at 0x43567B only to see that it exists and closed again at 0x43569C). The work is done by
 * D3DXAssembleShaderFromFileA at 0x4EA48A, statically linked, and the result goes to the device's
 * CreatePixelShader. The handles live in the shader manager at 0x5CBF20 + 0x1016C, index 0 being the
 * black-and-white shader and index 1 the alpha one.
 *
 * So a shader of ours needs no assembler of its own, no d3dx9 beside the game, and no bytecode written by hand:
 * it needs text. RiotFix writes that text to a file of its own in the temporary folder - no game file is
 * touched - hands the path to the engine's assembler, and puts the resulting handle in index 0. From then on
 * the field the game already uses to turn that slot on, [0x6DD650] + 0x134, turns ours on instead: over the
 * scene, under the heads-up display, through the engine's own path.
 *
 * Shader model 1.4 is enough for this and is what every card of the era supports; the linked D3DX knows 2.0 if
 * a later effect needs it (its profile table at 0x5999D0 lists ps.2.0). A source that will not assemble fails
 * here with the compiler's own message and the slot is left exactly as it was. */

static int GradingWanted;
static float GradingSaturation = 1.0f, GradingContrast = 1.0f;
static int GradingConsoleRange;

/* One number, written the way a shader source wants it, because wsprintf has no %f. */
static void WriteNumber(char *Out, float Value) {
    const int Negative = Value < 0.0f;
    if (Negative) Value = -Value;
    const int Whole = (int)Value;
    int Fraction = (int)((Value - (float)Whole) * 1000.0f + 0.5f);
    int Carry = 0;
    if (Fraction > 999) { Fraction = 0; Carry = 1; }
    wsprintfA(Out, "%s%d.%03d", Negative ? "-" : "", Whole + Carry, Fraction);
}

/* The shader, built from the ini.
 *
 * Saturation and contrast are kept apart, which is the point of working in luminance and chrominance rather
 * than on the three channels at once. Contrast on raw colour drags saturation with it and crushes the darks
 * towards whatever hue they happen to carry; here the picture is split into a brightness and what is left
 * over, each is adjusted on its own, and they are put back together:
 *
 *     Y      = dot(colour, NTSC weights)      the same grey the game's own black-and-white shader makes
 *     chroma = colour - Y
 *     out    = chroma * Saturation + (Y * Contrast + (0.5 - 0.5 * Contrast))
 *
 * so Saturation at 1 and Contrast at 1 give the picture back unchanged, exactly. The weights are the game's
 * own, read out of xvsh\xpsBW_PC.psh, so grey comes out the shade the game itself would choose.
 *
 * ConsoleRange squeezes the result into 16-235 afterwards. A console sends that range to a television and the
 * set opens it back up; a PC monitor does not, which is a good part of why the same game looks flatter here
 * than it does on a PS2 through a TV. It is a separate knob because it is a fact about the display chain, not
 * a matter of taste.
 *
 * Five arithmetic instructions, inside what ps.1.4 allows; alpha is left as the texture had it because only
 * the colour channels are written. */
static void WriteGradingSource(char *Path, size_t Room) {
    char Folder[MAX_PATH];
    if (!GetTempPathA(sizeof Folder, Folder)) Folder[0] = 0;
    wsprintfA(Path, "%sRiotFixScene.psh", Folder);
    HANDLE File = CreateFileA(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE) { Path[0] = 0; return; }

    /* Every number below is written as half of what it means, and _x2 doubles the instruction's result back.
     *
     * That is not decoration: ps.1.4 takes no constant outside -1 to 1, and a shader holding one is refused at
     * creation without a word - the engine simply keeps the shader it already had, so the setting appears to be
     * ignored rather than to fail. Writing Saturation and Contrast straight into a def is what broke 0.6.19,
     * and it was mis-diagnosed as the shader's structure until six shaders differing in one feature each were
     * loaded in play: _x2 drew, the structure drew, and only a negated constant source modifier was refused.
     * Halving each number reaches 0 to 2 with every constant legal. */
    char Saturation[16], Contrast[16], Shift[16];
    WriteNumber(Saturation, GradingSaturation * 0.5f);
    WriteNumber(Contrast, GradingContrast * 0.5f);
    WriteNumber(Shift, (0.5f - 0.5f * GradingContrast) * 0.5f);

    char Text[1400];
    int Length = wsprintfA(Text,
        "; written by RiotFix - saturation and contrast kept apart, in luminance and chrominance.\r\n"
        "; Each constant is half of what it means; the _x2 on the instruction doubles it back, which is how a\r\n"
        "; value above 1.0 is reached at all - ps.1.4 refuses a constant outside -1 to 1.\r\n"
        "ps.1.4\r\n"
        "def c0, 0.3, 0.55, 0.15, 1.0          ; the game's own NTSC weights\r\n"
        "def c1, %s, %s, %s, 1.0               ; saturation / 2\r\n"
        "def c2, %s, %s, %s, 1.0               ; contrast / 2\r\n"
        "def c3, %s, %s, %s, 0.0               ; and half of what it shifts the middle by\r\n"
        "texld r0, t0\r\n"
        "dp3 r1.rgb, r0, c0                    ; the brightness\r\n"
        "sub r2.rgb, r0, r1                    ; what is left over: the colour\r\n"
        "mul_x2 r2.rgb, r2, c1                 ; the colour, scaled\r\n"
        "mad_x2 r1.rgb, r1, c2, c3             ; brightness, with contrast around the middle\r\n"
        "add r0.rgb, r2, r1                    ; put back together\r\n",
        Saturation, Saturation, Saturation, Contrast, Contrast, Contrast, Shift, Shift, Shift);
    if (GradingConsoleRange)
        Length += wsprintfA(Text + Length,
            "def c4, 0.859, 0.859, 0.859, 1.0      ; 219/255, the range a console sends to a television\r\n"
            "def c5, 0.063, 0.063, 0.063, 0.0      ; 16/255\r\n"
            "mad r0.rgb, r0, c4, c5\r\n");
    DWORD Written = 0;
    WriteFile(File, Text, (DWORD)Length, &Written, NULL);
    CloseHandle(File);
    (void)Room;
}

/* Why the engine is asked to do the loading, and we only change a name.
 *
 * Calling the assembler ourselves failed twice, and the second failure is the instructive one: the check added
 * after the first crash did refuse the bytes, and the game died anyway, so the harm was already done inside the
 * call. ret 0x18 says six arguments, which is what was passed, yet it returns success and leaves text from the
 * file's own path where the buffer should be - the last two parameters are not what I read them to be. Rather
 * than guess that interface a third time, the engine does its own loading with its own code, and all we change
 * is which file it loads: the name pointer in its table at 0x58F1A0, the entry for xpsBW_PC.psh.
 *
 * The name has to be relative, and that is measured rather than assumed: the resolver at 0x4062C0 copies the
 * game's base path, then concatenates the name, with no case for a drive letter - an absolute path of ours
 * would become "C:\game\C:\Users\...". So RiotFix writes the shader to the temporary folder, which keeps
 * the game's own folder untouched, and gives the table a name that climbs out of the game folder with as many
 * "..\" as it has levels. Both have to be on one drive; when they are not, nothing is patched and the log
 * says so. */
static char GradingName[MAX_PATH * 2];

static int BuildRelativeName(const char *Full) {
    char Exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, Exe, sizeof Exe)) return 0;
    char *const Slash = strrchr(Exe, '\\');
    if (!Slash) return 0;
    *Slash = 0;                                     /* the folder the game runs from */
    if (((Exe[0] | 32) != (Full[0] | 32)) || Exe[1] != ':' || Full[1] != ':') {
        Log("Shader.ColourGrading: the game is on %c: and the temporary folder on %c:, so no relative name "
            "can reach it", Exe[0], Full[0]);
        return 0;
    }
    int Levels = 0;
    for (const char *At = Exe + 2; *At; At++) if (*At == '\\' && At[1]) Levels++;
    GradingName[0] = 0;
    for (int k = 0; k < Levels; k++) lstrcatA(GradingName, "..\\");
    lstrcatA(GradingName, Full + 3);                /* everything after "C:\" */
    Log("Shader: the shader is at %s, reached from the game's folder as %s", Full, GradingName);
    return 1;
}

static int PointTableAt(void) {
    unsigned char *const Entry = (unsigned char *)0x58F1A0;   /* the table's name pointer for xpsBW_PC.psh */
    const DWORD Was = *(DWORD *)Entry;
    if (Was == (DWORD)(size_t)GradingName) return 1;          /* already ours, from a reload */
    if (Was != 0x58F1C0) {
        Log("Shader.ColourGrading: the shader table does not hold what it should at 0x58F1A0 (0x%08lX), "
            "so nothing is changed", (unsigned long)Was);
        return 0;
    }
    const DWORD Ours = (DWORD)(size_t)GradingName;
    if (!WriteCode((DWORD)(size_t)Entry, &Ours, sizeof Ours)) { Log("Shader.ColourGrading: could not point the table at our shader"); return 0; }
    Log("Shader: the scene is drawn through %s, assembled by the game itself ([Shader] ColourGrading)",
        GradingName);
    return 1;
}

/* A shader of the player's own, in place of the one RiotFix writes. The engine assembles its pixel shaders from
 * text on every run, so a shader is a file to edit rather than a build to wait for - which is the whole reason
 * this option exists: trying a change should cost a text edit and a launch, not a round trip. */
/* Returns whether our shader is in the slot. It matters: the field that turns the slot on must only be written
 * when it is, or a failure here switches on the shader the game already had, which is the black-and-white one -
 * and a screen that goes monochrome when the player asked for colour reads as a far stranger bug than the
 * missing file it actually was. */
static int InstallGrading(void) {
    char Path[MAX_PATH], Named[MAX_PATH];
    ShaderString("ShaderFile", Named, sizeof Named);
    if (Named[0]) {
        /* A bare name means the game's own CustomShader folder, which is where a shader of the player's
         * belongs: the engine resolves its shader names against the game's folder, so one living under it needs
         * no climb out and no second drive to worry about - the name handed to the table is just
         * "CustomShader\whatever.psh". An absolute path still works and takes the long way round. */
        const int Absolute = Named[1] == ':' || (Named[0] == '\\' && Named[1] == '\\');
        if (Absolute) lstrcpynA(Path, Named, sizeof Path);
        else {
            char Folder[MAX_PATH];
            if (!GetModuleFileNameA(NULL, Folder, sizeof Folder)) return 0;
            char *const Slash = strrchr(Folder, '\\');
            if (!Slash) return 0;
            *Slash = 0;
            wsprintfA(Path, "%s\\CustomShader\\%s", Folder, Named);
        }
        if (GetFileAttributesA(Path) == INVALID_FILE_ATTRIBUTES) {
            Log("Shader.ColourGrading: [Shader] ShaderFile names %s, which is not there", Path);
            return 0;
        }
        Log("Shader: the scene shader is read from %s, so Saturation and Contrast are ignored", Path);
        if (!Absolute) {   /* already under the game's folder, so the engine's own resolver reaches it */
            wsprintfA(GradingName, "CustomShader\\%s", Named);
            return PointTableAt();
        }
    } else {
        WriteGradingSource(Path, sizeof Path);
        if (!Path[0]) { Log("Shader.ColourGrading: could not write the shader source to the temporary folder"); return 0; }
    }
    if (!BuildRelativeName(Path)) return 0;
    return PointTableAt();
}

/* Reloading a shader without closing the game.
 *
 * Nothing of ours assembles anything: 0x435490 is the engine's own "load every shader in my table" - thiscall,
 * no arguments, on the manager at 0x5CBF20, and it is what runs at startup. Pressing the key re-reads the ini,
 * points the table at whatever [Shader] ShaderFile now names, and calls it. So editing a .psh in a text editor
 * and tapping one key shows the change, which is the difference between tuning a shader and waiting on builds.
 *
 * It reloads all twenty shaders, not just ours, and does not free the handles it replaces - a few leaked shader
 * objects per press, which is fine for something only used while tuning and is why it is off unless asked for.
 * The call happens from the per-frame callback, at a frame boundary rather than in the middle of a draw. */
typedef void (ThisCall *LoadShadersType)(void *Self);
#define ShaderManagerAddress 0x5CBF20   /* the engine's shader manager, the object 0x435490 wants */
static int ReloadKey;

static void KeepGrading(void) {
    void **const Renderer = *(void ***)0x6DD650;
    if (Renderer) *(int *)((char *)Renderer + 0x134) = 1;

    if (!ReloadKey) return;
    static int WasDown;
    const int Down = (GetAsyncKeyState(ReloadKey) & 0x8000) != 0;
    if (Down && !WasDown) {
        if (InstallGrading()) {
            ((LoadShadersType)0x435490)((void *)ShaderManagerAddress);
            Log("Shader: shaders reloaded at your asking, now drawing through %s", GradingName);
        }
    }
    WasDown = Down;
}

static void ApplyColourGrading(void) {
    GradingWanted = ShaderInt("ColourGrading", 0);
    if (!GradingWanted) return;
    if (CurrentGame != GamePIH) { Log("Shader.ColourGrading: the addresses have not been measured in this game yet"); return; }
    if (BlackAndWhiteWanted) { Log("Shader.ColourGrading: not applied, because [Shader] BlackAndWhite has the same slot"); return; }
    GradingSaturation = ShaderFloat("Saturation", 1.0f);
    GradingContrast = ShaderFloat("Contrast", 1.0f);
    if (GradingSaturation < 0.0f) GradingSaturation = 0.0f;
    if (GradingSaturation > 2.0f) GradingSaturation = 2.0f;
    if (GradingContrast < 0.5f) GradingContrast = 0.5f;
    if (GradingContrast > 2.0f) GradingContrast = 2.0f;
    GradingConsoleRange = ShaderInt("ConsoleRange", 0);
    ReloadKey = ShaderInt("ShaderReloadKey", 0);
    if (ReloadKey) Log("Shader: virtual key %d reloads the scene shader without closing the game", ReloadKey);
    /* before the engine's own loader runs, which is why this is not per-frame */
    if (!InstallGrading()) return;    /* nothing of ours is in the slot, so the slot must stay off */
    RiotTimingOnFrame(KeepGrading);
}

void RiotShaderApply(void) {
    if (CurrentGame == GameUnknown) return;
    ApplyBlackAndWhite();
    ApplyColourGrading();   /* after it, because the two share a slot and black and white has the older claim */
}
