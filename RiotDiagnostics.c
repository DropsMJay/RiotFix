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
    /* MiniDumpWithIndirectlyReferencedMemory | WithDataSegs | WithHandleData | WithFullMemoryInfo | WithThreadInfo */
    BOOL Ok = WriteMiniDump(GetCurrentProcess(), GetCurrentProcessId(), File, 0x41 | 0x4 | 0x800 | 0x1000, &Info, NULL, NULL);
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
    AddVectoredExceptionHandler(1, VectoredHandler);
    SetUnhandledExceptionFilter(UnhandledFilter);
    HANDLE Thread = CreateThread(NULL, 0, KeepFilter, NULL, 0, NULL); if (Thread) CloseHandle(Thread);
    Log("Crash diagnostics on (turn off with [Debug] CrashLog=0)");
}
