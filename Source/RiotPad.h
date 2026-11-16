/* RiotPad - the console's pad as a device of the engine's own kind. Licence: MIT */
#ifndef RiotPadHeader
#define RiotPadHeader

/* Puts the pad in the engine's device array ([Controller] NativeDevice). Called at attach, before the game runs. */
void RiotPadApply(void);

/* 1 once the pad is a device of the engine. */
int RiotPadIsNative(void);

/* Called for every question the engine asks about an action, so that an action the pad is bound to can be followed
 * from the button to the answer ([Debug] LogBindings). */
void RiotPadWatchQuery(int Set, DWORD Action, int Mode, float Answer);

/* Turns that watching on, and with it the couple-of-seconds summary it writes. */
void RiotPadWatch(void);

/* Where the device array now lives, and how many devices it holds; NULL while the game's own array is still in use. */
const void *RiotPadArray(void);
int RiotPadDeviceCount(void);

#endif
