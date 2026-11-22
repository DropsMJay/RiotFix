/*
 * RiotAudio - DirectSound: what the game asks of it and what it gets back
 *
 * Both games use DirectSound 8 (PIH imports it by ordinal: 11 = DirectSoundCreate8, 2 = DirectSoundEnumerateA,
 * 1 = DirectSoundCreate; the TTB by name). The reverb is the standard software effect of DirectSound, not EAX:
 * the engine fills a DSEFFECTDESC with GUID_DSFX_STANDARD_I3DL2REVERB (PIH SUFFERING.EXE+0x4334E and +0x4341F,
 * TTB SufferingTTB.exe+0x49740 and +0x49839) and calls SetFX on the buffer; neither game ever uses IKsPropertySet,
 * which is the EAX path. Right after the call the engine looks for two results, E_INVALIDARG and 0x887800B4
 * (DSERR_BUFFERTOOSMALL), and on either of them it turns the effect off for that sound and carries on in silence.
 * DirectSound only puts effects on buffers that are at least 150 ms long and are not in hardware, so a sound that
 * misses either condition comes out dry.
 *
 * [Debug] LogAudio writes what each sound asks for and what DirectSound answers, so the dry sounds can be counted
 * instead of guessed at. The import table entries of DirectSoundCreate8 and DirectSoundCreate (PIH 0x58B038 and
 * 0x58B040, TTB 0x5E5038 and 0x5E5040) are made to point here; from the interfaces they return, the vtable slots
 * of CreateSoundBuffer (3) and SetFX (21) are taken over once.
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotAudio.h"
#include "RiotTiming.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>
#include <objbase.h>
void CallChain(const void *From, DWORD *Chain, int Count);

/* If the engine times a sound out by subtracting the frame time from a counter, a single long frame can take the
 * whole sound at once. The frame length is measured here so a cut can be reported next to the frame it happened in. */
static LARGE_INTEGER FrameTicks, TickRate;
static double LastFrameMilliseconds;
static void ReportAudibleDistance(void);
static void MeasureFrame(void) {
    ReportAudibleDistance();
    LARGE_INTEGER Now;
    if (!QueryPerformanceCounter(&Now)) return;
    if (!TickRate.QuadPart) QueryPerformanceFrequency(&TickRate);
    if (FrameTicks.QuadPart && TickRate.QuadPart)
        LastFrameMilliseconds = (double)(Now.QuadPart - FrameTicks.QuadPart) * 1000.0 / (double)TickRate.QuadPart;
    FrameTicks = Now;
}

/* The GUIDs are defined here instead of linking the DirectSound library: RiotFix links nothing of DirectX. */
static const GUID BufferEightId = { 0x6825a449, 0x7524, 0x4d82, { 0x92, 0x0f, 0x50, 0xe3, 0x6a, 0xb3, 0xab, 0x1e } };
static const GUID I3DL2ReverbId = { 0xef985e71, 0xd5c7, 0x42d4, { 0xba, 0x4d, 0x2d, 0x07, 0x3e, 0x2e, 0x96, 0xf4 } };
static const GUID Buffer3DId = { 0x279afa86, 0x4981, 0x11ce, { 0xa5, 0x21, 0x00, 0x20, 0xaf, 0x0b, 0xe5, 0x60 } };
static const GUID I3DL2ReverbInterfaceId = { 0x4b166a6a, 0x0d66, 0x43f3, { 0x80, 0xe3, 0xee, 0x62, 0x80, 0xde, 0xe1, 0xa4 } };

typedef struct {
    DWORD Create8Slot, CreateSlot;   /* import table entries of DirectSoundCreate8 and DirectSoundCreate */
    DWORD ThreeDimensionalFlag;      /* bit 0: the engine's "3D sound" setting (read by PIH 0x443800 / TTB 0x449BD0) */
    /* The engine only asks for an effect-capable buffer when its own gate lets it (PIH 0x442F81, TTB 0x449385): the
     * sound's category entry in CategoryTable (stride 0x1C) must be non-zero, and the byte at SoundSystemFlag of the
     * sound system object must have bit 1 set. Both are read here only to say in the log which one closes the gate. */
    DWORD SoundSystemPointer, SoundSystemFlag, CategoryTable;
    /* The engine gives itself a fixed number of voices in its sound system's constructor: twenty in all and eight
     * placed in 3D (PIH 0x444224 and 0x444232; it raises them afterwards only if the sound card reports more, which
     * no card has done since hardware mixing left Windows). When they run out, a new sound takes the voice of the
     * quietest-priority sound playing and stops it where it stands, which is why a shot fired in a busy room can be
     * cut off the instant it starts. These are the two numbers in the code, written as plain values. Ties That Bind
     * already gives itself thirty-two of each, and takes both from one instruction, so there a single number sets
     * both and the two addresses below are the same one. */
    DWORD VoiceCount, ThreeDimensionalVoiceCount;
    /* The Suffering clears the flag that lets its sound system ask for an effect, in its own constructor and with no
     * test of the configuration screen's EAX box (0x44423C: and eax, 0xFFFFFFFD). Ties That Bind tests the box there
     * instead, which is the whole difference between the two games. Turning the instruction into an or lets the
     * first game take the path it already carries, and the levels' own Reverb Effect Templates with it. */
    DWORD EffectGateClear;
    DWORD SoundSystemInLevelLibrary;   /* .rfl offset of the pointer the level library keeps to the sound system */
    /* The two sound-system methods that take six arguments, the shape of "play this sound": their offset in the
       vtable and the address each one is expected to hold, so a wrong offset is refused instead of followed. */
    DWORD PlayRequestSlots[3], PlayRequestMethods[3];
    /* The squared distance past which the engine refuses to play a sound, unless the sound carries bit 0x10
       (PIH 0x5C6DF8, read at 0x4415D8 and 0x43B594 and written nowhere in either binary). */
    DWORD AudibleDistanceSquared;
    /* Offsets in the sound system of the two fields the play method tests before anything else: a flag that stops
       it dead when set (PIH [esi+0x666], 0x441442) and a count it refuses on when not positive ([esi+0x446],
       0x441515). Reading them says which guard turned a request down. */
    DWORD RefusalFlag, RefusalCount;
} AudioAddresses;

static const AudioAddresses Addresses[GameCount] = {
    [GamePIH] = { 0x58B038, 0x58B040, 0x5C6DB8, 0x5DC1B8, 0x20, 0x5B9B18, 0x444227, 0x444235, 0x44423C, 0x17BF78, { 0xA0, 0xA4, 0xA8 }, { 0x441410, 0x441B00, 0x441A80 }, 0x5C6DF8, 0x666, 0x446 },
    [GameTTB] = { 0x5E5038, 0x5E5040, 0x639F98, 0x63FD24, 0x1E, 0x62A030, 0x44A4B1, 0x44A4B1, 0, 0x1B20B4, { 0, 0, 0 }, { 0, 0, 0 }, 0, 0, 0 },
};
static const AudioAddresses *A;

static int LogAudio, SoftwareBuffers;
static LONG VolumeFloor;
static int CarryEngineReverb;

typedef struct KnownBufferRecord { void *Buffer; DWORD Flags, Milliseconds, Bytes, PlayedAt; volatile LONG Looping; } KnownBuffer;
static KnownBuffer KnownBuffers[1024]; static volatile LONG KnownBufferCount;
/* DirectSound hands the same address out again once a buffer is released, so the table is read from the newest entry
 * backwards: an address that has been reused must report the sound that is in it now, not the one that was there
 * first. Reading it forwards measured one sound against another's length, which is what produced positions past the
 * end of the buffer in the log. */
static KnownBuffer *FindBufferToChange(void *Buffer) {
    const LONG Count = KnownBufferCount < 1024 ? KnownBufferCount : 1024;
    for (LONG Index = Count - 1; Index >= 0; Index--)
        if (KnownBuffers[Index].Buffer == Buffer) return &KnownBuffers[Index];
    return NULL;
}
static const KnownBuffer *FindBuffer(void *Buffer) { return FindBufferToChange(Buffer); }
static void RememberBuffer(void *Buffer, DWORD Flags, DWORD Milliseconds, DWORD Bytes) {
    KnownBuffer *Old = FindBufferToChange(Buffer);
    if (Old) { *Old = (KnownBuffer){ Buffer, Flags, Milliseconds, Bytes, 0, 0 }; return; }
    const LONG Index = InterlockedIncrement(&KnownBufferCount) - 1;
    if (Index < 1024) KnownBuffers[Index] = (KnownBuffer){ Buffer, Flags, Milliseconds, Bytes, 0, 0 };
}
/* [Debug] WatchSound: the size in bytes of one sound, as the editor reports it. Every call that touches a buffer of
 * exactly that size is written to the log, with no line limit, so a sound that is made and never played can be told
 * apart from one that is played at silence. The size is the fingerprint: it identifies the sound without the engine
 * having to tell us its name. */
static DWORD WatchSounds[8]; static int WatchSoundCount;
static int WatchedSize(DWORD Bytes) {
    for (int Index = 0; Index < WatchSoundCount; ++Index) if (WatchSounds[Index] == Bytes) return 1;
    return 0;
}
static int Watched(void *Buffer) {
    if (!WatchSoundCount) return 0;
    const KnownBuffer *Known = FindBuffer(Buffer);
    return Known && WatchedSize(Known->Bytes);
}

static volatile LONG PlaysOverPlaying, StopsWhilePlaying, OneShotsCutShort;
static volatile LONG StoppedEarly, StoppedNearlyDone;   /* the same stops, split by how much was left unplayed */
/* A stop that leaves less than this has not cut anything off; the comment on the Stop hook says where it comes
   from and why a percentage was the wrong measure. */
#define SoundFinishedMilliseconds 100
static volatile LONG StopsAsked, StopsAskedAtOnce, StopsAskedGone;        /* by handle: asked, no fade, already gone */
static volatile LONG ObjectStopsAsked, ObjectStopsForced, ObjectStopsGone; /* the same, by sound object */
static volatile LONG VolumesRaised;
static const char *ResultName(HRESULT Result);

/* ================================================================ Reverb
   [Audio] Reverb: the engine knows the DirectSound reverb (it fills a DSEFFECTDESC with the I3DL2 effect at PIH
   SUFFERING.EXE+0x4334E and TTB SufferingTTB.exe+0x49740) but never asks for it: that creation only happens when a
   field of the sound object is set, and no level seen so far sets it, so every sound comes out dry on the PC.
   With this on, RiotFix asks for the effect itself: each 3D sound long enough to take one (DirectSound needs 150 ms)
   is created with DSBCAPS_CTRLFX in software, given the I3DL2 effect and the chosen environment. The effect is one
   per sound, as the engine would have done, so a very loud scene costs more processor than a dry one. */
static int Reverb, ReverbEverySound, ReverbShortSounds;
/* Set once DirectSound has answered DSERR_FXUNAVAILABLE to our own SetFX: see GiveUpOnReverb below. */
static volatile LONG ReverbUnavailable;
#define ResultFxUnavailable 0x887800DCu
static DSFXI3DL2Reverb ReverbSettings;
static volatile LONG ReverbGeneration = 1, EaxGeneration;   /* bumped when the environment changes, so EAX is told */
static int EaxReverbPut;
static const struct { const char *Name; DSFXI3DL2Reverb Settings; } ReverbPresets[] = {
    { "generic",        { I3DL2_ENVIRONMENT_PRESET_GENERIC } },
    { "room",           { I3DL2_ENVIRONMENT_PRESET_ROOM } },
    { "paddedcell",     { I3DL2_ENVIRONMENT_PRESET_PADDEDCELL } },
    { "bathroom",       { I3DL2_ENVIRONMENT_PRESET_BATHROOM } },
    { "stoneroom",      { I3DL2_ENVIRONMENT_PRESET_STONEROOM } },
    { "hallway",        { I3DL2_ENVIRONMENT_PRESET_HALLWAY } },
    { "stonecorridor",  { I3DL2_ENVIRONMENT_PRESET_STONECORRIDOR } },
    { "cave",           { I3DL2_ENVIRONMENT_PRESET_CAVE } },
    { "hangar",         { I3DL2_ENVIRONMENT_PRESET_HANGAR } },
    { "arena",          { I3DL2_ENVIRONMENT_PRESET_ARENA } },
    { "sewerpipe",      { I3DL2_ENVIRONMENT_PRESET_SEWERPIPE } },
    { "alley",          { I3DL2_ENVIRONMENT_PRESET_ALLEY } },
    { "forest",         { I3DL2_ENVIRONMENT_PRESET_FOREST } },
    { "city",           { I3DL2_ENVIRONMENT_PRESET_CITY } },
    { "underwater",     { I3DL2_ENVIRONMENT_PRESET_UNDERWATER } },
};
static volatile LONG ReverbsPut, ReverbsFailed;
static int ReverbPreset, ReverbCycleKey;

/* [Audio] ReverbCycleKey: a key that walks through the environments while the game runs, to hear them one after
   another. Sounds already playing keep the environment they were made with; the next ones take the new one. */
static void ReverbCycleFrame(void) {
    static int Held;
    const int Down = (GetAsyncKeyState(ReverbCycleKey) & 0x8000) != 0;
    if (!Down) { Held = 0; return; }
    if (Held) return;
    Held = 1;
    ReverbPreset = (ReverbPreset + 1) % (int)(sizeof ReverbPresets / sizeof *ReverbPresets);
    const LONG Level = ReverbSettings.lRoom;
    ReverbSettings = ReverbPresets[ReverbPreset].Settings;
    ReverbSettings.lRoom = Level;   /* the level chosen in the .ini is kept across the environments */
    InterlockedIncrement(&ReverbGeneration);   /* the next 3D buffer sends it to EAX again */
    Log("[audio] reverb environment: %s", ReverbPresets[ReverbPreset].Name);
}

/* DirectSound puts effects only on buffers of at least 150 ms that are not in hardware. [Audio] ReverbSounds=all
   adds the 2D sounds as well, except the long ones, which are the music and the streamed tracks (the engine streams
   them in buffers of several seconds, and reverb on music is wrong). [Audio] ReverbShortSounds makes room in the
   buffers that are too short by asking for 220 ms and leaving the end silent, so they can take the effect too; a
   sound played in a loop would then loop through that silence, so it is off by default. */
