/*
 * RiotSystem - small fixes around Windows: debug output, registry flushes, DirectPlay check, free disk space
 *
 * Licence: MIT
 */
#ifndef RiotSystemHeader
#define RiotSystemHeader

/* Reads [System] from RiotFix.ini and applies the fixes. Must run from DllMain (the DirectPlay check runs in the
   executable's static constructors). */
void RiotSystemApply(void);
/* Logs what the fixes did during the session (called when the game closes). */
void RiotSystemReport(void);

#endif
