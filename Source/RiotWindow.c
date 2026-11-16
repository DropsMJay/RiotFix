/*
 * RiotWindow - the game's window
 *
 * The engine gives the window its size in one place (PIH SUFFERING.EXE+0x4B3BF0, TTB SufferingTTB.exe+0x4BC630):
 * it reads the window's style and extended style (GetWindowLongA -16 and -20), and when the display flags say full
 * screen (bit 0 of PIH 0x5C6CE8 / TTB 0x639EC8 clear) it forces WS_POPUP | WS_VISIBLE; then it grows the rectangle
 * by the border with AdjustWindowRectEx and calls SetWindowPos (PIH +0x4B3C79, TTB +0x4BC6B9). That call is the one
 * RiotFix takes over: with [Window] Borderless the window keeps the size of the chosen resolution, with no border
 * and no title bar, centred on the screen.
 *
 * The rest is done once per frame, from the window handle that call gives: the title, the cursor held inside the
 * window while it has the focus, and the sleep that keeps the game from using a whole processor core in the
 * background.
 *
 * Licence: MIT
 */
#include "RiotCore.h"
#include "RiotWindow.h"
#include "RiotTiming.h"
#include <stdio.h>

typedef struct {
    DWORD SetWindowPosCall;   /* the call that gives the window its size ("call dword ptr [SetWindowPos]") */
    DWORD SetWindowPosSlot;   /* the import slot that call reads */
    DWORD DisplayFlags;       /* bit 0 set = windowed */
    DWORD DisplayMode;        /* two ints: the width and height the game draws */
    const char *Title;
} WindowAddresses;

static const WindowAddresses Addresses[GameCount] = {
    [GamePIH] = { 0x4B3C79, 0x58B368, 0x5C6CE8, 0x5C6CEC, "The Suffering" },
    [GameTTB] = { 0x4BC6B9, 0x5E53A4, 0x639EC8, 0x639ECC, "The Suffering: Ties That Bind" },
};
static const WindowAddresses *A;

static HWND GameWindow;
static int Borderless, ConfineCursor, HideCursor, IdleWhenInactive;
static char WindowTitle[128];

static int Windowed(void) { return *(const DWORD *)A->DisplayFlags & 1; }

/* ================================================================ Borderless */
static BOOL WINAPI WindowPos(HWND Window, HWND After, int X, int Y, int Width, int Height, UINT Flags) {
    GameWindow = Window;
    const int *Mode = (const int *)A->DisplayMode;
    if (!Borderless || !Windowed() || Mode[0] <= 0 || Mode[1] <= 0 || Mode[0] > 32767 || Mode[1] > 32767)
        return SetWindowPos(Window, After, X, Y, Width, Height, Flags);
    SetWindowLongA(Window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    const int ScreenWidth = GetSystemMetrics(SM_CXSCREEN), ScreenHeight = GetSystemMetrics(SM_CYSCREEN);
    int Left = (ScreenWidth - Mode[0]) / 2, Top = (ScreenHeight - Mode[1]) / 2;
    if (Left < 0) Left = 0;
    if (Top < 0) Top = 0;
    static int Logged;
    if (!Logged++) Log("[window] borderless %dx%d at %d,%d", Mode[0], Mode[1], Left, Top);
    return SetWindowPos(Window, NULL, Left, Top, Mode[0], Mode[1], SWP_NOZORDER | SWP_FRAMECHANGED);
}
static void *const WindowPosSlot = (void *)WindowPos;

/* ================================================================ Once per frame */
static void WindowFrame(void) {
    if (!GameWindow || !IsWindow(GameWindow)) return;
    static int TitleSet;
    if (!TitleSet++ && WindowTitle[0]) SetWindowTextA(GameWindow, WindowTitle);

    const int Active = GetForegroundWindow() == GameWindow;
    static int WasActive = -1, CursorHidden;

    if (ConfineCursor && (Active != WasActive || Active)) {
        if (Active) {
            RECT Client;
            if (GetClientRect(GameWindow, &Client)) {
                POINT TopLeft = { Client.left, Client.top }, BottomRight = { Client.right, Client.bottom };
                if (ClientToScreen(GameWindow, &TopLeft) && ClientToScreen(GameWindow, &BottomRight)) {
                    RECT Screen = { TopLeft.x, TopLeft.y, BottomRight.x, BottomRight.y };
                    ClipCursor(&Screen);
                }
            }
        } else ClipCursor(NULL);
    }

    /* The counter of ShowCursor is shared with the game, so each change is made once. */
    if (HideCursor && Active && !CursorHidden) { while (ShowCursor(FALSE) >= 0) {} CursorHidden = 1; }
    else if (HideCursor && !Active && CursorHidden) { while (ShowCursor(TRUE) < 0) {} CursorHidden = 0; }

    WasActive = Active;
    if (IdleWhenInactive && !Active) Sleep(50);
}

/* ================================================================ Apply */
void RiotWindowApply(void) {
    if (CurrentGame == GameUnknown) return;
    A = &Addresses[CurrentGame];

    Borderless = IniInt("Window", "Borderless", 1);
    ConfineCursor = IniInt("Window", "ConfineCursor", 1);
    HideCursor = IniInt("Window", "HideCursor", 1);
    IdleWhenInactive = IniInt("Window", "IdleWhenInactive", 1);
    GetPrivateProfileStringA("Window", "Title", A->Title, WindowTitle, sizeof WindowTitle, IniPath);
    /* [Window] ShowVersion: the version after the game's name, so a window tells which RiotFix it is running.
       An empty title means the game's own, which The Suffering leaves blank, so the game's name is put back first. */
    if (IniInt("Window", "ShowVersion", 1)) {
        static const char Suffix[] = " - RiotFix " RiotFixBuildVersion;
        if (!WindowTitle[0]) {
            strncpy(WindowTitle, A->Title, sizeof WindowTitle - 1);
            WindowTitle[sizeof WindowTitle - 1] = 0;
        }
        const size_t Length = strlen(WindowTitle);
        if (!strstr(WindowTitle, "RiotFix") && Length + sizeof Suffix <= sizeof WindowTitle)
            memcpy(WindowTitle + Length, Suffix, sizeof Suffix);
    }

    const int Repointed = RepointImportUse(A->SetWindowPosCall, A->SetWindowPosSlot, &WindowPosSlot);
    Log("Window placed by RiotFix (borderless %s): %s", Borderless ? "on" : "off",
        Repointed ? "ok" : "FAILED (unexpected bytes)");
    if (!Repointed) { Borderless = 0; return; }
    RiotTimingOnFrame(WindowFrame);
    Log("Window title \"%s\", cursor %s, %s when inactive", WindowTitle,
        ConfineCursor ? (HideCursor ? "held inside the window and hidden" : "held inside the window")
                      : (HideCursor ? "hidden" : "as the game leaves it"),
        IdleWhenInactive ? "sleeps" : "runs on");
}
