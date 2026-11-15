#include "platform/vita_perf.h"
#include "platform/vita_perf_rolling.h"

#if VITA_PERF_ENABLED

#include <SDL2/SDL.h>
#include <psp2/power.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PERF_LOG_PATH "ux0:data/pokeemerald/perf.log"
#define PERF_REPORT_SECONDS 5
#define PERF_LOG_BUF_SIZE 8192
#define PERF_LOG_QUEUE_SLOTS 4
#define PERF_RENDERER_NAME_SIZE 48
#define PERF_PERCENTILE_BUCKET_WIDTH_US 250
#define PERF_PERCENTILE_BUCKET_COUNT 1024
#define PERF_OVERLAY_METRIC_COUNT 4
#define PERF_OVERLAY_WIDTH 640
#define PERF_OVERLAY_HEIGHT 176
#define PERF_OVERLAY_REFRESH_US 250000
#define PERF_BATCH_HISTOGRAM_BUCKETS FRAME_PACING_BATCH_HISTOGRAM_BUCKETS

struct PerfStat
{
    uint64_t totalUs;
    uint64_t maxUs;
    uint32_t samples;
};

struct AudioQueueStat
{
    uint64_t totalBytes;
    uint32_t maxBytes;
    uint32_t samples;
    uint32_t prequeueZeroCount;
    uint32_t supplyGapCount;
    uint32_t maxSupplyGapUs;
    uint32_t overflowClearCount;
};

struct MainLoopBatchStat
{
    uint32_t histogram[PERF_BATCH_HISTOGRAM_BUCKETS];
    uint32_t samples;
    uint32_t servicedFrames;
    uint32_t maxServicedFrames;
};

struct PerfHistogram
{
    uint32_t buckets[PERF_PERCENTILE_BUCKET_COUNT];
    uint32_t saturatedSamples;
};

enum PerfRollingMetric
{
    PERF_ROLLING_GAME,
    PERF_ROLLING_PPU,
    PERF_ROLLING_UPLOAD,
    PERF_ROLLING_PRESENT,
    PERF_ROLLING_AUDIO_SUPPLY,
    PERF_ROLLING_METRIC_COUNT,
};

static const char *const sOverlayLabels[PERF_OVERLAY_METRIC_COUNT] = {
    "GAME",
    "PPU",
    "UPLOAD",
    "PRESENT",
};

struct EasyDrawHeightStat
{
    uint64_t totalRows;
    uint32_t minRows;
    uint32_t maxRows;
    uint32_t samples;
};

struct VitaPerfReportSnapshot
{
    uint64_t windowTicks;
    uint32_t waitReturns;
    uint32_t vblankMisses;
    uint32_t gameTimingPairMissingMixer;
    struct PerfStat stats[VITA_PERF_STAGE_COUNT];
    struct PerfStat activeSpriteStat;
    struct PerfHistogram drawFrameHistogram;
    struct PerfHistogram tickIntervalHistogram;
    struct VitaPerfRollingWindow rollingFrames[PERF_ROLLING_METRIC_COUNT];
    struct AudioQueueStat audioQueue;
    struct MainLoopBatchStat mainLoopBatch;
    struct PerfStat easyDrawTimingStats[VITA_EASY_DRAW_TIMING_COUNT];
    struct EasyDrawHeightStat easyDrawHeight;
    uint32_t easyDrawPaths[VITA_EASY_DRAW_PATH_COUNT];
    uint32_t easyDrawFallbacks[VITA_EASY_DRAW_FALLBACK_COUNT];
    SDL_bool rendererInfoPending;
    char rendererName[PERF_RENDERER_NAME_SIZE];
    int windowWidth;
    int windowHeight;
    int outputWidth;
    int outputHeight;
    uint32_t rendererFlags;
};

enum PerfLogChunkKind
{
    PERF_LOG_CHUNK_TEXT,
    PERF_LOG_CHUNK_POLL_REPORT,
};

struct PerfLogChunk
{
    enum PerfLogChunkKind kind;
    union
    {
        struct
        {
            char data[PERF_LOG_BUF_SIZE];
            size_t len;
        } text;
        struct VitaPerfReportSnapshot report;
    } u;
};

struct PerfStrBuf
{
    char *dst;
    size_t cap;
    size_t len;
};

static SDL_SpinLock sLock;
static SDL_SpinLock sLogQueueLock;
static FILE *sLog;
static SDL_Thread *sLogThread;
static SDL_sem *sLogSem;
static SDL_atomic_t sLogThreadQuit;
static struct PerfLogChunk sLogQueue[PERF_LOG_QUEUE_SLOTS];
static struct VitaPerfReportSnapshot sPollStaging;
static char sWriterReportBuf[PERF_LOG_BUF_SIZE];
static uint32_t sLogQueueHead;
static uint32_t sLogQueueTail;
static uint32_t sLogQueueCount;
static SDL_bool sInitialized;
static SDL_bool sLogFailed;
static uint64_t sFrequency;
static uint64_t sWindowStart;
static uint64_t sPreviousFrameStart;
static uint64_t sPreviousFrameEnd;
static uint64_t sPreviousAudioQueueEnd;
static uint32_t sPreviousAudioQueuedBytes;
static uint32_t sPendingMixerDurationUs;
static uint32_t sGameTimingPairMissingMixer;
static SDL_bool sHasGameFrame;
static SDL_bool sHasPreviousAudioQueue;
static SDL_bool sHasPendingMixerDuration;
static SDL_bool sPendingMixerDurationValid;
static SDL_bool sPpuFrameWasParallel;
static uint32_t sVBlankWaitReturns;
static uint32_t sVBlankMissCount;
static struct PerfStat sStats[VITA_PERF_STAGE_COUNT];
static struct PerfStat sActiveSpriteStat;
static struct PerfHistogram sDrawFrameHistogram;
static struct PerfHistogram sTickIntervalHistogram;
static struct VitaPerfRollingWindow sRollingFrames[PERF_ROLLING_METRIC_COUNT];
static uint32_t sEasyDrawPaths[VITA_EASY_DRAW_PATH_COUNT];
static uint32_t sEasyDrawFallbacks[VITA_EASY_DRAW_FALLBACK_COUNT];
static struct PerfStat sEasyDrawTimingStats[VITA_EASY_DRAW_TIMING_COUNT];
static struct EasyDrawHeightStat sEasyDrawHeight;
static struct AudioQueueStat sAudioQueue;
static struct MainLoopBatchStat sMainLoopBatch;
static char sRendererName[PERF_RENDERER_NAME_SIZE] = "unknown";
static int sWindowWidth;
static int sWindowHeight;
static int sOutputWidth;
static int sOutputHeight;
static uint32_t sRendererFlags;
static SDL_bool sRendererInfoPending;
static SDL_bool sOverlayVisible;
static SDL_bool sOverlayDirty = SDL_TRUE;
static SDL_bool sOverlayUnavailable;
static SDL_Surface *sOverlaySurface;
static SDL_Texture *sOverlayTexture;
static SDL_Renderer *sOverlayRenderer;
static uint64_t sOverlayLastUpdate;
static uint32_t sOverlayGeneration;

static uint64_t TicksToUs(uint64_t ticks)
{
    if (sFrequency == 0)
        return 0;
    return (ticks / sFrequency) * 1000000
         + ((ticks % sFrequency) * 1000000) / sFrequency;
}

static uint32_t RollingDurationUs(uint64_t durationUs)
{
    return durationUs > UINT32_MAX ? UINT32_MAX : (uint32_t)durationUs;
}

static void PerfBufPrintf(struct PerfStrBuf *buf, const char *fmt, ...)
{
    if (buf == NULL || buf->dst == NULL || buf->len >= buf->cap)
        return;

    va_list args;
    va_start(args, fmt);
    size_t rem = buf->cap - buf->len;
    int written = vsnprintf(buf->dst + buf->len, rem, fmt, args);
    va_end(args);

    if (written > 0)
    {
        size_t adv = ((size_t)written < rem) ? (size_t)written : (rem - 1);
        buf->len += adv;
    }
}

static void FormatPollReportSnapshot(const struct VitaPerfReportSnapshot *snap,
                                     struct PerfStrBuf *log);

/* Dedicated low-overhead background writer so SD card fflush and report
 * summarizing/formatting never stall the main Vita emulation/present loop. */
