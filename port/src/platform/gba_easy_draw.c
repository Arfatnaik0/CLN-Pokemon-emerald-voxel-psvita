#ifdef RENDERER_EASY_DRAW
#include "global.h"
#include <stdbool.h>
#include "platform/dma.h"
#if defined(PLATFORM_VITA) || defined(EASY_DRAW_TEST_PARALLEL)
#include "gba_easy_draw_workers.h"
#endif
#ifdef PLATFORM_VITA
#include "platform/vita_perf.h"
#endif
#include "platform/framedraw.h"
#include <string.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

int32_t gFastDrawBackdropOverride = -1;
bool8 gFastDrawScreenObjOnly = FALSE;
u16 gFastDrawDispcntClear = 0;
u16 gFastDrawBldcntClear = 0;
bool8 gFastDrawCentredOnly = FALSE;
#ifdef __GNUC__
__attribute__((weak))
#endif
u32 gScreenOam[4] = {0};
#ifdef EASY_DRAW_TEST_PARALLEL
static bool sForceFullSpriteCompositor;
static bool sForceLegacyBgCompositor;
static bool sForceLegacyLayerClears;
static bool sDisableSpriteScanlinePrefilter;
static bool sForceLegacySpritePixelPath;
static bool sForceLegacySimpleSpritePath;
static bool sTestLastWindowDmaParallel;
#endif

#define mosaicBGEffectX (REG_MOSAIC & 0xF)
#define mosaicBGEffectY ((REG_MOSAIC >> 4) & 0xF)
#define mosaicSpriteEffectX ((REG_MOSAIC >> 8) & 0xF)
#define mosaicSpriteEffectY ((REG_MOSAIC >> 12) & 0xF)

static inline unsigned int ApplyMosaicFloorU(unsigned int value, unsigned int step)
{
    static const uint32_t sMosaicInv[17] = {
        0u, 65536u, 32768u, 21846u, 16384u, 13108u, 10923u, 9363u,
        8192u, 7282u, 6554u, 5958u, 5462u, 5042u, 4682u, 4370u, 4096u
    };

    if (step <= 1u)
        return value;
    if (__builtin_expect(value < 256u && step <= 16u, 1))
        return (unsigned int)((((uint32_t)value * sMosaicInv[step]) >> 16) * step);

    unsigned int rem = value;
    while (rem >= step)
        rem -= step;
    return value - rem;
}

#define applyBGHorizontalMosaicEffect(x) ApplyMosaicFloorU((unsigned int)(x), (unsigned int)(mosaicBGEffectX + 1u))
#define applyBGVerticalMosaicEffect(y) ApplyMosaicFloorU((unsigned int)(y), (unsigned int)(mosaicBGEffectY + 1u))
#define applySpriteHorizontalMosaicEffect(x) ApplyMosaicFloorU((unsigned int)(x), (unsigned int)(mosaicSpriteEffectX + 1u))
#define applySpriteVerticalMosaicEffect(y) ApplyMosaicFloorU((unsigned int)(y), (unsigned int)(mosaicSpriteEffectY + 1u))

#define getAlphaBit(x) ((x >> 15) & 1)
#define getRedChannel(x) ((x >>  0) & 0x1F)
#define getGreenChannel(x) ((x >>  5) & 0x1F)
#define getBlueChannel(x) ((x >>  10) & 0x1F)
#define isbgEnabled(x) ((REG_DISPCNT >> 8) & 0xF) & (1 << x)

#define WINMASK_BG0    (1 << 0)
#define WINMASK_BG1    (1 << 1)
#define WINMASK_BG2    (1 << 2)
#define WINMASK_BG3    (1 << 3)
#define WINMASK_OBJ    (1 << 4)
#define WINMASK_CLR    (1 << 5)
#define WINMASK_WINOUT  (1 << 6)

extern void (*const gIntrTable[])(void);
#if defined(PLATFORM_VITA) && !defined(EASY_DRAW_TEST_PARALLEL)
typedef void (*IntrCallback)(void);
struct MainEasyDraw {
    void *callback1;
    void *callback2;
    void *savedCallback;
    IntrCallback vblankCallback;
    IntrCallback hblankCallback;
    IntrCallback vcountCallback;
    IntrCallback serialCallback;
};
extern struct MainEasyDraw gMain;
extern u8 BattleTransition_IsPurePpuHBlankCallback(void (*callback)(void));
#endif

struct HBlankRowState {
    uint16_t win0h;
    uint16_t win1h;
    uint16_t win0v;
    uint16_t win1v;
    uint16_t winin;
    uint16_t winout;
    uint16_t bgHofs[4];
    uint16_t bgVofs[4];
    uint16_t bldcnt;
    uint16_t bldalpha;
    uint16_t bldy;
};

static inline void CaptureHBlankRowState(struct HBlankRowState *row)
{
    row->win0h = REG_WIN0H;
    row->win1h = REG_WIN1H;
    row->win0v = REG_WIN0V;
    row->win1v = REG_WIN1V;
    row->winin = REG_WININ;
    row->winout = REG_WINOUT;
    row->bgHofs[0] = REG_BG0HOFS;
    row->bgVofs[0] = REG_BG0VOFS;
    row->bgHofs[1] = REG_BG1HOFS;
    row->bgVofs[1] = REG_BG1VOFS;
    row->bgHofs[2] = REG_BG2HOFS;
    row->bgVofs[2] = REG_BG2VOFS;
    row->bgHofs[3] = REG_BG3HOFS;
    row->bgVofs[3] = REG_BG3VOFS;
    row->bldcnt = REG_BLDCNT;
    row->bldalpha = REG_BLDALPHA;
    row->bldy = REG_BLDY;
}

static inline void RestoreHBlankRowState(const struct HBlankRowState *row)
{
    REG_WIN0H = row->win0h;
    REG_WIN1H = row->win1h;
    REG_WIN0V = row->win0v;
    REG_WIN1V = row->win1v;
    REG_WININ = row->winin;
    REG_WINOUT = row->winout;
    REG_BG0HOFS = row->bgHofs[0];
    REG_BG0VOFS = row->bgVofs[0];
    REG_BG1HOFS = row->bgHofs[1];
    REG_BG1VOFS = row->bgVofs[1];
    REG_BG2HOFS = row->bgHofs[2];
    REG_BG2VOFS = row->bgVofs[2];
    REG_BG3HOFS = row->bgHofs[3];
    REG_BG3VOFS = row->bgVofs[3];
    REG_BLDCNT = row->bldcnt;
    REG_BLDALPHA = row->bldalpha;
    REG_BLDY = row->bldy;
}

struct scanlineData {
    uint16_t layers[4][DISPLAY_WIDTH];
    uint16_t spriteLayers[4][DISPLAY_WIDTH];
    uint16_t winMask[DISPLAY_WIDTH];
    uint16_t spriteMinX[4];
    uint16_t spriteMaxX[4];
    uint16_t bgcnts[4];
    char prioritySortedBgsCount[4];
    char prioritySortedBgs[4][4];
    char bgtoprio[4]; //background to priority
    uint8_t spritePriorityMask;
} __attribute__((aligned(4)));

struct bgPriority {
    char priority;
    char subPriority;
};

static const uint16_t bgMapSizes[][2] =
{
    {32, 32},
    {64, 32},
    {32, 64},
    {64, 64},
};

static inline uint32_t ReadU32(const void *ptr)
{
    uint32_t value;
    __builtin_memcpy(&value, ptr, sizeof(value));
    return value;
}

static inline void WriteU32(void *ptr, uint32_t value)
{
    __builtin_memcpy(ptr, &value, sizeof(value));
}

static inline void ZeroU32(void *dst, size_t byteCount)
{
    uint8_t *ptr = (uint8_t *)dst;
    size_t i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    uint32x4_t zero = vdupq_n_u32(0);
    for (; i + 32 <= byteCount; i += 32)
    {
        vst1q_u32((uint32_t *)(ptr + i), zero);
        vst1q_u32((uint32_t *)(ptr + i + 16), zero);
    }
    for (; i + 16 <= byteCount; i += 16)
        vst1q_u32((uint32_t *)(ptr + i), zero);
#endif
    uint32_t *words = (uint32_t *)(ptr + i);
    size_t remWords = (byteCount - i) >> 2;
    for (size_t w = 0; w < remWords; w++)
        words[w] = 0;
}

static inline void FillU16(uint16_t *dst, uint16_t fill, size_t count)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (count >= 16)
    {
        uint16x8_t v = vdupq_n_u16(fill);
        size_t i = 0;
        for (; i + 16 <= count; i += 16)
        {
            vst1q_u16(dst + i, v);
            vst1q_u16(dst + i + 8, v);
        }
        for (; i + 8 <= count; i += 8)
            vst1q_u16(dst + i, v);
        dst += i;
        count -= i;
    }
#endif
    if (((uintptr_t)dst & 2) && count > 0)
    {
        *dst++ = fill;
        count--;
    }
    uint32_t fill32 = ((uint32_t)fill << 16) | (uint32_t)fill;
    uint32_t *dst32 = (uint32_t *)dst;
    size_t words = count >> 1;
    for (size_t i = 0; i < words; i++)
        dst32[i] = fill32;
    if (count & 1)
        dst[words * 2] = fill;
}

static inline void Decode4BppTileWordsOpaque(const uint32_t row32, const uint16_t *paletteBase,
                                             bool hflip, uint32_t *w0, uint32_t *w1,
                                             uint32_t *w2, uint32_t *w3)
{
    uint32_t p0, p1, p2, p3, p4, p5, p6, p7;
    if (!hflip)
    {
        p0 = paletteBase[row32 & 0xFu];
        p1 = paletteBase[(row32 >> 4) & 0xFu];
        p2 = paletteBase[(row32 >> 8) & 0xFu];
        p3 = paletteBase[(row32 >> 12) & 0xFu];
        p4 = paletteBase[(row32 >> 16) & 0xFu];
        p5 = paletteBase[(row32 >> 20) & 0xFu];
        p6 = paletteBase[(row32 >> 24) & 0xFu];
        p7 = paletteBase[row32 >> 28];
    }
    else
    {
        p7 = paletteBase[row32 & 0xFu];
        p6 = paletteBase[(row32 >> 4) & 0xFu];
        p5 = paletteBase[(row32 >> 8) & 0xFu];
        p4 = paletteBase[(row32 >> 12) & 0xFu];
        p3 = paletteBase[(row32 >> 16) & 0xFu];
        p2 = paletteBase[(row32 >> 20) & 0xFu];
        p1 = paletteBase[(row32 >> 24) & 0xFu];
        p0 = paletteBase[row32 >> 28];
    }
    *w0 = (p0 | (p1 << 16)) | 0x80008000u;
    *w1 = (p2 | (p3 << 16)) | 0x80008000u;
    *w2 = (p4 | (p5 << 16)) | 0x80008000u;
    *w3 = (p6 | (p7 << 16)) | 0x80008000u;
}

static void Decode4BppTileSpanSparse(const uint32_t row32, const uint16_t *paletteBase,
                                     bool hflip, uint16_t *dst)
{
    if (!hflip)
    {
        uint32_t lo = row32 & 0xFFFFu;
        if (lo != 0)
        {
            uint32_t p;
            if ((p = (lo & 0xFu)))         dst[0] = paletteBase[p] | 0x8000;
            if ((p = ((lo >> 4) & 0xFu)))  dst[1] = paletteBase[p] | 0x8000;
            if ((p = ((lo >> 8) & 0xFu)))  dst[2] = paletteBase[p] | 0x8000;
            if ((p = (lo >> 12)))          dst[3] = paletteBase[p] | 0x8000;
        }
        uint32_t hi = row32 >> 16;
        if (hi != 0)
        {
            uint32_t p;
            if ((p = (hi & 0xFu)))         dst[4] = paletteBase[p] | 0x8000;
            if ((p = ((hi >> 4) & 0xFu)))  dst[5] = paletteBase[p] | 0x8000;
            if ((p = ((hi >> 8) & 0xFu)))  dst[6] = paletteBase[p] | 0x8000;
            if ((p = (hi >> 12)))          dst[7] = paletteBase[p] | 0x8000;
        }
    }
    else
    {
        uint32_t lo = row32 & 0xFFFFu;
        if (lo != 0)
        {
            uint32_t p;
            if ((p = (lo & 0xFu)))         dst[7] = paletteBase[p] | 0x8000;
            if ((p = ((lo >> 4) & 0xFu)))  dst[6] = paletteBase[p] | 0x8000;
            if ((p = ((lo >> 8) & 0xFu)))  dst[5] = paletteBase[p] | 0x8000;
            if ((p = (lo >> 12)))          dst[4] = paletteBase[p] | 0x8000;
        }
        uint32_t hi = row32 >> 16;
        if (hi != 0)
        {
            uint32_t p;
            if ((p = (hi & 0xFu)))         dst[3] = paletteBase[p] | 0x8000;
            if ((p = ((hi >> 4) & 0xFu)))  dst[2] = paletteBase[p] | 0x8000;
            if ((p = ((hi >> 8) & 0xFu)))  dst[1] = paletteBase[p] | 0x8000;
            if ((p = (hi >> 12)))          dst[0] = paletteBase[p] | 0x8000;
        }
    }
}

static inline void Decode4BppTileSpanPartial(const uint32_t row32, const uint16_t *paletteBase,
                                             bool hflip, unsigned int tileX,
                                             unsigned int span, uint16_t *dst)
{
    if (!hflip)
    {
        uint32_t pixels32 = row32 >> (tileX * 4);
        for (unsigned int offset = 0; offset < span; offset++)
        {
            uint32_t pixel = pixels32 & 0xFu;
            if (pixel != 0)
                dst[offset] = paletteBase[pixel] | 0x8000;
            pixels32 >>= 4;
        }
    }
    else
    {
        uint32_t pixels32 = row32 << (tileX * 4);
        for (unsigned int offset = 0; offset < span; offset++)
        {
            uint32_t pixel = pixels32 >> 28;
            if (pixel != 0)
                dst[offset] = paletteBase[pixel] | 0x8000;
            pixels32 <<= 4;
        }
    }
}

static inline void Decode8BppTileSpanFull(const uint32_t lo, const uint32_t hi,
                                          const uint16_t *pal, bool hflip, uint16_t *dst)
{
    if (!hflip)
    {
        if (lo != 0)
        {
            if ((((lo - 0x01010101u) & ~lo) & 0x80808080u) == 0u)
            {
                uint32_t c0 = pal[(uint8_t)lo] | 0x8000u;
                uint32_t c1 = pal[(uint8_t)(lo >> 8)] | 0x8000u;
                uint32_t c2 = pal[(uint8_t)(lo >> 16)] | 0x8000u;
                uint32_t c3 = pal[(uint8_t)(lo >> 24)] | 0x8000u;
                WriteU32(&dst[0], c0 | (c1 << 16));
                WriteU32(&dst[2], c2 | (c3 << 16));
            }
            else
            {
                uint8_t p;
                if ((p = (uint8_t)lo))         dst[0] = pal[p] | 0x8000;
                if ((p = (uint8_t)(lo >> 8)))  dst[1] = pal[p] | 0x8000;
                if ((p = (uint8_t)(lo >> 16))) dst[2] = pal[p] | 0x8000;
                if ((p = (uint8_t)(lo >> 24))) dst[3] = pal[p] | 0x8000;
            }
        }
        if (hi != 0)
        {
            if ((((hi - 0x01010101u) & ~hi) & 0x80808080u) == 0u)
            {
                uint32_t c0 = pal[(uint8_t)hi] | 0x8000u;
                uint32_t c1 = pal[(uint8_t)(hi >> 8)] | 0x8000u;
                uint32_t c2 = pal[(uint8_t)(hi >> 16)] | 0x8000u;
                uint32_t c3 = pal[(uint8_t)(hi >> 24)] | 0x8000u;
                WriteU32(&dst[4], c0 | (c1 << 16));
                WriteU32(&dst[6], c2 | (c3 << 16));
            }
            else
            {
                uint8_t p;
                if ((p = (uint8_t)hi))         dst[4] = pal[p] | 0x8000;
                if ((p = (uint8_t)(hi >> 8)))  dst[5] = pal[p] | 0x8000;
                if ((p = (uint8_t)(hi >> 16))) dst[6] = pal[p] | 0x8000;
                if ((p = (uint8_t)(hi >> 24))) dst[7] = pal[p] | 0x8000;
            }
        }
    }
    else
    {
        if (lo != 0)
        {
            if ((((lo - 0x01010101u) & ~lo) & 0x80808080u) == 0u)
            {
                uint32_t c0 = pal[(uint8_t)lo] | 0x8000u;
                uint32_t c1 = pal[(uint8_t)(lo >> 8)] | 0x8000u;
                uint32_t c2 = pal[(uint8_t)(lo >> 16)] | 0x8000u;
                uint32_t c3 = pal[(uint8_t)(lo >> 24)] | 0x8000u;
                WriteU32(&dst[4], c3 | (c2 << 16));
                WriteU32(&dst[6], c1 | (c0 << 16));
            }
            else
            {
                uint8_t p;
                if ((p = (uint8_t)lo))         dst[7] = pal[p] | 0x8000;
                if ((p = (uint8_t)(lo >> 8)))  dst[6] = pal[p] | 0x8000;
                if ((p = (uint8_t)(lo >> 16))) dst[5] = pal[p] | 0x8000;
                if ((p = (uint8_t)(lo >> 24))) dst[4] = pal[p] | 0x8000;
            }
        }
        if (hi != 0)
        {
            if ((((hi - 0x01010101u) & ~hi) & 0x80808080u) == 0u)
            {
                uint32_t c0 = pal[(uint8_t)hi] | 0x8000u;
                uint32_t c1 = pal[(uint8_t)(hi >> 8)] | 0x8000u;
                uint32_t c2 = pal[(uint8_t)(hi >> 16)] | 0x8000u;
                uint32_t c3 = pal[(uint8_t)(hi >> 24)] | 0x8000u;
                WriteU32(&dst[0], c3 | (c2 << 16));
                WriteU32(&dst[2], c1 | (c0 << 16));
            }
            else
            {
                uint8_t p;
                if ((p = (uint8_t)hi))         dst[3] = pal[p] | 0x8000;
                if ((p = (uint8_t)(hi >> 8)))  dst[2] = pal[p] | 0x8000;
                if ((p = (uint8_t)(hi >> 16))) dst[1] = pal[p] | 0x8000;
                if ((p = (uint8_t)(hi >> 24))) dst[0] = pal[p] | 0x8000;
            }
        }
    }
}

static inline bool TileRow4BppAllOpaque(uint32_t row32)
{
    return ((((row32 & 0x77777777u) + 0x77777777u) | row32) & 0x88888888u) == 0x88888888u;
}

