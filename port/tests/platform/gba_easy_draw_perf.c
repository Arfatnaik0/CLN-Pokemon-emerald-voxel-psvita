#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>

#include "global.h"

// The production GBA memory model stores VRAM addresses in 32 bits. Widen the
// pointer only for this native host harness so it can run on 64-bit machines.
#undef VRAM
#define VRAM ((uintptr_t)VRAM_)
#define RENDERER_EASY_DRAW
void RunDMAs(u32 type);
static void TestRunDMAs(u32 type);
#define RunDMAs TestRunDMAs
#include "../../src/platform/gba_easy_draw.c"
#undef RunDMAs

unsigned char REG_BASE[0x400] __attribute__((aligned(4)));
unsigned char PLTT[PLTT_SIZE] __attribute__((aligned(4)));
unsigned char VRAM_[VRAM_SIZE] __attribute__((aligned(4)));
unsigned char OAM[OAM_SIZE] __attribute__((aligned(4)));
struct SoundInfo *SOUND_INFO_PTR;
unsigned short INTR_CHECK;
void *INTR_VECTOR;

#define REG16(offset) (*(uint16_t *)(REG_BASE + (offset)))
#define REG32(offset) (*(uint32_t *)(REG_BASE + (offset)))

static unsigned int sVCountCallbacks;
static uint16_t sVCountCallbackStatusFlags;
static unsigned int sHBlankCallbacks;
static unsigned int sDmaCalls;
static bool sEnableHBlankDmaAtVCount;
static bool sEnableHBlankIrqAtVCount;
static bool sMutateSpritePriorityAtVCount;
static bool sMutateSpritePriorityAtHBlank;

static void NoInterrupt(void) { }
static void TestVCountInterrupt(void)
{
    sVCountCallbacks++;
    sVCountCallbackStatusFlags = REG16(REG_OFFSET_DISPSTAT)
                               & (INTR_FLAG_HBLANK | INTR_FLAG_VCOUNT);
    REG16(REG_OFFSET_BG0HOFS)++;
    if (sMutateSpritePriorityAtVCount)
        ((struct OamData *)OAM)[0].priority = 3;
    if (sEnableHBlankDmaAtVCount)
    {
        static uint16_t hblankValue = 13;
        u32 control = (u32)(DMA_ENABLE | DMA_START_HBLANK | DMA_REPEAT
                          | DMA_SRC_FIXED | DMA_DEST_FIXED | DMA_16BIT) << 16 | 1u;
        DmaSet(0, &hblankValue, (void *)&REG_BG0HOFS, control);
    }
    if (sEnableHBlankIrqAtVCount)
        REG16(REG_OFFSET_DISPSTAT) |= DISPSTAT_HBLANK_INTR;
}
static void TestHBlankInterrupt(void)
{
    sHBlankCallbacks++;
    REG16(REG_OFFSET_BG0HOFS)++;
    if (sMutateSpritePriorityAtHBlank)
        ((struct OamData *)OAM)[0].priority = sHBlankCallbacks & 3;
}
void (*const gIntrTable[4])(void) = {TestVCountInterrupt, NoInterrupt, NoInterrupt, TestHBlankInterrupt};

#include "../../src/platform/dma.c"

static void TestRunDMAs(u32 type)
{
    if (type == DMA_HBLANK)
        sDmaCalls++;
    RunDMAs(type);
}

static uint32_t sRng;

static uint8_t NextByte(void)
{
    sRng = sRng * 1664525u + 1013904223u;
    return sRng >> 24;
}

static void FillState(unsigned int scenario)
{
    sRng = 0xC001D00D;
    memset(REG_BASE, 0, sizeof(REG_BASE));
    for (size_t i = 0; i < sizeof(PLTT); i++)
        PLTT[i] = NextByte();
    for (size_t i = 0; i < sizeof(VRAM_); i++)
        VRAM_[i] = NextByte();

    // Keep generated tile references inside allocated tile memory while
    // varying palette selection, flips, and tile rows.
    uint16_t *map = (uint16_t *)(VRAM_ + 12 * BG_SCREEN_SIZE);
    for (unsigned int i = 0; i < 0x1000; i++)
        map[i] = (NextByte() & 0x3F) | ((NextByte() & 0xF) << 12) | ((NextByte() & 3) << 10);

    memset(OAM, 0, sizeof(OAM));
    unsigned int mode = (scenario == 1 || scenario == 3) ? 1 : 0;
    REG16(REG_OFFSET_DISPCNT) = 0x1F40 | mode
        | ((scenario == 2 || scenario == 5) ? DISPCNT_WIN0_ON : 0)
        | (scenario == 3 ? DISPCNT_WIN1_ON : 0);
    REG16(REG_OFFSET_DISPSTAT) = 0;

    for (unsigned int bg = 0; bg < 4; bg++)
    {
        uint16_t control = (bg % 4) | ((bg % 4) << 2) | (12 << 8);
        if ((scenario == 1 && bg == 2) || (scenario == 4 && bg > 0) || (scenario == 5 && bg == 3))
            control |= BGCNT_256COLOR;
        if ((scenario == 2 && bg == 0) || (scenario == 5 && bg == 1))
            control |= BGCNT_MOSAIC;
        if (scenario == 3)
            control |= (bg % 4) << 14;

        REG16(REG_OFFSET_BG0CNT + bg * 2) = control;
        REG16(REG_OFFSET_BG0HOFS + bg * 4) = scenario * 37 + bg * 19 + 251;
        REG16(REG_OFFSET_BG0VOFS + bg * 4) = scenario == 4 ? 250 + bg * 11 : scenario * 13 + bg * 5 + 63;
    }

    REG16(REG_OFFSET_MOSAIC) = 0x3210;
    REG16(REG_OFFSET_WIN0H) = 0x20C0;
    REG16(REG_OFFSET_WIN0V) = 0x1090;
    REG16(REG_OFFSET_WIN1H) = 0x4080;
    REG16(REG_OFFSET_WIN1V) = 0x2080;
    REG16(REG_OFFSET_WININ) = 0x3F3F;
    REG16(REG_OFFSET_WINOUT) = 0x3F3F;
    REG16(REG_OFFSET_BLDCNT) = scenario == 0 ? 0 : (1 | (1 << 4) | (1 << 6) | (1 << 8) | (1 << 12));
    REG16(REG_OFFSET_BLDALPHA) = 8 | (8 << 8);
    REG16(REG_OFFSET_BLDY) = 8;
    REG16(REG_OFFSET_BG2PA) = 0x100;
    REG16(REG_OFFSET_BG2PB) = 0;
    REG16(REG_OFFSET_BG2PC) = 0;
    REG16(REG_OFFSET_BG2PD) = 0x100;
    REG32(REG_OFFSET_BG2X) = 0x1800;
    REG32(REG_OFFSET_BG2Y) = 0x1000;

    struct OamData *oam = (struct OamData *)OAM;
    for (int i = 0; i < 128; i++)
        oam[i].affineMode = 2;
    for (int i = 0; i < 4; i++)
    {
        oam[i].affineMode = 0;
        oam[i].shape = i % 3;
        oam[i].size = i % 3;
        oam[i].x = 32 + i * 40;
        oam[i].y = 40 + i * 25;
        oam[i].tileNum = i * 8;
        oam[i].priority = i % 4;
        oam[i].paletteNum = i % 16;
        oam[i].bpp = i % 2;
        oam[i].objMode = scenario == 3 && i == 0 ? 1 : 0;
    }
}

// The pre-optimization scanline implementation is kept here as a reference,
// with the GBA 8bpp transparent-palette-index-zero rule corrected.
static void ReferenceRenderBGScanline(int bgNum, uint16_t control, uint16_t hoffs, uint16_t voffs,
                                      int lineNum, uint16_t *line)
{
    unsigned int charBaseBlock = (control >> 2) & 3;
    unsigned int screenBaseBlock = (control >> 8) & 0x1F;
    unsigned int bitsPerPixel = ((control >> 7) & 1) ? 8 : 4;
    unsigned int mapWidth = bgMapSizes[control >> 14][0];
    unsigned int mapHeight = bgMapSizes[control >> 14][1];
    unsigned int mapWidthInPixels = mapWidth * 8;
    unsigned int mapHeightInPixels = mapHeight * 8;
    uint8_t *bgtiles = (uint8_t *)BG_CHAR_ADDR(charBaseBlock);
    uint16_t *pal = (uint16_t *)PLTT;

    (void)bgNum;
    if (control & BGCNT_MOSAIC)
        lineNum = applyBGVerticalMosaicEffect(lineNum);

    hoffs &= 0x1FF;
    voffs &= 0x1FF;

    for (unsigned int x = 0; x < DISPLAY_WIDTH; x++)
    {
        uint16_t *bgmap = (uint16_t *)BG_SCREEN_ADDR(screenBaseBlock);
        unsigned int xx;
        if (control & BGCNT_MOSAIC)
            xx = (applyBGHorizontalMosaicEffect(x) + hoffs) & 0x1FF;
        else
            xx = (x + hoffs) & 0x1FF;

        unsigned int yy = (lineNum + voffs) & 0x1FF;
        if (xx > 255 && mapWidthInPixels > 256)
            bgmap += 0x400;
        if (yy > 255 && mapHeightInPixels > 256)
            bgmap += (mapWidthInPixels > 256) ? 0x800 : 0x400;

        xx &= 0xFF;
        yy &= 0xFF;
        unsigned int mapX = xx / 8;
        unsigned int mapY = yy / 8;
        uint16_t entry = bgmap[mapY * 32 + mapX];
        unsigned int tileNum = entry & 0x3FF;
        unsigned int paletteNum = (entry >> 12) & 0xF;
        unsigned int tileX = xx % 8;
        unsigned int tileY = yy % 8;

        if (entry & (1 << 10))
            tileX = 7 - tileX;
        if (entry & (1 << 11))
            tileY = 7 - tileY;

        uint16_t tileLoc = tileNum * (bitsPerPixel * 8);
        uint16_t tileLocY = tileY * bitsPerPixel;
        uint16_t tileLocX = tileX;
        if (bitsPerPixel == 4)
            tileLocX /= 2;

        uint8_t pixel = bgtiles[tileLoc + tileLocY + tileLocX];
        if (bitsPerPixel == 4)
        {
            if (tileX & 1)
                pixel >>= 4;
            else
                pixel &= 0xF;
            if (pixel != 0)
                line[x] = pal[16 * paletteNum + pixel] | 0x8000;
        }
        else
        {
            if (pixel != 0)
                line[x] = pal[pixel] | 0x8000;
        }
    }
}