static int SDLCALL VitaPerfLogWriterThread(void *unused)
{
    (void)unused;

    for (;;)
    {
        SDL_SemWait(sLogSem);

        for (;;)
        {
            enum PerfLogChunkKind kind = PERF_LOG_CHUNK_TEXT;
            SDL_bool hasItem = SDL_FALSE;
            const struct PerfLogChunk *slot = NULL;
            const char *data = NULL;
            size_t len = 0;

            SDL_AtomicLock(&sLogQueueLock);
            if (sLogQueueCount > 0)
            {
                slot = &sLogQueue[sLogQueueHead];
                hasItem = SDL_TRUE;
                kind = slot->kind;
            }
            SDL_AtomicUnlock(&sLogQueueLock);

            if (!hasItem)
                break;

            if (kind == PERF_LOG_CHUNK_TEXT)
            {
                data = slot->u.text.data;
                len = slot->u.text.len;
            }
            else
            {
                struct PerfStrBuf out = { sWriterReportBuf, sizeof(sWriterReportBuf), 0 };
                FormatPollReportSnapshot(&slot->u.report, &out);
                data = out.dst;
                len = out.len;
            }

            if (sLog != NULL && data != NULL && len > 0)
            {
                fwrite(data, 1, len, sLog);
                fflush(sLog);
            }

            SDL_AtomicLock(&sLogQueueLock);
            sLogQueueHead = (sLogQueueHead + 1u) % PERF_LOG_QUEUE_SLOTS;
            sLogQueueCount--;
            SDL_AtomicUnlock(&sLogQueueLock);
        }

        if (SDL_AtomicGet(&sLogThreadQuit) != 0)
            break;
    }

    return 0;
}

static void EnqueuePerfLogText(const char *text, size_t len)
{
    if (sLog == NULL || sLogThread == NULL || sLogSem == NULL || text == NULL || len == 0)
        return;
    if (SDL_AtomicGet(&sLogThreadQuit) != 0)
        return;
    if (len > PERF_LOG_BUF_SIZE)
        len = PERF_LOG_BUF_SIZE;

    SDL_AtomicLock(&sLogQueueLock);
    if (sLogQueueCount < PERF_LOG_QUEUE_SLOTS)
    {
        struct PerfLogChunk *slot = &sLogQueue[sLogQueueTail];
        slot->kind = PERF_LOG_CHUNK_TEXT;
        memcpy(slot->u.text.data, text, len);
        slot->u.text.len = len;
        sLogQueueTail = (sLogQueueTail + 1u) % PERF_LOG_QUEUE_SLOTS;
        sLogQueueCount++;
        SDL_AtomicUnlock(&sLogQueueLock);
        SDL_SemPost(sLogSem);
    }
    else
    {
        SDL_AtomicUnlock(&sLogQueueLock);
    }
}

void VitaPerf_Init(void)
{
    SDL_AtomicLock(&sLock);
    if (!sInitialized)
    {
        sFrequency = SDL_GetPerformanceFrequency();
        if (sFrequency == 0)
        {
            sLogFailed = SDL_TRUE;
        }
        else
        {
            sWindowStart = SDL_GetPerformanceCounter();
            sInitialized = SDL_TRUE;
            sLog = fopen(PERF_LOG_PATH, "w");
            if (sLog == NULL)
            {
                sLogFailed = SDL_TRUE;
            }
            else
            {
                setvbuf(sLog, NULL, _IOFBF, 4096);
                fprintf(sLog, "# Vita perf; report interval=%d s; durations in us; stage counts are samples.\n",
                        PERF_REPORT_SECONDS);
                fprintf(sLog, "# SDL Vita performance counter uses sceKernelGetProcessTimeWide at 1 MHz.\n");
                fprintf(sLog, "# emulated_fps is completed VBlankIntrWait returns per report window; it is not display FPS.\n");
                fprintf(sLog, "# emulated_tick_interval is request-to-request wall time; present_n counts SDL_RenderPresent calls, not unique displayed frames.\n");
                fprintf(sLog, "# Event-triggered dirty redraws can present without servicing a game tick.\n");
                fprintf(sLog, "# VITA gxm queues display flips asynchronously; present timing is the SDL_RenderPresent call duration, not displayed FPS.\n");
                fprintf(sLog, "# main_loop_batch histogram counts VBlankIntrWait frames serviced per outer-loop iteration (4 means 4+).\n");
                fprintf(sLog, "# draw_frame and emulated_tick_interval p95/p99 use 250us histogram buckets, saturating at 256ms.\n");
                fprintf(sLog, "# easy_* timings cover successful parallel dispatch, each scanline range, scheduler wake, and join wait.\n");
                fprintf(sLog, "# audio_prequeue_zero_n counts empty queue snapshots; audio_supply_gap_n estimates gaps beyond previously queued playback + 1ms; it is not a device underrun callback.\n");
                fprintf(sLog, "# Rolling metrics use the latest 300 samples per bucket; each *_n is the partial/full sample count.\n");
                fprintf(sLog, "# Rolling median is the middle sample (mean of middle pair when even); p99 uses nearest rank ceil(0.99*N). Values are ms.\n");
                fprintf(sLog, "# game_ms pairs logic since the last wait return with m4aSoundMain/RunMixerFrame from the corresponding VBlank, before semaphore release. First warm-up logic interval is omitted.\n");
                fprintf(sLog, "# ppu_ms is full DrawFrame wall time for every path; *_parallel_n counts full-parallel and VCOUNT-prefix frames, and *_serial_n counts serial frames.\n");
                fprintf(sLog, "# easy_* and easy_parallel_total are separate successful worker dispatch/range/join timings; audio_supply_ms covers QueueVitaAudio including volume gain, SDL_AudioStream conversion and SDL_QueueAudio. audio_queue covers only SDL_QueueAudio call time.\n");
                fprintf(sLog, "# present_ms sums Vita render submission (clear/gameplay quad/optional overlay) and the SDL_RenderPresent call duration; GXM flips are asynchronous. Vita does not draw BG.png or Border.png in this path.\n");
                fprintf(sLog, "# classic_draw is the clear/quad/overlay substage and overlaps render_queue and scene_classic; fallback pixel_convert and texture_update also overlap those scene totals.\n");
                fflush(sLog);

                SDL_AtomicSet(&sLogThreadQuit, 0);
                sLogQueueHead = 0;
                sLogQueueTail = 0;
                sLogQueueCount = 0;
                sLogSem = SDL_CreateSemaphore(0);
                if (sLogSem != NULL)
                {
                    sLogThread = SDL_CreateThread(VitaPerfLogWriterThread, "VitaPerfLog", NULL);
                    if (sLogThread == NULL)
                    {
                        SDL_DestroySemaphore(sLogSem);
                        sLogSem = NULL;
                    }
                }
            }
        }
    }
    SDL_AtomicUnlock(&sLock);

    if (sLogFailed)
    {
        if (sFrequency == 0)
            SDL_Log("Vita perf timer unavailable; on-screen performance metrics are disabled");
        else
            SDL_Log("Vita perf log unavailable at %s; on-screen metrics remain enabled", PERF_LOG_PATH);
    }
}

void VitaPerf_Shutdown(void)
{
    if (sLogThread != NULL)
    {
        SDL_AtomicSet(&sLogThreadQuit, 1);
        if (sLogSem != NULL)
            SDL_SemPost(sLogSem);
        SDL_WaitThread(sLogThread, NULL);
        sLogThread = NULL;
    }
    if (sLogSem != NULL)
    {
        SDL_DestroySemaphore(sLogSem);
        sLogSem = NULL;
    }
    if (sLog != NULL)
    {
        fclose(sLog);
        sLog = NULL;
    }
}

uint64_t VitaPerf_Now(void)
{
    return SDL_GetPerformanceCounter();
}