static inline void DrawCached4BppTile8(uint16_t entry, unsigned int tileY,
                                       const uint8_t *bgtiles, const uint16_t *pal,
                                       int *prevEntry, uint32_t *prevRow32,
                                       const uint16_t **prevPal, bool *prevHflip,
                                       bool *prevOpaqueCached, uint32_t *prevW0,
                                       uint32_t *prevW1, uint32_t *prevW2,
                                       uint32_t *prevW3, uint16_t *dst)
{
    if ((int)entry != *prevEntry)
    {
        unsigned int row = (entry & (1u << 11)) ? (7u - tileY) : tileY;
        *prevEntry = (int)entry;
        *prevRow32 = ReadU32(&bgtiles[(entry & 0x3FFu) * 32u + row * 4u]);
        *prevPal = &pal[16u * ((entry >> 12) & 0xFu)];
        *prevHflip = (entry & (1u << 10)) != 0;
        *prevOpaqueCached = false;
    }

    if (*prevRow32 == 0)
        return;

    if (*prevOpaqueCached)
    {
        WriteU32(&dst[0], *prevW0);
        WriteU32(&dst[2], *prevW1);
        WriteU32(&dst[4], *prevW2);
        WriteU32(&dst[6], *prevW3);
        return;
    }

    if (TileRow4BppAllOpaque(*prevRow32))
    {
        Decode4BppTileWordsOpaque(*prevRow32, *prevPal, *prevHflip,
                                  prevW0, prevW1, prevW2, prevW3);
        WriteU32(&dst[0], *prevW0);
        WriteU32(&dst[2], *prevW1);
        WriteU32(&dst[4], *prevW2);
        WriteU32(&dst[6], *prevW3);
        *prevOpaqueCached = true;
    }
    else
    {
        Decode4BppTileSpanSparse(*prevRow32, *prevPal, *prevHflip, dst);
    }
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-tree-slp-vectorize")))
#endif
static void RenderBGScanline4Bpp(const uint8_t *bgtiles, const uint16_t *pal,
                                 const uint16_t *bgRow0, const uint16_t *bgRow1,
                                 bool wideMap, unsigned int hoffs,
                                 unsigned int tileY, uint16_t *line)
{
    unsigned int x = 0;
    unsigned int initTileX = hoffs & 7u;

    if (initTileX != 0)
    {
        unsigned int span = 8u - initTileX;
        const uint16_t *mapRow = (wideMap && hoffs > 255u) ? bgRow1 : bgRow0;
        uint16_t entry = mapRow[(hoffs & 0xFFu) >> 3];
        unsigned int row = (entry & (1u << 11)) ? (7u - tileY) : tileY;
        uint32_t row32 = ReadU32(&bgtiles[(entry & 0x3FFu) * 32u + row * 4u]);
        if (row32 != 0)
            Decode4BppTileSpanPartial(row32, &pal[16u * ((entry >> 12) & 0xFu)],
                                      (entry & (1u << 10)) != 0, initTileX, span, line);
        x = span;
    }

    int prevEntry = -1;
    uint32_t prevRow32 = 0;
    const uint16_t *prevPal = pal;
    bool prevHflip = false;
    bool prevOpaqueCached = false;
    uint32_t prevW0 = 0, prevW1 = 0, prevW2 = 0, prevW3 = 0;
    unsigned int mapTile = (x + hoffs) >> 3;
    const uint16_t *rowAlt = wideMap ? bgRow1 : bgRow0;

    for (; x + 8u <= DISPLAY_WIDTH; x += 8u, mapTile++)
    {
        const uint16_t *mapRow = (mapTile & 32u) ? rowAlt : bgRow0;
        DrawCached4BppTile8(mapRow[mapTile & 31u], tileY, bgtiles, pal,
                            &prevEntry, &prevRow32, &prevPal, &prevHflip,
                            &prevOpaqueCached, &prevW0, &prevW1, &prevW2, &prevW3,
                            &line[x]);
    }

    if (x < DISPLAY_WIDTH)
    {
        unsigned int span = DISPLAY_WIDTH - x;
        const uint16_t *mapRow = (wideMap && (mapTile & 32u)) ? bgRow1 : bgRow0;
        uint16_t entry = mapRow[mapTile & 31u];
        unsigned int row = (entry & (1u << 11)) ? (7u - tileY) : tileY;
        uint32_t row32 = ReadU32(&bgtiles[(entry & 0x3FFu) * 32u + row * 4u]);
        if (row32 != 0)
            Decode4BppTileSpanPartial(row32, &pal[16u * ((entry >> 12) & 0xFu)],
                                      (entry & (1u << 10)) != 0, 0, span, &line[x]);
    }
}

static __attribute__((noinline)) void RenderBGScanline8Bpp(const uint8_t *bgtiles, const uint16_t *pal,
                                                          const uint16_t *bgRow0, const uint16_t *bgRow1,
                                                          bool wideMap, unsigned int hoffs,
                                                          unsigned int tileY, uint16_t *line)
{
    for (unsigned int x = 0; x < DISPLAY_WIDTH;)
    {
        unsigned int xx = (x + hoffs) & 0x1FFu;
        unsigned int tileX = xx & 7u;
        unsigned int span = 8u - tileX;
        unsigned int remaining = DISPLAY_WIDTH - x;
        if (span > remaining)
            span = remaining;

        const uint16_t *mapRow = (wideMap && xx > 255u) ? bgRow1 : bgRow0;
        uint16_t entry = mapRow[(xx & 0xFFu) >> 3];
        unsigned int tileNum = entry & 0x3FFu;
        unsigned int row = (entry & (1u << 11)) ? (7u - tileY) : tileY;
        const uint8_t *tileRow = &bgtiles[tileNum * 64u + row * 8u];
        uint32_t lo = ReadU32(tileRow);
        uint32_t hi = ReadU32(tileRow + 4);
        if ((lo | hi) != 0)
        {
            if (span == 8u)
            {
                Decode8BppTileSpanFull(lo, hi, pal, (entry & (1u << 10)) != 0, &line[x]);
            }
            else
            {
                uint16_t *dst = &line[x];
                for (unsigned int offset = 0; offset < span; offset++)
                {
                    unsigned int sourceX = (entry & (1u << 10))
                        ? 7u - (tileX + offset) : tileX + offset;
                    uint8_t pixel = tileRow[sourceX];
                    if (pixel != 0)
                        dst[offset] = pal[pixel] | 0x8000;
                }
            }
        }
        x += span;
    }
}

static __attribute__((noinline)) void RenderBGScanlineMosaic(const uint8_t *bgtiles, const uint16_t *pal,
                                                            const uint16_t *bgRow0, const uint16_t *bgRow1,
                                                            unsigned int mapWidthInPixels, unsigned int bitsPerPixel,
                                                            unsigned int hoffs, unsigned int tileY, uint16_t *line)
{
    int cachedMapEntry = -1;
    const uint8_t *tileRow = NULL;
    const uint16_t *paletteBase = NULL;
    bool flipX = false;

    unsigned int mosaicStepX = (unsigned int)mosaicBGEffectX + 1u;
    unsigned int mosaicAnchorX = 0u;
    unsigned int nextMosaicAnchorX = mosaicStepX;

    for (unsigned int x = 0; x < DISPLAY_WIDTH; x++)
    {
        if (x == nextMosaicAnchorX)
        {
            mosaicAnchorX = x;
            nextMosaicAnchorX += mosaicStepX;
        }
        unsigned int xx = (mosaicAnchorX + hoffs) & 0x1FFu;
        unsigned int mapBlock = (xx > 255u && mapWidthInPixels > 256u) ? 1u : 0u;
        xx &= 0xFFu;

        unsigned int mapX = xx >> 3;
        int mapEntryIndex = (int)((mapBlock << 5) | mapX);
        if (mapEntryIndex != cachedMapEntry)
        {
            const uint16_t *mapRow = mapBlock ? bgRow1 : bgRow0;
            uint16_t entry = mapRow[mapX];
            unsigned int tileNum = entry & 0x3FFu;
            unsigned int paletteNum = (entry >> 12) & 0xFu;
            unsigned int row = (entry & (1u << 11)) ? 7u - tileY : tileY;
            unsigned int tileLoc = tileNum * (bitsPerPixel * 8u);

            tileRow = &bgtiles[tileLoc + row * bitsPerPixel];
            paletteBase = (bitsPerPixel == 4u) ? &pal[16u * paletteNum] : pal;
            flipX = (entry & (1u << 10)) != 0;
            cachedMapEntry = mapEntryIndex;
        }

        unsigned int tileX = xx & 7u;
        if (flipX)
            tileX = 7u - tileX;

        if (bitsPerPixel == 4u)
        {
            uint8_t pixel = tileRow[tileX >> 1];
            if (tileX & 1u)
                pixel >>= 4;
            else
                pixel &= 0xFu;

            if (pixel != 0)
                line[x] = paletteBase[pixel] | 0x8000;
        }
        else
        {
            uint8_t pixel = tileRow[tileX];
            if (pixel != 0)
                line[x] = paletteBase[pixel] | 0x8000;
        }
    }
}

static void RenderBGScanline(int bgNum, uint16_t control, uint16_t hoffs, uint16_t voffs, int lineNum, uint16_t *line)
{
    unsigned int charBaseBlock = (control >> 2) & 3u;
    unsigned int screenBaseBlock = (control >> 8) & 0x1Fu;
    unsigned int bitsPerPixel = ((control >> 7) & 1u) ? 8u : 4u;
    unsigned int mapWidth = bgMapSizes[control >> 14][0];
    unsigned int mapHeight = bgMapSizes[control >> 14][1];
    unsigned int mapWidthInPixels = mapWidth * 8u;
    unsigned int mapHeightInPixels = mapHeight * 8u;

    const uint8_t *bgtiles = (const uint8_t *)BG_CHAR_ADDR(charBaseBlock);
    const uint16_t *pal = (const uint16_t *)PLTT;

    if ((control & BGCNT_MOSAIC) && mosaicBGEffectY != 0u)
        lineNum = (int)applyBGVerticalMosaicEffect(lineNum);

    hoffs &= 0x1FFu;
    voffs &= 0x1FFu;

    unsigned int yy = (lineNum + voffs) & 0x1FFu;
    const uint16_t *bgmap = (const uint16_t *)BG_SCREEN_ADDR(screenBaseBlock);

    if (yy > 255u && mapHeightInPixels > 256u)
        bgmap += (mapWidthInPixels > 256u) ? 0x800 : 0x400;

    yy &= 0xFFu;
    unsigned int mapY = yy >> 3;
    unsigned int tileY = yy & 7u;

    const uint16_t *bgRow0 = &bgmap[mapY * 32u];
    const uint16_t *bgRow1 = (mapWidthInPixels > 256u) ? (bgRow0 + 0x400) : bgRow0;

    if (!(control & BGCNT_MOSAIC) || mosaicBGEffectX == 0u)
    {
        if (bitsPerPixel == 4u)
            RenderBGScanline4Bpp(bgtiles, pal, bgRow0, bgRow1, mapWidthInPixels > 256u, hoffs, tileY, line);
        else
            RenderBGScanline8Bpp(bgtiles, pal, bgRow0, bgRow1, mapWidthInPixels > 256u, hoffs, tileY, line);
        return;
    }

    RenderBGScanlineMosaic(bgtiles, pal, bgRow0, bgRow1, mapWidthInPixels, bitsPerPixel, hoffs, tileY, line);
}

static inline uint32_t getBgX(int bgNumber)
{
    if (bgNumber == 2)
    {
        return REG_BG2X;
    }
    else if (bgNumber == 3)
    {
        return REG_BG3X;
    }
}

static inline uint32_t getBgY(int bgNumber)
{
    if (bgNumber == 2)
    {
        return REG_BG2Y;
    }
    else if (bgNumber == 3)
    {
        return REG_BG3Y;
    }
}

static inline uint16_t getBgPA(int bgNumber)
{
    if (bgNumber == 2)
    {
        return REG_BG2PA;
    }
    else if (bgNumber == 3)
    {
        return REG_BG3PA;
    }
}

static inline uint16_t getBgPB(int bgNumber)
{
    if (bgNumber == 2)
    {
        return REG_BG2PB;
    }
    else if (bgNumber == 3)
    {
        return REG_BG3PB;
    }
}

static inline uint16_t getBgPC(int bgNumber)
{
    if (bgNumber == 2)
    {
        return REG_BG2PC;
    }
    else if (bgNumber == 3)
    {
        return REG_BG3PC;
    }
}

static inline uint16_t getBgPD(int bgNumber)
{
    if (bgNumber == 2)
    {
        return REG_BG2PD;
    }
    else if (bgNumber == 3)
    {
        return REG_BG3PD;
    }
}

static void RenderRotScaleBGScanline(int bgNum, uint16_t control, uint16_t x, uint16_t y, int lineNum, uint16_t *line)
{
    vBgCnt *bgcnt = (vBgCnt *)&control;
    unsigned int charBaseBlock = bgcnt->charBaseBlock;
    unsigned int screenBaseBlock = bgcnt->screenBaseBlock;
    unsigned int mapWidth = 1 << (4 + (bgcnt->screenSize)); // number of tiles

    uint8_t *bgtiles = (uint8_t *)(VRAM_ + charBaseBlock * 0x4000);
    uint8_t *bgmap = (uint8_t *)(VRAM_ + screenBaseBlock * 0x800);
    uint16_t *pal = (uint16_t *)PLTT;

    if ((control & BGCNT_MOSAIC) && mosaicBGEffectY != 0u)
        lineNum = (int)applyBGVerticalMosaicEffect(lineNum);
    

    s16 pa = getBgPA(bgNum);
    s16 pb = getBgPB(bgNum);
    s16 pc = getBgPC(bgNum);
    s16 pd = getBgPD(bgNum);

    int sizeX = 128;
    int sizeY = 128;

    switch (bgcnt->screenSize)
    {
    case 0:
        break;
    case 1:
        sizeX = sizeY = 256;
        break;
    case 2:
        sizeX = sizeY = 512;
        break;
    case 3:
        sizeX = sizeY = 1024;
        break;
    }

    int maskX = sizeX - 1;
    int maskY = sizeY - 1;

    int yshift = ((control >> 14) & 3) + 4;

    /*int dx = pa & 0x7FFF;
    if (pa & 0x8000)
        dx |= 0xFFFF8000;
    int dmx = pb & 0x7FFF;
    if (pb & 0x8000)
        dmx |= 0xFFFF8000;
    int dy = pc & 0x7FFF;
    if (pc & 0x8000)
        dy |= 0xFFFF8000;
    int dmy = pd & 0x7FFF;
    if (pd & 0x8000)
        dmy |= 0xFFFF8000;*/

    s32 currentX = getBgX(bgNum);
    s32 currentY = getBgY(bgNum);
    //sign extend 28 bit number
    currentX = ((currentX & (1 << 27)) ? currentX | 0xF0000000 : currentX);
    currentY = ((currentY & (1 << 27)) ? currentY | 0xF0000000 : currentY);

    currentX += lineNum * pb;
    currentY += lineNum * pd;

    int realX = currentX;
    int realY = currentY;

    if (pb == 0 && pc == 0 && pa == 0x100 && (!(control & BGCNT_MOSAIC) || mosaicBGEffectX == 0u))
    {
        int startX = realX >> 8;
        int yyy = realY >> 8;
        bool areaOverflow = (control & BGCNT_WRAP) != 0;
        if (areaOverflow)
        {
            yyy &= maskY;
        }
        else if (yyy < 0 || yyy >= sizeY)
        {
            return;
        }

        const uint8_t *bgRow = &bgmap[(yyy >> 3) << yshift];
        int tileY = yyy & 7;
        int xStart = 0;
        int xEnd = DISPLAY_WIDTH;
        if (!areaOverflow)
        {
            if (startX < 0)
                xStart = -startX;
            if (startX + DISPLAY_WIDTH > sizeX)
                xEnd = sizeX - startX;
            if (xStart >= xEnd)
                return;
        }

        for (int x = xStart; x < xEnd;)
        {
            int xxx = areaOverflow ? ((startX + x) & maskX) : (startX + x);
            int tileX = xxx & 7;
            int span = 8 - tileX;
            if (span > xEnd - x)
                span = xEnd - x;

            int tile = bgRow[xxx >> 3];
            const uint8_t *tileRow = &bgtiles[(tile << 6) + (tileY << 3)];
            uint32_t lo = ReadU32(tileRow);
            uint32_t hi = ReadU32(tileRow + 4);
            if ((lo | hi) != 0)
            {
                if (span == 8)
                {
                    Decode8BppTileSpanFull(lo, hi, pal, false, &line[x]);
                }
                else
                {
                    for (int off = 0; off < span; off++)
                    {
                        uint8_t pixel = tileRow[tileX + off];
                        if (pixel != 0)
                            line[x + off] = pal[pixel] | 0x8000;
                    }
                }
            }
            x += span;
        }
        return;
    }

    if (bgcnt->areaOverflowMode)
    {
        for (int x = 0; x < DISPLAY_WIDTH; x++)
        {
            int xxx = (realX >> 8) & maskX;
            int yyy = (realY >> 8) & maskY;

            int tile = bgmap[(xxx >> 3) + ((yyy >> 3) << yshift)];

            int tileX = xxx & 7;
            int tileY = yyy & 7;

            uint8_t pixel = bgtiles[(tile << 6) + (tileY << 3) + tileX];

            if (pixel != 0) {
                line[x] = pal[pixel] | 0x8000;
            }

            realX += pa;
            realY += pc;
        }
    }
    else
    {
        for (int x = 0; x < DISPLAY_WIDTH; x++)
        {
            int xxx = (realX >> 8);
            int yyy = (realY >> 8);

            if (xxx < 0 || yyy < 0 || xxx >= sizeX || yyy >= sizeY)
            {
                //line[x] = 0x80000000;
            }
            else
            {
                int tile = bgmap[(xxx >> 3) + ((yyy >> 3) << yshift)];

                int tileX = xxx & 7;
                int tileY = yyy & 7;

                uint8_t pixel = bgtiles[(tile << 6) + (tileY << 3) + tileX];

                if (pixel != 0) {
                    line[x] = pal[pixel] | 0x8000;
                }
            }
            realX += pa;
            realY += pc;
        }
    }
    //the only way i could figure out how to get accurate mosaic on affine bgs 
    //luckily i dont think pokemon emerald uses mosaic on affine bgs
    if ((control & BGCNT_MOSAIC) && mosaicBGEffectX > 0)
    {
        unsigned int step = (unsigned int)mosaicBGEffectX + 1u;
        unsigned int nextAnchor = step;
        uint16_t holdColor = line[0];

        for (unsigned int x = 1; x < DISPLAY_WIDTH; x++)
        {
            if (x == nextAnchor)
            {
                holdColor = line[x];
                nextAnchor += step;
            }
            else
            {
                line[x] = holdColor;
            }
        }
    }
}

const u8 spriteSizes[][2] =
{
    {8, 16},
    {8, 32},
    {16, 32},
    {32, 64},
};

struct SpriteScanlineBins {
    uint8_t count[DISPLAY_HEIGHT];
    uint8_t indices[DISPLAY_HEIGHT][128];
    uint8_t priorityMask[DISPLAY_HEIGHT];
    uint8_t hasDirectUnsupported[DISPLAY_HEIGHT];
    uint32_t activeCount;
};
static struct SpriteScanlineBins sSpriteBins;
static bool sSpriteBinsValid;
static bool sSpritePriorityMaskSafe;

static bool HasHBlankScanlineEvents(void);
static bool IsEarlyVCountSafeForDeferredSync(uint16_t dispstat);
#if defined(PLATFORM_VITA) || defined(EASY_DRAW_TEST_PARALLEL)
static bool HasSafeHBlankReplayEvents(void);
#endif

static bool CanUseSpritePriorityMask(void)
{
    if (!IsEarlyVCountSafeForDeferredSync(REG_DISPSTAT))
        return false;

    if (HasHBlankScanlineEvents())
    {
#if defined(PLATFORM_VITA) || defined(EASY_DRAW_TEST_PARALLEL)
        return HasSafeHBlankReplayEvents();
#else
        return false;
#endif
    }

    return true;
}

static void BuildSpriteScanlineBins(struct SpriteScanlineBins *bins)
{
    ZeroU32(bins->count, sizeof(bins->count));
    ZeroU32(bins->priorityMask, sizeof(bins->priorityMask));
    ZeroU32(bins->hasDirectUnsupported, sizeof(bins->hasDirectUnsupported));
    bins->activeCount = 0;

    for (int i = 127; i >= 0; i--)
    {
        if (gFastDrawScreenObjOnly && ((gScreenOam[i >> 5] & (1u << (i & 31))) == 0))
            continue;
        struct OamData *oam = &((struct OamData *)OAM)[i];
        bool isAffine = oam->affineMode & 1;
        bool doubleSizeOrDisabled = (oam->affineMode >> 1) & 1;

        if (!isAffine && doubleSizeOrDisabled)
            continue;

        unsigned int height;
        if (oam->shape == 0)
        {
            height = (1 << oam->size) * 8;
        }
        else if (oam->shape == 1) // wide
        {
            height = spriteSizes[oam->size][0];
        }
        else if (oam->shape == 2) // tall
        {
            height = spriteSizes[oam->size][1];
        }
        else
        {
            continue; // prohibited
        }

        int half_height = height / 2;
        int32_t y = oam->y;
        if (y >= DISPLAY_HEIGHT)
            y -= 256;

        if (isAffine && doubleSizeOrDisabled)
            half_height *= 2;

        int startY = y;
        int endY = y + half_height * 2;
        if (startY < 0)
            startY = 0;
        if (endY > DISPLAY_HEIGHT)
            endY = DISPLAY_HEIGHT;

        if (startY < endY)
        {
            uint8_t unsupported = (oam->objMode != 0 || isAffine || (oam->mosaic != 0 && (REG_MOSAIC & 0xFF00u) != 0u)) ? 1u : 0u;
            bins->activeCount++;
            for (int line = startY; line < endY; line++)
            {
                uint8_t c = bins->count[line];
                bins->indices[line][c] = (uint8_t)i;
                bins->count[line] = c + 1;
                bins->priorityMask[line] |= 1u << oam->priority;
                bins->hasDirectUnsupported[line] |= unsupported;
            }
        }
    }
}

static inline uint16_t alphaBlendColorWithCoeffs(uint16_t targetA, uint16_t targetB, unsigned int eva, unsigned int evb)
{
    unsigned int r = ((getRedChannel(targetA) * eva) + (getRedChannel(targetB) * evb)) >> 4;
    unsigned int g = ((getGreenChannel(targetA) * eva) + (getGreenChannel(targetB) * evb)) >> 4;
    unsigned int b = ((getBlueChannel(targetA) * eva) + (getBlueChannel(targetB) * evb)) >> 4;
    
    if (r > 31)
        r = 31;
    if (g > 31)
        g = 31;
    if (b > 31)
        b = 31;

     return r | (g << 5) | (b << 10) | (1 << 15);
}

static inline uint16_t alphaBrightnessIncreaseWithY(uint16_t targetA, unsigned int evy)
{
    unsigned int r = getRedChannel(targetA) + (((31 - getRedChannel(targetA)) * evy) >> 4);
    unsigned int g = getGreenChannel(targetA) + (((31 - getGreenChannel(targetA)) * evy) >> 4);
    unsigned int b = getBlueChannel(targetA) + (((31 - getBlueChannel(targetA)) * evy) >> 4);
    
    if (r > 31)
        r = 31;
    if (g > 31)
        g = 31;
    if (b > 31)
        b = 31;
    
     return r | (g << 5) | (b << 10) | (1 << 15);
}

static inline uint16_t alphaBrightnessDecreaseWithY(uint16_t targetA, unsigned int evy)
{
    unsigned int r = getRedChannel(targetA) - ((getRedChannel(targetA) * evy) >> 4);
    unsigned int g = getGreenChannel(targetA) - ((getGreenChannel(targetA) * evy) >> 4);
    unsigned int b = getBlueChannel(targetA) - ((getBlueChannel(targetA) * evy) >> 4);
    
    if (r > 31)
        r = 31;
    if (g > 31)
        g = 31;
    if (b > 31)
        b = 31;
    
     return r | (g << 5) | (b << 10) | (1 << 15);
}

static uint16_t alphaBlendColor(uint16_t targetA, uint16_t targetB)
{
    unsigned int eva = REG_BLDALPHA & 0x1F;
    unsigned int evb = (REG_BLDALPHA >> 8) & 0x1F;
    return alphaBlendColorWithCoeffs(targetA, targetB, eva, evb);
}

static uint16_t alphaBrightnessIncrease(uint16_t targetA)
{
    unsigned int evy = (REG_BLDY & 0x1F);
    return alphaBrightnessIncreaseWithY(targetA, evy);
}

static uint16_t alphaBrightnessDecrease(uint16_t targetA)
{
    unsigned int evy = (REG_BLDY & 0x1F);
    return alphaBrightnessDecreaseWithY(targetA, evy);
}

//outputs the blended pixel in colorOutput, the prxxx are the bg priority and subpriority, pixelpos is pixel offset in scanline
static inline bool alphaBlendSelectTargetB(const struct scanlineData* scanline, uint16_t* colorOutput,
                                           int prnum, int prsub, int pixelpos, bool spriteBlendEnabled,
                                           unsigned int enabledBgs, bool objEnabled, unsigned int bldcnt,
                                           uint16_t backdropColor)
{   
    //iterate trough every possible bg to blend with, starting from specified priorities from arguments
    for (unsigned int blndprnum = prnum; blndprnum <= 3; blndprnum++)
    {
        //check if sprite is available to blend with, if sprite blending is enabled
        if (spriteBlendEnabled && objEnabled
         && (scanline->spritePriorityMask & (1u << blndprnum))
         && getAlphaBit(scanline->spriteLayers[blndprnum][pixelpos]) == 1)
        {
            *colorOutput = scanline->spriteLayers[blndprnum][pixelpos];
            return true;
        }
            
        unsigned int prCount = (unsigned int)scanline->prioritySortedBgsCount[blndprnum];
        for (unsigned int blndprsub = prsub; blndprsub < prCount; blndprsub++)
        {
            unsigned int currLayer = (unsigned int)scanline->prioritySortedBgs[blndprnum][blndprsub];
            // Disabled layers have no pixels and need not be read or cleared.
            if (!(enabledBgs & (1u << currLayer)))
                continue;
            if (getAlphaBit(scanline->layers[currLayer][pixelpos]) == 1)
            {
                if (bldcnt & (1u << (8 + currLayer)))
                {
                    *colorOutput = scanline->layers[currLayer][pixelpos];
                    return true;
                }
                if ((unsigned int)prnum != blndprnum)
                    return false;
            }
        }
        prsub = 0; //start from zero in the next iteration
    }
    //no background got hit, check if backdrop is enabled and return it if enabled otherwise fail
    if (bldcnt & BLDCNT_TGT2_BD)
    {
        *colorOutput = backdropColor;
        return true;
    }
    return false;
}

//checks if window horizontal is in bounds and takes account WIN wraparound
static bool winCheckHorizontalBounds(u16 left, u16 right, u16 xpos)
{
    if (left > right)
        return (xpos >= left || xpos < right);
    else
        return (xpos >= left && xpos < right);
}

static inline void CompositeSpriteLayer(const struct scanlineData *scanline, uint16_t *pixels,
                                        unsigned int priority, bool windowsEnabled,
                                        unsigned int firstX, unsigned int endX)
{
    const uint16_t *src = scanline->spriteLayers[priority];
    unsigned int xpos = firstX;

    if (!windowsEnabled)
    {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
        if (endX - xpos >= 8u)
        {
            uint16x8_t vAlpha = vdupq_n_u16(0x8000u);
            for (; xpos + 8u <= endX; xpos += 8u)
            {
                uint16x8_t s = vld1q_u16(&src[xpos]);
                uint16x8_t d = vld1q_u16(&pixels[xpos]);
                vst1q_u16(&pixels[xpos], vbslq_u16(vtstq_u16(s, vAlpha), s, d));
            }
        }
#endif
        for (; xpos < endX; xpos++)
        {
            if (getAlphaBit(src[xpos]))
                pixels[xpos] = src[xpos];
        }
        return;
    }

    const uint16_t *winMask = scanline->winMask;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (endX - xpos >= 8u)
    {
        uint16x8_t vAlpha = vdupq_n_u16(0x8000u);
        uint16x8_t vObjBit = vdupq_n_u16(WINMASK_OBJ);
        for (; xpos + 8u <= endX; xpos += 8u)
        {
            uint16x8_t s = vld1q_u16(&src[xpos]);
            uint16x8_t w = vld1q_u16(&winMask[xpos]);
            uint16x8_t d = vld1q_u16(&pixels[xpos]);
            uint16x8_t mask = vandq_u16(vtstq_u16(s, vAlpha), vtstq_u16(w, vObjBit));
            vst1q_u16(&pixels[xpos], vbslq_u16(mask, s, d));
        }
    }
#endif
    for (; xpos < endX; xpos++)
    {
        if (getAlphaBit(src[xpos]) && (winMask[xpos] & WINMASK_OBJ))
            pixels[xpos] = src[xpos];
    }
}

static inline bool GetSpriteDimensions(const struct OamData *oam, unsigned int *outWidth, unsigned int *outHeight)
{
    if (oam->shape == 0)
    {
        unsigned int dim = (1u << oam->size) * 8u;
        *outWidth = dim;
        *outHeight = dim;
        return true;
    }
    if (oam->shape == 1) // wide
    {
        *outWidth = spriteSizes[oam->size][1];
        *outHeight = spriteSizes[oam->size][0];
        return true;
    }
    if (oam->shape == 2) // tall
    {
        *outWidth = spriteSizes[oam->size][0];
        *outHeight = spriteSizes[oam->size][1];
        return true;
    }
    return false;
}

static inline void RenderNonAffineSpriteSpan(const struct OamData *oam,
                                             unsigned int width,
                                             unsigned int height,
                                             int32_t centerX,
                                             int nonAffineTileY,
                                             int nonAffineBlockRow,
                                             const uint8_t *tileBase,
                                             const uint16_t *palette,
                                             bool flipX,
                                             bool is8BPP,
                                             uint16_t *dstPixels,
                                             uint16_t *trackMinX,
                                             uint16_t *trackMaxX)
{
    int half_width = (int)(width / 2);
    int min_local_x = -half_width;
    int max_local_x = half_width - 1;
    if (min_local_x + centerX < 0)
        min_local_x = -centerX;
    if (max_local_x + centerX >= DISPLAY_WIDTH)
        max_local_x = (int)DISPLAY_WIDTH - 1 - centerX;
    if (min_local_x > max_local_x)
        return;

    int startX = centerX - half_width;
    int drawStart = centerX + min_local_x;
    int drawEnd = centerX + max_local_x + 1;
    uint16_t minWritten = DISPLAY_WIDTH;
    uint16_t maxWritten = 0;

    if (!is8BPP)
    {
        for (int gx = drawStart; gx < drawEnd;)
        {
            int tex_x = gx - startX;
            if (flipX)
                tex_x = (int)width - tex_x - 1;
            int tileX = tex_x & 7;
            int span;
            const uint8_t *tileRow = &tileBase[(nonAffineBlockRow + (tex_x >> 3) + oam->tileNum) * 32 + nonAffineTileY * 4];
            uint32_t row32 = ReadU32(tileRow);

            if (!flipX)
            {
                span = 8 - tileX;
                if (span > drawEnd - gx)
                    span = drawEnd - gx;
                if (row32 != 0)
                {
                    if (trackMinX == NULL && span == 8)
                    {
                        uint32_t hasZero = (row32 - 0x11111111u) & ~row32 & 0x88888888u;
                        uint16_t *dst = &dstPixels[gx];
                        if (hasZero == 0u)
                        {
                            uint32_t c0 = palette[row32 & 0xFu] | 0x8000u;
                            uint32_t c1 = palette[(row32 >> 4) & 0xFu] | 0x8000u;
                            uint32_t c2 = palette[(row32 >> 8) & 0xFu] | 0x8000u;
                            uint32_t c3 = palette[(row32 >> 12) & 0xFu] | 0x8000u;
                            uint32_t c4 = palette[(row32 >> 16) & 0xFu] | 0x8000u;
                            uint32_t c5 = palette[(row32 >> 20) & 0xFu] | 0x8000u;
                            uint32_t c6 = palette[(row32 >> 24) & 0xFu] | 0x8000u;
                            uint32_t c7 = palette[row32 >> 28] | 0x8000u;
                            WriteU32(&dst[0], c0 | (c1 << 16));
                            WriteU32(&dst[2], c2 | (c3 << 16));
                            WriteU32(&dst[4], c4 | (c5 << 16));
                            WriteU32(&dst[6], c6 | (c7 << 16));
                        }
                        else
                        {
                            uint32_t px;
                            if ((px = row32 & 0xFu) != 0)         dst[0] = palette[px] | 0x8000;
                            if ((px = (row32 >> 4) & 0xFu) != 0)  dst[1] = palette[px] | 0x8000;
                            if ((px = (row32 >> 8) & 0xFu) != 0)  dst[2] = palette[px] | 0x8000;
                            if ((px = (row32 >> 12) & 0xFu) != 0) dst[3] = palette[px] | 0x8000;
                            if ((px = (row32 >> 16) & 0xFu) != 0) dst[4] = palette[px] | 0x8000;
                            if ((px = (row32 >> 20) & 0xFu) != 0) dst[5] = palette[px] | 0x8000;
                            if ((px = (row32 >> 24) & 0xFu) != 0) dst[6] = palette[px] | 0x8000;
                            if ((px = (row32 >> 28)) != 0)        dst[7] = palette[px] | 0x8000;
                        }
                    }
                    else
                    {
                        uint32_t p = row32 >> (tileX * 4);
                        for (int k = 0; k < span; k++)
                        {
                            uint32_t px = p & 0xFu;
                            if (px != 0)
                            {
                                uint16_t global_x = (uint16_t)(gx + k);
                                dstPixels[global_x] = palette[px] | 0x8000;
                                if (trackMinX != NULL)
                                {
                                    if (global_x < minWritten)
                                        minWritten = global_x;
                                    if (global_x >= maxWritten)
                                        maxWritten = global_x + 1;
                                }
                            }
                            p >>= 4;
                        }
                    }
                }
            }
            else
            {
                span = tileX + 1;
                if (span > drawEnd - gx)
                    span = drawEnd - gx;
                if (row32 != 0)
                {
                    uint32_t p = row32 << ((7 - tileX) * 4);
                    for (int k = 0; k < span; k++)
                    {
                        uint32_t px = p >> 28;
                        if (px != 0)
                        {
                            uint16_t global_x = (uint16_t)(gx + k);
                            dstPixels[global_x] = palette[px] | 0x8000;
                            if (trackMinX != NULL)
                            {
                                if (global_x < minWritten)
                                    minWritten = global_x;
                                if (global_x >= maxWritten)
                                    maxWritten = global_x + 1;
                            }
                        }
                        p <<= 4;
                    }
                }
            }
            gx += span;
        }
    }
    else
    {
        for (int gx = drawStart; gx < drawEnd; gx++)
        {
            int tex_x = gx - startX;
            if (flipX)
                tex_x = (int)width - tex_x - 1;
            uint8_t pixel = tileBase[(nonAffineBlockRow * 2 + (tex_x >> 3) * 2 + oam->tileNum) * 32
                                   + (nonAffineTileY * 8) + (tex_x & 7)];
            if (pixel != 0)
            {
                dstPixels[gx] = palette[pixel] | 0x8000;
                if (trackMinX != NULL)
                {
                    if ((uint16_t)gx < minWritten)
                        minWritten = (uint16_t)gx;
                    if ((uint16_t)gx >= maxWritten)
                        maxWritten = (uint16_t)(gx + 1);
                }
            }
        }
    }

    if (trackMinX != NULL)
    {
        if (minWritten < *trackMinX)
            *trackMinX = minWritten;
        if (maxWritten > *trackMaxX)
            *trackMaxX = maxWritten;
    }
}

// Parts of this code heavily borrowed from NanoboyAdvance.
static void DrawSprites(struct scanlineData* scanline, uint16_t vcount, bool windowsEnabled,
                        const struct HBlankRowState *hblankRow)
{
    int i;
    unsigned int x;
    unsigned int y;
    void *objtiles = VRAM_ + 0x10000;
    unsigned int dispcnt = REG_DISPCNT & ~(unsigned int)gFastDrawDispcntClear;
    unsigned int mode = dispcnt & 3u;
    unsigned int enabledBgs = (dispcnt >> 8) & ((1u << (mode == 0u ? 4u : 3u)) - 1u);
    bool objEnabled = (dispcnt & DISPCNT_OBJ_ON) != 0;
    unsigned int bldcnt = (hblankRow ? hblankRow->bldcnt : REG_BLDCNT) & ~(unsigned int)gFastDrawBldcntClear;
    unsigned int blendMode = (bldcnt >> 6) & 3;
    unsigned int bldalpha = hblankRow ? hblankRow->bldalpha : REG_BLDALPHA;
    unsigned int eva = bldalpha & 0x1F;
    unsigned int evb = (bldalpha >> 8) & 0x1F;
    unsigned int evy = (hblankRow ? hblankRow->bldy : REG_BLDY) & 0x1F;
    uint16_t backdropRaw = (gFastDrawBackdropOverride >= 0) ? (uint16_t)(gFastDrawBackdropOverride & 0x7FFFu) : *(uint16_t *)PLTT;
    bool objOneDimensional = (dispcnt & DISPCNT_OBJ_1D_MAP) != 0;
    bool winShouldBlendPixel = true;

    int16_t matrix[2][2];

    if (!(dispcnt & (1 << 6)))
    {
        puts("2-D OBJ Character mapping not supported.");
    }

#ifdef EASY_DRAW_TEST_PARALLEL
    int spriteCount = 128;
    if (!sDisableSpriteScanlinePrefilter)
    {
        if (!sSpriteBinsValid)
        {
            BuildSpriteScanlineBins(&sSpriteBins);
            sSpriteBinsValid = true;
        }
        spriteCount = sSpriteBins.count[vcount];
    }
#else
    if (!sSpriteBinsValid)
    {
        BuildSpriteScanlineBins(&sSpriteBins);
        sSpriteBinsValid = true;
    }
    int spriteCount = sSpriteBins.count[vcount];
#endif

    for (int spriteIdx = 0; spriteIdx < spriteCount; spriteIdx++)
    {
#ifdef EASY_DRAW_TEST_PARALLEL
        if (sDisableSpriteScanlinePrefilter)
            i = 127 - spriteIdx;
        else
            i = sSpriteBins.indices[vcount][spriteIdx];
        if (gFastDrawScreenObjOnly && ((gScreenOam[i >> 5] & (1u << (i & 31))) == 0))
            continue;
#else
        i = sSpriteBins.indices[vcount][spriteIdx];
#endif
        struct OamData *oam = &((struct OamData *)OAM)[i];
        unsigned int width;
        unsigned int height;
        uint16_t *pixels;

        bool isAffine  = oam->affineMode & 1;
        bool doubleSizeOrDisabled = (oam->affineMode >> 1) & 1;
        bool isSemiTransparent = (oam->objMode == 1);
        bool isObjWin = (oam->objMode == 2);
        bool useGeneralSpritePixelPath = isAffine;
#ifdef EASY_DRAW_TEST_PARALLEL
        useGeneralSpritePixelPath |= sForceLegacySpritePixelPath;
#endif

        if (!(isAffine) && doubleSizeOrDisabled) // disable for non-affine
        {
            continue;
        }

        if (oam->shape == 0)
        {
            width = (1 << oam->size) * 8;
            height = (1 << oam->size) * 8;
        }
        else if (oam->shape == 1) // wide
        {
            width = spriteSizes[oam->size][1];
            height = spriteSizes[oam->size][0];
        }
        else if (oam->shape == 2) // tall
        {
            width = spriteSizes[oam->size][0];
            height = spriteSizes[oam->size][1];
        }
        else
        {
            continue; // prohibited, do not draw
        }

        int half_width = width / 2;
        int half_height = height / 2;
        int32_t y = oam->y;
        if (y >= DISPLAY_HEIGHT)
            y -= 256;

        if (isAffine && doubleSizeOrDisabled)
        {
            half_width *= 2;
            half_height *= 2;
        }

#ifdef EASY_DRAW_TEST_PARALLEL
        if (!sDisableSpriteScanlinePrefilter
         && (vcount < y || (int32_t)vcount >= y + half_height * 2))
            continue;
#else
        if (vcount < y || (int32_t)vcount >= y + half_height * 2)
            continue;
#endif

        pixels = scanline->spriteLayers[oam->priority];

        int32_t x = oam->x;
        if (x >= DISPLAY_WIDTH)
            x -= 512;

        if (isAffine)
        {
            //TODO: there is probably a better way to do this
            u8 matrixNum = oam->matrixNum * 4;

            struct OamData *oam1 = &((struct OamData *)OAM)[matrixNum];
            struct OamData *oam2 = &((struct OamData *)OAM)[matrixNum + 1];
            struct OamData *oam3 = &((struct OamData *)OAM)[matrixNum + 2];
            struct OamData *oam4 = &((struct OamData *)OAM)[matrixNum + 3];

            matrix[0][0] = oam1->affineParam;
            matrix[0][1] = oam2->affineParam;
            matrix[1][0] = oam3->affineParam;
            matrix[1][1] = oam4->affineParam;
        }
#ifdef EASY_DRAW_TEST_PARALLEL
        else if (sForceLegacySpritePixelPath)
        {
            matrix[0][0] = 0x100;
            matrix[0][1] = 0;
            matrix[1][0] = 0;
            matrix[1][1] = 0x100;
        }
#endif

        x += half_width;
        y += half_height;

#ifdef EASY_DRAW_TEST_PARALLEL
        if (sDisableSpriteScanlinePrefilter
         && !(vcount >= (y - half_height) && vcount < (y + half_height)))
            continue;
#endif
        {
            bool spriteMosaicY = (oam->mosaic == 1) && (mosaicSpriteEffectY != 0u);
            bool spriteMosaicX = (oam->mosaic == 1) && (mosaicSpriteEffectX != 0u);
            int local_y = spriteMosaicY ? (int)applySpriteVerticalMosaicEffect(vcount) - y : vcount - y;
            int paletteNum = oam->paletteNum;
            bool flipX  = !isAffine && ((oam->matrixNum >> 3) & 1);
            bool flipY  = !isAffine && ((oam->matrixNum >> 4) & 1);
            bool is8BPP  = oam->bpp & 1;
            uint8_t *tiledata = (uint8_t *)objtiles;
            uint16_t *palette = (uint16_t *)(PLTT + 0x200);
            if (!is8BPP)
                palette += paletteNum * 16;

            int objTileRowStride = objOneDimensional ? width / 8 : 16;
            int nonAffineTileY = 0;
            int nonAffineBlockRow = 0;
            if (!useGeneralSpritePixelPath)
            {
                int source_y = local_y + (height / 2);
                if (flipY)
                    source_y = height - source_y - 1;
                if (source_y < 0 || source_y >= height)
                    continue;

                nonAffineTileY = source_y & 7;
                nonAffineBlockRow = (source_y >> 3) * objTileRowStride;
            }

            if (!useGeneralSpritePixelPath && !spriteMosaicX)
            {
                int min_local_x = -half_width;
                int max_local_x = half_width - 1;
                if (min_local_x + x < 0)
                    min_local_x = -x;
                if (max_local_x + x >= DISPLAY_WIDTH)
                    max_local_x = (int)DISPLAY_WIDTH - 1 - x;
                if (min_local_x > max_local_x)
                    continue;

                bool simpleSpriteComposition = !windowsEnabled && !isObjWin
                    && !isSemiTransparent
                    && (blendMode == 0 || !(bldcnt & BLDCNT_TGT1_OBJ));
#ifdef EASY_DRAW_TEST_PARALLEL
                simpleSpriteComposition &= !sForceLegacySimpleSpritePath;
#endif
                if (simpleSpriteComposition)
                {
                    RenderNonAffineSpriteSpan(oam, width, height, x,
                                              nonAffineTileY, nonAffineBlockRow,
                                              tiledata, palette, flipX, is8BPP,
                                              pixels,
                                              &scanline->spriteMinX[oam->priority],
                                              &scanline->spriteMaxX[oam->priority]);
                    continue;
                }

                for (int local_x = min_local_x; local_x <= max_local_x; local_x++)
                {
                    unsigned int global_x = (unsigned int)(local_x + x);
                    int tex_x = local_x + (width / 2);
                    if (flipX)
                        tex_x = width - tex_x - 1;

                    int tile_y = nonAffineTileY;
                    int block_offset = nonAffineBlockRow + (tex_x >> 3);
                    int tile_x = tex_x & 7;
                    uint16_t pixel = 0;

                    if (!is8BPP)
                    {
                        pixel = tiledata[(block_offset + oam->tileNum) * 32 + (tile_y * 4) + (tile_x / 2)];
                        if (tile_x & 1)
                            pixel >>= 4;
                        else
                            pixel &= 0xF;
                    }
                    else
                    {
                        pixel = tiledata[(block_offset * 2 + oam->tileNum) * 32 + (tile_y * 8) + tile_x];
                    }

                    if (pixel != 0)
                    {
                        uint16_t color = palette[pixel];

                        if (isObjWin)
                        {
                            if (windowsEnabled && (scanline->winMask[global_x] & WINMASK_WINOUT))
                                scanline->winMask[global_x] = (REG_WINOUT >> 8) & 0x3F;
                            continue;
                        }

                        winShouldBlendPixel = (windowsEnabled == false || (scanline->winMask[global_x] & WINMASK_CLR));

                        if ((blendMode == 1 && (bldcnt & BLDCNT_TGT1_OBJ) && winShouldBlendPixel) || isSemiTransparent)
                        {
                            uint16_t targetA = color;
                            uint16_t targetB = 0;
                            if (alphaBlendSelectTargetB(scanline, &targetB, oam->priority, 0, global_x, false,
                                                        enabledBgs, objEnabled, bldcnt, backdropRaw))
                            {
                                color = alphaBlendColorWithCoeffs(targetA, targetB, eva, evb);
                            }
                        }
                        else if ((bldcnt & BLDCNT_TGT1_OBJ) && winShouldBlendPixel)
                        {
                            switch (blendMode)
                            {
                            case 2:
                                color = alphaBrightnessIncreaseWithY(color, evy);
                                break;
                            case 3:
                                color = alphaBrightnessDecreaseWithY(color, evy);
                                break;
                            }
                        }

                        pixels[global_x] = color | (1 << 15);
                        if (global_x < scanline->spriteMinX[oam->priority])
                            scanline->spriteMinX[oam->priority] = global_x;
                        if (global_x >= scanline->spriteMaxX[oam->priority])
                            scanline->spriteMaxX[oam->priority] = global_x + 1;
                    }
                }
                continue;
            }

            int minLocalX = -half_width;
            int maxLocalX = half_width;
            if (minLocalX + x < 0)
                minLocalX = -x;
            if (maxLocalX + x >= (int32_t)DISPLAY_WIDTH)
                maxLocalX = (int32_t)DISPLAY_WIDTH - 1 - x;
            if (minLocalX > maxLocalX)
                continue;

            int stepTx = 0;
            int stepTy = 0;
            int curTxFp = 0;
            int curTyFp = 0;
            bool stepAffineUv = useGeneralSpritePixelPath && !spriteMosaicX;
            if (stepAffineUv)
            {
                stepTx = matrix[0][0];
                stepTy = matrix[1][0];
                curTxFp = stepTx * minLocalX + matrix[0][1] * local_y + ((int)(width / 2) << 8);
                curTyFp = stepTy * minLocalX + matrix[1][1] * local_y + ((int)(height / 2) << 8);
            }

            for (int local_x = minLocalX; local_x <= maxLocalX; local_x++, curTxFp += stepTx, curTyFp += stepTy)
            {
                int tex_x;
                int tex_y;

                unsigned int global_x = (unsigned int)(local_x + x);

                if (stepAffineUv)
                {
                    tex_x = curTxFp >> 8;
                    tex_y = curTyFp >> 8;
                }
                else
                {
                    int source_x = local_x;
                    if (spriteMosaicX)
                        source_x = (int)applySpriteHorizontalMosaicEffect(global_x) - x;

                    if (useGeneralSpritePixelPath)
                    {
                        tex_x = ((matrix[0][0] * source_x + matrix[0][1] * local_y) >> 8) + (width / 2);
                        tex_y = ((matrix[1][0] * source_x + matrix[1][1] * local_y) >> 8) + (height / 2);
                    }
                    else
                    {
                        /* Non-affine sprites use the identity matrix. Apply that
                         * directly so every OBJ pixel avoids four multiplies. */
                        tex_x = source_x + (width / 2);
                        tex_y = local_y + (height / 2);
                    }
                }

                int tile_y;
                int block_offset;
                if (useGeneralSpritePixelPath)
                {
                    /* The legacy non-affine path uses the identity matrix;
                     * affine transforms can change both coordinates. */
                    if (tex_x >= width || tex_y >= height || tex_x < 0 || tex_y < 0)
                        continue;

                    if (flipX)
                        tex_x = width - tex_x - 1;
                    if (flipY)
                        tex_y = height - tex_y - 1;

                    tile_y = tex_y & 7;
                    block_offset = (tex_y >> 3) * objTileRowStride + (tex_x >> 3);
#ifdef EASY_DRAW_TEST_PARALLEL
                    if (!isAffine && sForceLegacySpritePixelPath)
                    {
                        int block_x = tex_x / 8;
                        int block_y = tex_y / 8;
                        tile_y = tex_y % 8;
                        block_offset = block_y * (REG_DISPCNT & 0x40 ? width / 8 : 16) + block_x;
                    }
#endif
                }
                else
                {
                    if (tex_x >= width || tex_x < 0)
                        continue;

                    if (flipX)
                        tex_x = width - tex_x - 1;

                    tile_y = nonAffineTileY;
                    block_offset = nonAffineBlockRow + (tex_x >> 3);
                }

                int tile_x = tex_x & 7;
                uint16_t pixel = 0;

                if (!is8BPP)
                {
                    pixel = tiledata[(block_offset + oam->tileNum) * 32 + (tile_y * 4) + (tile_x / 2)];
                    if (tile_x & 1)
                        pixel >>= 4;
                    else
                        pixel &= 0xF;
                }
                else
                {
                    pixel = tiledata[(block_offset * 2 + oam->tileNum) * 32 + (tile_y * 8) + tile_x];
                }

                if (pixel != 0)
                {
                    uint16_t color = palette[pixel];;
                    
                    //if sprite mode is 2 then write to the window mask instead
                    if (isObjWin)
                    {
                        if (windowsEnabled && (scanline->winMask[global_x] & WINMASK_WINOUT))
                        {
                            uint16_t winoutReg = hblankRow ? hblankRow->winout : REG_WINOUT;
                            scanline->winMask[global_x] = (winoutReg >> 8) & 0x3F;
                        }
                        continue;
                    }
                    //this code runs if pixel is to be drawn
                    if (global_x < DISPLAY_WIDTH && global_x >= 0)
                    {
                        //check if its enabled in the window (if window is enabled)
                        winShouldBlendPixel = (windowsEnabled == false || scanline->winMask[global_x] & WINMASK_CLR);
                        
                        //has to be separated from the blend mode switch statement because of OBJ semi transparancy feature
                        if ((blendMode == 1 && bldcnt & BLDCNT_TGT1_OBJ && winShouldBlendPixel) || isSemiTransparent)
                        {
                            uint16_t targetA = color;
                            uint16_t targetB = 0;
                            if (alphaBlendSelectTargetB(scanline, &targetB, oam->priority, 0, global_x, false,
                                                        enabledBgs, objEnabled, bldcnt, backdropRaw))
                            {
                                color = alphaBlendColorWithCoeffs(targetA, targetB, eva, evb);
                            }
                        }
                        else if (bldcnt & BLDCNT_TGT1_OBJ && winShouldBlendPixel)
                        {
                            switch (blendMode)
                            {
                            case 2:
                                color = alphaBrightnessIncreaseWithY(color, evy);
                                break;
                            case 3:
                                color = alphaBrightnessDecreaseWithY(color, evy);
                                break;
                            }
                        }
                        
                        //write pixel to pixel framebuffer
                        pixels[global_x] = color | (1 << 15);
                        if (global_x < scanline->spriteMinX[oam->priority])
                            scanline->spriteMinX[oam->priority] = global_x;
                        if (global_x >= scanline->spriteMaxX[oam->priority])
                            scanline->spriteMaxX[oam->priority] = global_x + 1;
                    }
                }
            }
        }
    }
}

static void DrawSpritesDirectForPriority(uint16_t *pixels, uint16_t vcount, unsigned int priority)
{
    const uint8_t *tiledata = (const uint8_t *)(VRAM_ + 0x10000);
    const uint16_t *objPalette = (const uint16_t *)(PLTT + 0x200);
    bool objOneDimensional = (REG_DISPCNT & DISPCNT_OBJ_1D_MAP) != 0;
    int spriteCount = sSpriteBins.count[vcount];

    for (int spriteIdx = 0; spriteIdx < spriteCount; spriteIdx++)
    {
        int i = sSpriteBins.indices[vcount][spriteIdx];
        const struct OamData *oam = &((const struct OamData *)OAM)[i];
        if (oam->priority != priority)
            continue;

        unsigned int width, height;
        if (!GetSpriteDimensions(oam, &width, &height))
            continue;

        int half_width = (int)(width / 2);
        int half_height = (int)(height / 2);
        int32_t y = oam->y;
        if (y >= DISPLAY_HEIGHT)
            y -= 256;
        if ((int32_t)vcount < y || (int32_t)vcount >= y + half_height * 2)
            continue;

        int32_t x = oam->x;
        if (x >= DISPLAY_WIDTH)
            x -= 512;
        x += half_width;
        y += half_height;

        int source_y = ((int)vcount - y) + half_height;
        bool flipX = ((oam->matrixNum >> 3) & 1) != 0;
        bool flipY = ((oam->matrixNum >> 4) & 1) != 0;
        if (flipY)
            source_y = (int)height - source_y - 1;
        if (source_y < 0 || source_y >= (int)height)
            continue;

        bool is8BPP = (oam->bpp & 1) != 0;
        const uint16_t *palette = objPalette + (is8BPP ? 0 : oam->paletteNum * 16);
        int objTileRowStride = objOneDimensional ? (int)(width / 8) : 16;
        int nonAffineTileY = source_y & 7;
        int nonAffineBlockRow = (source_y >> 3) * objTileRowStride;

        RenderNonAffineSpriteSpan(oam, width, height, x,
                                  nonAffineTileY, nonAffineBlockRow,
                                  tiledata, palette, flipX, is8BPP,
                                  pixels, NULL, NULL);
    }
}

static void DrawScanlineDirectWithDispcnt(uint16_t *pixels, uint16_t vcount,
                                          const struct HBlankRowState *hblankRow,
                                          unsigned int dispcnt)
{
    unsigned int mode = dispcnt & 3u;
    unsigned int numOfBgs = (mode == 0u) ? 4u : 3u;
    unsigned int enabledBgs = (dispcnt >> 8) & ((1u << numOfBgs) - 1u);
    bool objEnabled = (dispcnt & DISPCNT_OBJ_ON) != 0;
    uint16_t bgcnts[4] = { 0, 0, 0, 0 };
    uint8_t bgPriorityMask = 0;

    if (enabledBgs != 0)
    {
        for (unsigned int bgnum = 0; bgnum < numOfBgs; bgnum++)
        {
            if (enabledBgs & (1u << bgnum))
            {
                uint16_t bgcnt = *(uint16_t *)(REG_ADDR_BG0CNT + bgnum * 2);
                bgcnts[bgnum] = bgcnt;
                bgPriorityMask |= (uint8_t)(1u << (bgcnt & 3u));
            }
        }
    }

    uint8_t objPriorityMask = objEnabled ? sSpriteBins.priorityMask[vcount] : 0;

    for (int prnum = 3; prnum >= 0; prnum--)
    {
        if (bgPriorityMask & (1u << prnum))
        {
            for (int bgnum = (int)numOfBgs - 1; bgnum >= 0; bgnum--)
            {
                if ((enabledBgs & (1u << bgnum)) && (int)(bgcnts[bgnum] & 3u) == prnum)
                {
                    if (mode == 1u && bgnum == 2)
                    {
                        RenderRotScaleBGScanline(2, bgcnts[2], REG_BG2X, REG_BG2Y, vcount, pixels);
                    }
                    else
                    {
                        uint16_t bghoffs = hblankRow
                            ? hblankRow->bgHofs[bgnum]
                            : *(uint16_t *)(REG_ADDR_BG0HOFS + bgnum * 4);
                        uint16_t bgvoffs = hblankRow
                            ? hblankRow->bgVofs[bgnum]
                            : *(uint16_t *)(REG_ADDR_BG0VOFS + bgnum * 4);
                        RenderBGScanline(bgnum, bgcnts[bgnum], bghoffs, bgvoffs, vcount, pixels);
                    }
                }
            }
        }

        if (objPriorityMask & (1u << prnum))
            DrawSpritesDirectForPriority(pixels, vcount, (unsigned int)prnum);
    }
}

static inline void DrawScanlineDirect(uint16_t *pixels, uint16_t vcount,
                                      const struct HBlankRowState *hblankRow)
{
    DrawScanlineDirectWithDispcnt(pixels, vcount, hblankRow,
                                  REG_DISPCNT & ~(unsigned int)gFastDrawDispcntClear);
}

struct BlendTargetEntry {
    const uint16_t *pixels;
    bool isTarget2;
    bool stopOnNonTarget2;
};

static inline int CollectBgBlendTargets(const struct scanlineData *scanline,
                                        int prnum, int prsub,
                                        bool spriteBlendEnabled,
                                        unsigned int enabledBgs,
                                        bool objEnabled,
                                        unsigned int bldcnt,
                                        struct BlendTargetEntry out[8])
{
    int count = 0;
    int subStart = prsub + 1;

    for (unsigned int blndprnum = (unsigned int)prnum; blndprnum <= 3u; blndprnum++)
    {
        if (spriteBlendEnabled && objEnabled && (scanline->spritePriorityMask & (1u << blndprnum)))
        {
            out[count].pixels = scanline->spriteLayers[blndprnum];
            out[count].isTarget2 = true;
            out[count].stopOnNonTarget2 = false;
            count++;
        }

        unsigned int prCount = (unsigned int)scanline->prioritySortedBgsCount[blndprnum];
        for (unsigned int blndprsub = (unsigned int)subStart; blndprsub < prCount; blndprsub++)
        {
            unsigned int currLayer = (unsigned int)scanline->prioritySortedBgs[blndprnum][blndprsub];
            if (!(enabledBgs & (1u << currLayer)))
                continue;
            out[count].pixels = scanline->layers[currLayer];
            out[count].isTarget2 = (bldcnt & (1u << (8u + currLayer))) != 0u;
            out[count].stopOnNonTarget2 = (blndprnum != (unsigned int)prnum);
            count++;
        }
        subStart = 0;
    }

    return count;
}

static inline bool SelectPrebuiltTargetB(const struct BlendTargetEntry *targets,
                                         int targetCount,
                                         int pixelpos,
                                         bool hasBackdropTarget,
                                         uint16_t backdropColor,
                                         uint16_t *colorOutput)
{
    for (int i = 0; i < targetCount; i++)
    {
        uint16_t candidate = targets[i].pixels[pixelpos];
        if (candidate & 0x8000u)
        {
            if (targets[i].isTarget2)
            {
                *colorOutput = candidate;
                return true;
            }
            if (targets[i].stopOnNonTarget2)
                return false;
        }
    }

    if (hasBackdropTarget)
    {
        *colorOutput = backdropColor;
        return true;
    }
    return false;
}

static inline void CompositeRowOpaqueFast(uint16_t *pixels, const uint16_t *src)
{
    unsigned int xpos = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    uint16x8_t vAlpha = vdupq_n_u16(0x8000u);
    for (; xpos < DISPLAY_WIDTH; xpos += 8u)
    {
        uint16x8_t s = vld1q_u16(&src[xpos]);
        uint16x8_t d = vld1q_u16(&pixels[xpos]);
        vst1q_u16(&pixels[xpos], vbslq_u16(vtstq_u16(s, vAlpha), s, d));
    }
#else
    const uint32_t *src32 = (const uint32_t *)src;
    uint32_t *pixels32 = (uint32_t *)pixels;
    for (xpos = 0; xpos < DISPLAY_WIDTH / 2u; xpos++)
    {
        uint32_t pair = src32[xpos];
        uint32_t alphaMask = pair & 0x80008000u;
        if (alphaMask == 0x80008000u)
        {
            pixels32[xpos] = pair;
        }
        else if (alphaMask != 0u)
        {
            if (alphaMask & 0x8000u)
                pixels[xpos * 2u] = (uint16_t)pair;
            if (alphaMask & 0x80000000u)
                pixels[xpos * 2u + 1u] = (uint16_t)(pair >> 16);
        }
    }
#endif
}

static inline void CompositeRowMaskedFast(uint16_t *pixels, const uint16_t *src,
                                          const uint16_t *winMask, uint16_t bgBit)
{
    unsigned int xpos = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    uint16x8_t vAlpha = vdupq_n_u16(0x8000u);
    uint16x8_t vBgBit = vdupq_n_u16(bgBit);
    for (; xpos < DISPLAY_WIDTH; xpos += 8u)
    {
        uint16x8_t s = vld1q_u16(&src[xpos]);
        uint16x8_t w = vld1q_u16(&winMask[xpos]);
        uint16x8_t d = vld1q_u16(&pixels[xpos]);
        uint16x8_t mask = vandq_u16(vtstq_u16(s, vAlpha), vtstq_u16(w, vBgBit));
        vst1q_u16(&pixels[xpos], vbslq_u16(mask, s, d));
    }
#else
    for (; xpos < DISPLAY_WIDTH; xpos++)
    {
        uint16_t color = src[xpos];
        if ((color & 0x8000u) && (winMask[xpos] & bgBit))
            pixels[xpos] = color;
    }
#endif
}

static inline void CompositeRowBrightnessFast(uint16_t *pixels, const uint16_t *src,
                                              const uint16_t *winMask, uint16_t bgBit,
                                              bool increase, unsigned int evy)
{
    unsigned int xpos = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (evy <= 16u)
    {
        uint16x8_t vMask5 = vdupq_n_u16(0x001Fu);
        uint16x8_t v31 = vdupq_n_u16(31u);
        uint16x8_t vEvy = vdupq_n_u16((uint16_t)evy);
        uint16x8_t vAlpha = vdupq_n_u16(0x8000u);
        uint16x8_t vBgBit = vdupq_n_u16(bgBit);
        uint16x8_t vClrBit = vdupq_n_u16(WINMASK_CLR);

        for (; xpos < DISPLAY_WIDTH; xpos += 8u)
        {
            uint16x8_t s = vld1q_u16(&src[xpos]);
            uint16x8_t d = vld1q_u16(&pixels[xpos]);
            uint16x8_t r = vandq_u16(s, vMask5);
            uint16x8_t g = vandq_u16(vshrq_n_u16(s, 5), vMask5);
            uint16x8_t b = vandq_u16(vshrq_n_u16(s, 10), vMask5);
            if (increase)
            {
                r = vaddq_u16(r, vshrq_n_u16(vmulq_u16(vsubq_u16(v31, r), vEvy), 4));
                g = vaddq_u16(g, vshrq_n_u16(vmulq_u16(vsubq_u16(v31, g), vEvy), 4));
                b = vaddq_u16(b, vshrq_n_u16(vmulq_u16(vsubq_u16(v31, b), vEvy), 4));
            }
            else
            {
                r = vsubq_u16(r, vshrq_n_u16(vmulq_u16(r, vEvy), 4));
                g = vsubq_u16(g, vshrq_n_u16(vmulq_u16(g, vEvy), 4));
                b = vsubq_u16(b, vshrq_n_u16(vmulq_u16(b, vEvy), 4));
            }
            uint16x8_t faded = vorrq_u16(vorrq_u16(r, vshlq_n_u16(g, 5)),
                                         vorrq_u16(vshlq_n_u16(b, 10), vAlpha));
            uint16x8_t opaque = vtstq_u16(s, vAlpha);
            if (winMask != NULL)
            {
                uint16x8_t w = vld1q_u16(&winMask[xpos]);
                uint16x8_t clrMask = vtstq_u16(w, vClrBit);
                uint16x8_t out = vbslq_u16(clrMask, faded, s);
                uint16x8_t drawMask = vandq_u16(opaque, vtstq_u16(w, vBgBit));
                vst1q_u16(&pixels[xpos], vbslq_u16(drawMask, out, d));
            }
            else
            {
                vst1q_u16(&pixels[xpos], vbslq_u16(opaque, faded, d));
            }
        }
        return;
    }
#endif
    for (; xpos < DISPLAY_WIDTH; xpos++)
    {
        uint16_t color = src[xpos];
        if (!(color & 0x8000u))
            continue;
        if (winMask != NULL)
        {
            uint16_t mask = winMask[xpos];
            if (!(mask & bgBit))
                continue;
            if (mask & WINMASK_CLR)
                color = increase ? alphaBrightnessIncreaseWithY(color, evy)
                                 : alphaBrightnessDecreaseWithY(color, evy);
        }
        else
        {
            color = increase ? alphaBrightnessIncreaseWithY(color, evy)
                             : alphaBrightnessDecreaseWithY(color, evy);
        }
        pixels[xpos] = color;
    }
}

static inline void ResolveTargetBRowsFast(const struct BlendTargetEntry *targets,
                                          int targetCount,
                                          bool hasBackdropTarget,
                                          uint16_t backdropRaw,
                                          uint16_t *tgtB,
                                          uint16_t *tgtHit)
{
    FillU16(tgtB, backdropRaw, DISPLAY_WIDTH);
    FillU16(tgtHit, hasBackdropTarget ? 0xFFFFu : 0u, DISPLAY_WIDTH);

    for (int t = targetCount - 1; t >= 0; t--)
    {
        const uint16_t *candPixels = targets[t].pixels;
        if (targets[t].isTarget2)
        {
            unsigned int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            uint16x8_t vAlpha = vdupq_n_u16(0x8000u);
            for (; x < DISPLAY_WIDTH; x += 8u)
            {
                uint16x8_t c = vld1q_u16(&candPixels[x]);
                uint16x8_t m = vtstq_u16(c, vAlpha);
                vst1q_u16(&tgtB[x], vbslq_u16(m, c, vld1q_u16(&tgtB[x])));
                vst1q_u16(&tgtHit[x], vorrq_u16(vld1q_u16(&tgtHit[x]), m));
            }
#else
            for (; x < DISPLAY_WIDTH; x++)
            {
                uint16_t c = candPixels[x];
                if (c & 0x8000u)
                {
                    tgtB[x] = c;
                    tgtHit[x] = 0xFFFFu;
                }
            }
#endif
        }
        else if (targets[t].stopOnNonTarget2)
        {
            unsigned int x = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            uint16x8_t vAlpha = vdupq_n_u16(0x8000u);
            for (; x < DISPLAY_WIDTH; x += 8u)
            {
                uint16x8_t c = vld1q_u16(&candPixels[x]);
                uint16x8_t m = vtstq_u16(c, vAlpha);
                vst1q_u16(&tgtHit[x], vbicq_u16(vld1q_u16(&tgtHit[x]), m));
            }
#else
            for (; x < DISPLAY_WIDTH; x++)
            {
                if (candPixels[x] & 0x8000u)
                    tgtHit[x] = 0u;
            }
#endif
        }
    }
}

static inline void CompositeRowAlphaFast(uint16_t *pixels, const uint16_t *src,
                                         const uint16_t *tgtB, const uint16_t *tgtHit,
                                         const uint16_t *winMask, uint16_t bgBit,
                                         unsigned int eva, unsigned int evb)
{
    unsigned int xpos = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    uint16x8_t vMask5 = vdupq_n_u16(0x001Fu);
    uint16x8_t v31 = vdupq_n_u16(31u);
    uint16x8_t vAlpha = vdupq_n_u16(0x8000u);
    uint16x8_t vBgBit = vdupq_n_u16(bgBit);
    uint16x8_t vClrBit = vdupq_n_u16(WINMASK_CLR);
    uint16_t eva16 = (uint16_t)eva;
    uint16_t evb16 = (uint16_t)evb;

    for (; xpos < DISPLAY_WIDTH; xpos += 8u)
    {
        uint16x8_t s = vld1q_u16(&src[xpos]);
        uint16x8_t d = vld1q_u16(&pixels[xpos]);
        uint16x8_t bSrc = (tgtB != NULL) ? vld1q_u16(&tgtB[xpos]) : d;

        uint16x8_t rA = vandq_u16(s, vMask5);
        uint16x8_t gA = vandq_u16(vshrq_n_u16(s, 5), vMask5);
        uint16x8_t bA = vandq_u16(vshrq_n_u16(s, 10), vMask5);

        uint16x8_t rB = vandq_u16(bSrc, vMask5);
        uint16x8_t gB = vandq_u16(vshrq_n_u16(bSrc, 5), vMask5);
        uint16x8_t bB = vandq_u16(vshrq_n_u16(bSrc, 10), vMask5);

        uint16x8_t r = vminq_u16(vshrq_n_u16(vmlaq_n_u16(vmulq_n_u16(rA, eva16), rB, evb16), 4), v31);
        uint16x8_t g = vminq_u16(vshrq_n_u16(vmlaq_n_u16(vmulq_n_u16(gA, eva16), gB, evb16), 4), v31);
        uint16x8_t b = vminq_u16(vshrq_n_u16(vmlaq_n_u16(vmulq_n_u16(bA, eva16), bB, evb16), 4), v31);

        uint16x8_t blended = vorrq_u16(vorrq_u16(r, vshlq_n_u16(g, 5)),
                                       vorrq_u16(vshlq_n_u16(b, 10), vAlpha));
        uint16x8_t opaque = vtstq_u16(s, vAlpha);
        uint16x8_t hit = (tgtHit != NULL) ? vld1q_u16(&tgtHit[xpos]) : vdupq_n_u16(0xFFFFu);
        if (winMask != NULL)
        {
            uint16x8_t w = vld1q_u16(&winMask[xpos]);
            uint16x8_t blendMask = vandq_u16(hit, vtstq_u16(w, vClrBit));
            uint16x8_t out = vbslq_u16(blendMask, blended, s);
            uint16x8_t drawMask = vandq_u16(opaque, vtstq_u16(w, vBgBit));
            vst1q_u16(&pixels[xpos], vbslq_u16(drawMask, out, d));
        }
        else
        {
            uint16x8_t out = vbslq_u16(hit, blended, s);
            vst1q_u16(&pixels[xpos], vbslq_u16(opaque, out, d));
        }
    }
#else
    for (; xpos < DISPLAY_WIDTH; xpos++)
    {
        uint16_t color = src[xpos];
        if (!(color & 0x8000u))
            continue;
        if (winMask != NULL)
        {
            uint16_t mask = winMask[xpos];
            if (!(mask & bgBit))
                continue;
            if ((mask & WINMASK_CLR) && (tgtHit == NULL || tgtHit[xpos] != 0u))
                color = alphaBlendColorWithCoeffs(color, tgtB ? tgtB[xpos] : pixels[xpos], eva, evb);
        }
        else
        {
            if (tgtHit == NULL || tgtHit[xpos] != 0u)
                color = alphaBlendColorWithCoeffs(color, tgtB ? tgtB[xpos] : pixels[xpos], eva, evb);
        }
        pixels[xpos] = color;
    }
#endif
}

static inline void UpdateTgt2MaskFromRow(uint16_t *tgt2Mask, const uint16_t *src, uint16_t isTgt2Fill)
{
    unsigned int xpos = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    uint16x8_t vAlpha = vdupq_n_u16(0x8000u);
    uint16x8_t vFill = vdupq_n_u16(isTgt2Fill);
    for (; xpos < DISPLAY_WIDTH; xpos += 8u)
    {
        uint16x8_t s = vld1q_u16(&src[xpos]);
        uint16x8_t cur = vld1q_u16(&tgt2Mask[xpos]);
        vst1q_u16(&tgt2Mask[xpos], vbslq_u16(vtstq_u16(s, vAlpha), vFill, cur));
    }
#else
    for (; xpos < DISPLAY_WIDTH; xpos++)
    {
        if (src[xpos] & 0x8000u)
            tgt2Mask[xpos] = isTgt2Fill;
    }
#endif
}

static inline bool HasSamePriorityNonTarget2Hazard(unsigned int numOfBgs,
                                                   unsigned int enabledBgs,
                                                   const uint16_t bgcnts[4],
                                                   unsigned int bldcnt)
{
    for (unsigned int bg = 0; bg < numOfBgs; bg++)
    {
        if (!(enabledBgs & (1u << bg)) || !(bldcnt & (1u << bg)))
            continue;
        unsigned int prio = bgcnts[bg] & 3u;
        for (unsigned int lower = bg + 1u; lower < numOfBgs; lower++)
        {
            if ((enabledBgs & (1u << lower))
             && (bgcnts[lower] & 3u) == prio
             && !(bldcnt & (1u << (8u + lower))))
                return true;
        }
    }
    return false;
}

static __attribute__((noinline)) void DrawScanlineFallback(uint16_t *pixels, uint16_t vcount,
                                                           const struct HBlankRowState *hblankRow)
{
    unsigned int dispcnt = REG_DISPCNT & ~(unsigned int)gFastDrawDispcntClear;
    unsigned int mode = dispcnt & 3u;
    unsigned char numOfBgs = (mode == 0u ? 4 : 3);
    unsigned int enabledBgs = (dispcnt >> 8) & ((1u << numOfBgs) - 1u);
    int bgnum, prnum;
    struct scanlineData scanline;
    unsigned int bldcnt = (hblankRow ? hblankRow->bldcnt : REG_BLDCNT) & ~(unsigned int)gFastDrawBldcntClear;
    unsigned int blendMode = (bldcnt >> 6) & 3u;
    unsigned int bldalpha = hblankRow ? hblankRow->bldalpha : REG_BLDALPHA;
    unsigned int eva = bldalpha & 0x1Fu;
    unsigned int evb = (bldalpha >> 8) & 0x1Fu;
    unsigned int evy = (hblankRow ? hblankRow->bldy : REG_BLDY) & 0x1Fu;
    uint16_t backdropRaw = (gFastDrawBackdropOverride >= 0) ? (uint16_t)(gFastDrawBackdropOverride & 0x7FFFu) : *(uint16_t *)PLTT;
    unsigned int xpos;

    //initialize all priority bookkeeping data
#ifdef EASY_DRAW_TEST_PARALLEL
    if (sForceLegacyLayerClears)
        ZeroU32(scanline.layers, sizeof(scanline.layers));
    else
#endif
    {
        unsigned int afterFirst = enabledBgs & (enabledBgs - 1u);
        if (afterFirst & (afterFirst - 1u))
        {
            // Bulk clear is faster once at least three layers are active.
            ZeroU32(scanline.layers, sizeof(scanline.layers));
        }
        else
        {
            for (bgnum = 0; bgnum < numOfBgs; bgnum++)
                if (enabledBgs & (1u << bgnum))
                    ZeroU32(scanline.layers[bgnum], sizeof(scanline.layers[bgnum]));
        }
    }
    // The stable frame path knows which OBJ priorities can contribute on this
    // row. Clear only those planes; alpha blending and compositing also consult
    // the same mask, so untouched stack storage is never read. Raster-event
    // frames keep the full clear because callbacks or DMA can change OAM after
    // the bins were built.
    bool objEnabled = (dispcnt & DISPCNT_OBJ_ON) != 0;
#ifdef EASY_DRAW_TEST_PARALLEL
    bool useSparseSpritePlanes = objEnabled && sSpriteBinsValid
                              && sSpritePriorityMaskSafe && !sForceLegacyLayerClears;
#else
    bool useSparseSpritePlanes = objEnabled && sSpriteBinsValid
                              && sSpritePriorityMaskSafe;
#endif
    scanline.spritePriorityMask = objEnabled
        ? (useSparseSpritePlanes ? sSpriteBins.priorityMask[vcount] : 0x0Fu)
        : 0u;
    if (objEnabled && !useSparseSpritePlanes)
    {
        ZeroU32(scanline.spriteLayers, sizeof(scanline.spriteLayers));
    }
    else if (useSparseSpritePlanes)
    {
        for (int priority = 0; priority < 4; priority++)
        {
            if (scanline.spritePriorityMask & (1u << priority))
                ZeroU32(scanline.spriteLayers[priority], sizeof(scanline.spriteLayers[priority]));
        }
    }
    __builtin_memset(scanline.prioritySortedBgsCount, 0, sizeof(scanline.prioritySortedBgsCount));
    for (int priority = 0; priority < 4; priority++)
    {
        scanline.spriteMinX[priority] = DISPLAY_WIDTH;
        scanline.spriteMaxX[priority] = 0;
    }

    for (bgnum = 0; bgnum < numOfBgs; bgnum++)
    {
        uint16_t bgcnt = *(uint16_t*)(REG_ADDR_BG0CNT + bgnum * 2);
        uint16_t priority;
        scanline.bgcnts[bgnum] = bgcnt;
        scanline.bgtoprio[bgnum] = priority = (bgcnt & 3);
        
        char priorityCount = scanline.prioritySortedBgsCount[priority];
        scanline.prioritySortedBgs[priority][priorityCount] = bgnum;
        scanline.prioritySortedBgsCount[priority]++;
    }
    
    switch (mode)
    {
    case 0:
        // All backgrounds are text mode
        for (bgnum = 3; bgnum >= 0; bgnum--)
        {
            if (enabledBgs & (1u << bgnum))
            {
                uint16_t bghoffs = hblankRow
                    ? hblankRow->bgHofs[bgnum]
                    : *(uint16_t *)(REG_ADDR_BG0HOFS + bgnum * 4);
                uint16_t bgvoffs = hblankRow
                    ? hblankRow->bgVofs[bgnum]
                    : *(uint16_t *)(REG_ADDR_BG0VOFS + bgnum * 4);
                
                RenderBGScanline(bgnum, scanline.bgcnts[bgnum], bghoffs, bgvoffs, vcount, scanline.layers[bgnum]);
            }
        }
        
        break;
    case 1:
        // BG2 is affine
        bgnum = 2;
        if (enabledBgs & (1u << bgnum))
        {
            RenderRotScaleBGScanline(bgnum, scanline.bgcnts[bgnum], REG_BG2X, REG_BG2Y, vcount, scanline.layers[bgnum]);
        }
        // BG0 and BG1 are text mode
        for (bgnum = 1; bgnum >= 0; bgnum--)
        {
            if (enabledBgs & (1u << bgnum))
            {
                uint16_t bghoffs = hblankRow
                    ? hblankRow->bgHofs[bgnum]
                    : *(uint16_t *)(REG_ADDR_BG0HOFS + bgnum * 4);
                uint16_t bgvoffs = hblankRow
                    ? hblankRow->bgVofs[bgnum]
                    : *(uint16_t *)(REG_ADDR_BG0VOFS + bgnum * 4);
                
                RenderBGScanline(bgnum, scanline.bgcnts[bgnum], bghoffs, bgvoffs, vcount, scanline.layers[bgnum]);
            }
        }
        break;
    default:
        DBGPRINTF("Video mode %u is unsupported.\n", mode);
        break;
    }
    
    bool windowsEnabled = false;
    uint16_t WIN0bottom, WIN0top, WIN0right = 0, WIN0left = 0;
    uint16_t WIN1bottom, WIN1top, WIN1right = 0, WIN1left = 0;
    bool WIN0enable = false;
    bool WIN1enable = false;

    //figure out if WIN0 masks on this scanline
    if (dispcnt & DISPCNT_WIN0_ON)
    {
        uint16_t win0v = hblankRow ? hblankRow->win0v : REG_WIN0V;
        WIN0bottom = win0v & 0xFFu;
        WIN0top = (win0v >> 8) & 0xFFu;
        uint16_t win0h = hblankRow ? hblankRow->win0h : REG_WIN0H;
        WIN0right = win0h & 0xFFu;
        WIN0left = (win0h >> 8) & 0xFFu;
        
        //figure out WIN Y wraparound and check bounds accordingly
        if (WIN0top > WIN0bottom) {
            if (vcount >= WIN0top || vcount < WIN0bottom)
                WIN0enable = true;
        } else {
            if (vcount >= WIN0top && vcount < WIN0bottom)
                WIN0enable = true;
        }
        
        windowsEnabled = true;
    }
    //figure out if WIN1 masks on this scanline
    if (dispcnt & DISPCNT_WIN1_ON)
    {
        uint16_t win1v = hblankRow ? hblankRow->win1v : REG_WIN1V;
        WIN1bottom = win1v & 0xFFu;
        WIN1top = (win1v >> 8) & 0xFFu;
        uint16_t win1h = hblankRow ? hblankRow->win1h : REG_WIN1H;
        WIN1right = win1h & 0xFFu;
        WIN1left = (win1h >> 8) & 0xFFu;
        
        if (WIN1top > WIN1bottom) {
            if (vcount >= WIN1top || vcount < WIN1bottom)
                WIN1enable = true;
        } else {
            if (vcount >= WIN1top && vcount < WIN1bottom)
                WIN1enable = true;
        }
        
        windowsEnabled = true;
    }
    //enable windows if OBJwin is enabled
    if ((dispcnt & DISPCNT_OBJWIN_ON) && objEnabled)
    {
        windowsEnabled = true;
    }
    
    //draw to pixel mask (all 240 entries are populated whenever windowsEnabled is true)
    if (windowsEnabled)
    {
        uint16_t winin = hblankRow ? hblankRow->winin : REG_WININ;
        uint16_t winout = hblankRow ? hblankRow->winout : REG_WINOUT;
        uint16_t win0Val = winin & 0x3Fu;
        uint16_t win1Val = (winin >> 8) & 0x3Fu;
        uint16_t winoutVal = (winout & 0x3Fu) | WINMASK_WINOUT;

        if (WIN0enable && ((WIN0left <= WIN0right)
                ? (WIN0left == 0 && WIN0right >= DISPLAY_WIDTH)
                : (WIN0right >= DISPLAY_WIDTH)))
        {
            FillU16(scanline.winMask, win0Val, DISPLAY_WIDTH);
        }
        else if (!WIN0enable && !WIN1enable)
        {
            FillU16(scanline.winMask, winoutVal, DISPLAY_WIDTH);
        }
        else if (WIN0enable && !WIN1enable && WIN0left <= WIN0right)
        {
            unsigned int l = WIN0left < DISPLAY_WIDTH ? WIN0left : DISPLAY_WIDTH;
            unsigned int r = WIN0right < DISPLAY_WIDTH ? WIN0right : DISPLAY_WIDTH;
            if (l > 0)
                FillU16(scanline.winMask, winoutVal, l);
            if (r > l)
                FillU16(&scanline.winMask[l], win0Val, r - l);
            if (r < DISPLAY_WIDTH)
                FillU16(&scanline.winMask[r], winoutVal, DISPLAY_WIDTH - r);
        }
        else
        {
            for (xpos = 0; xpos < DISPLAY_WIDTH; xpos++)
            {
                if (WIN0enable && winCheckHorizontalBounds(WIN0left, WIN0right, xpos))
                    scanline.winMask[xpos] = win0Val;
                else if (WIN1enable && winCheckHorizontalBounds(WIN1left, WIN1right, xpos))
                    scanline.winMask[xpos] = win1Val;
                else
                    scanline.winMask[xpos] = winoutVal;
            }
        }
    }

#ifdef EASY_DRAW_TEST_PARALLEL
    if (objEnabled && (sDisableSpriteScanlinePrefilter || !sSpriteBinsValid || sSpriteBins.count[vcount] > 0))
#else
    if (objEnabled && (!sSpriteBinsValid || sSpriteBins.count[vcount] > 0))
#endif
        DrawSprites(&scanline, vcount, windowsEnabled, hblankRow);

    //iterate trough every priority in order
    for (prnum = 3; prnum >= 0; prnum--)
    {
        int prCount = scanline.prioritySortedBgsCount[prnum];
        for (int prsub = prCount - 1; prsub >= 0; prsub--)
        {
            char bgnum = scanline.prioritySortedBgs[prnum][prsub];
            //if background is enabled then draw it
            if (enabledBgs & (1u << bgnum))
            {
                uint16_t *src = scanline.layers[bgnum];
                bool bgBlended = (blendMode != 0u && (bldcnt & (1u << bgnum)) != 0u);
#ifdef EASY_DRAW_TEST_PARALLEL
                bool useFastBg = !sForceLegacyBgCompositor && !bgBlended;
                bool useOptCompositor = !sForceLegacyBgCompositor;
#else
                bool useFastBg = !bgBlended;
                bool useOptCompositor = true;
#endif
                if (useFastBg)
                {
                    if (!windowsEnabled)
                    {
                        CompositeRowOpaqueFast(pixels, src);
                    }
                    else
                    {
                        uint16_t bgBit = (uint16_t)(1u << bgnum);
                        CompositeRowMaskedFast(pixels, src, scanline.winMask, bgBit);
                    }
                    continue;
                }

                unsigned int activeBlend = bgBlended ? blendMode : 0u;
                if (!windowsEnabled)
                {
                    if (activeBlend == 1u)
                    {
                        bool isSpriteBlendingEnabled = (bldcnt & BLDCNT_TGT2_OBJ) != 0;
                        bool hasBackdropTarget = (bldcnt & BLDCNT_TGT2_BD) != 0;
                        struct BlendTargetEntry blendTargetsList[8];
                        int blendTargetCount = CollectBgBlendTargets(&scanline, prnum, prsub,
                                                                     isSpriteBlendingEnabled,
                                                                     enabledBgs, objEnabled,
                                                                     bldcnt, blendTargetsList);
                        if (useOptCompositor)
                        {
                            if (blendTargetCount == 0)
                            {
                                if (hasBackdropTarget)
                                {
                                    uint16_t tgtB[DISPLAY_WIDTH];
                                    FillU16(tgtB, backdropRaw, DISPLAY_WIDTH);
                                    CompositeRowAlphaFast(pixels, src, tgtB, NULL, NULL, 0u, eva, evb);
                                }
                                else
                                {
                                    CompositeRowOpaqueFast(pixels, src);
                                }
                            }
                            else
                            {
                                uint16_t tgtB[DISPLAY_WIDTH];
                                uint16_t tgtHit[DISPLAY_WIDTH];
                                ResolveTargetBRowsFast(blendTargetsList, blendTargetCount,
                                                       hasBackdropTarget, backdropRaw, tgtB, tgtHit);
                                CompositeRowAlphaFast(pixels, src, tgtB, tgtHit, NULL, 0u, eva, evb);
                            }
                        }
                        else
                        {
                            for (xpos = 0; xpos < DISPLAY_WIDTH; xpos++)
                            {
                                uint16_t color = src[xpos];
                                if (!(color & 0x8000u))
                                    continue;
                                uint16_t targetB = 0;
                                if (SelectPrebuiltTargetB(blendTargetsList, blendTargetCount, (int)xpos,
                                                          hasBackdropTarget, backdropRaw, &targetB))
                                    color = alphaBlendColorWithCoeffs(color, targetB, eva, evb);
                                pixels[xpos] = color;
                            }
                        }
                    }
                    else if (activeBlend == 2u || activeBlend == 3u)
                    {
                        if (useOptCompositor)
                        {
                            CompositeRowBrightnessFast(pixels, src, NULL, 0u, activeBlend == 2u, evy);
                        }
                        else
                        {
                            for (xpos = 0; xpos < DISPLAY_WIDTH; xpos++)
                            {
                                uint16_t color = src[xpos];
                                if (color & 0x8000u)
                                    pixels[xpos] = (activeBlend == 2u)
                                                 ? alphaBrightnessIncreaseWithY(color, evy)
                                                 : alphaBrightnessDecreaseWithY(color, evy);
                            }
                        }
                    }
                    else
                    {
                        for (xpos = 0; xpos < DISPLAY_WIDTH; xpos++)
                        {
                            uint16_t color = src[xpos];
                            if (color & 0x8000u)
                                pixels[xpos] = color;
                        }
                    }
                    continue;
                }

                //copy all pixels to framebuffer with window + blend enabled
                uint16_t bgBit = (uint16_t)(1u << bgnum);
                const uint16_t *winMask = scanline.winMask;
                if (activeBlend == 1u)
                {
                    bool isSpriteBlendingEnabled = (bldcnt & BLDCNT_TGT2_OBJ) != 0;
                    bool hasBackdropTarget = (bldcnt & BLDCNT_TGT2_BD) != 0;
                    struct BlendTargetEntry blendTargetsList[8];
                    int blendTargetCount = CollectBgBlendTargets(&scanline, prnum, prsub,
                                                                 isSpriteBlendingEnabled,
                                                                 enabledBgs, objEnabled,
                                                                 bldcnt, blendTargetsList);
                    if (useOptCompositor)
                    {
                        if (blendTargetCount == 0)
                        {
                            if (hasBackdropTarget)
                            {
                                uint16_t tgtB[DISPLAY_WIDTH];
                                FillU16(tgtB, backdropRaw, DISPLAY_WIDTH);
                                CompositeRowAlphaFast(pixels, src, tgtB, NULL, winMask, bgBit, eva, evb);
                            }
                            else
                            {
                                CompositeRowMaskedFast(pixels, src, winMask, bgBit);
                            }
                        }
                        else
                        {
                            uint16_t tgtB[DISPLAY_WIDTH];
                            uint16_t tgtHit[DISPLAY_WIDTH];
                            ResolveTargetBRowsFast(blendTargetsList, blendTargetCount,
                                                   hasBackdropTarget, backdropRaw, tgtB, tgtHit);
                            CompositeRowAlphaFast(pixels, src, tgtB, tgtHit, winMask, bgBit, eva, evb);
                        }
                    }
                    else
                    {
                        for (xpos = 0; xpos < DISPLAY_WIDTH; xpos++)
                        {
                            uint16_t color = src[xpos];
                            if (!(color & 0x8000u))
                                continue;
                            uint16_t mask = winMask[xpos];
                            if (!(mask & bgBit))
                                continue;
                            if (mask & WINMASK_CLR)
                            {
                                uint16_t targetB = 0;
                                if (SelectPrebuiltTargetB(blendTargetsList, blendTargetCount, (int)xpos,
                                                          hasBackdropTarget, backdropRaw, &targetB))
                                    color = alphaBlendColorWithCoeffs(color, targetB, eva, evb);
                            }
                            pixels[xpos] = color;
                        }
                    }
                }
                else if (activeBlend == 2u || activeBlend == 3u)
                {
                    if (useOptCompositor)
                    {
                        CompositeRowBrightnessFast(pixels, src, winMask, bgBit, activeBlend == 2u, evy);
                    }
                    else
                    {
                        for (xpos = 0; xpos < DISPLAY_WIDTH; xpos++)
                        {
                            uint16_t color = src[xpos];
                            if (!(color & 0x8000u))
                                continue;
                            uint16_t mask = winMask[xpos];
                            if (!(mask & bgBit))
                                continue;
                            if (mask & WINMASK_CLR)
                                color = (activeBlend == 2u)
                                      ? alphaBrightnessIncreaseWithY(color, evy)
                                      : alphaBrightnessDecreaseWithY(color, evy);
                            pixels[xpos] = color;
                        }
                    }
                }
                else
                {
                    for (xpos = 0; xpos < DISPLAY_WIDTH; xpos++)
                    {
                        uint16_t color = src[xpos];
                        if ((color & 0x8000u) && (winMask[xpos] & bgBit))
                            pixels[xpos] = color;
                    }
                }
            }
        }
        // Only composite the horizontal range populated by this priority's sprites.
#ifdef EASY_DRAW_TEST_PARALLEL
        if (sForceFullSpriteCompositor)
            CompositeSpriteLayer(&scanline, pixels, prnum, windowsEnabled, 0, DISPLAY_WIDTH);
        else if (scanline.spritePriorityMask & (1u << prnum))
            CompositeSpriteLayer(&scanline, pixels, prnum, windowsEnabled,
                                 scanline.spriteMinX[prnum], scanline.spriteMaxX[prnum]);
#else
        if (scanline.spritePriorityMask & (1u << prnum))
            CompositeSpriteLayer(&scanline, pixels, prnum, windowsEnabled,
                                 scanline.spriteMinX[prnum], scanline.spriteMaxX[prnum]);
#endif
    }
}

enum WindowRowCoverage {
    WIN_ROW_OUT,
    WIN_ROW_FULL_IN,
    WIN_ROW_SPAN,
    WIN_ROW_WRAP,
};

struct WindowRowSpan {
    enum WindowRowCoverage cov;
    unsigned int l;
    unsigned int r;
};

static inline struct WindowRowSpan ClassifyWindowRow(bool enabled, uint16_t winv, uint16_t winh,
                                                     uint16_t vcount)
{
    struct WindowRowSpan span = { WIN_ROW_OUT, 0u, 0u };
    if (!enabled)
        return span;

    uint16_t top = (winv >> 8) & 0xFFu;
    uint16_t bottom = winv & 0xFFu;
    bool inY = (top > bottom)
        ? (vcount >= top || vcount < bottom)
        : (vcount >= top && vcount < bottom);
    if (!inY)
        return span;

    uint16_t left = (winh >> 8) & 0xFFu;
    uint16_t right = winh & 0xFFu;
    if (left <= right)
    {
        unsigned int l = left < DISPLAY_WIDTH ? left : DISPLAY_WIDTH;
        unsigned int r = right < DISPLAY_WIDTH ? right : DISPLAY_WIDTH;
        if (l >= r)
            return span;
        if (l == 0u && r == DISPLAY_WIDTH)
        {
            span.cov = WIN_ROW_FULL_IN;
            return span;
        }
        span.cov = WIN_ROW_SPAN;
        span.l = l;
        span.r = r;
        return span;
    }

    if (right >= DISPLAY_WIDTH)
    {
        span.cov = WIN_ROW_FULL_IN;
        return span;
    }
    if (right == 0u && left >= DISPLAY_WIDTH)
        return span;
    if (right == 0u)
    {
        span.cov = WIN_ROW_SPAN;
        span.l = left;
        span.r = DISPLAY_WIDTH;
        return span;
    }
    if (left >= DISPLAY_WIDTH)
    {
        span.cov = WIN_ROW_SPAN;
        span.l = 0u;
        span.r = right;
        return span;
    }

    span.cov = WIN_ROW_WRAP;
    span.l = left;
    span.r = right;
    return span;
}

static inline void ApplyBrightnessToOpaquePixels(uint16_t *pixels, unsigned int startX,
                                                unsigned int endX, bool increase, unsigned int evy)
{
    if (evy == 0u || startX >= endX)
        return;

    unsigned int x = startX;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (evy <= 16u && (endX - x) >= 8u)
    {
        uint16x8_t vMask5 = vdupq_n_u16(0x001Fu);
        uint16x8_t v31 = vdupq_n_u16(31u);
        uint16x8_t vEvy = vdupq_n_u16((uint16_t)evy);
        uint16x8_t vAlpha = vdupq_n_u16(0x8000u);

        for (; x + 8u <= endX; x += 8u)
        {
            uint16x8_t orig = vld1q_u16(&pixels[x]);
            uint16x8_t opaque = vtstq_u16(orig, vAlpha);
            uint16x8_t r = vandq_u16(orig, vMask5);
            uint16x8_t g = vandq_u16(vshrq_n_u16(orig, 5), vMask5);
            uint16x8_t b = vandq_u16(vshrq_n_u16(orig, 10), vMask5);

            if (increase)
            {
                r = vaddq_u16(r, vshrq_n_u16(vmulq_u16(vsubq_u16(v31, r), vEvy), 4));
                g = vaddq_u16(g, vshrq_n_u16(vmulq_u16(vsubq_u16(v31, g), vEvy), 4));
                b = vaddq_u16(b, vshrq_n_u16(vmulq_u16(vsubq_u16(v31, b), vEvy), 4));
            }
            else
            {
                r = vsubq_u16(r, vshrq_n_u16(vmulq_u16(r, vEvy), 4));
                g = vsubq_u16(g, vshrq_n_u16(vmulq_u16(g, vEvy), 4));
                b = vsubq_u16(b, vshrq_n_u16(vmulq_u16(b, vEvy), 4));
            }

            uint16x8_t faded = vorrq_u16(vorrq_u16(r, vshlq_n_u16(g, 5)),
                                         vorrq_u16(vshlq_n_u16(b, 10), vAlpha));
            vst1q_u16(&pixels[x], vbslq_u16(opaque, faded, orig));
        }
    }
#endif
    if (increase)
    {
        for (; x < endX; x++)
        {
            uint16_t c = pixels[x];
            if (c & 0x8000u)
                pixels[x] = alphaBrightnessIncreaseWithY(c, evy);
        }
    }
    else
    {
        for (; x < endX; x++)
        {
            uint16_t c = pixels[x];
            if (c & 0x8000u)
                pixels[x] = alphaBrightnessDecreaseWithY(c, evy);
        }
    }
}

static bool TryDrawSingleWindowDirect(uint16_t *pixels, uint16_t vcount,
                                      const struct HBlankRowState *hblankRow,
                                      struct WindowRowSpan span,
                                      uint16_t inMask, uint16_t outMask,
                                      unsigned int activeLayerMask,
                                      unsigned int blendMode, unsigned int blendTargets,
                                      bool baseDirectSupported)
{
    bool inAll = (inMask & activeLayerMask) == activeLayerMask;
    bool inNone = (inMask & activeLayerMask) == 0u;
    bool outAll = (outMask & activeLayerMask) == activeLayerMask;
    bool outNone = (outMask & activeLayerMask) == 0u;
    bool inNoEffects = (blendMode == 0u) || ((inMask & WINMASK_CLR) == 0u) || (blendTargets == 0u);
    bool outNoEffects = (blendMode == 0u) || ((outMask & WINMASK_CLR) == 0u) || (blendTargets == 0u);
    bool uniformBrightnessFade = (blendMode == 2u || blendMode == 3u)
                              && (blendTargets == activeLayerMask)
                              && (activeLayerMask != 0u);
    unsigned int evy = uniformBrightnessFade
                     ? ((hblankRow ? hblankRow->bldy : REG_BLDY) & 0x1Fu)
                     : 0u;
    bool incFade = (blendMode == 2u);

    if (span.cov == WIN_ROW_OUT)
    {
        if (outNone)
            return true;
        if (outAll && baseDirectSupported && (outNoEffects || uniformBrightnessFade))
        {
            DrawScanlineDirect(pixels, vcount, hblankRow);
            if (!outNoEffects)
                ApplyBrightnessToOpaquePixels(pixels, 0u, DISPLAY_WIDTH, incFade, evy);
            return true;
        }
        return false;
    }

    if (span.cov == WIN_ROW_FULL_IN)
    {
        if (inNone)
            return true;
        if (inAll && baseDirectSupported && (inNoEffects || uniformBrightnessFade))
        {
            DrawScanlineDirect(pixels, vcount, hblankRow);
            if (!inNoEffects)
                ApplyBrightnessToOpaquePixels(pixels, 0u, DISPLAY_WIDTH, incFade, evy);
            return true;
        }
        return false;
    }

    if (!baseDirectSupported)
        return false;
    if (inNone && outNone)
        return true;

    unsigned int l = span.l;
    unsigned int r = span.r;
    if (span.cov == WIN_ROW_SPAN)
    {
        if (inAll && outNone && (inNoEffects || uniformBrightnessFade))
        {
            uint16_t rowBackdrop = pixels[0];
            DrawScanlineDirect(pixels, vcount, hblankRow);
            if (!inNoEffects)
                ApplyBrightnessToOpaquePixels(pixels, l, r, incFade, evy);
            if (l > 0u)
                FillU16(pixels, rowBackdrop, l);
            if (r < DISPLAY_WIDTH)
                FillU16(&pixels[r], rowBackdrop, DISPLAY_WIDTH - r);
            return true;
        }
        if (outAll && inNone && (outNoEffects || uniformBrightnessFade))
        {
            uint16_t rowBackdrop = pixels[0];
            DrawScanlineDirect(pixels, vcount, hblankRow);
            if (!outNoEffects)
            {
                ApplyBrightnessToOpaquePixels(pixels, 0u, l, incFade, evy);
                ApplyBrightnessToOpaquePixels(pixels, r, DISPLAY_WIDTH, incFade, evy);
            }
            FillU16(&pixels[l], rowBackdrop, r - l);
            return true;
        }
    }
    else if (span.cov == WIN_ROW_WRAP)
    {
        if (inAll && outNone && (inNoEffects || uniformBrightnessFade))
        {
            uint16_t rowBackdrop = pixels[0];
            DrawScanlineDirect(pixels, vcount, hblankRow);
            if (!inNoEffects)
            {
                ApplyBrightnessToOpaquePixels(pixels, 0u, r, incFade, evy);
                ApplyBrightnessToOpaquePixels(pixels, l, DISPLAY_WIDTH, incFade, evy);
            }
            FillU16(&pixels[r], rowBackdrop, l - r);
            return true;
        }
        if (outAll && inNone && (outNoEffects || uniformBrightnessFade))
        {
            uint16_t rowBackdrop = pixels[0];
            DrawScanlineDirect(pixels, vcount, hblankRow);
            if (!outNoEffects)
                ApplyBrightnessToOpaquePixels(pixels, r, l, incFade, evy);
            if (r > 0u)
                FillU16(pixels, rowBackdrop, r);
            if (l < DISPLAY_WIDTH)
                FillU16(&pixels[l], rowBackdrop, DISPLAY_WIDTH - l);
            return true;
        }
    }

    return false;
}

static bool TryDrawDirectOrderedBrightness(uint16_t *pixels, uint16_t vcount,
                                           const struct HBlankRowState *hblankRow,
                                           unsigned int dispcnt, unsigned int bldcnt,
                                           bool incFade, unsigned int evy)
{
    unsigned int mode = dispcnt & 3u;
    unsigned int numOfBgs = (mode == 0u) ? 4u : 3u;
    unsigned int enabledBgs = (dispcnt >> 8) & ((1u << numOfBgs) - 1u);
    bool objEnabled = (dispcnt & DISPCNT_OBJ_ON) != 0;
    uint16_t bgcnts[4] = { 0, 0, 0, 0 };
    uint8_t bgPriorityMask = 0;

    for (unsigned int bgnum = 0; bgnum < numOfBgs; bgnum++)
    {
        if (enabledBgs & (1u << bgnum))
        {
            uint16_t bgcnt = *(uint16_t *)(REG_ADDR_BG0CNT + bgnum * 2);
            bgcnts[bgnum] = bgcnt;
            bgPriorityMask |= (uint8_t)(1u << (bgcnt & 3u));
        }
    }
    uint8_t objPriorityMask = objEnabled ? sSpriteBins.priorityMask[vcount] : 0;

    bool sawNonTgt1 = false;
    bool hasTgt1 = false;
    for (int prnum = 3; prnum >= 0; prnum--)
    {
        if (bgPriorityMask & (1u << prnum))
        {
            for (int bgnum = (int)numOfBgs - 1; bgnum >= 0; bgnum--)
            {
                if ((enabledBgs & (1u << bgnum)) && (int)(bgcnts[bgnum] & 3u) == prnum)
                {
                    if (bldcnt & (1u << bgnum))
                    {
                        if (sawNonTgt1)
                            return false;
                        hasTgt1 = true;
                    }
                    else
                    {
                        sawNonTgt1 = true;
                    }
                }
            }
        }
        if (objPriorityMask & (1u << prnum))
        {
            if (bldcnt & BLDCNT_TGT1_OBJ)
            {
                if (sawNonTgt1)
                    return false;
                hasTgt1 = true;
            }
            else
            {
                sawNonTgt1 = true;
            }
        }
    }

    for (int phase = 0; phase < 2; phase++)
    {
        if (phase == 0 && !hasTgt1)
            continue;
        if (phase == 1 && !sawNonTgt1)
            break;

        bool wantTgt1 = (phase == 0);
        for (int prnum = 3; prnum >= 0; prnum--)
        {
            if (bgPriorityMask & (1u << prnum))
            {
                for (int bgnum = (int)numOfBgs - 1; bgnum >= 0; bgnum--)
                {
                    if ((enabledBgs & (1u << bgnum)) && (int)(bgcnts[bgnum] & 3u) == prnum)
                    {
                        bool isTgt1 = (bldcnt & (1u << bgnum)) != 0u;
                        if (isTgt1 != wantTgt1)
                            continue;
                        if (mode == 1u && bgnum == 2)
                        {
                            RenderRotScaleBGScanline(2, bgcnts[2], REG_BG2X, REG_BG2Y, vcount, pixels);
                        }
                        else
                        {
                            uint16_t bghoffs = hblankRow
                                ? hblankRow->bgHofs[bgnum]
                                : *(uint16_t *)(REG_ADDR_BG0HOFS + bgnum * 4);
                            uint16_t bgvoffs = hblankRow
                                ? hblankRow->bgVofs[bgnum]
                                : *(uint16_t *)(REG_ADDR_BG0VOFS + bgnum * 4);
                            RenderBGScanline(bgnum, bgcnts[bgnum], bghoffs, bgvoffs, vcount, pixels);
                        }
                    }
                }
            }
            if ((objPriorityMask & (1u << prnum)) && (((bldcnt & BLDCNT_TGT1_OBJ) != 0u) == wantTgt1))
                DrawSpritesDirectForPriority(pixels, vcount, (unsigned int)prnum);
        }

        if (phase == 0)
            ApplyBrightnessToOpaquePixels(pixels, 0u, DISPLAY_WIDTH, incFade, evy);
    }
    return true;
}

static bool TryDrawDirectAlphaRow(uint16_t *pixels, uint16_t vcount,
                                  const struct HBlankRowState *hblankRow,
                                  unsigned int dispcnt, unsigned int bldcnt)
{
    unsigned int mode = dispcnt & 3u;
    unsigned int numOfBgs = (mode == 0u) ? 4u : 3u;
    unsigned int enabledBgs = (dispcnt >> 8) & ((1u << numOfBgs) - 1u);
    bool objEnabled = (dispcnt & DISPCNT_OBJ_ON) != 0;
    if (objEnabled && (bldcnt & BLDCNT_TGT1_OBJ))
        return false;

    uint16_t bgcnts[4] = { 0, 0, 0, 0 };
    uint8_t bgPriorityMask = 0;
    for (unsigned int bgnum = 0; bgnum < numOfBgs; bgnum++)
    {
        if (enabledBgs & (1u << bgnum))
        {
            uint16_t bgcnt = *(uint16_t *)(REG_ADDR_BG0CNT + bgnum * 2);
            bgcnts[bgnum] = bgcnt;
            bgPriorityMask |= (uint8_t)(1u << (bgcnt & 3u));
        }
    }
    if (HasSamePriorityNonTarget2Hazard(numOfBgs, enabledBgs, bgcnts, bldcnt))
        return false;

    uint8_t objPriorityMask = objEnabled ? sSpriteBins.priorityMask[vcount] : 0;
    bool allActiveTgt2 = ((bldcnt & BLDCNT_TGT2_BD) != 0u)
                      && ((~((bldcnt >> 8) & 0xFu) & enabledBgs) == 0u)
                      && (!objEnabled || (bldcnt & BLDCNT_TGT2_OBJ) != 0u);
    unsigned int bldalpha = hblankRow ? hblankRow->bldalpha : REG_BLDALPHA;
    unsigned int eva = bldalpha & 0x1Fu;
    unsigned int evb = (bldalpha >> 8) & 0x1Fu;

    uint16_t tmpRow[DISPLAY_WIDTH];
    uint16_t tgt2Mask[DISPLAY_WIDTH];
    if (!allActiveTgt2)
        FillU16(tgt2Mask, (bldcnt & BLDCNT_TGT2_BD) ? 0xFFFFu : 0u, DISPLAY_WIDTH);

    for (int prnum = 3; prnum >= 0; prnum--)
    {
        if (bgPriorityMask & (1u << prnum))
        {
            for (int bgnum = (int)numOfBgs - 1; bgnum >= 0; bgnum--)
            {
                if (!((enabledBgs & (1u << bgnum)) && (int)(bgcnts[bgnum] & 3u) == prnum))
                    continue;
                bool bgBlended = (bldcnt & (1u << bgnum)) != 0u;
                bool bgIsTgt2 = (bldcnt & (1u << (8u + bgnum))) != 0u;

                if (!bgBlended && allActiveTgt2)
                {
                    if (mode == 1u && bgnum == 2)
                        RenderRotScaleBGScanline(2, bgcnts[2], REG_BG2X, REG_BG2Y, vcount, pixels);
                    else
                    {
                        uint16_t bghoffs = hblankRow ? hblankRow->bgHofs[bgnum] : *(uint16_t *)(REG_ADDR_BG0HOFS + bgnum * 4);
                        uint16_t bgvoffs = hblankRow ? hblankRow->bgVofs[bgnum] : *(uint16_t *)(REG_ADDR_BG0VOFS + bgnum * 4);
                        RenderBGScanline(bgnum, bgcnts[bgnum], bghoffs, bgvoffs, vcount, pixels);
                    }
                    continue;
                }

                ZeroU32(tmpRow, sizeof(tmpRow));
                if (mode == 1u && bgnum == 2)
                    RenderRotScaleBGScanline(2, bgcnts[2], REG_BG2X, REG_BG2Y, vcount, tmpRow);
                else
                {
                    uint16_t bghoffs = hblankRow ? hblankRow->bgHofs[bgnum] : *(uint16_t *)(REG_ADDR_BG0HOFS + bgnum * 4);
                    uint16_t bgvoffs = hblankRow ? hblankRow->bgVofs[bgnum] : *(uint16_t *)(REG_ADDR_BG0VOFS + bgnum * 4);
                    RenderBGScanline(bgnum, bgcnts[bgnum], bghoffs, bgvoffs, vcount, tmpRow);
                }

                if (bgBlended)
                {
                    CompositeRowAlphaFast(pixels, tmpRow, NULL, allActiveTgt2 ? NULL : tgt2Mask,
                                          NULL, 0u, eva, evb);
                }
                else
                {
                    CompositeRowOpaqueFast(pixels, tmpRow);
                }
                if (!allActiveTgt2)
                    UpdateTgt2MaskFromRow(tgt2Mask, tmpRow, bgIsTgt2 ? 0xFFFFu : 0u);
            }
        }

        if (objPriorityMask & (1u << prnum))
        {
            if (allActiveTgt2)
            {
                DrawSpritesDirectForPriority(pixels, vcount, (unsigned int)prnum);
            }
            else
            {
                ZeroU32(tmpRow, sizeof(tmpRow));
                DrawSpritesDirectForPriority(tmpRow, vcount, (unsigned int)prnum);
                CompositeRowOpaqueFast(pixels, tmpRow);
                UpdateTgt2MaskFromRow(tgt2Mask, tmpRow, (bldcnt & BLDCNT_TGT2_OBJ) ? 0xFFFFu : 0u);
            }
        }
    }
    return true;
}

static bool TryDrawDirectScanlineWithRegs(uint16_t *pixels, uint16_t vcount,
                                          const struct HBlankRowState *hblankRow,
                                          unsigned int dispcnt, unsigned int bldcnt)
{
    unsigned int mode = dispcnt & 3u;
    unsigned int numOfBgs = (mode == 0u) ? 4u : 3u;
    unsigned int enabledBgs = (dispcnt >> 8) & ((1u << numOfBgs) - 1u);
    bool objEnabled = (dispcnt & DISPCNT_OBJ_ON) != 0;
    bool rowHasSprites = objEnabled
                      && (!sSpriteBinsValid || !sSpritePriorityMaskSafe || sSpriteBins.count[vcount] > 0);
    unsigned int activeLayerMask = enabledBgs | (rowHasSprites ? WINMASK_OBJ : 0u);
    unsigned int blendMode = (bldcnt >> 6) & 3u;
    unsigned int blendTargets = bldcnt & (enabledBgs | (rowHasSprites ? BLDCNT_TGT1_OBJ : 0u));

    if (blendMode == 0u || blendTargets == 0u)
    {
        DrawScanlineDirectWithDispcnt(pixels, vcount, hblankRow, dispcnt);
        return true;
    }
    if (blendMode == 2u || blendMode == 3u)
    {
        unsigned int evy = (hblankRow ? hblankRow->bldy : REG_BLDY) & 0x1Fu;
        if (blendTargets == activeLayerMask && activeLayerMask != 0u)
        {
            DrawScanlineDirectWithDispcnt(pixels, vcount, hblankRow, dispcnt);
            ApplyBrightnessToOpaquePixels(pixels, 0u, DISPLAY_WIDTH, blendMode == 2u, evy);
            return true;
        }
        return TryDrawDirectOrderedBrightness(pixels, vcount, hblankRow, dispcnt, bldcnt, blendMode == 2u, evy);
    }
    if (blendMode == 1u)
        return TryDrawDirectAlphaRow(pixels, vcount, hblankRow, dispcnt, bldcnt);
    return false;
}

static bool TryDrawUniformWindowDirect(uint16_t *pixels, uint16_t vcount,
                                       const struct HBlankRowState *hblankRow,
                                       unsigned int baseDispcnt, unsigned int baseBldcnt,
                                       uint16_t mask, bool baseDirectSupported)
{
    if (!baseDirectSupported)
        return false;
    unsigned int mode = baseDispcnt & 3u;
    unsigned int bgMaskBits = (mode == 0u) ? 0x0Fu : 0x07u;
    unsigned int enabledLayers = (baseDispcnt >> 8) & (bgMaskBits | WINMASK_OBJ);
    unsigned int visibleLayers = (unsigned int)mask & enabledLayers;
    unsigned int maskedDispcnt = (baseDispcnt & ~0x1F00u) | (visibleLayers << 8);
    unsigned int maskedBldcnt = (mask & WINMASK_CLR) ? baseBldcnt : 0u;
    return TryDrawDirectScanlineWithRegs(pixels, vcount, hblankRow, maskedDispcnt, maskedBldcnt);
}

static void DrawScanlineFallbackNoLayers(uint16_t *pixels, uint16_t vcount,
                                         const struct HBlankRowState *hblankRow,
                                         unsigned int dispcnt, unsigned int bldcnt,
                                         bool objEnabled, struct WindowRowSpan w0,
                                         struct WindowRowSpan w1, uint16_t winin0,
                                         uint16_t winin1, uint16_t winoutVal)
{
    unsigned int mode = dispcnt & 3u;
    unsigned int numOfBgs = (mode == 0u) ? 4u : 3u;
    unsigned int enabledBgs = (dispcnt >> 8) & ((1u << numOfBgs) - 1u);
    unsigned int blendMode = (bldcnt >> 6) & 3u;
    unsigned int evy = (hblankRow ? hblankRow->bldy : REG_BLDY) & 0x1Fu;

    uint16_t bgcnts[4] = { 0, 0, 0, 0 };
    uint8_t bgPriorityMask = 0;
    for (unsigned int bgnum = 0; bgnum < numOfBgs; bgnum++)
    {
        if (enabledBgs & (1u << bgnum))
        {
            uint16_t bgcnt = *(uint16_t *)(REG_ADDR_BG0CNT + bgnum * 2);
            bgcnts[bgnum] = bgcnt;
            bgPriorityMask |= (uint8_t)(1u << (bgcnt & 3u));
        }
    }

    uint16_t winMask[DISPLAY_WIDTH];
    uint16_t winUnion = winoutVal;
    uint16_t winCommon = winoutVal;
    if (w0.cov == WIN_ROW_FULL_IN)
    {
        FillU16(winMask, winin0, DISPLAY_WIDTH);
        winUnion = winin0;
        winCommon = winin0;
    }
    else if (w0.cov == WIN_ROW_SPAN && w1.cov == WIN_ROW_OUT)
    {
        if (w0.l > 0u)
            FillU16(winMask, winoutVal, w0.l);
        if (w0.r > w0.l)
            FillU16(&winMask[w0.l], winin0, w0.r - w0.l);
        if (w0.r < DISPLAY_WIDTH)
            FillU16(&winMask[w0.r], winoutVal, DISPLAY_WIDTH - w0.r);
        winUnion = winin0 | winoutVal;
        winCommon = winin0 & winoutVal;
    }
    else if (w0.cov == WIN_ROW_WRAP && w1.cov == WIN_ROW_OUT)
    {
        if (w0.r > 0u)
            FillU16(winMask, winin0, w0.r);
        if (w0.l > w0.r)
            FillU16(&winMask[w0.r], winoutVal, w0.l - w0.r);
        if (w0.l < DISPLAY_WIDTH)
            FillU16(&winMask[w0.l], winin0, DISPLAY_WIDTH - w0.l);
        winUnion = winin0 | winoutVal;
        winCommon = winin0 & winoutVal;
    }
    else
    {
        winUnion = winoutVal | ((w0.cov != WIN_ROW_OUT) ? winin0 : 0u) | ((w1.cov != WIN_ROW_OUT) ? winin1 : 0u);
        winCommon = winoutVal & ((w0.cov != WIN_ROW_OUT) ? winin0 : 0x3Fu) & ((w1.cov != WIN_ROW_OUT) ? winin1 : 0x3Fu);
        bool win0Enable = (dispcnt & DISPCNT_WIN0_ON) != 0 && (w0.cov != WIN_ROW_OUT);
        bool win1Enable = (dispcnt & DISPCNT_WIN1_ON) != 0 && (w1.cov != WIN_ROW_OUT);
        uint16_t win0h = hblankRow ? hblankRow->win0h : REG_WIN0H;
        uint16_t win1h = hblankRow ? hblankRow->win1h : REG_WIN1H;
        uint16_t win0L = (win0h >> 8) & 0xFFu;
        uint16_t win0R = win0h & 0xFFu;
        uint16_t win1L = (win1h >> 8) & 0xFFu;
        uint16_t win1R = win1h & 0xFFu;
        for (unsigned int xpos = 0; xpos < DISPLAY_WIDTH; xpos++)
        {
            if (win0Enable && winCheckHorizontalBounds(win0L, win0R, (u16)xpos))
                winMask[xpos] = winin0;
            else if (win1Enable && winCheckHorizontalBounds(win1L, win1R, (u16)xpos))
                winMask[xpos] = winin1;
            else
                winMask[xpos] = winoutVal;
        }
    }

    uint8_t objPriorityMask = (objEnabled && (winUnion & WINMASK_OBJ)) ? sSpriteBins.priorityMask[vcount] : 0;
    uint16_t tmpRow[DISPLAY_WIDTH];

    for (int prnum = 3; prnum >= 0; prnum--)
    {
        if (bgPriorityMask & (1u << prnum))
        {
            for (int bgnum = (int)numOfBgs - 1; bgnum >= 0; bgnum--)
            {
                if (!((enabledBgs & (1u << bgnum)) && (int)(bgcnts[bgnum] & 3u) == prnum))
                    continue;
                uint16_t bgBit = (uint16_t)(1u << bgnum);
                if (!(winUnion & bgBit))
                    continue;
                bool alwaysVisible = (winCommon & bgBit) != 0u;
                unsigned int activeBlend = (blendMode != 0u && (bldcnt & bgBit) != 0u) ? blendMode : 0u;

                if (alwaysVisible && activeBlend == 0u)
                {
                    if (mode == 1u && bgnum == 2)
                        RenderRotScaleBGScanline(2, bgcnts[2], REG_BG2X, REG_BG2Y, vcount, pixels);
                    else
                    {
                        uint16_t bghoffs = hblankRow ? hblankRow->bgHofs[bgnum] : *(uint16_t *)(REG_ADDR_BG0HOFS + bgnum * 4);
                        uint16_t bgvoffs = hblankRow ? hblankRow->bgVofs[bgnum] : *(uint16_t *)(REG_ADDR_BG0VOFS + bgnum * 4);
                        RenderBGScanline(bgnum, bgcnts[bgnum], bghoffs, bgvoffs, vcount, pixels);
                    }
                    continue;
                }

                ZeroU32(tmpRow, sizeof(tmpRow));
                if (mode == 1u && bgnum == 2)
                    RenderRotScaleBGScanline(2, bgcnts[2], REG_BG2X, REG_BG2Y, vcount, tmpRow);
                else
                {
                    uint16_t bghoffs = hblankRow ? hblankRow->bgHofs[bgnum] : *(uint16_t *)(REG_ADDR_BG0HOFS + bgnum * 4);
                    uint16_t bgvoffs = hblankRow ? hblankRow->bgVofs[bgnum] : *(uint16_t *)(REG_ADDR_BG0VOFS + bgnum * 4);
                    RenderBGScanline(bgnum, bgcnts[bgnum], bghoffs, bgvoffs, vcount, tmpRow);
                }

                if (activeBlend == 2u || activeBlend == 3u)
                    CompositeRowBrightnessFast(pixels, tmpRow, alwaysVisible && (winCommon & WINMASK_CLR) ? NULL : winMask,
                                               bgBit, activeBlend == 2u, evy);
                else
                    CompositeRowMaskedFast(pixels, tmpRow, winMask, bgBit);
            }
        }

        if (objPriorityMask & (1u << prnum))
        {
            bool objAlwaysVisible = (winCommon & WINMASK_OBJ) != 0u;
            unsigned int objBlend = (blendMode != 0u && (bldcnt & BLDCNT_TGT1_OBJ) != 0u) ? blendMode : 0u;
            if (objAlwaysVisible && objBlend == 0u)
            {
                DrawSpritesDirectForPriority(pixels, vcount, (unsigned int)prnum);
            }
            else
            {
                ZeroU32(tmpRow, sizeof(tmpRow));
                DrawSpritesDirectForPriority(tmpRow, vcount, (unsigned int)prnum);
                if (objBlend == 2u || objBlend == 3u)
                    CompositeRowBrightnessFast(pixels, tmpRow, objAlwaysVisible && (winCommon & WINMASK_CLR) ? NULL : winMask,
                                               WINMASK_OBJ, objBlend == 2u, evy);
                else
                    CompositeRowMaskedFast(pixels, tmpRow, winMask, WINMASK_OBJ);
            }
        }
    }
}

static void DrawScanline(uint16_t *pixels, uint16_t vcount,
                         const struct HBlankRowState *hblankRow)
{
    unsigned int dispcnt = REG_DISPCNT & ~(unsigned int)gFastDrawDispcntClear;
    unsigned int mode = dispcnt & 3u;
    unsigned int bldcnt = (hblankRow ? hblankRow->bldcnt : REG_BLDCNT) & ~(unsigned int)gFastDrawBldcntClear;
    unsigned int blendMode = (bldcnt >> 6) & 3u;
    bool objEnabled = (dispcnt & DISPCNT_OBJ_ON) != 0;
    unsigned int numOfBgs = (mode == 0u) ? 4u : 3u;
    unsigned int enabledBgs = (dispcnt >> 8) & ((1u << numOfBgs) - 1u);
    bool baseDirectSupported = mode <= 1u
                            && !(mode == 1u && (dispcnt & (1u << 10)) != 0 && (REG_BG2CNT & BGCNT_MOSAIC) != 0 && (REG_MOSAIC & 0xFFu) != 0u)
                            && (!objEnabled || (sSpriteBinsValid && sSpritePriorityMaskSafe && !sSpriteBins.hasDirectUnsupported[vcount]));
#ifdef EASY_DRAW_TEST_PARALLEL
    if (sForceFullSpriteCompositor || sForceLegacyBgCompositor || sForceLegacyLayerClears
     || sDisableSpriteScanlinePrefilter || sForceLegacySpritePixelPath || sForceLegacySimpleSpritePath)
        baseDirectSupported = false;
#endif
    bool rowHasSprites = objEnabled
                      && (!sSpriteBinsValid || !sSpritePriorityMaskSafe || sSpriteBins.count[vcount] > 0);
#ifdef EASY_DRAW_TEST_PARALLEL
    if (objEnabled && sDisableSpriteScanlinePrefilter)
        rowHasSprites = true;
#endif
    unsigned int activeLayerMask = enabledBgs | (rowHasSprites ? WINMASK_OBJ : 0u);
    unsigned int blendTargets = bldcnt & (enabledBgs | (rowHasSprites ? BLDCNT_TGT1_OBJ : 0u));
    bool windowsPossible = (dispcnt & (DISPCNT_WIN0_ON | DISPCNT_WIN1_ON)) != 0
                        || ((dispcnt & DISPCNT_OBJWIN_ON) != 0 && objEnabled);

    if (!windowsPossible)
    {
        if (baseDirectSupported && TryDrawDirectScanlineWithRegs(pixels, vcount, hblankRow, dispcnt, bldcnt))
            return;
        DrawScanlineFallback(pixels, vcount, hblankRow);
        return;
    }

    bool objWinOnRow = ((dispcnt & DISPCNT_OBJWIN_ON) != 0 && rowHasSprites);
    if (objWinOnRow && sSpriteBinsValid && sSpritePriorityMaskSafe && !sSpriteBins.hasDirectUnsupported[vcount])
        objWinOnRow = false;
#ifdef EASY_DRAW_TEST_PARALLEL
    if (((dispcnt & DISPCNT_OBJWIN_ON) != 0 && objEnabled) && sDisableSpriteScanlinePrefilter)
        objWinOnRow = true;
#endif

    if (!objWinOnRow)
    {
        uint16_t wininReg = hblankRow ? hblankRow->winin : REG_WININ;
        uint16_t winin0 = wininReg & 0x3Fu;
        uint16_t winin1 = (wininReg >> 8) & 0x3Fu;
        uint16_t winoutVal = (hblankRow ? hblankRow->winout : REG_WINOUT) & 0x3Fu;

        struct WindowRowSpan w0 = ClassifyWindowRow((dispcnt & DISPCNT_WIN0_ON) != 0,
                                                    hblankRow ? hblankRow->win0v : REG_WIN0V,
                                                    hblankRow ? hblankRow->win0h : REG_WIN0H,
                                                    vcount);
        struct WindowRowSpan w1 = ClassifyWindowRow((dispcnt & DISPCNT_WIN1_ON) != 0,
                                                    hblankRow ? hblankRow->win1v : REG_WIN1V,
                                                    hblankRow ? hblankRow->win1h : REG_WIN1H,
                                                    vcount);
        unsigned int winCompareMask = activeLayerMask | ((blendMode != 0u && blendTargets != 0u) ? WINMASK_CLR : 0u);

        if (w0.cov == WIN_ROW_FULL_IN)
        {
            if (TryDrawUniformWindowDirect(pixels, vcount, hblankRow, dispcnt, bldcnt, winin0, baseDirectSupported))
                return;
        }
        else if (w0.cov == WIN_ROW_OUT)
        {
            if (w1.cov == WIN_ROW_FULL_IN)
            {
                if (TryDrawUniformWindowDirect(pixels, vcount, hblankRow, dispcnt, bldcnt, winin1, baseDirectSupported))
                    return;
            }
            else if (w1.cov == WIN_ROW_OUT)
            {
                if (TryDrawUniformWindowDirect(pixels, vcount, hblankRow, dispcnt, bldcnt, winoutVal, baseDirectSupported))
                    return;
            }
            else if (TryDrawSingleWindowDirect(pixels, vcount, hblankRow, w1, winin1, winoutVal,
                                               activeLayerMask, blendMode, blendTargets, baseDirectSupported))
            {
                return;
            }
        }
        else if (w1.cov == WIN_ROW_OUT || ((winin1 & winCompareMask) == (winoutVal & winCompareMask)))
        {
            if ((winin0 & winCompareMask) == (winoutVal & winCompareMask))
            {
                if (TryDrawUniformWindowDirect(pixels, vcount, hblankRow, dispcnt, bldcnt, winin0, baseDirectSupported))
                    return;
            }
            if (TryDrawSingleWindowDirect(pixels, vcount, hblankRow, w0, winin0, winoutVal,
                                          activeLayerMask, blendMode, blendTargets, baseDirectSupported))
                return;
        }
        else if (w1.cov == WIN_ROW_FULL_IN)
        {
            if ((winin0 & winCompareMask) == (winin1 & winCompareMask))
            {
                if (TryDrawUniformWindowDirect(pixels, vcount, hblankRow, dispcnt, bldcnt, winin0, baseDirectSupported))
                    return;
            }
            if (TryDrawSingleWindowDirect(pixels, vcount, hblankRow, w0, winin0, winin1,
                                          activeLayerMask, blendMode, blendTargets, baseDirectSupported))
                return;
        }

        if (baseDirectSupported && blendMode != 1u)
        {
            DrawScanlineFallbackNoLayers(pixels, vcount, hblankRow, dispcnt, bldcnt,
                                         objEnabled, w0, w1, winin0, winin1, winoutVal);
            return;
        }
    }

    DrawScanlineFallback(pixels, vcount, hblankRow);
}

uint16_t *memsetu16(uint16_t *dst, uint16_t fill, size_t count)
{
    FillU16(dst, fill, count);
    return dst;
}

static uint16_t ComputeBackdropColor(unsigned int bldcnt, unsigned int blendMode, unsigned int evy)
{
    if (gFastDrawBackdropOverride >= 0)
        return (uint16_t)(gFastDrawBackdropOverride & 0x7FFFu);

    uint16_t backdropColor = *(uint16_t *)PLTT;
    bldcnt &= ~(unsigned int)gFastDrawBldcntClear;
    if (bldcnt & BLDCNT_TGT1_BD)
    {
        switch (blendMode)
        {
        case 2:
            backdropColor = alphaBrightnessIncreaseWithY(backdropColor, evy);
            break;
        case 3:
            backdropColor = alphaBrightnessDecreaseWithY(backdropColor, evy);
            break;
        }
    }
    return backdropColor & 0x7FFFu;
}

static void DrawFrameSerialFrom(uint16_t *pixels, int firstLine)
{
    int i;
    for (i = firstLine; i < DISPLAY_HEIGHT; i++)
    {
        REG_VCOUNT = i;
        if(((REG_DISPSTAT >> 8) & 0xFF) == REG_VCOUNT)
        {
            REG_DISPSTAT |= INTR_FLAG_VCOUNT;
#ifdef __ANDROID__
            if (REG_IE & INTR_FLAG_VCOUNT)
#else
            if (REG_DISPSTAT & DISPSTAT_VCOUNT_INTR)
#endif
                    gIntrTable[0]();
        }

        // Render the backdrop color before the each individual scanline.
        unsigned int bldcnt = REG_BLDCNT & ~(unsigned int)gFastDrawBldcntClear;
        uint16_t backdropColor = ComputeBackdropColor(bldcnt, (bldcnt >> 6) & 3u, REG_BLDY & 0x1Fu);

        memsetu16(&pixels[i * DISPLAY_WIDTH], backdropColor & 0x7FFF, DISPLAY_WIDTH);
        DrawScanline(&pixels[i * DISPLAY_WIDTH], i, NULL);
        
        REG_DISPSTAT |= INTR_FLAG_HBLANK;

        RunDMAs(DMA_HBLANK);
        
        if (REG_DISPSTAT & DISPSTAT_HBLANK_INTR)
            gIntrTable[3]();

        REG_DISPSTAT &= ~INTR_FLAG_HBLANK;
        REG_DISPSTAT &= ~INTR_FLAG_VCOUNT;
    }
}

static void DrawFrameSerial(uint16_t *pixels)
{
    if (!sSpriteBinsValid)
    {
        if ((REG_DISPCNT & ~(unsigned int)gFastDrawDispcntClear) & DISPCNT_OBJ_ON)
        {
            BuildSpriteScanlineBins(&sSpriteBins);
            sSpriteBinsValid = true;
            sSpritePriorityMaskSafe = CanUseSpritePriorityMask();
        }
        else
        {
            sSpriteBinsValid = false;
            sSpritePriorityMaskSafe = false;
        }
    }
    DrawFrameSerialFrom(pixels, 0);
    sSpriteBinsValid = false;
}

static bool HasHBlankScanlineEvents(void)
{
    if ((REG_DISPSTAT & DISPSTAT_HBLANK_INTR) != 0)
        return true;

    return (REG_DMA0CNT_H & (DMA_ENABLE | DMA_START_MASK)) == (DMA_ENABLE | DMA_START_HBLANK)
        || (REG_DMA1CNT_H & (DMA_ENABLE | DMA_START_MASK)) == (DMA_ENABLE | DMA_START_HBLANK)
        || (REG_DMA2CNT_H & (DMA_ENABLE | DMA_START_MASK)) == (DMA_ENABLE | DMA_START_HBLANK)
        || (REG_DMA3CNT_H & (DMA_ENABLE | DMA_START_MASK)) == (DMA_ENABLE | DMA_START_HBLANK);
}

#ifdef PLATFORM_VITA
static enum VitaEasyDrawFallbackReason GetEasyDrawFallbackReason(void)
{
    if ((REG_DISPSTAT & DISPSTAT_HBLANK_INTR) != 0)
        return VITA_EASY_DRAW_FALLBACK_HBLANK_IRQ;

    if ((REG_DMA0CNT_H & (DMA_ENABLE | DMA_START_MASK)) == (DMA_ENABLE | DMA_START_HBLANK)
     || (REG_DMA1CNT_H & (DMA_ENABLE | DMA_START_MASK)) == (DMA_ENABLE | DMA_START_HBLANK)
     || (REG_DMA2CNT_H & (DMA_ENABLE | DMA_START_MASK)) == (DMA_ENABLE | DMA_START_HBLANK)
     || (REG_DMA3CNT_H & (DMA_ENABLE | DMA_START_MASK)) == (DMA_ENABLE | DMA_START_HBLANK))
        return VITA_EASY_DRAW_FALLBACK_HBLANK_DMA;

    int compare = (REG_DISPSTAT >> 8) & 0xFF;
    if ((REG_DISPSTAT & DISPSTAT_VCOUNT_INTR) != 0 && compare < DISPLAY_HEIGHT)
        return VITA_EASY_DRAW_FALLBACK_EARLY_VCOUNT;

    return VITA_EASY_DRAW_FALLBACK_OTHER;
}
#endif

static bool IsEarlyVCountSafeForDeferredSync(uint16_t dispstat)
{
    int compare = (dispstat >> 8) & 0xFF;
    if ((dispstat & DISPSTAT_VCOUNT_INTR) == 0 || compare >= DISPLAY_HEIGHT)
        return true;

#if defined(PLATFORM_VITA) && !defined(EASY_DRAW_TEST_PARALLEL)
    return gMain.vcountCallback == NULL;
#else
    return false;
#endif
}

static bool CanDrawScanlinesInParallel(bool *soundSyncDeferred)
{
    if (soundSyncDeferred)
        *soundSyncDeferred = false;

    /* HBlank-triggered DMA and interrupts make scanlines order-dependent. */
    if (HasHBlankScanlineEvents())
        return false;

    u16 dispstat = REG_DISPSTAT;
    if ((dispstat & DISPSTAT_VCOUNT_INTR) != 0 && ((dispstat >> 8) & 0xFF) < DISPLAY_HEIGHT)
    {
        if (IsEarlyVCountSafeForDeferredSync(dispstat))
        {
            if (soundSyncDeferred)
                *soundSyncDeferred = true;
            return true;
        }
        return false;
    }

    return true;
}

static bool CanDrawPrefixBeforeVCountInParallel(int *vcountCompare)
{
    u16 dispstat = REG_DISPSTAT;
    int compare = (dispstat >> 8) & 0xFF;

    /* Keep at least half the frame on workers to amortize dispatch and join. */
    if (HasHBlankScanlineEvents()
     || (dispstat & DISPSTAT_VCOUNT_INTR) == 0
     || compare < DISPLAY_HEIGHT / 2
     || compare >= DISPLAY_HEIGHT)
        return false;

    *vcountCompare = compare;
    return true;
}

static bool IsSafeHBlankDmaTarget(const void *dst, bool is32Bit)
{
    if (!is32Bit)
    {
        return dst == (const void *)&REG_WIN0H
            || dst == (const void *)&REG_WIN1H
            || dst == (const void *)&REG_WIN0V
            || dst == (const void *)&REG_WIN1V
            || dst == (const void *)&REG_WININ
            || dst == (const void *)&REG_WINOUT
            || dst == (const void *)&REG_BG0HOFS
            || dst == (const void *)&REG_BG0VOFS
            || dst == (const void *)&REG_BG1HOFS
            || dst == (const void *)&REG_BG1VOFS
            || dst == (const void *)&REG_BG2HOFS
            || dst == (const void *)&REG_BG2VOFS
            || dst == (const void *)&REG_BG3HOFS
            || dst == (const void *)&REG_BG3VOFS
            || dst == (const void *)&REG_BLDCNT
            || dst == (const void *)&REG_BLDALPHA
            || dst == (const void *)&REG_BLDY;
    }

    return dst == (const void *)&REG_BG0HOFS
        || dst == (const void *)&REG_BG1HOFS
        || dst == (const void *)&REG_BG2HOFS
        || dst == (const void *)&REG_BG3HOFS
        || dst == (const void *)&REG_WIN0H
        || dst == (const void *)&REG_WIN0V
        || dst == (const void *)&REG_WININ
        || dst == (const void *)&REG_BLDCNT;
}

static bool CheckSafeHBlankReplayInternal(const uint16_t *pixels)
{
    u16 dispstat = REG_DISPSTAT;
    bool hasAnyEvent = false;

    if (dispstat & DISPSTAT_HBLANK_INTR)
    {
#if defined(PLATFORM_VITA) && !defined(EASY_DRAW_TEST_PARALLEL)
        if (gMain.hblankCallback != NULL
         && !BattleTransition_IsPurePpuHBlankCallback(gMain.hblankCallback))
            return false;
        hasAnyEvent = true;
#else
        return false;
#endif
    }

    for (int i = 0; i < DMA_COUNT; i++)
    {
        struct DMATransfer *dma = &DMAList[i];
        if (!(dma->control & DMA_ENABLE)
         || (dma->control & DMA_START_MASK) != DMA_START_HBLANK)
            continue;

        bool is32Bit = (dma->control & DMA_32BIT) != 0;
        if ((((&REG_DMA0CNT)[i * 3] >> 16) & DMA_ENABLE) == 0
         || dma->size != 1
         || (dma->control & DMA_SRC_MASK) != DMA_SRC_INC
         || ((dma->control & DMA_DEST_MASK) != DMA_DEST_RELOAD
          && (dma->control & DMA_DEST_MASK) != DMA_DEST_FIXED)
         || !(dma->control & DMA_REPEAT)
         || (dma->control & DMA_DREQ_ON)
         || dma->src == NULL
         || !IsSafeHBlankDmaTarget(dma->dst, is32Bit))
            return false;

        if (pixels != NULL)
        {
            /* A DMA source that aliases the output surface can be changed by
             * earlier scanline draws, so fail closed for overlapping ranges. */
            uintptr_t sourceStart = (uintptr_t)dma->src;
            uintptr_t outputStart = (uintptr_t)pixels;
            const size_t dmaReadBytes = DISPLAY_HEIGHT * (is32Bit ? sizeof(uint32_t) : sizeof(uint16_t));
            const size_t outputBytes = DISPLAY_HEIGHT * DISPLAY_WIDTH * sizeof(uint16_t);
            if (sourceStart > UINTPTR_MAX - dmaReadBytes
             || outputStart > UINTPTR_MAX - outputBytes)
                return false;
            uintptr_t sourceEnd = sourceStart + dmaReadBytes;
            uintptr_t outputEnd = outputStart + outputBytes;
            if (sourceStart < outputEnd && outputStart < sourceEnd)
                return false;
        }
        hasAnyEvent = true;
    }

    return hasAnyEvent;
}

static bool HasSafeHBlankReplayEvents(void)
{
    return CheckSafeHBlankReplayInternal(NULL);
}

/* Replays HBlank scanline register updates (safe PPU HBlank DMAs and verified
 * pure-PPU HBlank callbacks) serially into per-row snapshots before dispatching
 * independent scanlines across the PPU worker pool. */
static bool CanDrawHBlankReplayInParallel(const uint16_t *pixels, bool *soundSyncDeferred)
{
    if (soundSyncDeferred)
        *soundSyncDeferred = false;

    u16 dispstat = REG_DISPSTAT;
    if (pixels == NULL)
        return false;
    if (!IsEarlyVCountSafeForDeferredSync(dispstat))
        return false;
    if (!CheckSafeHBlankReplayInternal(pixels))
        return false;

    if (soundSyncDeferred
     && (dispstat & DISPSTAT_VCOUNT_INTR) != 0
     && ((dispstat >> 8) & 0xFF) < DISPLAY_HEIGHT)
        *soundSyncDeferred = true;
    return true;
}

static struct HBlankRowState sHBlankReplayRows[DISPLAY_HEIGHT];

struct EasyDrawFrameContext
{
    uint16_t *pixels;
    const struct HBlankRowState *hblankRows;
};

static void ReplayHBlankScanlineState(struct HBlankRowState rows[DISPLAY_HEIGHT])
{
    CaptureHBlankRowState(&rows[0]);
    for (int line = 0; line < DISPLAY_HEIGHT; line++)
    {
        REG_VCOUNT = line;
        REG_DISPSTAT |= INTR_FLAG_HBLANK;
        RunDMAs(DMA_HBLANK);
        if (REG_DISPSTAT & DISPSTAT_HBLANK_INTR)
            gIntrTable[3]();
        REG_DISPSTAT &= ~(INTR_FLAG_HBLANK | INTR_FLAG_VCOUNT);
        if (line + 1 < DISPLAY_HEIGHT)
            CaptureHBlankRowState(&rows[line + 1]);
    }
}

static void DrawScanlineRange(void *context, int firstLine, int endLine)
{
    struct EasyDrawFrameContext *frame = context;
    uint16_t *pixels = frame->pixels;

    if (frame->hblankRows == NULL)
    {
        unsigned int bldcnt = REG_BLDCNT;
        uint16_t backdropColor = ComputeBackdropColor(bldcnt, (bldcnt >> 6) & 3u, REG_BLDY & 0x1Fu);
        for (int line = firstLine; line < endLine; line++)
        {
            memsetu16(&pixels[line * DISPLAY_WIDTH], backdropColor, DISPLAY_WIDTH);
            DrawScanline(&pixels[line * DISPLAY_WIDTH], line, NULL);
        }
    }
    else
    {
        for (int line = firstLine; line < endLine; line++)
        {
            const struct HBlankRowState *row = &frame->hblankRows[line];
            uint16_t backdropColor = ComputeBackdropColor(row->bldcnt, (row->bldcnt >> 6) & 3u, row->bldy & 0x1Fu);
            memsetu16(&pixels[line * DISPLAY_WIDTH], backdropColor, DISPLAY_WIDTH);
            DrawScanline(&pixels[line * DISPLAY_WIDTH], line, row);
        }
    }
}

void DrawFrame(uint16_t *pixels)
{
#ifdef EASY_DRAW_TEST_PARALLEL
    sTestLastWindowDmaParallel = false;
#endif
    bool hasFastDrawOverride = (gFastDrawDispcntClear != 0)
                            || (gFastDrawBldcntClear != 0)
                            || (gFastDrawScreenObjOnly != FALSE)
                            || (gFastDrawBackdropOverride >= 0);
    if ((REG_DISPCNT & ~(unsigned int)gFastDrawDispcntClear) & DISPCNT_OBJ_ON)
    {
        BuildSpriteScanlineBins(&sSpriteBins);
        sSpriteBinsValid = true;
        sSpritePriorityMaskSafe = CanUseSpritePriorityMask();
#if VITA_PERF_ENABLED
        VitaPerf_RecordActiveSprites(sSpriteBins.activeCount);
#endif
    }
    else
    {
        sSpriteBinsValid = false;
        sSpritePriorityMaskSafe = false;
    }
#if VITA_PERF_ENABLED
    uint64_t drawFrameStart = VitaPerf_Now();
#endif
#if defined(PLATFORM_VITA) || defined(EASY_DRAW_TEST_PARALLEL)
    bool soundSyncDeferred = false;
    bool drawWholeFrameInParallel = !hasFastDrawOverride && CanDrawScanlinesInParallel(&soundSyncDeferred);
#if defined(PLATFORM_VITA) && !defined(VITA_ENABLE_HBLANK_WIN0_PARALLEL)
    bool drawHBlankReplayInParallel = false;
#else
    bool hblankSoundSyncDeferred = false;
    bool drawHBlankReplayInParallel = !hasFastDrawOverride
                                   && !drawWholeFrameInParallel
                                   && CanDrawHBlankReplayInParallel(pixels, &hblankSoundSyncDeferred);
    if (drawHBlankReplayInParallel)
        soundSyncDeferred = hblankSoundSyncDeferred;
#endif
    int vcountCompare = 0;
    bool drawPrefixInParallel = !hasFastDrawOverride
                             && !drawWholeFrameInParallel
                             && !drawHBlankReplayInParallel
                             && CanDrawPrefixBeforeVCountInParallel(&vcountCompare);
#ifdef PLATFORM_VITA
    enum VitaEasyDrawFallbackReason fallbackReason = VITA_EASY_DRAW_FALLBACK_NONE;
#endif

    if (drawWholeFrameInParallel || drawHBlankReplayInParallel || drawPrefixInParallel)
    {
        /*
         * The serial loop calls RunDMAs once per scanline. With no enabled
         * HBlank transfer, only its first call can clear stale software DMA
         * state left behind by DmaStop; later calls are no-ops. Preserve that
         * transition before workers read the immutable display state.
         */
        struct EasyDrawFrameContext frame = {pixels, NULL};
        struct DMATransfer hblankDmaBefore[DMA_COUNT];
        struct HBlankRowState regsBefore;
        uint16_t vcountBefore = 0;
        uint16_t dispstatBefore = 0;
        if (drawHBlankReplayInParallel)
        {
            for (int ch = 0; ch < DMA_COUNT; ch++)
                hblankDmaBefore[ch] = DMAList[ch];
            CaptureHBlankRowState(&regsBefore);
            vcountBefore = REG_VCOUNT;
            dispstatBefore = REG_DISPSTAT;
#if VITA_PERF_ENABLED
            uint64_t replayStart = VitaPerf_Now();
#endif
            ReplayHBlankScanlineState(sHBlankReplayRows);
#if VITA_PERF_ENABLED
            VitaPerf_Record(VITA_PERF_HBLANK_WIN0_REPLAY, replayStart, VitaPerf_Now());
#endif
            frame.hblankRows = sHBlankReplayRows;
        }
        else
        {
            RunDMAs(DMA_HBLANK);
        }
        int parallelHeight = drawPrefixInParallel ? vcountCompare : DISPLAY_HEIGHT;
        if (GbaEasyDrawWorkers_Run(DrawScanlineRange, &frame, parallelHeight))
        {
            if (drawPrefixInParallel)
            {
                /* The serial prefix would leave VCOUNT on the previous row and
                 * clear both status flags. Restore that boundary state before
                 * servicing VCOUNT, then preserve later callback/DMA order. */
                REG_VCOUNT = vcountCompare - 1;
                REG_DISPSTAT &= ~(INTR_FLAG_HBLANK | INTR_FLAG_VCOUNT);
                DrawFrameSerialFrom(pixels, vcountCompare);
            }
            else
            {
                if (soundSyncDeferred)
                {
                    REG_DISPSTAT |= INTR_FLAG_VCOUNT;
                    gIntrTable[0]();
                }
                REG_VCOUNT = DISPLAY_HEIGHT - 1;
                REG_DISPSTAT &= ~(INTR_FLAG_HBLANK | INTR_FLAG_VCOUNT);
            }
#ifdef EASY_DRAW_TEST_PARALLEL
            sTestLastWindowDmaParallel = drawHBlankReplayInParallel;
#endif
#ifdef PLATFORM_VITA
            VitaPerf_RecordEasyDrawPath(drawPrefixInParallel ? VITA_EASY_DRAW_VCOUNT_PREFIX
                                        : drawHBlankReplayInParallel ? VITA_EASY_DRAW_WIN0H_DMA_PARALLEL
                                                                     : VITA_EASY_DRAW_FULL_PARALLEL,
                                        VITA_EASY_DRAW_FALLBACK_NONE);
#if VITA_PERF_ENABLED
            VitaPerf_Record(VITA_PERF_DRAW_FRAME, drawFrameStart, VitaPerf_Now());
#endif
#endif
            sSpriteBinsValid = false;
            return;
        }
#ifdef PLATFORM_VITA
        fallbackReason = VITA_EASY_DRAW_FALLBACK_WORKER_FAILURE;
#endif
        if (drawHBlankReplayInParallel)
        {
            /* The DMA replay advances the emulated source pointers before
             * worker dispatch. Restore pre-replay state if worker dispatch
             * fails so the serial fallback runs in the original order. */
            for (int ch = 0; ch < DMA_COUNT; ch++)
                DMAList[ch] = hblankDmaBefore[ch];
            RestoreHBlankRowState(&regsBefore);
            REG_VCOUNT = vcountBefore;
            REG_DISPSTAT = dispstatBefore;
        }
    }
#ifdef PLATFORM_VITA
    else
    {
        fallbackReason = GetEasyDrawFallbackReason();
    }
#endif
#endif

    DrawFrameSerial(pixels);
    sSpriteBinsValid = false;
#ifdef PLATFORM_VITA
    VitaPerf_RecordEasyDrawPath(VITA_EASY_DRAW_SERIAL, fallbackReason);
#endif
#if VITA_PERF_ENABLED
    VitaPerf_Record(VITA_PERF_DRAW_FRAME, drawFrameStart, VitaPerf_Now());
#endif
}

void RunFrameLineEvents(void)
{
    for (int i = 0; i < DISPLAY_HEIGHT; i++)
    {
        REG_VCOUNT = i;
        if (((REG_DISPSTAT >> 8) & 0xFF) == REG_VCOUNT)
        {
            REG_DISPSTAT |= INTR_FLAG_VCOUNT;
#ifdef __ANDROID__
            if (REG_IE & INTR_FLAG_VCOUNT)
#else
            if (REG_DISPSTAT & DISPSTAT_VCOUNT_INTR)
#endif
                gIntrTable[0]();
        }
        REG_DISPSTAT |= INTR_FLAG_HBLANK;
        RunDMAs(DMA_HBLANK);
        if (REG_DISPSTAT & DISPSTAT_HBLANK_INTR)
            gIntrTable[3]();
        REG_DISPSTAT &= ~(INTR_FLAG_HBLANK | INTR_FLAG_VCOUNT);
    }
}
#endif
