/*
 * RiotSdl - loads SDL3, the ground for the audio path and for controller support
 *
 * Licence: MIT
 */
#ifndef RiotSdlHeader
#define RiotSdlHeader

#include "RiotCore.h"

/* Reads [Sdl] Enable from RiotFix.ini. Loads SDL3.dll from the folder RiotFix itself was loaded from, starts it
   with its DirectInput joystick backend off (RiotFix is usually dinput8.dll itself), and writes what it found. */
void RiotSdlApply(void);
/* Whether a controller was found and opened. */
int RiotSdlHasPad(void);
/* Refreshes the controller's state. Called once a frame; the readers below do no work of their own. */
void RiotSdlReadPad(void);
/* The controller's state, as SDL reports it: axes -32768..32767 (0 left X, 1 left Y, 2 right X, 3 right Y,
   4 left trigger, 5 right trigger), buttons 0 or 1 (0 south, 1 east, 2 west, 3 north, 4 back, 6 start,
   9 left shoulder, 10 right shoulder, 11..14 the d-pad up, down, left, right). */
int RiotSdlAxis(int Axis);
int RiotSdlButton(int Button);
/* Drives the controller's two motors, 0..65535 each: Low is the big low-frequency one, High the small
   high-frequency one. Called once a frame by the pad device's rumble slot; a repeat of the levels already playing
   costs nothing, so the caller need not track them. */
void RiotSdlRumble(unsigned Low, unsigned High);
int RiotSdlButtonLabel(int Button);
/* Closes SDL and releases the library. */
void RiotSdlStop(void);

#endif
