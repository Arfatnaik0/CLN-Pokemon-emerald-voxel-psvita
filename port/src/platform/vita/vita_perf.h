#ifndef GUARD_VITA_VOXEL_PERF_H
#define GUARD_VITA_VOXEL_PERF_H

/*
 * Lightweight compatibility shim for HctrRB/pokeemerald-vita-3ds voxel_renderer
 * telemetry markers (MIT; see LICENSE and NOTICE.md). Keeps our optimized
 * 2D/async perf logger in src/platform/vita_perf.c separate.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

double VitaPerf_NowMs(void);
void VitaPerf_LogMemory(const char *stage);

typedef enum
{
    VITA_PERF_T_BUILD_GROUND,
    VITA_PERF_T_BUILD_ATLAS_EXT,
    VITA_PERF_T_BUILD_TREES,
    VITA_PERF_T_BUILD_BUILDINGS,
    VITA_PERF_T_BUILD_UPLOAD,
    VITA_PERF_T_ATLAS_COMPOSE,
    VITA_PERF_T_ATLAS_UPLOAD,
    VITA_PERF_T_MAP_SHIFT,
    VITA_PERF_T_WORLD_INSTANCES,
    VITA_PERF_T_ATLAS_SLOTS,
    VITA_PERF_T_WORKER_COMPUTE,
    VITA_PERF_T_WORKER_WAIT,
    VITA_PERF_T_ENTITIES,
    VITA_PERF_T_LIVE_DIGEST,
    VITA_PERF_T_LIGHT_RAYS,
    VITA_PERF_T_LIGHT_CONTACT,
    VITA_PERF_T_LIGHT_API,
    VITA_PERF_T_UI2_RENDER,
    VITA_PERF_T_UI2_TICK,
    VITA_PERF_T_COUNT
} VitaPerfTimer;

typedef enum
{
    VITA_PERF_C_LIGHT_RAYS,
    VITA_PERF_C_BUILD_VERTS,
    VITA_PERF_C_ATLAS_UPLOADS,
    VITA_PERF_C_HOLES,
    VITA_PERF_C_LIGHT_STEPS,
    VITA_PERF_C_LIGHT_CELLS,
    VITA_PERF_C_LIGHT_BLOCKS,
    VITA_PERF_C_WORKER_BUILT,
    VITA_PERF_C_WORKER_DROPPED,
    VITA_PERF_C_WORKER_OVERLAP,
    VITA_PERF_C_WORKER_HELD,
    VITA_PERF_C_UI2_QUADS,
    VITA_PERF_C_COUNT
} VitaPerfCounter;

typedef enum
{
    VITA_PERF_E_MAP_RESET = 1u << 0,
    VITA_PERF_E_CROSS = 1u << 1,
    VITA_PERF_E_LIVE = 1u << 2,
    VITA_PERF_E_SUN = 1u << 3,
    VITA_PERF_E_WARMUP = 1u << 4,
    VITA_PERF_E_SYNC = 1u << 5,
    VITA_PERF_E_ATLAS_EXT = 1u << 6,
    VITA_PERF_E_ATLAS_EVICT = 1u << 7,
} VitaPerfEvent;

static inline void VitaPerf_MarkAtlasAnimStart(void) {}
static inline void VitaPerf_MarkAtlasAnimEnd(uint32_t rowsUpdated) { (void)rowsUpdated; }
static inline void VitaPerf_MarkCullStart(void) {}
static inline void VitaPerf_MarkCullEnd(int32_t candidates, int32_t visible) { (void)candidates; (void)visible; }
static inline void VitaPerf_MarkBuildStart(void) {}
static inline void VitaPerf_MarkBuildEnd(int32_t chunksBuilt) { (void)chunksBuilt; }
static inline void VitaPerf_MarkDraw3DStart(void) {}
static inline void VitaPerf_MarkDraw3DEnd(int32_t drawCalls, uint32_t vertexCount) { (void)drawCalls; (void)vertexCount; }
static inline void VitaPerf_MarkUiScanlinesStart(void) {}
static inline void VitaPerf_MarkUiScanlinesEnd(void) {}
static inline void VitaPerf_MarkUiUploadStart(void) {}
static inline void VitaPerf_MarkUiUploadEnd(void) {}
static inline void VitaPerf_MarkSwapStart(void) {}
static inline void VitaPerf_MarkSwapEnd(void) {}
static inline void VitaPerf_SubBegin(VitaPerfTimer timer) { (void)timer; }
static inline void VitaPerf_SubEnd(VitaPerfTimer timer) { (void)timer; }
static inline void VitaPerf_AddTime(VitaPerfTimer timer, double ms) { (void)timer; (void)ms; }
static inline void VitaPerf_AddCount(VitaPerfCounter counter, uint32_t amount) { (void)counter; (void)amount; }
static inline void VitaPerf_MarkEvent(VitaPerfEvent event) { (void)event; }

#ifdef __cplusplus
}
#endif

#endif // GUARD_VITA_VOXEL_PERF_H
