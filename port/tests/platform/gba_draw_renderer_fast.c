#include <stdint.h>
#include "global.h"

#undef VRAM
#define VRAM ((uintptr_t)VRAM_)

#define RENDERER_FAST_DRAW
#define DrawFrame DrawFrameFast
#define memsetu16 memsetu16Fast
#define frameskipcounter frameskipcounterFast
#define spriteSizes spriteSizesFast
#include "../../src/platform/gba_fast_draw.c"
