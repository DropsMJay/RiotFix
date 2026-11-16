/* RiotSdl - loads SDL3 and reports what it finds. Always on; [Sdl] Enable=0 still turns it off, but is no longer
 * listed in RiotFix.ini.
 *
 * SDL3 is the ground for two things planned: the audio path, which would replace a DirectSound whose effect
 * classes Windows no longer registers and whose voice count is decided by hardware that has not existed since
 * Vista, and controller support, which the PC port does not have at all - both exes import only
 * DirectInput8Create, and there is not one joystick or analog-axis string anywhere in either binary.
 *
 * This file is the shared ground floor of both: it finds SDL3.dll, opens it, and says what came out. Nothing in
 * the game is touched. See Docs/Sdl.md for where it goes from here.
 *
 * Two decisions are deliberate.
 *
 * SDL3 is loaded at run time and by its full path, built from the folder RiotFix itself was loaded from, never
 * as a bare LoadLibrary("SDL3.dll"): the Windows search order would otherwise find another mod's copy sitting in
 * the same game folder, of another version, and the result would be nobody's fault and impossible to read in a
 * log. Loading by hand also means a missing SDL3.dll is a line in the log and not a game that refuses to start.
 *
 * SDL's DirectInput joystick backend is turned off before SDL starts. RiotFix is usually loaded AS dinput8.dll,
 * so a DirectInput call from inside SDL would come back into this very module while it is still initialising.
 * Raw input and HIDAPI cover every controller worth having, DualSense included. */

#include "RiotSdl.h"
#include "RiotTiming.h"

/* The handful of SDL functions this milestone needs. SDL3 returns bool from the calls that used to return 0 on
   success in SDL2, so the result of SDL_Init reads the other way round from what SDL2 habits expect. */
typedef int SdlBool;
typedef unsigned int SdlInitFlags;
typedef unsigned int SdlDeviceId;

#define SdlInitAudio   0x00000010u
#define SdlInitGamepad 0x00002000u

typedef SdlBool     (__cdecl *SdlInitFunctionType)(SdlInitFlags);
typedef void        (__cdecl *SdlQuitFunctionType)(void);
typedef const char *(__cdecl *SdlGetErrorFunctionType)(void);
typedef int         (__cdecl *SdlGetVersionFunctionType)(void);
typedef void        (__cdecl *SdlFreeFunctionType)(void *);
typedef SdlBool     (__cdecl *SdlSetHintFunctionType)(const char *, const char *);
typedef SdlDeviceId *(__cdecl *SdlGetAudioPlaybackDevicesFunctionType)(int *);
typedef const char *(__cdecl *SdlGetAudioDeviceNameFunctionType)(SdlDeviceId);
typedef SdlDeviceId *(__cdecl *SdlGetGamepadsFunctionType)(int *);
/* Signatures from SDL3's SDL_gamepad.h: SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID), void SDL_UpdateGamepads(void),
 * Sint16 SDL_GetGamepadAxis(SDL_Gamepad *, SDL_GamepadAxis), bool SDL_GetGamepadButton(SDL_Gamepad *,
 * SDL_GamepadButton). All SDLCALL, which is cdecl on Windows. */
typedef void *(__cdecl *SdlOpenGamepadFunctionType)(SdlDeviceId);
typedef void (__cdecl *SdlCloseGamepadFunctionType)(void *);
typedef void (__cdecl *SdlUpdateGamepadsFunctionType)(void);
typedef short (__cdecl *SdlGetGamepadAxisFunctionType)(void *, int);
typedef unsigned char (__cdecl *SdlGetGamepadButtonFunctionType)(void *, int);
typedef const char *(__cdecl *SdlGetGamepadNameForIdFunctionType)(SdlDeviceId);
/* bool SDL_RumbleGamepad(SDL_Gamepad *, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble,
 * Uint32 duration_ms). */
typedef SdlBool (__cdecl *SdlRumbleGamepadFunctionType)(void *, unsigned short, unsigned short, unsigned);
/* int SDL_GetGamepadButtonLabel(SDL_Gamepad *, SDL_GamepadButton): what the button is called on the pad that is
 * actually plugged in, so a PlayStation pad is asked about by its shapes and an Xbox pad by its letters. The
 * answers are 0 unknown, 1 A, 2 B, 3 X, 4 Y, 5 cross, 6 circle, 7 square, 8 triangle. */
