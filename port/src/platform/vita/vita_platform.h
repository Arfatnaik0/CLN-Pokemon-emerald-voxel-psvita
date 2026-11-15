#ifndef GUARD_VITA_PLATFORM_H
#define GUARD_VITA_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VITA_DATA_DIR "ux0:data/pokeemerald/"

struct VitaLocalTime
{
    int32_t year;
    int32_t month;
    int32_t day;
    int32_t dayOfWeek;
    int32_t hour;
    int32_t minute;
    int32_t second;
};

int32_t VitaPlatform_GetLocalTime(struct VitaLocalTime *out);
uint64_t VitaPlatform_GetMilliseconds(void);
int64_t VitaPlatform_GetWallSeconds(void);
void VitaPlatform_PinCurrentThread(const char *name, int32_t core);
void VitaPlatform_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef VITA_LOGGING
#define VITA_LOG(...) VitaPlatform_Log(__VA_ARGS__)
#else
#define VITA_LOG(...) ((void)0)
#endif

#endif // GUARD_VITA_PLATFORM_H