static double Now(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec + time.tv_nsec / 1e9;
}

enum RasterEvent
{
    RASTER_EVENT_NONE,
    RASTER_EVENT_HBLANK_IRQ,
    RASTER_EVENT_VCOUNT_IRQ_0,
    RASTER_EVENT_VCOUNT_IRQ_150,
    RASTER_EVENT_VCOUNT_IRQ_150_STALE_STATUS,
    RASTER_EVENT_VCOUNT_IRQ_150_ENABLE_HBLANK_DMA,
    RASTER_EVENT_VCOUNT_IRQ_150_ENABLE_HBLANK_IRQ,
    RASTER_EVENT_VCOUNT_IRQ_159,
    RASTER_EVENT_VCOUNT_IRQ_160,
    RASTER_EVENT_HBLANK_DMA0,
    RASTER_EVENT_HBLANK_DMA1,
    RASTER_EVENT_HBLANK_DMA2,
    RASTER_EVENT_HBLANK_DMA3,
    RASTER_EVENT_STOPPED_HBLANK_DMA,
};

static int HBlankDmaChannel(enum RasterEvent event)
{
    if (event >= RASTER_EVENT_HBLANK_DMA0 && event <= RASTER_EVENT_HBLANK_DMA3)
        return (int)event - RASTER_EVENT_HBLANK_DMA0;
    if (event == RASTER_EVENT_STOPPED_HBLANK_DMA)
        return 3;
    return -1;
}

static void ResetFrameState(enum RasterEvent event)
{
    FillState(0);
    for (int dma = 0; dma < DMA_COUNT; dma++)
        DmaSet(dma, NULL, NULL, 0);

    sVCountCallbacks = 0;
    sVCountCallbackStatusFlags = 0;
    sHBlankCallbacks = 0;
    sDmaCalls = 0;
    sEnableHBlankDmaAtVCount = false;
    sEnableHBlankIrqAtVCount = false;
    sMutateSpritePriorityAtVCount = false;
    sMutateSpritePriorityAtHBlank = false;
    sForceLegacySimpleSpritePath = false;

    if (event == RASTER_EVENT_HBLANK_IRQ)
        REG16(REG_OFFSET_DISPSTAT) |= DISPSTAT_HBLANK_INTR;
    else if (event == RASTER_EVENT_VCOUNT_IRQ_0)
        REG16(REG_OFFSET_DISPSTAT) = DISPSTAT_VCOUNT_INTR;
    else if (event == RASTER_EVENT_VCOUNT_IRQ_150
          || event == RASTER_EVENT_VCOUNT_IRQ_150_STALE_STATUS
          || event == RASTER_EVENT_VCOUNT_IRQ_150_ENABLE_HBLANK_DMA
          || event == RASTER_EVENT_VCOUNT_IRQ_150_ENABLE_HBLANK_IRQ)
    {
        REG16(REG_OFFSET_DISPSTAT) = (150u << 8) | DISPSTAT_VCOUNT_INTR;
        if (event == RASTER_EVENT_VCOUNT_IRQ_150_STALE_STATUS)
            REG16(REG_OFFSET_DISPSTAT) |= INTR_FLAG_HBLANK | INTR_FLAG_VCOUNT;
        sEnableHBlankDmaAtVCount = event == RASTER_EVENT_VCOUNT_IRQ_150_ENABLE_HBLANK_DMA;
        sEnableHBlankIrqAtVCount = event == RASTER_EVENT_VCOUNT_IRQ_150_ENABLE_HBLANK_IRQ;
    }
    else if (event == RASTER_EVENT_VCOUNT_IRQ_159)
        REG16(REG_OFFSET_DISPSTAT) = (159u << 8) | DISPSTAT_VCOUNT_INTR;
    else if (event == RASTER_EVENT_VCOUNT_IRQ_160)
        REG16(REG_OFFSET_DISPSTAT) = (160u << 8) | DISPSTAT_VCOUNT_INTR;
    else if (HBlankDmaChannel(event) >= 0)
    {
        static uint16_t hblankValue = 7;
        u32 control = (u32)(DMA_ENABLE | DMA_START_HBLANK | DMA_REPEAT
                          | DMA_SRC_FIXED | DMA_DEST_FIXED | DMA_16BIT) << 16 | 1u;
        DmaSet(HBlankDmaChannel(event), &hblankValue, (void *)&REG_BG0HOFS, control);
        if (event == RASTER_EVENT_STOPPED_HBLANK_DMA)
            DmaStop(3);
    }
}