typedef int (__cdecl *SdlGetGamepadButtonLabelFunctionType)(void *, int);

static struct {
    HMODULE Module;
    SdlInitFunctionType Init;
    SdlQuitFunctionType Quit;
    SdlGetErrorFunctionType GetError;
    SdlGetVersionFunctionType GetVersion;
    SdlFreeFunctionType Free;
    SdlSetHintFunctionType SetHint;
    SdlGetAudioPlaybackDevicesFunctionType GetAudioPlaybackDevices;
    SdlGetAudioDeviceNameFunctionType GetAudioDeviceName;
    SdlGetGamepadsFunctionType GetGamepads;
    SdlGetGamepadNameForIdFunctionType GetGamepadNameForId;
    SdlOpenGamepadFunctionType OpenGamepad;
    SdlCloseGamepadFunctionType CloseGamepad;
    SdlUpdateGamepadsFunctionType UpdateGamepads;
    SdlGetGamepadAxisFunctionType GetGamepadAxis;
    SdlGetGamepadButtonFunctionType GetGamepadButton;
    SdlRumbleGamepadFunctionType RumbleGamepad;   /* wanted, not required: see FindOptionalFunctions */
    SdlGetGamepadButtonLabelFunctionType GetGamepadButtonLabel;   /* wanted, not required */
} Sdl;

/* Every function this milestone uses, so a half-loaded SDL is refused instead of crashing on the first call. */
static int FindFunctions(void) {
    static const struct { const char *Name; size_t Place; } Wanted[] = {
        { "SDL_Init",                     offsetof(typeof(Sdl), Init) },
        { "SDL_Quit",                     offsetof(typeof(Sdl), Quit) },
        { "SDL_GetError",                 offsetof(typeof(Sdl), GetError) },
        { "SDL_GetVersion",               offsetof(typeof(Sdl), GetVersion) },
        { "SDL_free",                     offsetof(typeof(Sdl), Free) },
        { "SDL_SetHint",                  offsetof(typeof(Sdl), SetHint) },
        { "SDL_GetAudioPlaybackDevices",  offsetof(typeof(Sdl), GetAudioPlaybackDevices) },
        { "SDL_GetAudioDeviceName",       offsetof(typeof(Sdl), GetAudioDeviceName) },
        { "SDL_GetGamepads",              offsetof(typeof(Sdl), GetGamepads) },
        { "SDL_GetGamepadNameForID",      offsetof(typeof(Sdl), GetGamepadNameForId) },
        { "SDL_OpenGamepad",              offsetof(typeof(Sdl), OpenGamepad) },
        { "SDL_CloseGamepad",             offsetof(typeof(Sdl), CloseGamepad) },
        { "SDL_UpdateGamepads",           offsetof(typeof(Sdl), UpdateGamepads) },
        { "SDL_GetGamepadAxis",           offsetof(typeof(Sdl), GetGamepadAxis) },
        { "SDL_GetGamepadButton",         offsetof(typeof(Sdl), GetGamepadButton) },
    };
    for (size_t Index = 0; Index < sizeof Wanted / sizeof *Wanted; Index++) {
        FARPROC Found = GetProcAddress(Sdl.Module, Wanted[Index].Name);
        if (!Found) {
            Log("SDL: %s is missing from SDL3.dll - not used", Wanted[Index].Name);
            return 0;
        }
        *(FARPROC *)((char *)&Sdl + Wanted[Index].Place) = Found;
    }
    return 1;
}

/* What RiotFix can do without. A missing one costs its own feature and nothing else, so it must not refuse the whole
 * of SDL the way a missing function above does: audio would go with it. */
