/*
 * RiotCore - shared services for every RiotFix module
 *
 * Log file, RiotFix.ini access, game detection and the memory helpers every hook uses
 * (byte checks, code writes, call redirection, vtable slot replacement).
 * No module writes to game memory without going through these helpers.
 *
 * Licence: MIT
 */
#ifndef RiotCoreHeader
#define RiotCoreHeader

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>

#define ThisCall __attribute__((thiscall))

#define RiotFixVersion "0.0.8"

/* ---------------------------------------------------------------- Games
   Every module keeps its own address table indexed by RiotGame. */
typedef enum {
    GamePIH = 0,          /* The Suffering (Prison Is Hell), Suffering.exe */
    GameTTB = 1,          /* The Suffering: Ties That Bind, SufferingTTB.exe */
    GameCount = 2,
    GameUnknown = -1
} RiotGame;

extern RiotGame CurrentGame;

/* Recognises the running executable. Returns GameUnknown when no signature matches. */
RiotGame DetectGame(void);
const char *GameName(RiotGame Game);

/* ---------------------------------------------------------------- Log and settings */
extern char IniPath[MAX_PATH], LogPath[MAX_PATH];
extern int TraceEnabled;   /* [Debug] Trace=1 logs the first calls of each hook. */

void LogInitialize(void);
void LogOpen(void);        /* Keeps the log file open (start-up). */
void LogClose(void);       /* Back to open-append-close per message. */
void Log(const char *Format, ...) __attribute__((format(printf, 1, 2)));
void RotateLogs(const char *Folder);

float IniFloat(const char *Section, const char *Key, float Default);
int IniInt(const char *Section, const char *Key, int Default);

/* Writes "module+offset" for a code address (e.g. "SUFFERING.EXE+0x4a0ac8"). */
void AddressName(DWORD Address, char *Out, size_t Size);

/* ---------------------------------------------------------------- Memory helpers */
/* Writes Size bytes at Address (code or data), restoring the page protection afterwards. */
int WriteCode(DWORD Address, const void *Source, size_t Size);
/* True when Address is committed memory holding exactly these bytes. */
int BytesAre(DWORD Address, const unsigned char *Bytes, size_t Size);
/* Replaces "call Expected" at At with "call Target". Fails if At is not that call. */
int RedirectCall(DWORD At, DWORD Expected, DWORD Target);
/* Repoints the memory operand of "call dword ptr [Slot]" (FF 15) or "mov ebx/ebp/esi/edi, dword ptr [Slot]" at At
   from the import slot ExpectedSlot to NewSlot, a pointer variable in RiotFix holding the replacement function.
   Only that one use changes; every other caller of the import keeps the original function. */
int RepointImportUse(DWORD At, DWORD ExpectedSlot, void *const *NewSlot);
/* Replaces a vtable slot that must currently hold Expected. */
int ReplaceVtableSlot(DWORD Vtable, int Slot, DWORD Expected, DWORD Target);

#endif
