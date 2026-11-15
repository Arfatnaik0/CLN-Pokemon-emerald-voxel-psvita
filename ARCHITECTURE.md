# Developer Architecture Guide — Pokémon Emerald Voxel (PS Vita)

This document explains how the PS Vita port (`PKEV00001`) works under the hood for developers who want to debug, profile, or adapt the codebase. Every symbol and file path below refers to files inside `port/`.

---

## 1. Execution & Threading Model

Entry and platform lifecycle live in [port/src/platform/sdl2.c](port/src/platform/sdl2.c).

- **Heap & GPU Memory Reservation**:
  - `port/src/platform/voxel/voxel_vita_bridge.c` sets `_newlib_heap_size_user = 64 * 1024 * 1024` (64 MB) so VitaSDK leaves ample CDRAM and physically contiguous memory for `vitaGL`.
  - `sdl2.c` initializes `vitaGL` with `vglInitWithCustomSizes(8 MB param buffer, 960, 544, ramPool<=56 MB, cdramPool<=88 MB, phycontPool<=8 MB, 0, SCE_GXM_MULTISAMPLE_NONE)`, `vglSetupGarbageCollector(127, SCE_KERNEL_CPU_MASK_USER_2)`, triple buffering, cached memory, and `eglSwapInterval(NULL, 1)`.
- **Thread Topology** (with `VITA_PIN_RENDER_THREADS=ON`):
  1. **Main / Render / Present Thread (`main`)**: Polls SDL2 controller events, steps fixed-timestep pacing (`1.0 / 60.0` s via `FramePacing_*` in `port/include/platform/frame_pacing.h`), drives either `VoxelRenderer_RenderFrame()` (3D) or `VDraw()` → `RenderVitaDisplayFrame()` (2D), dispatches `gIntrTable[4]` (`VBlankIntr`), wakes `AgbMain`, and presents via `PresentVitaDisplayFrame()`.
  2. **Emulation Thread (`DoMain` / `AgbMain`)**: Runs Pokémon Emerald game logic and calls `RunDMAs()` / `m4aSoundVSync()`, synchronizing with the render thread through `isFrameAvailable`, `vBlankSemaphore`, and `sVitaFrameReadySem`.
  3. **2D PPU Worker Threads (`port/src/platform/gba_easy_draw_workers.c`)**: Two persistent `EasyDrawWorkerMain` threads (pinned to Vita user cores `1` and `2`, cooperating with the render thread) pull 8-scanline chunks atomically (`__sync_fetch_and_add(&sNextChunkLine, 8)`). When HBlank DMA is absent or supported (`WIN0H` snapshot replay), all 3 cores render the 160-line GBA framebuffer simultaneously.

---

## 2. 2D GBA PPU Pipeline (`gba_easy_draw.c`)

Implemented in [port/src/platform/gba_easy_draw.c](port/src/platform/gba_easy_draw.c).

### Direct Scanline Path vs Fallback
- **Pre-frame Sprite Binning (`BuildSpriteScanlineBins`)**: Bins visible OAM entries into per-scanline arrays (`sSpriteBins`), computing `priorityMask[line]` and `hasDirectUnsupported[line]` (affine sprites, semi-transparent/OBJ-window mode, or active sprite mosaic).
- **Direct Composition (`DrawScanlineDirectWithDispcnt` / `TryDrawDirectScanlineWithRegs`)**: Used on Mode 0/1 scanlines with no windows and no unsupported sprites. Clears only active background/sprite priority buffers (`SpriteLayerClears`) instead of unconditionally clearing all layers, and composites layers in ARM NEON SIMD (`CompositeRow...Fast`).
- **Overworld Mosaic Elimination**:
  - Vanilla `overworld.c` (`InitOverworldBgs`) sets `BGCNT_MOSAIC` permanently on `BG1`–`BG3` even when `REG_MOSAIC == 0`.
  - `RenderBGScanline()`, `RenderRotScaleBGScanline()`, `BuildSpriteScanlineBins()`, `DrawSprites()`, and `DrawScanline()` gate mosaic behavior on whether the corresponding `REG_MOSAIC` nibble (`mosaicBGEffectX`, `mosaicBGEffectY`, `mosaicSpriteEffectX`, `mosaicSpriteEffectY`) is non-zero.
  - When mosaic *is* active, `ApplyMosaicFloorU()` uses a 16-entry fixed-point reciprocal table (`sMosaicInv[17]`) and scanline anchor counters (`nextAnchorX`), completely removing software `__aeabi_uidivmod` calls from PPU rendering.
