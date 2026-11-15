#include "gba_easy_draw_workers.h"
#include "platform/vita_perf.h"
#include "vita_thread_affinity.h"
#include <string.h>

#ifdef EASY_DRAW_TEST_PARALLEL
static int sTestFailAfter = -1;

static bool ShouldInjectThreadCreateFailure(void)
{
    if (sTestFailAfter == 0)
    {
        sTestFailAfter = -1;
        return true;
    }
    if (sTestFailAfter > 0)
        sTestFailAfter--;
    return false;
}
#endif

#if defined(PLATFORM_VITA)

#include <SDL2/SDL.h>

#define EASY_DRAW_WORKER_COUNT VITA_PERF_EASY_DRAW_WORKER_COUNT

#ifndef EASY_DRAW_CHUNK_SIZE
#define EASY_DRAW_CHUNK_SIZE 8
#endif

struct EasyDrawWorker
{
    SDL_Thread *thread;
    SDL_sem *wake;
    SDL_sem *finished;
    int stopping;
    int index;
#if VITA_PERF_ENABLED
    uint64_t rangeStart;
    uint64_t rangeEnd;
#endif
};

static struct EasyDrawWorker sWorkers[EASY_DRAW_WORKER_COUNT];
static int sWorkerCount;
static bool sWorkersReady;

static GbaEasyDrawRangeFn sCurrentDrawRange;
static void *sCurrentContext;
static int sCurrentTotalHeight;
static int sNextChunkLine;

static int EasyDrawWorkerMain(void *data)
{
    struct EasyDrawWorker *worker = data;

    /* Pin worker 0 to user core 1 (idle while AgbMain waits in VBlank) and
     * worker 1 to user core 2 so all 3 cores render scanlines in parallel. */
    (void)VitaThread_PinCurrentToCore(1 + (worker->index & 1));

    for (;;)
    {
        if (SDL_SemWait(worker->wake) != 0)
            continue;
        if (worker->stopping)
            break;

#if VITA_PERF_ENABLED
        worker->rangeStart = VitaPerf_Now();
#endif
        int chunkLine;
        while ((chunkLine = __sync_fetch_and_add(&sNextChunkLine, EASY_DRAW_CHUNK_SIZE)) < sCurrentTotalHeight)
        {
            int endLine = chunkLine + EASY_DRAW_CHUNK_SIZE;
            if (endLine > sCurrentTotalHeight)
                endLine = sCurrentTotalHeight;
            sCurrentDrawRange(sCurrentContext, chunkLine, endLine);
        }
#if VITA_PERF_ENABLED
        worker->rangeEnd = VitaPerf_Now();
#endif
        SDL_SemPost(worker->finished);
    }

    return 0;
}

static void StopWorkers(void)
{
    for (int i = 0; i < sWorkerCount; i++)
    {
        struct EasyDrawWorker *worker = &sWorkers[i];
        if (worker->thread != NULL)
        {
            worker->stopping = 1;
            SDL_SemPost(worker->wake);
        }
    }

    for (int i = 0; i < sWorkerCount; i++)
    {
        struct EasyDrawWorker *worker = &sWorkers[i];
        if (worker->thread != NULL)
        {
            SDL_WaitThread(worker->thread, NULL);
            worker->thread = NULL;
        }
        if (worker->wake != NULL)
        {
            SDL_DestroySemaphore(worker->wake);
            worker->wake = NULL;
        }
        if (worker->finished != NULL)
        {
            SDL_DestroySemaphore(worker->finished);
            worker->finished = NULL;
        }
    }

    SDL_memset(sWorkers, 0, sizeof(sWorkers));
    sWorkerCount = 0;
    sWorkersReady = false;
}

static bool StartWorkers(void)
{
    if (sWorkersReady)
        return true;

    for (int i = 0; i < EASY_DRAW_WORKER_COUNT; i++)
    {
        struct EasyDrawWorker *worker = &sWorkers[i];
        sWorkerCount = i + 1;
        worker->index = i;
        worker->wake = SDL_CreateSemaphore(0);
        worker->finished = SDL_CreateSemaphore(0);
        if (worker->wake == NULL || worker->finished == NULL)
        {
            StopWorkers();
            return false;
        }

        char name[24];
        SDL_snprintf(name, sizeof(name), "EasyDraw-%d", i);
        /* SDL's Vita backend uses affinity mask 0 (all available CPUs). */
#ifdef EASY_DRAW_TEST_PARALLEL
        if (ShouldInjectThreadCreateFailure())
        {
            StopWorkers();
            return false;
        }
#endif
        worker->thread = SDL_CreateThread(EasyDrawWorkerMain, name, worker);
        if (worker->thread == NULL)
        {
            StopWorkers();
            return false;
        }
    }

    sWorkersReady = true;
    return true;
}

