/*
 * RiotAccessibility - what makes the game easier to see and to use
 *
 * Interface size (HUD and mouse cursor) and text outline, all from the [Accessibility] section.
 * The layout itself (4:3 anchoring, menu box) stays in RiotDisplay; this module only asks it
 * where an element belongs and how the text passes work.
 *
 * Licence: MIT
 */
#ifndef RiotAccessibilityHeader
#define RiotAccessibilityHeader

#include "RiotCore.h"

/* One call of the font's slot 5. Stack arguments (both games, from the calls at PIH Suffering.rfl+0x6AA5E and TTB
   SufferingTTB.rfl+0x12E56A): 0 font data, 1 text, 2 x, 3 y, 4 out pointer, 5 scale x, 6 scale y, 7 colours (four
   ARGB corners), 8 flags, 9 matrix (TTB only: the interface's text leaf 0x43F7A0 passes 0 as flags and its scaled
   4 x 4 matrix as the 10th argument). PIH has 9 arguments, TTB 10 ("ret 24h" / "ret 28h"). */
typedef struct { void *Self; DWORD Arguments[10]; } TextCall;

/* Reads [Accessibility]. Called before RiotDisplayApply, which asks for the interface size. */
void RiotAccessibilityRead(void);
/* Hooks the text outline and the cursor. Called after RiotDisplayApply. */
void RiotAccessibilityApply(void);

/* [Accessibility] InterfaceScale: 1 = the game's size. Also the cursor's size (on top of its automatic size). */
float RiotInterfaceScale(void);
/* Moves a point of the HUD out from the side it is anchored to and from the third of the screen it is in. */
void RiotScalePoint(float *X, float *Y, int Anchor, float CentreY);
/* Hooks the font's draw method (the text outline and the widescreen text anchoring share it). */
int RiotTextHook(void);

#endif
