/*
 * RiotDisplay - game window, video modes, widescreen layout and the HUD's place on the screen
 *
 * The functions below are what RiotAccessibility (interface size, cursor, text outline) asks of the layout:
 * the screen it is drawing on, where an element belongs, and the text passes the widescreen anchoring uses.
 *
 * Licence: MIT
 */
#ifndef RiotDisplayHeader
#define RiotDisplayHeader

#include "RiotCore.h"
#include "RiotAccessibility.h"

/* Reads [Display] and [Widescreen] from RiotFix.ini and applies the fixes to CurrentGame. */
void RiotDisplayApply(void);

/* ---------------------------------------------------------------- The screen */
float RiotScreenWidth(void);
float RiotScreenHeight(void);
/* 1 while a game is running and the HUD (not a menu) is being drawn. */
int RiotPlayLayout(void);
/* 1 while menus are drawn inside a centred 4:3 box; fills the box's left edge and the aspect difference. */
int RiotMenuBox(float *Offset, float *Difference);

/* ---------------------------------------------------------------- Screen manager (the cursor's owner) */
void *RiotScreenManagerObject(void);
void RiotNeedScreenManager(void);     /* asks for the screen manager to be watched */
void RiotCursorPass(int On);          /* inside the cursor's own drawing: the quad is left as it was placed */

/* ---------------------------------------------------------------- Text passes (widescreen anchoring)
   Each string is measured first (its quads are recorded and drawn empty), then drawn with one anchor for all its
   letters. The same string drawn again in the same frame (its shadow) reuses the anchor. */
int RiotTextNeedsAnchor(void);
int RiotTextIdleNow(void);                                  /* 1 outside the measuring and drawing passes */
int RiotTextAnchorFor(DWORD Hash, float Y);                 /* AnchorNone when this frame has not seen the string */
void RiotTextAnchorRemember(DWORD Hash, float Y, int Anchor);
void RiotTextMeasureBegin(void);
int RiotTextMeasureEnd(void);                               /* the anchor of what was measured */
void RiotTextUseAnchor(int Anchor);
void RiotTextIdle(void);

/* ---------------------------------------------------------------- Diagnostics */
int RiotLogHudOn(void);                                     /* [Debug] LogHud */
void RiotLogFont(const TextCall *Call, DWORD Caller);

#endif
