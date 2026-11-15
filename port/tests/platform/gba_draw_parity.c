#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "global.h"
#include "platform/dma.h"

unsigned char REG_BASE[0x400] __attribute__((aligned(4)));
unsigned char PLTT[PLTT_SIZE] __attribute__((aligned(4)));
unsigned char VRAM_[VRAM_SIZE] __attribute__((aligned(4)));
unsigned char OAM[OAM_SIZE] __attribute__((aligned(4)));
struct SoundInfo *SOUND_INFO_PTR;
unsigned short INTR_CHECK;
void *INTR_VECTOR;

void DrawFrameEasy(uint16_t *pixels);
void DrawFrameFast(uint16_t *pixels);

#define REG16(offset) (*(uint16_t *)(REG_BASE + (offset)))
#define REG32(offset) (*(uint32_t *)(REG_BASE + (offset)))

static uint32_t sRng;
static uint16_t sInitialRegs[0x200];
static uint8_t sInitialPalette[PLTT_SIZE];
static uint8_t sInitialVram[VRAM_SIZE];
static uint8_t sInitialOam[OAM_SIZE];
static uint16_t sEasyFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static uint16_t sFastFrame[DISPLAY_WIDTH * DISPLAY_HEIGHT];
static unsigned int sRasterMode;
static unsigned int sRasterLine;
static bool sSimpleCase;
static bool sForce4bpp;
static bool sForceMode0;

static uint8_t NextByte(void)
{
    sRng = sRng * 1664525u + 1013904223u;
    return sRng >> 24;
}

static uint16_t NextColor(void)
{
    return (NextByte() & 31) | ((NextByte() & 31) << 5) | ((NextByte() & 31) << 10);
}

static void NoInterrupt(void) { }

static void RasterVCountInterrupt(void)
{
    // Model common per-scanline register changes made by game callbacks.
    if (sRasterMode)
        REG16(REG_OFFSET_BG0HOFS) = (REG16(REG_OFFSET_BG0HOFS) + 3) & 0x1FF;
}

static void RasterHBlankInterrupt(void)
{
    if (!sRasterMode)
        return;

    sRasterLine++;
    REG16(REG_OFFSET_BG1HOFS) = (REG16(REG_OFFSET_BG1HOFS) + 5) & 0x1FF;
    if (sRasterMode > 1 && (sRasterLine & 7) == 0)
    {
        REG16(REG_OFFSET_BLDCNT) ^= BLDCNT_TGT1_BG0 | BLDCNT_TGT1_BG1;
        REG16(REG_OFFSET_BLDALPHA) = ((sRasterLine & 15) << 8) | (16 - (sRasterLine & 15));
    }
}

void (*const gIntrTable[])(void) = {
    RasterVCountInterrupt,
    NoInterrupt,
    NoInterrupt,
    RasterHBlankInterrupt,
};

void RunDMAs(u32 type)
{
    if (type == DMA_HBLANK && sRasterMode > 1)
        REG16(REG_OFFSET_BG2HOFS) = (REG16(REG_OFFSET_BG2HOFS) + 1) & 0x1FF;
}