#define ReverbMinimumMilliseconds 220
#define ReverbStreamMilliseconds 5000
static DWORD BufferMilliseconds(const DSBUFFERDESC *Description) {
    const WAVEFORMATEX *Format = Description->lpwfxFormat;
    if (!Format || !Format->nAvgBytesPerSec) return 0;
    return Description->dwBufferBytes * 1000 / Format->nAvgBytesPerSec;
}
static int TakesReverb(const DSBUFFERDESC *Description) {
    if (Description->dwFlags & DSBCAPS_PRIMARYBUFFER) return 0;
    if (!(Description->dwFlags & DSBCAPS_CTRL3D) && !ReverbEverySound) return 0;
    const DWORD Milliseconds = BufferMilliseconds(Description);
    if (!Milliseconds || Milliseconds >= ReverbStreamMilliseconds) return 0;
    return Milliseconds >= ReverbMinimumMilliseconds || ReverbShortSounds;
}
/* The room asked for above is silence until the game writes its sound, and the game writes only its own length. */
static void SilenceBuffer(IDirectSoundBuffer *Buffer, const WAVEFORMATEX *Format) {
    void *First = NULL, *Second = NULL; DWORD FirstSize = 0, SecondSize = 0;
    if (FAILED(IDirectSoundBuffer_Lock(Buffer, 0, 0, &First, &FirstSize, &Second, &SecondSize, DSBLOCK_ENTIREBUFFER))) return;
    const int Silence = Format && Format->wBitsPerSample == 8 ? 0x80 : 0;
    if (First) memset(First, Silence, FirstSize);
    if (Second) memset(Second, Silence, SecondSize);
    IDirectSoundBuffer_Unlock(Buffer, First, FirstSize, Second, SecondSize);
}
/* DirectSound builds an effect by asking COM for the effect's object. That needs two things from the process that the
 * games never arrange themselves: a COM apartment on the calling thread, and the effect class registered in Windows.
 * CoInitializeEx gives the first (it only adds a reference when the thread already has one, and RPC_E_CHANGED_MODE
 * means the thread is already in another apartment, which is just as good). The probe below reports the second, so the
 * log says which of the two is missing instead of leaving REGDB_E_CLASSNOTREG unexplained. */
/* The engine has a reverb path of its own (PIH SUFFERING.EXE+0x4334E, TTB SufferingTTB.exe+0x49740): it asks for a
 * buffer with DSBCAPS_CTRLFX, calls CoInitialize, fills a DSEFFECTDESC from the class at PIH 0x5970FC (which holds
 * GUID_DSFX_STANDARD_I3DL2REVERB) and calls SetFX. Counting its calls apart from RiotFix's own says whether the games
 * ever ask for reverb by themselves, which no reading of the code alone can settle. */
static volatile LONG InsideOurSetFX;
static volatile LONG EngineSetFXCalls;
static const GUID UnknownId = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID WavesReverbId = { 0x87fc0268, 0x9a55, 0x4360, { 0x95, 0xaa, 0x00, 0x4a, 0x1d, 0x9d, 0xe2, 0x6c } };
static const GUID WavesReverbInterfaceId = { 0x46858c3a, 0x0dc6, 0x45e3, { 0xb7, 0x60, 0xd4, 0xee, 0xf1, 0x6c, 0xb3, 0x25 } };
static const GUID GargleId = { 0xdafd8210, 0x5711, 0x4b91, { 0x9f, 0xe3, 0xf7, 0x5b, 0x7a, 0xe2, 0x79, 0xbf } };
static const GUID EchoId = { 0xef3e932c, 0xd40b, 0x4f51, { 0x8c, 0xcf, 0x3f, 0x98, 0xf1, 0xb2, 0x9d, 0x5d } };
static int ClassAvailable(const GUID *Class) {
    IUnknown *Probe = NULL;
    const HRESULT Made = CoCreateInstance(Class, NULL, CLSCTX_INPROC_SERVER, &UnknownId, (void **)&Probe);
    if (SUCCEEDED(Made) && Probe) Probe->lpVtbl->Release(Probe);
    return SUCCEEDED(Made);
}
/* Which effect class to ask DirectSound for. The I3DL2 reverb is the one the engine itself names, so it is the first
 * choice; the Waves reverb is the other reverb Windows ships, with coarser controls but the same audible purpose. The
 * probe below decides between them once, from what this Windows actually answers, and the log says which one won. */
static const GUID *ChosenReverbId;
static volatile LONG EffectsChecked;
static void PrepareEffects(void) {
    const HRESULT Apartment = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (InterlockedExchange(&EffectsChecked, 1)) return;
    Log("[audio] COM apartment for effects: %s", Apartment == S_OK ? "opened by RiotFix (the game had none)" :
        Apartment == S_FALSE ? "already open" : ResultName(Apartment));
    const int HasI3DL2 = ClassAvailable(&I3DL2ReverbId), HasWaves = ClassAvailable(&WavesReverbId);
    Log("[audio] effect classes in this Windows: I3DL2 reverb %s, Waves reverb %s, gargle %s, echo %s",
        HasI3DL2 ? "yes" : "NO", HasWaves ? "yes" : "NO",
        ClassAvailable(&GargleId) ? "yes" : "NO", ClassAvailable(&EchoId) ? "yes" : "NO");
    ChosenReverbId = HasI3DL2 ? &I3DL2ReverbId : HasWaves ? &WavesReverbId : NULL;
    CarryEngineReverb = CarryEngineReverb && !HasI3DL2 && HasWaves;
    Log("[audio] reverb will be asked as: %s", !ChosenReverbId ? "nothing (no reverb class is registered)" :
        ChosenReverbId == &I3DL2ReverbId ? "I3DL2 reverb" : "Waves reverb (the I3DL2 one is missing)");
}
/* The Waves reverb has four controls instead of the I3DL2 set, so the chosen environment is carried over by the two
 * values both effects share: how loud the reverb is and how long it takes to die away. */
static void PutWavesSettings(IDirectSoundBuffer8 *Eight) {
    IDirectSoundFXWavesReverb *Settings = NULL;
    if (FAILED(IDirectSoundBuffer8_GetObjectInPath(Eight, &WavesReverbId, 0, &WavesReverbInterfaceId,
        (void **)&Settings)) || !Settings) return;
    DSFXWavesReverb Waves;
    Waves.fInGain = 0.0f;
    Waves.fReverbMix = ReverbSettings.lRoom / 100.0f;
    if (Waves.fReverbMix < -96.0f) Waves.fReverbMix = -96.0f;
    if (Waves.fReverbMix > 0.0f) Waves.fReverbMix = 0.0f;
    Waves.fReverbTime = ReverbSettings.flDecayTime * 1000.0f;
    if (Waves.fReverbTime < 1.0f) Waves.fReverbTime = 1.0f;
    if (Waves.fReverbTime > 3000.0f) Waves.fReverbTime = 3000.0f;
    Waves.fHighFreqRTRatio = ReverbSettings.flDecayHFRatio;
    if (Waves.fHighFreqRTRatio < 0.001f) Waves.fHighFreqRTRatio = 0.001f;
    if (Waves.fHighFreqRTRatio > 0.999f) Waves.fHighFreqRTRatio = 0.999f;
    IDirectSoundFXWavesReverb_SetAllParameters(Settings, &Waves);
    IDirectSoundFXWavesReverb_Release(Settings);
}
static void PutReverb(IDirectSoundBuffer *Buffer) {
    PrepareEffects();
    IDirectSoundBuffer8 *Eight = NULL;
    if (FAILED(IDirectSoundBuffer_QueryInterface(Buffer, &BufferEightId, (void **)&Eight)) || !Eight) {
        InterlockedIncrement(&ReverbsFailed);
        return;
    }
    DSEFFECTDESC Effect; DWORD Status = 0;
    memset(&Effect, 0, sizeof Effect);
    Effect.dwSize = sizeof Effect;
    Effect.dwFlags = DSFX_LOCSOFTWARE;
    if (!ChosenReverbId) {
        InterlockedIncrement(&ReverbsFailed);
        IDirectSoundBuffer8_Release(Eight);
        return;
    }
    Effect.guidDSFXClass = *ChosenReverbId;
    InterlockedIncrement(&InsideOurSetFX);
    const HRESULT Result = IDirectSoundBuffer8_SetFX(Eight, 1, &Effect, &Status);
    InterlockedDecrement(&InsideOurSetFX);
    if (SUCCEEDED(Result)) {
        if (ChosenReverbId == &WavesReverbId) PutWavesSettings(Eight);
        else {
            IDirectSoundFXI3DL2Reverb *Settings = NULL;
            if (SUCCEEDED(IDirectSoundBuffer8_GetObjectInPath(Eight, &I3DL2ReverbId, 0, &I3DL2ReverbInterfaceId, (void **)&Settings)) && Settings) {
                IDirectSoundFXI3DL2Reverb_SetAllParameters(Settings, &ReverbSettings);
                IDirectSoundFXI3DL2Reverb_Release(Settings);
            }
        }
        InterlockedIncrement(&ReverbsPut);
    } else {
        InterlockedIncrement(&ReverbsFailed);
        if (ReverbsFailed <= 5) Log("[audio] reverb refused by DirectSound: %s", ResultName(Result));
        /* GiveUpOnReverb. DSERR_FXUNAVAILABLE means this DirectSound does not do DMO effects at all, and the
         * reporter's two sessions show what that costs when we keep asking anyway. With Windows' own
         * dsound.dll: 41 SetFX calls, 41 placed in software. With DSOAL in the game's folder: 64 calls, 64
         * refused, every one DSERR_FXUNAVAILABLE - DSOAL implements EAX through IKsPropertySet and does not
         * implement SetFX.
         *
         * The refusals are not the real cost. To ask for an effect a buffer must be created with
         * DSBCAPS_CTRLFX, and that flag forces it into software. So on a DSOAL machine 64 buffers were pushed
         * out of the 128 hardware voices DSOAL provides, in exchange for a reverb that was never applied to
         * any of them. One refusal is enough to know the whole session will go the same way, so stop asking:
         * later buffers are created as the game asked, and the reverb stays off until RiotFix can reach EAX. */
        if ((unsigned)Result == ResultFxUnavailable && !InterlockedExchange(&ReverbUnavailable, 1))
            Log("[audio] this DirectSound has no DMO effects (DSERR_FXUNAVAILABLE), so RiotFix will stop asking"
                " for reverb: the rest of the sounds are left as the game asked for them");
    }
    IDirectSoundBuffer8_Release(Eight);
}

/* ================================================================ The engine's own reverb, carried to another effect
 *
 * Ties That Bind asks DirectSound for the I3DL2 reverb by itself (SufferingTTB.exe+0x49740), once the EAX box of the
 * configuration screen is ticked: that sets bit 1 of the setup flags, which the sound system's constructor copies to
 * its own flags at +0x1E, which opens the gate at +0x49385. The level then writes its Reverb Effect Template into the
 * effect. Windows 11 no longer registers that effect class, so the call is refused and the level's reverb is lost.
 * What follows puts the Waves reverb on the buffer in its place and hands the engine an object that looks like the
 * I3DL2 one, turning the settings it writes into the two values the Waves reverb understands. The environments then
 * come from the game's own levels rather than from RiotFix.
 */
static volatile LONG EngineReverbsCarried;

typedef struct { const void *Vtable; volatile LONG References; IDirectSoundFXWavesReverb *Waves; DSFXI3DL2Reverb Asked; } ReverbShim;