void VitaPerf_LogRenderer(const char *name, int windowWidth, int windowHeight,
                          int outputWidth, int outputHeight, uint32_t flags)
{
    if (!sInitialized)
        return;

    char buf[384];
    struct PerfStrBuf out = { buf, sizeof(buf), 0 };

    SDL_AtomicLock(&sLock);
    SDL_strlcpy(sRendererName, name != NULL ? name : "unknown", sizeof(sRendererName));
    sWindowWidth = windowWidth;
    sWindowHeight = windowHeight;
    sOutputWidth = outputWidth;
    sOutputHeight = outputHeight;
    sRendererFlags = flags;
    sRendererInfoPending = SDL_FALSE;
    if (sLog != NULL)
    {
        PerfBufPrintf(&out, "renderer=%s window=%dx%d output=%dx%d renderer_flags=0x%08x"
                            " renderer_accelerated=%u renderer_software=%u renderer_vsync=%u\n",
                      sRendererName, windowWidth, windowHeight, outputWidth, outputHeight,
                      sRendererFlags,
                      (sRendererFlags & SDL_RENDERER_ACCELERATED) != 0,
                      (sRendererFlags & SDL_RENDERER_SOFTWARE) != 0,
                      (sRendererFlags & SDL_RENDERER_PRESENTVSYNC) != 0);
    }
    SDL_AtomicUnlock(&sLock);
    EnqueuePerfLogText(out.dst, out.len);
    SDL_Log("SDL renderer=%s flags=0x%08x accelerated=%u software=%u vsync=%u",
            sRendererName, sRendererFlags,
            (sRendererFlags & SDL_RENDERER_ACCELERATED) != 0,
            (sRendererFlags & SDL_RENDERER_SOFTWARE) != 0,
            (sRendererFlags & SDL_RENDERER_PRESENTVSYNC) != 0);
}

void VitaPerf_LogClockRequest(const char *phase, int targetMhz, int beforeMhz,
                              int setResult, int afterMhz)
{
    if (!sInitialized || sLog == NULL)
        return;

    char buf[256];
    struct PerfStrBuf out = { buf, sizeof(buf), 0 };
    PerfBufPrintf(&out, "arm_clock_request phase=%s target_mhz=%d before_mhz=%d"
                        " set_result=0x%08x after_mhz=%d\n",
                  phase != NULL ? phase : "unknown", targetMhz, beforeMhz,
                  (unsigned int)setResult, afterMhz);
    EnqueuePerfLogText(out.dst, out.len);
}

static void RecordUsLocked(enum VitaPerfStage stage, uint64_t durationUs)
{
    if (stage < 0 || stage >= VITA_PERF_STAGE_COUNT)
        return;

    struct PerfStat *stat = &sStats[stage];
    stat->totalUs += durationUs;
    if (durationUs > stat->maxUs)
        stat->maxUs = durationUs;
    stat->samples++;

    if (stage == VITA_PERF_DRAW_FRAME || stage == VITA_PERF_TICK_INTERVAL)
    {
        struct PerfHistogram *histogram = stage == VITA_PERF_DRAW_FRAME
                                        ? &sDrawFrameHistogram
                                        : &sTickIntervalHistogram;
        uint64_t bucket = durationUs / PERF_PERCENTILE_BUCKET_WIDTH_US;
        if (bucket >= PERF_PERCENTILE_BUCKET_COUNT)
        {
            bucket = PERF_PERCENTILE_BUCKET_COUNT - 1;
            histogram->saturatedSamples++;
        }
        histogram->buckets[bucket]++;
    }

    switch (stage)
    {
    case VITA_PERF_DRAW_FRAME:
        VitaPerfRolling_AddPpuFrame(&sRollingFrames[PERF_ROLLING_PPU],
                                    RollingDurationUs(durationUs), sPpuFrameWasParallel);
        sPpuFrameWasParallel = SDL_FALSE;
        break;
    case VITA_PERF_TEXTURE_UPDATE:
        VitaPerfRolling_Add(&sRollingFrames[PERF_ROLLING_UPLOAD],
                            RollingDurationUs(durationUs), 0);
        break;
    case VITA_PERF_MIXER:
        /* m4aSoundMain runs in the VBlank handler before it releases the
         * one blocked VBlankIntrWait request. Keep exactly one pending sample
         * for that request; duplicate or missing samples are not paired. */
        if (sHasPendingMixerDuration)
            sPendingMixerDurationValid = SDL_FALSE;
        else
            sPendingMixerDurationValid = SDL_TRUE;
        sPendingMixerDurationUs = RollingDurationUs(durationUs);
        sHasPendingMixerDuration = SDL_TRUE;
        break;
    case VITA_PERF_AUDIO_SUPPLY:
        VitaPerfRolling_Add(&sRollingFrames[PERF_ROLLING_AUDIO_SUPPLY],
                            RollingDurationUs(durationUs), 0);
        break;
    default:
        break;
    }
}

void VitaPerf_Record(enum VitaPerfStage stage, uint64_t start, uint64_t end)
{
    if (!sInitialized || stage < 0 || stage >= VITA_PERF_STAGE_COUNT)
        return;

    uint64_t durationUs = TicksToUs(end >= start ? end - start : 0);
    SDL_AtomicLock(&sLock);
    RecordUsLocked(stage, durationUs);
    SDL_AtomicUnlock(&sLock);
}

void VitaPerf_RecordEasyDrawPath(enum VitaEasyDrawPath path,
                                 enum VitaEasyDrawFallbackReason fallbackReason)
{
    if (!sInitialized || path < 0 || path >= VITA_EASY_DRAW_PATH_COUNT)
        return;

    SDL_AtomicLock(&sLock);
    sEasyDrawPaths[path]++;
    sPpuFrameWasParallel = path == VITA_EASY_DRAW_FULL_PARALLEL
                        || path == VITA_EASY_DRAW_VCOUNT_PREFIX
                        || path == VITA_EASY_DRAW_WIN0H_DMA_PARALLEL;
    if (path == VITA_EASY_DRAW_SERIAL
     && fallbackReason > VITA_EASY_DRAW_FALLBACK_NONE
     && fallbackReason < VITA_EASY_DRAW_FALLBACK_COUNT)
        sEasyDrawFallbacks[fallbackReason]++;
    SDL_AtomicUnlock(&sLock);
}

static void RecordEasyDrawMetricLocked(enum VitaEasyDrawTimingMetric metric,
                                       uint64_t start, uint64_t end)
{
    if (metric >= VITA_EASY_DRAW_TIMING_COUNT)
        return;

    uint64_t durationUs = TicksToUs(end >= start ? end - start : 0);
    struct PerfStat *stat = &sEasyDrawTimingStats[metric];
    stat->totalUs += durationUs;
    if (durationUs > stat->maxUs)
        stat->maxUs = durationUs;
    stat->samples++;
}

void VitaPerf_RecordEasyDrawTiming(const struct VitaEasyDrawTiming *timing)
{
    if (!sInitialized || timing == NULL)
        return;

    SDL_AtomicLock(&sLock);
    RecordEasyDrawMetricLocked(VITA_EASY_DRAW_TIMING_DISPATCH,
                               timing->dispatchStart, timing->dispatchEnd);
    RecordEasyDrawMetricLocked(VITA_EASY_DRAW_TIMING_MAIN_RANGE,
                               timing->mainStart, timing->mainEnd);
    RecordEasyDrawMetricLocked(VITA_EASY_DRAW_TIMING_WORKER_0_SCHEDULE,
                               timing->dispatchStart, timing->workerStart[0]);
    RecordEasyDrawMetricLocked(VITA_EASY_DRAW_TIMING_WORKER_0_RANGE,
                               timing->workerStart[0], timing->workerEnd[0]);
    RecordEasyDrawMetricLocked(VITA_EASY_DRAW_TIMING_WORKER_1_SCHEDULE,
                               timing->dispatchStart, timing->workerStart[1]);
    RecordEasyDrawMetricLocked(VITA_EASY_DRAW_TIMING_WORKER_1_RANGE,
                               timing->workerStart[1], timing->workerEnd[1]);
    RecordEasyDrawMetricLocked(VITA_EASY_DRAW_TIMING_JOIN_WAIT,
                               timing->mainEnd, timing->completed);
    RecordEasyDrawMetricLocked(VITA_EASY_DRAW_TIMING_PARALLEL_TOTAL,
                               timing->dispatchStart, timing->completed);

    if (timing->parallelHeight > 0)
    {
        if (sEasyDrawHeight.samples == 0 || timing->parallelHeight < sEasyDrawHeight.minRows)
            sEasyDrawHeight.minRows = timing->parallelHeight;
        if (sEasyDrawHeight.samples == 0 || timing->parallelHeight > sEasyDrawHeight.maxRows)
            sEasyDrawHeight.maxRows = timing->parallelHeight;
        sEasyDrawHeight.totalRows += timing->parallelHeight;
        sEasyDrawHeight.samples++;
    }
    SDL_AtomicUnlock(&sLock);
}

