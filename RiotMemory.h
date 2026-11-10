/*
 * RiotMemory - resource cache budgets
 *
 * Licence: MIT
 */
#ifndef RiotMemoryHeader
#define RiotMemoryHeader

/* Reads [Memory] from RiotFix.ini and raises the budgets of the game's resource managers. Must run from DllMain,
   before the executable's static constructors, which pass the budgets to the managers. */
void RiotMemoryApply(void);

#endif
