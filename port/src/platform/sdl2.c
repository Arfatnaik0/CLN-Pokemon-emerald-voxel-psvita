#ifdef PLATFORM_SDL2
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#ifdef PLATFORM_VITA
#include <psp2/appmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>
#include <vitaGL.h>
#include "platform/gba_rgba.h"
#ifndef VITA_DEFAULT_ARM_CLOCK_MHZ
#define VITA_DEFAULT_ARM_CLOCK_MHZ 222
#endif
#endif

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#endif

#ifdef __ANDROID__
#include <jni.h>
#include <SDL.h>
#else
#include <SDL2/SDL.h>
#endif
#ifdef NATIVE_LINUX
#include <SDL2/SDL_image.h>
#endif
#include "mods/mod_manager.h"

#include "global.h"
#include "platform.h"
#include "rtc.h"
#include "gba/defines.h"
#include "gba/m4a_internal.h"
#include "cgb_audio.h"
#include "gba/flash_internal.h"
#include "platform/dma.h"
#include "platform/framedraw.h"
#include "platform/frame_pacing.h"
#include "platform/vita_perf.h"
#ifdef PLATFORM_VITA
#include "vita_thread_affinity.h"
/* Default to per-iteration presentation; CMake exposes the A/B switch. */
#ifndef VITA_PRESENT_EACH_SERVICED_TICK
#define VITA_PRESENT_EACH_SERVICED_TICK 0
#endif
#endif
#ifdef PLATFORM_VITA
#include "gba_easy_draw_workers.h"
#ifdef VOXEL_SUPPORTED
#include "voxel/voxel_renderer.h"
#include "voxel/voxel_world.h"
#include "voxel/voxel_entities.h"
#endif
#endif

extern void (*const gIntrTable[])(void);

SDL_Thread *mainLoopThread;
SDL_Window *sdlWindow;
SDL_Renderer *sdlRenderer;
SDL_Texture *sdlTexture;
#ifdef PLATFORM_VITA
#define VITA_RENDER_W 960
#define VITA_RENDER_H 544
#define VITA_GAME_TEXTURE_RING 8
static GLuint sGameTextureRing[VITA_GAME_TEXTURE_RING];
static uint16_t *sGameTextureDataPtr[VITA_GAME_TEXTURE_RING];
static volatile uint8_t sGameTextureUploadIdx;
static bool sVitaGlInitialized;
static bool sLastRenderedVoxel;
static uint16_t sVitaRgb565Frame[DISPLAY_WIDTH * DISPLAY_HEIGHT] __attribute__((aligned(16)));
static SDL_sem *sVitaFrameReadySem;
static int sVitaClearFramesRemaining = 3;
#endif
#if defined(NATIVE_LINUX) || defined(_WIN32)
#define MAX_BORDER_BACKGROUNDS 15
SDL_Texture *sdlBackgroundTextures[MAX_BORDER_BACKGROUNDS];
SDL_Texture *sdlBorderTexture;
#endif
static u8 sBorderBackgroundCount = 1;
SDL_AudioDeviceID sdlAudioDevice;
#ifdef PLATFORM_VITA
static SDL_AudioStream *sdlVitaAudioStream;
static SDL_atomic_t sVitaAudioQueueClosing;
static SDL_atomic_t sVitaAudioQueueActive;
static bool sVitaAudioQueueOverflowLogged;
#define VITA_GAME_AUDIO_RATE 42060
#define VITA_DEVICE_AUDIO_RATE 44100
#define VITA_AUDIO_QUEUE_LIMIT_FRAMES 4
#endif
SDL_sem *vBlankSemaphore;
SDL_atomic_t isFrameAvailable;
bool speedUp = false;
unsigned int videoScale = 1;
bool isRunning = true;
bool paused = false;
double simTime = 0;
double lastGameTime = 0;
double curGameTime = 0;
double fixedTimestep = 1.0 / 60.0; // 16.666667ms
double timeScale = 1.0;
struct SiiRtcInfo internalClock;

#ifdef PLATFORM_VITA
static void StopVitaAudioQueue(void)
{
    /* AgbMain has no quit path, so close the queue gate instead of joining it. */
    SDL_AtomicSet(&sVitaAudioQueueClosing, 1);
    while (SDL_AtomicGet(&sVitaAudioQueueActive) != 0)
        SDL_Delay(1);

    if (sdlVitaAudioStream != NULL)
    {
        SDL_FreeAudioStream(sdlVitaAudioStream);
        sdlVitaAudioStream = NULL;
    }
    if (sdlAudioDevice != 0)
    {
        SDL_PauseAudioDevice(sdlAudioDevice, 1);
        SDL_ClearQueuedAudio(sdlAudioDevice);
        sdlAudioDevice = 0;
    }
}
#endif

static bool sSaveFileReady;
static char sSavePath[1024] = "pokeemerald.sav";
static char sConfigPath[1024] = "pokeemerald.cfg";
static u8 sBorderBackground;
static bool sHasBorderBackgroundConfig;
static u8 sBackgroundOrderVersion;
static u8 sPlatformSettings[PLATFORM_SETTING_COUNT] = {0, 4, 0, 1, 1, 10, 0, 0, 1};
#if defined(__ANDROID__) || defined(PLATFORM_VITA)
static SDL_GameController *platformController;
static u16 controllerKeys;
static u16 controllerAxisKeys;
static Sint16 controllerAxisX;
static Sint16 controllerAxisY;
#endif
#ifdef PLATFORM_VITA
static bool sSaveUsesBackup;
static bool sSavePathNeedsReplace;
/* Default to filling the Vita display; older configs omit this setting. */
static bool sVitaStretchDisplay = true;
static bool sVitaDisplayDirty;
static bool sVitaDisplayRenderPending;
static uint64_t sVitaDisplayRenderStart;
static uint64_t sVitaDisplayRenderEnd;
#endif

extern void AgbMain(void);
extern void DoSoftReset(void);

int DoMain(void *param);
void ProcessEvents(void);
void VDraw(SDL_Texture *texture);

static void ReadSaveFile(const char *path);
static void ReadConfigFile(void);
static void StoreConfigFile(void);
static void ApplyPlatformSettings(void);
static bool8 StoreSaveFile(void);

#ifdef PLATFORM_VITA
static bool PrepareVitaDataDirectory(void);
static void GetVitaBackupPath(char *path, size_t pathSize);
#endif

static void UpdateInternalClock(void);

#ifdef PLATFORM_VITA
int VitaPlatform_GetRenderWidth(void)
{
    return VITA_RENDER_W;
}

int VitaPlatform_GetRenderHeight(void)
{
    return VITA_RENDER_H;
}

bool8 Platform_IsVoxelOverworldActive(void)
{
#ifdef VOXEL_SUPPORTED
    return (sPlatformSettings[PLATFORM_SETTING_VOXEL_3D] != 0)
        && gVoxelModeEnabled
        && VoxelWorld_IsMapAvailable();
#else
    return FALSE;
#endif
}

static void RenderVitaDisplayFrame(void)
{
    uint64_t renderQueueStart = VitaPerf_Now();
    sVitaDisplayRenderStart = renderQueueStart;
    sVitaDisplayRenderPending = true;

    uint8_t renderTexIdx = sGameTextureUploadIdx;
    __sync_synchronize();
    GLuint gameTex = sGameTextureRing[renderTexIdx];
    if (sVitaGlInitialized && gameTex != 0)
    {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (sVitaClearFramesRemaining > 0)
            sVitaClearFramesRemaining--;

        glUseProgram(0);
        glViewport(0, 0, VITA_RENDER_W, VITA_RENDER_H);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, (double)VITA_RENDER_W, (double)VITA_RENDER_H, 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_FOG);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glDisable(GL_ALPHA_TEST);
        glActiveTexture(GL_TEXTURE0);
        glEnable(GL_TEXTURE_2D);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBindTexture(GL_TEXTURE_2D, gameTex);

        float vx0 = 0.0f;
        float vx1 = (float)VITA_RENDER_W;
        if (!sVitaStretchDisplay)
        {
            const float gameW = ((float)VITA_RENDER_H * (float)DISPLAY_WIDTH) / (float)DISPLAY_HEIGHT;
            vx0 = ((float)VITA_RENDER_W - gameW) * 0.5f;
            vx1 = vx0 + gameW;
        }
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glBegin(GL_TRIANGLE_FAN);
        glTexCoord2f(0.0f, 0.0f);
        glVertex2f(vx0, 0.0f);
        glTexCoord2f(1.0f, 0.0f);
        glVertex2f(vx1, 0.0f);
        glTexCoord2f(1.0f, 1.0f);
        glVertex2f(vx1, (float)VITA_RENDER_H);
        glTexCoord2f(0.0f, 1.0f);
        glVertex2f(vx0, (float)VITA_RENDER_H);
        glEnd();
    }

    sVitaDisplayRenderEnd = VitaPerf_Now();
    VitaPerf_Record(VITA_PERF_RENDER_QUEUE, renderQueueStart,
                    sVitaDisplayRenderEnd);
}

static void RequestVitaDefaultArmClock(const char *phase)
{
    int beforeClockMhz = scePowerGetArmClockFrequency();
    int result = scePowerSetArmClockFrequency(VITA_DEFAULT_ARM_CLOCK_MHZ);
    int actualClockMhz = scePowerGetArmClockFrequency();

    VitaPerf_LogClockRequest(phase, VITA_DEFAULT_ARM_CLOCK_MHZ, beforeClockMhz,
                             result, actualClockMhz);

    if (result < 0)
    {
        sceClibPrintf("[VITA] ARM clock (%s) request %d MHz failed: 0x%08X; before=%d after=%d MHz\n",
                      phase != NULL ? phase : "unknown", VITA_DEFAULT_ARM_CLOCK_MHZ,
                      (unsigned int)result, beforeClockMhz, actualClockMhz);
    }
    else if (actualClockMhz != VITA_DEFAULT_ARM_CLOCK_MHZ)
    {
        sceClibPrintf("[VITA] ARM clock (%s) request %d MHz returned %d; before=%d after=%d MHz\n",
                      phase != NULL ? phase : "unknown", VITA_DEFAULT_ARM_CLOCK_MHZ,
                      result, beforeClockMhz, actualClockMhz);
    }
    else
    {
        sceClibPrintf("[VITA] ARM clock (%s) set to %d MHz (was %d MHz)\n",
                      phase != NULL ? phase : "unknown", actualClockMhz, beforeClockMhz);
    }
}

static void PresentVitaDisplayFrame(void)
{
    static bool sVitaAfterFirstPresentClockApplied = false;
    uint64_t presentStart = VitaPerf_Now();
    if (sVitaGlInitialized)
    {
        if (sLastRenderedVoxel)
        {
#ifdef VOXEL_SUPPORTED
            VoxelRenderer_SwapBuffers();
#else
            vglSwapBuffers(GL_FALSE);
#endif
        }
        else
        {
            vglSwapBuffers(GL_FALSE);
        }
    }
    uint64_t presentEnd = VitaPerf_Now();

    VitaPerf_Record(VITA_PERF_PRESENT, presentStart, presentEnd);
    if (sVitaDisplayRenderPending)
    {
        VitaPerf_RecordFrameRenderPresent(sVitaDisplayRenderStart,
                                          sVitaDisplayRenderEnd,
                                          presentStart, presentEnd);
        sVitaDisplayRenderPending = false;
    }
    if (!sVitaAfterFirstPresentClockApplied)
    {
        sVitaAfterFirstPresentClockApplied = true;
        RequestVitaDefaultArmClock("after_first_present");
    }
}
#endif