void VitaPerf_RecordGameWait(uint64_t start, uint64_t end)
{
    if (!sInitialized)
        return;

    SDL_AtomicLock(&sLock);
    if (!sHasGameFrame)
    {
        /* Start the first reporting window when game frames begin, not during boot. */
        sWindowStart = start;
        sHasGameFrame = SDL_TRUE;
    }
    else if (start >= sPreviousFrameStart)
    {
        uint64_t intervalUs = TicksToUs(start - sPreviousFrameStart);
        RecordUsLocked(VITA_PERF_TICK_INTERVAL, intervalUs);
        if (intervalUs > 18000)
            sVBlankMissCount++;
    }
    if (sPreviousFrameEnd != 0 && start >= sPreviousFrameEnd)
    {
        uint64_t logicUs = TicksToUs(start - sPreviousFrameEnd);
        RecordUsLocked(VITA_PERF_GAME_LOGIC, logicUs);
        if (sHasPendingMixerDuration && sPendingMixerDurationValid)
            VitaPerfRolling_Add(&sRollingFrames[PERF_ROLLING_GAME],
                                RollingDurationUs(logicUs
                                                  + sPendingMixerDurationUs), 0);
        else
            sGameTimingPairMissingMixer++;
    }
    /* This callback runs after the semaphore wait. The VBlank handler stores
     * its mixer sample before SDL_SemPost, so consuming here pairs the logic
     * interval since the previous return with this serviced frame's mixer.
     * A missing/duplicate mixer sample is dropped instead of reusing a stale
     * timing. The first wait has no prior logic interval and is warm-up only. */
    sHasPendingMixerDuration = SDL_FALSE;
    sPendingMixerDurationValid = SDL_FALSE;
    if (end >= start)
        RecordUsLocked(VITA_PERF_FRAME_WAIT, TicksToUs(end - start));
    sPreviousFrameStart = start;
    sPreviousFrameEnd = end;
    sVBlankWaitReturns++;
    SDL_AtomicUnlock(&sLock);
}

void VitaPerf_RecordFrameRenderPresent(uint64_t renderStart, uint64_t renderEnd,
                                       uint64_t presentStart, uint64_t presentEnd)
{
    if (!sInitialized)
        return;

    uint64_t renderTicks = renderEnd >= renderStart ? renderEnd - renderStart : 0;
    uint64_t presentTicks = presentEnd >= presentStart ? presentEnd - presentStart : 0;
    SDL_AtomicLock(&sLock);
    VitaPerfRolling_Add(&sRollingFrames[PERF_ROLLING_PRESENT],
                        RollingDurationUs(TicksToUs(renderTicks)
                                          + TicksToUs(presentTicks)), 0);
    SDL_AtomicUnlock(&sLock);
}

void VitaPerf_ResetAudioQueueTiming(void)
{
    if (!sInitialized)
        return;

    SDL_AtomicLock(&sLock);
    sHasPreviousAudioQueue = SDL_FALSE;
    sPreviousAudioQueueEnd = 0;
    sPreviousAudioQueuedBytes = 0;
    SDL_AtomicUnlock(&sLock);
}

void VitaPerf_RecordAudioQueue(uint64_t start, uint64_t end,
                               uint32_t queuedBeforeBytes,
                               uint32_t queuedAfterBytes,
                               uint32_t bytesPerSecond,
                               uint32_t overflowCleared)
{
    if (!sInitialized)
        return;

    SDL_AtomicLock(&sLock);
    RecordUsLocked(VITA_PERF_AUDIO_QUEUE, TicksToUs(end >= start ? end - start : 0));
    sAudioQueue.totalBytes += queuedAfterBytes;
    if (queuedAfterBytes > sAudioQueue.maxBytes)
        sAudioQueue.maxBytes = queuedAfterBytes;
    uint64_t gapUs = start >= sPreviousAudioQueueEnd
                   ? TicksToUs(start - sPreviousAudioQueueEnd)
                   : 0;
    if (sHasPreviousAudioQueue && queuedBeforeBytes == 0)
        sAudioQueue.prequeueZeroCount++;
    if (VitaPerf_AudioQueueMayUnderrun(sHasPreviousAudioQueue, gapUs,
                                       sPreviousAudioQueuedBytes, bytesPerSecond))
    {
        sAudioQueue.supplyGapCount++;
        if (gapUs > sAudioQueue.maxSupplyGapUs)
            sAudioQueue.maxSupplyGapUs = gapUs > UINT32_MAX
                                       ? UINT32_MAX
                                       : (uint32_t)gapUs;
    }
    if (overflowCleared)
        sAudioQueue.overflowClearCount++;
    sAudioQueue.samples++;
    sPreviousAudioQueueEnd = end;
    sPreviousAudioQueuedBytes = queuedAfterBytes;
    sHasPreviousAudioQueue = SDL_TRUE;
    SDL_AtomicUnlock(&sLock);
}

void VitaPerf_RecordMainLoopBatch(uint32_t servicedFrames, uint64_t start, uint64_t end)
{
    if (!sInitialized)
        return;

    SDL_AtomicLock(&sLock);
    RecordUsLocked(VITA_PERF_MAIN_LOOP_BATCH,
                   TicksToUs(end >= start ? end - start : 0));
    unsigned int bucket = FramePacing_BatchHistogramBucket(servicedFrames);
    sMainLoopBatch.histogram[bucket]++;
    sMainLoopBatch.samples++;
    sMainLoopBatch.servicedFrames += servicedFrames;
    if (servicedFrames > sMainLoopBatch.maxServicedFrames)
        sMainLoopBatch.maxServicedFrames = servicedFrames;
    SDL_AtomicUnlock(&sLock);
}

void VitaPerf_RecordActiveSprites(uint32_t activeSprites)
{
    if (!sInitialized)
        return;

    SDL_AtomicLock(&sLock);
    sActiveSpriteStat.totalUs += activeSprites;
    if (activeSprites > sActiveSpriteStat.maxUs)
        sActiveSpriteStat.maxUs = activeSprites;
    sActiveSpriteStat.samples++;
    SDL_AtomicUnlock(&sLock);
}

static void PrintStat(struct PerfStrBuf *log, const char *name, const struct PerfStat *stat)
{
    if (stat->samples == 0)
        PerfBufPrintf(log, " %s_n=0 %s_avg_us=- %s_max_us=-", name, name, name);
    else
        PerfBufPrintf(log, " %s_n=%u %s_avg_us=%llu %s_max_us=%llu", name, stat->samples,
                      name, (unsigned long long)(stat->totalUs / stat->samples), name,
                      (unsigned long long)stat->maxUs);
}

static void PrintVitaPowerReading(struct PerfStrBuf *log, const char *name, int value)
{
    if (value < 0)
        PerfBufPrintf(log, " %s=ERR_%d", name, value);
    else
        PerfBufPrintf(log, " %s=%d", name, value);
}

static void PrintVitaHardwareTelemetry(struct PerfStrBuf *log)
{
    PrintVitaPowerReading(log, "vita_arm_clock_mhz", scePowerGetArmClockFrequency());
    PrintVitaPowerReading(log, "vita_bus_clock_mhz", scePowerGetBusClockFrequency());
    PrintVitaPowerReading(log, "vita_gpu_clock_mhz", scePowerGetGpuClockFrequency());
    PrintVitaPowerReading(log, "vita_gpu_xbar_clock_mhz", scePowerGetGpuXbarClockFrequency());

    /* The public user-mode SDK exposes battery temperature, not SoC temperature. */
    int batteryTemp = scePowerGetBatteryTemp();
    if (batteryTemp < 0)
        PerfBufPrintf(log, " vita_battery_temp_c=ERR_%d", batteryTemp);
    else
        PerfBufPrintf(log, " vita_battery_temp_c=%d.%02d", batteryTemp / 100,
                      batteryTemp % 100);
}

static uint64_t GetPercentileUs(const struct PerfHistogram *histogram,
                                uint32_t samples, uint32_t percentile)
{
    if (samples == 0)
        return 0;

    uint64_t rank = ((uint64_t)samples * percentile + 99) / 100;
    uint64_t seen = 0;
    for (uint32_t bucket = 0; bucket < PERF_PERCENTILE_BUCKET_COUNT; bucket++)
    {
        seen += histogram->buckets[bucket];
        if (seen >= rank)
        {
            if (bucket == PERF_PERCENTILE_BUCKET_COUNT - 1)
                return (uint64_t)bucket * PERF_PERCENTILE_BUCKET_WIDTH_US;
            return (uint64_t)bucket * PERF_PERCENTILE_BUCKET_WIDTH_US
                 + PERF_PERCENTILE_BUCKET_WIDTH_US / 2;
        }
    }

    return 0;
}

