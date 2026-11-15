#ifndef GUARD_VITA_PERF_ROLLING_H
#define GUARD_VITA_PERF_ROLLING_H

#include <stdint.h>

#define VITA_PERF_ROLLING_CAPACITY 300

/* `taggedSamples` is used by the PPU series to count parallel and serial
 * samples separately. Other series always pass a zero tag. */
struct VitaPerfRollingWindow
{
    uint32_t durationUs[VITA_PERF_ROLLING_CAPACITY];
    uint8_t tags[VITA_PERF_ROLLING_CAPACITY];
    uint16_t next;
    uint16_t count;
    uint16_t taggedSamples;
    uint64_t totalUs;
};

struct VitaPerfRollingSummary
{
    uint16_t count;
    uint16_t taggedSamples;
    uint64_t averageUs;
    uint32_t medianUs;
    uint32_t p99Us;
};

static inline void VitaPerfRolling_Reset(struct VitaPerfRollingWindow *window)
{
    for (uint16_t i = 0; i < VITA_PERF_ROLLING_CAPACITY; i++)
    {
        window->durationUs[i] = 0;
        window->tags[i] = 0;
    }
    window->next = 0;
    window->count = 0;
    window->taggedSamples = 0;
    window->totalUs = 0;
}

static inline void VitaPerfRolling_Add(struct VitaPerfRollingWindow *window,
                                       uint32_t durationUs, uint8_t tag)
{
    if (window->count == VITA_PERF_ROLLING_CAPACITY)
    {
        window->totalUs -= window->durationUs[window->next];
        if (window->tags[window->next] != 0)
            window->taggedSamples--;
    }
    else
    {
        window->count++;
    }

    window->durationUs[window->next] = durationUs;
    window->tags[window->next] = tag != 0;
    window->totalUs += durationUs;
    if (tag != 0)
        window->taggedSamples++;
    window->next = (window->next + 1) % VITA_PERF_ROLLING_CAPACITY;
}

/* PPU samples are always one complete DrawFrame duration. The tag records
 * the path used; dispatch and worker timings belong in their own statistics. */
static inline void VitaPerfRolling_AddPpuFrame(
    struct VitaPerfRollingWindow *window, uint32_t fullDrawFrameUs,
    uint8_t parallelPath)
{
    VitaPerfRolling_Add(window, fullDrawFrameUs, parallelPath);
}

static inline void VitaPerfRolling_Summarize(
    const struct VitaPerfRollingWindow *window,
    struct VitaPerfRollingSummary *summary)
{
    uint32_t sorted[VITA_PERF_ROLLING_CAPACITY];

    summary->count = window->count;
    summary->taggedSamples = window->taggedSamples;
    summary->averageUs = window->count ? window->totalUs / window->count : 0;
    summary->medianUs = 0;
    summary->p99Us = 0;
    if (window->count == 0)
        return;

    for (uint16_t i = 0; i < window->count; i++)
        sorted[i] = window->durationUs[i];
    for (uint16_t i = 1; i < window->count; i++)
    {
        uint32_t value = sorted[i];
        uint16_t j = i;
        while (j > 0 && sorted[j - 1] > value)
        {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = value;
    }

    uint16_t middle = window->count / 2;
    if (window->count & 1)
        summary->medianUs = sorted[middle];
    else
        summary->medianUs = (uint32_t)(((uint64_t)sorted[middle - 1]
                                      + sorted[middle]) / 2);

    /* Nearest-rank percentile: rank = ceil(0.99 * N), one-based. */
    uint16_t p99Rank = (uint16_t)(((uint32_t)window->count * 99 + 99) / 100);
    summary->p99Us = sorted[p99Rank - 1];
}

#endif // GUARD_VITA_PERF_ROLLING_H
