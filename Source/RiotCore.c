/*
 * RiotCore - shared services for every RiotFix module (see RiotCore.h)
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

RiotGame CurrentGame = GameUnknown;
char IniPath[MAX_PATH], LogPath[MAX_PATH];
int TraceEnabled;

static FILE *LogFile;
static CRITICAL_SECTION LogLock;

/* ================================================================ Games */
typedef struct {
    const char *Name;
    DWORD SignatureAt;                 /* "push 0x380; push 0x100" in the shadow set-up of each executable */
    unsigned char Signature[10];
} GameSignature;

static const GameSignature Signatures[GameCount] = {
    [GamePIH] = { "The Suffering",                 0x492AE2, {0x68,0x80,0x03,0x00,0x00,0x68,0x00,0x01,0x00,0x00} },
    [GameTTB] = { "The Suffering: Ties That Bind", 0x496B30, {0x68,0x80,0x03,0x00,0x00,0x68,0x00,0x01,0x00,0x00} },
};

RiotGame DetectGame(void) {
    for (int Index = 0; Index < GameCount; Index++)
        if (BytesAre(Signatures[Index].SignatureAt, Signatures[Index].Signature, sizeof Signatures[Index].Signature))
            return (RiotGame)Index;
    return GameUnknown;
}

const char *GameName(RiotGame Game) {
    return Game >= 0 && Game < GameCount ? Signatures[Game].Name : "unknown";
}

/* ================================================================ Log */
void LogInitialize(void) {
    InitializeCriticalSection(&LogLock);
}

/* The previous run's log is kept, because the run worth reading is often the one before this one.
 *
 * A player who hits a crash or a freeze restarts the game to tell someone about it, and starting the game is
 * exactly what used to destroy the evidence: the log is opened for writing, which empties it. One copy back
 * costs nothing and has already been wanted twice. */
void LogOpen(void) {
    char Previous[MAX_PATH];
    const int Length = snprintf(Previous, sizeof Previous, "%s", LogPath);
    if (Length > 4 && Length < (int)sizeof Previous - 10 && !_stricmp(LogPath + Length - 4, ".log")) {
        strcpy(Previous + Length - 4, ".previous.log");
        MoveFileExA(LogPath, Previous, MOVEFILE_REPLACE_EXISTING);
    }
    LogFile = fopen(LogPath, "w");
}

void LogClose(void) {
    if (LogFile) { fclose(LogFile); LogFile = NULL; }
}

void Log(const char *Format, ...) {
    EnterCriticalSection(&LogLock);
    FILE *File = LogFile ? LogFile : fopen(LogPath, "a");
    if (File) {
        va_list Arguments; va_start(Arguments, Format); vfprintf(File, Format, Arguments); va_end(Arguments);
        fputc('\n', File); fflush(File);
        if (File != LogFile) fclose(File);
    }
    LeaveCriticalSection(&LogLock);
}

/* Keeps the previous three runs: RiotFix.1.log (the latest), .2, .3, and the latest dump as RiotFix.1.dmp. */
void RotateLogs(const char *Folder) {
    char From[MAX_PATH], To[MAX_PATH];
    for (int Index = 3; Index >= 1; Index--) {
        if (Index == 1) strcpy(From, LogPath); else snprintf(From, MAX_PATH, "%sRiotFix.%d.log", Folder, Index - 1);
        snprintf(To, MAX_PATH, "%sRiotFix.%d.log", Folder, Index);
        MoveFileExA(From, To, MOVEFILE_REPLACE_EXISTING);
    }
    snprintf(From, MAX_PATH, "%sRiotFix.dmp", Folder); snprintf(To, MAX_PATH, "%sRiotFix.1.dmp", Folder);
    MoveFileExA(From, To, MOVEFILE_REPLACE_EXISTING);
}

/* ================================================================ Settings */
float IniFloat(const char *Section, const char *Key, float Default) {
    char Buffer[64]; GetPrivateProfileStringA(Section, Key, "", Buffer, sizeof Buffer, IniPath);
    return Buffer[0] ? (float)atof(Buffer) : Default;
}

/* Numbers, and the words people write when a setting is a switch.
 *
 * GetPrivateProfileInt answers 0 for anything that is not a number, so "BlackAndWhite=true" turned the feature off
 * and said nothing about why. Nobody reading their own file would see the mistake: the line is there, it is
 * spelled right, and it reads as "on". The words below are accepted for that reason. They cost nothing, because
 * each one parsed as 0 before, so no file that worked can change meaning. */
