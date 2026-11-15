#ifndef GUARD_FRAMEDRAW_H
#define GUARD_FRAMEDRAW_H

#include "global.h"

#ifdef WIDESCREEN_SUPPORTED
#include "platform/widescreen.h"
#define FRAME_LINE_WIDTH WIDESCREEN_WIDTH
#define FRAME_LINE_X0 WIDESCREEN_MARGIN
#else
#define FRAME_LINE_WIDTH DISPLAY_WIDTH
#define FRAME_LINE_X0 0
#define WIDESCREEN_VISIBLE_U0 0.0f
#define WIDESCREEN_VISIBLE_U1 1.0f
#endif

void DrawFrame(uint16_t *pixels);

/* Pass 6 UI overlay hooks used by VoxelRenderer over classic 2D PPU output. */
extern int32_t gFastDrawBackdropOverride;
extern bool8 gFastDrawScreenObjOnly;
extern u16 gFastDrawDispcntClear;
extern u16 gFastDrawBldcntClear;
extern bool8 gFastDrawCentredOnly;

void RunFrameLineEvents(void);

#endif
