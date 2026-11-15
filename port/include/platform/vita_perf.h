#ifndef GUARD_VITA_PERF_H
#define GUARD_VITA_PERF_H

#include <stdint.h>
#include <stdbool.h>

#include "platform/frame_pacing.h"

struct SDL_Renderer;

#define VITA_PERF_EASY_DRAW_WORKER_COUNT 2

#if defined(PLATFORM_VITA) && !defined(VITA_PERF_DISABLE) && !defined(EASY_DRAW_TEST_PARALLEL)
#define VITA_PERF_ENABLED 1
#else
#define VITA_PERF_ENABLED 0
#endif

enum VitaPerfStage
{
    VITA_PERF_DRAW_FRAME,
    VITA_PERF_PIXEL_CONVERT,
    VITA_PERF_TEXTURE_UPDATE,
    VITA_PERF_RENDER_QUEUE,
    VITA_PERF_PRESENT,
    VITA_PERF_GAME_LOGIC,
    VITA_PERF_FRAME_WAIT,
    VITA_PERF_TICK_INTERVAL,
    VITA_PERF_MIXER,
    VITA_PERF_AUDIO_SUPPLY,
    VITA_PERF_AUDIO_QUEUE,
    VITA_PERF_MAIN_LOOP_BATCH,
    VITA_PERF_SCENE_CLASSIC,
    VITA_PERF_REPORT_IO,
    VITA_PERF_HBLANK_WIN0_REPLAY,
    VITA_PERF_CLASSIC_DRAW,
    VITA_PERF_STAGE_COUNT,
};

enum VitaEasyDrawPath
{
    VITA_EASY_DRAW_SERIAL,
    VITA_EASY_DRAW_FULL_PARALLEL,
    VITA_EASY_DRAW_VCOUNT_PREFIX,
    VITA_EASY_DRAW_WIN0H_DMA_PARALLEL,
    VITA_EASY_DRAW_PATH_COUNT,
};

enum VitaEasyDrawFallbackReason
{
    VITA_EASY_DRAW_FALLBACK_NONE,
    VITA_EASY_DRAW_FALLBACK_HBLANK_IRQ,
    VITA_EASY_DRAW_FALLBACK_HBLANK_DMA,
    VITA_EASY_DRAW_FALLBACK_EARLY_VCOUNT,
    VITA_EASY_DRAW_FALLBACK_WORKER_FAILURE,
    VITA_EASY_DRAW_FALLBACK_OTHER,
    VITA_EASY_DRAW_FALLBACK_COUNT,
};

enum VitaEasyDrawTimingMetric
{
    VITA_EASY_DRAW_TIMING_DISPATCH,
    VITA_EASY_DRAW_TIMING_MAIN_RANGE,
    VITA_EASY_DRAW_TIMING_WORKER_0_SCHEDULE,
    VITA_EASY_DRAW_TIMING_WORKER_0_RANGE,
    VITA_EASY_DRAW_TIMING_WORKER_1_SCHEDULE,
    VITA_EASY_DRAW_TIMING_WORKER_1_RANGE,
    VITA_EASY_DRAW_TIMING_JOIN_WAIT,
    VITA_EASY_DRAW_TIMING_PARALLEL_TOTAL,
    VITA_EASY_DRAW_TIMING_COUNT,
};

struct VitaEasyDrawTiming
{
    uint64_t dispatchStart;
    uint64_t dispatchEnd;
    uint64_t mainStart;
    uint64_t mainEnd;
    uint64_t workerStart[VITA_PERF_EASY_DRAW_WORKER_COUNT];
    uint64_t workerEnd[VITA_PERF_EASY_DRAW_WORKER_COUNT];
    uint64_t completed;
    uint32_t parallelHeight;
};

/* A likely producer underrun: the producer waited longer than the queued
 * audio could cover. Keep a small tolerance for timer/SDL sampling jitter. */
static inline bool VitaPerf_AudioQueueMayUnderrun(bool hasPreviousQueue,
                                                 uint64_t elapsedUs,
                                                 uint32_t bufferedBytes,
                                                 uint32_t bytesPerSecond)
{
    if (!hasPreviousQueue || bytesPerSecond == 0)
        return false;

    uint64_t bufferedUs = (uint64_t)bufferedBytes * 1000000u / bytesPerSecond;
    return elapsedUs > bufferedUs + 1000u;
}

#if VITA_PERF_ENABLED
uint64_t VitaPerf_Now(void);
void VitaPerf_Init(void);
void VitaPerf_Shutdown(void);
void VitaPerf_Poll(void);
void VitaPerf_LogRenderer(const char *name, int windowWidth, int windowHeight,
                          int outputWidth, int outputHeight, uint32_t flags);