#ifdef __ANDROID__
static void HandleTouchEvent(const SDL_TouchFingerEvent *event);
static void DrawTouchControls(void);
#endif

int main(int argc, char **argv)
{
#ifdef PLATFORM_VITA
    if (!VitaThread_PinCurrentToCore(0))
        sceClibPrintf("[VITA] renderer affinity to user core 0 failed; using scheduler defaults\n");
    RequestVitaDefaultArmClock("pre_init");
#endif
    // Open an output console on Windows
#ifdef _WIN32
    AllocConsole() ;
    AttachConsole( GetCurrentProcessId() ) ;
    freopen( "CON", "w", stdout ) ;
#endif

#ifdef NATIVE_LINUX
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mods") == 0)
            gModsEnabled = true;
        if (strcmp(argv[i], "--mod") == 0 && i + 1 < argc) {
            gModsEnabled = true;
            gActiveModSelector = argv[i+1];
            i++;
        }
    }
#endif

#ifdef __ANDROID__
    SDL_setenv("SDL_AUDIODRIVER", "openslES", 1);
#endif
#if defined(__ANDROID__) || defined(PLATFORM_VITA)
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
#endif
    if (!ModManager_Init())
    {
        fprintf(stderr, "[Mods][FATAL] Could not initialize game data tables; exiting before SDL startup.\n");
        return 1;
    }
    if (SDL_Init(
#ifndef PLATFORM_VITA
                 SDL_INIT_VIDEO |
#endif
                 SDL_INIT_AUDIO
#if defined(__ANDROID__) || defined(PLATFORM_VITA)
                 | SDL_INIT_GAMECONTROLLER
#endif
                 ) < 0)
    {
        DBGPRINTF("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

#if defined(__ANDROID__) || defined(PLATFORM_VITA)
    for (int i = 0; i < SDL_NumJoysticks() && platformController == NULL; i++)
    {
        if (SDL_IsGameController(i))
            platformController = SDL_GameControllerOpen(i);
    }
#endif
#ifdef PLATFORM_VITA
    if (platformController == NULL)
        SDL_Log("No mapped Vita game controller detected; physical controls are unavailable");
#endif

#ifdef __ANDROID__
    char *prefPath = SDL_GetPrefPath("pokeemerald", "pokeemerald");
    if (prefPath != NULL)
    {
        SDL_snprintf(sSavePath, sizeof(sSavePath), "%spokeemerald.sav", prefPath);
        SDL_snprintf(sConfigPath, sizeof(sConfigPath), "%spokeemerald.cfg", prefPath);
        SDL_free(prefPath);
    }
#endif
#ifdef PLATFORM_VITA
    if (!PrepareVitaDataDirectory())
    {
        SDL_Quit();
        return 1;
    }
    SDL_snprintf(sSavePath, sizeof(sSavePath), "ux0:data/pokeemerald/pokeemerald.sav");
    SDL_snprintf(sConfigPath, sizeof(sConfigPath), "ux0:data/pokeemerald/pokeemerald.cfg");
    VitaPerf_Init();
#endif
    ReadSaveFile(sSavePath);
    ReadConfigFile();

#ifdef PLATFORM_VITA
    vglSetupGarbageCollector(127, SCE_KERNEL_CPU_MASK_USER_2);
    vglSetupRuntimeShaderCompiler(SHARK_OPT_FAST, GL_TRUE, GL_FALSE, GL_TRUE);
    vglUseLowPrecision(GL_TRUE);
    vglUseVramForUSSE(GL_TRUE);
    vglUseCachedMem(GL_TRUE);
    vglUseTripleBuffering(GL_TRUE);

    SceKernelFreeMemorySizeInfo freeMem;
    freeMem.size = sizeof(freeMem);
    sceKernelGetFreeMemorySize(&freeMem);
    int32_t ramPool = 56 * 1024 * 1024;
    if (freeMem.size_user <= 8 * 1024 * 1024)
        ramPool = 0;
    else if ((uint32_t)ramPool > freeMem.size_user - 8 * 1024 * 1024)
        ramPool = (int32_t)((freeMem.size_user - 8 * 1024 * 1024) & ~0xFFFu);

    int32_t cdramPool = 88 * 1024 * 1024;
    if (freeMem.size_cdram <= 1024 * 1024)
        cdramPool = 0;
    else if ((uint32_t)cdramPool > freeMem.size_cdram)
        cdramPool = (int32_t)(freeMem.size_cdram & ~0x3FFFFu);

    int32_t phycontPool = 8 * 1024 * 1024;
    if ((uint32_t)phycontPool > freeMem.size_phycont)
        phycontPool = (int32_t)(freeMem.size_phycont & ~0xFFFFFu);

    extern int msc_vgl_printf(const char *format, ...);
    msc_vgl_printf("init free user=%u cdram=%u phycont=%u pools=(%d,%d,%d)\n",
                   (unsigned)freeMem.size_user, (unsigned)freeMem.size_cdram,
                   (unsigned)freeMem.size_phycont, ramPool, cdramPool, phycontPool);

    GLboolean resFallback = vglInitWithCustomSizes(8 * 1024 * 1024, VITA_RENDER_W, VITA_RENDER_H,
                                                   ramPool, cdramPool, phycontPool,
                                                   0, SCE_GXM_MULTISAMPLE_NONE);
    msc_vgl_printf("vglInitWithCustomSizes done res_fallback=%u\n", (unsigned)resFallback);
    sVitaGlInitialized = true;
    eglSwapInterval(NULL, 1);

    glGenTextures(VITA_GAME_TEXTURE_RING, sGameTextureRing);
    for (int i = 0; i < VITA_GAME_TEXTURE_RING; i++)
    {
        glBindTexture(GL_TEXTURE_2D, sGameTextureRing[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
        sGameTextureDataPtr[i] = (uint16_t *)vglGetTexDataPointer(GL_TEXTURE_2D);
    }
    sGameTextureUploadIdx = 0;
#ifdef VOXEL_SUPPORTED
    VoxelRenderer_Init();
#endif
    VitaPerf_LogRenderer("vitaGL", VITA_RENDER_W, VITA_RENDER_H, VITA_RENDER_W, VITA_RENDER_H, 0);
    RequestVitaDefaultArmClock("post_renderer");
#else
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
#if defined(NATIVE_LINUX) || defined(_WIN32)
    Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    sdlWindow = SDL_CreateWindow("Pokemon Emerald Voxel", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, windowFlags);
#else
    sdlWindow = SDL_CreateWindow("pokeemerald", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
#endif
    if (sdlWindow == NULL)
    {
        DBGPRINTF("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

#if defined(__ANDROID__)
    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_ACCELERATED);
#else
    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_PRESENTVSYNC);
#endif
    if (sdlRenderer == NULL)
    {
        DBGPRINTF("Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(sdlRenderer);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");


    for (int i = 1; i < 15; i++)
    {
        char filename[16];
#ifdef _WIN32
        snprintf(filename, sizeof(filename), "BG%d.bmp", i);
#else
        snprintf(filename, sizeof(filename), "BG%d.png", i);
#endif
        SDL_RWops *backgroundFile = SDL_RWFromFile(filename, "rb");
        if (backgroundFile == NULL)
            break;
        SDL_RWclose(backgroundFile);
        sBorderBackgroundCount++;
    }
    if (sBackgroundOrderVersion < 2)
    {
        if (sHasBorderBackgroundConfig)
        {
            if (sBorderBackground == 1)
                sBorderBackground = sBorderBackgroundCount;
            else if (sBorderBackground >= 2)
                sBorderBackground--;
        }
        sBackgroundOrderVersion = 2;
        StoreConfigFile();
    }
#ifdef NATIVE_LINUX
    if (sdlRenderer != NULL)
    {
        SDL_RenderSetLogicalSize(sdlRenderer, 0, 0);
        if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0)
        {
            SDL_Log("SDL_image could not initialize: %s", IMG_GetError());
        }
        else
        {
            for (int i = 0; i < sBorderBackgroundCount; i++)
            {
                char filename[16];
                snprintf(filename, sizeof(filename), i == 0 ? "BG.png" : "BG%d.png", i);
                sdlBackgroundTextures[i] = IMG_LoadTexture(sdlRenderer, filename);
            }
            sdlBorderTexture = IMG_LoadTexture(sdlRenderer, "Border.png");
            if (sdlBackgroundTextures[0] == NULL)
                SDL_Log("Background image could not be loaded: %s", IMG_GetError());
            if (sdlBorderTexture == NULL)
                SDL_Log("Border image could not be loaded: %s", IMG_GetError());
        }
    }
#elif defined(_WIN32)
    if (sdlRenderer != NULL)
    {
        SDL_RenderSetLogicalSize(sdlRenderer, 0, 0);
        SDL_Surface *borderSurface = SDL_LoadBMP("Border.bmp");
        for (int i = 0; i < sBorderBackgroundCount; i++)
        {
            char filename[16];
            snprintf(filename, sizeof(filename), i == 0 ? "BG.bmp" : "BG%d.bmp", i);
            SDL_Surface *backgroundSurface = SDL_LoadBMP(filename);
            if (backgroundSurface == NULL)
                continue;
            sdlBackgroundTextures[i] = SDL_CreateTextureFromSurface(sdlRenderer, backgroundSurface);
            SDL_FreeSurface(backgroundSurface);
        }
        if (sdlBackgroundTextures[0] == NULL)
            SDL_Log("Background image could not be loaded: %s", SDL_GetError());
        if (borderSurface == NULL)
        {
            SDL_Log("Border image could not be loaded: %s", SDL_GetError());
        }
        else
        {
            sdlBorderTexture = SDL_CreateTextureFromSurface(sdlRenderer, borderSurface);
            SDL_FreeSurface(borderSurface);
        }
    }
#else
    if (sdlRenderer != NULL)
    {
        SDL_RenderSetLogicalSize(sdlRenderer, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        SDL_RenderSetIntegerScale(sdlRenderer, SDL_TRUE);
    }
#endif
    ApplyPlatformSettings();

    if (sdlRenderer != NULL) {
        SDL_TextureAccess frameTextureAccess = SDL_TEXTUREACCESS_STREAMING;
        sdlTexture = SDL_CreateTexture(sdlRenderer,
                                       SDL_PIXELFORMAT_ARGB8888,
                                       frameTextureAccess,
                                       DISPLAY_WIDTH, DISPLAY_HEIGHT);
        if (sdlTexture == NULL)
        {
            DBGPRINTF("Texture could not be created! SDL_Error: %s\n", SDL_GetError());
            return 1;
        }
        SDL_SetTextureBlendMode(sdlTexture, SDL_BLENDMODE_NONE);
    }
#endif

    simTime = curGameTime = lastGameTime = SDL_GetPerformanceCounter();

    isFrameAvailable.value = 0;
    vBlankSemaphore = SDL_CreateSemaphore(0);
#ifdef PLATFORM_VITA
    sVitaFrameReadySem = SDL_CreateSemaphore(0);
#endif

    SDL_AudioSpec want;

    SDL_memset(&want, 0, sizeof(want)); /* or SDL_zero(want) */
#ifdef PLATFORM_VITA
    /* Vita's audio port accepts standard rates only; keep the game mixer at
     * its native 42060 Hz and resample to the supported 44100 Hz port rate. */
    want.freq = VITA_DEVICE_AUDIO_RATE;
#else
    want.freq = 42060;
#endif
    want.format = AUDIO_F32;
    want.channels = 2;
    want.samples = 1024;
#ifdef PLATFORM_VITA
    cgb_audio_init(VITA_GAME_AUDIO_RATE);
#else
    cgb_audio_init(want.freq);
#endif


    sdlAudioDevice = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (sdlAudioDevice == 0)
        SDL_Log("Failed to open audio: %s", SDL_GetError());
    else
    {
#ifdef PLATFORM_VITA
        sdlVitaAudioStream = SDL_NewAudioStream(AUDIO_F32, 2,
                                                VITA_GAME_AUDIO_RATE,
                                                AUDIO_F32, 2,
                                                VITA_DEVICE_AUDIO_RATE);
        if (sdlVitaAudioStream == NULL)
        {
            SDL_Log("Failed to create Vita audio resampler (42060 -> 44100 Hz): %s", SDL_GetError());
            SDL_CloseAudioDevice(sdlAudioDevice);
            sdlAudioDevice = 0;
        }
#endif
    }
    if (sdlAudioDevice != 0)
    {
        if (want.format != AUDIO_F32) /* we let this one thing change. */
            SDL_Log("We didn't get Float32 audio format.");
#ifdef PLATFORM_VITA
        SDL_Log("Opened Vita audio at %d Hz; game mixer runs at %d Hz",
                VITA_DEVICE_AUDIO_RATE, VITA_GAME_AUDIO_RATE);
        VitaPerf_ResetAudioQueueTiming();
#endif
        SDL_PauseAudioDevice(sdlAudioDevice, 0);
    }
#ifndef __ANDROID__
#ifdef PLATFORM_VITA
    /* Prime the emulated LCD into VBlank before AgbMain enables VBlank IRQs. */
    REG_VCOUNT = 161;
    VDraw(sdlTexture);
#else
    if (sdlTexture != NULL)
        VDraw(sdlTexture);
#endif
#endif
    mainLoopThread = SDL_CreateThread(DoMain, "AgbMain", NULL);

    FramePacingState framePacing = {0.0};
#ifdef PLATFORM_VITA
#if VITA_PRESENT_EACH_SERVICED_TICK
    const FramePacingPresentationMode presentationMode =
        FRAME_PACING_PRESENT_PER_SERVICED_FRAME;
#else
    const FramePacingPresentationMode presentationMode =
        FRAME_PACING_PRESENT_PER_ITERATION;
#endif
#endif

    memset(&internalClock, 0, sizeof(internalClock));
    internalClock.status = SIIRTCINFO_24HOUR;
    UpdateInternalClock();

    while (isRunning)
    {
#ifdef PLATFORM_VITA
        uint64_t mainLoopStart = VitaPerf_Now();
        FramePacingBatch frameBatch;
        FramePacing_BeginBatch(&frameBatch, presentationMode);
#endif
        ProcessEvents();

        if (!paused)
        {
            double dt = fixedTimestep / timeScale; // TODO: Fix speedup

            curGameTime = SDL_GetPerformanceCounter();
            double deltaTime = (double)((curGameTime - lastGameTime) / (double)SDL_GetPerformanceFrequency());
            lastGameTime = curGameTime;

            FramePacing_Accumulate(&framePacing, deltaTime, dt);

            while (FramePacing_HasStep(&framePacing, dt))
            {
                if (!SDL_AtomicGet(&isFrameAvailable))
                    break;
                {
#ifdef PLATFORM_VITA
                    {
#ifdef VOXEL_SUPPORTED
                        bool renderVoxel = Platform_IsVoxelOverworldActive() && VoxelRenderer_RenderFrame();
#else
                        bool renderVoxel = false;
#endif
                        if (renderVoxel)
                        {
                            sLastRenderedVoxel = true;
                            sVitaDisplayDirty = false;
                        }
                        else
                        {
                            sLastRenderedVoxel = false;
                            VDraw(NULL);
                            RenderVitaDisplayFrame();
                            sVitaDisplayDirty = false;
                        }
                    }
#else
                    {
                        VDraw(sdlTexture);
                        uint64_t renderQueueStart = VitaPerf_Now();
                        if (sdlRenderer) SDL_RenderClear(sdlRenderer);
#if defined(NATIVE_LINUX) || defined(_WIN32)
                        u8 backgroundOption = Platform_GetBorderBackground();
                        if (backgroundOption < sBorderBackgroundCount
                         && sdlBackgroundTextures[backgroundOption] != NULL)
                            SDL_RenderCopy(sdlRenderer, sdlBackgroundTextures[backgroundOption], NULL, NULL);
                        int outputWidth;
                        int outputHeight;
                        SDL_GetRendererOutputSize(sdlRenderer, &outputWidth, &outputHeight);
                        int gameHeight;
                        int gameWidth;
                        if (sPlatformSettings[PLATFORM_SETTING_INTEGER_SCALE])
                        {
                            int scale = outputWidth / DISPLAY_WIDTH;
                            if (outputHeight / DISPLAY_HEIGHT < scale)
                                scale = outputHeight / DISPLAY_HEIGHT;
                            if (scale < 1)
                                scale = 1;
                            gameWidth = DISPLAY_WIDTH * scale;
                            gameHeight = DISPLAY_HEIGHT * scale;
                        }
                        else
                        {
                            gameHeight = outputHeight * 8 / 9;
                            gameWidth = gameHeight * 3 / 2;
                        }
                        SDL_Rect gameViewport = {(outputWidth - gameWidth) / 2,
                                                 (outputHeight - gameHeight) / 2,
                                                 gameWidth, gameHeight};
                        SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &gameViewport);
                        if (sPlatformSettings[PLATFORM_SETTING_BORDER] && sdlBorderTexture != NULL)
                        {
                            SDL_Rect borderSource = {141, 18, 1000, 683};
                            int innerWidth = gameViewport.w - 2;
                            int innerHeight = gameViewport.h - 2;
                            SDL_Rect borderViewport = {
                                gameViewport.x + 1 - innerWidth * 19 / 961,
                                gameViewport.y + 1 - innerHeight * 20 / 643,
                                innerWidth * 1000 / 961,
                                innerHeight * 683 / 643
                            };
                            SDL_RenderCopy(sdlRenderer, sdlBorderTexture, &borderSource, &borderViewport);
                        }
#else
                        if (sdlRenderer) SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
#endif
                        VitaPerf_Record(VITA_PERF_RENDER_QUEUE, renderQueueStart,
                                        VitaPerf_Now());
                    }
#endif
#ifdef __ANDROID__
                    if (sdlRenderer) SDL_RenderPresent(sdlRenderer);
#endif
                    SDL_AtomicSet(&isFrameAvailable, 0);

                    REG_DISPSTAT |= INTR_FLAG_VBLANK;

                    RunDMAs(DMA_HBLANK);

#ifdef __ANDROID__
                    if (REG_IE & INTR_FLAG_VBLANK)
#else
                    if (REG_DISPSTAT & DISPSTAT_VBLANK_INTR)
#endif
                    {
#ifndef PLATFORM_VITA
                        static int sVBlankPrints = 0;
                        if (sVBlankPrints < 300) {
                            if (sVBlankPrints % 30 == 0) printf("[DoMain] Executing VBlankIntr, REG_DISPSTAT=%04x\n", REG_DISPSTAT);
                            sVBlankPrints++;
                        }
#endif
                        gIntrTable[4]();
                    }
                    else
                    {
#ifndef PLATFORM_VITA
                        static int sNoVBlankPrints = 0;
                        if (sNoVBlankPrints < 300) {
                            if (sNoVBlankPrints % 30 == 0) printf("[DoMain] SKIPPING VBlankIntr, REG_DISPSTAT=%04x, sRegIE=%04x\n", REG_DISPSTAT, REG_IE);
                            sNoVBlankPrints++;
                        }
#endif
                    }
                    REG_DISPSTAT &= ~INTR_FLAG_VBLANK;

                    SDL_SemPost(vBlankSemaphore);

                    FramePacing_ConsumeStep(&framePacing, dt);
#ifdef PLATFORM_VITA
                    FramePacing_RecordServicedFrame(&frameBatch);
                    /* Keep game-thread handoff order; one persistent backend
                     * owns presentation in either classic or voxel mode. */
                    if (FramePacing_ShouldPresentAfterServicedFrame(&frameBatch))
                    {
                        PresentVitaDisplayFrame();
                    }
#endif
                }
            }
        }

#ifndef __ANDROID__
#ifdef PLATFORM_VITA
        bool dirtyRedraw = FramePacing_ShouldRedrawDirtyDisplay(
            sVitaDisplayDirty, &frameBatch);
        if (dirtyRedraw)
        {
#ifdef VOXEL_SUPPORTED
            if (Platform_IsVoxelOverworldActive() && VoxelRenderer_RenderFrame())
                sLastRenderedVoxel = true;
            else
#endif
            {
                sLastRenderedVoxel = false;
                RenderVitaDisplayFrame();
            }
            sVitaDisplayDirty = false;
        }
        if ((FramePacing_ShouldPresentAtBatchEnd(&frameBatch) && frameBatch.servicedFrames > 0) || dirtyRedraw)
        {
            PresentVitaDisplayFrame();
        }
        VitaPerf_RecordMainLoopBatch(frameBatch.servicedFrames, mainLoopStart,
                                     VitaPerf_Now());
        VitaPerf_Poll();
        if (paused)
        {
            SDL_Delay(8);
        }
        else if (frameBatch.servicedFrames == 0 && !dirtyRedraw && sVitaFrameReadySem != NULL)
        {
            if (!SDL_AtomicGet(&isFrameAvailable))
                SDL_SemWaitTimeout(sVitaFrameReadySem, 1);
            while (SDL_SemTryWait(sVitaFrameReadySem) == 0)
            {
            }
        }
#else
        if (sdlRenderer)
        {
            uint64_t presentStart = VitaPerf_Now();
            SDL_RenderPresent(sdlRenderer);
            VitaPerf_Record(VITA_PERF_PRESENT, presentStart, VitaPerf_Now());
        }
        VitaPerf_Poll();
#endif
#else
        VitaPerf_Poll();
#endif
    }

#if defined(NATIVE_LINUX) || defined(_WIN32)
    for (int i = 0; i < sBorderBackgroundCount; i++)
        SDL_DestroyTexture(sdlBackgroundTextures[i]);
    SDL_DestroyTexture(sdlBorderTexture);
#endif

#ifdef NATIVE_LINUX
    IMG_Quit();
#endif
#if defined(__ANDROID__) || defined(PLATFORM_VITA)
    if (platformController != NULL)
    {
        SDL_GameControllerClose(platformController);
        platformController = NULL;
    }
#endif
#ifdef PLATFORM_VITA
    GbaEasyDrawWorkers_Shutdown();
    if (sVitaGlInitialized)
    {
#ifdef VOXEL_SUPPORTED
        VoxelRenderer_Shutdown();
#endif
        glDeleteTextures(VITA_GAME_TEXTURE_RING, sGameTextureRing);
        sVitaGlInitialized = false;
    }
    if (sVitaFrameReadySem != NULL)
    {
        SDL_DestroySemaphore(sVitaFrameReadySem);
        sVitaFrameReadySem = NULL;
    }
#endif
    if (sdlRenderer != NULL)
        SDL_DestroyRenderer(sdlRenderer);
    if (sdlWindow != NULL)
        SDL_DestroyWindow(sdlWindow);
    ModManager_Shutdown();
#ifdef PLATFORM_VITA
    StopVitaAudioQueue();
    VitaPerf_Shutdown();
#endif
    SDL_Quit();
    return 0;
}

#ifdef PLATFORM_VITA
static bool PrepareVitaDataDirectory(void)
{
    const char *dataDirectory = "ux0:data/pokeemerald";
    SceUID directory = sceIoDopen(dataDirectory);
    if (directory >= 0)
    {
        sceIoDclose(directory);
        return true;
    }

    int createResult = sceIoMkdir(dataDirectory, 0777);
    directory = sceIoDopen(dataDirectory);
    if (directory >= 0)
    {
        sceIoDclose(directory);
        return true;
    }

    SDL_Log("Unable to create or open Vita data directory %s (mkdir=%08x)",
            dataDirectory, (unsigned int)createResult);
    return false;
}

static void GetVitaBackupPath(char *path, size_t pathSize)
{
    SDL_snprintf(path, pathSize, "%s.bak", sSavePath);
}
#endif

static void ReadSaveFile(const char *path)
{
    // Check whether the saveFile exists, and create it if not
    FILE *saveFile = fopen(path, "r+b");
#ifdef PLATFORM_VITA
    long fileSize = -1;
    bool recoveredFromBackup = false;
    if (saveFile != NULL)
    {
        fseek(saveFile, 0, SEEK_END);
        fileSize = ftell(saveFile);
        fseek(saveFile, 0, SEEK_SET);
    }

    // Prefer the last complete save if a previous application-level write was interrupted.
    if (saveFile == NULL || (fileSize >= 0 && fileSize < sizeof(FLASH_BASE)))
    {
        char backupPath[sizeof(sSavePath) + 8];
        GetVitaBackupPath(backupPath, sizeof(backupPath));
        FILE *backupFile = fopen(backupPath, "r+b");
        if (backupFile != NULL)
        {
            fseek(backupFile, 0, SEEK_END);
            long backupSize = ftell(backupFile);
            fseek(backupFile, 0, SEEK_SET);
            if (backupSize >= sizeof(FLASH_BASE))
            {
                if (saveFile != NULL)
                    fclose(saveFile);
                saveFile = backupFile;
                recoveredFromBackup = true;
                sSaveUsesBackup = true;
                sSavePathNeedsReplace = fileSize >= 0;
                fileSize = backupSize;
                SDL_Log("Recovering save from %s", backupPath);
            }
            else
            {
                fclose(backupFile);
            }
        }
    }
#endif
    if (saveFile == NULL)
    {
        saveFile = fopen(path, "w+b");
    }

    if (saveFile == NULL)
    {
        memset(FLASH_BASE, 0xFF, sizeof(FLASH_BASE));
        SDL_Log("Unable to open save file: %s", path);
        sSaveFileReady = false;
        return;
    }

#ifndef PLATFORM_VITA
    fseek(saveFile, 0, SEEK_END);
    int fileSize = ftell(saveFile);
    fseek(saveFile, 0, SEEK_SET);
#else
    if (fileSize < 0)
    {
        fseek(saveFile, 0, SEEK_END);
        fileSize = ftell(saveFile);
        fseek(saveFile, 0, SEEK_SET);
        if (fileSize < 0)
            fileSize = 0;
    }
#endif

    // Only read as many bytes as fit inside the buffer
    // or as many bytes as are in the file
    int bytesToRead = (fileSize < sizeof(FLASH_BASE)) ? fileSize : sizeof(FLASH_BASE);

    int bytesRead = fread(FLASH_BASE, 1, bytesToRead, saveFile);
    sSaveFileReady = bytesRead == bytesToRead;
#ifdef PLATFORM_VITA
    sSaveFileReady = sSaveFileReady
                  && (recoveredFromBackup || fileSize == 0 || fileSize >= sizeof(FLASH_BASE));
    if (!sSaveFileReady)
        SDL_Log("Vita save is incomplete; refusing to replace it without a complete backup");
#endif
    fclose(saveFile);

    // Fill the buffer if the savefile was just created or smaller than the buffer itself
    for (int i = bytesRead; i < sizeof(FLASH_BASE); i++)
    {
        FLASH_BASE[i] = 0xFF;
    }
}

static void ReadConfigFile(void)
{
    FILE *configFile = fopen(sConfigPath, "r");
    char line[64];
    unsigned int value;

    if (configFile == NULL)
        return;
    while (fgets(line, sizeof(line), configFile) != NULL)
    {
        if (sscanf(line, "borderBackground=%u", &value) == 1 && value < 16)
        {
            sBorderBackground = value;
            sHasBorderBackgroundConfig = true;
        }
        else if (sscanf(line, "backgroundOrder=%u", &value) == 1)
            sBackgroundOrderVersion = value;
        else if (sscanf(line, "fullscreen=%u", &value) == 1)
            sPlatformSettings[PLATFORM_SETTING_FULLSCREEN] = value != 0;
#ifdef PLATFORM_VITA
        else if (sscanf(line, "vitaStretch=%u", &value) == 1)
            sVitaStretchDisplay = value != 0;
        else if (sscanf(line, "voxel3d=%u", &value) == 1)
            sPlatformSettings[PLATFORM_SETTING_VOXEL_3D] = value != 0;
        else if (sscanf(line, "voxelRes=%u", &value) == 1 && value <= 1)
            sPlatformSettings[PLATFORM_SETTING_VOXEL_RES] = value;
        else if (sscanf(line, "voxelFps=%u", &value) == 1 && value <= 1)
            sPlatformSettings[PLATFORM_SETTING_VOXEL_FPS] = value;
#endif
        else if (sscanf(line, "windowScale=%u", &value) == 1 && value >= 2 && value <= 5)
            sPlatformSettings[PLATFORM_SETTING_WINDOW_SCALE] = value;
        else if (sscanf(line, "integerScale=%u", &value) == 1)
            sPlatformSettings[PLATFORM_SETTING_INTEGER_SCALE] = value != 0;
        else if (sscanf(line, "vsync=%u", &value) == 1)
            sPlatformSettings[PLATFORM_SETTING_VSYNC] = value != 0;
        else if (sscanf(line, "border=%u", &value) == 1)
            sPlatformSettings[PLATFORM_SETTING_BORDER] = value != 0;
        else if (sscanf(line, "volume=%u", &value) == 1 && value <= 10)
            sPlatformSettings[PLATFORM_SETTING_VOLUME] = value;
    }
#if defined(PLATFORM_VITA) && defined(VOXEL_SUPPORTED)
    gVoxelModeEnabled = (sPlatformSettings[PLATFORM_SETTING_VOXEL_3D] != 0);
#endif
    fclose(configFile);
}

static void StoreConfigFile(void)
{
    FILE *configFile = fopen(sConfigPath, "w");

    if (configFile == NULL)
    {
#ifdef PLATFORM_VITA
        SDL_Log("Unable to write config file: %s", sConfigPath);
#endif
        return;
    }
    fprintf(configFile, "borderBackground=%u\n", sBorderBackground);
    fprintf(configFile, "backgroundOrder=2\n");
    fprintf(configFile, "fullscreen=%u\n", sPlatformSettings[PLATFORM_SETTING_FULLSCREEN]);
#ifdef PLATFORM_VITA
    fprintf(configFile, "vitaStretch=%u\n", sVitaStretchDisplay);
    fprintf(configFile, "voxel3d=%u\n", sPlatformSettings[PLATFORM_SETTING_VOXEL_3D]);
    fprintf(configFile, "voxelRes=%u\n", sPlatformSettings[PLATFORM_SETTING_VOXEL_RES]);
    fprintf(configFile, "voxelFps=%u\n", sPlatformSettings[PLATFORM_SETTING_VOXEL_FPS]);
#endif
    fprintf(configFile, "windowScale=%u\n", sPlatformSettings[PLATFORM_SETTING_WINDOW_SCALE]);
    fprintf(configFile, "integerScale=%u\n", sPlatformSettings[PLATFORM_SETTING_INTEGER_SCALE]);
    fprintf(configFile, "vsync=%u\n", sPlatformSettings[PLATFORM_SETTING_VSYNC]);
    fprintf(configFile, "border=%u\n", sPlatformSettings[PLATFORM_SETTING_BORDER]);
    fprintf(configFile, "volume=%u\n", sPlatformSettings[PLATFORM_SETTING_VOLUME]);
    if (fclose(configFile) != 0)
    {
#ifdef PLATFORM_VITA
        SDL_Log("Unable to flush config file: %s", sConfigPath);
#endif
    }
}

static void ApplyPlatformSettings(void)
{
    if (sdlRenderer == NULL)
        return;
    SDL_RenderSetVSync(sdlRenderer, sPlatformSettings[PLATFORM_SETTING_VSYNC]);
#if defined(NATIVE_LINUX) || defined(_WIN32)
    SDL_SetWindowFullscreen(sdlWindow, sPlatformSettings[PLATFORM_SETTING_FULLSCREEN]
                                      ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!sPlatformSettings[PLATFORM_SETTING_FULLSCREEN])
    {
        int scale = sPlatformSettings[PLATFORM_SETTING_WINDOW_SCALE];
        SDL_SetWindowSize(sdlWindow, 320 * scale, 180 * scale);
        SDL_SetWindowPosition(sdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
#endif
}

static bool8 StoreSaveFile(void)
{
    if (!sSaveFileReady)
    {
        SDL_Log("Cannot store save: no complete source save was loaded");
        return FALSE;
    }

#ifdef PLATFORM_VITA
    /*
     * fflush/fclose drain stdio buffers, but the Vita SDK marks sceIoSyncByFd's
     * flag undocumented. This temp/backup sequence does not guarantee survival
     * through sudden power loss or device-level write caching.
     */
    char tempPath[sizeof(sSavePath) + 8];
    char backupPath[sizeof(sSavePath) + 8];
    SDL_snprintf(tempPath, sizeof(tempPath), "%s.tmp", sSavePath);
    GetVitaBackupPath(backupPath, sizeof(backupPath));

    FILE *tempFile = fopen(tempPath, "wb");
    if (tempFile == NULL)
    {
        SDL_Log("Unable to open temporary save %s", tempPath);
        return FALSE;
    }
    bool writeOk = fwrite(FLASH_BASE, 1, sizeof(FLASH_BASE), tempFile) == sizeof(FLASH_BASE);
    if (fflush(tempFile) != 0)
        writeOk = false;
    if (fclose(tempFile) != 0)
        writeOk = false;
    if (!writeOk)
    {
        remove(tempPath);
        SDL_Log("Unable to flush complete temporary save %s", tempPath);
        return FALSE;
    }

    bool hasCurrentSave = false;
    FILE *currentFile = fopen(sSavePath, "rb");
    if (currentFile != NULL)
    {
        hasCurrentSave = true;
        fclose(currentFile);
    }

    bool rotatedCurrentSave = false;
    if (sSaveUsesBackup)
    {
        if (sSavePathNeedsReplace && hasCurrentSave && remove(sSavePath) != 0)
        {
            SDL_Log("Unable to replace incomplete Vita save %s", sSavePath);
            remove(tempPath);
            return FALSE;
        }
    }
    else if (hasCurrentSave)
    {
        FILE *oldBackup = fopen(backupPath, "rb");
        if (oldBackup != NULL)
        {
            fclose(oldBackup);
            if (remove(backupPath) != 0)
            {
                SDL_Log("Unable to rotate Vita save backup %s", backupPath);
                remove(tempPath);
                return FALSE;
            }
        }
        if (rename(sSavePath, backupPath) != 0)
        {
            SDL_Log("Unable to preserve current Vita save %s", sSavePath);
            remove(tempPath);
            return FALSE;
        }
        rotatedCurrentSave = true;
    }

    if (rename(tempPath, sSavePath) != 0)
    {
        SDL_Log("Unable to install new Vita save %s", sSavePath);
        if (rotatedCurrentSave && rename(backupPath, sSavePath) != 0)
        {
            sSaveUsesBackup = true;
            sSavePathNeedsReplace = false;
            SDL_Log("Save rollback failed; previous save remains at %s", backupPath);
        }
        remove(tempPath);
        return FALSE;
    }
    sSaveUsesBackup = false;
    sSavePathNeedsReplace = false;
    return TRUE;
#else
    FILE *saveFile = fopen(sSavePath, "r+b");
    if (saveFile == NULL)
    {
        SDL_Log("Unable to open save file for writing: %s", sSavePath);
        return FALSE;
    }
    bool writeOk = fseek(saveFile, 0, SEEK_SET) == 0
                && fwrite(FLASH_BASE, 1, sizeof(FLASH_BASE), saveFile) == sizeof(FLASH_BASE)
                && fflush(saveFile) == 0;
    if (fclose(saveFile) != 0)
        writeOk = false;
    if (!writeOk)
        SDL_Log("Unable to flush complete save file: %s", sSavePath);
    return writeOk ? TRUE : FALSE;
#endif
}

bool8 Platform_StoreSaveFile(void)
{
    return StoreSaveFile();
}

void Platform_ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size)
{
#ifndef PLATFORM_VITA
    DBGPRINTF("ReadFlash(sectorNum=0x%04X,offset=0x%08X,size=0x%02X)\n",sectorNum,offset,size);
#endif
#ifdef PLATFORM_VITA
    char backupPath[sizeof(sSavePath) + 8];
    GetVitaBackupPath(backupPath, sizeof(backupPath));
    const char *readPath = sSaveUsesBackup ? backupPath : sSavePath;
#else
    const char *readPath = sSavePath;
#endif
    FILE * savefile = fopen(readPath, "r+b");
    if (savefile == NULL)
    {
        SDL_Log("Error opening save file: %s", readPath);
        return;
    }
    if (fseek(savefile, (sectorNum << gFlash->sector.shift) + offset, SEEK_SET))
    {
        fclose(savefile);
        return;
    }
    if (fread(dest, 1, size, savefile) != size)
    {
        fclose(savefile);
        return;
    }
    fclose(savefile);
}

#ifdef PLATFORM_VITA
static void QueueVitaAudio(float *audioBuffer, s32 samplesPerFrame)
{
    if (sdlVitaAudioStream == NULL)
        return;

    /* Fast-forward pauses the device while the game keeps producing frames. */
    if (SDL_GetAudioDeviceStatus(sdlAudioDevice) != SDL_AUDIO_PLAYING)
    {
        SDL_AudioStreamClear(sdlVitaAudioStream);
        VitaPerf_ResetAudioQueueTiming();
        return;
    }

    Uint32 queuedBeforeBytes = SDL_GetQueuedAudioSize(sdlAudioDevice);
    Uint32 queueLimitBytes = (VITA_DEVICE_AUDIO_RATE / 60)
                           * 2 * sizeof(float) * VITA_AUDIO_QUEUE_LIMIT_FRAMES;
#if VITA_PERF_ENABLED
    uint32_t overflowCleared = 0;
#endif
    if (queuedBeforeBytes > queueLimitBytes)
    {
        if (!sVitaAudioQueueOverflowLogged)
        {
            SDL_Log("Vita audio queue reached %u bytes; dropping stale audio", queuedBeforeBytes);
            sVitaAudioQueueOverflowLogged = true;
        }
        SDL_ClearQueuedAudio(sdlAudioDevice);
        SDL_AudioStreamClear(sdlVitaAudioStream);
#if VITA_PERF_ENABLED
        overflowCleared = 1;
#endif
    }

    if (samplesPerFrame % (s32)sizeof(float) != 0)
    {
        SDL_Log("Ignoring misaligned audio buffer (%d bytes)", samplesPerFrame);
        return;
    }

    int floatCount = samplesPerFrame / sizeof(float);
    static float sAdjustedAudio[4096];
    const void *streamData = audioBuffer;
    int settingVolume = sPlatformSettings[PLATFORM_SETTING_VOLUME];
    if (settingVolume != 10)
    {
        float volume = settingVolume / 10.0f;
        if (floatCount <= (int)ARRAY_COUNT(sAdjustedAudio))
        {
            int i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
            float32x4_t v = vdupq_n_f32(volume);
            for (; i + 4 <= floatCount; i += 4)
                vst1q_f32(&sAdjustedAudio[i], vmulq_f32(vld1q_f32(&audioBuffer[i]), v));
#endif
            for (; i < floatCount; i++)
                sAdjustedAudio[i] = audioBuffer[i] * volume;
            streamData = sAdjustedAudio;
        }
        else
        {
            for (int i = 0; i < floatCount; i++)
                audioBuffer[i] *= volume;
        }
    }

    if (SDL_AudioStreamPut(sdlVitaAudioStream, streamData, samplesPerFrame) < 0)
    {
        SDL_Log("Failed to resample Vita audio input: %s", SDL_GetError());
        SDL_AudioStreamClear(sdlVitaAudioStream);
        return;
    }

    int availableBytes = SDL_AudioStreamAvailable(sdlVitaAudioStream);
    if (availableBytes < 0)
    {
        SDL_Log("Failed to query resampled Vita audio: %s", SDL_GetError());
        SDL_AudioStreamClear(sdlVitaAudioStream);
        return;
    }
    if (availableBytes == 0)
        return;
    if (availableBytes % (int)sizeof(float) != 0)
    {
        SDL_Log("Ignoring misaligned resampled Vita audio (%d bytes)", availableBytes);
        SDL_AudioStreamClear(sdlVitaAudioStream);
        return;
    }

    static float sResampledAudio[4096];
    int resampledBytes;
    const void *queueData = sResampledAudio;
    if (availableBytes <= (int)sizeof(sResampledAudio))
    {
        resampledBytes = SDL_AudioStreamGet(sdlVitaAudioStream, sResampledAudio, availableBytes);
    }
    else
    {
        float *dynamicBuf = (float *)SDL_malloc((size_t)availableBytes);
        if (dynamicBuf == NULL)
            return;
        resampledBytes = SDL_AudioStreamGet(sdlVitaAudioStream, dynamicBuf, availableBytes);
        queueData = dynamicBuf;
    }
    if (resampledBytes < 0)
    {
        if (queueData != sResampledAudio)
            SDL_free((void *)queueData);
        SDL_Log("Failed to read resampled Vita audio: %s", SDL_GetError());
        SDL_AudioStreamClear(sdlVitaAudioStream);
        return;
    }
    if (resampledBytes > 0)
    {
#if VITA_PERF_ENABLED
        uint64_t queueStart = VitaPerf_Now();
#endif
        int queueResult = SDL_QueueAudio(sdlAudioDevice, queueData, resampledBytes);
#if VITA_PERF_ENABLED
        uint64_t queueEnd = VitaPerf_Now();
        VitaPerf_RecordAudioQueue(queueStart, queueEnd, queuedBeforeBytes,
                                  SDL_GetQueuedAudioSize(sdlAudioDevice),
                                  VITA_DEVICE_AUDIO_RATE * 2 * sizeof(float),
                                  overflowCleared);
#endif
        if (queueResult < 0)
        {
            SDL_Log("Failed to queue resampled Vita audio: %s", SDL_GetError());
            SDL_AudioStreamClear(sdlVitaAudioStream);
        }
    }
    if (queueData != sResampledAudio)
        SDL_free((void *)queueData);
}
#endif

void Platform_QueueAudio(float *audioBuffer, s32 samplesPerFrame)
{
#ifdef PLATFORM_VITA
    if (audioBuffer == NULL || samplesPerFrame <= 0
     || SDL_AtomicGet(&sVitaAudioQueueClosing))
        return;

    SDL_AtomicAdd(&sVitaAudioQueueActive, 1);
    if (SDL_AtomicGet(&sVitaAudioQueueClosing))
    {
        SDL_AtomicAdd(&sVitaAudioQueueActive, -1);
        return;
    }

    if (sdlAudioDevice != 0)
    {
#if VITA_PERF_ENABLED
        uint64_t audioSupplyStart = VitaPerf_Now();
#endif
        QueueVitaAudio(audioBuffer, samplesPerFrame);
#if VITA_PERF_ENABLED
        VitaPerf_Record(VITA_PERF_AUDIO_SUPPLY, audioSupplyStart, VitaPerf_Now());
#endif
    }

    SDL_AtomicAdd(&sVitaAudioQueueActive, -1);
#else
    if (sdlAudioDevice != 0 && audioBuffer != NULL && samplesPerFrame > 0)
    {
        if (samplesPerFrame % (s32)sizeof(float) != 0)
        {
            SDL_Log("Ignoring misaligned audio buffer (%d bytes)", samplesPerFrame);
            return;
        }

        int floatCount = samplesPerFrame / sizeof(float);
        float adjustedAudio[floatCount];
        float volume = sPlatformSettings[PLATFORM_SETTING_VOLUME] / 10.0f;
        for (int i = 0; i < floatCount; i++)
            adjustedAudio[i] = audioBuffer[i] * volume;
        if (SDL_QueueAudio(sdlAudioDevice, adjustedAudio, samplesPerFrame) < 0)
            SDL_Log("Failed to queue audio: %s", SDL_GetError());
    }
#endif
}

u8 Platform_GetBorderBackgroundCount(void)
{
    return sBorderBackgroundCount + 1;
}

u8 Platform_GetBorderBackground(void)
{
    if (sHasBorderBackgroundConfig)
        return sBorderBackground;
    if (gSaveBlock2Ptr != NULL)
    {
        u8 legacySelection = gSaveBlock2Ptr->optionsBorderBackground;
        if (legacySelection == 1)
            return sBorderBackgroundCount;
        if (legacySelection >= 2)
            return legacySelection - 1;
    }
    return 0;
}

void Platform_SetBorderBackground(u8 selection)
{
    sBorderBackground = selection;
    sHasBorderBackgroundConfig = true;
    StoreConfigFile();
}

u8 Platform_GetSetting(enum PlatformSetting setting)
{
    return sPlatformSettings[setting];
}

void Platform_SetSetting(enum PlatformSetting setting, u8 value)
{
    sPlatformSettings[setting] = value;
#if defined(PLATFORM_VITA) && defined(VOXEL_SUPPORTED)
    if (setting == PLATFORM_SETTING_VOXEL_3D)
        gVoxelModeEnabled = (value != 0);
#endif
    if (setting == PLATFORM_SETTING_VSYNC && sdlRenderer != NULL)
        SDL_RenderSetVSync(sdlRenderer, value);
#if defined(NATIVE_LINUX) || defined(_WIN32)
    else if (setting == PLATFORM_SETTING_FULLSCREEN)
    {
        SDL_SetWindowFullscreen(sdlWindow, value ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        if (!value)
        {
            int scale = sPlatformSettings[PLATFORM_SETTING_WINDOW_SCALE];
            SDL_SetWindowSize(sdlWindow, 320 * scale, 180 * scale);
            SDL_SetWindowPosition(sdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    }
    else if (setting == PLATFORM_SETTING_WINDOW_SCALE && !sPlatformSettings[PLATFORM_SETTING_FULLSCREEN])
    {
        SDL_SetWindowSize(sdlWindow, 320 * value, 180 * value);
        SDL_SetWindowPosition(sdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
#endif
    StoreConfigFile();
}

#ifdef __ANDROID__
JNIEXPORT jint JNICALL Java_com_pokeemerald_experimental_GbaControlsView_getBorderBackground(JNIEnv *env, jclass clazz)
{
    return Platform_GetBorderBackground();
}

JNIEXPORT jint JNICALL Java_com_pokeemerald_experimental_GbaControlsView_getPlatformSetting(JNIEnv *env, jclass clazz, jint setting)
{
    if (setting < 0 || setting >= PLATFORM_SETTING_COUNT)
        return 0;
    return Platform_GetSetting(setting);
}
#endif


// Key mappings
#define KEY_A_BUTTON      SDLK_z
#define KEY_B_BUTTON      SDLK_x
#define KEY_START_BUTTON  SDLK_RETURN
#define KEY_SELECT_BUTTON SDLK_BACKSLASH
#define KEY_L_BUTTON      SDLK_a
#define KEY_R_BUTTON      SDLK_s
#define KEY_DPAD_UP       SDLK_UP
#define KEY_DPAD_DOWN     SDLK_DOWN
#define KEY_DPAD_LEFT     SDLK_LEFT
#define KEY_DPAD_RIGHT    SDLK_RIGHT

#define HANDLE_KEYUP(key) \
case KEY_##key:  keyboardKeys &= ~key; break;

#define HANDLE_KEYDOWN(key) \
case KEY_##key:  keyboardKeys |= key; break;

static u16 keyboardKeys;

#ifdef __ANDROID__
#define MAX_TOUCH_FINGERS 10

struct TouchFinger
{
    SDL_FingerID id;
    float x;
    float y;
    bool active;
};

static struct TouchFinger touchFingers[MAX_TOUCH_FINGERS];
static u16 touchKeys;

static bool IsInsideRect(int x, int y, SDL_Rect rect)
{
    SDL_Point point = {x, y};
    return SDL_PointInRect(&point, &rect);
}

static int MinInt(int a, int b)
{
    return a < b ? a : b;
}

static int GetControlSideWidth(int windowWidth, int windowHeight)
{
    int sideWidth = (windowWidth - windowHeight * 3 / 2) / 2;
    int minimumWidth = windowWidth * 14 / 100;
    return sideWidth > minimumWidth ? sideWidth : minimumWidth;
}

static void UpdateTouchKeys(void)
{
    int windowWidth;
    int windowHeight;
    SDL_GetWindowSize(sdlWindow, &windowWidth, &windowHeight);
    int sideWidth = GetControlSideWidth(windowWidth, windowHeight);
    int buttonSize = MinInt(sideWidth * 2 / 5, windowHeight / 6);
    int dpadUnit = MinInt(sideWidth / 3, windowHeight / 8);
    int dpadX = sideWidth * 2 / 3;
    int dpadY = windowHeight * 7 / 10;
    SDL_Rect dpadUp = {dpadX - dpadUnit / 2, dpadY - dpadUnit * 3 / 2,
                       dpadUnit, dpadUnit};
    SDL_Rect dpadDown = {dpadX - dpadUnit / 2, dpadY + dpadUnit / 2,
                         dpadUnit, dpadUnit};
    SDL_Rect dpadLeft = {dpadX - dpadUnit * 3 / 2, dpadY - dpadUnit / 2,
                         dpadUnit, dpadUnit};
    SDL_Rect dpadRight = {dpadX + dpadUnit / 2, dpadY - dpadUnit / 2,
                          dpadUnit, dpadUnit};
    SDL_Rect aButton = {windowWidth - sideWidth / 4 - buttonSize,
                        windowHeight * 58 / 100, buttonSize, buttonSize};
    SDL_Rect bButton = {windowWidth - sideWidth + sideWidth / 4,
                        windowHeight * 76 / 100, buttonSize, buttonSize};
    SDL_Rect selectButton = {sideWidth / 4, windowHeight / 4,
                             sideWidth / 2, windowHeight / 10};
    SDL_Rect startButton = {windowWidth - sideWidth * 3 / 4, windowHeight / 4,
                            sideWidth / 2, windowHeight / 10};
    SDL_Rect lButton = {sideWidth / 4, windowHeight / 20,
                        sideWidth / 2, windowHeight / 10};
    SDL_Rect rButton = {windowWidth - sideWidth * 3 / 4, windowHeight / 20,
                        sideWidth / 2, windowHeight / 10};

    touchKeys = 0;

    for (int i = 0; i < MAX_TOUCH_FINGERS; i++)
    {
        if (!touchFingers[i].active)
            continue;

        int x = touchFingers[i].x * windowWidth;
        int y = touchFingers[i].y * windowHeight;

        if (IsInsideRect(x, y, dpadUp)) touchKeys |= DPAD_UP;
        if (IsInsideRect(x, y, dpadDown)) touchKeys |= DPAD_DOWN;
        if (IsInsideRect(x, y, dpadLeft)) touchKeys |= DPAD_LEFT;
        if (IsInsideRect(x, y, dpadRight)) touchKeys |= DPAD_RIGHT;

        if (IsInsideRect(x, y, aButton)) touchKeys |= A_BUTTON;
        if (IsInsideRect(x, y, bButton)) touchKeys |= B_BUTTON;
        if (IsInsideRect(x, y, startButton)) touchKeys |= START_BUTTON;
        if (IsInsideRect(x, y, selectButton)) touchKeys |= SELECT_BUTTON;
        if (IsInsideRect(x, y, lButton)) touchKeys |= L_BUTTON;
        if (IsInsideRect(x, y, rButton)) touchKeys |= R_BUTTON;
    }
}

static void HandleTouchEvent(const SDL_TouchFingerEvent *event)
{
    int slot = -1;
    for (int i = 0; i < MAX_TOUCH_FINGERS; i++)
    {
        if (touchFingers[i].active && touchFingers[i].id == event->fingerId)
        {
            slot = i;
            break;
        }
        if (slot < 0 && !touchFingers[i].active)
            slot = i;
    }

    if (slot < 0)
        return;

    if (event->type == SDL_FINGERUP)
    {
        touchFingers[slot].active = false;
    }
    else
    {
        touchFingers[slot].id = event->fingerId;
        touchFingers[slot].x = event->x;
        touchFingers[slot].y = event->y;
        touchFingers[slot].active = true;
    }

    UpdateTouchKeys();
}

static const Uint8 *GetGlyph(char character)
{
    static const Uint8 glyphA[7] = {14, 17, 17, 31, 17, 17, 17};
    static const Uint8 glyphB[7] = {30, 17, 17, 30, 17, 17, 30};
    static const Uint8 glyphC[7] = {15, 16, 16, 16, 16, 16, 15};
    static const Uint8 glyphE[7] = {31, 16, 16, 30, 16, 16, 31};
    static const Uint8 glyphL[7] = {16, 16, 16, 16, 16, 16, 31};
    static const Uint8 glyphR[7] = {30, 17, 17, 30, 20, 18, 17};
    static const Uint8 glyphS[7] = {15, 16, 16, 14, 1, 1, 30};
    static const Uint8 glyphT[7] = {31, 4, 4, 4, 4, 4, 4};

    switch (character)
    {
    case 'A': return glyphA;
    case 'B': return glyphB;
    case 'C': return glyphC;
    case 'E': return glyphE;
    case 'L': return glyphL;
    case 'R': return glyphR;
    case 'S': return glyphS;
    case 'T': return glyphT;
    default:  return NULL;
    }
}

static void DrawControlLabel(SDL_Rect rect, const char *label)
{
    int length = SDL_strlen(label);
    int scale = MinInt(rect.h / 9, rect.w / (length * 6));
    if (scale < 1)
        scale = 1;
    int startX = rect.x + (rect.w - (length * 6 - 1) * scale) / 2;
    int startY = rect.y + (rect.h - 7 * scale) / 2;

    SDL_SetRenderDrawColor(sdlRenderer, 255, 255, 255, 230);
    for (int character = 0; character < length; character++)
    {
        const Uint8 *glyph = GetGlyph(label[character]);
        if (glyph == NULL)
            continue;
        for (int row = 0; row < 7; row++)
        {
            for (int column = 0; column < 5; column++)
            {
                if (glyph[row] & (1 << (4 - column)))
                {
                    SDL_Rect pixel = {startX + (character * 6 + column) * scale,
                                      startY + row * scale, scale, scale};
                    SDL_RenderFillRect(sdlRenderer, &pixel);
                }
            }
        }
    }
}

static void DrawControlRect(SDL_Rect rect, bool pressed, const char *label)
{
    SDL_SetRenderDrawColor(sdlRenderer, 255, 255, 255, pressed ? 150 : 65);
    SDL_RenderFillRect(sdlRenderer, &rect);
    SDL_SetRenderDrawColor(sdlRenderer, 255, 255, 255, pressed ? 230 : 130);
    SDL_RenderDrawRect(sdlRenderer, &rect);
    if (label != NULL)
        DrawControlLabel(rect, label);
}

static void DrawTouchControls(void)
{
    int windowWidth;
    int windowHeight;
    SDL_GetWindowSize(sdlWindow, &windowWidth, &windowHeight);
    int sideWidth = GetControlSideWidth(windowWidth, windowHeight);
    int buttonSize = MinInt(sideWidth * 2 / 5, windowHeight / 6);
    int dpadUnit = MinInt(sideWidth / 3, windowHeight / 8);
    int dpadX = sideWidth * 2 / 3;
    int dpadY = windowHeight * 7 / 10;

    SDL_RenderSetLogicalSize(sdlRenderer, 0, 0);
    SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_BLEND);

    DrawControlRect((SDL_Rect){dpadX - dpadUnit / 2, dpadY - dpadUnit * 3 / 2,
                               dpadUnit, dpadUnit}, touchKeys & DPAD_UP, NULL);
    DrawControlRect((SDL_Rect){dpadX - dpadUnit / 2, dpadY + dpadUnit / 2,
                               dpadUnit, dpadUnit}, touchKeys & DPAD_DOWN, NULL);
    DrawControlRect((SDL_Rect){dpadX - dpadUnit * 3 / 2, dpadY - dpadUnit / 2,
                               dpadUnit, dpadUnit}, touchKeys & DPAD_LEFT, NULL);
    DrawControlRect((SDL_Rect){dpadX + dpadUnit / 2, dpadY - dpadUnit / 2,
                               dpadUnit, dpadUnit}, touchKeys & DPAD_RIGHT, NULL);
    DrawControlRect((SDL_Rect){windowWidth - sideWidth / 4 - buttonSize,
                               windowHeight * 58 / 100, buttonSize, buttonSize}, touchKeys & A_BUTTON, "A");
    DrawControlRect((SDL_Rect){windowWidth - sideWidth + sideWidth / 4,
                               windowHeight * 76 / 100, buttonSize, buttonSize}, touchKeys & B_BUTTON, "B");
    DrawControlRect((SDL_Rect){windowWidth - sideWidth * 3 / 4, windowHeight / 4,
                               sideWidth / 2, windowHeight / 10}, touchKeys & START_BUTTON, "START");
    DrawControlRect((SDL_Rect){sideWidth / 4, windowHeight / 4,
                               sideWidth / 2, windowHeight / 10}, touchKeys & SELECT_BUTTON, "SELECT");
    DrawControlRect((SDL_Rect){sideWidth / 4, windowHeight / 20,
                               sideWidth / 2, windowHeight / 10}, touchKeys & L_BUTTON, "L");
    DrawControlRect((SDL_Rect){windowWidth - sideWidth * 3 / 4, windowHeight / 20,
                               sideWidth / 2, windowHeight / 10}, touchKeys & R_BUTTON, "R");

    SDL_SetRenderDrawColor(sdlRenderer, 0, 0, 0, 255);
    SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_NONE);
    SDL_RenderSetLogicalSize(sdlRenderer, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    SDL_RenderSetIntegerScale(sdlRenderer, SDL_TRUE);
}

#endif

#if defined(__ANDROID__) || defined(PLATFORM_VITA)
static u16 ControllerButtonMask(Uint8 button)
{
    switch (button)
    {
    case SDL_CONTROLLER_BUTTON_A:             return A_BUTTON;
    case SDL_CONTROLLER_BUTTON_B:             return B_BUTTON;
    case SDL_CONTROLLER_BUTTON_BACK:          return SELECT_BUTTON;
    case SDL_CONTROLLER_BUTTON_START:         return START_BUTTON;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return L_BUTTON;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return R_BUTTON;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return DPAD_RIGHT;
    default:                                  return 0;
    }
}
#endif

void ProcessEvents(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_QUIT:
            isRunning = false;
            break;
#ifdef PLATFORM_VITA
        case SDL_APP_DIDENTERFOREGROUND:
            RequestVitaDefaultArmClock("foreground");
            break;
#endif
#if defined(__ANDROID__) || defined(PLATFORM_VITA)
        case SDL_CONTROLLERDEVICEADDED:
            if (platformController == NULL && SDL_IsGameController(event.cdevice.which))
            {
                platformController = SDL_GameControllerOpen(event.cdevice.which);
                if (platformController == NULL)
                    SDL_Log("Unable to open SDL game controller: %s", SDL_GetError());
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (platformController != NULL
             && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(platformController)) == event.cdevice.which)
            {
                SDL_GameControllerClose(platformController);
                platformController = NULL;
                controllerKeys = 0;
                controllerAxisKeys = 0;
                controllerAxisX = 0;
                controllerAxisY = 0;
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
#ifdef PLATFORM_VITA
            /* Square and Triangle are reserved for Vita display shortcuts. */
            if (event.cbutton.button != SDL_CONTROLLER_BUTTON_X
             && event.cbutton.button != SDL_CONTROLLER_BUTTON_Y)
#endif
            {
                u16 buttonMask = ControllerButtonMask(event.cbutton.button);
                controllerKeys |= buttonMask;
            }
            break;
        case SDL_CONTROLLERBUTTONUP:
        {
            u16 buttonMask = ControllerButtonMask(event.cbutton.button);
            controllerKeys &= ~buttonMask;
#ifdef PLATFORM_VITA
            if (event.cbutton.button == SDL_CONTROLLER_BUTTON_X)
            {
#ifdef VOXEL_SUPPORTED
                u8 nextVoxel = (sPlatformSettings[PLATFORM_SETTING_VOXEL_3D] != 0) ? 0 : 1;
                gVoxelModeEnabled = (nextVoxel != 0);
                Platform_SetSetting(PLATFORM_SETTING_VOXEL_3D, nextVoxel);
                sVitaClearFramesRemaining = 3;
                sVitaDisplayDirty = true;
#endif
            }
            else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_Y)
            {
                sVitaStretchDisplay = !sVitaStretchDisplay;
                sVitaClearFramesRemaining = 3;
                sVitaDisplayDirty = true;
                StoreConfigFile();
            }
#endif
            break;
        }
        case SDL_CONTROLLERAXISMOTION:
            if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)
                controllerAxisX = event.caxis.value;
            else if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
                controllerAxisY = event.caxis.value;

            controllerAxisKeys = 0;
            if (controllerAxisX < -16000) controllerAxisKeys |= DPAD_LEFT;
            if (controllerAxisX >  16000) controllerAxisKeys |= DPAD_RIGHT;
            if (controllerAxisY < -16000) controllerAxisKeys |= DPAD_UP;
            if (controllerAxisY >  16000) controllerAxisKeys |= DPAD_DOWN;
            break;
#endif
        case SDL_KEYUP:
            switch (event.key.keysym.sym)
            {
            HANDLE_KEYUP(A_BUTTON)
            HANDLE_KEYUP(B_BUTTON)
            HANDLE_KEYUP(START_BUTTON)
            HANDLE_KEYUP(SELECT_BUTTON)
            HANDLE_KEYUP(L_BUTTON)
            HANDLE_KEYUP(R_BUTTON)
            HANDLE_KEYUP(DPAD_UP)
            HANDLE_KEYUP(DPAD_DOWN)
            HANDLE_KEYUP(DPAD_LEFT)
            HANDLE_KEYUP(DPAD_RIGHT)
            case SDLK_SPACE:
                if (speedUp)
                {
                    speedUp = false;
                    timeScale = 1.0;
                    SDL_ClearQueuedAudio(sdlAudioDevice);
#ifdef PLATFORM_VITA
                    VitaPerf_ResetAudioQueueTiming();
#endif
                    SDL_PauseAudioDevice(sdlAudioDevice, 0);
                }
                break;
            }
            break;
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym)
            {
            HANDLE_KEYDOWN(A_BUTTON)
            HANDLE_KEYDOWN(B_BUTTON)
            HANDLE_KEYDOWN(START_BUTTON)
            HANDLE_KEYDOWN(SELECT_BUTTON)
            HANDLE_KEYDOWN(L_BUTTON)
            HANDLE_KEYDOWN(R_BUTTON)
            HANDLE_KEYDOWN(DPAD_UP)
            HANDLE_KEYDOWN(DPAD_DOWN)
            HANDLE_KEYDOWN(DPAD_LEFT)
            HANDLE_KEYDOWN(DPAD_RIGHT)
            case SDLK_r:
                if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                {
                    DoSoftReset();
                }
                break;
            case SDLK_p:
                if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                {
                    paused = !paused;
#ifdef PLATFORM_VITA
                    VitaPerf_ResetAudioQueueTiming();
#endif
                }
                break;
            case SDLK_SPACE:
                if (!speedUp)
                {
                    speedUp = true;
                    timeScale = 5.0;
                    SDL_PauseAudioDevice(sdlAudioDevice, 1);
#ifdef PLATFORM_VITA
                    VitaPerf_ResetAudioQueueTiming();
#endif
                }
                break;
            }
            break;
        }
    }
}

#ifdef _WIN32
#define STICK_THRESHOLD 0.5f
u16 GetXInputKeys()
{
    XINPUT_STATE state;
    ZeroMemory(&state, sizeof(XINPUT_STATE));

    DWORD dwResult = XInputGetState(0, &state);
    u16 xinputKeys = 0;

    if (dwResult == ERROR_SUCCESS)
    {
        /* A */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) >> 12;
        /* B */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_X) >> 13;
        /* Start */  xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) >> 1;
        /* Select */ xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) >> 3;
        /* L */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) << 1;
        /* R */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) >> 1;
        /* Up */     xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) << 6;
        /* Down */   xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) << 6;
        /* Left */   xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) << 3;
        /* Right */  xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) << 1;


        /* Control Stick */
        float xAxis = (float)state.Gamepad.sThumbLX / (float)SHRT_MAX;
        float yAxis = (float)state.Gamepad.sThumbLY / (float)SHRT_MAX;

        if (xAxis < -STICK_THRESHOLD) xinputKeys |= DPAD_LEFT;
        if (xAxis >  STICK_THRESHOLD) xinputKeys |= DPAD_RIGHT;
        if (yAxis < -STICK_THRESHOLD) xinputKeys |= DPAD_DOWN;
        if (yAxis >  STICK_THRESHOLD) xinputKeys |= DPAD_UP;


        /* Speedup */
        // Note: 'speedup' variable is only (un)set on keyboard input
        double oldTimeScale = timeScale;
        timeScale = (state.Gamepad.bRightTrigger > 0x80 || speedUp) ? 5.0 : 1.0;

        if (oldTimeScale != timeScale)
        {
            if (timeScale > 1.0)
            {
                SDL_PauseAudioDevice(sdlAudioDevice, 1);
#ifdef PLATFORM_VITA
                VitaPerf_ResetAudioQueueTiming();
#endif
            }
            else
            {
                SDL_ClearQueuedAudio(sdlAudioDevice);
                VitaPerf_ResetAudioQueueTiming();
                SDL_PauseAudioDevice(sdlAudioDevice, 0);
            }
        }
    }

    return xinputKeys;
}
#endif // _WIN32