- **HBlank Raster Effects (`WIN0H` Replay & VCOUNT Split)**:
  - With `VITA_ENABLE_HBLANK_WIN0_PARALLEL=ON`, supported `WIN0H` HBlank DMA sequences are snapshotted per row (`HasSafeHBlankReplayEvents`) before dispatching parallel workers, while DMA/register side effects are restored cleanly on any unsupported condition or test fault.
  - Known `VCOUNT` split points (`compare >= 150`) use a parallel prefix (`0..compare`) followed by serial completion of the suffix rows.

### Framebuffer Upload & Presentation (`sdl2.c`)
- **BGR555 → RGB565 Conversion (`GbaConvertDisplayFrameToRgb565`)**: ARM NEON loop unrolled to 16 pixels/iteration with `__builtin_prefetch`, writing directly into the next slot of an 8-buffer CDRAM texture ring (`sGameTextureRing` mapped via `vglGetTexDataPointer`).
- **Frame Pacing (`VITA_PRESENT_EACH_SERVICED_TICK=ON`)**: Presents each serviced tick via `glBegin(GL_TRIANGLE_FAN)` orthographic quad in either stretched (`960×544`) or pixel-exact `3:2` (`816×544`) viewport mode.

---

## 3. 3D Voxel Overworld Pipeline (`port/src/platform/voxel/`)

Engine sources reside in [port/src/platform/voxel/](port/src/platform/voxel/):

- **State & Logical Map View (`voxel_world.c`, `voxel_regions.c`, `voxel_camera.c`)**:
  - Reads live `gBackupMapLayout` for the current map and static layouts for seamless connected neighbor maps.
  - `overworld.c` (`sMapMutationLock`) guards map-transition reads so geometry extraction never tears mid-warp.
  - `src/field_door.c` (`PortDoor_Get`, `PortDoor_GetTwin`, `PortDoor_Forget`) exports live animated door frame state to `voxel_entities.c`.
  - `event_object_movement.c` widens NPC/object visibility handling when `Platform_IsVoxelOverworldActive()` is active so characters do not pop in early at 3D frustum edges.
- **Asset Formats Loaded from `app0:voxel/` or `ux0:data/pokeemerald/voxel/`**:
  - `buildings.bin` (`VXB9`): 3D building meshes, roof/wall materials, and placement conditions.
  - `relief.bin` / `regions.bin`: Continuous ground heightfields, ramps, ledges, and map region definitions.
  - `trees.rgba5551` & `signposts.bin`: Clean-alpha billboard trees, long grass (`VOXEL_TREE_GRASS`), and 3D wooden signposts; plus runtime-populated emerald tall grass (`MB_TALL_GRASS` at `(0,112)`) and Route 113 ash grass (`MB_ASHGRASS` at `(16,112)`) dual-row 3D blade cards emitted cleanly around `Pass_Entities` character depth (`voxel_tree.c` + `voxel_world.c`).
  - Optional per-layout `VBL4` packs (`port/tools/extract_voxel_layout_models.py`).
- **2D Overlay + Audio Sync in 3D Mode (`voxel_renderer.c`)**:
  - `Pass_Overlay2D()` renders `BG0` UI (text boxes, party/bag HUD, start menu) via `gba_easy_draw` onto a GL overlay texture, masking out opaque tiles.
  - Crucially, `VoxelRenderer_RenderFrame()` runs `RunFrameLineEvents()` so GBA HBlank/VBlank DMA & audio timers execute continuously even when full-frame 2D scanline compositing is bypassed in 3D mode.
- **GPU Stability (`vitagl-safe` & `-DDRAW_SPEEDHACK` Vertex Ring)**:
  - Links against a fixed-function-capable `vitaGL` build (`_glDrawArrays_FixedFunctionIMPL`) with ping-pong uniform arenas (`[VGL_UNIFORM_LIFETIME]`), avoiding runtime `libshacccg` shader compilation stalls/crashes and preventing stray white vertex/particle artifacts during 3D frame transitions.
  - Because `vitaGL` is built with `-DDRAW_SPEEDHACK` (forwarding client vertex pointers straight to `sceGxmSetVertexStream` without copying), `voxel_renderer.c` allocates a 6-slot GPU-mapped dynamic vertex stream ring (`vglMemalign` out of `sceGxmMapMemory`-mapped pools) for every non-VBO draw (`Pass_CastShadows`, `Pass_Reflections`, `Pass_Entities`, `Pass_DayNightGrade`, `Pass_NightLamps`, `Pass_Atmosphere3D`/`2D`, `Pass_DepthOfField`, `Pass_Overlay2D`), ensuring no stack, `.rodata`, `.bss`, or in-flight vertices are ever read directly by GXM.

---

## 4. Audio Pipeline (`42060 Hz` Game Mixer → `44100 Hz` Vita Hardware)