int IniInt(const char *Section, const char *Key, int Default) {
    char Buffer[64]; GetPrivateProfileStringA(Section, Key, "", Buffer, sizeof Buffer, IniPath);
    const char *Text = Buffer;
    while (*Text == ' ' || *Text == '\t') Text++;
    if (!*Text) return Default;
    static const char *const Yes[] = { "true", "yes", "on", "enabled" };
    static const char *const No[]  = { "false", "no", "off", "disabled" };
    for (size_t Index = 0; Index < sizeof Yes / sizeof *Yes; Index++) if (!_stricmp(Text, Yes[Index])) return 1;
    for (size_t Index = 0; Index < sizeof No / sizeof *No; Index++) if (!_stricmp(Text, No[Index])) return 0;
    return atoi(Text);
}

/* ================================================================ The game's .rfl */
HMODULE RflModule(void) {
    static HMODULE Module;   /* the .rfl is loaded after RiotFix, so the lookup is kept until it succeeds */
    if (!Module && CurrentGame != GameUnknown) Module = GetModuleHandleA(CurrentGame == GameTTB ? "SufferingTTB.rfl" : "Suffering.rfl");
    return Module;
}

DWORD RflOffset(DWORD Address) {
    HMODULE Module = RflModule();
    return Module ? Address - (DWORD)Module : 0;
}

/* ================================================================ Memory helpers */
int WriteCode(DWORD Address, const void *Source, size_t Size) {
    DWORD Old;
    if (!VirtualProtect((void *)Address, Size, PAGE_EXECUTE_READWRITE, &Old)) return 0;
    memcpy((void *)Address, Source, Size);
    VirtualProtect((void *)Address, Size, Old, &Old);
    FlushInstructionCache(GetCurrentProcess(), (void *)Address, Size);
    return 1;
}

int BytesAre(DWORD Address, const unsigned char *Bytes, size_t Size) {
    MEMORY_BASIC_INFORMATION Info;
    if (!VirtualQuery((void *)Address, &Info, sizeof Info) || Info.State != MEM_COMMIT) return 0;
    return memcmp((void *)Address, Bytes, Size) == 0;
}

int RedirectCall(DWORD At, DWORD Expected, DWORD Target) {
    unsigned char *Code = (unsigned char *)At;
    if (Code[0] != 0xE8 || At + 5 + *(int *)(Code + 1) != Expected) return 0;
    unsigned char Call[5] = {0xE8}; int Relative = (int)(Target - (At + 5)); memcpy(Call + 1, &Relative, 4);
    return WriteCode(At, Call, 5);
}

int ReplaceVtableSlot(DWORD Vtable, int Slot, DWORD Expected, DWORD Target) {
    DWORD *Entry = (DWORD *)Vtable + Slot;
    if (*Entry != Expected) return 0;
    return WriteCode((DWORD)Entry, &Target, 4);
}

int RepointImportUse(DWORD At, DWORD ExpectedSlot, void *const *NewSlot) {
    /* call [Slot]; mov ebx, [Slot]; mov ebp, [Slot]; mov esi, [Slot]; mov edi, [Slot] */
    static const unsigned char Opcodes[5][2] = { {0xFF, 0x15}, {0x8B, 0x1D}, {0x8B, 0x2D}, {0x8B, 0x35}, {0x8B, 0x3D} };
    int Known = 0;
    for (int Index = 0; Index < 5 && !Known; Index++) {
        unsigned char Expected[6] = { Opcodes[Index][0], Opcodes[Index][1] };
        memcpy(Expected + 2, &ExpectedSlot, 4);
        Known = BytesAre(At, Expected, 6);
    }
    if (!Known) return 0;
    DWORD Operand = (DWORD)NewSlot;
    return WriteCode(At + 2, &Operand, 4);
}

void AddressName(DWORD Address, char *Out, size_t Size) {
    HMODULE Module = NULL; char Path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)Address, &Module) && Module) {
        GetModuleFileNameA(Module, Path, MAX_PATH);
        char *Base = strrchr(Path, '\\');
        snprintf(Out, Size, "%s+0x%lx", Base ? Base + 1 : Path, Address - (DWORD)Module);
    } else snprintf(Out, Size, "?");
}

/* The options that exist to answer a question rather than to be set live in [Testing], where a player reading
 * the .ini can see at a glance that they are not settings. They were in with the rest until then, so each is
 * still looked for in the section it used to be in - an .ini written before the move keeps working, exactly as
 * [Shader] does for the settings that came out of [Display].
 *
 * Moving a key between sections is not free: the reader looks in one place, and a key that moved without the
 * reader moving with it reads as absent and takes its default. MenuExtras defaults to 1 and was set to 1, so
 * nothing would have looked wrong, which is the kind of silence worth avoiding. */
int IniTestInt(const char *Section, const char *Key, int Default) {
    if (IniInt("Testing", Key, 0) == IniInt("Testing", Key, 1)) return IniInt("Testing", Key, 0);
    return IniInt(Section, Key, Default);
}