static const uint8_t sOverlayFont[42][7] = {
    {14, 17, 19, 21, 25, 17, 14}, /* 0 */
    {4, 12, 4, 4, 4, 4, 14},       /* 1 */
    {14, 17, 1, 2, 4, 8, 31},      /* 2 */
    {30, 1, 1, 14, 1, 1, 30},      /* 3 */
    {2, 6, 10, 18, 31, 2, 2},      /* 4 */
    {31, 16, 16, 30, 1, 1, 30},    /* 5 */
    {14, 16, 16, 30, 17, 17, 14}, /* 6 */
    {31, 1, 2, 4, 8, 8, 8},        /* 7 */
    {14, 17, 17, 14, 17, 17, 14}, /* 8 */
    {14, 17, 17, 15, 1, 1, 14},    /* 9 */
    {14, 17, 17, 31, 17, 17, 17}, /* A */
    {30, 17, 17, 30, 17, 17, 30}, /* B */
    {14, 17, 16, 16, 16, 17, 14}, /* C */
    {28, 18, 17, 17, 17, 18, 28}, /* D */
    {31, 16, 16, 30, 16, 16, 31}, /* E */
    {31, 16, 16, 30, 16, 16, 16}, /* F */
    {14, 17, 16, 23, 17, 17, 15}, /* G */
    {17, 17, 17, 31, 17, 17, 17},/* H */
    {14, 4, 4, 4, 4, 4, 14},      /* I */
    {7, 2, 2, 2, 18, 18, 12},     /* J */
    {17, 18, 20, 24, 20, 18, 17},/* K */
    {16, 16, 16, 16, 16, 16, 31},/* L */
    {17, 27, 21, 21, 17, 17, 17},/* M */
    {17, 25, 21, 19, 17, 17, 17},/* N */
    {14, 17, 17, 17, 17, 17, 14},/* O */
    {30, 17, 17, 30, 16, 16, 16},/* P */
    {14, 17, 17, 17, 21, 18, 13},/* Q */
    {30, 17, 17, 30, 20, 18, 17},/* R */
    {15, 16, 16, 14, 1, 1, 30},   /* S */
    {31, 4, 4, 4, 4, 4, 4},       /* T */
    {17, 17, 17, 17, 17, 17, 14},/* U */
    {17, 17, 17, 17, 17, 10, 4}, /* V */
    {17, 17, 17, 21, 21, 21, 10},/* W */
    {17, 17, 10, 4, 10, 17, 17}, /* X */
    {17, 17, 10, 4, 4, 4, 4},    /* Y */
    {31, 1, 2, 4, 8, 16, 31},    /* Z */
    {0, 0, 0, 0, 0, 12, 12},     /* . */
    {0, 0, 0, 31, 0, 0, 0},      /* - */
    {0, 4, 4, 0, 4, 4, 0},       /* | */
    {1, 2, 4, 8, 4, 2, 1},       /* > */
    {4, 21, 14, 31, 14, 21, 4},  /* * */
    {0, 0, 0, 10, 21, 0, 0},     /* ~ */
};

static int GetOverlayGlyphIndex(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'A' && character <= 'Z')
        return character - 'A' + 10;
    if (character == '.')
        return 36;
    if (character == '-')
        return 37;
    if (character == '|')
        return 38;
    if (character == '>')
        return 39;
    if (character == '*')
        return 40;
    if (character == '~')
        return 41;
    return -1;
}

static void DrawOverlayText(SDL_Surface *surface, int x, int y,
                            const char *text, Uint32 color)
{
    Uint8 *surfacePixels = (Uint8 *)surface->pixels;

    for (; *text != '\0'; text++, x += 12)
    {
        int glyphIndex = GetOverlayGlyphIndex(*text);
        if (glyphIndex < 0)
            continue;

        for (int row = 0; row < 7; row++)
        {
            uint8_t glyphRow = sOverlayFont[glyphIndex][row];
            for (int column = 0; column < 5; column++)
            {
                if (!(glyphRow & (1 << (4 - column))))
                    continue;

                for (int pixelY = 0; pixelY < 2; pixelY++)
                {
                    Uint32 *destination = (Uint32 *)(surfacePixels
                        + (y + row * 2 + pixelY) * surface->pitch)
                        + x + column * 2;
                    destination[0] = color;
                    destination[1] = color;
                }
            }
        }
    }
}

static void GetOverlayValues(
    struct VitaPerfRollingSummary values[PERF_OVERLAY_METRIC_COUNT])
{
    struct VitaPerfRollingWindow snapshots[PERF_OVERLAY_METRIC_COUNT];

    SDL_AtomicLock(&sLock);
    memcpy(snapshots, sRollingFrames, sizeof(snapshots));
    SDL_AtomicUnlock(&sLock);

    for (int i = 0; i < PERF_OVERLAY_METRIC_COUNT; i++)
        VitaPerfRolling_Summarize(&snapshots[i], &values[i]);
}

static void FormatOverlayMs(uint64_t timeUs, char output[16])
{
    SDL_snprintf(output, 16, "%llu.%03llu",
                 (unsigned long long)(timeUs / 1000),
                 (unsigned long long)(timeUs % 1000));
}

static void RedrawOverlay(void)
{
    struct VitaPerfRollingSummary values[PERF_OVERLAY_METRIC_COUNT];
    char line[80];
    Uint32 panelColor;
    Uint32 titleColor;
    Uint32 headerColor;
    Uint32 textColor;

    GetOverlayValues(values);
    panelColor = SDL_MapRGBA(sOverlaySurface->format, 0, 0, 0, 208);
    titleColor = SDL_MapRGBA(sOverlaySurface->format, 130, 255, 170, 255);
    headerColor = SDL_MapRGBA(sOverlaySurface->format, 150, 190, 255, 255);
    textColor = SDL_MapRGBA(sOverlaySurface->format, 255, 255, 255, 255);
    SDL_FillRect(sOverlaySurface, NULL, panelColor);
    DrawOverlayText(sOverlaySurface, 10, 8, "ROLLING 300 AVG MED P99 MS | SQUARE HIDE", titleColor);
    DrawOverlayText(sOverlaySurface, 10, 27, "BUCKET N PAR SER AVG MS MED MS P99 MS", headerColor);

    for (int i = 0; i < PERF_OVERLAY_METRIC_COUNT; i++)
    {
        char average[16];
        char median[16];
        char p99[16];

        if (values[i].count == 0)
        {
            if (i == PERF_ROLLING_PPU)
                SDL_snprintf(line, sizeof(line), "%s 0 0 0 ------ ------ ------",
                             sOverlayLabels[i]);
            else
                SDL_snprintf(line, sizeof(line), "%s 0 - - ------ ------ ------",
                             sOverlayLabels[i]);
            DrawOverlayText(sOverlaySurface, 10, 46 + i * 19, line, textColor);
            continue;
        }

        FormatOverlayMs(values[i].averageUs, average);
        FormatOverlayMs(values[i].medianUs, median);
        FormatOverlayMs(values[i].p99Us, p99);
        if (i == PERF_ROLLING_PPU)
            SDL_snprintf(line, sizeof(line), "%s %u %u %u %s %s %s",
                         sOverlayLabels[i], values[i].count,
                         values[i].taggedSamples,
                         values[i].count - values[i].taggedSamples,
                         average, median, p99);
        else
            SDL_snprintf(line, sizeof(line), "%s %u - - %s %s %s",
                         sOverlayLabels[i], values[i].count,
                         average, median, p99);
        DrawOverlayText(sOverlaySurface, 10, 46 + i * 19, line, textColor);
    }
    DrawOverlayText(sOverlaySurface, 10, 122,
                    "GAME LOGIC AND M4A CGB SAME SERVICED FRAME", headerColor);
    DrawOverlayText(sOverlaySurface, 10, 141,
                    "PPU PAR DISPATCH>JOIN SER FULL DRAW", headerColor);
    DrawOverlayText(sOverlaySurface, 10, 160,
                    "PRESENT RENDER>SDL PRESENT RETURN ASYNC", headerColor);
    sOverlayGeneration++;
}

void VitaPerf_ToggleOverlay(void)
{
    sOverlayVisible = !sOverlayVisible;
    sOverlayDirty = SDL_TRUE;
    sOverlayLastUpdate = 0;
}

