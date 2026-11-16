/*
 * RiotDiagnostics - crash log, minidump and process memory report
 *
 * On a serious exception it logs the location, registers, code bytes, process memory and likely return addresses,
 * and on an unhandled one it also writes RiotFix.dmp next to the log.
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotDiagnostics.h"
#include "RiotTiming.h"
#include <wchar.h>
#include <stdio.h>
#include <string.h>

static volatile LONG CrashCount, DumpWritten;

static const char *ExceptionName(DWORD Code) {
    switch (Code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS";
    case 0xC0000409: return "STACK_BUFFER_OVERRUN";
    case 0xC0000374: return "HEAP_CORRUPTION";
    default: return NULL;
    }
}

/* Process memory: address space (2 GB without LAA, 4 GB with LAA on 64-bit Windows), largest free block and
   private bytes. */
typedef struct { DWORD cb, PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
    QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage; } ProcessMemoryCountersEx;
typedef BOOL (WINAPI *GetProcessMemoryInfoFunctionType)(HANDLE, void *, DWORD);
void RiotDiagnosticsLogMemory(const char *When) {
    MEMORYSTATUSEX Status = {0}; Status.dwLength = sizeof Status;
    GlobalMemoryStatusEx(&Status);
    SIZE_T Largest = 0; MEMORY_BASIC_INFORMATION Info; BYTE *Cursor = (BYTE *)0x10000;
    while ((ULONG_PTR)Cursor < (ULONG_PTR)Status.ullTotalVirtual && VirtualQuery(Cursor, &Info, sizeof Info)) {
        if (Info.State == MEM_FREE && Info.RegionSize > Largest) Largest = Info.RegionSize;
        Cursor = (BYTE *)Info.BaseAddress + Info.RegionSize;
    }
    ProcessMemoryCountersEx Counters = {0}; Counters.cb = sizeof Counters;
    GetProcessMemoryInfoFunctionType GetInfo = (GetProcessMemoryInfoFunctionType)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    int HasCounters = GetInfo && GetInfo(GetCurrentProcess(), &Counters, sizeof Counters);
    Log("  memory (%s): free addresses %llu of %llu MB, largest free block %lu MB, private %lu MB (peak %lu MB)",
        When, Status.ullAvailVirtual >> 20, Status.ullTotalVirtual >> 20, (unsigned long)(Largest >> 20),
        HasCounters ? (unsigned long)(Counters.PrivateUsage >> 20) : 0UL, HasCounters ? (unsigned long)(Counters.PeakPagefileUsage >> 20) : 0UL);
}

static int FullCrashDump;
typedef BOOL (WINAPI *MiniDumpWriteDumpFunctionType)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
typedef struct { DWORD ThreadId; PEXCEPTION_POINTERS ExceptionPointers; BOOL ClientPointers; } DumpExceptionInfo;
static void WriteDump(PEXCEPTION_POINTERS Pointers) {
    if (InterlockedExchange(&DumpWritten, 1)) return;
    /* The Windows dbghelp.dll first: the old copy in the game folder rejects some flags. */
    char SystemDbghelp[MAX_PATH]; GetSystemDirectoryA(SystemDbghelp, MAX_PATH); strcat(SystemDbghelp, "\\dbghelp.dll");
    HMODULE Dbghelp = LoadLibraryA(SystemDbghelp);
    if (!Dbghelp) Dbghelp = LoadLibraryA("dbghelp.dll");
    MiniDumpWriteDumpFunctionType WriteMiniDump = Dbghelp ? (MiniDumpWriteDumpFunctionType)GetProcAddress(Dbghelp, "MiniDumpWriteDump") : NULL;
    if (!WriteMiniDump) { Log("  (dbghelp.dll not available: no .dmp)"); return; }
    char Path[MAX_PATH]; strcpy(Path, LogPath); char *Dot = strrchr(Path, '.'); if (Dot) strcpy(Dot, ".dmp");
    HANDLE File = CreateFileA(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE) return;
    DumpExceptionInfo Info = { GetCurrentThreadId(), Pointers, FALSE };
    /* MiniDumpWithIndirectlyReferencedMemory | WithDataSegs | WithHandleData | WithFullMemoryInfo | WithThreadInfo.
     * [Debug] FullCrashDump adds MiniDumpWithFullMemory, which brings every page the process has, code and
     * read-only data included. The ordinary dump leaves those out, and without them a crash cannot be followed
     * past the first object: its class is a pointer into read-only data that is not in the file. The cost is the
     * size - the whole of the game's memory instead of a few megabytes - so it stays off until a crash needs it. */
    DWORD Flags = 0x41 | 0x4 | 0x800 | 0x1000;
    if (FullCrashDump) Flags |= 0x2;
    BOOL Ok = WriteMiniDump(GetCurrentProcess(), GetCurrentProcessId(), File, Flags, &Info, NULL, NULL);
    DWORD Error = Ok ? 0 : GetLastError();
    if (!Ok) {   /* old dbghelp: version 1.2 flags only */
        SetFilePointer(File, 0, NULL, FILE_BEGIN); SetEndOfFile(File);
        Ok = WriteMiniDump(GetCurrentProcess(), GetCurrentProcessId(), File, 0x41 | 0x4, &Info, NULL, NULL);
    }
    CloseHandle(File);
    Log("  dump: %s (%s%s)", Path, Ok ? "ok" : "failed", Error ? ", without the memory map" : "");
}