void VitaPerf_LogClockRequest(const char *phase, int targetMhz, int beforeMhz,
                              int setResult, int afterMhz);
void VitaPerf_Record(enum VitaPerfStage stage, uint64_t start, uint64_t end);
void VitaPerf_RecordEasyDrawPath(enum VitaEasyDrawPath path,
                                 enum VitaEasyDrawFallbackReason fallbackReason);
void VitaPerf_RecordEasyDrawTiming(const struct VitaEasyDrawTiming *timing);
void VitaPerf_RecordGameWait(uint64_t start, uint64_t end);
void VitaPerf_RecordFrameRenderPresent(uint64_t renderStart, uint64_t renderEnd,
                                       uint64_t presentStart, uint64_t presentEnd);
void VitaPerf_ResetAudioQueueTiming(void);
void VitaPerf_RecordAudioQueue(uint64_t start, uint64_t end,
                               uint32_t queuedBeforeBytes,
                               uint32_t queuedAfterBytes,
                               uint32_t bytesPerSecond,
                               uint32_t overflowCleared);
void VitaPerf_RecordMainLoopBatch(uint32_t servicedFrames, uint64_t start, uint64_t end);
void VitaPerf_RecordActiveSprites(uint32_t activeSprites);
void VitaPerf_ToggleOverlay(void);
void VitaPerf_RenderOverlay(struct SDL_Renderer *renderer);
bool VitaPerf_GetOverlayImage(const void **pixels, int *width, int *height,
                              uint32_t *generation);
#else
static inline uint64_t VitaPerf_Now(void) { return 0; }
static inline void VitaPerf_Init(void) { }
static inline void VitaPerf_Shutdown(void) { }
static inline void VitaPerf_Poll(void) { }
static inline void VitaPerf_LogRenderer(const char *name, int windowWidth, int windowHeight,
                                        int outputWidth, int outputHeight, uint32_t flags)
{
    (void)name;
    (void)windowWidth;
    (void)windowHeight;
    (void)outputWidth;
    (void)outputHeight;
    (void)flags;
}
static inline void VitaPerf_LogClockRequest(const char *phase, int targetMhz,
                                            int beforeMhz, int setResult,
                                            int afterMhz)
{
    (void)phase;
    (void)targetMhz;
    (void)beforeMhz;
    (void)setResult;
    (void)afterMhz;
}
static inline void VitaPerf_Record(enum VitaPerfStage stage, uint64_t start, uint64_t end)
{
    (void)stage;
    (void)start;
    (void)end;
}
static inline void VitaPerf_RecordEasyDrawPath(enum VitaEasyDrawPath path,
                                               enum VitaEasyDrawFallbackReason fallbackReason)
{
    (void)path;
    (void)fallbackReason;
}
static inline void VitaPerf_RecordEasyDrawTiming(const struct VitaEasyDrawTiming *timing)
{
    (void)timing;
}
static inline void VitaPerf_RecordGameWait(uint64_t start, uint64_t end)
{
    (void)start;
    (void)end;
}
static inline void VitaPerf_RecordFrameRenderPresent(uint64_t renderStart,
                                                     uint64_t renderEnd,
                                                     uint64_t presentStart,
                                                     uint64_t presentEnd)
{
    (void)renderStart;
    (void)renderEnd;
    (void)presentStart;
    (void)presentEnd;
}
static inline void VitaPerf_ResetAudioQueueTiming(void) { }
static inline void VitaPerf_RecordAudioQueue(uint64_t start, uint64_t end,
                                             uint32_t queuedBeforeBytes,
                                             uint32_t queuedAfterBytes,
                                             uint32_t bytesPerSecond,
                                             uint32_t overflowCleared)
{
    (void)start;
    (void)end;
    (void)queuedBeforeBytes;
    (void)queuedAfterBytes;
    (void)bytesPerSecond;
    (void)overflowCleared;
}
static inline void VitaPerf_RecordMainLoopBatch(uint32_t servicedFrames,
                                                uint64_t start, uint64_t end)
{
    (void)servicedFrames;
    (void)start;
    (void)end;
}
static inline void VitaPerf_RecordActiveSprites(uint32_t activeSprites) { (void)activeSprites; }
static inline void VitaPerf_ToggleOverlay(void) { }
static inline void VitaPerf_RenderOverlay(struct SDL_Renderer *renderer)
{
    (void)renderer;
}
static inline bool VitaPerf_GetOverlayImage(const void **pixels, int *width,
                                            int *height, uint32_t *generation)
{
    (void)pixels;
    (void)width;
    (void)height;
    (void)generation;
    return false;
}
#endif

#endif // GUARD_VITA_PERF_H