bool VitaPerf_GetOverlayImage(const void **pixels, int *width, int *height,
                              uint32_t *generation)
{
    uint64_t now;

    if (!sInitialized || !sOverlayVisible || sOverlayUnavailable)
        return false;

    if (sOverlaySurface == NULL)
    {
        sOverlaySurface = SDL_CreateRGBSurfaceWithFormat(0, PERF_OVERLAY_WIDTH,
                            PERF_OVERLAY_HEIGHT, 32, SDL_PIXELFORMAT_RGBA32);
        if (sOverlaySurface == NULL)
        {
            SDL_Log("Unable to create Vita performance overlay image: %s", SDL_GetError());
            sOverlayUnavailable = SDL_TRUE;
            return false;
        }
        sOverlayDirty = SDL_TRUE;
    }

    now = SDL_GetPerformanceCounter();
    if (sOverlayDirty || sOverlayLastUpdate == 0
     || now < sOverlayLastUpdate
     || now - sOverlayLastUpdate >= sFrequency * PERF_OVERLAY_REFRESH_US / 1000000)
    {
        RedrawOverlay();
        sOverlayLastUpdate = now;
        sOverlayDirty = SDL_FALSE;
    }

    if (pixels != NULL)
        *pixels = sOverlaySurface->pixels;
    if (width != NULL)
        *width = sOverlaySurface->w;
    if (height != NULL)
        *height = sOverlaySurface->h;
    if (generation != NULL)
        *generation = sOverlayGeneration;
    return true;
}

void VitaPerf_RenderOverlay(struct SDL_Renderer *renderer)
{
    uint64_t now;
    int outputWidth;
    int outputHeight;
    SDL_Rect destination = {10, 10, PERF_OVERLAY_WIDTH, PERF_OVERLAY_HEIGHT};

    if (!sInitialized || !sOverlayVisible || renderer == NULL || sOverlayUnavailable)
        return;

    if (sOverlayTexture == NULL || sOverlayRenderer != renderer)
    {
        if (sOverlayRenderer != NULL && sOverlayTexture != NULL)
            SDL_DestroyTexture(sOverlayTexture);
        if (sOverlaySurface != NULL)
            SDL_FreeSurface(sOverlaySurface);
        sOverlaySurface = SDL_CreateRGBSurfaceWithFormat(0, PERF_OVERLAY_WIDTH,
                            PERF_OVERLAY_HEIGHT, 32, SDL_PIXELFORMAT_RGBA32);
        sOverlayTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                            SDL_TEXTUREACCESS_STREAMING, PERF_OVERLAY_WIDTH,
                            PERF_OVERLAY_HEIGHT);
        if (sOverlaySurface == NULL || sOverlayTexture == NULL)
        {
            SDL_Log("Unable to create Vita performance overlay: %s", SDL_GetError());
            if (sOverlayTexture != NULL)
            {
                SDL_DestroyTexture(sOverlayTexture);
                sOverlayTexture = NULL;
            }
            if (sOverlaySurface != NULL)
            {
                SDL_FreeSurface(sOverlaySurface);
                sOverlaySurface = NULL;
            }
            sOverlayUnavailable = SDL_TRUE;
            return;
        }
        SDL_SetTextureBlendMode(sOverlayTexture, SDL_BLENDMODE_BLEND);
        sOverlayRenderer = renderer;
        sOverlayDirty = SDL_TRUE;
    }

    now = SDL_GetPerformanceCounter();
    if (sOverlayDirty || sOverlayLastUpdate == 0
     || now < sOverlayLastUpdate
     || now - sOverlayLastUpdate >= sFrequency * PERF_OVERLAY_REFRESH_US / 1000000)
    {
        RedrawOverlay();
        if (SDL_UpdateTexture(sOverlayTexture, NULL, sOverlaySurface->pixels,
                              sOverlaySurface->pitch) != 0)
        {
            SDL_Log("Unable to update Vita performance overlay: %s", SDL_GetError());
            sOverlayUnavailable = SDL_TRUE;
            return;
        }
        sOverlayLastUpdate = now;
        sOverlayDirty = SDL_FALSE;
    }

    if (SDL_GetRendererOutputSize(renderer, &outputWidth, &outputHeight) != 0
     || outputWidth <= 20 || outputHeight <= 20)
        return;
    if (destination.w > outputWidth - 20 || destination.h > outputHeight - 20)
    {
        double widthScale = (double)(outputWidth - 20) / destination.w;
        double heightScale = (double)(outputHeight - 20) / destination.h;
        double scale = widthScale < heightScale ? widthScale : heightScale;
        destination.w = (int)(destination.w * scale);
        destination.h = (int)(destination.h * scale);
    }
    SDL_RenderCopy(renderer, sOverlayTexture, NULL, &destination);
}

static void PrintPercentiles(struct PerfStrBuf *log, const char *name,
                             const struct PerfHistogram *histogram,
                             uint32_t samples)
{
    if (samples == 0)
    {
        PerfBufPrintf(log, " %s_p95_us=- %s_p99_us=- %s_hist_saturated_n=0",
                      name, name, name);
        return;
    }

    PerfBufPrintf(log, " %s_p95_us=%llu %s_p99_us=%llu %s_hist_saturated_n=%u",
                  name, (unsigned long long)GetPercentileUs(histogram, samples, 95),
                  name, (unsigned long long)GetPercentileUs(histogram, samples, 99),
                  name, histogram->saturatedSamples);
}

static void PrintRollingMetric(struct PerfStrBuf *log, const char *name,
                               const struct VitaPerfRollingSummary *summary)
{
    if (summary->count == 0)
    {
        PerfBufPrintf(log, " %s_n=0 %s_avg=- %s_median=- %s_p99=-",
                      name, name, name, name);
        return;
    }

    PerfBufPrintf(log, " %s_n=%u %s_avg=%.3f %s_median=%.3f %s_p99=%.3f",
                  name, summary->count,
                  name, (double)summary->averageUs / 1000.0,
                  name, (double)summary->medianUs / 1000.0,
                  name, (double)summary->p99Us / 1000.0);
}

static void PrintRollingFrameMetricsFromSnapshot(struct PerfStrBuf *log,
                                                 const struct VitaPerfReportSnapshot *snap)
{
    struct VitaPerfRollingSummary summaries[PERF_ROLLING_METRIC_COUNT];

    for (int i = 0; i < PERF_ROLLING_METRIC_COUNT; i++)
        VitaPerfRolling_Summarize(&snap->rollingFrames[i], &summaries[i]);

    PerfBufPrintf(log, " rolling_capacity_frames=%u game_logic_mixer_pair_missing_n=%u",
                  VITA_PERF_ROLLING_CAPACITY, snap->gameTimingPairMissingMixer);
    PrintRollingMetric(log, "game_ms", &summaries[PERF_ROLLING_GAME]);
    PrintRollingMetric(log, "ppu_ms", &summaries[PERF_ROLLING_PPU]);
    PerfBufPrintf(log, " ppu_ms_parallel_n=%u ppu_ms_serial_n=%u",
                  summaries[PERF_ROLLING_PPU].taggedSamples,
                  summaries[PERF_ROLLING_PPU].count
                      - summaries[PERF_ROLLING_PPU].taggedSamples);
    PrintRollingMetric(log, "upload_ms", &summaries[PERF_ROLLING_UPLOAD]);
    PrintRollingMetric(log, "present_ms", &summaries[PERF_ROLLING_PRESENT]);
    PrintRollingMetric(log, "audio_supply_ms", &summaries[PERF_ROLLING_AUDIO_SUPPLY]);
}

