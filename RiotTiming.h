/*
 * RiotTiming - threads, locks and frame timing
 *
 * Licence: MIT
 */
#ifndef RiotTimingHeader
#define RiotTimingHeader

/* Reads [Timing] from RiotFix.ini and applies the fixes to CurrentGame. */
void RiotTimingApply(void);

/* Functions run once per frame, before the game's frame function (registered before RiotTimingApply runs). */
typedef void (*RiotFrameCallback)(void);
void RiotTimingOnFrame(RiotFrameCallback Callback);

#endif
