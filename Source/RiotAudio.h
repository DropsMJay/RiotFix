/*
 * RiotAudio - DirectSound diagnostics (and, later, its fixes)
 *
 * Licence: MIT
 */
#ifndef RiotAudioHeader
#define RiotAudioHeader

#include "RiotCore.h"

/* Reads [Debug] LogAudio from RiotFix.ini and watches DirectSound. */
void RiotAudioApply(void);
/* One line with the session's totals, written when the game ends. */
void RiotAudioReport(void);

#endif
