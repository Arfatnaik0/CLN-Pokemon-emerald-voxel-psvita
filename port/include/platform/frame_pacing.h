#ifndef GUARD_PLATFORM_FRAME_PACING_H
#define GUARD_PLATFORM_FRAME_PACING_H

#include <stdbool.h>
#include <stdint.h>

#define FRAME_PACING_BATCH_HISTOGRAM_BUCKETS 5

typedef struct FramePacingState
{
    double accumulator;
} FramePacingState;

typedef enum FramePacingPresentationMode
{
    /* Keep one call at the end of each platform-loop iteration. */
    FRAME_PACING_PRESENT_PER_ITERATION,
    /* Submit one present for each game frame serviced by the platform loop. */
    FRAME_PACING_PRESENT_PER_SERVICED_FRAME,
} FramePacingPresentationMode;

typedef struct FramePacingBatch
{
    uint32_t servicedFrames;
    FramePacingPresentationMode presentationMode;
} FramePacingBatch;

static inline void FramePacing_BeginBatch(FramePacingBatch *batch,
                                          FramePacingPresentationMode mode)
{
    batch->servicedFrames = 0;
    batch->presentationMode = mode;
}

static inline bool FramePacing_ShouldPresentAfterServicedFrame(
    const FramePacingBatch *batch)
{
    return batch->presentationMode == FRAME_PACING_PRESENT_PER_SERVICED_FRAME;
}

static inline bool FramePacing_ShouldPresentAtBatchEnd(
    const FramePacingBatch *batch)
{
    return batch->presentationMode == FRAME_PACING_PRESENT_PER_ITERATION;
}

static inline bool FramePacing_ShouldRedrawDirtyDisplay(
    bool displayDirty, const FramePacingBatch *batch)
{
    return displayDirty && batch->servicedFrames == 0;
}

static inline void FramePacing_RecordServicedFrame(FramePacingBatch *batch)
{
    batch->servicedFrames++;
}

/* Keep the platform loop's existing long-stall clamp explicit and testable. */
static inline double FramePacing_ClampDelta(double deltaTime, double timestep)
{
    if (deltaTime > timestep * 5.0)
        return timestep;
    return deltaTime;
}

static inline void FramePacing_Accumulate(FramePacingState *state,
                                          double deltaTime,
                                          double timestep)
{
    state->accumulator += FramePacing_ClampDelta(deltaTime, timestep);
}

static inline bool FramePacing_HasStep(const FramePacingState *state,
                                       double timestep)
{
    return state->accumulator >= timestep;
}

static inline void FramePacing_ConsumeStep(FramePacingState *state,
                                           double timestep)
{
    state->accumulator -= timestep;
}

static inline unsigned int FramePacing_BatchHistogramBucket(uint32_t frames)
{
    return frames < FRAME_PACING_BATCH_HISTOGRAM_BUCKETS - 1
         ? frames
         : FRAME_PACING_BATCH_HISTOGRAM_BUCKETS - 1;
}

#endif // GUARD_PLATFORM_FRAME_PACING_H