u16 Platform_GetKeyInput(void)
{
#ifdef _WIN32
    u16 gamepadKeys = GetXInputKeys();
    return gamepadKeys | keyboardKeys;
#elif defined(__ANDROID__)
    return keyboardKeys | controllerKeys | controllerAxisKeys;
#elif defined(PLATFORM_VITA)
    return keyboardKeys | controllerKeys | controllerAxisKeys;
#endif

    return keyboardKeys;
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

static const uint8_t sRgb5To8Table[32] = {
    0, 8, 16, 24, 32, 41, 49, 57, 65, 74, 82, 90, 98, 106, 115, 123,
    131, 139, 148, 156, 164, 172, 180, 189, 197, 205, 213, 222, 230, 238, 246, 255
};

#ifdef PLATFORM_VITA
void GbaConvertDisplayFrameToRgba(const uint16_t *src, uint32_t *dst, size_t count)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    uint8x8x4_t lut;
    lut.val[0] = vld1_u8(&sRgb5To8Table[0]);
    lut.val[1] = vld1_u8(&sRgb5To8Table[8]);
    lut.val[2] = vld1_u8(&sRgb5To8Table[16]);
    lut.val[3] = vld1_u8(&sRgb5To8Table[24]);
    uint8x8_t alpha = vdup_n_u8(0xFF);
    const uint16x8_t mask5 = vdupq_n_u16(0x001F);
    size_t i = 0;
    size_t count8 = count & ~7u;

    for (; i < count8; i += 8)
    {
        uint16x8_t raw = vld1q_u16(&src[i]);
        uint8x8_t r_idx = vmovn_u16(vandq_u16(raw, mask5));
        uint8x8_t g_idx = vmovn_u16(vandq_u16(vshrq_n_u16(raw, 5), mask5));
        uint8x8_t b_idx = vmovn_u16(vandq_u16(vshrq_n_u16(raw, 10), mask5));

        uint8x8x4_t out;
        out.val[0] = vtbl4_u8(lut, r_idx);
        out.val[1] = vtbl4_u8(lut, g_idx);
        out.val[2] = vtbl4_u8(lut, b_idx);
        out.val[3] = alpha;

        vst4_u8((uint8_t *)&dst[i], out);
    }
    for (; i < count; i++)
    {
        uint16_t c = src[i];
        dst[i] = (uint32_t)sRgb5To8Table[c & 0x1F] |
                 ((uint32_t)sRgb5To8Table[(c >> 5) & 0x1F] << 8) |
                 ((uint32_t)sRgb5To8Table[(c >> 10) & 0x1F] << 16) |
                 0xFF000000u;
    }
#else
    size_t i = 0;
    size_t count4 = count & ~3u;
    for (; i < count4; i += 4)
    {
        uint16_t c0 = src[i];
        uint16_t c1 = src[i + 1];
        uint16_t c2 = src[i + 2];
        uint16_t c3 = src[i + 3];

        dst[i]     = (uint32_t)sRgb5To8Table[c0 & 0x1F] |
                     ((uint32_t)sRgb5To8Table[(c0 >> 5) & 0x1F] << 8) |
                     ((uint32_t)sRgb5To8Table[(c0 >> 10) & 0x1F] << 16) |
                     0xFF000000u;
        dst[i + 1] = (uint32_t)sRgb5To8Table[c1 & 0x1F] |
                     ((uint32_t)sRgb5To8Table[(c1 >> 5) & 0x1F] << 8) |
                     ((uint32_t)sRgb5To8Table[(c1 >> 10) & 0x1F] << 16) |
                     0xFF000000u;
        dst[i + 2] = (uint32_t)sRgb5To8Table[c2 & 0x1F] |
                     ((uint32_t)sRgb5To8Table[(c2 >> 5) & 0x1F] << 8) |
                     ((uint32_t)sRgb5To8Table[(c2 >> 10) & 0x1F] << 16) |
                     0xFF000000u;
        dst[i + 3] = (uint32_t)sRgb5To8Table[c3 & 0x1F] |
                     ((uint32_t)sRgb5To8Table[(c3 >> 5) & 0x1F] << 8) |
                     ((uint32_t)sRgb5To8Table[(c3 >> 10) & 0x1F] << 16) |
                     0xFF000000u;
    }
    for (; i < count; i++)
    {
        uint16_t c = src[i];
        dst[i] = (uint32_t)sRgb5To8Table[c & 0x1F] |
                 ((uint32_t)sRgb5To8Table[(c >> 5) & 0x1F] << 8) |
                 ((uint32_t)sRgb5To8Table[(c >> 10) & 0x1F] << 16) |
                 0xFF000000u;
    }
#endif
}
#endif

