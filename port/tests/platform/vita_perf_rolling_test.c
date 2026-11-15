#include <assert.h>

#include "platform/vita_perf_rolling.h"

int main(void)
{
    struct VitaPerfRollingWindow window;
    struct VitaPerfRollingSummary summary;
    struct VitaPerfRollingWindow ppuWindow;
    struct VitaPerfRollingSummary ppuSummary;
    struct VitaPerfRollingWindow audioSupplyWindow;
    struct VitaPerfRollingSummary audioSupplySummary;
    struct VitaPerfRollingWindow audioQueueWindow;
    struct VitaPerfRollingSummary audioQueueSummary;

    VitaPerfRolling_Reset(&window);
    VitaPerfRolling_Summarize(&window, &summary);
    assert(summary.count == 0);
    assert(summary.averageUs == 0);
    assert(summary.medianUs == 0);
    assert(summary.p99Us == 0);

    /* Early runs keep a partial window and use nearest-rank p99. */
    VitaPerfRolling_Add(&window, 10, 0);
    VitaPerfRolling_Add(&window, 30, 1);
    VitaPerfRolling_Summarize(&window, &summary);
    assert(summary.count == 2);
    assert(summary.taggedSamples == 1);
    assert(summary.averageUs == 20);
    assert(summary.medianUs == 20);
    assert(summary.p99Us == 30);

    VitaPerfRolling_Reset(&window);

    for (uint32_t value = 1; value <= VITA_PERF_ROLLING_CAPACITY; value++)
        VitaPerfRolling_Add(&window, value, value & 1);

    VitaPerfRolling_Summarize(&window, &summary);
    assert(summary.count == 300);
    assert(summary.taggedSamples == 150);
    assert(summary.averageUs == 150);
    assert(summary.medianUs == 150); /* midpoint of 150 and 151, rounded down */
    assert(summary.p99Us == 297);    /* nearest rank, ceil(0.99 * 300) */

    /* The 301st sample evicts exactly the oldest duration and its tag. */
    VitaPerfRolling_Add(&window, 301, 0);
    VitaPerfRolling_Summarize(&window, &summary);
    assert(summary.count == 300);
    assert(summary.taggedSamples == 149);
    assert(summary.averageUs == 151);
    assert(summary.medianUs == 151);
    assert(summary.p99Us == 298);

    /* PPU rolling samples are complete DrawFrame durations on every path.
     * A VCOUNT-prefix sample includes both the parallel prefix and serial tail. */
    VitaPerfRolling_Reset(&ppuWindow);
    VitaPerfRolling_AddPpuFrame(&ppuWindow, 3200, 0); /* serial */
    VitaPerfRolling_AddPpuFrame(&ppuWindow, 1800, 1); /* full parallel */
    VitaPerfRolling_AddPpuFrame(&ppuWindow, 2900, 1); /* VCOUNT prefix */
    VitaPerfRolling_Summarize(&ppuWindow, &ppuSummary);
    assert(ppuSummary.count == 3);
    assert(ppuSummary.taggedSamples == 2);
    assert(ppuSummary.averageUs == 2633);
    assert(ppuSummary.medianUs == 2900);
    assert(ppuSummary.p99Us == 3200);

    /* Audio supply and SDL_QueueAudio call time use independent rolling buckets. */
    VitaPerfRolling_Reset(&audioSupplyWindow);
    VitaPerfRolling_Reset(&audioQueueWindow);
    VitaPerfRolling_Add(&audioSupplyWindow, 900, 0);
    VitaPerfRolling_Add(&audioQueueWindow, 35, 0);
    VitaPerfRolling_Summarize(&audioSupplyWindow, &audioSupplySummary);
    VitaPerfRolling_Summarize(&audioQueueWindow, &audioQueueSummary);
    assert(audioSupplySummary.count == 1 && audioSupplySummary.medianUs == 900);
    assert(audioQueueSummary.count == 1 && audioQueueSummary.medianUs == 35);
    return 0;
}
