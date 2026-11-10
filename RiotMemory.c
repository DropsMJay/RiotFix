/*
 * RiotMemory - resource cache budgets
 *
 * Facts (original GOG executables, see RiotEngineNotes.md):
 * - Every resource type has a manager object (table PIH 0x5DC828 / TTB 0x640380, indexed by type). Field +4 is
 *   the budget in bytes and +8 the amount in use. A budget of 0 means no limit.
 * - When a load would exceed the budget, the loader (PIH 0x463920) recycles a loaded resource of the same type;
 *   after a load, if the total is over budget, the manager unloads resources (PIH 0x463650, vtable slot 0x1C).
 * - The budgets come from the executable's static constructors: "push Budget; mov ecx, Manager; call Ctor".
 *   The constructor passes the value to the manager base constructor (PIH 0x462F10 / TTB 0x4693D0), which stores
 *   it at +4.
 * - Slot 4 of the game object's vtable (PIH 0x401810, vtable 0x58C4B4 / TTB 0x401890, vtable 0x5E6560) sets the
 *   Model, Texture and Animation budgets again through the budget setter (PIH 0x4627A0 / TTB 0x4687A0, thiscall,
 *   one argument; it unloads resources when the new budget is lower).
 *
 * What this module does:
 * - Replaces the immediate of each static constructor's "push Budget" (original bytes checked first). The DLL is
 *   loaded before the executable's static constructors run, so the managers start with the new budget.
 * - Redirects the three setter calls of slot 4 to SetBudgetHook, which passes the configured budget instead and
 *   logs the first calls, so the log shows whether and when the game re-applies the budgets.
 * - [Memory] LogUsage=1 starts a thread that logs every new peak of the amount in use of each manager.
 *
 * Budgets can only be raised: a value lower than the original is rejected. Video keeps its original 0 (no limit).
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotMemory.h"
#include "RiotDiagnostics.h"
#include <string.h>

/* ================================================================ Per-game addresses */
typedef struct {
    const char *Name;       /* also the RiotFix.ini key in [Memory] */
    DWORD Push;             /* "push imm32" of the static constructor, followed by "mov ecx, Manager" */
    DWORD Manager;
    DWORD Original;         /* original budget in bytes */
} BudgetSite;

#define MaximumBudgets 10

typedef struct {
    BudgetSite Budgets[MaximumBudgets]; int BudgetCount;
    DWORD SetterCalls[3];   /* calls to the budget setter in slot 4 of the game object (Model, Texture, Animation) */
    DWORD Setter;
    DWORD SetterVtables[12]; int SetterVtableCount;   /* manager vtables whose slot 1 is the budget setter */
} MemoryAddresses;

static const MemoryAddresses Addresses[GameCount] = {
    [GamePIH] = {
        { { "Model",     0x582A90, 0x5C7880, 0xA00000 },
          { "Texture",   0x582B70, 0x5C8BD0, 0xF00000 },
          { "Animation", 0x582930, 0x5C5EF0, 0x600000 },
          { "Sound",     0x582BD0, 0x5C9478, 0x1E00000 },
          { "Sequence",  0x582B90, 0x5C9010, 0x10000 },
          { "String",    0x582BF0, 0x5C9C98, 0x5000 },
          { "Lighting",  0x582C30, 0x5CA170, 0x100000 },
          { "Waveform",  0x582AD0, 0x5C8118, 0x10000 },
          { "Font",      0x582AF0, 0x5C8558, 0x2800 } }, 9,
        { 0x40181A, 0x401829, 0x401838 }, 0x4627A0,
        { 0x58C2B0, 0x58C310, 0x58C358, 0x58C3B0, 0x58C3F8, 0x58FC10, 0x590198, 0x590728, 0x590EC0, 0x591AA0, 0x591E50 }, 11 },
    [GameTTB] = {
        { { "Model",     0x5D9390, 0x63AA88, 0xA00000 },
          { "Texture",   0x5D94C0, 0x63C2E0, 0xF00000 },
          { "Animation", 0x5D9230, 0x639458, 0x600000 },
          { "Sound",     0x5D9520, 0x63CBB8, 0x1E00000 },
          { "Sequence",  0x5D94E0, 0x63C738, 0x20000 },
          { "String",    0x5D9540, 0x63D3F0, 0x5000 },
          { "Lighting",  0x5D9582, 0x63D8E0, 0x100000 },
          { "Waveform",  0x5D93D2, 0x63B350, 0x10000 },
          { "Font",      0x5D9440, 0x63BC08, 0x2800 },
          { "ScreenSet", 0x5D9402, 0x63B7A8, 0x26C00 } }, 10,
        { 0x40189A, 0x4018A9, 0x4018B8 }, 0x4687A0,
        { 0x5E62F0, 0x5E6360, 0x5E63B8, 0x5E6420, 0x5E6478, 0x5E6660, 0x5EAB38, 0x5EB0F8, 0x5EB658, 0x5EBE68, 0x5EC7D0, 0x5ECB90 }, 12 },
};
static const MemoryAddresses *A;