static void FindOptionalFunctions(void) {
    Sdl.RumbleGamepad = (SdlRumbleGamepadFunctionType)(void *)GetProcAddress(Sdl.Module, "SDL_RumbleGamepad");
    if (!Sdl.RumbleGamepad) Log("SDL: SDL_RumbleGamepad is missing from SDL3.dll - no rumble");
    Sdl.GetGamepadButtonLabel =
        (SdlGetGamepadButtonLabelFunctionType)(void *)GetProcAddress(Sdl.Module, "SDL_GetGamepadButtonLabel");
    if (!Sdl.GetGamepadButtonLabel)
        Log("SDL: SDL_GetGamepadButtonLabel is missing from SDL3.dll - the face buttons keep their letters");
}

/* SDL3.dll beside RiotFix itself, by full path. */
static HMODULE OpenLibrary(char *Path, size_t Room) {
    HMODULE Self = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(void *)OpenLibrary, &Self))
        return NULL;
    DWORD Length = GetModuleFileNameA(Self, Path, (DWORD)Room);
    if (!Length || Length >= Room) return NULL;
    char *Slash = strrchr(Path, '\\');
    if (!Slash) return NULL;
    Slash[1] = 0;
    if (strlen(Path) + sizeof "SDL3.dll" > Room) return NULL;
    strcat(Path, "SDL3.dll");
    return LoadLibraryExA(Path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}

static void ReportAudioDevices(void) {
    int Count = 0;
    SdlDeviceId *Devices = Sdl.GetAudioPlaybackDevices(&Count);
    if (!Devices) { Log("SDL: no audio device was listed (%s)", Sdl.GetError()); return; }
    Log("SDL: %d audio playback device(s)", Count);
    for (int Index = 0; Index < Count && Index < 8; Index++) {
        const char *Name = Sdl.GetAudioDeviceName(Devices[Index]);
        Log("SDL:   audio %d: %s", Index, Name ? Name : "(unnamed)");
    }
    Sdl.Free(Devices);
}

static void *OpenPad;
static short PadAxes[6];
static unsigned char PadButtons[15];

static void StartSdl(void);
static void ReportGamepads(void) {
    int Count = 0;
    SdlDeviceId *Pads = Sdl.GetGamepads(&Count);
    if (!Pads) { Log("SDL: no controller was listed (%s)", Sdl.GetError()); return; }
    Log("SDL: %d controller(s) connected", Count);
    for (int Index = 0; Index < Count && Index < 8; Index++) {
        const char *Name = Sdl.GetGamepadNameForId(Pads[Index]);
        Log("SDL:   controller %d: %s", Index, Name ? Name : "(unnamed)");
    }
    /* The first one is opened and kept: everything RiotFix reads comes from it. */
    if (Count > 0 && Sdl.OpenGamepad) {
        OpenPad = Sdl.OpenGamepad(Pads[0]);
        Log("SDL: controller 0 opened for input: %s", OpenPad ? "ok" : Sdl.GetError());
    }
    Sdl.Free(Pads);
}

/* What the rest of RiotFix reads. The state is refreshed once per frame by RiotSdlReadPad; the readers below are
 * plain lookups so they cost nothing in the engine's hot input path, which asks about actions many times a frame. */
int RiotSdlHasPad(void) { return OpenPad != NULL; }

void RiotSdlReadPad(void) {
    if (!OpenPad || !Sdl.UpdateGamepads) return;
    Sdl.UpdateGamepads();
    for (int Index = 0; Index < 6; Index++) PadAxes[Index] = Sdl.GetGamepadAxis(OpenPad, Index);
    for (int Index = 0; Index < 15; Index++) PadButtons[Index] = Sdl.GetGamepadButton(OpenPad, Index) ? 1 : 0;
}

/* -32768..32767 as SDL gives it, so the caller decides the dead zone and the curve. */
int RiotSdlAxis(int Axis) { return (Axis >= 0 && Axis < 6) ? PadAxes[Axis] : 0; }
int RiotSdlButton(int Button) { return (Button >= 0 && Button < 15) ? PadButtons[Button] : 0; }

/* The engine asks for rumble once a frame, with the levels it wants playing right now rather than an event. Each
 * call is a write over the pad's own wire, so the levels are only sent on when they change; a silent frame among
 * silent frames sends nothing. The length given is longer than a frame, so an ordinary frame refreshes a still
 * running effect instead of restarting it, and shorter than a stall, so a hitch or a sudden pause lets the motors
 * fall quiet by themselves instead of buzzing on. */