static void ShimApply(ReverbShim *Shim) {
    DSFXWavesReverb Waves;
    Waves.fInGain = 0.0f;
    Waves.fReverbMix = Shim->Asked.lRoom / 100.0f;
    if (Waves.fReverbMix < -96.0f) Waves.fReverbMix = -96.0f;
    if (Waves.fReverbMix > 0.0f) Waves.fReverbMix = 0.0f;
    Waves.fReverbTime = Shim->Asked.flDecayTime * 1000.0f;
    if (Waves.fReverbTime < 1.0f) Waves.fReverbTime = 1.0f;
    if (Waves.fReverbTime > 3000.0f) Waves.fReverbTime = 3000.0f;
    Waves.fHighFreqRTRatio = Shim->Asked.flDecayHFRatio;
    if (Waves.fHighFreqRTRatio < 0.001f) Waves.fHighFreqRTRatio = 0.001f;
    if (Waves.fHighFreqRTRatio > 0.999f) Waves.fHighFreqRTRatio = 0.999f;
    IDirectSoundFXWavesReverb_SetAllParameters(Shim->Waves, &Waves);
}
static HRESULT WINAPI ShimQueryInterface(void *Self, REFIID Interface, void **Object) {
    if (!Object) return E_POINTER;
    if (IsEqualGUID(Interface, &UnknownId) || IsEqualGUID(Interface, &I3DL2ReverbInterfaceId)) {
        InterlockedIncrement(&((ReverbShim *)Self)->References);
        *Object = Self; return S_OK;
    }
    *Object = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI ShimAddRef(void *Self) { return (ULONG)InterlockedIncrement(&((ReverbShim *)Self)->References); }
static ULONG WINAPI ShimRelease(void *Self) {
    ReverbShim *Shim = (ReverbShim *)Self;
    const LONG Left = InterlockedDecrement(&Shim->References);
    if (Left <= 0) { IDirectSoundFXWavesReverb_Release(Shim->Waves); free(Shim); return 0; }
    return (ULONG)Left;
}
static HRESULT WINAPI ShimSetAllParameters(void *Self, LPCDSFXI3DL2Reverb Settings) {
    if (!Settings) return E_POINTER;
    ((ReverbShim *)Self)->Asked = *Settings;
    ShimApply((ReverbShim *)Self);
    return S_OK;
}
static HRESULT WINAPI ShimGetAllParameters(void *Self, LPDSFXI3DL2Reverb Settings) {
    if (!Settings) return E_POINTER;
    *Settings = ((ReverbShim *)Self)->Asked;
    return S_OK;
}
static HRESULT WINAPI ShimSetPreset(void *Self, DWORD Preset) {
    static const DSFXI3DL2Reverb Known[] = {
        { I3DL2_ENVIRONMENT_PRESET_GENERIC }, { I3DL2_ENVIRONMENT_PRESET_PADDEDCELL },
        { I3DL2_ENVIRONMENT_PRESET_ROOM }, { I3DL2_ENVIRONMENT_PRESET_BATHROOM },
    };
    if (Preset >= sizeof Known / sizeof *Known) return E_INVALIDARG;
    ((ReverbShim *)Self)->Asked = Known[Preset];
    ShimApply((ReverbShim *)Self);
    return S_OK;
}
static HRESULT WINAPI ShimGetPreset(void *Self, LPDWORD Preset) { (void)Self; if (!Preset) return E_POINTER; *Preset = 0; return S_OK; }
static HRESULT WINAPI ShimSetQuality(void *Self, LONG Quality) { (void)Self; (void)Quality; return S_OK; }
static HRESULT WINAPI ShimGetQuality(void *Self, LONG *Quality) { (void)Self; if (!Quality) return E_POINTER; *Quality = 2; return S_OK; }

static const void *ShimVtable[] = {
    (const void *)ShimQueryInterface, (const void *)ShimAddRef, (const void *)ShimRelease,
    (const void *)ShimSetAllParameters, (const void *)ShimGetAllParameters,
    (const void *)ShimSetPreset, (const void *)ShimGetPreset,
    (const void *)ShimSetQuality, (const void *)ShimGetQuality,
};

static volatile LONG AudioLogLines;
#define MaximumAudioLogLines 400
/* [Debug] LogAllSounds: LogAudio keeps its line limits so one long session does not fill the log with the same
   handful of sounds, but a limit reached in the middle of a hunt loses exactly the line that mattered, without
   saying so. With this on, every limit below is lifted and the log holds every sound of the session. */
static int LogAllSounds;
#define LogRoom(Counter, Limit) (LogAllSounds || InterlockedIncrement(&(Counter)) <= (Limit))

typedef HRESULT (WINAPI *CreateFunctionType)(LPCGUID, LPDIRECTSOUND8 *, LPUNKNOWN);
typedef HRESULT (WINAPI *CreateSoundBufferFunctionType)(IDirectSound8 *, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER *, LPUNKNOWN);
typedef HRESULT (WINAPI *StopFunctionType)(IDirectSoundBuffer *);
static StopFunctionType OriginalStop;
static HRESULT WINAPI Stop(IDirectSoundBuffer *Self) {
    DWORD Status = 0;
    if (SUCCEEDED(IDirectSoundBuffer_GetStatus(Self, &Status)) && (Status & DSBSTATUS_PLAYING)) {
        InterlockedIncrement(&StopsWhilePlaying);
        const KnownBuffer *Known = FindBuffer(Self);
        if (Known && !Known->Looping) {
            InterlockedIncrement(&OneShotsCutShort);
            /* How far in it was decides whether this is a defect at all, and the old counter did not ask.
             *
             * A one-shot stopped at 97% has finished; stopping it is housekeeping. One stopped at 0% was never
             * heard. Both were counted the same and reported as "one-shots cut short", so a session with 33 of
             * them read as 33 faults when nearly all were the harmless kind - which is how a theory survived a
             * log that should have killed it. They are counted apart now, and the caller is written down for
             * the ones that matter, because who calls Stop is the whole question: the engine's voice stealer,
             * a distance cull and a scripted silence are three different bugs. */
            /* The measure is milliseconds left, not the percentage played, and that correction came out of a
             * session. Nine of the twenty stops it named left between 34 and 44 ms unplayed - a 356 ms sound
             * at 89%, a 293 ms one at 85%, a 212 ms one at 84%. They cluster that tightly because they are one
             * event: the sound reached its end and the stop arrived on the next update, roughly 40 ms later.
             * None was audible. But "90% played" calls a 356 ms sound with 39 ms left an early stop, so all
             * nine were reported as genuinely cut off and inflated the only number that mattered.
             *
             * 40 ms left is inaudible whatever the sound's length and 400 ms left is audible whatever it is, so
             * the percentage was measuring the wrong thing. In that session the two groups were 34-44 ms and
             * 341-2158 ms, with nothing at all in between; 100 ms sits inside that gap, clear of the update
             * period on one side and far from the real defects on the other. The line prints the number as
             * well, so a reader can draw the line somewhere else without needing another build. */
            DWORD Position = 0;
            if (SUCCEEDED(IDirectSoundBuffer_GetCurrentPosition(Self, &Position, NULL)) && Known->Bytes) {
                const DWORD Played = Position >= Known->Bytes ? Known->Bytes : Position;
                const unsigned long Through = Played * 100 / Known->Bytes;
                const unsigned long Left = (unsigned long)
                    ((unsigned long long)(Known->Bytes - Played) * Known->Milliseconds / Known->Bytes);
                if (Left <= SoundFinishedMilliseconds) InterlockedIncrement(&StoppedNearlyDone);
                else {
                    InterlockedIncrement(&StoppedEarly);
                    if (LogAudio && (LogAllSounds || StoppedEarly <= 60)) {
                        char Caller[MAX_PATH + 24];
                        AddressName((DWORD)__builtin_return_address(0), Caller, sizeof Caller);
                        Log("[audio] a %lu ms sound was stopped with %lu ms still to play (%lu%% in), by %s",
                            (unsigned long)Known->Milliseconds, Left, Through, Caller);
                    }
                }
                if (Left <= SoundFinishedMilliseconds && LogAudio && LogAllSounds)
                    Log("[audio] a %lu ms sound was stopped with %lu ms left (%lu%% in; it had finished)",
                        (unsigned long)Known->Milliseconds, Left, Through);
            }
        }
    }
    if (Watched(Self)) {
        const KnownBuffer *Known = FindBuffer(Self);
        DWORD Position = 0;
        if (!(Status & DSBSTATUS_PLAYING)) Log("[watch] the sound was stopped, it had already finished");
        else if (SUCCEEDED(IDirectSoundBuffer_GetCurrentPosition(Self, &Position, NULL)) && Known && Known->Bytes) {
            const DWORD Through = Position >= Known->Bytes ? 100 : Position * 100 / Known->Bytes;
            /* A sound cut off before it could be heard is the one worth tracing: name the engine code that stopped
             * it, so the decision can be read in Ghidra instead of guessed at. */
            if (Through <= 10) {
                /* If the engine runs out of voices it recycles the oldest, which is what a sound cut off at its very
                 * start looks like from here. Counting the buffers playing at that moment says whether the cuts
                 * happen at a ceiling: a number that repeats exactly is a limit, a number that varies is not. */
                /* Counted from what is already recorded here. Asking each buffer for its state would mean calling a
                 * method on buffers the game may have released, which is reading memory that no longer belongs to
                 * anyone: a sound told to play less than its own length ago is counted as still sounding instead. */
                LONG Playing = 0;
                const DWORD Now = GetTickCount();
                const LONG Count = KnownBufferCount < 1024 ? KnownBufferCount : 1024;
                for (LONG At = 0; At < Count; At++) {
                    const KnownBuffer *Other = &KnownBuffers[At];
                    if (Other->Buffer == Self || !Other->PlayedAt) continue;
                    if (Other->Looping || Now - Other->PlayedAt < Other->Milliseconds) Playing++;
                }
                Log("[watch] %ld other sounds were playing at that moment", Playing);
                DWORD Chain[3]; CallChain(&Status, Chain, 3);
                char Names[3][MAX_PATH + 32];
                for (int Index = 0; Index < 3; Index++)
                    if (Chain[Index]) AddressName(Chain[Index], Names[Index], sizeof Names[Index]);
                    else strcpy(Names[Index], "-");
                Log("[watch] the sound was stopped %lu%% of the way through, in a frame of %.1f ms, by %s <- %s <- %s",
                    (unsigned long)Through, LastFrameMilliseconds, Names[0], Names[1], Names[2]);
            } else Log("[watch] the sound was stopped %lu%% of the way through", (unsigned long)Through);
        }
        else Log("[watch] the sound was stopped while still playing");
    }
    return OriginalStop(Self);
}

typedef HRESULT (WINAPI *GetObjectInPathFunctionType)(IDirectSoundBuffer8 *, REFGUID, DWORD, REFGUID, void **);
static GetObjectInPathFunctionType OriginalGetObjectInPath;
static HRESULT WINAPI GetObjectInPath(IDirectSoundBuffer8 *Self, REFGUID Class, DWORD Index, REFGUID Interface,
                                      void **Object) {
    if (CarryEngineReverb && !InsideOurSetFX && Class && Interface && Object &&
        IsEqualGUID(Class, &I3DL2ReverbId) && IsEqualGUID(Interface, &I3DL2ReverbInterfaceId)) {
        IDirectSoundFXWavesReverb *Waves = NULL;
        const HRESULT Got = OriginalGetObjectInPath(Self, &WavesReverbId, Index, &WavesReverbInterfaceId,
                                                    (void **)&Waves);
        if (FAILED(Got) || !Waves) return Got;
        ReverbShim *Shim = (ReverbShim *)calloc(1, sizeof *Shim);
        if (!Shim) { IDirectSoundFXWavesReverb_Release(Waves); return E_OUTOFMEMORY; }
        Shim->Vtable = ShimVtable; Shim->References = 1; Shim->Waves = Waves;
        *Object = Shim;
        return S_OK;
    }
    return OriginalGetObjectInPath(Self, Class, Index, Interface, Object);
}

typedef HRESULT (WINAPI *SetFXFunctionType)(IDirectSoundBuffer8 *, DWORD, LPDSEFFECTDESC, LPDWORD);
static CreateFunctionType OriginalCreate8, OriginalCreate;
static CreateSoundBufferFunctionType OriginalCreateSoundBuffer;
static SetFXFunctionType OriginalSetFX;

static char ResultText[64];
static const char *ResultName(HRESULT Result) {
    switch ((DWORD)Result) {
        case S_OK: return "ok";
        case (DWORD)DS_NO_VIRTUALIZATION: return "ok (no virtualisation)";
        case (DWORD)E_INVALIDARG: return "E_INVALIDARG";
        case (DWORD)E_OUTOFMEMORY: return "E_OUTOFMEMORY";
        case 0x887800B4u: return "DSERR_BUFFERTOOSMALL (under 150 ms)";
        case 0x887800DCu: return "DSERR_FXUNAVAILABLE";
        case 0x8878001Eu: return "DSERR_CONTROLUNAVAIL";
        case 0x88780032u: return "DSERR_INVALIDCALL";
        case 0x887800AAu: return "DSERR_UNINITIALIZED";
        case 0x88780078u: return "DSERR_ALLOCATED (no free hardware buffer)";
        case 0x8878000Au: return "DSERR_INVALIDPARAM";
        case 0x88780046u: return "DSERR_OUTOFMEMORY";
        case 0x887800FAu: return "DSERR_NODRIVER";
        case 0x88780096u: return "DSERR_BADFORMAT";
        case 0x88780050u: return "DSERR_UNSUPPORTED";
        case (DWORD)E_NOINTERFACE: return "E_NOINTERFACE";
        case 0x80040154u: return "REGDB_E_CLASSNOTREG (Windows has no registered DirectSound effect: see RiotFix.ini)";
        default: snprintf(ResultText, sizeof ResultText, "0x%08lx", (unsigned long)Result); return ResultText;
    }
}

/* ================================================================ Which DirectSound is this, and what does it speak
 *
 * A wrapper in the game's folder - DSOAL, IndirectSound - replaces dsound.dll and answers the same calls through
 * something else, OpenAL in DSOAL's case. That matters here because the two ways of asking for reverb are not the
 * same way:
 *
 *   SetFX with a DSEFFECTDESC        the DMO path, which is what both of these games use
 *   IKsPropertySet with an EAX set   the property-set path, which is how games of the EAX era asked
 *
 * DSOAL's own description says it implements EAX by way of OpenAL and that it "fools applications into thinking
 * there is a hardware accelerated sound device", which is why a log from a machine running it reports 128 hardware
 * buffers where Windows alone reports none. What it does with SetFX is not stated anywhere found, and absence of a
 * mention is not an answer - so this asks the thing itself, on the machine it is running on, rather than reasoning
 * about it. The answer decides whether RiotFix can one day carry the game's reverb across from one path to the
 * other, and it costs one QueryInterface and four QuerySupport calls, once. */
typedef struct KsPropertySet KsPropertySet;
typedef struct {
    HRESULT (WINAPI *QueryInterface)(KsPropertySet *, const GUID *, void **);
    ULONG   (WINAPI *AddRef)(KsPropertySet *);
    ULONG   (WINAPI *Release)(KsPropertySet *);
    HRESULT (WINAPI *Get)(KsPropertySet *, const GUID *, ULONG, void *, ULONG, void *, ULONG, ULONG *);
    HRESULT (WINAPI *Set)(KsPropertySet *, const GUID *, ULONG, void *, ULONG, void *, ULONG);
    HRESULT (WINAPI *QuerySupport)(KsPropertySet *, const GUID *, ULONG, ULONG *);
} KsPropertySetMethods;
struct KsPropertySet { const KsPropertySetMethods *Methods; };

static const GUID PlacedBufferId = { 0x279AFA86, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID PropertySetId  = { 0x31EFAC30, 0x515C, 0x11D0, { 0xA9, 0xAA, 0x00, 0xAA, 0x00, 0x61, 0xBE, 0x93 } };
static const GUID Eax2ListenerId = { 0x0306A6A8, 0xB224, 0x11D2, { 0x99, 0xE5, 0x00, 0x00, 0xE8, 0xD8, 0xC7, 0x22 } };
static const GUID Eax2BufferId   = { 0x0306A6A7, 0xB224, 0x11D2, { 0x99, 0xE5, 0x00, 0x00, 0xE8, 0xD8, 0xC7, 0x22 } };
static const GUID Eax3ListenerId = { 0xA8FA6882, 0xB476, 0x11D3, { 0xBD, 0xB9, 0x00, 0xC0, 0xF0, 0x2D, 0xDF, 0x87 } };
static const GUID Eax3BufferId   = { 0xA8FA6881, 0xB476, 0x11D3, { 0xBD, 0xB9, 0x00, 0xC0, 0xF0, 0x2D, 0xDF, 0x87 } };

/* ALLPARAMETERS is 1 in both of these sets: the EAX 3.0 header has DSPROPERTY_EAXLISTENER_NONE first and
 * ALLPARAMETERS second, and DSOAL's own eax.h has EAXSOURCE_NONE first and EAXSOURCE_ALLPARAMETERS second.
 * This read 2 until the header was obtained, which asks about ENVIRONMENT instead - the probe's answer still
 * stood, because it proves the property set is live either way, but the number was wrong. */
#define EaxListenerAllParameters 1
#define EaxBufferAllParameters   1

/* EAXLISTENERPROPERTIES, EAX 3.0, field for field from Creative's header as shipped in id Software's released
 * Return to Castle Wolfenstein source. Not written from memory: one field out of place writes a decay time into
 * a rolloff factor, and the result is silence or noise with nothing in the log to explain it.
 *
 * DSOAL defines none of this. Its eax.h carries the GUIDs and the property enum and nothing else, and the bytes
 * go straight through to the OpenAL driver by a loaded EAXSetDirect, so the layout that matters is Creative's. */
typedef struct { float x, y, z; } EaxVector;
typedef struct {
    unsigned long ulEnvironment;
    float flEnvironmentSize;
    float flEnvironmentDiffusion;
    long  lRoom;
    long  lRoomHF;
    long  lRoomLF;
    float flDecayTime;
    float flDecayHFRatio;
    float flDecayLFRatio;
    long  lReflections;
    float flReflectionsDelay;
    EaxVector vReflectionsPan;
    long  lReverb;
    float flReverbDelay;
    EaxVector vReverbPan;
    float flEchoTime;
    float flEchoDepth;
    float flModulationTime;
    float flModulationDepth;
    float flAirAbsorptionHF;
    float flHFReference;
    float flLFReference;
    float flRoomRolloffFactor;
    unsigned long ulFlags;
} EaxListenerProperties;

#define EaxEnvironmentUndefined 26          /* no preset: the numbers below are used as given */
#define EaxListenerDefaultFlags 0x0000003Fu /* the six scaling flags the header's DEFAULTFLAGS names */
#define KsPropertySupportGet 1
#define KsPropertySupportSet 2

static const GUID EarsId = { 0x279AFA84, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };

static int EaxProbedPrimary, EaxProbedPlaced;
static int EaxListenerSettable;   /* what the probe found, for whoever carries the reverb across later */

static void ReportDirectSoundInUse(void) {
    HMODULE Module = GetModuleHandleA("dsound.dll");
    char Path[MAX_PATH] = "";
    if (Module && GetModuleFileNameA(Module, Path, sizeof Path))
        Log("Audio: DirectSound is %s", Path);
    else
        Log("Audio: dsound.dll could not be located in this process");
}

/* Ask one object whether it carries an EAX property set.
 *
 * There are four doors, not one, and the first version of this probe stopped at the first that opened - which is
 * how it produced an answer that looked conclusive and was not. On a machine running DSOAL it reported "the
 * property set came from the buffer" and then EAX 2.0 and 3.0 as not offered, and both halves were true: the
 * object it asked was the primary buffer, that buffer did hand over a property set, and that property set knows
 * nothing about EAX. The doors it never knocked on were the ones the documentation points at.
 *
 * Microsoft's own page gets the property set by calling QueryInterface on a DirectSound 3D buffer, and the
 * listener half of EAX lives with IDirectSound3DListener, which is reached from the primary buffer. Wine had to
 * add IKsPropertySet to its primary buffer as a separate change years after the secondary one worked. So an
 * implementation can answer at one door and be silent at another without being broken, and a probe that stops
 * early will mislabel it.
 *
 * This asks all four and names each in the log: the primary buffer and its 3D listener for the listener
 * properties, a 3D sound buffer and its 3D interface for the per-sound ones. It only reads. */
static void AskDoor(const char *Where, void *Object) {
    KsPropertySet *Set = NULL;
    if (Object) ((IUnknown *)Object)->lpVtbl->QueryInterface((IUnknown *)Object, &PropertySetId, (void **)&Set);
    if (!Set) { Log("Audio: %s offers no property set at all", Where); return; }
    static const struct { const GUID *Id; ULONG Property; const char *Name; } Asked[] = {
        { &Eax2ListenerId, EaxListenerAllParameters, "EAX 2.0 listener" },
        { &Eax2BufferId,   EaxBufferAllParameters,   "EAX 2.0 buffer"   },
        { &Eax3ListenerId, EaxListenerAllParameters, "EAX 3.0 listener" },
        { &Eax3BufferId,   EaxBufferAllParameters,   "EAX 3.0 buffer"   },
    };
    for (size_t Index = 0; Index < sizeof Asked / sizeof *Asked; Index++) {
        ULONG Support = 0;
        const HRESULT Result = Set->Methods->QuerySupport(Set, Asked[Index].Id, Asked[Index].Property, &Support);
        const int CanSet = SUCCEEDED(Result) && (Support & KsPropertySupportSet);
        Log("Audio: %s, %s: %s", Where, Asked[Index].Name,
            FAILED(Result) ? "not offered" :
            CanSet ? ((Support & KsPropertySupportGet) ? "can be read and set" : "can be set") :
            (Support & KsPropertySupportGet) ? "can be read but not set" : "offered but neither read nor set");
        if (CanSet && Asked[Index].Id == &Eax3ListenerId && !EaxListenerSettable) {
            EaxListenerSettable = 1;
            /* Say what was measured, not what will happen: with no environment named in the .ini there is no
             * reverb to carry, and the first wording claimed otherwise in a session that had none. */
            Log("Audio: the EAX 3.0 listener can be set through %s%s", Where, Reverb
                ? ", so the reverb goes that way instead of through DirectSound effects"
                : " (no environment is chosen in the .ini, so nothing is sent)");
        }
    }
    Set->Methods->Release(Set);
}

/* The primary buffer, and the 3D listener that is reached from it: where the listener half of EAX lives. */
static void ProbeEaxListener(IDirectSoundBuffer *Primary) {
    if (EaxProbedPrimary) return;
    EaxProbedPrimary = 1;
    ReportDirectSoundInUse();
    AskDoor("the primary buffer", Primary);
    IDirectSound3DListener *Ears = NULL;
    if (SUCCEEDED(IDirectSoundBuffer_QueryInterface(Primary, &EarsId, (void **)&Ears)) && Ears) {
        AskDoor("the 3D listener", Ears);
        IDirectSound3DListener_Release(Ears);
    } else {
        Log("Audio: the primary buffer has no 3D listener to ask");
    }
}

/* A 3D sound buffer, and its 3D interface: where the per-sound half lives. */
static void ProbeEax(IDirectSoundBuffer *Buffer) {
    if (EaxProbedPlaced) return;
    EaxProbedPlaced = 1;
    if (!EaxProbedPrimary) ReportDirectSoundInUse();
    AskDoor("a 3D sound buffer", Buffer);
    IDirectSound3DBuffer *Placed = NULL;
    if (SUCCEEDED(IDirectSoundBuffer_QueryInterface(Buffer, &PlacedBufferId, (void **)&Placed)) && Placed) {
        AskDoor("that buffer's 3D interface", Placed);
        IDirectSound3DBuffer_Release(Placed);
    } else {
        Log("Audio: that sound buffer has no 3D interface to ask");
    }
}

/* The reverb through EAX, for the machines where DirectSound effects do not exist.
 *
 * This is the whole reason the probe was built. DSOAL refuses every SetFX with DSERR_FXUNAVAILABLE - 64 of 64
 * in the session that proved it - and offers EAX 2.0 and 3.0 on its 3D buffers instead. The nine numbers
 * RiotFix already holds as DSFXI3DL2Reverb are the same nine EAX names in the same units, so the environment
 * the .ini asks for can be sent either way.
 *
 * EAX is a listener property: one call sets the room for every 3D sound at once, where SetFX had to be put on
 * each buffer separately. So there is nothing to repeat per sound, and no DSBCAPS_CTRLFX, which means the
 * buffers are no longer forced out of hardware to carry an effect.
 *
 * Three of the EAX fields have no I3DL2 counterpart and take the header's own defaults; I3DL2's flDensity has
 * no EAX counterpart and is dropped. ulEnvironment is set to UNDEFINED so that no preset is applied over the
 * numbers that follow it - that is the one choice here that the header does not make for us, and it is the
 * first thing to question if a room comes out sounding like somewhere else. */
static void ApplyEaxReverb(IDirectSoundBuffer *Buffer) {
    if (!Reverb || !EaxListenerSettable || EaxGeneration == ReverbGeneration) return;
    KsPropertySet *Set = NULL;
    if (FAILED(IDirectSoundBuffer_QueryInterface(Buffer, &PropertySetId, (void **)&Set)) || !Set) return;

    EaxListenerProperties Room;
    memset(&Room, 0, sizeof Room);
    Room.ulEnvironment         = EaxEnvironmentUndefined;
    Room.flEnvironmentSize     = 7.5f;
    Room.flEnvironmentDiffusion = ReverbSettings.flDiffusion / 100.0f;   /* I3DL2 is a percentage, EAX is 0..1 */
    Room.lRoom                 = ReverbSettings.lRoom;
    Room.lRoomHF               = ReverbSettings.lRoomHF;
    Room.lRoomLF               = 0;
    Room.flDecayTime           = ReverbSettings.flDecayTime;
    Room.flDecayHFRatio        = ReverbSettings.flDecayHFRatio;
    Room.flDecayLFRatio        = 1.0f;
    Room.lReflections          = ReverbSettings.lReflections;
    Room.flReflectionsDelay    = ReverbSettings.flReflectionsDelay;
    Room.lReverb               = ReverbSettings.lReverb;
    Room.flReverbDelay         = ReverbSettings.flReverbDelay;
    Room.flEchoTime            = 0.25f;
    Room.flEchoDepth           = 0.0f;
    Room.flModulationTime      = 0.25f;
    Room.flModulationDepth     = 0.0f;
    Room.flAirAbsorptionHF     = -5.0f;
    Room.flHFReference         = ReverbSettings.flHFReference;
    Room.flLFReference         = 250.0f;
    Room.flRoomRolloffFactor   = ReverbSettings.flRoomRolloffFactor;
    Room.ulFlags               = EaxListenerDefaultFlags;
    /* Every field held to the EAX header's own limits, because one out-of-range number loses the whole call.
     *
     * Found in play: the F2 key walked the environments and EAX refused exactly one of them, "city", with
     * E_FAIL, while the other eight went through. The reason is in dsound.h, which contradicts itself - it
     * defines DSFX_I3DL2REVERB_REVERB_MAX as 2000 and then gives the CITY preset lReverb = 2217. EAX's own
     * maximum is 2000 as well, so it threw the structure out. It is not a lost minus sign: GENERIC is 200,
     * BATHROOM 1030 and UNDERWATER 1700, so a positive late reverb is ordinary here and CITY is simply 217
     * over the line. Clamping is the only legal reading, and it costs 2.17 dB on that one environment.
     *
     * That refusal is also the best evidence the field order is right. A misaligned structure would have been
     * refused for nearly every preset, not one: the values in this table include 5000 for flHFReference and
     * levels in the -10000..0 range, and the fields they would land on if we were off by a slot have maxima
     * of 1.0, 0.1 and 0.3. Eight accepted and exactly the one known-bad value refused is what a correct
     * layout looks like. */
    #define HoldBetween(Field, Low, High) do { if ((Field) < (Low)) (Field) = (Low); \
                                               else if ((Field) > (High)) (Field) = (High); } while (0)
    HoldBetween(Room.flEnvironmentDiffusion, 0.0f, 1.0f);
    HoldBetween(Room.lRoom, -10000, 0);
    HoldBetween(Room.lRoomHF, -10000, 0);
    HoldBetween(Room.flDecayTime, 0.1f, 20.0f);
    HoldBetween(Room.flDecayHFRatio, 0.1f, 2.0f);
    HoldBetween(Room.lReflections, -10000, 1000);
    HoldBetween(Room.flReflectionsDelay, 0.0f, 0.3f);
    HoldBetween(Room.lReverb, -10000, 2000);
    HoldBetween(Room.flReverbDelay, 0.0f, 0.1f);
    HoldBetween(Room.flHFReference, 1000.0f, 20000.0f);
    HoldBetween(Room.flRoomRolloffFactor, 0.0f, 10.0f);
    #undef HoldBetween

    const HRESULT Result = Set->Methods->Set(Set, &Eax3ListenerId, EaxListenerAllParameters,
                                             NULL, 0, &Room, sizeof Room);
    Set->Methods->Release(Set);
    EaxGeneration = ReverbGeneration;
    if (SUCCEEDED(Result)) {
        EaxReverbPut = 1;
        Log("[audio] reverb \"%s\" set through EAX 3.0 (level %ld, decay %.2f s): ok",
            ReverbPresets[ReverbPreset].Name, (long)Room.lRoom, Room.flDecayTime);
    } else {
        Log("[audio] reverb refused by EAX 3.0: %s", ResultName(Result));
    }
}

/* ================================================================ SetFX (the reverb) */
static volatile LONG EffectsAsked, EffectsFailed;
/* What SetFX wrote into the results array, one word per effect. DirectSound fills this whether the call
   succeeded or not, and on failure it is the only thing that says which effect went wrong and why. */
static const char *EffectResultName(DWORD Code) {
    switch (Code) {
        case DSFXR_PRESENT:     return "present";
        case DSFXR_LOCHARDWARE: return "placed in hardware";
        case DSFXR_LOCSOFTWARE: return "placed in software";
        case DSFXR_UNALLOCATED: return "not allocated";
        case DSFXR_FAILED:      return "failed";
        case DSFXR_UNKNOWN:     return "unknown effect";
        case DSFXR_SENDLOOP:    return "send loop";
        default:                return "?";
    }
}

static volatile LONG EngineAskedHardware;
static HRESULT WINAPI SetFX(IDirectSoundBuffer8 *Self, DWORD Count, LPDSEFFECTDESC Effects, LPDWORD Results) {
    DSEFFECTDESC Carried[4];
    DWORD OwnResults[4] = { 0, 0, 0, 0 };
    const DWORD Room = sizeof Carried / sizeof *Carried;
    DWORD AskedFlags = 0xFFFFFFFF;
    /* The game may pass no results array. Lend it ours when it fits, so a failure can be read rather than
       guessed at; the game never sees it, because it did not ask for one. */
    LPDWORD Written = Results ? Results : (Count && Count <= Room ? OwnResults : NULL);
    if (!InsideOurSetFX) {
        InterlockedIncrement(&EngineSetFXCalls);
        PrepareEffects();
        if (Effects && Count && Count <= Room) {
            int Changed = 0, Swapped = 0;
            AskedFlags = Effects[0].dwFlags;
            for (DWORD Index = 0; Index < Count; ++Index) {
                Carried[Index] = Effects[Index];
                /* The one effect this Windows lacks, swapped for one that is here ([Audio] CarryEngineReverb). */
                if (CarryEngineReverb && IsEqualGUID(&Effects[Index].guidDSFXClass, &I3DL2ReverbId)) {
                    Carried[Index].guidDSFXClass = WavesReverbId;
                    Swapped = 1; Changed = 1;
                }
                /* An effect asked for in hardware, on a Windows that has had no hardware effects since Vista.
                 * DirectSound answers DSERR_FXUNAVAILABLE for an effect "expected in hardware and found in
                 * software" and places nothing, so a game written for the hardware era loses its reverb on
                 * every call. Asking for the same effect in software is the same effect, in the only place it
                 * can be. The buffers already carry DSBCAPS_CTRLFX and sit in software, so there is nowhere
                 * else for it to go. */
                if (Carried[Index].dwFlags & DSFX_LOCHARDWARE) {
                    Carried[Index].dwFlags = (Carried[Index].dwFlags & ~(DWORD)DSFX_LOCHARDWARE) | DSFX_LOCSOFTWARE;
                    Changed = 1;
                    InterlockedIncrement(&EngineAskedHardware);
                }
            }
            if (Changed) Effects = Carried;
            if (Swapped) InterlockedIncrement(&EngineReverbsCarried);
        }
    }
    const HRESULT Result = OriginalSetFX(Self, Count, Effects, Written);
    InterlockedIncrement(&EffectsAsked);
    if (FAILED(Result)) InterlockedIncrement(&EffectsFailed);
    if (LogAudio && LogRoom(AudioLogLines, MaximumAudioLogLines)) {
        const char *What = "effect";
        if (Count == 1 && Effects && IsEqualGUID(&Effects[0].guidDSFXClass, &I3DL2ReverbId)) What = "I3DL2 reverb";
        char Asked[48] = "", Placed[48] = "";
        if (AskedFlags != 0xFFFFFFFF)
            snprintf(Asked, sizeof Asked, ", asked in %s",
                     AskedFlags & DSFX_LOCHARDWARE ? "hardware (sent to software instead)" :
                     AskedFlags & DSFX_LOCSOFTWARE ? "software" : "no particular place");
        if (Written && Count) snprintf(Placed, sizeof Placed, ", %s", EffectResultName(Written[0]));
        Log("[audio] SetFX %s on buffer %p asked by %s: %s%s%s", What, (void *)Self,
            InsideOurSetFX ? "RiotFix" : "THE GAME ITSELF", ResultName(Result), Asked, Placed);
    }
    return Result;
}

/* ================================================================ What each sound does
   [Debug] LogAudio also follows what happens to a buffer after it is made: the game asking it to play, and the
   volume it asks for. A sound that is made and never played, or played at silence, is a different fault from a
   sound that is never made. */

typedef HRESULT (WINAPI *PlayFunctionType)(IDirectSoundBuffer *, DWORD, DWORD, DWORD);
typedef HRESULT (WINAPI *SetVolumeFunctionType)(IDirectSoundBuffer *, LONG);
static PlayFunctionType OriginalPlay;
static SetVolumeFunctionType OriginalSetVolume;
static volatile LONG PlaysAsked, PlaysFailed, PlayLogLines;

/* A sound that plays some of the time and not others, while every call reports success, points at the engine's own
 * handling of its voices rather than at DirectSound. Two things are counted here to tell those apart: a buffer told to
 * play while it is already playing (the engine reusing a voice that has not finished, which cuts the first sound
 * short), and a buffer told to stop before its sound could have ended (a voice taken away for something else). */
/* A 3D sound the engine never placed ([Audio] FixUnplacedSounds).
 *
 * The engine puts a minimum distance of 1e9 on every sound it positions, which switches DirectSound's own
 * attenuation off and leaves the volume the engine worked out alone. One path does not: the sound a cutscene
 * sequence plays is created with 3D control, never given a position or a distance, and so keeps DirectSound's
 * defaults - the origin, and a minimum distance of 1 unit. These levels put the listener around a million units
 * from the origin, so that one unit attenuates the sound by something like 123 dB. It plays, it reports ok, and
 * nobody hears it. Measured: of 61 3D sounds played in one session, exactly one came out this way, and it was the
 * cutscene sound the player reported missing.
 *
 * A sound with no position is not meant to be anywhere, so the fix is to turn 3D off for it rather than to invent
 * a distance: DS3DMODE_DISABLE plays it at the volume the engine asked for, with no attenuation and no panning
 * towards an origin it was never meant to sit at.
 *
 * The mode belongs to the buffer, not to the sound, and the engine keeps a pool of buffers and hands the same one
 * to a different sound later - which is what the voice counts are counting. So a mode written once and left there
 * follows the buffer into every sound that reuses it: the first build of this did exactly that, fixed the cutscene
 * sound, and took the position away from the sounds that came after it through the same buffer. The mode is
 * therefore decided from the buffer's state at every play and never left behind: turned off for a sound with no
 * position, and turned back on for a sound that has one, undoing a previous play's decision on that same buffer. */
static int FixUnplacedSounds;
static volatile LONG UnplacedSounds, ReplacedSounds;

static void CheckPlacement(IDirectSoundBuffer *Buffer, DWORD Flags) {
    if ((Flags && !(Flags & DSBCAPS_CTRL3D)) || (!FixUnplacedSounds && !LogAllSounds)) return;
    IDirectSound3DBuffer *Placed = NULL;
    if (FAILED(IDirectSoundBuffer_QueryInterface(Buffer, &PlacedBufferId, (void **)&Placed)) || !Placed) return;
    D3DVALUE Minimum = 0.0f, Maximum = 0.0f; DWORD Mode = 0; D3DVECTOR Place = { 0, 0, 0 };
    IDirectSound3DBuffer_GetMinDistance(Placed, &Minimum);
    IDirectSound3DBuffer_GetMaxDistance(Placed, &Maximum);
    IDirectSound3DBuffer_GetMode(Placed, &Mode);
    IDirectSound3DBuffer_GetPosition(Placed, &Place);
    /* Never placed: the DirectSound default minimum distance, and still at the origin. */
    const int Stray = Minimum <= 1.0f && Place.x == 0.0f && Place.y == 0.0f && Place.z == 0.0f;
    if (FixUnplacedSounds && Stray && Mode != DS3DMODE_DISABLE) {
        const HRESULT Result = IDirectSound3DBuffer_SetMode(Placed, DS3DMODE_DISABLE, DS3D_IMMEDIATE);
        if (SUCCEEDED(Result)) InterlockedIncrement(&UnplacedSounds);
        if (LogAllSounds)
            Log("[audio]   3d: never placed (origin, minimum distance 1) - 3d turned off so it can be heard: %s",
                ResultName(Result));
    } else if (FixUnplacedSounds && !Stray && Mode == DS3DMODE_DISABLE) {
        /* This buffer is placed in the world again, and the 3D we turned off was for the sound that had it before.
         * Without this the sound would keep playing as if it had no position, wherever the player stood. */
        const HRESULT Result = IDirectSound3DBuffer_SetMode(Placed, DS3DMODE_NORMAL, DS3D_IMMEDIATE);
        if (SUCCEEDED(Result)) InterlockedIncrement(&ReplacedSounds);
        if (LogAllSounds)
            Log("[audio]   3d: placed again at %.0f %.0f %.0f - 3d turned back on: %s", (double)Place.x,
                (double)Place.y, (double)Place.z, ResultName(Result));
    } else if (LogAllSounds) {
        static int Lines;
        if (Lines++ <= 60)
            Log("[audio]   3d: at %.0f %.0f %.0f, heard from %.1f to %.1f units, mode %lu%s", (double)Place.x,
                (double)Place.y, (double)Place.z, (double)Minimum, (double)Maximum, (unsigned long)Mode,
                Stray ? "  <- never placed: DirectSound's default, inaudible this far out" : "");
    }
    IDirectSound3DBuffer_Release(Placed);
}
static HRESULT WINAPI Play(IDirectSoundBuffer *Self, DWORD Reserved, DWORD Priority, DWORD Flags) {
    DWORD Status = 0;
    if (SUCCEEDED(IDirectSoundBuffer_GetStatus(Self, &Status)) && (Status & DSBSTATUS_PLAYING)) {
        InterlockedIncrement(&PlaysOverPlaying);
        if (LogAudio && (LogAllSounds || PlaysOverPlaying <= 10)) {
            const KnownBuffer *Known = FindBuffer(Self);
            Log("[audio] play on %p while it was still playing (%lu ms sound cut short)", (void *)Self,
                Known ? (unsigned long)Known->Milliseconds : 0UL);
        }
    }
    KnownBuffer *Mine = FindBufferToChange(Self);
    if (Mine) { Mine->Looping = (Flags & DSBPLAY_LOOPING) != 0; Mine->PlayedAt = GetTickCount(); }
    /* Before it starts, not after: by now the engine has finished setting the buffer up, so a 3D sound still sitting
     * at DirectSound's defaults is one it never meant to place, and it would be inaudible if it started that way. */
    CheckPlacement(Self, Mine ? Mine->Flags : 0);
    const HRESULT Result = OriginalPlay(Self, Reserved, Priority, Flags);
    InterlockedIncrement(&PlaysAsked);
    if (Watched(Self)) {
        LONG Volume = 0;
        IDirectSoundBuffer_GetVolume(Self, &Volume);
        Log("[watch] the sound was told to play: buffer %p, volume %ld, %s", (void *)Self, Volume, ResultName(Result));
    }
    if (FAILED(Result)) InterlockedIncrement(&PlaysFailed);
    if (LogAudio && (FAILED(Result) || LogRoom(PlayLogLines, 120))) {
        const KnownBuffer *Known = FindBuffer(Self);
        LONG Volume = 0;
        IDirectSoundBuffer_GetVolume(Self, &Volume);
        Log("[audio] play %p (%s%lu ms, flags %08lx), volume %ld: %s", (void *)Self, Known ? "" : "unknown, ",
            Known ? (unsigned long)Known->Milliseconds : 0UL, Known ? (unsigned long)Known->Flags : 0UL, Volume,
            ResultName(Result));
        /* The chain of callers, so the engine's own "play this sound" method can be found and watched in turn:
           a sound the engine decides not to play never reaches here, and leaves no line at all. */
        if (LogAllSounds) {
            DWORD Chain[4]; CallChain(&Self, Chain, 4);
            char Names[4][64];
            for (int Index = 0; Index < 4; Index++) {
                Names[Index][0] = 0;
                if (Chain[Index]) AddressName(Chain[Index], Names[Index], sizeof Names[Index]);
            }
            Log("[audio]   asked by %s <- %s <- %s <- %s", Names[0][0] ? Names[0] : "-", Names[1][0] ? Names[1] : "-",
                Names[2][0] ? Names[2] : "-", Names[3][0] ? Names[3] : "-");
        }
    }
    return Result;
}
/* [Audio] VolumeFloor: a test, not a fix. DirectSound is given distances of a thousand million units, so it attenuates
 * nothing and the engine works out loudness itself, writing -10000 (silence) for anything it places too far away. Raising
 * that floor says whether a sound the player cannot hear was silenced on purpose or never reached the output at all. */
static HRESULT WINAPI SetVolume(IDirectSoundBuffer *Self, LONG Volume) {
    if (VolumeFloor && Volume < VolumeFloor) {
        InterlockedIncrement(&VolumesRaised);
        Volume = VolumeFloor;
    }
    if (Watched(Self)) Log("[watch] the sound's volume was set to %ld", Volume);
    const HRESULT Result = OriginalSetVolume(Self, Volume);
    if (LogAudio && Volume <= -3000 && (LogAllSounds || PlayLogLines <= 120)) {
        const KnownBuffer *Known = FindBuffer(Self);
        if (Known) Log("[audio] volume %ld on %p (%lu ms, flags %08lx)", Volume, (void *)Self,
                       (unsigned long)Known->Milliseconds, (unsigned long)Known->Flags);
    }
    return Result;
}

/* ================================================================ 3D placing
   A 3D buffer is placed through IDirectSound3DBuffer: the distance at which it starts to fade (minimum) and the one
   where it is gone (maximum), and its position. DirectSound attenuates by those distances on its own, on top of
   whatever volume the engine sets, so distances in the wrong units leave a sound inaudible even while it plays. */
typedef HRESULT (WINAPI *Set3DDistanceFunctionType)(IDirectSound3DBuffer *, D3DVALUE, DWORD);
typedef HRESULT (WINAPI *Set3DPositionFunctionType)(IDirectSound3DBuffer *, D3DVALUE, D3DVALUE, D3DVALUE, DWORD);
typedef HRESULT (WINAPI *Set3DAllFunctionType)(IDirectSound3DBuffer *, LPCDS3DBUFFER, DWORD);
static Set3DDistanceFunctionType OriginalSetMinDistance, OriginalSetMaxDistance;
static Set3DPositionFunctionType OriginalSet3DPosition;
static Set3DAllFunctionType OriginalSet3DAll;
static volatile LONG Place3DLogLines;
static int Placing3DHooked;

static HRESULT WINAPI SetMinDistance(IDirectSound3DBuffer *Self, D3DVALUE Distance, DWORD Apply) {
    if (LogAudio && LogRoom(Place3DLogLines, 60)) Log("[audio] 3d minimum distance %.2f", Distance);
    return OriginalSetMinDistance(Self, Distance, Apply);
}
static HRESULT WINAPI SetMaxDistance(IDirectSound3DBuffer *Self, D3DVALUE Distance, DWORD Apply) {
    if (LogAudio && LogRoom(Place3DLogLines, 60)) Log("[audio] 3d maximum distance %.2f", Distance);
    return OriginalSetMaxDistance(Self, Distance, Apply);
}
static HRESULT WINAPI Set3DPosition(IDirectSound3DBuffer *Self, D3DVALUE X, D3DVALUE Y, D3DVALUE Z, DWORD Apply) {
    if (LogAudio && InterlockedIncrement(&Place3DLogLines) <= 60) Log("[audio] 3d position %.2f, %.2f, %.2f", X, Y, Z);
    return OriginalSet3DPosition(Self, X, Y, Z, Apply);
}
static HRESULT WINAPI Set3DAll(IDirectSound3DBuffer *Self, LPCDS3DBUFFER Parameters, DWORD Apply) {
    if (LogAudio && Parameters && InterlockedIncrement(&Place3DLogLines) <= 60)
        Log("[audio] 3d placed at %.2f, %.2f, %.2f, fades from %.2f to %.2f, mode %lu", Parameters->vPosition.x,
            Parameters->vPosition.y, Parameters->vPosition.z, Parameters->flMinDistance, Parameters->flMaxDistance,
            (unsigned long)Parameters->dwMode);
    return OriginalSet3DAll(Self, Parameters, Apply);
}
static void Watch3DPlacing(IDirectSoundBuffer *Buffer) {
    if (Placing3DHooked) return;
    IDirectSound3DBuffer *Placing = NULL;
    if (FAILED(IDirectSoundBuffer_QueryInterface(Buffer, &Buffer3DId, (void **)&Placing)) || !Placing) return;
    DWORD *Vtable = *(DWORD **)Placing;
    OriginalSet3DAll = (Set3DAllFunctionType)Vtable[12];
    OriginalSetMaxDistance = (Set3DDistanceFunctionType)Vtable[16];
    OriginalSetMinDistance = (Set3DDistanceFunctionType)Vtable[17];
    OriginalSet3DPosition = (Set3DPositionFunctionType)Vtable[19];
    Placing3DHooked = WriteCode((DWORD)&Vtable[12], &(DWORD){ (DWORD)Set3DAll }, 4) &&
                      WriteCode((DWORD)&Vtable[16], &(DWORD){ (DWORD)SetMaxDistance }, 4) &&
                      WriteCode((DWORD)&Vtable[17], &(DWORD){ (DWORD)SetMinDistance }, 4) &&
                      WriteCode((DWORD)&Vtable[19], &(DWORD){ (DWORD)Set3DPosition }, 4);
    Log("Audio: 3D placing watched (distances and positions): %s", Placing3DHooked ? "ok" : "FAILED");
    IDirectSound3DBuffer_Release(Placing);
}

/* ================================================================ Buffers */
/* Says why the engine's own effect path is or is not reachable. Read-only: nothing here changes what the game does.
 * The six categories are reported by index because their names are not in the executable. */
/* The level library keeps the engine's sound system at Suffering.rfl+0x17BF78 (it is handed over at start-up from a
 * table of subsystems). Its update of an ambient sound, at rfl+0x12BDF0, asks that object three questions and stops
 * the sound when the answers line up; the questions are vtable entries 0x10C, 0x11C and 0x114, and the stop is 0xC8.
 * Naming them here turns four unknown offsets into four addresses that can be read in Ghidra. */
/* The engine's own distance cull, watched because it never shows up in a DirectSound call: a sound refused here
   is simply never created. Logged whenever it changes, which is the only way to learn what it holds. */
static void ReportAudibleDistance(void) {
    if (!LogAudio || !A->AudibleDistanceSquared) return;
    static float Last = -1.0f; static int Logged;
    const float Now = *(const float *)A->AudibleDistanceSquared;
    if (Now == Last || Logged > 20) return;
    Last = Now; Logged++;
    Log("[audio] the engine plays a sound only within %g units (squared limit %g)%s",
        Now > 0.0f ? sqrt((double)Now) : 0.0, Now, Now > 0.0f ? "" : " - nothing but sounds marked always audible");
}
static void WatchPlayRequests(const DWORD *Vtable);
static void ReportSoundSystem(void) {
    const HMODULE Rfl = RflModule();
    if (!Rfl) { Log("Audio: the level library is not loaded, the sound system was not read"); return; }
    const DWORD *Slot = (const DWORD *)((DWORD)Rfl + A->SoundSystemInLevelLibrary);
    if (IsBadReadPtr(Slot, 4) || !*Slot) { Log("Audio: the engine's sound system is not up yet"); return; }
    /* Whatever stands at that offset is only a pointer if this is the right offset for this game: check it before
     * following it, or a wrong address turns a report into a crash. */
    if (IsBadReadPtr((const void *)*Slot, 4)) { Log("Audio: the engine's sound system could not be read"); return; }
    const DWORD *Vtable = *(const DWORD **)*Slot;
    if (IsBadReadPtr(Vtable, 0x120)) { Log("Audio: the sound system's methods could not be read"); return; }
    static const struct { DWORD Offset; const char *What; } Methods[] = {
        { 0xC8,  "stops a sound" }, { 0x10C, "first question" },
        { 0x114, "third question" }, { 0x11C, "second question" },
    };
    for (int Index = 0; Index < 4; ++Index) {
        char Name[MAX_PATH + 32];
        AddressName(Vtable[Methods[Index].Offset / 4], Name, sizeof Name);
        Log("Audio: the sound system's %s is %s", Methods[Index].What, Name);
    }
    WatchPlayRequests(Vtable);
}

/* ================================================================ What the engine asks for ([Debug] LogAllSounds)
 * The log above holds what reached DirectSound. A sound the engine decides not to play never gets there and leaves
 * no line at all, which is exactly the case worth catching. Three methods of the sound system lead to DirectSound,
 * found by logging the chain of callers of every Play in a running game: +0xA0 (0x441410), +0xA4 (0x441B00) and
 * +0xA8 (0x441A80). Hooking them writes a line for every request, so a request with no play after it stands out.
 *
 * They do not take the same arguments, and passing the wrong number destroys the stack on return: the first build
 * of this hook called +0xA0 with six and the game died at EIP 0. The counts below are measured from the ret of each
 * function inside its real bounds (0x441410 ends at 0x4416EF with ret 0x10, so four; the other two end at 0x441B6F
 * and 0x441AEF with ret 0x18, so six). The same measurement gives the already known stop method two arguments,
 * which its disassembly confirms. Only with LogAllSounds on, and only to watch: every call is passed through. */
typedef int (ThisCall *FourArgumentFunctionType)(void *, DWORD, DWORD, DWORD, DWORD);
typedef int (ThisCall *SixArgumentFunctionType)(void *, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
static FourArgumentFunctionType OriginalFirstRequest;
static SixArgumentFunctionType OriginalSecondRequest, OriginalThirdRequest;
static volatile LONG PlayRequests;

static void LogPlayRequest(const char *Which, int Result, const DWORD *Arguments, int Count) {
    char Text[128]; int Place = 0;
    for (int Index = 0; Index < Count && Place < (int)sizeof Text - 10; Index++)
        Place += sprintf(Text + Place, Index ? " %08lx" : "%08lx", (unsigned long)Arguments[Index]);
    Log("[audio] request %ld: the engine asked %s for a sound (%s) -> %d",
        InterlockedIncrement(&PlayRequests), Which, Text, Result);
}
/* Every request carries a pointer as its second argument, and the refusals cluster on it perfectly: across two
   sessions, 79 and 60 distinct values, not one of them both played and refused. So what the engine refuses is
   decided by whatever that points at, not by the sound. Some of those values are stack addresses and some are on
   the heap, so it is a small structure rather than a lasting object - the first words of it are dumped here, once
   per value, to find the field that separates a pointer that always plays from one that never does. */
#define MaximumEmitters 96
static DWORD SeenEmitters[MaximumEmitters]; static int SeenEmitterCount;
static void ReportEmitter(DWORD Emitter, DWORD Sound, int Played) {
    if (!Emitter || IsBadReadPtr((const void *)Emitter, 0x20)) return;
    for (int Index = 0; Index < SeenEmitterCount; Index++)
        if (SeenEmitters[Index] == Emitter) return;
    if (SeenEmitterCount >= MaximumEmitters) return;
    SeenEmitters[SeenEmitterCount++] = Emitter;
    const DWORD *Words = (const DWORD *)Emitter;
    const float *Numbers = (const float *)Emitter;
    char Text[256]; int Place = 0;
    for (int Index = 0; Index < 8 && Place < (int)sizeof Text - 24; Index++)
        Place += sprintf(Text + Place, "%08lx ", (unsigned long)Words[Index]);
    Place += sprintf(Text + Place, "| as numbers %.1f %.1f %.1f", (double)Numbers[0], (double)Numbers[1], (double)Numbers[2]);
    Log("[audio] source %08lx %s (sound %08lx): %s", (unsigned long)Emitter, Played ? "plays" : "is REFUSED",
        (unsigned long)Sound, Text);
}
/* Which of the play method's guards turned a request down. Five of its seven refusal exits test something that can
   be read from outside: the sound system itself, a field of it, the sound asked for, a flag of the sound system and
   a count of it. The other two are inside the method - no free voice, and a distance past the audible limit - so a
   refusal that none of these explains belongs to one of those. Read-only. */
static void ReportRefusal(void *Self, DWORD Sound) {
    static int Lines;
    if (Lines++ > 40 || !A->RefusalFlag) return;
    const DWORD System = *(const DWORD *)A->SoundSystemPointer;
    const char *Why = "neither: no free voice, or past the audible distance";
    if (!System) Why = "the sound system is not there";
    else if (!*(const DWORD *)(System + 0x2C)) Why = "the sound system's +0x2C is zero";
    else if (IsBadReadPtr(Self, A->RefusalFlag + 4)) Why = "the sound system could not be read";
    else if (*(const DWORD *)((char *)Self + A->RefusalFlag)) Why = "the sound system's stop-everything flag is set";
    else if ((int)*(const DWORD *)((char *)Self + A->RefusalCount) <= 0) Why = "the sound system has no voices";
    Log("[audio] sound %08lx was refused: %s (flag %08lx, count %ld, audible limit %g)", (unsigned long)Sound, Why,
        (unsigned long)*(const DWORD *)((char *)Self + A->RefusalFlag),
        (long)*(const DWORD *)((char *)Self + A->RefusalCount),
        (double)*(const float *)A->AudibleDistanceSquared);
}
/* [Audio] AudibleDistance: the engine refuses to play a sound whose squared distance is past the limit it keeps at
 * a global of its own, unless the sound is marked always audible. The limit is not written anywhere in either
 * binary and comes out as 102400 units in play. Raised here only for the length of the call that tests it, so
 * nothing else in the engine sees a different world. 0 = the game's own limit. */
static float AudibleDistance;
static int ThisCall FirstRequest(void *Self, DWORD A1, DWORD A2, DWORD A3, DWORD A4) {
    float *const Limit = A->AudibleDistanceSquared ? (float *)A->AudibleDistanceSquared : NULL;
    const float Kept = Limit ? *Limit : 0.0f;
    if (Limit && AudibleDistance > 0.0f) *Limit = AudibleDistance * AudibleDistance;
    const int Result = OriginalFirstRequest(Self, A1, A2, A3, A4);
    if (Limit && AudibleDistance > 0.0f) *Limit = Kept;
    const DWORD Arguments[4] = { A1, A2, A3, A4 };
    LogPlayRequest("+0xA0", Result, Arguments, 4);
    if (A1) ReportEmitter(A2, A1, Result != 0);
    if (!Result && A1 && LogAllSounds) ReportRefusal(Self, A1);
    return Result;
}
static int ThisCall SecondRequest(void *Self, DWORD A1, DWORD A2, DWORD A3, DWORD A4, DWORD A5, DWORD A6) {
    const int Result = OriginalSecondRequest(Self, A1, A2, A3, A4, A5, A6);
    const DWORD Arguments[6] = { A1, A2, A3, A4, A5, A6 };
    LogPlayRequest("+0xA4", Result, Arguments, 6);
    return Result;
}
static int ThisCall ThirdRequest(void *Self, DWORD A1, DWORD A2, DWORD A3, DWORD A4, DWORD A5, DWORD A6) {
    const int Result = OriginalThirdRequest(Self, A1, A2, A3, A4, A5, A6);
    const DWORD Arguments[6] = { A1, A2, A3, A4, A5, A6 };
    LogPlayRequest("+0xA8", Result, Arguments, 6);
    return Result;
}
static void WatchPlayRequests(const DWORD *Vtable) {
    if (!LogAllSounds) return;
    if (!A->PlayRequestSlots[0]) {
        Log("Audio: the engine's play requests are not watched (the methods are not known for this game)");
        return;
    }
    static void *const Ours[3] = { (void *)FirstRequest, (void *)SecondRequest, (void *)ThirdRequest };
    void **const Keep[3] = { (void **)&OriginalFirstRequest, (void **)&OriginalSecondRequest,
                             (void **)&OriginalThirdRequest };
    int Done = 0;
    for (int Index = 0; Index < 3; ++Index) {
        const DWORD Offset = A->PlayRequestSlots[Index];
        if (!Offset) continue;
        /* The method must still be the one these argument counts were measured from, or the hook would call it
           with the wrong number and take the game down with it. */
        if (Vtable[Offset / 4] != A->PlayRequestMethods[Index]) {
            Log("Audio: play request +0x%02lX holds %p, not the method it was measured from: not watched",
                (unsigned long)Offset, (void *)Vtable[Offset / 4]);
            continue;
        }
        *Keep[Index] = (void *)Vtable[Offset / 4];
        Done += WriteCode((DWORD)&Vtable[Offset / 4], &Ours[Index], 4);
    }
    Log("Audio: the engine's play requests watched: %d/3 methods", Done);
}

static void ReportEffectGate(void) {
    const DWORD System = *(const DWORD *)A->SoundSystemPointer;
    if (!System) { Log("Audio: the engine's sound system is not up yet, effect gate not read"); return; }
    const BYTE Flags = *(const BYTE *)(System + A->SoundSystemFlag);
    Log("Audio: the engine's effect gate: sound system flags %02X, so effects are %s", Flags,
        (Flags & 2) ? "allowed" : "REFUSED for every sound");
    /* Each category record is 0x1C bytes. The first field is the one the effect gate reads; the rest are printed raw
     * because what they hold is not known yet, and a limit on how many sounds of a category may play at once would
     * show up here. The categories are the ones the editor names: Effect, Ambient, Music, GUI, Dialog and so on. */
    for (int Index = 0; Index < 6; ++Index) {
        const DWORD *Record = (const DWORD *)(A->CategoryTable + (DWORD)Index * 0x1C);
        Log("Audio: category %d record: %08lx %08lx %08lx %08lx %08lx %08lx %08lx", Index,
            (unsigned long)Record[0], (unsigned long)Record[1], (unsigned long)Record[2], (unsigned long)Record[3],
            (unsigned long)Record[4], (unsigned long)Record[5], (unsigned long)Record[6]);
    }
}

static volatile LONG BuffersMade, BuffersWithEffects, BuffersFailed, BuffersMovedToSoftware;
static int SetFXHooked;
static HRESULT WINAPI CreateSoundBuffer(IDirectSound8 *Self, LPCDSBUFFERDESC Description, LPDIRECTSOUNDBUFFER *Buffer,
                                        LPUNKNOWN Outer) {
    DSBUFFERDESC Wanted;
    int WantReverb = 0, Padded = 0;
    /* [Audio] SoftwareBuffers: the engine asks for DSBCAPS_LOCHARDWARE on every 3D sound (its flags are
       0x20094 or, with an effect, 0x20290). Windows has had no hardware mixing since Vista, so those buffers are
       never created and the sound is silently lost, unless a DirectSound wrapper pretends hardware is there.
       Asking for a software buffer instead gives the same sound from the system's own mixer. */
    if (Description && Description->dwSize >= sizeof Wanted - 4 && Description->dwSize <= sizeof Wanted &&
        !(Description->dwFlags & DSBCAPS_PRIMARYBUFFER)) {
        const int Hardware = (Description->dwFlags & DSBCAPS_LOCHARDWARE) != 0;
        WantReverb = Reverb && !ReverbUnavailable && !EaxListenerSettable &&
                     !(Description->dwFlags & DSBCAPS_CTRLFX) && TakesReverb(Description);
        if ((SoftwareBuffers && Hardware) || WantReverb) {
            memset(&Wanted, 0, sizeof Wanted);
            memcpy(&Wanted, Description, Description->dwSize);
            if (SoftwareBuffers && Hardware) {
                Wanted.dwFlags = (Wanted.dwFlags & ~(DWORD)DSBCAPS_LOCHARDWARE) | DSBCAPS_LOCSOFTWARE;
                InterlockedIncrement(&BuffersMovedToSoftware);
            }
            if (WantReverb) {
                Wanted.dwFlags = (Wanted.dwFlags & ~(DWORD)DSBCAPS_LOCHARDWARE) | DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLFX;
                const WAVEFORMATEX *Format = Wanted.lpwfxFormat;
                if (Format && BufferMilliseconds(&Wanted) < ReverbMinimumMilliseconds) {
                    const DWORD Least = Format->nAvgBytesPerSec * ReverbMinimumMilliseconds / 1000;
                    if (Least > Wanted.dwBufferBytes) { Wanted.dwBufferBytes = Least; Padded = 1; }
                }
            }
            Description = &Wanted;
        }
    }
    const HRESULT Result = OriginalCreateSoundBuffer(Self, Description, Buffer, Outer);
    if (FAILED(Result) || !Description) {
        InterlockedIncrement(&BuffersFailed);
        if (BuffersFailed <= 20)
            Log("[audio] CreateSoundBuffer FAILED (flags %08lx): %s", Description ? (unsigned long)Description->dwFlags : 0UL,
                ResultName(Result));
        return Result;
    }
    if (!InterlockedIncrement(&BuffersMade) || BuffersMade == 1) {
        Log("Audio: the engine's 3D sound setting is %s", (*(const DWORD *)A->ThreeDimensionalFlag & 1) ? "on" : "off");
        ReportEffectGate();
        ReportSoundSystem();
    }
    if (Description->dwFlags & DSBCAPS_CTRLFX) InterlockedIncrement(&BuffersWithEffects);

    if (LogAudio && Buffer && *Buffer && (Description->dwFlags & DSBCAPS_CTRL3D)) Watch3DPlacing(*Buffer);
    if (Buffer && *Buffer) {
        const WAVEFORMATEX *Format = Description->lpwfxFormat;
        RememberBuffer(*Buffer, Description->dwFlags,
                       Format && Format->nAvgBytesPerSec ? Description->dwBufferBytes * 1000 / Format->nAvgBytesPerSec : 0,
                       Description->dwBufferBytes);
        if (WatchedSize(Description->dwBufferBytes))
            Log("[watch] the sound was made: buffer %p, %lu bytes, flags %08lx", (void *)*Buffer,
                (unsigned long)Description->dwBufferBytes, (unsigned long)Description->dwFlags);
    }
    if (WantReverb && Buffer && *Buffer) {
        if (Padded) SilenceBuffer(*Buffer, Description->lpwfxFormat);
        PutReverb(*Buffer);
    }

    /* The buffer's own vtable holds SetFX (slot 21 of IDirectSoundBuffer8); every buffer shares it. */
    if (!SetFXHooked && Buffer && *Buffer) {
        IDirectSoundBuffer8 *Eight = NULL;
        if (SUCCEEDED(IDirectSoundBuffer_QueryInterface(*Buffer, &BufferEightId, (void **)&Eight)) && Eight) {
            DWORD *Vtable = *(DWORD **)Eight;
            OriginalSetFX = (SetFXFunctionType)Vtable[21];
            OriginalPlay = (PlayFunctionType)Vtable[12];
            OriginalGetObjectInPath = (GetObjectInPathFunctionType)Vtable[23];
            OriginalStop = (StopFunctionType)Vtable[18];
            OriginalSetVolume = (SetVolumeFunctionType)Vtable[15];
            SetFXHooked = WriteCode((DWORD)&Vtable[21], &(DWORD){ (DWORD)SetFX }, 4) &&
                          WriteCode((DWORD)&Vtable[12], &(DWORD){ (DWORD)Play }, 4) &&
                          WriteCode((DWORD)&Vtable[15], &(DWORD){ (DWORD)SetVolume }, 4) &&
                          WriteCode((DWORD)&Vtable[23], &(DWORD){ (DWORD)GetObjectInPath }, 4) &&
                          WriteCode((DWORD)&Vtable[18], &(DWORD){ (DWORD)Stop }, 4);
            Log("Audio: buffer methods watched (SetFX, Play, Stop, SetVolume, GetObjectInPath): %s",
                SetFXHooked ? "ok" : "FAILED");
            IDirectSoundBuffer8_Release(Eight);
        }
    }
    /* Asked of a 3D buffer, because that is where an EAX property set lives when there is one. */
    if (Buffer && *Buffer && (Description->dwFlags & DSBCAPS_PRIMARYBUFFER)) ProbeEaxListener(*Buffer);
    else if (Buffer && *Buffer && (Description->dwFlags & DSBCAPS_CTRL3D)) {
        ProbeEax(*Buffer);
        ApplyEaxReverb(*Buffer);
    }
    if (LogAudio && LogRoom(AudioLogLines, MaximumAudioLogLines)) {
        const WAVEFORMATEX *Format = Description->lpwfxFormat;
        const DWORD Bytes = Description->dwBufferBytes;
        const DWORD Milliseconds = Format && Format->nAvgBytesPerSec ? Bytes * 1000 / Format->nAvgBytesPerSec : 0;
        Log("[audio] buffer %lu bytes (%lu ms, %lu Hz, %d channels), flags %08lx%s%s%s%s%s", (unsigned long)Bytes,
            (unsigned long)Milliseconds, Format ? (unsigned long)Format->nSamplesPerSec : 0UL, Format ? Format->nChannels : 0,
            (unsigned long)Description->dwFlags,
            Description->dwFlags & DSBCAPS_CTRLFX ? " effects" : "",
            Description->dwFlags & DSBCAPS_CTRL3D ? " 3d" : "",
            Description->dwFlags & DSBCAPS_LOCHARDWARE ? " hardware" : "",
            Description->dwFlags & DSBCAPS_LOCSOFTWARE ? " software" : "",
            Description->dwFlags & DSBCAPS_LOCDEFER ? " deferred" : "");
    }
    return Result;
}

/* ================================================================ The device */
static int BufferHooked;
static void WatchDevice(IDirectSound8 *Device) {
    if (!Device || BufferHooked) return;
    DSCAPS Caps = { .dwSize = sizeof Caps };
    if (SUCCEEDED(IDirectSound8_GetCaps(Device, &Caps)))
        Log("Audio: DirectSound reports %lu hardware buffers (%lu free), %lu 3D hardware buffers",
            (unsigned long)Caps.dwMaxHwMixingAllBuffers, (unsigned long)Caps.dwFreeHwMixingAllBuffers,
            (unsigned long)Caps.dwMaxHw3DAllBuffers);
    DWORD *Vtable = *(DWORD **)Device;
    OriginalCreateSoundBuffer = (CreateSoundBufferFunctionType)Vtable[3];
    BufferHooked = WriteCode((DWORD)&Vtable[3], &(DWORD){ (DWORD)CreateSoundBuffer }, 4);
    Log("Audio: sound buffers watched (DirectSound vtable): %s", BufferHooked ? "ok" : "FAILED");
}
static HRESULT WINAPI Create8(LPCGUID Device, LPDIRECTSOUND8 *Out, LPUNKNOWN Outer) {
    const HRESULT Result = OriginalCreate8(Device, Out, Outer);
    if (SUCCEEDED(Result) && Out) WatchDevice(*Out);
    return Result;
}
static HRESULT WINAPI Create(LPCGUID Device, LPDIRECTSOUND8 *Out, LPUNKNOWN Outer) {
    const HRESULT Result = OriginalCreate(Device, Out, Outer);
    if (SUCCEEDED(Result) && Out) WatchDevice(*Out);
    return Result;
}

/* ================================================================ Apply */
void RiotAudioReport(void) {
    if (!A || !(BuffersMade | BuffersFailed)) return;
    Log("Audio this session: %ld sound buffers (%ld asked for effects, %ld moved to software), %ld failed to be made,"
        " %ld SetFX calls, %ld of them failed", BuffersMade, BuffersWithEffects, BuffersMovedToSoftware, BuffersFailed,
        EffectsAsked, EffectsFailed);
    Log("Audio: of those SetFX calls, %ld came from the game itself and %ld from RiotFix",
        EngineSetFXCalls, EffectsAsked - EngineSetFXCalls);
    if (EaxReverbPut) Log("Audio: the reverb was carried by the EAX 3.0 listener, not by DirectSound effects;"
                          " no sound had to be moved into software for it");
    else if (Reverb) Log("Audio: reverb put on %ld sounds, refused on %ld", ReverbsPut, ReverbsFailed);
    if (EngineReverbsCarried) Log("Audio: the game's own reverb carried to the Waves effect on %ld sounds",
                                  EngineReverbsCarried);
    if (PlaysAsked) Log("Audio: %ld sounds asked to play, %ld of those calls failed", PlaysAsked, PlaysFailed);
    if (PlaysAsked) Log("Audio: %ld of those were told to play while already playing; %ld playing sounds were"
                        " stopped, %ld of them one-shots (the rest were loops ending normally)",
                        PlaysOverPlaying, StopsWhilePlaying, OneShotsCutShort);
    if (OneShotsCutShort) Log("Audio: of those one-shots, %ld had finished (under %d ms left) and %ld were"
                              " genuinely cut off", StoppedNearlyDone, SoundFinishedMilliseconds, StoppedEarly);
    if (StopsAsked) Log("Audio: the engine asked the sound manager to stop %ld sounds by handle, %ld of them at"
                        " once with no fade at all, and %ld for a sound that had already gone",
                        StopsAsked, StopsAskedAtOnce, StopsAskedGone);
    if (ObjectStopsAsked) Log("Audio: and %ld by sound object, %ld of those forced, %ld for a sound that had"
                              " already gone", ObjectStopsAsked, ObjectStopsForced, ObjectStopsGone);
    RiotTimingOnFrame(MeasureFrame);
    if (VolumeFloor) Log("Audio: volume floor %ld raised %ld settings the engine had put below it",
                         VolumeFloor, VolumesRaised);
    if (UnplacedSounds) Log("Audio: %ld sounds were about to play in 3d without ever having been placed, and had"
                            " their 3d turned off so they could be heard; %ld later sounds got that same buffer with"
                            " a position of their own and had it turned back on", UnplacedSounds, ReplacedSounds);
}

static void WatchSoundOwners(void);         /* defined at the end of this file */
static void WatchSoundStopRequests(void);   /* defined at the end of this file */

void RiotAudioApply(void) {
    if (CurrentGame == GameUnknown) return;
    A = &Addresses[CurrentGame];
    LogAudio = IniInt("Debug", "LogAudio", 0);
    AudibleDistance = IniFloat("Audio", "AudibleDistance", 1000000.0f);
    if (AudibleDistance != 0.0f && !(AudibleDistance >= 1000.0f && AudibleDistance <= 1.0e9f)) {
        Log("Audio.AudibleDistance=%g is invalid (use 1000 to 1000000000, or 0)", AudibleDistance);
        AudibleDistance = 0.0f;
    }
    if (AudibleDistance > 0.0f) Log("Audio: a sound is played up to %g units away (the engine's own limit is about 102400)", AudibleDistance);
    FixUnplacedSounds = IniInt("Audio", "FixUnplacedSounds", 1);
    LogAllSounds = IniInt("Debug", "LogAllSounds", 0);
    if (LogAllSounds && !LogAudio) { LogAudio = 1; Log("Audio: LogAllSounds needs LogAudio: turned it on"); }
    if (LogAllSounds) Log("Audio: every sound of the session is logged (no line limits)");
    SoftwareBuffers = IniInt("Audio", "SoftwareBuffers", 1);
    CarryEngineReverb = IniInt("Audio", "CarryEngineReverb", 1);
    /* [Debug] WatchSound takes the sizes of up to eight sounds, separated by commas. */
    char Sizes[128]; GetPrivateProfileStringA("Debug", "WatchSound", "", Sizes, sizeof Sizes, IniPath);
    for (const char *At = Sizes; *At && WatchSoundCount < 8; ) {
        while (*At == ' ' || *At == ',') At++;
        if (*At < '0' || *At > '9') break;
        WatchSounds[WatchSoundCount++] = (DWORD)strtoul(At, (char **)&At, 10);
    }
    for (int Index = 0; Index < WatchSoundCount; ++Index)
        Log("Audio: watching the sound of %lu bytes", (unsigned long)WatchSounds[Index]);
    /* These two patch the game's own code, so they are applied once and are not part of the loop above - an earlier
     * edit of this file left them inside it, where an empty [Debug] WatchSound meant they never ran at all. */
    static int GateDone;
    if (!GateDone && IniTestInt("Audio", "EngineReverb", 0)) {
        GateDone = 1;
        if (!A->EffectGateClear) Log("Audio.EngineReverb: this game already asks for reverb by itself");
        else {
            static const BYTE SetTheFlag[] = { 0x83, 0xC8, 0x02 };   /* or eax, 2, in place of and eax, 0xFFFFFFFD */
            Log("Audio: the game may ask for reverb by itself (the path it disables in its own code): %s",
                WriteCode(A->EffectGateClear, SetTheFlag, sizeof SetTheFlag) ? "ok" : "FAILED");
        }
    }
    /* Read back what the game actually holds, so a log always says the truth rather than what the .ini asked for:
     * an option that silently fails to apply is worth more to find in one line here than in a refusal rate. */
    if (A->VoiceCount)
        Log("Audio: the game holds %lu voices and %lu placed in 3D", *(const DWORD *)A->VoiceCount,
            *(const DWORD *)A->ThreeDimensionalVoiceCount);
    static int VoicesDone;
    const int Voices = VoicesDone ? 0 : IniInt("Audio", "Voices", 64);
    const int SpaceVoices = VoicesDone ? 0 : IniInt("Audio", "ThreeDimensionalVoices", 32);
    if (Voices || SpaceVoices) {
        VoicesDone = 1;
        if (!A->VoiceCount) Log("Audio.Voices: not available in this game yet");
        else if (A->VoiceCount == A->ThreeDimensionalVoiceCount) {
            DWORD Both = (DWORD)(Voices > SpaceVoices ? Voices : SpaceVoices);
            if (!Both) Both = 32;
            Log("Audio: %lu voices, in all and placed in 3D alike (this game sets both from one number, and gives"
                " itself 32): %s", (unsigned long)Both,
                WriteCode(A->VoiceCount, &Both, 4) ? "ok" : "FAILED");
        } else {
            const DWORD All = Voices ? (DWORD)Voices : 20, Space = SpaceVoices ? (DWORD)SpaceVoices : 8;
            const int Done = WriteCode(A->VoiceCount, &All, 4) && WriteCode(A->ThreeDimensionalVoiceCount, &Space, 4);
            Log("Audio: voices %lu in all and %lu placed in 3D (the game gives itself 20 and 8): %s",
                (unsigned long)All, (unsigned long)Space, Done ? "ok" : "FAILED");
        }
        Log("Audio: the game now holds %lu voices and %lu placed in 3D", *(const DWORD *)A->VoiceCount,
            *(const DWORD *)A->ThreeDimensionalVoiceCount);
    }
    if (LogAudio) WatchSoundOwners();
    if (LogAudio) WatchSoundStopRequests();
    VolumeFloor = IniTestInt("Audio", "VolumeFloor", 0);
    if (VolumeFloor > 0) VolumeFloor = 0;
    if (VolumeFloor < -10000) VolumeFloor = -10000;
    RiotTimingOnFrame(MeasureFrame);
    if (VolumeFloor) Log("Audio: volume floor %ld (a test: sounds the engine silences are raised to it)", VolumeFloor);
    char Name[32]; GetPrivateProfileStringA("Audio", "Reverb", "off", Name, sizeof Name, IniPath);
    for (size_t Index = 0; Index < sizeof ReverbPresets / sizeof *ReverbPresets; Index++)
        if (!_stricmp(Name, ReverbPresets[Index].Name)) { Reverb = 1; ReverbPreset = (int)Index; ReverbSettings = ReverbPresets[Index].Settings; }
    if (!Reverb && _stricmp(Name, "off") && _stricmp(Name, "0"))
        Log("Audio.Reverb=%s is not one of the environments (see RiotFix.ini)", Name);
    if (Reverb) {
        char Which[16]; GetPrivateProfileStringA("Audio", "ReverbSounds", "3d", Which, sizeof Which, IniPath);
        ReverbEverySound = !_stricmp(Which, "all");
        ReverbShortSounds = IniInt("Audio", "ReverbShortSounds", 0);
        ReverbSettings.lRoom = IniInt("Audio", "ReverbLevel", ReverbSettings.lRoom);
        if (ReverbSettings.lRoom > 0) ReverbSettings.lRoom = 0;
        if (ReverbSettings.lRoom < -10000) ReverbSettings.lRoom = -10000;
        char Key[16]; GetPrivateProfileStringA("Audio", "ReverbCycleKey", "", Key, sizeof Key, IniPath);
        if (Key[0] == 'F' || Key[0] == 'f') {
            const int Number = atoi(Key + 1);
            if (Number >= 1 && Number <= 12) ReverbCycleKey = VK_F1 + Number - 1;
        }
        if (ReverbCycleKey) {
            RiotTimingOnFrame(ReverbCycleFrame);
            Log("Audio: %s walks through the reverb environments (the log says which one)", Key);
        }
        Log("Audio: reverb \"%s\" on %s sounds%s (level %ld, decay %.2f s)", Name,
            ReverbEverySound ? "all" : "3D", ReverbShortSounds ? ", short ones made longer" : "",
            ReverbSettings.lRoom, ReverbSettings.flDecayTime);
    }
    if (!LogAudio && !SoftwareBuffers && !Reverb) return;

    OriginalCreate8 = *(CreateFunctionType *)A->Create8Slot;
    OriginalCreate = *(CreateFunctionType *)A->CreateSlot;
    const int Hooked = OriginalCreate8 && OriginalCreate &&
                       WriteCode(A->Create8Slot, &(DWORD){ (DWORD)Create8 }, 4) &&
                       WriteCode(A->CreateSlot, &(DWORD){ (DWORD)Create }, 4);
    Log("Audio: sound buffers watched (software buffers %s, reverb %s, log %s): %s", SoftwareBuffers ? "on" : "off",
        Reverb ? "on" : "off", LogAudio ? "on" : "off", Hooked ? "ok" : "FAILED (unexpected import table)");

}

/* ================================================================ Why a sound is taken down with its owner
 *
 * One of the two ways a one-shot is cut off goes through an object of the engine's own, whose vtable is at
 * 0x592028: once a frame its slot 39 decides whether to end the sound it holds, and the condition is three
 * fields - [this+0x100] zero, bit 0x10 of [this+0x28] clear, bit 0x40 clear - with [this+0x24] holding the
 * sound. What those fields mean is not known. [this+0x100] is filled from [[this+0x64]+4], and the sound
 * header in a .sdu carries volume in its second field, so volume is the obvious guess and exactly the kind of
 * guess that has been wrong twice today.
 *
 * Reading them by hand in Cheat Engine took two attempts and gave one sample from one object, because a heap
 * address is only good inside the pause it was read in. Taking the slot over does the same reading on every
 * object that is about to lose its sound, for as long as the game runs, and costs the player nothing: the
 * original is called either way, so behaviour is unchanged whatever the numbers turn out to be.
 *
 * The Suffering only. Everything here was measured in that build. */
#define SoundOwnerVtable 0x592028
#define SoundOwnerUpdateSlot 39

typedef void (ThisCall *SoundOwnerUpdateType)(void *Self);
static SoundOwnerUpdateType OriginalSoundOwnerUpdate;
static volatile LONG OwnerTakedownsSeen;

static void ThisCall WatchedSoundOwnerUpdate(void *Self) {
    if (Self && LogAudio) {
        const unsigned char *Object = (const unsigned char *)Self;
        const DWORD Flags    = *(const DWORD *)(Object + 0x28);
        const DWORD Resource = *(const DWORD *)(Object + 0x64);
        const DWORD Deciding = *(const DWORD *)(Object + 0x100);
        const DWORD Sound    = *(const DWORD *)(Object + 0x24);
        if (Sound && !Deciding && !(Flags & 0x10) && !(Flags & 0x40) &&
            LogRoom(AudioLogLines, MaximumAudioLogLines) && OwnerTakedownsSeen < 40) {
            InterlockedIncrement(&OwnerTakedownsSeen);
            Log("[audio] a sound is about to be taken down with its owner: flags %08lX, resource %08lX,"
                " the field that decides %lu", (unsigned long)Flags, (unsigned long)Resource,
                (unsigned long)Deciding);
        }
    }
    OriginalSoundOwnerUpdate(Self);
}

static void WatchSoundOwners(void) {
    if (CurrentGame != GamePIH) return;
    const DWORD Entry = SoundOwnerVtable + 4 * SoundOwnerUpdateSlot;
    const DWORD Current = *(const DWORD *)Entry;
    /* Refuse unless the slot still holds what it held when this was measured, so a different build is left
       alone rather than sent somewhere arbitrary. */
    if (Current != 0x486D70) {
        Log("Audio: the sound owner's update is %08lX, not the 486D70 this was measured against; left alone",
            (unsigned long)Current);
        return;
    }
    OriginalSoundOwnerUpdate = (SoundOwnerUpdateType)Current;
    const DWORD Ours = (DWORD)&WatchedSoundOwnerUpdate;
    if (WriteCode(Entry, &Ours, sizeof Ours))
        Log("Audio: watching why a sound is taken down with its owner ([Debug] LogAudio): ok");
}

/* ================================================================ Who asks for a sound to stop, and with what fade
 *
 * The Stop hook on the DirectSound buffer names the last stop in the chain, and that is not enough: both of the
 * call sites it ever reports are the same two engine methods, slot 10 and slot 12 of the vtable at 0x58FD98,
 * and slot 10 is reached both by a hard stop and by the per-frame poll that asks whether a wind-down has
 * finished. Two different faults arrive at the log looking identical.
 *
 * What separates them is one argument, higher up. The sound manager's stop at 0x441200 takes a sound and a
 * float, and the float is a fade time in seconds - its first instructions compare it with 0.0f at 0x58C2AC and
 * branch:
 *
 *     fade  > 0  ->  0x43AF80, a volume ramp: state 2 at [sound+0x68], rate at [sound+0x50]
 *     fade <= 0  ->  0x43AEF0, which stops the buffer through slot 10 and lets the sound go
 *
 * The executable has exactly four calls to it, and three push a literal zero. So the engine owns a fade and
 * almost never uses it, and a one-shot that ends at 41% of its length was not faded out - something asked for
 * silence immediately. Which of the four asked is the whole question, and the four are different bugs:
 *
 *     0x44EBAB   zero     stops the sound, then drops it from the list at [this+0x4C]
 *     0x44EC7E   zero     the same, in a loop over that whole list: "stop everything I own"
 *     0x4732C4   1.0f     the only fade in the executable, while unlinking a node
 *     0x486F09   zero     the owner teardown, slot 36 and bit 0x40, documented in Docs/Audio.md
 *
 * Each call site is redirected on its own rather than the function's entry being detoured, so the return
 * address is the exact push that was read in Ghidra and no trampoline is needed. RedirectCall refuses a site
 * that does not still hold "call 0x441200", so another build is left alone instead of being sent somewhere
 * arbitrary. The original runs either way: this watches, it does not change what the game does.
 *
 * It does not cover the other family. The six stops the log attributes to slot 12 come from Suffering.rfl
 * through 0x43AECE and never pass through here.
 *
 * The Suffering only. Every address was read in that build. */
#define SoundManagerStop 0x441200
#define SoundManagerStopByObject 0x4410F0
#define SoundWasAlreadyGone (-2)     /* 0x441200 and 0x4410F0 both exit this way when the sound is not in the array */
static const DWORD SoundStopCallSites[] = { 0x44EBAB, 0x44EC7E, 0x4732C4, 0x486F09 };
static const DWORD ObjectStopCallSites[] = { 0x43EBF0, 0x43EDE2, 0x43EE23, 0x43F2CF, 0x441580, 0x441987 };

typedef int (ThisCall *SoundManagerStopType)(void *Self, DWORD Sound, DWORD Fade);
static SoundManagerStopType OriginalSoundManagerStop;
static volatile LONG StopRequestLines;

static int ThisCall WatchedSoundManagerStop(void *Self, DWORD Sound, DWORD Fade) {
    const DWORD Caller = (DWORD)__builtin_return_address(0);
    float Seconds;
    memcpy(&Seconds, &Fade, sizeof Seconds);
    InterlockedIncrement(&StopsAsked);
    if (!(Seconds > 0.0f)) InterlockedIncrement(&StopsAskedAtOnce);
    const int Result = OriginalSoundManagerStop(Self, Sound, Fade);
    if (Result == SoundWasAlreadyGone) InterlockedIncrement(&StopsAskedGone);
    if (LogAudio && LogRoom(StopRequestLines, 60)) {
        char Name[MAX_PATH + 24];
        AddressName(Caller, Name, sizeof Name);
        if (Seconds > 0.0f)
            Log("[audio] sound %08lX asked to fade out over %.2f s by %s (result %d)",
                (unsigned long)Sound, Seconds, Name, Result);
        else
            Log("[audio] sound %08lX asked to stop at once, no fade, by %s (result %d)",
                (unsigned long)Sound, Name, Result);
    }
    return Result;
}

/* The other door, and the one that matters. A session with the four sites above hooked came back with 21
 * requests against 34 one-shots genuinely cut off, so they could not be the cause - and the per-caller results
 * said why: 0x44EBAB asked thirteen times and got "no such sound" thirteen times, and the owner teardown that
 * cost a morning in Cheat Engine fired once.
 *
 * The call graph says where the rest went, and it is exhaustive rather than a guess. 0x43AEF0, the immediate
 * stop that reaches slot 10, has exactly two callers: 0x44127A inside 0x441200, the door above, which takes a
 * sound handle; and 0x441145 inside 0x4410F0, which takes the sound OBJECT. Four stops were carried out
 * through the first, so every other immediate stop in that session came through the second.
 *
 * 0x4410F0 is ret 0xC: the object, an index into the manager's array at [this+0x43E] (negative means "find
 * it"), and a force flag it hands straight to 0x43AEF0. Six call sites, listed above. */
typedef int (ThisCall *ObjectStopType)(void *Self, void *Sound, int Index, DWORD Force);
static ObjectStopType OriginalObjectStop;
static volatile LONG ObjectStopLines;

static int ThisCall WatchedObjectStop(void *Self, void *Sound, int Index, DWORD Force) {
    const DWORD Caller = (DWORD)__builtin_return_address(0);
    InterlockedIncrement(&ObjectStopsAsked);
    if (Force) InterlockedIncrement(&ObjectStopsForced);
    const int Result = OriginalObjectStop(Self, Sound, Index, Force);
    if (Result == SoundWasAlreadyGone) InterlockedIncrement(&ObjectStopsGone);
    if (LogAudio && LogRoom(ObjectStopLines, 60)) {
        char Name[MAX_PATH + 24];
        AddressName(Caller, Name, sizeof Name);
        Log("[audio] sound object %08lX asked to stop%s by %s (result %d)", (unsigned long)(DWORD)Sound,
            Force ? ", forced," : ",", Name, Result);
    }
    return Result;
}

static void WatchSoundStopRequests(void) {
    if (CurrentGame != GamePIH) return;
    const int Sites = (int)(sizeof SoundStopCallSites / sizeof *SoundStopCallSites);
    OriginalSoundManagerStop = (SoundManagerStopType)SoundManagerStop;
    int Done = 0;
    for (int Index = 0; Index < Sites; Index++)
        Done += RedirectCall(SoundStopCallSites[Index], SoundManagerStop, (DWORD)&WatchedSoundManagerStop);
    if (Done) Log("Audio: watching who asks for a sound to stop by handle, and with what fade"
                  " ([Debug] LogAudio): %d/%d call sites", Done, Sites);
    else Log("Audio: the calls to the sound manager's stop by handle are not where they were measured;"
             " left alone");

    const int ObjectSites = (int)(sizeof ObjectStopCallSites / sizeof *ObjectStopCallSites);
    OriginalObjectStop = (ObjectStopType)SoundManagerStopByObject;
    int ObjectDone = 0;
    for (int Index = 0; Index < ObjectSites; Index++)
        ObjectDone += RedirectCall(ObjectStopCallSites[Index], SoundManagerStopByObject,
                                   (DWORD)&WatchedObjectStop);
    if (ObjectDone) Log("Audio: watching who asks for a sound to stop by object ([Debug] LogAudio):"
                        " %d/%d call sites", ObjectDone, ObjectSites);
    else Log("Audio: the calls to the sound manager's stop by object are not where they were measured;"
             " left alone");
}