/* Budget chosen for each site (0 = unchanged). */
static DWORD Chosen[MaximumBudgets];
static int LogUsage;   /* [Memory] LogUsage */

#define MaximumBudgetBytes (1024u << 20)   /* 1 GB: the process only has 2 GB of addresses without LAA */

/* ================================================================ Re-applied budgets (slot 4) */
typedef void (ThisCall *SetBudgetFunctionType)(void *, DWORD);
static SetBudgetFunctionType OriginalSetBudget;
static volatile LONG SetBudgetCalls;

static void ThisCall SetBudgetHook(void *Manager, DWORD Budget) {
    DWORD Value = Budget;
    const char *Name = "?";
    for (int Index = 0; Index < A->BudgetCount; Index++)
        if (A->Budgets[Index].Manager == (DWORD)Manager) {
            Name = A->Budgets[Index].Name;
            if (Chosen[Index]) Value = Chosen[Index];
            break;
        }
    if (InterlockedIncrement(&SetBudgetCalls) <= 6)
        Log("Game re-applies the %s budget: %lu KB -> %lu KB", Name, Budget >> 10, Value >> 10);
    OriginalSetBudget(Manager, Value);
}

/* ================================================================ Budgets set at run time (vtable slot 1)
   Fact (0.0.4 in-game test, PIH, and Suffering.rfl 0x100B2160): when a level loads, the game DLL reads the
   level's "Level Properties" object and, for properties 0, 1, 2 and 4 (floats, MB) greater than zero, calls
   slot 1 of the Texture, Model, Animation and Lighting managers (e.g. 10.05 / 6.75 / 3.97 / 0.675 MB in the
   level tested; class defaults 6.5 / 4 / 2.65 / 0.45 in PIH). Property 3 goes to another object (slot 0x30).
   Every call through slot 1 of a manager vtable comes here: the caller is logged, and a budget lower than the
   configured one is kept at the configured value (a level asking for more still gets more). */
static volatile LONG VirtualSetBudgetCalls[MaximumBudgets];
static void ThisCall VirtualSetBudgetHook(void *Manager, DWORD Budget) {
    DWORD Value = Budget;
    for (int Index = 0; Index < A->BudgetCount; Index++)
        if (A->Budgets[Index].Manager == (DWORD)Manager) {
            if (Index == 1 && LogUsage) RiotDiagnosticsLogMemory("at level load");   /* levels set Texture first */
            if (Chosen[Index] && Value < Chosen[Index]) Value = Chosen[Index];
            if (InterlockedIncrement(&VirtualSetBudgetCalls[Index]) <= 4) {
                char Caller[MAX_PATH + 32]; AddressName((DWORD)__builtin_return_address(0), Caller, sizeof Caller);
                Log("[memory] %s budget set to %lu KB by %s%s", A->Budgets[Index].Name, Budget >> 10, Caller,
                    Value != Budget ? " (kept at the configured budget)" : "");
            }
            break;
        }
    OriginalSetBudget(Manager, Value);
}