static void FillState(unsigned int scenario)
{
    sRng = 0xC001D00D ^ (scenario * 0x9E3779B9u);
    memset(REG_BASE, 0, sizeof(REG_BASE));
    memset(PLTT, 0, sizeof(PLTT));
    memset(VRAM_, 0, sizeof(VRAM_));
    memset(OAM, 0, sizeof(OAM));

    for (unsigned int i = 0; i < PLTT_SIZE / 2; i++)
    {
        uint16_t color = NextColor();
        memcpy(PLTT + i * 2, &color, sizeof(color));
    }
    for (unsigned int i = 0; i < VRAM_SIZE; i++)
        VRAM_[i] = NextByte();

    unsigned int mode = !sSimpleCase && !sForceMode0 && (scenario % 5 == 1) ? 1 : 0;
    unsigned int objWin = !sSimpleCase && scenario % 5 == 4;
    unsigned int win0 = !sSimpleCase && (scenario % 5 == 2 || scenario % 5 == 4);
    unsigned int win1 = !sSimpleCase && scenario % 5 == 3;
    unsigned int blendKind = sSimpleCase ? 0 : (scenario / 5) % 4;
    REG16(REG_OFFSET_DISPCNT) = (uint16_t)(mode | (sSimpleCase ? 0x0100 : 0x1F00) | (sSimpleCase ? 0 : DISPCNT_OBJ_ON)
        | DISPCNT_OBJ_1D_MAP | (win0 ? DISPCNT_WIN0_ON : 0)
        | (win1 ? DISPCNT_WIN1_ON : 0) | (objWin ? DISPCNT_OBJWIN_ON : 0));

    for (unsigned int bg = 0; bg < 4; bg++)
    {
        unsigned int priority = (bg + scenario) & 3;
        unsigned int mapSize = (scenario + bg) & 3;
        unsigned int screenBase = 12 + bg * 4;
        uint16_t control = BGCNT_PRIORITY(sSimpleCase ? 0 : priority) | BGCNT_CHARBASE(0)
            | BGCNT_SCREENBASE(sSimpleCase ? 12 : screenBase) | (uint16_t)((sSimpleCase ? 0 : mapSize) << 14);
        if (!sSimpleCase && !sForce4bpp && ((scenario + bg) & 1) != 0)
            control |= BGCNT_256COLOR;
        if (!sSimpleCase && ((scenario + bg / 2) & 2) != 0)
            control |= BGCNT_MOSAIC;
        REG16(REG_OFFSET_BG0CNT + bg * 2) = control;
        REG16(REG_OFFSET_BG0HOFS + bg * 4) = sSimpleCase ? 0 : (uint16_t)((scenario * 47 + bg * 83 + 251) & 0x1FF);
        REG16(REG_OFFSET_BG0VOFS + bg * 4) = sSimpleCase ? 0 : (uint16_t)((scenario * 29 + bg * 61 + 243) & 0x1FF);

        // Populate each 32x32 screen base tilemap. In the wide/tall cases the
        // adjacent screen bases are supplied by the same deterministic data.
        uint16_t *map = (uint16_t *)(VRAM_ + screenBase * BG_SCREEN_SIZE);
        for (unsigned int entry = 0; entry < 0x400; entry++)
        {
            uint16_t tile = NextByte() % 128;
            uint16_t palette = NextByte() & 15;
            uint16_t flips = (NextByte() & 3) << 10;
            map[entry] = tile | palette << 12 | flips;
        }
    }

    REG16(REG_OFFSET_MOSAIC) = (uint16_t)(0x3210 ^ (scenario * 0x0111));
    REG16(REG_OFFSET_WIN0H) = 0x30D0;
    REG16(REG_OFFSET_WIN0V) = 0x20B0;
    REG16(REG_OFFSET_WIN1H) = 0x7090;
    REG16(REG_OFFSET_WIN1V) = 0x5080;
    REG16(REG_OFFSET_WININ) = 0x1F1F;
    REG16(REG_OFFSET_WINOUT) = 0x3F3F;
    REG16(REG_OFFSET_BLDCNT) = (uint16_t)(blendKind == 0 ? 0
        : (BLDCNT_TGT1_BG0 | BLDCNT_TGT1_BG1 | BLDCNT_TGT1_OBJ
            | BLDCNT_TGT2_BG2 | BLDCNT_TGT2_OBJ | (blendKind << 6)));
    REG16(REG_OFFSET_BLDALPHA) = 8 | (8 << 8);
    REG16(REG_OFFSET_BLDY) = 8;
    REG16(REG_OFFSET_BG2PA) = 0x100;
    REG16(REG_OFFSET_BG2PB) = (scenario & 1) ? 0x20 : 0;
    REG16(REG_OFFSET_BG2PC) = (scenario & 2) ? 0x10 : 0;
    REG16(REG_OFFSET_BG2PD) = 0x100;
    REG32(REG_OFFSET_BG2X) = 0x1800;
    REG32(REG_OFFSET_BG2Y) = 0x1000;

    struct OamData *oam = (struct OamData *)OAM;
    for (int i = 0; i < 128; i++)
        oam[i].affineMode = 2;
    for (int i = 0; i < (sSimpleCase ? 0 : 12); i++)
    {
        oam[i].affineMode = ((scenario + i) % 3 == 0) ? 1 : 0;
        oam[i].shape = i % 3;
        oam[i].size = (scenario + i) % 3;
        oam[i].x = (i * 29 + scenario * 7 + 20) & 0x1FF;
        oam[i].y = (i * 17 + scenario * 11 + 12) & 0xFF;
        oam[i].tileNum = i * 4;
        oam[i].priority = (i + scenario) & 3;
        oam[i].paletteNum = (i + scenario) & 15;
        oam[i].bpp = ((i + scenario) & 1);
        oam[i].objMode = (objWin && i == 0) ? 2 : ((i % 4 == 0) ? 1 : 0);
        oam[i].mosaic = (i % 3 == 1);
        if (!(oam[i].affineMode & 1))
            oam[i].matrixNum = ((i & 1) ? 8 : 16) | ((scenario & 1) ? 16 : 0);
    }
    for (int i = 0; i < 32; i++)
    {
        struct OamData *matrix = &oam[i * 4];
        matrix[0].affineParam = (i & 1) ? 0x100 : 0xE0;
        matrix[1].affineParam = (i & 2) ? 0x20 : 0;
        matrix[2].affineParam = (i & 4) ? 0x10 : 0;
        matrix[3].affineParam = 0x100;
    }

    sRasterMode = sSimpleCase ? 0 : scenario % 3;
    sRasterLine = 0;
    REG16(REG_OFFSET_DISPSTAT) = (uint16_t)(sRasterMode
        ? DISPSTAT_VCOUNT_INTR | DISPSTAT_HBLANK_INTR | (40 << 8) : 0);
}