1. **CGB Synthesizer (`port/src/platform/cgb_audio.c`)**:
   - Synthesizes Game Boy pulse/wave/noise channels at `VITA_GAME_AUDIO_RATE` (`42060 Hz`).
   - Caches phase/period increments when audio control registers are unchanged (verified byte-for-byte via `tests/platform/run_cgb_audio_phase_cache.sh`).
2. **M4A Mixer (`src/sound_mixer.c`, `src/music_player.c`)**:
   - Guards BDPCM decompression indices against out-of-range nibbles/overflows.
   - `m4aSoundVSync()` mixes M4A + CGB float buffers in ARM NEON, replaces `NaN` samples with `0.0f`, applies `0.40f` Vita gain (`#ifdef PLATFORM_VITA`), and clamps to `[-1.0f, 1.0f]`.
3. **SDL2 Audio Stream (`sdl2.c` + `sdl2-vita-audio-42060.patch`)**:
   - `Platform_QueueAudio()` feeds `42060 Hz` stereo `AUDIO_F32` frames through `SDL_AudioStream` (`42060 → 44100 Hz`) into `SDL_QueueAudio()`.
   - Bounds queued latency to `VITA_AUDIO_QUEUE_LIMIT_FRAMES` (`4` frames), flushing stale backlog if queue depth grows beyond target budget.

---

## 5. Profiling & Debugging (`perf.log` & `vitagl_error.log`)

Implemented in [port/src/platform/vita_perf.c](port/src/platform/vita_perf.c) and [port/src/platform/voxel/voxel_vita_bridge.c](port/src/platform/voxel/voxel_vita_bridge.c).

- **`ux0:data/pokeemerald/perf.log`** logs startup parameters (`arm_clock_request`, renderer resolution/pools) and periodic 1-second rolling windows containing:
  - `emulated_fps`, `present_fps`, `present_misses`
  - Stage averages/maxes (`draw_frame_avg_us`, `pixel_convert_avg_us`, `texture_update_avg_us`, `render_queue_avg_us`, `present_avg_us`, `mixer_avg_us`, `audio_supply_n`/`avg_us`, `audio_queue_max_bytes`, `audio_queue_underrun`)
  - Parallel PPU breakdown (`easy_path_counts`, `easy_main_range_avg_us`, `easy_worker0_range_avg_us`, `easy_worker1_range_avg_us`, `active_sprites_avg`)
  - Current ARM clock (`vita_arm_clock_mhz`)
- **`ux0:data/pokeemerald/vitagl_error.log`** records free user/CDRAM/phycont memory sizes prior to `vglInitWithCustomSizes`, `[VGL_UNIFORM_LIFETIME]`, and any non-zero `glGetError()` / bridge failures.
- To disable performance telemetry for baseline builds, configure CMake with `-DVITA_ENABLE_PERF=OFF`.

---

## 6. Host Test Suite (`port/tests/platform/`)

Most subsystems have fast native host regression tests and microbenchmarks runnable on Linux before cross-compiling for Vita:

| Test Script | What It Verifies |
| --- | --- |
| `run_gba_draw_parity.sh` | Pixel parity between `gba_easy_draw` and `gba_fast_draw` across 64 seeded mode/window/blend/sprite/affine/raster scenes |
| `run_gba_easy_draw_perf.sh` | Pthread + SDL2 worker backends: 32 scanline mosaic/scroll/wrap cases, `WIN0H` HBlank-DMA snapshot/fallback parity, 140 mixed-priority background compositor scenes, sprite binning & prefilter parity, worker lifecycle tests |
| `run_cgb_audio_phase_cache.sh` | Bit-exact parity of CGB phase cache across `42060`, `42048`, `44100`, `32`, `33 Hz` |
| `run_sdl2_frame_pacing_test.sh` | Fixed-step accumulator (`1/60s`) & per-serviced-frame presentation model |
| `run_vita_audio_queue_metrics_test.sh` | Producer audio-queue underrun metric calculation |
| `run_vita_controller_state_test.sh` | Controller shortcut disconnect reset behavior |
| `run_vita_perf_rolling_test.sh` | Rolling profiler window counters & averages |
| `run_vita_vblank_startup_test.sh` & `vita_vblank_startup_source_test.py` | LCD `REG_VCOUNT=161` startup VBlank priming |
| `follower_spawn_safety_test.py` | Follower reservation / spawn safety checks |
| `run_mod_manager_init_test.sh` | `ModManager` initialization error handling |
| `run_sdl_audio_resampler_parity.sh` | Verifies Vita audio SDL2 patch against an external `SDL2_SOURCE` `2.30.7` checkout |