#ifdef PLATFORM_VITA
static inline void GbaConvertDisplayFrameToRgb565(const uint16_t *src, uint16_t *dst, size_t count)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const uint16x8_t mask5 = vdupq_n_u16(0x001Fu);
    const uint16x8_t maskG5 = vdupq_n_u16(0x03E0u);
    const uint16x8_t maskG0 = vdupq_n_u16(0x0020u);
    size_t i = 0;
    size_t count16 = count & ~15u;

    for (; i < count16; i += 16)
    {
        __builtin_prefetch(&src[i + 32]);
        uint16x8_t raw0 = vld1q_u16(&src[i]);
        uint16x8_t raw1 = vld1q_u16(&src[i + 8]);
        uint16x8_t r0 = vshlq_n_u16(vandq_u16(raw0, mask5), 11);
        uint16x8_t r1 = vshlq_n_u16(vandq_u16(raw1, mask5), 11);
        uint16x8_t g0 = vshlq_n_u16(vandq_u16(raw0, maskG5), 1);
        uint16x8_t g1 = vshlq_n_u16(vandq_u16(raw1, maskG5), 1);
        uint16x8_t l0 = vandq_u16(vshrq_n_u16(raw0, 4), maskG0);
        uint16x8_t l1 = vandq_u16(vshrq_n_u16(raw1, 4), maskG0);
        uint16x8_t b0 = vandq_u16(vshrq_n_u16(raw0, 10), mask5);
        uint16x8_t b1 = vandq_u16(vshrq_n_u16(raw1, 10), mask5);
        vst1q_u16(&dst[i], vorrq_u16(vorrq_u16(r0, g0), vorrq_u16(l0, b0)));
        vst1q_u16(&dst[i + 8], vorrq_u16(vorrq_u16(r1, g1), vorrq_u16(l1, b1)));
    }
    for (; i + 8 <= count; i += 8)
    {
        uint16x8_t raw = vld1q_u16(&src[i]);
        uint16x8_t r = vshlq_n_u16(vandq_u16(raw, mask5), 11);
        uint16x8_t g = vshlq_n_u16(vandq_u16(raw, maskG5), 1);
        uint16x8_t g0 = vandq_u16(vshrq_n_u16(raw, 4), maskG0);
        uint16x8_t b = vandq_u16(vshrq_n_u16(raw, 10), mask5);
        vst1q_u16(&dst[i], vorrq_u16(vorrq_u16(r, g), vorrq_u16(g0, b)));
    }
    for (; i < count; i++)
    {
        uint16_t c = src[i];
        dst[i] = ((c & 0x001Fu) << 11) | ((c & 0x03E0u) << 1) | ((c >> 4) & 0x0020u) | ((c >> 10) & 0x001Fu);
    }