static void LogException(PEXCEPTION_POINTERS Pointers, const char *Kind) {
    EXCEPTION_RECORD *Record = Pointers->ExceptionRecord; CONTEXT *Context = Pointers->ContextRecord;
    char Where[MAX_PATH + 32]; AddressName((DWORD)Record->ExceptionAddress, Where, sizeof Where);
    const char *Name = ExceptionName(Record->ExceptionCode);
    Log("=== %s: %s (0x%08lx) at 0x%08lx  %s  thread %lu", Kind, Name ? Name : "?", Record->ExceptionCode,
        (DWORD)Record->ExceptionAddress, Where, GetCurrentThreadId());
    if (Record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && Record->NumberParameters >= 2)
        Log("  %s at address 0x%08lx", Record->ExceptionInformation[0] == 0 ? "read" : Record->ExceptionInformation[0] == 1 ? "write" : "execute",
            (DWORD)Record->ExceptionInformation[1]);
    Log("  EAX=%08lx EBX=%08lx ECX=%08lx EDX=%08lx ESI=%08lx EDI=%08lx", Context->Eax, Context->Ebx, Context->Ecx, Context->Edx, Context->Esi, Context->Edi);
    Log("  EBP=%08lx ESP=%08lx EIP=%08lx", Context->Ebp, Context->Esp, Context->Eip);
    MEMORY_BASIC_INFORMATION Info;
    if (VirtualQuery((void *)Context->Eip, &Info, sizeof Info) && Info.State == MEM_COMMIT && !(Info.Protect & PAGE_NOACCESS)) {
        unsigned char *Code = (unsigned char *)Context->Eip; char Hex[64] = "";
        for (int Index = 0; Index < 16; Index++) sprintf(Hex + 3 * Index, "%02x ", Code[Index]);
        Log("  code: %s", Hex);
    }
    RiotDiagnosticsLogMemory("at the crash");
    Log("  stack (likely return addresses):");
    DWORD *Stack = (DWORD *)Context->Esp; int Shown = 0;
    for (int Index = 0; Index < 256 && Shown < 16; Index++) {
        if (!VirtualQuery(Stack + Index, &Info, sizeof Info) || Info.State != MEM_COMMIT || (Info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
        DWORD Value = Stack[Index];
        MEMORY_BASIC_INFORMATION ValueInfo;
        if (Value > 0x10000 && VirtualQuery((void *)Value, &ValueInfo, sizeof ValueInfo) && ValueInfo.State == MEM_COMMIT &&
            (ValueInfo.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
            char ValueName[MAX_PATH + 32]; AddressName(Value, ValueName, sizeof ValueName);
            Log("    [esp+%03x] %08lx  %s", Index * 4, Value, ValueName); Shown++;
        }
    }
}

static LONG WINAPI VectoredHandler(PEXCEPTION_POINTERS Pointers) {
    if (!ExceptionName(Pointers->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    /* The first 5 serious exceptions (the game may handle some of them). */
    if (InterlockedIncrement(&CrashCount) <= 5) LogException(Pointers, "exception");
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI UnhandledFilter(PEXCEPTION_POINTERS Pointers) {
    LogException(Pointers, "CRASH (unhandled)");
    WriteDump(Pointers);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The game or its runtime may replace the filter; ours is put back every 0.5 s for 60 s. */
static DWORD WINAPI KeepFilter(LPVOID Unused) {
    (void)Unused;
    for (int Index = 0; Index < 120; Index++) { SetUnhandledExceptionFilter(UnhandledFilter); Sleep(500); }
    return 0;
}

void RiotDiagnosticsLogStartup(void) {
    /* Without LAA the game only gets 2 GB of addresses. */
    BYTE *Base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *Headers = (IMAGE_NT_HEADERS *)(Base + ((IMAGE_DOS_HEADER *)Base)->e_lfanew);
    Log("Exe %s", (Headers->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) ? "is Large Address Aware" : "without LAA: 2 GB limit");
    RiotDiagnosticsLogMemory("at start-up");
}

void RiotDiagnosticsApply(void) {
    if (!IniInt("Debug", "CrashLog", 1)) return;
    FullCrashDump = IniInt("Debug", "FullCrashDump", 0);
    AddVectoredExceptionHandler(1, VectoredHandler);
    SetUnhandledExceptionFilter(UnhandledFilter);
    HANDLE Thread = CreateThread(NULL, 0, KeepFilter, NULL, 0, NULL); if (Thread) CloseHandle(Thread);
    Log("Crash diagnostics on (turn off with [Debug] CrashLog=0)%s",
        FullCrashDump ? ", and the .dmp will hold the whole of the game's memory ([Debug] FullCrashDump)" : "");
    RiotWatchScreenText();
}

/* ================================================================ Watching a screen's text
 * The crash that 0.4.0 to 0.4.5 could not explain lands in the engine's screen walk, in the method at
 * Suffering.rfl+0x6ADF0. That method reserves 0x214 bytes of stack, fills a buffer 0x1C bytes into it from a
 * look-up capped at 0x100, and hands the buffer to rfl+0x6A4F0, which treats it as a **wide** string: it takes
 * its length, allocates twice that, and copies twice that many bytes. The arithmetic is the whole point. The
 * buffer starts 0x200 bytes below the frame's exception record and 0x20C below its return address, so 0x100
 * wide characters fill it exactly to the exception record and anything longer walks into the return address -
 * which is what the dumps show, overwritten with 3.
 *
 * Whether that is what happens cannot be read from a dump after the fact: by the time the handler runs, its own
 * records have been written over the same stack. So the string is measured as it arrives. This redirects the one
 * call site on the crash path and reports any string that comes near the buffer's end, then calls the original. */
/* Catching the write in the act. The dumps show the return address of rfl+0x6ADF0 replaced by 3 by the time that
 * method returns, and three calls inside it could be responsible. Rather than compute where that word lives from
 * the frame's shape - which depends on how the compiler passes arguments and is easy to get wrong - the word is
 * found by looking for it: from inside a hook, the stack is scanned upwards for the first value that is a return
 * address into the method's caller, and that address is then read again after the original call returns. A word
 * that was a code address and is no longer one was written by the call in between. */
static DWORD *FindMethodReturnSlot(void *Near, DWORD Low, DWORD High) {
    DWORD *const Start = (DWORD *)(((size_t)Near) & ~3u);
    for (int Step = 0; Step < 0x100; Step++) {
        DWORD *const At = Start + Step;
        if (IsBadReadPtr(At, 4)) break;
        if (*At >= Low && *At < High) return At;
    }
    return NULL;
}

/* The look-up that fills the buffer: thiscall(key, buffer, 1, 0x100), reached through slot 17 of a singleton
 * whose vtable sits at a fixed address in the exe. */
typedef int (ThisCall *FindTextFunctionType)(void *, void *, wchar_t *, int, int);
static FindTextFunctionType OriginalFindText;
static DWORD CallerLow, CallerHigh;
static int ThisCall WatchedFindText(void *Self, void *Key, wchar_t *Buffer, int One, int Limit) {
    volatile int Here = 0;
    DWORD *const Slot = CallerLow ? FindMethodReturnSlot((void *)&Here, CallerLow, CallerHigh) : NULL;
    const DWORD Before = Slot ? *Slot : 0;
    const int Result = OriginalFindText(Self, Key, Buffer, One, Limit);
    if (Slot && *Slot != Before) {
        static int Said;
        if (Said++ < 8)
            Log("[screen] the text look-up changed the return address above it from %08lx to %08lx "
                "(buffer now %u wide characters)", Before, *Slot, (unsigned)wcslen(Buffer));
    }
    return Result;
}

typedef void (ThisCall *ScreenTextFunctionType)(void *, const wchar_t *);
static ScreenTextFunctionType OriginalScreenText;
static void ThisCall WatchedScreenText(void *Self, const wchar_t *Text) {
    static int Reported;
    if (Text) {
        size_t Length = 0;
        /* Counted by hand and stopped early: a string already past the buffer may have no terminator at all. */
        while (Length < 0x400 && Text[Length]) Length++;
        /* Every one of them, not only the long ones: the crash follows one particular message, so the last line
         * written before the log ends names it. */
        if (Reported++ < 200) Log("[screen] text %u: \"%.100ls\" (%u characters)", Reported, Text,
                                  (unsigned)Length);
    }
    OriginalScreenText(Self, Text);
}

/* Called once a frame until it takes: the .rfl is loaded after RiotFix, so the call site does not exist yet when
 * the options are read. */
static void WatchScreenTextFrame(void) {
    static int Done;
    if (Done) return;
    HMODULE Module = RflModule();
    if (!Module) return;
    Done = 1;
    const DWORD Base = (DWORD)(size_t)Module;
    /* Slot 17 of the look-up singleton's class. The vtable is the exe's, at a fixed address, so the slot is
     * replaced only while it still holds the function it is expected to hold. */
    DWORD *const Slot = (DWORD *)(0x58C358 + 0x44);
    DWORD Old = 0;
    if (!IsBadReadPtr(Slot, 4) && *Slot == 0x484500) {
        OriginalFindText = (FindTextFunctionType)(size_t)*Slot;
        DWORD Protect = 0;
        if (VirtualProtect(Slot, 4, PAGE_EXECUTE_READWRITE, &Protect)) {
            Old = *Slot; *Slot = (DWORD)(size_t)WatchedFindText;
            VirtualProtect(Slot, 4, Protect, &Protect);
        }
    }
    /* The window of code the method's own return address must point into: the caller 0x100508A0 makes the call
     * at its 0x10050A0A, so the word sought is an address a little past that. */
    CallerLow = Base + 0x50A00; CallerHigh = Base + 0x50A50;
    Log("[screen] the text look-up is %s", Old ? "watched" : "not where it was expected, so it is not watched");
    OriginalScreenText = (ScreenTextFunctionType)(size_t)(Base + 0x6A4F0);
    if (RedirectCall(Base + 0x6AE84, Base + 0x6A4F0, (DWORD)(size_t)WatchedScreenText))
        Log("Screen text watched for strings that fill their buffer ([Debug] WatchScreenText): ok");
    else
        Log("[screen] the call at Suffering.rfl+0x6AE84 is not the one expected, so the text is not watched");
}

void RiotWatchScreenText(void) {
    if (!IniInt("Debug", "WatchScreenText", 0)) return;
    RiotTimingOnFrame(WatchScreenTextFrame);
}
