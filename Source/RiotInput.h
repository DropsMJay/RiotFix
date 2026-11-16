/*
 * RiotInput - mouse timing and the menu cursor's input
 *
 * Licence: MIT
 */
#ifndef RiotInputHeader
#define RiotInputHeader

/* Reads [Mouse] and [Cursor] Direct from RiotFix.ini and applies them to CurrentGame (before RiotTimingApply). */
void RiotInputApply(void);

/* Called with each IDirectInput8 the game makes (RiotFix as dinput8.dll), to watch the keyboard device. */
void RiotInputWatchDirectInput(void *DirectInput);

/* The engine's input manager, as the action query hands it over; NULL until the game has asked about an action. */
void *RiotInputManager(void);

/* 1 while a controller is the last thing used, so the mouse cursor is not drawn. */
int RiotInputHidesCursor(void);

#endif
