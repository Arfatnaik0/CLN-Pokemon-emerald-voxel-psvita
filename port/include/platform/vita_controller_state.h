#ifndef GUARD_PLATFORM_VITA_CONTROLLER_STATE_H
#define GUARD_PLATFORM_VITA_CONTROLLER_STATE_H

#include <stdbool.h>

struct VitaControllerShortcutState
{
    bool squareHeld;
    bool triangleHeld;
    bool profilerChordTriggered;
    bool profilerChordConsumed;
};

static inline void VitaControllerShortcutState_Reset(
    struct VitaControllerShortcutState *state)
{
    state->squareHeld = false;
    state->triangleHeld = false;
    state->profilerChordTriggered = false;
    state->profilerChordConsumed = false;
}

#endif // GUARD_PLATFORM_VITA_CONTROLLER_STATE_H
