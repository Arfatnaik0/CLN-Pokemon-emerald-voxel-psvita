#ifndef GUARD_GBA_EASY_DRAW_WORKERS_H
#define GUARD_GBA_EASY_DRAW_WORKERS_H

#include <stdbool.h>

typedef void (*GbaEasyDrawRangeFn)(void *context, int firstLine, int endLine);

/* Run one independent row range on the render thread and two persistent workers. */
bool GbaEasyDrawWorkers_Run(GbaEasyDrawRangeFn drawRange, void *context, int height);
void GbaEasyDrawWorkers_Shutdown(void);

#ifdef EASY_DRAW_TEST_PARALLEL
void GbaEasyDrawWorkers_TestFailAfter(int successfulThreadCreates);
#endif

#endif