static void FormatPollReportSnapshot(const struct VitaPerfReportSnapshot *snap,
                                     struct PerfStrBuf *log)
{
    const uint64_t windowUs = TicksToUs(snap->windowTicks);
    if (snap->rendererInfoPending)
        PerfBufPrintf(log, "renderer=%s window=%dx%d output=%dx%d renderer_flags=0x%08x"
                           " renderer_accelerated=%u renderer_software=%u renderer_vsync=%u\n",
                      snap->rendererName, snap->windowWidth, snap->windowHeight,
                      snap->outputWidth, snap->outputHeight,
                      snap->rendererFlags,
                      (snap->rendererFlags & SDL_RENDERER_ACCELERATED) != 0,
                      (snap->rendererFlags & SDL_RENDERER_SOFTWARE) != 0,
                      (snap->rendererFlags & SDL_RENDERER_PRESENTVSYNC) != 0);
    PerfBufPrintf(log, "window_ms=%llu vblank_wait_returns=%u vblank_miss_n=%u emulated_fps=%.2f",
                  (unsigned long long)(windowUs / 1000), snap->waitReturns, snap->vblankMisses,
                  windowUs == 0 ? 0.0 : (double)snap->waitReturns * 1000000.0 / (double)windowUs);
    PrintVitaHardwareTelemetry(log);
    PrintStat(log, "emulated_tick_interval", &snap->stats[VITA_PERF_TICK_INTERVAL]);
    PrintPercentiles(log, "emulated_tick_interval", &snap->tickIntervalHistogram,
                     snap->stats[VITA_PERF_TICK_INTERVAL].samples);
    PrintStat(log, "game_logic", &snap->stats[VITA_PERF_GAME_LOGIC]);
    PrintStat(log, "frame_wait", &snap->stats[VITA_PERF_FRAME_WAIT]);
    PrintStat(log, "draw_frame", &snap->stats[VITA_PERF_DRAW_FRAME]);
    PrintPercentiles(log, "draw_frame", &snap->drawFrameHistogram,
                     snap->stats[VITA_PERF_DRAW_FRAME].samples);
    PrintStat(log, "pixel_convert", &snap->stats[VITA_PERF_PIXEL_CONVERT]);
    PrintStat(log, "texture_update", &snap->stats[VITA_PERF_TEXTURE_UPDATE]);
    PrintStat(log, "render_queue", &snap->stats[VITA_PERF_RENDER_QUEUE]);
    PrintStat(log, "present", &snap->stats[VITA_PERF_PRESENT]);
    PrintStat(log, "mixer", &snap->stats[VITA_PERF_MIXER]);
    PrintStat(log, "audio_supply", &snap->stats[VITA_PERF_AUDIO_SUPPLY]);
    PrintStat(log, "audio_queue", &snap->stats[VITA_PERF_AUDIO_QUEUE]);
    PrintRollingFrameMetricsFromSnapshot(log, snap);
    PrintStat(log, "main_loop_batch", &snap->stats[VITA_PERF_MAIN_LOOP_BATCH]);
    // classic_draw is a substage of render_queue and scene_classic.
    PrintStat(log, "classic_draw", &snap->stats[VITA_PERF_CLASSIC_DRAW]);
    PrintStat(log, "scene_classic", &snap->stats[VITA_PERF_SCENE_CLASSIC]);
    // This sample belongs to the previous report, since formatting this report
    // must finish before we know its duration.
    PrintStat(log, "report_io", &snap->stats[VITA_PERF_REPORT_IO]);
    PrintStat(log, "hblank_win0_replay", &snap->stats[VITA_PERF_HBLANK_WIN0_REPLAY]);
    PerfBufPrintf(log,
                  " main_loop_batch_n=%u main_loop_serviced_frames=%u main_loop_frames_per_iteration_avg=%.2f"
                  " main_loop_frames_per_iteration_max=%u main_loop_batch_hist_0=%u"
                  " main_loop_batch_hist_1=%u main_loop_batch_hist_2=%u"
                  " main_loop_batch_hist_3=%u main_loop_batch_hist_4plus=%u",
                  snap->mainLoopBatch.samples, snap->mainLoopBatch.servicedFrames,
                  snap->mainLoopBatch.samples == 0 ? 0.0
                      : (double)snap->mainLoopBatch.servicedFrames / snap->mainLoopBatch.samples,
                  snap->mainLoopBatch.maxServicedFrames,
                  snap->mainLoopBatch.histogram[0], snap->mainLoopBatch.histogram[1],
                  snap->mainLoopBatch.histogram[2], snap->mainLoopBatch.histogram[3],
                  snap->mainLoopBatch.histogram[4]);
    uint32_t easyFullParallelFrames = snap->easyDrawPaths[VITA_EASY_DRAW_FULL_PARALLEL];
    uint32_t easyVCountPrefixFrames = snap->easyDrawPaths[VITA_EASY_DRAW_VCOUNT_PREFIX];
    uint32_t easyWin0hDmaFrames = snap->easyDrawPaths[VITA_EASY_DRAW_WIN0H_DMA_PARALLEL];
    uint32_t easySerialFrames = snap->easyDrawPaths[VITA_EASY_DRAW_SERIAL];
    uint32_t easyParallelFrames = easyFullParallelFrames + easyVCountPrefixFrames + easyWin0hDmaFrames;
    uint32_t easyFrameCount = easyParallelFrames + easySerialFrames;
    PerfBufPrintf(log,
                  " easy_full_parallel_frames=%u easy_vcount_prefix_frames=%u"
                  " easy_win0h_dma_parallel_frames=%u easy_serial_frames=%u"
                  " easy_parallel_frames=%u easy_parallel_share_pct=%.1f"
                  " easy_serial_hblank_irq=%u easy_serial_hblank_dma=%u easy_serial_early_vcount=%u"
                  " easy_serial_worker_failure=%u easy_serial_other=%u",
                  easyFullParallelFrames, easyVCountPrefixFrames, easyWin0hDmaFrames, easySerialFrames,
                  easyParallelFrames,
                  easyFrameCount == 0 ? 0.0 : (double)easyParallelFrames * 100.0 / easyFrameCount,
                  snap->easyDrawFallbacks[VITA_EASY_DRAW_FALLBACK_HBLANK_IRQ],
                  snap->easyDrawFallbacks[VITA_EASY_DRAW_FALLBACK_HBLANK_DMA],
                  snap->easyDrawFallbacks[VITA_EASY_DRAW_FALLBACK_EARLY_VCOUNT],
                  snap->easyDrawFallbacks[VITA_EASY_DRAW_FALLBACK_WORKER_FAILURE],
                  snap->easyDrawFallbacks[VITA_EASY_DRAW_FALLBACK_OTHER]);
    PrintStat(log, "easy_dispatch", &snap->easyDrawTimingStats[VITA_EASY_DRAW_TIMING_DISPATCH]);
    PrintStat(log, "easy_main_range", &snap->easyDrawTimingStats[VITA_EASY_DRAW_TIMING_MAIN_RANGE]);
    PrintStat(log, "easy_worker0_schedule", &snap->easyDrawTimingStats[VITA_EASY_DRAW_TIMING_WORKER_0_SCHEDULE]);
    PrintStat(log, "easy_worker0_range", &snap->easyDrawTimingStats[VITA_EASY_DRAW_TIMING_WORKER_0_RANGE]);
    PrintStat(log, "easy_worker1_schedule", &snap->easyDrawTimingStats[VITA_EASY_DRAW_TIMING_WORKER_1_SCHEDULE]);
    PrintStat(log, "easy_worker1_range", &snap->easyDrawTimingStats[VITA_EASY_DRAW_TIMING_WORKER_1_RANGE]);
    PrintStat(log, "easy_join_wait", &snap->easyDrawTimingStats[VITA_EASY_DRAW_TIMING_JOIN_WAIT]);
    PrintStat(log, "easy_parallel_total", &snap->easyDrawTimingStats[VITA_EASY_DRAW_TIMING_PARALLEL_TOTAL]);
    if (snap->easyDrawHeight.samples == 0)
        PerfBufPrintf(log, " easy_parallel_height_n=0 easy_parallel_height_avg_rows=-"
                           " easy_parallel_height_min_rows=- easy_parallel_height_max_rows=-");
    else
        PerfBufPrintf(log, " easy_parallel_height_n=%u easy_parallel_height_avg_rows=%.1f"
                           " easy_parallel_height_min_rows=%u easy_parallel_height_max_rows=%u",
                      snap->easyDrawHeight.samples,
                      (double)snap->easyDrawHeight.totalRows / snap->easyDrawHeight.samples,
                      snap->easyDrawHeight.minRows, snap->easyDrawHeight.maxRows);
    if (snap->audioQueue.samples == 0)
        PerfBufPrintf(log, " audio_depth_n=0 audio_depth_avg_bytes=- audio_depth_max_bytes=-"
                           " audio_prequeue_zero_n=0 audio_supply_gap_n=0"
                           " audio_supply_gap_max_us=0 audio_overflow_clear_n=0\n");
    else
        PerfBufPrintf(log, " audio_depth_n=%u audio_depth_avg_bytes=%llu audio_depth_max_bytes=%u"
                           " audio_prequeue_zero_n=%u audio_supply_gap_n=%u"
                           " audio_supply_gap_max_us=%u audio_overflow_clear_n=%u\n",
                      snap->audioQueue.samples,
                      (unsigned long long)(snap->audioQueue.totalBytes / snap->audioQueue.samples),
                      snap->audioQueue.maxBytes,
                      snap->audioQueue.prequeueZeroCount,
                      snap->audioQueue.supplyGapCount,
                      snap->audioQueue.maxSupplyGapUs,
                      snap->audioQueue.overflowClearCount);
    if (snap->activeSpriteStat.samples == 0)
        PerfBufPrintf(log, " active_sprites_n=0 active_sprites_avg=- active_sprites_max=-");
    else
        PerfBufPrintf(log, " active_sprites_n=%u active_sprites_avg=%.1f active_sprites_max=%llu",
                      snap->activeSpriteStat.samples,
                      (double)snap->activeSpriteStat.totalUs / snap->activeSpriteStat.samples,
                      (unsigned long long)snap->activeSpriteStat.maxUs);