bool GbaEasyDrawWorkers_Run(GbaEasyDrawRangeFn drawRange, void *context, int height)
{
    if (drawRange == NULL || height < EASY_DRAW_WORKER_COUNT + 1 || !StartWorkers())
        return false;

#if VITA_PERF_ENABLED
    struct VitaEasyDrawTiming timing;
    SDL_memset(&timing, 0, sizeof(timing));
    timing.parallelHeight = height;
#endif
    sCurrentDrawRange = drawRange;
    sCurrentContext = context;
    sCurrentTotalHeight = height;
    __atomic_store_n(&sNextChunkLine, 0, __ATOMIC_RELEASE);

    int signaledWorkers = 0;
#if VITA_PERF_ENABLED
    timing.dispatchStart = VitaPerf_Now();
#endif
    for (int i = 0; i < EASY_DRAW_WORKER_COUNT; i++)
    {
        if (SDL_SemPost(sWorkers[i].wake) != 0)
            break;
        signaledWorkers++;
    }
#if VITA_PERF_ENABLED
    timing.dispatchEnd = VitaPerf_Now();
#endif

    if (signaledWorkers != EASY_DRAW_WORKER_COUNT)
    {
        __atomic_store_n(&sNextChunkLine, sCurrentTotalHeight, __ATOMIC_RELEASE);
        for (int i = 0; i < signaledWorkers; i++)
            while (SDL_SemWait(sWorkers[i].finished) != 0) { }
        return false;
    }

#if VITA_PERF_ENABLED
    timing.mainStart = VitaPerf_Now();
#endif
    int chunkLine;
    while ((chunkLine = __sync_fetch_and_add(&sNextChunkLine, EASY_DRAW_CHUNK_SIZE)) < sCurrentTotalHeight)
    {
        int endLine = chunkLine + EASY_DRAW_CHUNK_SIZE;
        if (endLine > sCurrentTotalHeight)
            endLine = sCurrentTotalHeight;
        drawRange(context, chunkLine, endLine);
    }
#if VITA_PERF_ENABLED
    timing.mainEnd = VitaPerf_Now();
#endif
    for (int i = 0; i < EASY_DRAW_WORKER_COUNT; i++)
        while (SDL_SemWait(sWorkers[i].finished) != 0) { }
#if VITA_PERF_ENABLED
    timing.completed = VitaPerf_Now();
    for (int i = 0; i < EASY_DRAW_WORKER_COUNT; i++)
    {
        timing.workerStart[i] = sWorkers[i].rangeStart;
        timing.workerEnd[i] = sWorkers[i].rangeEnd;
    }
    VitaPerf_RecordEasyDrawTiming(&timing);
#endif
    return true;
}

void GbaEasyDrawWorkers_Shutdown(void)
{
    StopWorkers();
}

#elif defined(EASY_DRAW_TEST_PARALLEL)

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

#define EASY_DRAW_WORKER_COUNT VITA_PERF_EASY_DRAW_WORKER_COUNT

#ifndef EASY_DRAW_CHUNK_SIZE
#define EASY_DRAW_CHUNK_SIZE 16
#endif

struct EasyDrawWorker
{
    pthread_t thread;
    sem_t wake;
    sem_t finished;
    int stopping;
    bool threadStarted;
    bool wakeReady;
    bool finishedReady;
#if VITA_PERF_ENABLED
    uint64_t rangeStart;
    uint64_t rangeEnd;
#endif
};

static struct EasyDrawWorker sWorkers[EASY_DRAW_WORKER_COUNT];
static int sWorkerCount;
static bool sWorkersReady;

static GbaEasyDrawRangeFn sCurrentDrawRange;
static void *sCurrentContext;
static int sCurrentTotalHeight;
static int sNextChunkLine;

