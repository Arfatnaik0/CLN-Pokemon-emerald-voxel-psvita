#include <stdint.h>
#include "global.h"

// The production GBA memory model uses 32-bit addresses. Keep host pointers
// intact for this differential renderer harness.
#undef VRAM
#define VRAM ((uintptr_t)VRAM_)

#define RENDERER_EASY_DRAW
#define DrawFrame DrawFrameEasy
#define memsetu16 memsetu16Easy
#define frameskipcounter frameskipcounterEasy
#define spriteSizes spriteSizesEasy
#include "../../src/platform/gba_easy_draw.c"