/* ================================================================ Usage log */
static DWORD WINAPI UsageMonitor(LPVOID Unused) {
    (void)Unused;
    DWORD Peak[MaximumBudgets] = {0}, LastBudget[MaximumBudgets] = {0};
    for (;;) {
        for (int Index = 0; Index < A->BudgetCount; Index++) {
            const DWORD *Manager = (const DWORD *)A->Budgets[Index].Manager;
            DWORD Budget = Manager[1], Used = Manager[2];
            if (LastBudget[Index] && Budget != LastBudget[Index])
                Log("[memory] %s budget changed: %lu -> %lu KB", A->Budgets[Index].Name, LastBudget[Index] >> 10, Budget >> 10);
            LastBudget[Index] = Budget;
            if (Used > Peak[Index]) {
                Peak[Index] = Used;
                Log("[memory] %s peak: %lu of %lu KB", A->Budgets[Index].Name, Used >> 10, Budget >> 10);
            }
        }
        Sleep(2000);
    }
    return 0;
}

/* ================================================================ Apply */
void RiotMemoryApply(void) {
    if (CurrentGame == GameUnknown) return;
    A = &Addresses[CurrentGame];

    for (int Index = 0; Index < A->BudgetCount; Index++) {
        const BudgetSite *Site = &A->Budgets[Index];
        float Megabytes = IniFloat("Memory", Site->Name, 0.0f);
        if (Megabytes <= 0.0f) continue;
        DWORD Bytes = Megabytes >= 1024.0f ? MaximumBudgetBytes : (DWORD)(Megabytes * 1048576.0f);
        if (Bytes < Site->Original || Bytes > MaximumBudgetBytes) {
            Log("Memory.%s=%g is invalid (use %.2f to 1024 MB, or 0 for the original)", Site->Name, Megabytes, Site->Original / 1048576.0);
            continue;
        }
        if (Bytes == Site->Original) continue;
        unsigned char Expected[10] = {0x68}, Push[5] = {0x68};
        memcpy(Expected + 1, &Site->Original, 4);
        Expected[5] = 0xB9; memcpy(Expected + 6, &Site->Manager, 4);   /* mov ecx, Manager */
        memcpy(Push + 1, &Bytes, 4);
        int Ok = BytesAre(Site->Push, Expected, sizeof Expected) && WriteCode(Site->Push, Push, sizeof Push);
        Log("%s budget %lu KB -> %lu KB: %s", Site->Name, Site->Original >> 10, Bytes >> 10, Ok ? "ok" : "FAILED (unexpected bytes)");
        if (Ok) Chosen[Index] = Bytes;
    }

    /* Slot 4 re-applies Model, Texture and Animation: redirect its calls only when one of them changed. */
    if (Chosen[0] || Chosen[1] || Chosen[2]) {
        OriginalSetBudget = (SetBudgetFunctionType)A->Setter;
        int Redirected = 0;
        for (int Index = 0; Index < 3; Index++) Redirected += RedirectCall(A->SetterCalls[Index], A->Setter, (DWORD)SetBudgetHook);
        Log("Budgets re-applied by the game: %d/3 calls redirected", Redirected);
    }

    /* Run-time budget changes through slot 1 of the manager vtables: installed whenever a budget was raised or
       usage logging is on, so the log always says who sets the budgets. */
    int AnyChosen = 0;
    for (int Index = 0; Index < A->BudgetCount; Index++) AnyChosen |= Chosen[Index] != 0;
    LogUsage = IniInt("Memory", "LogUsage", 0);
    if (AnyChosen || LogUsage) {
        OriginalSetBudget = (SetBudgetFunctionType)A->Setter;
        int Hooked = 0;
        for (int Index = 0; Index < A->SetterVtableCount; Index++)
            Hooked += ReplaceVtableSlot(A->SetterVtables[Index], 1, A->Setter, (DWORD)VirtualSetBudgetHook);
        Log("Budget setter watched in %d/%d manager vtables", Hooked, A->SetterVtableCount);
    }

    if (LogUsage) {
        HANDLE Thread = CreateThread(NULL, 0, UsageMonitor, NULL, 0, NULL);
        if (Thread) CloseHandle(Thread);
        Log("Memory usage log on: every new peak is logged as [memory]");
    }
}