static bool WaitSemaphore(sem_t *semaphore)
{
    int result;
    do
    {
        result = sem_wait(semaphore);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static void *EasyDrawWorkerMain(void *data)
{
    struct EasyDrawWorker *worker = data;

    for (;;)
    {
        if (!WaitSemaphore(&worker->wake))
            continue;
        if (worker->stopping)
            break;

#if VITA_PERF_ENABLED
        worker->rangeStart = VitaPerf_Now();
#endif
        int chunkLine;
        while ((chunkLine = __sync_fetch_and_add(&sNextChunkLine, EASY_DRAW_CHUNK_SIZE)) < sCurrentTotalHeight)
        {
            int endLine = chunkLine + EASY_DRAW_CHUNK_SIZE;
            if (endLine > sCurrentTotalHeight)
                endLine = sCurrentTotalHeight;
            sCurrentDrawRange(sCurrentContext, chunkLine, endLine);
        }
#if VITA_PERF_ENABLED
        worker->rangeEnd = VitaPerf_Now();
#endif
        sem_post(&worker->finished);
    }

    return NULL;
}

static void StopWorkers(void)
{
    for (int i = 0; i < sWorkerCount; i++)
    {
        struct EasyDrawWorker *worker = &sWorkers[i];
        if (worker->threadStarted)
        {
            worker->stopping = 1;
            sem_post(&worker->wake);
        }
    }

    for (int i = 0; i < sWorkerCount; i++)
    {
        struct EasyDrawWorker *worker = &sWorkers[i];
        if (worker->threadStarted)
            pthread_join(worker->thread, NULL);
        if (worker->wakeReady)
            sem_destroy(&worker->wake);
        if (worker->finishedReady)
            sem_destroy(&worker->finished);
    }

    memset(sWorkers, 0, sizeof(sWorkers));
    sWorkerCount = 0;
    sWorkersReady = false;
}

static bool StartWorkers(void)
{
    if (sWorkersReady)
        return true;

    for (int i = 0; i < EASY_DRAW_WORKER_COUNT; i++)
    {
        struct EasyDrawWorker *worker = &sWorkers[i];
        sWorkerCount = i + 1;
        if (sem_init(&worker->wake, 0, 0) != 0)
        {
            StopWorkers();
            return false;
        }
        worker->wakeReady = true;
        if (sem_init(&worker->finished, 0, 0) != 0)
        {
            StopWorkers();
            return false;
        }
        worker->finishedReady = true;

        if (ShouldInjectThreadCreateFailure())
        {
            StopWorkers();
            return false;
        }
        if (pthread_create(&worker->thread, NULL, EasyDrawWorkerMain, worker) != 0)
        {
            StopWorkers();
            return false;
        }
        worker->threadStarted = true;
    }

    sWorkersReady = true;
    return true;
}

bool GbaEasyDrawWorkers_Run(GbaEasyDrawRangeFn drawRange, void *context, int height)
{
    if (drawRange == NULL || height < EASY_DRAW_WORKER_COUNT + 1 || !StartWorkers())
        return false;

#if VITA_PERF_ENABLED
    struct VitaEasyDrawTiming timing;
    memset(&timing, 0, sizeof(timing));
    timing.parallelHeight = height;
#endif
    sCurrentDrawRange = drawRange;
    sCurrentContext = context;
    sCurrentTotalHeight = height;
    __atomic_store_n(&sNextChunkLine, 0, __ATOMIC_RELEASE);

    int signaledWorkers = 0;
#if VITA_PERF_ENABLED
    timing.dispatchStart = VitaPerf_Now();
#endif
    for (int i = 0; i < EASY_DRAW_WORKER_COUNT; i++)
    {
        if (sem_post(&sWorkers[i].wake) != 0)
            break;
        signaledWorkers++;
    }
#if VITA_PERF_ENABLED
    timing.dispatchEnd = VitaPerf_Now();
#endif

    if (signaledWorkers != EASY_DRAW_WORKER_COUNT)
    {
        __atomic_store_n(&sNextChunkLine, sCurrentTotalHeight, __ATOMIC_RELEASE);
        for (int i = 0; i < signaledWorkers; i++)
            while (!WaitSemaphore(&sWorkers[i].finished)) { }
        return false;
    }

#if VITA_PERF_ENABLED
    timing.mainStart = VitaPerf_Now();
#endif
    int chunkLine;
    while ((chunkLine = __sync_fetch_and_add(&sNextChunkLine, EASY_DRAW_CHUNK_SIZE)) < sCurrentTotalHeight)
    {
        int endLine = chunkLine + EASY_DRAW_CHUNK_SIZE;
        if (endLine > sCurrentTotalHeight)
            endLine = sCurrentTotalHeight;
        drawRange(context, chunkLine, endLine);
    }
#if VITA_PERF_ENABLED
    timing.mainEnd = VitaPerf_Now();
#endif
    for (int i = 0; i < EASY_DRAW_WORKER_COUNT; i++)
        while (!WaitSemaphore(&sWorkers[i].finished)) { }
#if VITA_PERF_ENABLED
    timing.completed = VitaPerf_Now();
    for (int i = 0; i < EASY_DRAW_WORKER_COUNT; i++)
    {
        timing.workerStart[i] = sWorkers[i].rangeStart;
        timing.workerEnd[i] = sWorkers[i].rangeEnd;
    }
    VitaPerf_RecordEasyDrawTiming(&timing);
#endif
    return true;
}

void GbaEasyDrawWorkers_Shutdown(void)
{
    StopWorkers();
}

#else

bool GbaEasyDrawWorkers_Run(GbaEasyDrawRangeFn drawRange, void *context, int height)
{
    (void)drawRange;
    (void)context;
    (void)height;
    return false;
}

void GbaEasyDrawWorkers_Shutdown(void)
{
}

#endif

#ifdef EASY_DRAW_TEST_PARALLEL
void GbaEasyDrawWorkers_TestFailAfter(int successfulThreadCreates)
{
    sTestFailAfter = successfulThreadCreates;
}
#endif
