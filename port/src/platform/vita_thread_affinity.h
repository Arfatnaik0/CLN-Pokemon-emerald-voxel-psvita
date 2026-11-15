#ifndef GUARD_VITA_THREAD_AFFINITY_H
#define GUARD_VITA_THREAD_AFFINITY_H

#include <stdbool.h>

/* Vita user-core affinity is an A/B diagnostic. The default build leaves the
 * scheduler unchanged; enabled builds pin renderer/game/EASY draw threads. */
bool VitaThread_PinCurrentToCore(int core);

#endif
