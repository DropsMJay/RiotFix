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

void LogOpen(void) {
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

int IniInt(const char *Section, const char *Key, int Default) {
    return (int)GetPrivateProfileIntA(Section, Key, Default, IniPath);
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
