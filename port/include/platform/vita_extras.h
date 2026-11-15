#ifndef VITA_EXTRAS_H
#define VITA_EXTRAS_H

typedef enum
{
    FAST_EXTRA_DAY_NIGHT = 0,
    FAST_EXTRA_CLOCK_SPEED,
    FAST_EXTRA_VOXEL_3D,
    FAST_EXTRA_VOXEL_BATTLE,
    FAST_EXTRA_VOXEL_RES,
    FAST_EXTRA_VOXEL_FPS,
    FAST_EXTRA_VOXEL_BLUR,
    FAST_EXTRA_COUNT
} FastExtraId;

int VitaExtras_GetFast(FastExtraId id);
int VitaExtras_Get(const char *key);
void VitaExtras_Set(const char *key, int value);

#endif // VITA_EXTRAS_H
