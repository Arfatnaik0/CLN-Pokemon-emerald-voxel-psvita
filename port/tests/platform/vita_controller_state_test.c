#include <assert.h>

#include "platform/vita_controller_state.h"

int main(void)
{
    struct VitaControllerShortcutState state = {
        .squareHeld = true,
        .triangleHeld = true,
        .profilerChordTriggered = true,
        .profilerChordConsumed = true,
    };

    VitaControllerShortcutState_Reset(&state);

    assert(!state.squareHeld);
    assert(!state.triangleHeld);
    assert(!state.profilerChordTriggered);
    assert(!state.profilerChordConsumed);
    return 0;
}