static void SaveInitialState(void)
{
    memcpy(sInitialRegs, REG_BASE, sizeof(REG_BASE));
    memcpy(sInitialPalette, PLTT, sizeof(PLTT));
    memcpy(sInitialVram, VRAM_, sizeof(VRAM_));
    memcpy(sInitialOam, OAM, sizeof(OAM));
}

static void RestoreInitialState(void)
{
    memcpy(REG_BASE, sInitialRegs, sizeof(REG_BASE));
    memcpy(PLTT, sInitialPalette, sizeof(PLTT));
    memcpy(VRAM_, sInitialVram, sizeof(VRAM_));
    memcpy(OAM, sInitialOam, sizeof(OAM));
}

static void ClearFastFixtureState(void)
{
    memset(REG_BASE, 0, sizeof(REG_BASE));
    memset(PLTT, 0, sizeof(PLTT));
    memset(VRAM_, 0, sizeof(VRAM_));
    memset(OAM, 0, sizeof(OAM));
    sRasterMode = 0;
    sRasterLine = 0;
}

// These checks assert hardware rules directly; they intentionally do not use
// EASY as the expected-output oracle.
static bool RunFastExpectedFixtures(void)
{
    ClearFastFixtureState();
    uint16_t *palette = (uint16_t *)PLTT;
    uint16_t *bg0Map = (uint16_t *)(VRAM_ + 12 * BG_SCREEN_SIZE);
    uint16_t *bg1Map = (uint16_t *)(VRAM_ + 13 * BG_SCREEN_SIZE);
    palette[0] = 0x001F; // backdrop red
    palette[1] = 0x03E0; // green
    palette[2] = 0x7C00; // opaque blue on BG0
    bg0Map[0] = 1;
    bg1Map[0] = 2;
    VRAM_[1 * 64] = 0; // 8bpp BG0 color zero is transparent
    VRAM_[1 * 64 + 1] = 2;
    VRAM_[2 * 64] = 1;
    VRAM_[2 * 64 + 1] = 1;
    REG16(REG_OFFSET_BG0CNT) = BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0)
        | BGCNT_SCREENBASE(12) | BGCNT_256COLOR;
    REG16(REG_OFFSET_BG1CNT) = BGCNT_PRIORITY(1) | BGCNT_CHARBASE(0)
        | BGCNT_SCREENBASE(13) | BGCNT_256COLOR;
    REG16(REG_OFFSET_DISPCNT) = DISPCNT_BG0_ON | DISPCNT_BG1_ON;
    DrawFrameFast(sFastFrame);
    if (sFastFrame[0] != 0x83E0)
    {
        fprintf(stderr, "FAST 8bpp transparency fixture failed: expected=83e0 got=%04x\n",
                sFastFrame[0]);
        return false;
    }
    // Exercise both optimized paths independently against literal expected
    // colors: zero reveals BG1, while a nonzero BG0 pixel remains opaque.
    // Mosaic size 1 preserves coordinates while selecting the mosaic branch.
    for (unsigned int mosaic = 0; mosaic < 2; ++mosaic)
    {
        if (mosaic)
            REG16(REG_OFFSET_BG0CNT) |= BGCNT_MOSAIC;
        DrawFrameEasy(sEasyFrame);
        if (sEasyFrame[0] != 0x83E0 || sEasyFrame[1] != 0xFC00)
        {
            fprintf(stderr, "EASY 8bpp transparency fixture failed mosaic=%u: got=%04x,%04x\n",
                    mosaic, sEasyFrame[0], sEasyFrame[1]);
            return false;
        }
    }

    ClearFastFixtureState();
    palette = (uint16_t *)PLTT;
    bg0Map = (uint16_t *)(VRAM_ + 12 * BG_SCREEN_SIZE);
    bg1Map = (uint16_t *)(VRAM_ + 13 * BG_SCREEN_SIZE);
    palette[1] = 0x03E0; // green underlay
    palette[2] = 0x001F; // OBJ-area sentinel, which BGs cannot fetch
    palette[3] = 0x7C1F; // last valid BG byte
    bg0Map[0] = 256;     // first 8bpp tile in the OBJ character area
    bg0Map[1] = 255;     // last 8bpp tile in BG VRAM
    bg0Map[2] = 1023;    // would address beyond the 96 KiB VRAM allocation
    for (unsigned int entry = 0; entry < 0x400; entry++)
        bg1Map[entry] = 1;
    memset(VRAM_ + 64, 1, 64); // BG1 tile 1 is opaque green
    VRAM_[0x10000] = 2;
    VRAM_[BG_VRAM_SIZE - 1] = 3;
    REG16(REG_OFFSET_BG0CNT) = BGCNT_PRIORITY(0) | BGCNT_CHARBASE(3)
        | BGCNT_SCREENBASE(12) | BGCNT_256COLOR;
    REG16(REG_OFFSET_BG1CNT) = BGCNT_PRIORITY(1) | BGCNT_CHARBASE(0)
        | BGCNT_SCREENBASE(13) | BGCNT_256COLOR;
    REG16(REG_OFFSET_DISPCNT) = DISPCNT_BG0_ON | DISPCNT_BG1_ON;
    DrawFrameFast(sFastFrame);
    uint16_t bgAreaEdge = sFastFrame[7 * DISPLAY_WIDTH + 15];
    uint16_t clippedObjArea = sFastFrame[0];
    uint16_t clippedPastVram = sFastFrame[16];
    if (bgAreaEdge != 0xFC1F || clippedObjArea != 0x83E0 || clippedPastVram != 0x83E0)
    {
        fprintf(stderr,
                "FAST BG VRAM boundary fixture failed: edge expected=fc1f got=%04x; OBJ ref expected=83e0 got=%04x; past-VRAM ref expected=83e0 got=%04x\n",
                bgAreaEdge, clippedObjArea, clippedPastVram);
        return false;
    }

    ClearFastFixtureState();
    palette = (uint16_t *)PLTT;
    bg0Map = (uint16_t *)(VRAM_ + 12 * BG_SCREEN_SIZE);
    bg1Map = (uint16_t *)(VRAM_ + 13 * BG_SCREEN_SIZE);
    palette[1] = 0x001F;  // red first target
    palette[17] = 0x7C00; // blue second target in palette bank 1
    for (unsigned int entry = 0; entry < 0x400; entry++)
    {
        bg0Map[entry] = 1;
        bg1Map[entry] = 2 | (1 << 12);
    }
    VRAM_[1 * 32] = 0x01;
    VRAM_[2 * 32] = 0x01;
    REG16(REG_OFFSET_BG0CNT) = BGCNT_PRIORITY(0) | BGCNT_CHARBASE(0)
        | BGCNT_SCREENBASE(12);
    REG16(REG_OFFSET_BG1CNT) = BGCNT_PRIORITY(1) | BGCNT_CHARBASE(0)
        | BGCNT_SCREENBASE(13);
    REG16(REG_OFFSET_WIN1H) = (112 << 8) | 128;
    REG16(REG_OFFSET_WIN1V) = (80 << 8) | 96;
    REG16(REG_OFFSET_WININ) = 0x1F1F;  // WIN1 disables color effects
    REG16(REG_OFFSET_WINOUT) = 0x3F3F; // outside enables color effects
    REG16(REG_OFFSET_BLDCNT) = BLDCNT_TGT1_BG0 | BLDCNT_TGT2_BG1 | (1 << 6);
    REG16(REG_OFFSET_BLDALPHA) = 8 | (8 << 8);
    REG16(REG_OFFSET_DISPCNT) = DISPCNT_BG0_ON | DISPCNT_BG1_ON | DISPCNT_WIN1_ON;
    DrawFrameFast(sFastFrame);
    uint16_t inside = sFastFrame[88 * DISPLAY_WIDTH + 120];
    uint16_t outside = sFastFrame[88 * DISPLAY_WIDTH + 80];
    if (inside != 0x801F || outside != 0xBC0F)
    {
        fprintf(stderr,
                "FAST windowed blend fixture failed: inside expected=801f got=%04x; outside expected=bc0f got=%04x\n",
                inside, outside);
        return false;
    }

    // The voxel UI overlay uses bit 15 as coverage: an opaque black BG0
    // pixel must survive, while the (possibly brightened) backdrop must not.
    for (unsigned int fade = 0; fade < 2; fade++)
    {
        ClearFastFixtureState();
        palette = (uint16_t *)PLTT;
        bg0Map = (uint16_t *)(VRAM_ + 12 * BG_SCREEN_SIZE);
        palette[0] = 0x1234;
        palette[1] = 0; // opaque black, not transparent
        bg0Map[0] = 1;
        VRAM_[32] = 0x01; // first pixel black, second transparent
        REG16(REG_OFFSET_BG0CNT) = BGCNT_SCREENBASE(12);
        REG16(REG_OFFSET_DISPCNT) = DISPCNT_BG0_ON;
        if (fade)
        {
            REG16(REG_OFFSET_BLDCNT) = BLDCNT_TGT1_BD | (2 << 6);
            REG16(REG_OFFSET_BLDY) = 8;
        }
        DrawFrameEasy(sEasyFrame);
        DrawFrameFast(sFastFrame);
        if (sEasyFrame[0] != 0x8000 || sFastFrame[0] != 0x8000
         || (sEasyFrame[1] & 0x8000) || (sFastFrame[1] & 0x8000))
        {
            fprintf(stderr, "UI coverage fixture failed fade=%u: easy=%04x,%04x fast=%04x,%04x\n",
                    fade, sEasyFrame[0], sEasyFrame[1], sFastFrame[0], sFastFrame[1]);
            return false;
        }
    }

    printf("Independent fixtures: BG transparency, VRAM bounds, WIN1 blend, and UI coverage passed\n");
    fflush(stdout);
    return true;
}

