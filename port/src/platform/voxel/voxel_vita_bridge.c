#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "platform.h"
#include "platform/vita_extras.h"
#include "../vita/vita_perf.h"
#include "../vita/vita_platform.h"

#ifdef PLATFORM_VITA
#include <psp2/kernel/clib.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

/* Match upstream Vita port: reserve 64 MB for Newlib heap instead of taking
 * almost all user RAM before vglInitWithCustomSizes runs. */
int32_t _newlib_heap_size_user = 64 * 1024 * 1024;
#else
#include <SDL.h>
#include <time.h>
#endif

static int sClockSpeed = 0;

int VitaExtras_GetFast(FastExtraId id)
{
    switch (id)
    {
    case FAST_EXTRA_DAY_NIGHT:
        return 1;
    case FAST_EXTRA_CLOCK_SPEED:
        return sClockSpeed;
    case FAST_EXTRA_VOXEL_3D:
        return Platform_GetSetting(PLATFORM_SETTING_VOXEL_3D) != 0;
    case FAST_EXTRA_VOXEL_BATTLE:
        return 0; /* Battles remain in classic 2D style. */
    case FAST_EXTRA_VOXEL_RES:
        return Platform_GetSetting(PLATFORM_SETTING_VOXEL_RES);
    case FAST_EXTRA_VOXEL_FPS:
        return Platform_GetSetting(PLATFORM_SETTING_VOXEL_FPS);
    case FAST_EXTRA_VOXEL_BLUR:
        return 0;
    default:
        return 0;
    }
}

int VitaExtras_Get(const char *key)
{
    if (key == NULL)
        return 0;
    if (strcmp(key, "clock_speed") == 0)
        return sClockSpeed;
    if (strcmp(key, "voxel_3d") == 0)
        return Platform_GetSetting(PLATFORM_SETTING_VOXEL_3D);
    if (strcmp(key, "voxel_res") == 0)
        return Platform_GetSetting(PLATFORM_SETTING_VOXEL_RES);
    if (strcmp(key, "voxel_fps") == 0)
        return Platform_GetSetting(PLATFORM_SETTING_VOXEL_FPS);
    return 0;
}

void VitaExtras_Set(const char *key, int value)
{
    if (key == NULL)
        return;
    if (strcmp(key, "clock_speed") == 0)
        sClockSpeed = value;
    else if (strcmp(key, "voxel_3d") == 0)
        Platform_SetSetting(PLATFORM_SETTING_VOXEL_3D, (u8)value);
    else if (strcmp(key, "voxel_res") == 0)
        Platform_SetSetting(PLATFORM_SETTING_VOXEL_RES, (u8)value);
    else if (strcmp(key, "voxel_fps") == 0)
        Platform_SetSetting(PLATFORM_SETTING_VOXEL_FPS, (u8)value);
}

double VitaPerf_NowMs(void)
{
#ifdef PLATFORM_VITA
    return (double)sceKernelGetProcessTimeWide() * 0.001;
#else
    return (double)SDL_GetTicks64();
#endif
}

void VitaPerf_LogMemory(const char *stage)
{
    VitaPlatform_Log("[VOXEL] memory checkpoint: %s\n", stage ? stage : "");
}

void VitaPlatform_PinCurrentThread(const char *name, int32_t core)
{
#ifdef PLATFORM_VITA
    if (core >= 0 && core <= 2)
    {
        SceUID thid = sceKernelGetThreadId();
        int32_t ret = sceKernelChangeThreadCpuAffinityMask(thid, SCE_KERNEL_CPU_MASK_USER_0 << core);
        VitaPlatform_Log("[THREAD] %s -> core %d (ret=0x%x)\n", name ? name : "worker", core, ret);
        return;
    }
#endif
    (void)name;
    (void)core;
}

int32_t VitaPlatform_GetLocalTime(struct VitaLocalTime *out)
{
    if (out == NULL)
        return -1;
#ifdef PLATFORM_VITA
    SceDateTime now;
    int32_t ret = sceRtcGetCurrentClockLocalTime(&now);
    if (ret < 0)
        return ret;
    out->year = now.year;
    out->month = now.month;
    out->day = now.day;
    out->dayOfWeek = sceRtcGetDayOfWeek(now.year, now.month, now.day);
    out->hour = now.hour;
    out->minute = now.minute;
    out->second = now.second;
    return 0;
#else
    time_t t = time(NULL);
    struct tm tmNow;
    localtime_r(&t, &tmNow);
    out->year = tmNow.tm_year + 1900;
    out->month = tmNow.tm_mon + 1;
    out->day = tmNow.tm_mday;
    out->dayOfWeek = tmNow.tm_wday;
    out->hour = tmNow.tm_hour;
    out->minute = tmNow.tm_min;
    out->second = tmNow.tm_sec;
    return 0;
#endif
}

uint64_t VitaPlatform_GetMilliseconds(void)
{
#ifdef PLATFORM_VITA
    return (uint64_t)sceKernelGetProcessTimeWide() / 1000ULL;
#else
    return (uint64_t)SDL_GetTicks64();
#endif
}

int64_t VitaPlatform_GetWallSeconds(void)
{
#ifdef PLATFORM_VITA
    SceRtcTick tick;
    if (sceRtcGetCurrentTick(&tick) < 0)
        return 0;
    return (int64_t)(tick.tick / 1000000ULL);
#else
    return (int64_t)time(NULL);
#endif
}

void VitaPlatform_Log(const char *fmt, ...)
{
#ifndef VITA_LOGGING
    (void)fmt;
#else
    if (fmt == NULL)
        return;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
#endif
}

int msc_vgl_printf(const char *format, ...)
{
    if (format == NULL)
        return 0;
#ifdef PLATFORM_VITA
    static uint32_t sLoggedCount = 0;
    char buf[512];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (len <= 0)
        return 0;
    sceClibPrintf("[vitaGL] %s", buf);
    if (sLoggedCount < 256)
    {
        FILE *fp = fopen("ux0:data/pokeemerald/vitagl_error.log", sLoggedCount == 0 ? "w" : "a");
        if (fp != NULL)
        {
            fwrite(buf, 1, (size_t)(len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1), fp);
            fclose(fp);
        }
        sLoggedCount++;
    }
    return len;
#else
    (void)format;
    return 0;
#endif
}
