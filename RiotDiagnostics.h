/*
 * RiotDiagnostics - crash log, minidump and process memory report
 *
 * Licence: MIT
 */
#ifndef RiotDiagnosticsHeader
#define RiotDiagnosticsHeader

/* Logs whether the exe is Large Address Aware and the process memory at start-up. */
void RiotDiagnosticsLogStartup(void);
/* Logs free addresses, the largest free block and private bytes (current and peak) of the process. */
void RiotDiagnosticsLogMemory(const char *When);
/* Installs the exception handlers when [Debug] CrashLog=1 (the default). */
void RiotDiagnosticsApply(void);

#endif
