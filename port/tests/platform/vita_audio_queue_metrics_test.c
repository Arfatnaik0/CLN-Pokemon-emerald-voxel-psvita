#include <assert.h>
#include <stdint.h>

#include "platform/vita_perf.h"

int main(void)
{
    const uint32_t bytesPerSecond = 44100 * 2 * sizeof(float);
    const uint32_t oneFrameBytes = 5880;

    /* One 60 Hz block covers the nominal interval despite rounding jitter. */
    assert(!VitaPerf_AudioQueueMayUnderrun(true, 16667, oneFrameBytes, bytesPerSecond));
    assert(!VitaPerf_AudioQueueMayUnderrun(true, 17666, oneFrameBytes, bytesPerSecond));

    /* At 50 Hz, a 20 ms producer gap exceeds the one-frame queue headroom. */
    assert(VitaPerf_AudioQueueMayUnderrun(true, 20000, oneFrameBytes, bytesPerSecond));

    /* A two-frame backlog covers a 30 ms gap, but not a 40 ms gap. */
    assert(!VitaPerf_AudioQueueMayUnderrun(true, 30000, oneFrameBytes * 2, bytesPerSecond));
    assert(VitaPerf_AudioQueueMayUnderrun(true, 40000, oneFrameBytes * 2, bytesPerSecond));

    /* No prior sample after a reset cannot form a gap; invalid rates do not divide. */
    assert(!VitaPerf_AudioQueueMayUnderrun(false, 1000000, 0, bytesPerSecond));
    assert(VitaPerf_AudioQueueMayUnderrun(true, 1001, 0, bytesPerSecond));
    assert(!VitaPerf_AudioQueueMayUnderrun(true, 100000, oneFrameBytes, 0));
    return 0;
}