void RiotSdlRumble(unsigned Low, unsigned High) {
    static unsigned SentLow, SentHigh;
    static DWORD SentAt;
    if (!OpenPad || !Sdl.RumbleGamepad) return;
    if (Low > 0xFFFF) Low = 0xFFFF;
    if (High > 0xFFFF) High = 0xFFFF;
    if (Low == SentLow && High == SentHigh) {
        if (!Low && !High) return;                            /* quiet, and it stays quiet on its own */
        if (GetTickCount() - SentAt < 100) return;            /* still playing: no need to say so again */
    }
    SentLow = Low; SentHigh = High; SentAt = GetTickCount();
    Sdl.RumbleGamepad(OpenPad, (unsigned short)Low, (unsigned short)High, 250);
}

/* Not from DllMain. RiotFix applies everything from DLL_PROCESS_ATTACH, and LoadLibrary taken from there asks for
 * the loader lock the process is already holding: the game hangs before its first frame, with nothing after this
 * module's line in the log. SDL also starts threads of its own, and no thread can run until DllMain returns.
 * So the library is opened on the first frame instead, which is the first moment the loader is free. */
static int SdlWanted, SdlTried;

static void StartOnFirstFrame(void) {
    if (!SdlWanted || SdlTried) return;
    SdlTried = 1;
    StartSdl();
}

void RiotSdlApply(void) {
    SdlWanted = IniInt("Sdl", "Enable", 1);
    if (!SdlWanted) return;
    /* Every step says so before it is taken: if the game stops here again, the log names the line it stopped on. */
    Log("SDL: on, and starting on the first frame (never from DllMain)");
    RiotTimingOnFrame(StartOnFirstFrame);
}

static void StartSdl(void) {
    char Path[MAX_PATH];
    Log("SDL: opening the library");
    Sdl.Module = OpenLibrary(Path, sizeof Path);
    if (!Sdl.Module) {
        Log("SDL: SDL3.dll was not loaded from \"%s\" (error %lu): nothing done", Path, GetLastError());
        return;
    }
    if (!FindFunctions()) { FreeLibrary(Sdl.Module); Sdl.Module = NULL; return; }
    FindOptionalFunctions();

    /* Before SDL starts, and for the reason at the top of this file. */
    Sdl.SetHint("SDL_JOYSTICK_DIRECTINPUT", "0");
    /* No window of our own: the game owns its message loop, which RiotFix already replaces. */
    Sdl.SetHint("SDL_VIDEO_DRIVER", "dummy");

    Log("SDL: library open, starting it");
    const int Version = Sdl.GetVersion();
    if (!Sdl.Init(SdlInitAudio | SdlInitGamepad)) {
        Log("SDL: %d.%d.%d loaded but did not start (%s)", Version / 1000000, Version / 1000 % 1000, Version % 1000,
            Sdl.GetError());
        FreeLibrary(Sdl.Module); Sdl.Module = NULL;
        return;
    }
    Log("SDL: %d.%d.%d started from \"%s\"", Version / 1000000, Version / 1000 % 1000, Version % 1000, Path);
    ReportAudioDevices();
    ReportGamepads();
}

/* Never call this from DllMain: SDL_Quit waits for the joystick and audio threads, and those threads need the
 * loader lock that DllMain is holding. FreeLibrary is forbidden there for the same reason. See DllMain. */
void RiotSdlStop(void) {
    if (!Sdl.Module) return;
    if (OpenPad && Sdl.CloseGamepad) { Sdl.CloseGamepad(OpenPad); OpenPad = NULL; }
    Sdl.Quit();
    FreeLibrary(Sdl.Module);
    Sdl.Module = NULL;
}

/* Which shape or letter the pad itself puts on a face button, for the names the game shows in its help text.
 * 0 when SDL cannot say, in which case the caller keeps the Xbox letters. */
int RiotSdlButtonLabel(int Button) {
    if (!OpenPad || !Sdl.GetGamepadButtonLabel) return 0;
    return Sdl.GetGamepadButtonLabel(OpenPad, Button);
}
