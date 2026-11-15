#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "platform/frame_pacing.h"

static uint32_t ServiceAvailableFrames(FramePacingState *state,
                                       double elapsed,
                                       double timestep)
{
    uint32_t serviced = 0;

    FramePacing_Accumulate(state, elapsed, timestep);
    while (FramePacing_HasStep(state, timestep))
    {
        FramePacing_ConsumeStep(state, timestep);
        serviced++;
    }
    return serviced;
}

static uint32_t ServiceLegacyAccumulator(double *accumulator,
                                         double elapsed,
                                         double timestep)
{
    uint32_t serviced = 0;

    if (elapsed > timestep * 5.0)
        elapsed = timestep;
    *accumulator += elapsed;
    while (*accumulator >= timestep)
    {
        *accumulator -= timestep;
        serviced++;
    }
    return serviced;
}

static uint32_t CountBatchPresentations(FramePacingPresentationMode mode,
                                        uint32_t servicedFrames,
                                        bool displayDirty)
{
    FramePacingBatch batch;
    uint32_t presents = 0;

    FramePacing_BeginBatch(&batch, mode);
    for (uint32_t i = 0; i < servicedFrames; i++)
    {
        if (FramePacing_ShouldPresentAfterServicedFrame(&batch))
            presents++;
        FramePacing_RecordServicedFrame(&batch);
    }
    if (FramePacing_ShouldPresentAtBatchEnd(&batch)
     || FramePacing_ShouldRedrawDirtyDisplay(displayDirty, &batch))
        presents++;

    assert(batch.servicedFrames == servicedFrames);
    return presents;
}

static void TestHelperMatchesPreviousAccumulator(void)
{
    const double timestep = 1.0 / 60.0;
    const double elapsed[] = {
        0.0, timestep, timestep * 2.0, timestep * 5.0,
        timestep * 5.01, timestep * 6.0, 0.001, 0.050,
        timestep, timestep * 4.25, 0.0,
    };
    FramePacingState state = {0.0};
    double legacyAccumulator = 0.0;

    for (uint32_t i = 0; i < sizeof(elapsed) / sizeof(elapsed[0]); i++)
    {
        uint32_t legacyFrames = ServiceLegacyAccumulator(&legacyAccumulator,
                                                          elapsed[i], timestep);
        uint32_t helperFrames = ServiceAvailableFrames(&state, elapsed[i], timestep);
        assert(helperFrames == legacyFrames);
        assert(state.accumulator == legacyAccumulator);
    }
}

static void TestSingleTickPerOnTimeBatch(void)
{
    const double timestep = 1.0 / 60.0;
    FramePacingState state = {0.0};

    for (uint32_t i = 0; i < 60; i++)
        assert(ServiceAvailableFrames(&state, timestep, timestep) == 1);
    assert(state.accumulator == 0.0);
}

static void TestCatchupBurstAndLongStallClamp(void)
{
    const double timestep = 1.0 / 60.0;
    FramePacingState burst = {0.0};
    FramePacingState longStall = {0.0};

    assert(ServiceAvailableFrames(&burst, timestep * 3.0, timestep) == 3);
    assert(burst.accumulator < timestep);
    assert(ServiceAvailableFrames(&longStall, timestep * 6.0, timestep) == 1);
    assert(longStall.accumulator == 0.0);
}

static void TestPresentationBatchPolicies(void)
{
    const double timestep = 1.0 / 60.0;
    const double frameWork = 0.0205;
    FramePacingState pacing = {0.0};
    uint32_t previousBatchFrames = 1;
    uint32_t legacyPresents = 0;
    uint32_t perServicedFramePresents = 0;
    uint32_t servicedFrames = 0;
    uint32_t multiFrameBatches = 0;
    const uint32_t explicitBatches[] = {0, 1, 2, 3, 5};

    for (uint32_t i = 0; i < sizeof(explicitBatches) / sizeof(explicitBatches[0]); i++)
    {
        const uint32_t batchFrames = explicitBatches[i];
        assert(CountBatchPresentations(FRAME_PACING_PRESENT_PER_ITERATION,
                                       batchFrames, false) == 1);
        assert(CountBatchPresentations(FRAME_PACING_PRESENT_PER_SERVICED_FRAME,
                                       batchFrames, false) == batchFrames);
    }

    assert(CountBatchPresentations(FRAME_PACING_PRESENT_PER_ITERATION, 0, true) == 1);
    assert(CountBatchPresentations(FRAME_PACING_PRESENT_PER_SERVICED_FRAME, 0, true) == 1);
    assert(CountBatchPresentations(FRAME_PACING_PRESENT_PER_SERVICED_FRAME, 1, true) == 1);
    assert(CountBatchPresentations(FRAME_PACING_PRESENT_PER_SERVICED_FRAME, 3, true) == 3);

    for (uint32_t iteration = 0; iteration < 120; iteration++)
    {
        /* This models submission counts only. GXM queue/vblank backpressure is
         * deliberately not simulated by this deterministic accumulator test. */
        double elapsed = previousBatchFrames * frameWork;
        uint32_t batchFrames = ServiceAvailableFrames(&pacing, elapsed, timestep);

        legacyPresents += CountBatchPresentations(
            FRAME_PACING_PRESENT_PER_ITERATION, batchFrames, false);
        perServicedFramePresents += CountBatchPresentations(
            FRAME_PACING_PRESENT_PER_SERVICED_FRAME, batchFrames, false);
        servicedFrames += batchFrames;
        multiFrameBatches += batchFrames > 1;
        previousBatchFrames = batchFrames;
    }

    assert(multiFrameBatches > 0);
    assert(servicedFrames > legacyPresents);
    assert(perServicedFramePresents == servicedFrames);
    assert(FramePacing_BatchHistogramBucket(0) == 0);
    assert(FramePacing_BatchHistogramBucket(1) == 1);
    assert(FramePacing_BatchHistogramBucket(2) == 2);
    assert(FramePacing_BatchHistogramBucket(3) == 3);
    assert(FramePacing_BatchHistogramBucket(4) == 4);
    assert(FramePacing_BatchHistogramBucket(UINT32_MAX) == 4);

    printf("PASS: %.1f ms work model: %u serviced ticks across 120 outer loops; "
           "%u multi-tick batches; legacy submits=%u, per-tick submits=%u "
           "(GXM/vblank blocking not modeled)\n",
           frameWork * 1000.0, servicedFrames, multiFrameBatches,
           legacyPresents, perServicedFramePresents);
}

int main(void)
{
    TestHelperMatchesPreviousAccumulator();
    TestSingleTickPerOnTimeBatch();
    TestCatchupBurstAndLongStallClamp();
    TestPresentationBatchPolicies();
    puts("PASS: fixed-step accumulator and batch histogram checks");
    return 0;
}
