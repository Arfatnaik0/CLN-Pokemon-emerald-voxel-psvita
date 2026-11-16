#ifndef PLATFORM_UI_H
#define PLATFORM_UI_H

#include <stdbool.h>
#include <stdint.h>

/* Compatible stubs for HctrRB/pokeemerald-vita-3ds Pass 6 UI queries (MIT; see LICENSE and NOTICE.md). */
/* Original multiplatform GBA UI is preserved; these queries keep upstream voxel Pass 6 compatible. */
static inline bool Ui_MirrorCovers(void)
{
    return false;
}

static inline uint16_t Ui_BattleDispcntClear(void)
{
    return 0;
}

static inline void Ui_Render(int32_t viewWidth, int32_t viewHeight)
{
    (void)viewWidth;
    (void)viewHeight;
}

#endif // PLATFORM_UI_H
