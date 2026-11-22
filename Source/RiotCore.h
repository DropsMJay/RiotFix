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

/* A bare number is a release: 0.6.26. A letter after it is a test build on the way to the next one: 0.6.26a,
 * 0.6.26b, and so on, raised once per build handed over. When the bug it was chasing is fixed or the feature it
 * was carrying lands, the letters are dropped and the number goes up - 0.6.26c becomes 0.6.27, which is a release
 * again. So a version with no letter is something that was published, and one with a letter says for itself that
 * it is not. Before this rule, releases and test builds both took the next number and six of them went in an
 * evening, which told a bug reporter nothing about what he was running. */
#define RiotFixVersion "0.6.27"
/* The build's own date and time, so two builds of the same version can be told apart - which matters while a fix is
 * being tested several times in an evening. Build.sh passes a sortable stamp; __DATE__ / __TIME__ is the fallback so
 * the source still compiles without it. RiotFixBuildVersion is what the log and the window title carry. */
#ifndef RiotFixBuild
#define RiotFixBuild __DATE__ " " __TIME__
#endif
#define RiotFixBuildVersion RiotFixVersion " (" RiotFixBuild ")"

/* ---------------------------------------------------------------- Games
   Every module keeps its own address table indexed by RiotGame. */
typedef enum {
    GamePIH = 0,          /* The Suffering (Prison Is Hell), Suffering.exe */
    GameTTB = 1,          /* The Suffering: Ties That Bind, SufferingTTB.exe */
    GameCount = 2,
    GameUnknown = -1
} RiotGame;

extern RiotGame CurrentGame;

/* The game's .rfl (the game code; the .exe is the engine). Only valid once the game has loaded it. */
HMODULE RflModule(void);
DWORD RflOffset(DWORD Address);   /* an address inside the .rfl as an offset from its base, 0 when it is not loaded */

/* Where a HUD element belongs on a wide screen: the side it keeps its distance from. */
enum { AnchorNone, AnchorLeft, AnchorCentre, AnchorRight };

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
int IniTestInt(const char *Section, const char *Key, int Default);   /* [Testing] first, then the old section */

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