static bool RenderParityCase(enum RasterEvent event, const char *name, unsigned int expectedDmaCalls)
{
    uint16_t serial[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    uint16_t dispatched[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    unsigned int serialVCount;
    uint16_t serialVCountCallbackStatusFlags;
    unsigned int serialHBlank;
    uint16_t serialDispstat;
    uint16_t serialVcountReg;

    ResetFrameState(event);
    DrawFrameSerial(serial);
    if (sDmaCalls != DISPLAY_HEIGHT)
    {
        fprintf(stderr, "%s serial baseline called HBlank DMA %u times\n", name, sDmaCalls);
        return false;
    }
    serialVCount = sVCountCallbacks;
    serialVCountCallbackStatusFlags = sVCountCallbackStatusFlags;
    serialHBlank = sHBlankCallbacks;
    serialDispstat = REG16(REG_OFFSET_DISPSTAT);
    serialVcountReg = REG16(REG_OFFSET_VCOUNT);

    ResetFrameState(event);
    DrawFrame(dispatched);
    if (memcmp(serial, dispatched, sizeof(serial)) != 0
     || serialVCount != sVCountCallbacks
     || serialVCountCallbackStatusFlags != sVCountCallbackStatusFlags
     || serialHBlank != sHBlankCallbacks
     || sDmaCalls != expectedDmaCalls
     || serialDispstat != REG16(REG_OFFSET_DISPSTAT)
     || serialVcountReg != REG16(REG_OFFSET_VCOUNT))
    {
        fprintf(stderr, "%s render/callback parity failed (vcount %u/%u status %04x/%04x, hblank %u/%u, dispstat %04x/%04x, vcountreg %u/%u)\n",
                name, serialVCount, sVCountCallbacks, serialVCountCallbackStatusFlags,
                sVCountCallbackStatusFlags, serialHBlank, sHBlankCallbacks,
                serialDispstat, REG16(REG_OFFSET_DISPSTAT), serialVcountReg,
                REG16(REG_OFFSET_VCOUNT));
        return false;
    }

    if (event == RASTER_EVENT_HBLANK_IRQ && sHBlankCallbacks != DISPLAY_HEIGHT)
        return false;
    if (event == RASTER_EVENT_VCOUNT_IRQ_150_ENABLE_HBLANK_IRQ
     && sHBlankCallbacks != DISPLAY_HEIGHT - 150)
    {
        fprintf(stderr, "%s expected serial-tail HBlank IRQs, got %u\n", name, sHBlankCallbacks);
        return false;
    }
    if ((event == RASTER_EVENT_VCOUNT_IRQ_0 || event == RASTER_EVENT_VCOUNT_IRQ_150
      || event == RASTER_EVENT_VCOUNT_IRQ_150_STALE_STATUS
      || event == RASTER_EVENT_VCOUNT_IRQ_150_ENABLE_HBLANK_DMA
      || event == RASTER_EVENT_VCOUNT_IRQ_150_ENABLE_HBLANK_IRQ || event == RASTER_EVENT_VCOUNT_IRQ_159)
     && sVCountCallbacks != 1)
        return false;
    if ((event == RASTER_EVENT_VCOUNT_IRQ_150
      || event == RASTER_EVENT_VCOUNT_IRQ_150_STALE_STATUS
      || event == RASTER_EVENT_VCOUNT_IRQ_150_ENABLE_HBLANK_DMA
      || event == RASTER_EVENT_VCOUNT_IRQ_150_ENABLE_HBLANK_IRQ)
     && sVCountCallbackStatusFlags != INTR_FLAG_VCOUNT)
    {
        fprintf(stderr, "%s saw unexpected VCOUNT callback status flags %04x\n",
                name, sVCountCallbackStatusFlags);
        return false;
    }
    if (event == RASTER_EVENT_VCOUNT_IRQ_160 && sVCountCallbacks != 0)
        return false;
    if (HBlankDmaChannel(event) >= 0 && event != RASTER_EVENT_STOPPED_HBLANK_DMA
     && sDmaCalls != DISPLAY_HEIGHT)
    {
        fprintf(stderr, "%s expected serial HBlank calls, got %u\n", name, sDmaCalls);
        return false;
    }
    if (event == RASTER_EVENT_STOPPED_HBLANK_DMA
     && (sDmaCalls != expectedDmaCalls || (DMAList[3].control & DMA_ENABLE) != 0))
    {
        fprintf(stderr, "%s expected one DMA normalization and a stopped software channel, calls=%u control=%04x\n",
                name, sDmaCalls, DMAList[3].control);
        return false;
    }

    return true;
}

static void ConfigureWin0hHBlankDma(uint16_t values[DISPLAY_HEIGHT + 1])
{
    FillState(2); // Window 0 enabled; its moving horizontal edge changes pixels.
    for (int dma = 0; dma < DMA_COUNT; dma++)
        DmaSet(dma, NULL, NULL, 0);
    REG16(REG_OFFSET_DISPSTAT) = 0;
    REG16(REG_OFFSET_WIN0V) = 0x00A0; // Window covers every visible row.
    REG16(REG_OFFSET_WININ) = 0x003F;
    REG16(REG_OFFSET_WINOUT) = 0;
    REG16(REG_OFFSET_BLDCNT) = 0;
    values[0] = ((uint16_t)0 << 8) | 120;
    for (int line = 1; line <= DISPLAY_HEIGHT; line++)
    {
        uint16_t left = (uint16_t)((line * 7) % 120);
        uint16_t right = left + 120;
        values[line] = (left << 8) | right;
    }
    REG16(REG_OFFSET_WIN0H) = values[0];
    DmaSet(0, &values[1], (void *)&REG_WIN0H,
           (u32)(DMA_ENABLE | DMA_START_HBLANK | DMA_REPEAT | DMA_SRC_INC
            | DMA_DEST_RELOAD | DMA_16BIT) << 16 | 1);
    sDmaCalls = 0;
    sVCountCallbacks = 0;
    sVCountCallbackStatusFlags = 0;
    sHBlankCallbacks = 0;
}

static bool TestParallelWin0hHBlankDmaParity(void)
{
    static uint16_t sourceValues[DISPLAY_HEIGHT + 1];
    static uint16_t serial[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t parallel[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    struct DMATransfer serialDma;
    uint16_t serialDispstat, serialVcount;
    unsigned int serialDmaCalls;

    ConfigureWin0hHBlankDma(sourceValues);
    DrawFrameSerial(serial);
    if (sDmaCalls != DISPLAY_HEIGHT)
    {
        fprintf(stderr, "WIN0H HBlank DMA serial baseline made %u events\n", sDmaCalls);
        return false;
    }
    serialDma = DMAList[0];
    serialDispstat = REG16(REG_OFFSET_DISPSTAT);
    serialVcount = REG16(REG_OFFSET_VCOUNT);
    serialDmaCalls = sDmaCalls;

    ConfigureWin0hHBlankDma(sourceValues);
    DrawFrame(parallel);
    if (!sTestLastWindowDmaParallel
     || memcmp(serial, parallel, sizeof(serial)) != 0
     || sDmaCalls != serialDmaCalls
     || DMAList[0].src != serialDma.src
     || DMAList[0].dst != serialDma.dst
     || DMAList[0].size != serialDma.size
     || DMAList[0].control != serialDma.control
     || REG16(REG_OFFSET_WIN0H) != sourceValues[DISPLAY_HEIGHT]
     || REG16(REG_OFFSET_DISPSTAT) != serialDispstat
     || REG16(REG_OFFSET_VCOUNT) != serialVcount)
    {
        fprintf(stderr, "parallel WIN0H HBlank-DMA snapshot did not preserve serial frame/state\n");
        return false;
    }
    puts("WIN0H HBlank-DMA snapshot: 160 row values, frame pixels, DMA state and final registers match serial rendering");

    /* If DMA reads memory while the serial PPU is writing that same memory,
     * replaying all events before the PPU would change semantics. Verify that
     * this alias fails closed to the ordinary serial path. */
    static uint16_t aliasSerial[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t aliasParallel[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    FillState(2);
    for (int dma = 0; dma < DMA_COUNT; dma++)
        DmaSet(dma, NULL, NULL, 0);
    REG16(REG_OFFSET_DISPSTAT) = 0;
    REG16(REG_OFFSET_WIN0V) = 0x00A0;
    REG16(REG_OFFSET_WININ) = 0x003F;
    REG16(REG_OFFSET_WINOUT) = 0;
    REG16(REG_OFFSET_BLDCNT) = 0;
    REG16(REG_OFFSET_WIN0H) = 0x0078;
    DmaSet(0, &aliasSerial[1], (void *)&REG_WIN0H,
           (u32)(DMA_ENABLE | DMA_START_HBLANK | DMA_REPEAT | DMA_SRC_INC
            | DMA_DEST_RELOAD | DMA_16BIT) << 16 | 1);
    sDmaCalls = 0;
    DrawFrameSerial(aliasSerial);

    FillState(2);
    for (int dma = 0; dma < DMA_COUNT; dma++)
        DmaSet(dma, NULL, NULL, 0);
    REG16(REG_OFFSET_DISPSTAT) = 0;
    REG16(REG_OFFSET_WIN0V) = 0x00A0;
    REG16(REG_OFFSET_WININ) = 0x003F;
    REG16(REG_OFFSET_WINOUT) = 0;
    REG16(REG_OFFSET_BLDCNT) = 0;
    REG16(REG_OFFSET_WIN0H) = 0x0078;
    DmaSet(0, &aliasParallel[1], (void *)&REG_WIN0H,
           (u32)(DMA_ENABLE | DMA_START_HBLANK | DMA_REPEAT | DMA_SRC_INC
            | DMA_DEST_RELOAD | DMA_16BIT) << 16 | 1);
    sDmaCalls = 0;
    DrawFrame(aliasParallel);
    if (sTestLastWindowDmaParallel || sDmaCalls != DISPLAY_HEIGHT
     || memcmp(aliasSerial, aliasParallel, sizeof(aliasSerial)) != 0)
    {
        fprintf(stderr, "WIN0H HBlank DMA source/output alias did not fail closed to serial rendering\n");
        return false;
    }
    puts("WIN0H HBlank-DMA source alias: render remained serial and matched reference");
    return true;
}

static bool TestWin0hDmaVcountBoundary(uint16_t compare, bool expectParallel)
{
    static uint16_t sourceValues[DISPLAY_HEIGHT + 1];
    static uint16_t serial[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t candidate[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    const uint16_t initialDispstat = (compare << 8) | DISPSTAT_VCOUNT_INTR;
    struct DMATransfer serialDma;
    uint16_t serialDispstat, serialVcount;
    unsigned int serialVCountCallbacks, serialDmaCalls;

    ConfigureWin0hHBlankDma(sourceValues);
    REG16(REG_OFFSET_DISPSTAT) = initialDispstat;
    DrawFrameSerial(serial);
    serialDma = DMAList[0];
    serialDispstat = REG16(REG_OFFSET_DISPSTAT);
    serialVcount = REG16(REG_OFFSET_VCOUNT);
    serialVCountCallbacks = sVCountCallbacks;
    serialDmaCalls = sDmaCalls;

    ConfigureWin0hHBlankDma(sourceValues);
    REG16(REG_OFFSET_DISPSTAT) = initialDispstat;
    DrawFrame(candidate);
    if (sTestLastWindowDmaParallel != expectParallel
     || memcmp(serial, candidate, sizeof(serial)) != 0
     || sVCountCallbacks != serialVCountCallbacks
     || sDmaCalls != serialDmaCalls
     || DMAList[0].src != serialDma.src
     || DMAList[0].size != serialDma.size
     || DMAList[0].control != serialDma.control
     || REG16(REG_OFFSET_DISPSTAT) != serialDispstat
     || REG16(REG_OFFSET_VCOUNT) != serialVcount)
    {
        fprintf(stderr, "WIN0H DMA VCOUNT boundary %u failed (parallel=%u, callbacks=%u/%u)\n",
                compare, sTestLastWindowDmaParallel, serialVCountCallbacks, sVCountCallbacks);
        return false;
    }
    return true;
}

static bool TestWin0hDmaHblankInterruptFallback(void)
{
    static uint16_t sourceValues[DISPLAY_HEIGHT + 1];
    static uint16_t serial[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t candidate[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    uint16_t serialDispstat, serialVcount;
    unsigned int serialHBlankCallbacks;

    ConfigureWin0hHBlankDma(sourceValues);
    REG16(REG_OFFSET_DISPSTAT) |= DISPSTAT_HBLANK_INTR;
    DrawFrameSerial(serial);
    serialDispstat = REG16(REG_OFFSET_DISPSTAT);
    serialVcount = REG16(REG_OFFSET_VCOUNT);
    serialHBlankCallbacks = sHBlankCallbacks;

    ConfigureWin0hHBlankDma(sourceValues);
    REG16(REG_OFFSET_DISPSTAT) |= DISPSTAT_HBLANK_INTR;
    DrawFrame(candidate);
    if (sTestLastWindowDmaParallel
     || memcmp(serial, candidate, sizeof(serial)) != 0
     || sHBlankCallbacks != serialHBlankCallbacks
     || sHBlankCallbacks != DISPLAY_HEIGHT
     || sDmaCalls != DISPLAY_HEIGHT
     || REG16(REG_OFFSET_DISPSTAT) != serialDispstat
     || REG16(REG_OFFSET_VCOUNT) != serialVcount)
    {
        fprintf(stderr, "WIN0H DMA plus HBlank IRQ did not remain on the serial path\n");
        return false;
    }
    return true;
}

static bool TestWin0hDmaWorkerFailureRestore(void)
{
    static uint16_t sourceValues[DISPLAY_HEIGHT + 1];
    static uint16_t serial[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t candidate[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    struct DMATransfer serialDma;
    uint16_t serialDispstat, serialVcount, serialWin0h;

    ConfigureWin0hHBlankDma(sourceValues);
    DrawFrameSerial(serial);
    serialDma = DMAList[0];
    serialDispstat = REG16(REG_OFFSET_DISPSTAT);
    serialVcount = REG16(REG_OFFSET_VCOUNT);
    serialWin0h = REG16(REG_OFFSET_WIN0H);

    ConfigureWin0hHBlankDma(sourceValues);
    GbaEasyDrawWorkers_Shutdown();
    GbaEasyDrawWorkers_TestFailAfter(0);
    DrawFrame(candidate);
    GbaEasyDrawWorkers_Shutdown();
    if (sTestLastWindowDmaParallel
     || memcmp(serial, candidate, sizeof(serial)) != 0
     || sDmaCalls != 2 * DISPLAY_HEIGHT
     || DMAList[0].src != serialDma.src
     || DMAList[0].dst != serialDma.dst
     || DMAList[0].size != serialDma.size
     || DMAList[0].control != serialDma.control
     || REG16(REG_OFFSET_WIN0H) != serialWin0h
     || REG16(REG_OFFSET_DISPSTAT) != serialDispstat
     || REG16(REG_OFFSET_VCOUNT) != serialVcount)
    {
        fprintf(stderr, "WIN0H DMA worker-creation failure did not restore state before serial fallback\n");
        return false;
    }
    puts("WIN0H HBlank-DMA worker failure: DMA/register snapshot restored before serial fallback");
    return true;
}

static int sActiveRanges;
static int sMaxActiveRanges;
static int sRangeFailures;
static pthread_mutex_t sRangeStartMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sRangeStartCondition = PTHREAD_COND_INITIALIZER;
static int sRangesStarted;

static void TestSlowRange(void *context, int firstLine, int endLine)
{
    int *coverage = context;
    int active = __sync_add_and_fetch(&sActiveRanges, 1);
    int previous = __atomic_load_n(&sMaxActiveRanges, __ATOMIC_RELAXED);
    while (active > previous
        && !__atomic_compare_exchange_n(&sMaxActiveRanges, &previous, active, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    {
    }

    pthread_mutex_lock(&sRangeStartMutex);
    sRangesStarted++;
    pthread_cond_broadcast(&sRangeStartCondition);
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 2;
    while (sRangesStarted < 3)
    {
        if (pthread_cond_timedwait(&sRangeStartCondition, &sRangeStartMutex, &timeout) == ETIMEDOUT)
        {
            __sync_add_and_fetch(&sRangeFailures, 1);
            break;
        }
    }
    pthread_mutex_unlock(&sRangeStartMutex);

    if (firstLine < 0 || endLine <= firstLine || endLine > DISPLAY_HEIGHT)
        __sync_add_and_fetch(&sRangeFailures, 1);
    for (int line = firstLine; line < endLine; line++)
        __sync_add_and_fetch(&coverage[line], 1);

    struct timespec delay = {0, 5000000};
    nanosleep(&delay, NULL);
    __sync_sub_and_fetch(&sActiveRanges, 1);
}

static bool TestWorkerOverlapAndLifecycle(void)
{
    int coverage[DISPLAY_HEIGHT] = {0};
    __atomic_store_n(&sActiveRanges, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sMaxActiveRanges, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&sRangeFailures, 0, __ATOMIC_RELAXED);
    pthread_mutex_lock(&sRangeStartMutex);
    sRangesStarted = 0;
    pthread_mutex_unlock(&sRangeStartMutex);
    if (!GbaEasyDrawWorkers_Run(TestSlowRange, coverage, DISPLAY_HEIGHT))
        return false;
    int maxActiveRanges = __atomic_load_n(&sMaxActiveRanges, __ATOMIC_RELAXED);
    int rangeFailures = __atomic_load_n(&sRangeFailures, __ATOMIC_RELAXED);
    if (maxActiveRanges < 3 || rangeFailures != 0)
    {
        fprintf(stderr, "worker overlap test failed (expected main + 2 workers; max concurrency=%d range errors=%d)\n",
                maxActiveRanges, rangeFailures);
        return false;
    }
    for (int line = 0; line < DISPLAY_HEIGHT; line++)
    {
        if (coverage[line] != 1)
        {
            fprintf(stderr, "worker coverage failed at row %d (count=%d)\n", line, coverage[line]);
            return false;
        }
    }

    GbaEasyDrawWorkers_Shutdown();
    if (!RenderParityCase(RASTER_EVENT_NONE, "worker restart", 1))
        return false;

    GbaEasyDrawWorkers_Shutdown();
    GbaEasyDrawWorkers_TestFailAfter(0);
    if (!RenderParityCase(RASTER_EVENT_NONE, "first-thread initialization fallback", 1 + DISPLAY_HEIGHT))
        return false;

    GbaEasyDrawWorkers_Shutdown();
    GbaEasyDrawWorkers_TestFailAfter(1);
    if (!RenderParityCase(RASTER_EVENT_NONE, "partial initialization fallback", 1 + DISPLAY_HEIGHT))
        return false;

    /* The one-shot injected failure must leave no live thread or stale semaphore. */
    if (!RenderParityCase(RASTER_EVENT_NONE, "worker restart after partial failure", 1))
        return false;
    GbaEasyDrawWorkers_Shutdown();
    return true;
}

static bool TestWin1Coordinates(void)
{
    uint16_t serial[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    uint16_t parallel[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    ResetFrameState(RASTER_EVENT_NONE);

    /* Make BG0 opaque and deterministic, then show it only through WIN1. */
    REG16(REG_OFFSET_DISPCNT) &= ~(0x0E00 | DISPCNT_WIN0_ON | DISPCNT_OBJ_ON | DISPCNT_OBJWIN_ON);
    REG16(REG_OFFSET_DISPCNT) |= DISPCNT_BG0_ON | DISPCNT_WIN1_ON;
    REG16(REG_OFFSET_WIN0H) = 0x20C0;
    REG16(REG_OFFSET_WIN0V) = 0x1090;
    REG16(REG_OFFSET_WIN1H) = 0x4080;
    REG16(REG_OFFSET_WIN1V) = 0x2080;
    REG16(REG_OFFSET_WININ) = 0x3F00;
    REG16(REG_OFFSET_WINOUT) = 0;
    REG16(REG_OFFSET_BLDCNT) = 0;
    memset(VRAM_, 0, 0x4000);
    memset(VRAM_, 0x11, 32);
    memset(VRAM_ + 12 * BG_SCREEN_SIZE, 0, BG_SCREEN_SIZE);
    ((uint16_t *)PLTT)[0] = 0x0001;
    ((uint16_t *)PLTT)[1] = 0x1234;

    DrawFrameSerial(serial);
    if (serial[64 * DISPLAY_WIDTH + 100] != 0x9234
     || serial[64 * DISPLAY_WIDTH + 150] != 0x0001)
    {
        fprintf(stderr, "WIN1 coordinate check failed: inside=%04x outside=%04x\n",
                serial[64 * DISPLAY_WIDTH + 100], serial[64 * DISPLAY_WIDTH + 150]);
        return false;
    }

    ResetFrameState(RASTER_EVENT_NONE);
    REG16(REG_OFFSET_DISPCNT) &= ~(0x0E00 | DISPCNT_WIN0_ON | DISPCNT_OBJ_ON | DISPCNT_OBJWIN_ON);
    REG16(REG_OFFSET_DISPCNT) |= DISPCNT_BG0_ON | DISPCNT_WIN1_ON;
    REG16(REG_OFFSET_WIN0H) = 0x20C0;
    REG16(REG_OFFSET_WIN0V) = 0x1090;
    REG16(REG_OFFSET_WIN1H) = 0x4080;
    REG16(REG_OFFSET_WIN1V) = 0x2080;
    REG16(REG_OFFSET_WININ) = 0x3F00;
    REG16(REG_OFFSET_WINOUT) = 0;
    REG16(REG_OFFSET_BLDCNT) = 0;
    memset(VRAM_, 0, 0x4000);
    memset(VRAM_, 0x11, 32);
    memset(VRAM_ + 12 * BG_SCREEN_SIZE, 0, BG_SCREEN_SIZE);
    ((uint16_t *)PLTT)[0] = 0x0001;
    ((uint16_t *)PLTT)[1] = 0x1234;

    DrawFrame(parallel);
    if (memcmp(serial, parallel, sizeof(serial)) != 0)
    {
        fprintf(stderr, "WIN1 parallel frame differs from serial\n");
        return false;
    }
    return true;
}

static bool TestSpriteLayerBounds(void)
{
    static const unsigned int scenarios[] = {0, 3, 0, 6};
    struct scanlineData scanline;
    uint16_t fullFrame[DISPLAY_WIDTH];
    uint16_t boundedFrame[DISPLAY_WIDTH];

    for (unsigned int scenario = 0; scenario < sizeof(scenarios) / sizeof(scenarios[0]); scenario++)
    {
        FillState(scenarios[scenario]);
        bool windowsEnabled = scenario == 1 || scenario == 2;
        if (scenario == 2)
        {
            REG16(REG_OFFSET_DISPCNT) |= DISPCNT_OBJWIN_ON;
            REG16(REG_OFFSET_WINOUT) = ((WINMASK_OBJ | WINMASK_BG0) << 8);
            struct OamData *oam = (struct OamData *)OAM;
            for (int i = 0; i < 128; i++)
                oam[i].affineMode = 2;
            oam[0].affineMode = 0;
            oam[0].objMode = 2;
            windowsEnabled = true;
        }
        else if (scenario == 3)
        {
            struct OamData *oam = (struct OamData *)OAM;
            memset(VRAM_ + 0x10000, 0, VRAM_SIZE - 0x10000);
            memset(PLTT + 0x200, 0, PLTT_SIZE - 0x200);
            ((uint8_t *)(VRAM_ + 0x10000))[0] = 0x01; /* tile 0, row 0, x=0 */
            ((uint8_t *)(VRAM_ + 0x10000))[32 + 2] = 0x01; /* tile 1, row 0, x=4 */
            ((uint16_t *)(PLTT + 0x200))[1] = 0x1234;
            for (int i = 0; i < 128; i++)
                oam[i].affineMode = 2;
            oam[0].affineMode = 0;
            oam[0].x = 0;
            oam[0].y = 40;
            oam[0].tileNum = 0;
            oam[0].priority = 3;
            oam[1].affineMode = 0;
            oam[1].x = 235;
            oam[1].y = 40;
            oam[1].tileNum = 1;
            oam[1].priority = 0;
            oam[2].affineMode = 0;
            oam[2].x = 100;
            oam[2].y = 40;
            oam[2].tileNum = 2; /* Fully transparent sprite must keep empty bounds. */
            oam[2].priority = 2;
        }

        for (unsigned int line = 0; line < DISPLAY_HEIGHT; line++)
        {
            memset(scanline.spriteLayers, 0, sizeof(scanline.spriteLayers));
            memset(scanline.winMask, 0, sizeof(scanline.winMask));
            memset(scanline.prioritySortedBgsCount, 0, sizeof(scanline.prioritySortedBgsCount));
            for (int priority = 0; priority < 4; priority++)
            {
                scanline.spriteMinX[priority] = DISPLAY_WIDTH;
                scanline.spriteMaxX[priority] = 0;
            }
            if (windowsEnabled)
            {
                for (unsigned int x = 0; x < DISPLAY_WIDTH; x++)
                    scanline.winMask[x] = WINMASK_WINOUT | ((x & 1) ? 0 : WINMASK_OBJ);
            }

            if (REG_DISPCNT & DISPCNT_OBJ_ON)
                DrawSprites(&scanline, line, windowsEnabled, NULL);

            for (unsigned int priority = 0; priority < 4; priority++)
            {
                unsigned int expectedMin = DISPLAY_WIDTH;
                unsigned int expectedMax = 0;
                const uint16_t *src = scanline.spriteLayers[priority];
                for (unsigned int x = 0; x < DISPLAY_WIDTH; x++)
                {
                    if (getAlphaBit(src[x]))
                    {
                        if (expectedMin == DISPLAY_WIDTH)
                            expectedMin = x;
                        expectedMax = x + 1;
                    }
                }
                if (scanline.spriteMinX[priority] != expectedMin
                 || scanline.spriteMaxX[priority] != expectedMax)
                {
                    fprintf(stderr,
                            "sprite range mismatch (scenario=%u line=%u priority=%u): got %u..%u expected %u..%u\n",
                            scenario, line, priority, scanline.spriteMinX[priority],
                            scanline.spriteMaxX[priority], expectedMin, expectedMax);
                    return false;
                }

                for (unsigned int x = 0; x < DISPLAY_WIDTH; x++)
                    fullFrame[x] = boundedFrame[x] = 0x5A5A;
                CompositeSpriteLayer(&scanline, fullFrame, priority, windowsEnabled, 0, DISPLAY_WIDTH);
                CompositeSpriteLayer(&scanline, boundedFrame, priority, windowsEnabled,
                                     scanline.spriteMinX[priority], scanline.spriteMaxX[priority]);
                if (memcmp(fullFrame, boundedFrame, sizeof(fullFrame)) != 0)
                {
                    fprintf(stderr,
                            "sprite compositor bounds changed pixels (scenario=%u line=%u priority=%u)\n",
                            scenario, line, priority);
                    return false;
                }
            }
        }
    }

    puts("sprite layer ranges: exact bounds and full-width compositor parity passed (normal, semi-transparent, OBJ-window, transparent and screen-edge pixels)");
    return true;
}

static void ConfigureSpriteCompositorScene(unsigned int scene)
{
    struct OamData *oam = (struct OamData *)OAM;
    switch (scene)
    {
    case 0:
        /* Four text BGs and sprites with different sizes and priorities. */
        break;
    case 1:
        REG16(REG_OFFSET_DISPCNT) |= DISPCNT_WIN0_ON | DISPCNT_WIN1_ON;
        REG16(REG_OFFSET_WIN0H) = 0x30D0;
        REG16(REG_OFFSET_WIN0V) = 0x2090;
        REG16(REG_OFFSET_WIN1H) = 0xA050;
        REG16(REG_OFFSET_WIN1V) = 0x4080;
        REG16(REG_OFFSET_WININ) = 0x3F3F;
        REG16(REG_OFFSET_WINOUT) = 0x3F3F;
        REG16(REG_OFFSET_MOSAIC) = 0x4321;
        REG16(REG_OFFSET_BG0CNT) |= BGCNT_MOSAIC;
        REG16(REG_OFFSET_BG1CNT) |= BGCNT_MOSAIC;
        REG16(REG_OFFSET_BLDCNT) = BLDCNT_TGT1_BG0 | BLDCNT_TGT1_BG1 | BLDCNT_TGT1_OBJ
                                  | BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG1
                                  | BLDCNT_TGT2_OBJ | BLDCNT_TGT2_BD;
        REG16(REG_OFFSET_BLDALPHA) = 10 | (5 << 8);
        oam[0].objMode = 1; /* Semi-transparent OBJ */
        oam[1].matrixNum = ST_OAM_HFLIP | ST_OAM_VFLIP;
        oam[2].mosaic = 1;
        break;
    case 2:
        REG16(REG_OFFSET_DISPCNT) = (REG16(REG_OFFSET_DISPCNT) & ~3) | DISPCNT_MODE_1;
        REG16(REG_OFFSET_BG2PA) = 0x00E0;
        REG16(REG_OFFSET_BG2PB) = 0x0040;
        REG16(REG_OFFSET_BG2PC) = (uint16_t)-0x0020;
        REG16(REG_OFFSET_BG2PD) = 0x0120;
        REG32(REG_OFFSET_BG2X) = 0x2400;
        REG32(REG_OFFSET_BG2Y) = (uint32_t)-0x1800;
        REG16(REG_OFFSET_BG2CNT) |= BGCNT_MOSAIC;
        REG16(REG_OFFSET_MOSAIC) = 0x3210;
        REG16(REG_OFFSET_BLDCNT) = BLDCNT_TGT1_BG_ALL | BLDCNT_EFFECT_DARKEN;
        REG16(REG_OFFSET_BLDY) = 11;
        oam[0].mosaic = 1;
        oam[1].priority = 3;
        oam[2].priority = 0;
        oam[3].priority = 2;
        break;
    case 3:
        REG16(REG_OFFSET_DISPCNT) |= DISPCNT_WIN1_ON | DISPCNT_OBJWIN_ON;
        REG16(REG_OFFSET_WIN1H) = 0xD050;
        REG16(REG_OFFSET_WIN1V) = 0x2080;
        REG16(REG_OFFSET_WININ) = 0x1F3F;
        REG16(REG_OFFSET_WINOUT) = (WINMASK_OBJ | WINMASK_BG0 | WINMASK_BG1) << 8;
        oam[0].objMode = 2; /* OBJ-window */
        oam[1].objMode = 1;
        oam[2].matrixNum = ST_OAM_HFLIP;
        break;
    case 4:
        REG16(REG_OFFSET_BG0CNT) |= BGCNT_256COLOR;
        REG16(REG_OFFSET_BG2CNT) |= BGCNT_256COLOR;
        REG16(REG_OFFSET_BLDCNT) = BLDCNT_TGT1_BG_ALL | BLDCNT_TGT1_OBJ
                                  | BLDCNT_EFFECT_LIGHTEN;
        REG16(REG_OFFSET_BLDY) = 9;
        oam[0].bpp = 1;
        oam[1].bpp = 1;
        oam[2].matrixNum = ST_OAM_VFLIP;
        oam[3].priority = 3;
        break;
    case 5:
        /* Sparse nontransparent pixels at the visible x=0 and x=239 edges. */
        REG16(REG_OFFSET_DISPCNT) &= ~(DISPCNT_WIN0_ON | DISPCNT_WIN1_ON | DISPCNT_OBJWIN_ON);
        memset(VRAM_ + 0x10000, 0, VRAM_SIZE - 0x10000);
        memset(PLTT + 0x200, 0, PLTT_SIZE - 0x200);
        ((uint8_t *)(VRAM_ + 0x10000))[0] = 0x01;
        ((uint8_t *)(VRAM_ + 0x10000))[32 + 2] = 0x01;
        ((uint16_t *)(PLTT + 0x200))[1] = 0x1234;
        ((uint16_t *)(PLTT + 0x200))[17] = 0x7FFF;
        for (int i = 0; i < 128; i++)
            oam[i].affineMode = 2;
        oam[0].affineMode = 0;
        oam[0].x = 0;
        oam[0].y = 40;
        oam[0].tileNum = 0;
        oam[0].priority = 3;
        oam[1].affineMode = 0;
        oam[1].x = 235;
        oam[1].y = 40;
        oam[1].tileNum = 1;
        oam[1].priority = 0;
        oam[2].affineMode = 0;
        oam[2].x = 100;
        oam[2].y = 40;
        oam[2].tileNum = 2; /* Transparent sprite must not widen a priority range. */
        oam[2].priority = 2;
        break;
    }
}

static bool SpriteCompositorFrameParityCase(const char *name, unsigned int scene,
                                             enum RasterEvent event)
{
    static uint16_t fullScanFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t boundedFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    unsigned int fullVCountCallbacks;
    uint16_t fullVCountCallbackStatusFlags;
    unsigned int fullHBlankCallbacks;
    unsigned int fullDmaCalls;
    uint16_t fullDispstat;
    uint16_t fullVcount;

    ResetFrameState(event);
    ConfigureSpriteCompositorScene(scene);
    sForceFullSpriteCompositor = true;
    sForceLegacyBgCompositor = true;
    sForceLegacyLayerClears = true;
    DrawFrameSerial(fullScanFrame);
    fullVCountCallbacks = sVCountCallbacks;
    fullVCountCallbackStatusFlags = sVCountCallbackStatusFlags;
    fullHBlankCallbacks = sHBlankCallbacks;
    fullDmaCalls = sDmaCalls;
    fullDispstat = REG16(REG_OFFSET_DISPSTAT);
    fullVcount = REG16(REG_OFFSET_VCOUNT);

    ResetFrameState(event);
    ConfigureSpriteCompositorScene(scene);
    sForceFullSpriteCompositor = false;
    sForceLegacyBgCompositor = false;
    sForceLegacyLayerClears = false;
    DrawFrameSerial(boundedFrame);
    if (memcmp(fullScanFrame, boundedFrame, sizeof(fullScanFrame)) != 0
     || fullVCountCallbacks != sVCountCallbacks
     || fullVCountCallbackStatusFlags != sVCountCallbackStatusFlags
     || fullHBlankCallbacks != sHBlankCallbacks
     || fullDmaCalls != sDmaCalls
     || fullDispstat != REG16(REG_OFFSET_DISPSTAT)
     || fullVcount != REG16(REG_OFFSET_VCOUNT))
    {
        size_t offset;
        for (offset = 0; offset < sizeof(fullScanFrame)
             && ((uint8_t *)fullScanFrame)[offset] == ((uint8_t *)boundedFrame)[offset]; offset++)
            ;
        fprintf(stderr,
                "%s sprite compositor reference differs (scene=%u event=%u byte=%zu, VCOUNT=%u/%u HBlank=%u/%u DMA=%u/%u)\n",
                name, scene, event, offset, fullVCountCallbacks, sVCountCallbacks,
                fullHBlankCallbacks, sHBlankCallbacks, fullDmaCalls, sDmaCalls);
        sForceFullSpriteCompositor = false;
        return false;
    }
    return true;
}

static void ConfigureRandomBgCompositorScene(unsigned int scene)
{
    sRng = 0x9374A4Du ^ (scene * 0x9E3779B9u);
    // Different enabled sets, priorities, scroll, 4/8bpp, flips and map sizes;
    // the existing fixture also supplies transparency and multiple OBJ sprites.
    unsigned int flags = NextByte();
    REG16(REG_OFFSET_DISPCNT) = (REG16(REG_OFFSET_DISPCNT) & ~0xFF00)
        | ((flags & 15) << 8) | ((flags & 128) ? DISPCNT_OBJ_ON : 0)
        | ((flags & 16) ? DISPCNT_WIN0_ON : 0)
        | ((flags & 32) ? DISPCNT_WIN1_ON : 0)
        | ((flags & 64) ? DISPCNT_OBJWIN_ON : 0);
    for (unsigned int bg = 0; bg < 4; bg++)
    {
        uint16_t original = REG16(REG_OFFSET_BG0CNT + bg * 2);
        REG16(REG_OFFSET_BG0CNT + bg * 2) = (original & ~(3u | 0xC0C0u))
            | (NextByte() & 3) | ((NextByte() & 3) << 14)
            | ((NextByte() & 1) ? BGCNT_256COLOR : 0)
            | ((NextByte() & 1) ? BGCNT_MOSAIC : 0);
        REG16(REG_OFFSET_BG0HOFS + bg * 4) = (NextByte() << 1) | (NextByte() & 1);
        REG16(REG_OFFSET_BG0VOFS + bg * 4) = (NextByte() << 1) | (NextByte() & 1);
    }
    REG16(REG_OFFSET_BLDCNT) = (NextByte() & 0x3F)
        | ((NextByte() & 3) << 6) | ((NextByte() & 0x3F) << 8);
    REG16(REG_OFFSET_BLDALPHA) = (NextByte() % 17) | ((NextByte() % 17) << 8);
    REG16(REG_OFFSET_BLDY) = NextByte() % 17;
    REG16(REG_OFFSET_WININ) = (NextByte() & 0x3F) | ((NextByte() & 0x3F) << 8);
    REG16(REG_OFFSET_WINOUT) = (NextByte() & 0x3F) | ((NextByte() & 0x3F) << 8);
}

static bool TestRandomBgCompositorParity(void)
{
    static uint16_t legacy[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t optimized[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    for (unsigned int scene = 0; scene < 140; scene++)
    {
        enum RasterEvent event = (enum RasterEvent)(scene % (RASTER_EVENT_STOPPED_HBLANK_DMA + 1));
        ResetFrameState(event);
        ConfigureRandomBgCompositorScene(scene);
        sForceLegacyBgCompositor = true;
        sForceLegacyLayerClears = true;
        DrawFrameSerial(legacy);
        unsigned int vcount = sVCountCallbacks, hblank = sHBlankCallbacks, dma = sDmaCalls;
        uint16_t status = REG16(REG_OFFSET_DISPSTAT);

        ResetFrameState(event);
        ConfigureRandomBgCompositorScene(scene);
        sForceLegacyBgCompositor = false;
        sForceLegacyLayerClears = false;
        DrawFrameSerial(optimized);
        if (memcmp(legacy, optimized, sizeof(legacy)) != 0 || vcount != sVCountCallbacks
         || hblank != sHBlankCallbacks || dma != sDmaCalls || status != REG16(REG_OFFSET_DISPSTAT))
        {
            fprintf(stderr, "random BG compositor parity failed at scene %u event %u\n", scene, event);
            return false;
        }
    }
    puts("background compositor parity: 140 seeded mixed-priority, transparency, window, effects and raster scenes passed");
    return true;
}

static bool BenchmarkLayerClears(void)
{
    static uint16_t outputFrames[2][DISPLAY_WIDTH * DISPLAY_HEIGHT];
    const uint16_t enabled[] = {DISPCNT_BG0_ON, DISPCNT_BG0_ON | DISPCNT_BG1_ON | DISPCNT_BG2_ON | DISPCNT_OBJ_ON,
                                DISPCNT_BG0_ON | DISPCNT_BG1_ON | DISPCNT_BG2_ON | DISPCNT_BG3_ON | DISPCNT_OBJ_ON,
                                DISPCNT_BG0_ON | DISPCNT_BG1_ON | DISPCNT_BG2_ON | DISPCNT_BG3_ON | DISPCNT_OBJ_ON};
    const int benchmarkFrames = 500;
    for (unsigned int scene = 0; scene < 4; scene++)
    {
        double timings[2];
        ResetFrameState(RASTER_EVENT_NONE);
        REG16(REG_OFFSET_DISPCNT) = DISPCNT_OBJ_1D_MAP | enabled[scene];
        if (scene == 3)
        {
            struct OamData *oam = (struct OamData *)OAM;
            for (int i = 0; i < 4; i++)
                oam[i].priority = 2;
        }
        for (int mode = 0; mode < 2; mode++)
        {
            sForceLegacyLayerClears = mode == 0;
            sForceLegacySimpleSpritePath = mode == 0;
            double start = Now();
            for (int frame = 0; frame < benchmarkFrames; frame++)
                DrawFrameSerial(outputFrames[mode]);
            timings[mode] = (Now() - start) * 1e6 / benchmarkFrames;
        }
        if (memcmp(outputFrames[0], outputFrames[1], sizeof(outputFrames[0])) != 0)
        {
            fprintf(stderr, "host layer clear scene %u frame output changed\n", scene);
            sForceLegacyLayerClears = false;
            sForceLegacySimpleSpritePath = false;
            return false;
        }
        printf("host PPU fast paths scene=%u%s legacy/selected %.1f/%.1f us (%.1f%% difference)\n",
               scene, scene == 3 ? " (one OBJ priority)" : "", timings[0], timings[1],
               (timings[0] - timings[1]) * 100 / timings[0]);
    }
    sForceLegacyLayerClears = false;
    sForceLegacySimpleSpritePath = false;
    return true;
}

static bool TestSpriteCompositorFullFrameParity(void)
{
    if (!SpriteCompositorFrameParityCase("4bpp priorities and normal sprites", 0, RASTER_EVENT_NONE)
     || !SpriteCompositorFrameParityCase("window, mosaic, blend, semi-transparent sprite", 1, RASTER_EVENT_NONE)
     || !SpriteCompositorFrameParityCase("affine BG, mosaic and darken", 2, RASTER_EVENT_NONE)
     || !SpriteCompositorFrameParityCase("OBJ window and window clipping", 3, RASTER_EVENT_NONE)
     || !SpriteCompositorFrameParityCase("8bpp BGs and brighten", 4, RASTER_EVENT_NONE)
     || !SpriteCompositorFrameParityCase("transparent sprite pixels at x=0 and x=239", 5, RASTER_EVENT_NONE)
     || !SpriteCompositorFrameParityCase("HBlank DMA raster changes", 0, RASTER_EVENT_HBLANK_DMA0)
     || !SpriteCompositorFrameParityCase("VCOUNT enables HBlank DMA", 1, RASTER_EVENT_VCOUNT_IRQ_150_ENABLE_HBLANK_DMA)
     || !SpriteCompositorFrameParityCase("HBlank IRQ raster changes", 1, RASTER_EVENT_HBLANK_IRQ))
        return false;
    puts("full-frame background and sprite compositor parity: 6 scenes and 3 dynamic raster cases passed");
    return true;
}

static bool SpritePriorityMutationParityCase(const char *name, enum RasterEvent event,
                                              bool mutateAtVCount)
{
    static uint16_t legacy[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t optimized[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    unsigned int legacyVCountCallbacks;
    unsigned int legacyHBlankCallbacks;

    ResetFrameState(event);
    ConfigureSpriteCompositorScene(0);
    sMutateSpritePriorityAtVCount = mutateAtVCount;
    sMutateSpritePriorityAtHBlank = !mutateAtVCount;
    sForceLegacyLayerClears = true;
    DrawFrameSerial(legacy);
    legacyVCountCallbacks = sVCountCallbacks;
    legacyHBlankCallbacks = sHBlankCallbacks;

    ResetFrameState(event);
    ConfigureSpriteCompositorScene(0);
    sMutateSpritePriorityAtVCount = mutateAtVCount;
    sMutateSpritePriorityAtHBlank = !mutateAtVCount;
    sForceLegacyLayerClears = false;
    DrawFrameSerial(optimized);

    bool callbacksCanMutateVisibleOam = event == RASTER_EVENT_HBLANK_IRQ
        || (event == RASTER_EVENT_VCOUNT_IRQ_0 && mutateAtVCount);
    bool expectedSafe = !callbacksCanMutateVisibleOam;
    if (sSpritePriorityMaskSafe != expectedSafe
     || sVCountCallbacks != legacyVCountCallbacks
     || sHBlankCallbacks != legacyHBlankCallbacks
     || memcmp(legacy, optimized, sizeof(legacy)) != 0)
    {
        fprintf(stderr, "%s sparse sprite-plane fallback/parity failed\n", name);
        sForceLegacyLayerClears = false;
        return false;
    }

    sForceLegacyLayerClears = false;
    printf("%s: full sprite-plane clears retained when OAM priority changes during scanout\n", name);
    return true;
}

static bool TestSparseSpritePriorityPlanes(void)
{
    static uint16_t pixels[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    ResetFrameState(RASTER_EVENT_NONE);
    ConfigureSpriteCompositorScene(5);
    sForceLegacyLayerClears = false;
    DrawFrameSerial(pixels);
    if (!sSpritePriorityMaskSafe
     || sSpriteBins.priorityMask[40] != ((1u << 0) | (1u << 2) | (1u << 3))
     || sSpriteBins.priorityMask[48] != (1u << 2)
     || sSpriteBins.priorityMask[72] != 0)
    {
        fprintf(stderr, "per-scanline sprite priority mask did not track active and transparent sprites\n");
        return false;
    }

    puts("per-scanline sprite priority mask: disjoint and transparent sprite rows tracked");
    if (!SpriteCompositorFrameParityCase("sparse OBJ priority planes and transparent sprites",
                                         5, RASTER_EVENT_NONE)
     || !SpritePriorityMutationParityCase("VCOUNT priority transition",
                                          RASTER_EVENT_VCOUNT_IRQ_0, true)
     || !SpritePriorityMutationParityCase("HBlank priority transitions",
                                          RASTER_EVENT_HBLANK_IRQ, false))
        return false;
    return true;
}

static bool TestSimpleSpriteCompositionPathParity(void)
{
    static uint16_t legacy[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t optimized[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static const unsigned int scenes[] = {0, 1, 3, 4, 5};

    for (unsigned int i = 0; i < sizeof(scenes) / sizeof(scenes[0]); i++)
    {
        ResetFrameState(RASTER_EVENT_NONE);
        ConfigureSpriteCompositorScene(scenes[i]);
        sForceLegacySimpleSpritePath = true;
        DrawFrameSerial(legacy);

        ResetFrameState(RASTER_EVENT_NONE);
        ConfigureSpriteCompositorScene(scenes[i]);
        sForceLegacySimpleSpritePath = false;
        DrawFrameSerial(optimized);
        if (memcmp(legacy, optimized, sizeof(legacy)) != 0)
        {
            fprintf(stderr, "simple sprite composition path differs in scene %u\n", scenes[i]);
            sForceLegacySimpleSpritePath = false;
            return false;
        }
    }

    puts("simple normal-sprite composition: full-frame parity passed for unblended, windowed, affine, blended and transparent-edge scenes");
    return true;
}

static void ConfigureSpriteScanlinePrefilterStress(void)
{
    static const uint8_t edgeY[] = {0, 1, 7, 8, 31, 63, 127, 158, 159, 160, 200, 255};
    struct OamData *oam = (struct OamData *)OAM;

    REG16(REG_OFFSET_DISPCNT) |= DISPCNT_WIN0_ON | DISPCNT_WIN1_ON | DISPCNT_OBJWIN_ON;
    REG16(REG_OFFSET_WIN0H) = 0x30D0;
    REG16(REG_OFFSET_WIN0V) = 0x2090;
    REG16(REG_OFFSET_WIN1H) = 0xA050;
    REG16(REG_OFFSET_WIN1V) = 0x4080;
    REG16(REG_OFFSET_WININ) = 0x3F3F;
    REG16(REG_OFFSET_WINOUT) = 0x3F3F;
    REG16(REG_OFFSET_MOSAIC) = 0x4321;
    REG16(REG_OFFSET_BLDCNT) = BLDCNT_TGT1_BG_ALL | BLDCNT_TGT1_OBJ
                              | BLDCNT_EFFECT_BLEND | BLDCNT_TGT2_BG_ALL
                              | BLDCNT_TGT2_OBJ | BLDCNT_TGT2_BD;
    REG16(REG_OFFSET_BLDALPHA) = 10 | (5 << 8);

    for (int i = 0; i < 128; i++)
    {
        oam[i].affineMode = i % 13 == 0 ? 3 : i % 5 == 0 ? 1 : i % 7 == 0 ? 2 : 0;
        oam[i].objMode = i % 11 == 0 ? 2 : i % 9 == 0 ? 1 : 0;
        oam[i].mosaic = i % 6 == 0;
        oam[i].shape = i % 3;
        oam[i].size = i % 4;
        oam[i].x = (i * 47) & 0x1FF;
        oam[i].y = edgeY[i % (sizeof(edgeY) / sizeof(edgeY[0]))];
        oam[i].tileNum = (i * 4) & 0x7F;
        oam[i].priority = i % 4;
        oam[i].paletteNum = i % 16;
        oam[i].bpp = i % 2;
        oam[i].matrixNum = i % 32;
    }

    /* Give each affine matrix an identity transform with occasional shear so
     * row-boundary and transformed-row cases both contribute visible pixels. */
    for (int matrix = 0; matrix < 32; matrix++)
    {
        oam[matrix * 4].affineParam = 0x100;
        oam[matrix * 4 + 1].affineParam = matrix & 1 ? 0x20 : 0;
        oam[matrix * 4 + 2].affineParam = matrix & 2 ? 0x20 : 0;
        oam[matrix * 4 + 3].affineParam = 0x100;
    }
}

static bool TestSpriteScanlinePrefilterParity(void)
{
    static uint16_t legacyFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t optimizedFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    ResetFrameState(RASTER_EVENT_NONE);
    ConfigureSpriteScanlinePrefilterStress();
    sDisableSpriteScanlinePrefilter = true;
    sForceLegacySpritePixelPath = true;
    DrawFrameSerial(legacyFrame);

    ResetFrameState(RASTER_EVENT_NONE);
    ConfigureSpriteScanlinePrefilterStress();
    sDisableSpriteScanlinePrefilter = false;
    sForceLegacySpritePixelPath = false;
    DrawFrameSerial(optimizedFrame);

    if (memcmp(legacyFrame, optimizedFrame, sizeof(legacyFrame)) != 0)
    {
        size_t offset;
        for (offset = 0; offset < sizeof(legacyFrame)
             && ((uint8_t *)legacyFrame)[offset] == ((uint8_t *)optimizedFrame)[offset]; offset++)
            ;
        fprintf(stderr, "sprite scanline prefilter differs from legacy path at byte %zu\n", offset);
        return false;
    }

    puts("sprite scanline prefilter: legacy and optimized full frames match across affine bounds, wrap, mosaic, windows and blending");
    return true;
}

static bool BenchmarkSerialAndParallel(void)
{
    enum { WARMUP_FRAMES = 24, BENCHMARK_FRAMES = 200 };
    static uint16_t serialFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t parallelFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t warmupFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    /* Start the persistent workers before timing either path. */
    ResetFrameState(RASTER_EVENT_NONE);
    DrawFrame(warmupFrame);

    ResetFrameState(RASTER_EVENT_NONE);
    for (int i = 0; i < WARMUP_FRAMES; i++)
        DrawFrameSerial(serialFrame);
    ResetFrameState(RASTER_EVENT_NONE);
    for (int i = 0; i < WARMUP_FRAMES; i++)
        DrawFrame(parallelFrame);

    ResetFrameState(RASTER_EVENT_NONE);
    double serialStart = Now();
    for (int i = 0; i < BENCHMARK_FRAMES; i++)
        DrawFrameSerial(serialFrame);
    double serialElapsed = Now() - serialStart;

    ResetFrameState(RASTER_EVENT_NONE);
    double parallelStart = Now();
    for (int i = 0; i < BENCHMARK_FRAMES; i++)
        DrawFrame(parallelFrame);
    double parallelElapsed = Now() - parallelStart;

    if (memcmp(serialFrame, parallelFrame, sizeof(serialFrame)) != 0)
    {
        fprintf(stderr, "serial/parallel benchmark frames differ\n");
        return false;
    }

    double serialUs = serialElapsed * 1e6 / BENCHMARK_FRAMES;
    double parallelUs = parallelElapsed * 1e6 / BENCHMARK_FRAMES;
    printf("host mode-0 warmed draw average: serial %.1f us/frame, parallel %.1f us/frame (%d frames each, worker init excluded, speedup %.2fx)\n",
           serialUs, parallelUs, BENCHMARK_FRAMES,
           parallelUs == 0.0 ? 0.0 : serialUs / parallelUs);
    return true;
}

static bool BenchmarkVCountPrefixSplit(void)
{
    enum { WARMUP_FRAMES = 24, BENCHMARK_FRAMES = 200 };
    static uint16_t serialFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t splitFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t warmupFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    /* Warm the workers on the same line-150 event used by the timed split. */
    ResetFrameState(RASTER_EVENT_VCOUNT_IRQ_150);
    DrawFrame(warmupFrame);

    ResetFrameState(RASTER_EVENT_VCOUNT_IRQ_150);
    for (int i = 0; i < WARMUP_FRAMES; i++)
        DrawFrameSerial(serialFrame);
    ResetFrameState(RASTER_EVENT_VCOUNT_IRQ_150);
    for (int i = 0; i < WARMUP_FRAMES; i++)
        DrawFrame(splitFrame);

    ResetFrameState(RASTER_EVENT_VCOUNT_IRQ_150);
    double serialStart = Now();
    for (int i = 0; i < BENCHMARK_FRAMES; i++)
        DrawFrameSerial(serialFrame);
    double serialElapsed = Now() - serialStart;

    ResetFrameState(RASTER_EVENT_VCOUNT_IRQ_150);
    double splitStart = Now();
    for (int i = 0; i < BENCHMARK_FRAMES; i++)
        DrawFrame(splitFrame);
    double splitElapsed = Now() - splitStart;

    if (memcmp(serialFrame, splitFrame, sizeof(serialFrame)) != 0)
    {
        fprintf(stderr, "VCOUNT line-150 serial/split benchmark frames differ\n");
        return false;
    }

    double serialUs = serialElapsed * 1e6 / BENCHMARK_FRAMES;
    double splitUs = splitElapsed * 1e6 / BENCHMARK_FRAMES;
    printf("host mode-0 VCOUNT-line-150 warmed draw average: serial %.1f us/frame, prefix split %.1f us/frame (%d frames each, worker init excluded, speedup %.2fx)\n",
           serialUs, splitUs, BENCHMARK_FRAMES,
           splitUs == 0.0 ? 0.0 : serialUs / splitUs);
    return true;
}

static bool BenchmarkSpriteRichOverworld(void)
{
    enum { SPRITES = 24, WARMUP_FRAMES = 24, BENCHMARK_FRAMES = 200 };
    static uint16_t serialFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t parallelFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static uint16_t warmupFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    FillState(0);
    struct OamData *oam = (struct OamData *)OAM;
    for (int i = 0; i < SPRITES; i++)
    {
        oam[i].affineMode = 0;
        oam[i].shape = 0;
        oam[i].size = 1;
        oam[i].x = (i * 37) % DISPLAY_WIDTH;
        oam[i].y = (i * 23) % DISPLAY_HEIGHT;
        oam[i].tileNum = i * 8;
        oam[i].priority = i % 4;
        oam[i].paletteNum = i % 16;
        oam[i].bpp = 0;
        oam[i].objMode = 0;
    }

    /* Keep the four text BGs enabled like a typical overworld display, with
     * visible normal OBJ sprites distributed across the viewport. */
    DrawFrame(warmupFrame); // Initialize persistent workers before timing.
    for (int i = 0; i < WARMUP_FRAMES; i++)
        DrawFrameSerial(serialFrame);
    for (int i = 0; i < WARMUP_FRAMES; i++)
        DrawFrame(parallelFrame);

    double serialStart = Now();
    for (int i = 0; i < BENCHMARK_FRAMES; i++)
        DrawFrameSerial(serialFrame);
    double serialElapsed = Now() - serialStart;

    double parallelStart = Now();
    for (int i = 0; i < BENCHMARK_FRAMES; i++)
        DrawFrame(parallelFrame);
    double parallelElapsed = Now() - parallelStart;

    if (memcmp(serialFrame, parallelFrame, sizeof(serialFrame)) != 0)
    {
        fprintf(stderr, "sprite-rich overworld serial/parallel frames differ\n");
        return false;
    }

    double serialUs = serialElapsed * 1e6 / BENCHMARK_FRAMES;
    double parallelUs = parallelElapsed * 1e6 / BENCHMARK_FRAMES;
    printf("host sprite-rich overworld draw average: serial %.1f us/frame, parallel %.1f us/frame (%d normal sprites, %d frames each)\n",
           serialUs, parallelUs, SPRITES, BENCHMARK_FRAMES);

    enum { PAIRED_ROUNDS = 3, PAIRED_FRAMES = 1000 };
    double legacySerialElapsed = 0.0;
    double optimizedSerialElapsed = 0.0;
    double legacyParallelElapsed = 0.0;
    double optimizedParallelElapsed = 0.0;

    sDisableSpriteScanlinePrefilter = true;
    sForceLegacySpritePixelPath = true;
    DrawFrame(warmupFrame);
    for (int i = 0; i < WARMUP_FRAMES; i++)
        DrawFrameSerial(serialFrame);
    for (int i = 0; i < WARMUP_FRAMES; i++)
        DrawFrame(parallelFrame);

    for (int round = 0; round < PAIRED_ROUNDS; round++)
    {
        bool legacyFirst = (round & 1) == 0;
        for (int pass = 0; pass < 2; pass++)
        {
            bool legacyPath = pass == 0 ? legacyFirst : !legacyFirst;
            sDisableSpriteScanlinePrefilter = legacyPath;
            sForceLegacySpritePixelPath = legacyPath;
            uint16_t *frame = legacyPath ? serialFrame : parallelFrame;

            double start = Now();
            for (int i = 0; i < PAIRED_FRAMES; i++)
                DrawFrameSerial(frame);
            double elapsed = Now() - start;
            if (legacyPath)
                legacySerialElapsed += elapsed;
            else
                optimizedSerialElapsed += elapsed;

            start = Now();
            for (int i = 0; i < PAIRED_FRAMES; i++)
                DrawFrame(frame);
            elapsed = Now() - start;
            if (legacyPath)
                legacyParallelElapsed += elapsed;
            else
                optimizedParallelElapsed += elapsed;
        }
    }

    sDisableSpriteScanlinePrefilter = false;
    sForceLegacySpritePixelPath = false;
    if (memcmp(serialFrame, parallelFrame, sizeof(serialFrame)) != 0)
    {
        fprintf(stderr, "sprite sampler legacy/optimized frames differ\n");
        return false;
    }

    double timedFrames = PAIRED_ROUNDS * PAIRED_FRAMES;
    double legacySerialUs = legacySerialElapsed * 1e6 / timedFrames;
    double optimizedSerialUs = optimizedSerialElapsed * 1e6 / timedFrames;
    double legacyParallelUs = legacyParallelElapsed * 1e6 / timedFrames;
    double optimizedParallelUs = optimizedParallelElapsed * 1e6 / timedFrames;
    printf("host sprite sampler legacy/optimized: serial %.1f/%.1f us (%.1f%% faster), parallel %.1f/%.1f us (%.1f%% faster), %d normal sprites\n",
           legacySerialUs, optimizedSerialUs,
           legacySerialUs == 0.0 ? 0.0 : 100.0 * (legacySerialUs - optimizedSerialUs) / legacySerialUs,
           legacyParallelUs, optimizedParallelUs,
           legacyParallelUs == 0.0 ? 0.0 : 100.0 * (legacyParallelUs - optimizedParallelUs) / legacyParallelUs,
           SPRITES);

    enum { SIMPLE_ROUNDS = 3, SIMPLE_FRAMES = 500 };
    double simpleElapsed[2] = {0.0, 0.0};
    for (int round = 0; round < SIMPLE_ROUNDS; round++)
    {
        for (int pass = 0; pass < 2; pass++)
        {
            int mode = (round & 1) ? 1 - pass : pass;
            sForceLegacySimpleSpritePath = mode == 0;
            for (int i = 0; i < WARMUP_FRAMES; i++)
                DrawFrameSerial(warmupFrame);

            double start = Now();
            for (int i = 0; i < SIMPLE_FRAMES; i++)
                DrawFrameSerial(mode == 0 ? serialFrame : parallelFrame);
            simpleElapsed[mode] += Now() - start;
        }
    }
    sForceLegacySimpleSpritePath = false;
    if (memcmp(serialFrame, parallelFrame, sizeof(serialFrame)) != 0)
    {
        fprintf(stderr, "simple normal-sprite composition benchmark frames differ\n");
        return false;
    }
    double legacySimpleUs = simpleElapsed[0] * 1e6 / (SIMPLE_ROUNDS * SIMPLE_FRAMES);
    double optimizedSimpleUs = simpleElapsed[1] * 1e6 / (SIMPLE_ROUNDS * SIMPLE_FRAMES);
    printf("host unblended normal-sprite path: legacy/selected %.1f/%.1f us (%.1f%% faster), %d normal sprites\n",
           legacySimpleUs, optimizedSimpleUs,
           legacySimpleUs == 0.0 ? 0.0 : 100.0 * (legacySimpleUs - optimizedSimpleUs) / legacySimpleUs,
           SPRITES);
    return true;
}

int main(void)
{
    static const uint16_t textMapSizeBits[] = {
        BGCNT_TXT256x256,
        BGCNT_TXT512x256,
        BGCNT_TXT256x512,
        BGCNT_TXT512x512,
    };
    uint16_t referenceLine[DISPLAY_WIDTH];
    uint16_t optimizedLine[DISPLAY_WIDTH];
    int failures = 0;
    unsigned int cases = 0;

    for (unsigned int size = 0; size < sizeof(textMapSizeBits) / sizeof(textMapSizeBits[0]); size++)
    {
        for (unsigned int bpp = 0; bpp < 2; bpp++)
        {
            for (unsigned int mosaic = 0; mosaic < 2; mosaic++)
            {
                for (unsigned int scroll = 0; scroll < 2; scroll++)
                {
                    FillState(size * 8 + bpp * 4 + mosaic * 2 + scroll);
                    uint16_t control = BGCNT_PRIORITY(1) | BGCNT_CHARBASE(0) | BGCNT_SCREENBASE(12)
                        | textMapSizeBits[size] | (bpp ? BGCNT_256COLOR : BGCNT_16COLOR)
                        | (mosaic ? BGCNT_MOSAIC : 0);
                    uint16_t hofs = scroll ? 500 : 250;
                    uint16_t voffs = scroll ? 500 : 250;
                    int lineNum = scroll ? 20 : 10;
                    REG16(REG_OFFSET_MOSAIC) = mosaic ? 0x32A4 : 0x3210;

                    for (unsigned int x = 0; x < DISPLAY_WIDTH; x++)
                        referenceLine[x] = optimizedLine[x] = 0x5A5A;
                    ReferenceRenderBGScanline(0, control, hofs, voffs, lineNum, referenceLine);
                    RenderBGScanline(0, control, hofs, voffs, lineNum, optimizedLine);
                    cases++;

                    if (memcmp(referenceLine, optimizedLine, sizeof(referenceLine)) != 0)
                    {
                        unsigned int x;
                        for (x = 0; x < DISPLAY_WIDTH && referenceLine[x] == optimizedLine[x]; x++)
                            ;
                        fprintf(stderr,
                                "size=%u bpp=%u mosaic=%u scroll=%u differs at x=%u: old=%04x new=%04x\n",
                                size, bpp ? 8 : 4, mosaic, scroll, x, referenceLine[x], optimizedLine[x]);
                        failures++;
                    }
                }
            }
        }
    }

    if (failures != 0)
        return 1;

    printf("scanline parity: %u cases passed (256x256, 512x256, 256x512, 512x512 text maps; 4/8bpp; mosaic and scroll/wrap)\n",
           cases);
    if (!RenderParityCase(RASTER_EVENT_NONE, "parallel stable frame", 1)
     || !TestParallelWin0hHBlankDmaParity()
     || !TestWin0hDmaVcountBoundary(160, true)
     || !TestWin0hDmaVcountBoundary(150, false)
     || !TestWin0hDmaHblankInterruptFallback()
     || !TestWin0hDmaWorkerFailureRestore()
     || !RenderParityCase(RASTER_EVENT_HBLANK_IRQ, "HBlank IRQ fallback", DISPLAY_HEIGHT)
     || !RenderParityCase(RASTER_EVENT_VCOUNT_IRQ_0, "VCOUNT IRQ line 0 fallback", DISPLAY_HEIGHT)
     || !RenderParityCase(RASTER_EVENT_VCOUNT_IRQ_150, "VCOUNT IRQ line 150 prefix split", 1 + DISPLAY_HEIGHT - 150)
     || !RenderParityCase(RASTER_EVENT_VCOUNT_IRQ_150_STALE_STATUS,
                          "VCOUNT IRQ line 150 clears seeded HBlank/VCOUNT status before callback",
                          1 + DISPLAY_HEIGHT - 150)
     || !RenderParityCase(RASTER_EVENT_VCOUNT_IRQ_150_ENABLE_HBLANK_DMA,
                          "VCOUNT IRQ line 150 enables HBlank DMA for serial suffix",
                          1 + DISPLAY_HEIGHT - 150)
     || !RenderParityCase(RASTER_EVENT_VCOUNT_IRQ_150_ENABLE_HBLANK_IRQ,
                          "VCOUNT IRQ line 150 enables HBlank IRQ for serial suffix",
                          1 + DISPLAY_HEIGHT - 150)
     || !RenderParityCase(RASTER_EVENT_VCOUNT_IRQ_159, "VCOUNT IRQ line 159 prefix split", 1 + DISPLAY_HEIGHT - 159)
     || !RenderParityCase(RASTER_EVENT_VCOUNT_IRQ_160, "VCOUNT IRQ line 160 parallel", 1))
        return 1;
    for (int dma = 0; dma < DMA_COUNT; dma++)
    {
        char name[40];
        snprintf(name, sizeof(name), "HBlank DMA channel %d fallback", dma);
        if (!RenderParityCase((enum RasterEvent)(RASTER_EVENT_HBLANK_DMA0 + dma), name, DISPLAY_HEIGHT))
            return 1;
    }
    if (!RenderParityCase(RASTER_EVENT_STOPPED_HBLANK_DMA, "stopped DMA normalization", 1)
     || !TestWin1Coordinates()
     || !TestSpriteLayerBounds()
     || !TestSpriteCompositorFullFrameParity()
     || !TestRandomBgCompositorParity()
     || !TestSparseSpritePriorityPlanes()
     || !TestSimpleSpriteCompositionPathParity()
     || !TestSpriteScanlinePrefilterParity()
     || !TestWorkerOverlapAndLifecycle()
     || !BenchmarkSerialAndParallel()
     || !BenchmarkVCountPrefixSplit()
     || !BenchmarkSpriteRichOverworld()
     || !BenchmarkLayerClears())
        return 1;
    printf("frame parity: stable parallel draw and raster-event serial fallbacks passed\n");
    printf("raster coverage: HBlank DMA channels 0-3; VCOUNT line 0 fallback, lines 150/159 prefix split, line 160 parallel\n");
    printf("worker checks: main plus 2 workers overlapped, exact row coverage, restart and first/partial-init fallback passed\n");
    return 0;
}