static double Now(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec + time.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    sSimpleCase = argc > 1 && strcmp(argv[1], "simple") == 0;
    sForce4bpp = argc > 1 && strcmp(argv[1], "4bpp") == 0;
    sForceMode0 = sForce4bpp || (argc > 1 && strcmp(argv[1], "mode0") == 0);
    if (!RunFastExpectedFixtures())
        return 1;
    const unsigned int scenarios = sSimpleCase ? 1 : 64;
    unsigned int failures = 0;
    uint64_t differentPixels = 0;
    unsigned int firstScenario = 0, firstX = 0, firstY = 0;
    uint16_t firstEasy = 0, firstFast = 0;

    for (unsigned int scenario = 0; scenario < scenarios; scenario++)
    {
        FillState(scenario);
        SaveInitialState();
        sRasterLine = 0;
        RestoreInitialState();
        DrawFrameEasy(sEasyFrame);
        sRasterLine = 0;
        RestoreInitialState();
        DrawFrameFast(sFastFrame);

        unsigned int scenarioDiffs = 0;
        for (unsigned int y = 0; y < DISPLAY_HEIGHT; y++)
        {
            for (unsigned int x = 0; x < DISPLAY_WIDTH; x++)
            {
                unsigned int index = y * DISPLAY_WIDTH + x;
                if (sEasyFrame[index] != sFastFrame[index])
                {
                    if (failures == 0 && scenarioDiffs == 0)
                    {
                        firstScenario = scenario;
                        firstX = x;
                        firstY = y;
                        firstEasy = sEasyFrame[index];
                        firstFast = sFastFrame[index];
                    }
                    scenarioDiffs++;
                }
            }
        }
        if (scenarioDiffs != 0)
        {
            failures++;
            differentPixels += scenarioDiffs;
            fprintf(stderr, "scenario %u: %u/%u pixels differ\n", scenario,
                    scenarioDiffs, DISPLAY_WIDTH * DISPLAY_HEIGHT);
        }
    }

    if (failures != 0)
    {
        fprintf(stderr, "first difference: scenario=%u x=%u y=%u easy=%04x fast=%04x; %u/%u scenes differ, %llu pixels total\n",
                firstScenario, firstX, firstY, firstEasy, firstFast, failures, scenarios,
                (unsigned long long)differentPixels);
        return 1;
    }

    static const unsigned int benchmarkScenarios[] = {0, 5, 8, 12, 6};
    const unsigned int benchmarkSceneCount = sSimpleCase
        ? 1 : sizeof(benchmarkScenarios) / sizeof(benchmarkScenarios[0]);
    const int iterations = 80;
    double easyElapsed = 0;
    double fastElapsed = 0;
    for (unsigned int scene = 0; scene < benchmarkSceneCount; scene++)
    {
        FillState(sSimpleCase ? 0 : benchmarkScenarios[scene]);
        SaveInitialState();
        double start = Now();
        for (int i = 0; i < iterations; i++)
        {
            RestoreInitialState();
            sRasterLine = 0;
            DrawFrameEasy(sEasyFrame);
        }
        easyElapsed += Now() - start;
        start = Now();
        for (int i = 0; i < iterations; i++)
        {
            RestoreInitialState();
            sRasterLine = 0;
            DrawFrameFast(sFastFrame);
        }
        fastElapsed += Now() - start;
    }
    unsigned int benchmarkFrames = iterations * benchmarkSceneCount;
    printf("renderer parity: %u %s scenes passed\n", scenarios,
           sSimpleCase ? "simple 4bpp BG"
             : sForce4bpp ? "seeded mode-0 4bpp window/blend/sprite/raster"
             : sForceMode0 ? "seeded mode-0 window/blend/sprite/affine/raster"
             : "seeded mode/window/blend/sprite/affine/raster");
    printf("host benchmark over %u frames across %u scenes: EASY %.1f us/frame, FAST %.1f us/frame (%.2fx speedup)\n",
           benchmarkFrames, benchmarkSceneCount,
           easyElapsed * 1e6 / benchmarkFrames, fastElapsed * 1e6 / benchmarkFrames,
           easyElapsed / fastElapsed);
    return 0;
}