#else
    for (size_t i = 0; i < count; i++)
    {
        uint16_t c = src[i];
        dst[i] = ((c & 0x001Fu) << 11) | ((c & 0x03E0u) << 1) | ((c >> 4) & 0x0020u) | ((c >> 10) & 0x001Fu);
    }
#endif
}
#endif

void VDraw(SDL_Texture *texture)
{
    static uint16_t gbaImage[DISPLAY_WIDTH * DISPLAY_HEIGHT] __attribute__((aligned(16)));
#ifdef PLATFORM_VITA
    (void)texture;
    DrawFrame(gbaImage);

    uint64_t convertStart = VitaPerf_Now();
    uint8_t nextIdx = (uint8_t)((sGameTextureUploadIdx + 1u) % VITA_GAME_TEXTURE_RING);
    uint16_t *directTexPtr = sVitaGlInitialized ? sGameTextureDataPtr[nextIdx] : NULL;
    if (directTexPtr != NULL)
    {
        GbaConvertDisplayFrameToRgb565(gbaImage, directTexPtr,
                                       DISPLAY_WIDTH * DISPLAY_HEIGHT);
        __sync_synchronize();
        sGameTextureUploadIdx = nextIdx;
        uint64_t convertEnd = VitaPerf_Now();
        VitaPerf_Record(VITA_PERF_PIXEL_CONVERT, convertStart, convertEnd);
        VitaPerf_Record(VITA_PERF_TEXTURE_UPDATE, convertEnd, convertEnd);
    }
    else
    {
        GbaConvertDisplayFrameToRgb565(gbaImage, sVitaRgb565Frame,
                                       DISPLAY_WIDTH * DISPLAY_HEIGHT);
        uint64_t convertEnd = VitaPerf_Now();
        VitaPerf_Record(VITA_PERF_PIXEL_CONVERT, convertStart, convertEnd);

        if (sVitaGlInitialized)
        {
            sGameTextureUploadIdx = nextIdx;
            glBindTexture(GL_TEXTURE_2D, sGameTextureRing[nextIdx]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                            GL_RGB, GL_UNSIGNED_SHORT_5_6_5, sVitaRgb565Frame);
        }
        VitaPerf_Record(VITA_PERF_TEXTURE_UPDATE, convertEnd, VitaPerf_Now());
    }
    REG_VCOUNT = 161; // prep for being in VBlank period
#else
    static uint32_t image[DISPLAY_WIDTH * DISPLAY_HEIGHT];
    static int sClassicFrames = 0;

    sClassicFrames++;
    DrawFrame(gbaImage);
    if (sClassicFrames == 900) {
        printf("[Classic2D] frame 900, p[100]=%04x\n", gbaImage[100]);
    }
    uint64_t convertStart = VitaPerf_Now();
    for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
    {
        uint16_t color = gbaImage[i];
        uint32_t r = sRgb5To8Table[color & 0x1F];
        uint32_t g = sRgb5To8Table[(color >> 5) & 0x1F];
        uint32_t b = sRgb5To8Table[(color >> 10) & 0x1F];
        image[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
    VitaPerf_Record(VITA_PERF_PIXEL_CONVERT, convertStart, VitaPerf_Now());
    uint64_t updateStart = VitaPerf_Now();
    SDL_UpdateTexture(texture, NULL, image, DISPLAY_WIDTH * sizeof(Uint32));
    VitaPerf_Record(VITA_PERF_TEXTURE_UPDATE, updateStart, VitaPerf_Now());
    REG_VCOUNT = 161; // prep for being in VBlank period
#endif
}

int DoMain(void *data)
{
#ifdef PLATFORM_VITA
    if (!VitaThread_PinCurrentToCore(1))
        sceClibPrintf("[VITA] AgbMain affinity to user core 1 failed; using scheduler defaults\n");
#endif
    AgbMain();
    return 0;
}

void VBlankIntrWait(void)
{
    uint64_t waitStart = VitaPerf_Now();
#if defined(PLATFORM_VITA) && defined(VOXEL_SUPPORTED)
    VoxelEntities_PublishSnapshot();
#endif
    SDL_AtomicSet(&isFrameAvailable, 1);
#ifdef PLATFORM_VITA
    if (sVitaFrameReadySem != NULL)
        SDL_SemPost(sVitaFrameReadySem);
#endif
    SDL_SemWait(vBlankSemaphore);
    VitaPerf_RecordGameWait(waitStart, VitaPerf_Now());
}

u8 BinToBcd(u8 bin)
{
    int placeCounter = 1;
    u8 out = 0;
    do
    {
        out |= (bin % 10) * placeCounter;
        placeCounter *= 16;
    }
    while ((bin /= 10) > 0);

    return out;
}

void Platform_GetStatus(struct SiiRtcInfo *rtc)
{
    rtc->status = internalClock.status;
}

void Platform_SetStatus(struct SiiRtcInfo *rtc)
{
    internalClock.status = rtc->status;
}

static void UpdateInternalClock(void)
{
    time_t rawTime = time(NULL);
    struct tm *time = localtime(&rawTime);

    internalClock.year = BinToBcd(time->tm_year - 100);
    internalClock.month = BinToBcd(time->tm_mon + 1);
    internalClock.day = BinToBcd(time->tm_mday);
    internalClock.dayOfWeek = BinToBcd(time->tm_wday);
    internalClock.hour = BinToBcd(time->tm_hour);
    internalClock.minute = BinToBcd(time->tm_min);
    internalClock.second = BinToBcd(time->tm_sec);
}

void Platform_GetDateTime(struct SiiRtcInfo *rtc)
{
    UpdateInternalClock();

    rtc->year = internalClock.year;
    rtc->month = internalClock.month;
    rtc->day = internalClock.day;
    rtc->dayOfWeek = internalClock.dayOfWeek;
    rtc->hour = internalClock.hour;
    rtc->minute = internalClock.minute;
    rtc->second = internalClock.second;
#ifndef PLATFORM_VITA
    DBGPRINTF("GetDateTime: %d-%02d-%02d %02d:%02d:%02d\n", ConvertBcdToBinary(rtc->year),
                                                         ConvertBcdToBinary(rtc->month),
                                                         ConvertBcdToBinary(rtc->day),
                                                         ConvertBcdToBinary(rtc->hour),
                                                         ConvertBcdToBinary(rtc->minute),
                                                         ConvertBcdToBinary(rtc->second));
#endif
}

void Platform_SetDateTime(struct SiiRtcInfo *rtc)
{
    internalClock.month = rtc->month;
    internalClock.day = rtc->day;
    internalClock.dayOfWeek = rtc->dayOfWeek;
    internalClock.hour = rtc->hour;
    internalClock.minute = rtc->minute;
    internalClock.second = rtc->second;
}

void Platform_GetTime(struct SiiRtcInfo *rtc)
{
    UpdateInternalClock();

    rtc->hour = internalClock.hour;
    rtc->minute = internalClock.minute;
    rtc->second = internalClock.second;
#ifndef PLATFORM_VITA
    DBGPRINTF("GetTime: %02d:%02d:%02d\n", ConvertBcdToBinary(rtc->hour),
                                        ConvertBcdToBinary(rtc->minute),
                                        ConvertBcdToBinary(rtc->second));
#endif
}

void Platform_SetTime(struct SiiRtcInfo *rtc)
{
    internalClock.hour = rtc->hour;
    internalClock.minute = rtc->minute;
    internalClock.second = rtc->second;
}

void Platform_SetAlarm(u8 *alarmData)
{
    // TODO
}

void SoftReset(u32 resetFlags)
{
#ifdef PLATFORM_VITA
    /* Reload the executable to reset game globals and platform workers together. */
    int result = sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
    SDL_Log("Soft reset could not restart the game: 0x%08x", (unsigned int)result);
#else
    puts("Soft Reset called. Exiting.");
#endif
    exit(0);
}

#endif