    double avgPpuMs = snap->stats[VITA_PERF_DRAW_FRAME].samples > 0
                    ? (double)snap->stats[VITA_PERF_DRAW_FRAME].totalUs / snap->stats[VITA_PERF_DRAW_FRAME].samples / 1000.0
                    : 0.0;
    double avgGameMs = snap->stats[VITA_PERF_GAME_LOGIC].samples > 0
                     ? (double)snap->stats[VITA_PERF_GAME_LOGIC].totalUs / snap->stats[VITA_PERF_GAME_LOGIC].samples / 1000.0
                     : 0.0;
    double avgMixerMs = snap->stats[VITA_PERF_MIXER].samples > 0
                      ? (double)snap->stats[VITA_PERF_MIXER].totalUs / snap->stats[VITA_PERF_MIXER].samples / 1000.0
                      : 0.0;
    double avgConvertMs = snap->stats[VITA_PERF_PIXEL_CONVERT].samples > 0
                        ? (double)snap->stats[VITA_PERF_PIXEL_CONVERT].totalUs / snap->stats[VITA_PERF_PIXEL_CONVERT].samples / 1000.0
                        : 0.0;
    double avgUpdateMs = snap->stats[VITA_PERF_TEXTURE_UPDATE].samples > 0
                       ? (double)snap->stats[VITA_PERF_TEXTURE_UPDATE].totalUs / snap->stats[VITA_PERF_TEXTURE_UPDATE].samples / 1000.0
                       : 0.0;
    double avgClassicDrawMs = snap->stats[VITA_PERF_CLASSIC_DRAW].samples > 0
                            ? (double)snap->stats[VITA_PERF_CLASSIC_DRAW].totalUs / snap->stats[VITA_PERF_CLASSIC_DRAW].samples / 1000.0
                            : 0.0;
    double avgAudioSupplyMs = snap->stats[VITA_PERF_AUDIO_SUPPLY].samples > 0
                            ? (double)snap->stats[VITA_PERF_AUDIO_SUPPLY].totalUs / snap->stats[VITA_PERF_AUDIO_SUPPLY].samples / 1000.0
                            : 0.0;
    double avgPresentMs = snap->stats[VITA_PERF_PRESENT].samples > 0
                        ? (double)snap->stats[VITA_PERF_PRESENT].totalUs / snap->stats[VITA_PERF_PRESENT].samples / 1000.0
                        : 0.0;

    int armClockMhz = scePowerGetArmClockFrequency();
    double cpuScale = (armClockMhz > 0) ? ((double)armClockMhz / 222.0) : 1.5;
    /* Sum disjoint CPU substages. render_queue, scene_classic and rolling
     * present_ms enclose some or all of these and must not be added here. */
    double criticalPathCpu222Ms = (avgPpuMs + avgGameMs + avgMixerMs + avgConvertMs
                                 + avgUpdateMs + avgClassicDrawMs + avgAudioSupplyMs) * cpuScale;
    double criticalPath222mhzMs = criticalPathCpu222Ms + avgPresentMs;
    double headroom222Ms = 16.666 - criticalPath222mhzMs;
    PerfBufPrintf(log, " critical_path_222mhz_ms=%.2f budget_222mhz_headroom_ms=%.2f\n",
                  criticalPath222mhzMs, headroom222Ms);
}

void VitaPerf_Poll(void)
{
    if (!sInitialized)
        return;

    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t windowTicks;

    SDL_AtomicLock(&sLock);
    windowTicks = now >= sWindowStart ? now - sWindowStart : 0;
    if (windowTicks < sFrequency * PERF_REPORT_SECONDS)
    {
        SDL_AtomicUnlock(&sLock);
        return;
    }

    sPollStaging.windowTicks = windowTicks;
    memcpy(sPollStaging.stats, sStats, sizeof(sPollStaging.stats));
    memset(sStats, 0, sizeof(sStats));
    sPollStaging.activeSpriteStat = sActiveSpriteStat;
    memset(&sActiveSpriteStat, 0, sizeof(sActiveSpriteStat));
    sPollStaging.drawFrameHistogram = sDrawFrameHistogram;
    memset(&sDrawFrameHistogram, 0, sizeof(sDrawFrameHistogram));
    sPollStaging.tickIntervalHistogram = sTickIntervalHistogram;
    memset(&sTickIntervalHistogram, 0, sizeof(sTickIntervalHistogram));
    memcpy(sPollStaging.rollingFrames, sRollingFrames, sizeof(sPollStaging.rollingFrames));
    sPollStaging.audioQueue = sAudioQueue;
    memset(&sAudioQueue, 0, sizeof(sAudioQueue));
    sPollStaging.mainLoopBatch = sMainLoopBatch;
    memset(&sMainLoopBatch, 0, sizeof(sMainLoopBatch));
    memcpy(sPollStaging.easyDrawPaths, sEasyDrawPaths, sizeof(sPollStaging.easyDrawPaths));
    memset(sEasyDrawPaths, 0, sizeof(sEasyDrawPaths));
    memcpy(sPollStaging.easyDrawFallbacks, sEasyDrawFallbacks, sizeof(sPollStaging.easyDrawFallbacks));
    memset(sEasyDrawFallbacks, 0, sizeof(sEasyDrawFallbacks));
    memcpy(sPollStaging.easyDrawTimingStats, sEasyDrawTimingStats, sizeof(sPollStaging.easyDrawTimingStats));
    memset(sEasyDrawTimingStats, 0, sizeof(sEasyDrawTimingStats));
    sPollStaging.easyDrawHeight = sEasyDrawHeight;
    memset(&sEasyDrawHeight, 0, sizeof(sEasyDrawHeight));
    sPollStaging.waitReturns = sVBlankWaitReturns;
    sVBlankWaitReturns = 0;
    sPollStaging.vblankMisses = sVBlankMissCount;
    sVBlankMissCount = 0;
    sPollStaging.gameTimingPairMissingMixer = sGameTimingPairMissingMixer;
    sGameTimingPairMissingMixer = 0;
    sWindowStart = now;
    sPollStaging.rendererInfoPending = sRendererInfoPending;
    sRendererInfoPending = SDL_FALSE;
    SDL_strlcpy(sPollStaging.rendererName, sRendererName, sizeof(sPollStaging.rendererName));
    sPollStaging.windowWidth = sWindowWidth;
    sPollStaging.windowHeight = sWindowHeight;
    sPollStaging.outputWidth = sOutputWidth;
    sPollStaging.outputHeight = sOutputHeight;
    sPollStaging.rendererFlags = sRendererFlags;
    SDL_AtomicUnlock(&sLock);

    if (sLog == NULL || sLogThread == NULL || sLogSem == NULL
     || SDL_AtomicGet(&sLogThreadQuit) != 0)
        return;

    SDL_AtomicLock(&sLogQueueLock);
    if (sLogQueueCount < PERF_LOG_QUEUE_SLOTS)
    {
        struct PerfLogChunk *slot = &sLogQueue[sLogQueueTail];
        slot->kind = PERF_LOG_CHUNK_POLL_REPORT;
        memcpy(&slot->u.report, &sPollStaging, sizeof(sPollStaging));
        sLogQueueTail = (sLogQueueTail + 1u) % PERF_LOG_QUEUE_SLOTS;
        sLogQueueCount++;
        SDL_AtomicUnlock(&sLogQueueLock);
        SDL_SemPost(sLogSem);
    }
    else
    {
        SDL_AtomicUnlock(&sLogQueueLock);
    }
    VitaPerf_Record(VITA_PERF_REPORT_IO, now, VitaPerf_Now());
}

#endif // defined(PLATFORM_VITA) && !defined(VITA_PERF_DISABLE)
