# Pokémon Emerald Voxel: PS Vita port

A native PlayStation Vita port of the `pokeemerald-multiplatform` decompilation. It has the classic 2D GBA renderer, a live 3D voxel overworld mode, follower Pokémon, NEON and multi-core PPU optimizations, and `42060 → 44100 Hz` SDL2 audio. Both the 2D and 3D modes hold a locked 60 FPS at the default `222 MHz` ARM clock.

> **Data-free repository.** This repo contains no ROMs, no Nintendo or Game Freak assets, no compiled ELFs or VPKs, and no third-party sprite or voxel packs. See [SECURITY_OR_ASSET_POLICY.md](SECURITY_OR_ASSET_POLICY.md).
> To get a runnable VPK, you bring your own clean `pret/pokeemerald` or `pokeemerald-multiplatform` checkout, verify your own legally dumped Pokémon Emerald `.gba` ROM, and build on your own machine with VitaSDK.

---

## Features

- Runs natively on the Vita (`PKEV00001`, "Pokemon Emerald Voxel"). The decompiled C game logic executes directly on the Cortex-A9; this is not a GBA emulator. Saves go to `ux0:data/pokeemerald/pokeemerald.sav` and settings to `ux0:data/pokeemerald/pokeemerald.cfg`.
- Switches between 2D and 3D in the overworld. Press **Square** at any time to flip between classic 2D GBA rendering and the hardware-accelerated 3D voxel diorama (`vitaGL` fixed-function path). Battles, menus, interiors with no 3D fallback, and transitions always use the 2D path.
- Toggles display aspect with **Triangle**, between stretched widescreen (`960×544`) and a centered, pixel-accurate `3:2` (`816×544`).
- Supports follower Pokémon through external `FMN1` sprite packs, loaded from `app0:followers/` or `ux0:data/pokeemerald/followers/`.
- Keeps the GBA M4A and CGB synthesizers at their native `42060 Hz`. A CGB phase cache removes the per-sample phase recomputation, BDPCM samples are clamped and checked for NaN, and mixing runs in NEON SIMD (`0.40f` Vita output gain). One resample step then brings the output to the Vita's `44100 Hz` hardware rate (`port/vita/patches/sdl2-vita-audio-42060.patch`).
- Writes rolling per-stage CPU, PPU, audio and present timings to `ux0:data/pokeemerald/perf.log`, and GL pool diagnostics to `ux0:data/pokeemerald/vitagl_error.log`.

---

## Controls

| PS Vita input | Action |
| --- | --- |
| D-Pad / Left analog stick | Move, navigate menus |
| Cross (`×`) | GBA **A** |
| Circle (`○`) | GBA **B** |
| Start | GBA **Start** |
| Select | GBA **Select** |
| L / R | GBA **L** / **R** |
| Square (`□`) | Toggle 2D / 3D voxel overworld |
| Triangle (`△`) | Toggle widescreen stretch / 3:2 aspect |

---

## 3D voxel mode

The 3D renderer lives in `port/src/platform/voxel/`. It is a `vitaGL` diorama overworld renderer modeled on `emerald-3ds`.

- **Buildings** (`buildings.bin`): polygonal roofs, eaves, chamfers and doorways (`PortDoor_Get`), with a roof cutaway when the player walks into or behind a building.
- **Terrain** (`relief.bin`, `regions.bin`): continuous elevation, slopes, terraces and jump ledges.
- **Trees, grass and signposts** (`trees.rgba5551`, `signposts.bin`): alpha-tested billboard tree canopies and 3D wooden signposts. Tall grass (`MB_TALL_GRASS` and `MB_ASHGRASS`) uses dual-row blade cards, one set for emerald grass and one for volcanic ash, which rustle and throw leaf particles when you walk through them. Long grass (`MB_LONG_GRASS`) uses leaning cards.
- **2D overlay** (`Pass_Overlay2D`): `BG0` UI, dialogue boxes and the start menu are drawn on top of the 3D scene. GBA raster and scanline events (`RunFrameLineEvents`) keep advancing, so overworld audio stays in sync in 3D mode.
- **Fixed-function `vitaGL`**: the renderer needs no runtime shader compilation (`_glDrawArrays_FixedFunctionIMPL` plus a ping-pong uniform arena). That avoided GPU crashes and particle artifacts on real hardware.

For more detail, see [ARCHITECTURE.md](ARCHITECTURE.md) and [port/docs/VITA_VOXEL_VISUAL_LIMITATIONS.md](port/docs/VITA_VOXEL_VISUAL_LIMITATIONS.md).

---

## Performance notes

Both modes target a locked 60 FPS. The default ARM clock is `222 MHz`, and there is enough headroom to hold 60 FPS at `166 MHz` too.

### Multi-core GBA PPU

The PPU code is in `gba_easy_draw.c` and `gba_easy_draw_workers.c`.

- Scanline rendering is split into dynamic 8-line chunks (160 lines total) across the render thread and 2 persistent worker threads. The threads can be pinned to Vita user cores `0`, `1` and `2` with `VITA_PIN_RENDER_THREADS=ON`.
- Supported `WIN0H` HBlank DMA effects are replayed in parallel (`VITA_ENABLE_HBLANK_WIN0_PARALLEL=ON`), and `VCOUNT`-prefix splitting is supported.
- Scanline compositing uses ARM NEON: opaque fast paths for `4bpp` and `8bpp`, plus a NEON BGR555→RGB565 conversion. Frames go through an 8-slot CDRAM texture ring (`sGameTextureRing`).

### Overworld mosaic fast path

`InitOverworldBgs` permanently enables `BGCNT_MOSAIC` on `BG1`–`BG3`, even when `REG_MOSAIC == 0`. `gba_easy_draw.c` checks `(REG_MOSAIC & 0xF) == 0`, so overworld backgrounds stay on the fast NEON tile path instead of falling into software-division mosaic loops. The remaining integer modulo operations (`%`) were replaced with `ApplyMosaicFloorU()`, a fixed-point reciprocal lookup. That removes every `__aeabi_*divmod` call from `gba_easy_draw`.

### Clock behavior

- `VITA_DEFAULT_ARM_CLOCK_MHZ` defaults to `222` in `sdl2.c`, a balanced power and performance profile. The userland call `scePowerSetArmClockFrequency(222)` runs at `pre_init`, `post_renderer`, `after_first_present` and foreground resume.
- Retail `ScePower` and kernel clock plugins such as PSVshell can clamp or override userland frequency requests. `perf.log` records `before` and `after` values for each `scePowerSetArmClockFrequency` call. With PSVshell disabled or set to `Default` / `222 MHz`, both modes run at `222 MHz`.

### Save location

Saves live at `ux0:data/pokeemerald/pokeemerald.sav`. The port also writes `ux0:data/pokeemerald/pokeemerald.cfg` and `ux0:data/pokeemerald/perf.log` there.

---

## Building locally

### 1. Prerequisites

- A legal Pokémon Emerald (USA, Europe) Rev 0 `.gba` ROM (`BPEE`, `16,777,216` bytes, SHA-1 `f3ae088181bf583e55daf962a92bb46f4f1d07b7`).
- A local `pokeemerald` source tree at `./pokeemerald` (gitignored).
  - Upstream public base: [`gradenGnostic/pokeemerald-multiplatform`](https://github.com/gradenGnostic/pokeemerald-multiplatform) (which provides the SDL2 multiplatform layout, M0 build targets, and retained upstream platform modules omitted from `port/` such as `bios.c`, `dma.c`, `dma.h`, `gba_fast_draw.c`, `gba_rgba.h`, `nostd.c`, `win32.c`, and `mods/`).
  - Baseline availability note: [port/patches/upstream-vita-hooks.patch](port/patches/upstream-vita-hooks.patch) diffs cleanly against local commit `15c5805` in this workspace's private pre-integrated `pokeemerald-multiplatform` + follower + pre-overlay Vita checkout. Because `15c5805` is a local integrated baseline and is **not** a commit on public `gradenGnostic/pokeemerald-multiplatform`, `scripts/apply_port.sh --apply-patch` will exit with an error on a fresh public clone unless you manually merge those hook hunks (`include/{battle_transition,decoration,overworld,platform,shop,sprite}.h` and `src/{battle_transition,decoration,event_object_movement,field_camera,field_door,field_effect_helpers,music_player,overworld,shop,sound_mixer,sprite,tileset_anims}.c`).
  - `scripts/apply_port.sh` cleans obsolete `src/platform/voxel/` and `src/platform/vita/` files from the target checkout before copying `port/`, so verified baselines compile without stale pre-overlay `.c` units.
- [VitaSDK](https://vitasdk.org/) (`arm-vita-eabi-gcc`, the CMake toolchain, `vita-mksfoex`, `vita-create-vpk`). Verified toolchain revisions: `vita-toolchain c527abce`, `vita-headers ebc8f4f7`, `newlib 64aa7aa3`, `pthread-embedded 11d2e572`.
- SDL2 `2.30.7` for PS Vita, patched with [port/vita/patches/sdl2-vita-audio-42060.patch](port/vita/patches/sdl2-vita-audio-42060.patch) through [port/vita/scripts/build_audio_optimized_sdl2.sh](port/vita/scripts/build_audio_optimized_sdl2.sh).
- [`vitaGL`](https://github.com/Rinnegatamante/vitaGL) (`LGPL-3.0-or-later` / `GPL-3.0`), compiled with fixed-function pipeline, ping-pong uniform, and draw speedhack flags (`-DHAVE_SHADER_CACHE -DSKIP_SPLASHSCREEN -DSTRICT_DRAW_COMPLIANCE -DUNIFORM_PINGPONG -DALIGNED_POOL -DDRAW_SPEEDHACK -DINDICES_SPEEDHACK -DSAMPLERS_SPEEDHACK -DGXM_STATE_CACHE ...`) pointed to by `VITAGL_ROOT`. (Because `-DDRAW_SPEEDHACK` is active, [voxel_renderer.c](port/src/platform/voxel/voxel_renderer.c) routes all non-VBO draws through a 6-frame `vglMemalign` GPU-mapped stream ring.)
- Host build tools: `gcc`/`g++`, `make`, `cmake`, `ninja`, `python3`, and native SDL2/OpenGL dev packages. A container via `VITA_M0_DOCKER_IMAGE` also works. These build the host tools and the `Makefile_pc` M0 intermediates.
- External runtime and package packs. None of these are distributed here; you supply or generate them locally:
  - `voxel/` (`buildings.bin`, `regions.bin`, `relief.bin`, `signposts.bin`, `trees.rgba5551`): required for 3D voxel mode (`VITA_ENABLE_VOXEL=ON`, default). Generate this pack against your own Pokémon Emerald workspace/ROM using the host generator scripts documented in [`ZallaxDev/emerald-3ds`](https://github.com/ZallaxDev/emerald-3ds) (`>= 0.5.0`, see upstream `ASSET_PIPELINE.md` and `3ds_port/scripts/gen_voxel_*.py`). Those upstream scripts are not bundled here and no specific upstream commit is pinned in this repository; if you do not build a `voxel/` pack, pass `-DVITA_ENABLE_VOXEL=OFF -DVITA_PACKAGE_VOXEL_ASSETS=OFF` to build the 2D Vita port instead. [port/tools/extract_voxel_layout_models.py](port/tools/extract_voxel_layout_models.py) supplements them for per-layout `VBL4` models.
  - `followers/` (`FMN1` follower sprite pack): optional. Pass `-DVITA_ENABLE_FOLLOWER=OFF` to disable it, or `-DVITA_PACKAGE_FOLLOWER_ASSETS=OFF` to load it from `ux0:data/pokeemerald/followers/` instead.
  - LiveArea PNG8 images (`pokeemerald/vita/sce_sys/*.png`): generate them with [port/vita/prepare_livearea_assets.sh](port/vita/prepare_livearea_assets.sh). All four images must be 8-bit indexed PNGs with no alpha or `tRNS` chunk. If ImageMagick is missing, the script falls back to pure-Python geometric PNG8s.

### 2. Verify your ROM

Put your legally obtained ROM in `./ROM/` (gitignored) and check its SHA-1:

```sh
python3 scripts/verify_rom.py "ROM/Pokemon - Emerald Version (USA, Europe).gba"
```

> `verify_rom.py` only checks ROM identity and integrity. It does not extract the `voxel/` or `followers/` packs.

### 3. Apply the port overlay and hook patch

This removes obsolete pre-overlay files under `src/platform/voxel/` and `src/platform/vita/`, syncs `port/` into your local `./pokeemerald` tree, and applies the hook patch:

```sh
bash scripts/apply_port.sh --apply-patch
```

### 4. Build host tools, M0 intermediates, and run host tests

CMake compiles C sources and expects the generated headers (`include/constants/map_groups.h`), map includes (`data/maps/*/header.inc`), MIDI assembly (`sound/songs/midi/*.s`), and packed graphics/sound assets produced by the host `Makefile_pc` build (or inside a container via `-DVITA_M0_DOCKER_IMAGE=...`):

```sh
make -C pokeemerald -f make_tools.mk tools
make -C pokeemerald -f Makefile_pc linux -j"$(nproc)"
bash scripts/apply_port.sh --run-tests
```

### 5. Prepare LiveArea assets

This generates the four required `PNG8` package images: `icon0.png`, `pic0.png`, `livearea/contents/bg.png` and `livearea/contents/startup.png`. If you have no custom images under `./assets/`, or ImageMagick isn't installed, plain geometric placeholders are generated instead.

```sh
bash pokeemerald/vita/prepare_livearea_assets.sh
```

Step 7 runs this automatically if any of the four PNGs is missing.

### 6. Build the audio-optimized SDL2 (once)

```sh
export VITASDK=/path/to/vitasdk
export PATH="$VITASDK/bin:$PATH"
SDL2_SOURCE=/path/to/SDL2-2.30.7 bash pokeemerald/vita/scripts/build_audio_optimized_sdl2.sh
```

### 7. Build and audit the VPK

```sh
export VITASDK=/path/to/vitasdk
export PATH="$VITASDK/bin:$PATH"
export VITAGL_ROOT=/path/to/vitagl-fixed-function
```

Pick one packaging mode. In both modes `CMakeLists.txt` assembles game data and text into `eboot.bin`, so neither VPK is free of game data. Keep every VPK you build on your own machine.

- **Option A, slim VPK.** Leaves `voxel/` and `followers/` out of `app0:`. The game loads them at runtime from `ux0:data/pokeemerald/voxel/` and `ux0:data/pokeemerald/followers/`.

```sh
VITA_PACKAGE_VOXEL_ASSETS=OFF VITA_PACKAGE_FOLLOWER_ASSETS=OFF \
  bash ./pokeemerald/vita/build_vita_ab.sh slim-vpk
```

- **Option B, self-contained local VPK.** Embeds your local `voxel/` and `followers/` packs inside `app0:`, which is handy for testing on your own device.

```sh
VOXEL_ASSETS_DIR=/path/to/local/voxel FOLLOWER_ASSETS_DIR=/path/to/local/followers \
  bash ./pokeemerald/vita/build_vita_ab.sh voxel-222mhz
```

Either command configures CMake in `Release` (`-O3`, `-mcpu=cortex-a9`, `-mfpu=neon`), compiles `eboot.bin`, audits the compiler optimization flags, symbols and packaged layout, runs `verify_livearea_vpk.py`, and writes the `.vpk` under `local-output/test-build-checkpoints/`.

### 8. Install on the Vita

1. Copy your locally built `.vpk` to `ux0:downloads/pokemon-emerald-voxel.vpk`.
2. If you built the slim VPK (Option A), copy your local `voxel/` pack (`buildings.bin`, `regions.bin`, `relief.bin`, `signposts.bin`, `trees.rgba5551`) to `ux0:data/pokeemerald/voxel/`, and the optional `followers/` pack to `ux0:data/pokeemerald/followers/`.
3. Open VitaShell and install the `.vpk`.
4. The game shows up as Pokemon Emerald Voxel (`PKEV00001`). Saves and config are in `ux0:data/pokeemerald/`.
5. Optionally, after a play session, copy `ux0:data/pokeemerald/perf.log` and `ux0:data/pokeemerald/vitagl_error.log` back to your PC to look at timings.

---

## Repository structure

```text
.
├── README.md                        # Overview, controls, 3D/perf notes, build steps
├── ARCHITECTURE.md                  # Deep developer architecture & pipeline reference
├── LICENSE                          # Scoped licenses (MIT for Vita runtime; CC BY-NC-SA 4.0 for voxel/)
├── NOTICE.md                        # Upstream provenance, attributions, and change notes
├── SECURITY_OR_ASSET_POLICY.md      # Data-free policy rules
├── .gitignore                       # Ignores ROMs, assets, upstream source, build artifacts
├── port/
│   ├── src/platform/                # SDL2 Vita backend, PPU (gba_easy_draw), CGB audio, Voxel renderer
│   ├── include/platform/            # Public platform, pacing, perf & snapshot headers
│   ├── vita/                        # CMake build, VitaSDK scripts, SDL2 patch, LiveArea XML
│   ├── tests/platform/              # Host parity, PPU perf, pacing, audio & controller test suite
│   ├── patches/                     # Minimal hook diff against upstream pokeemerald C/H files
│   ├── tools/                       # Voxel layout model extractor helper (VBL4)
│   └── docs/                        # Voxel geometry/cutaway visual notes
└── scripts/
    ├── apply_port.sh                # Syncs port/ into a local pokeemerald tree & runs host tests
    ├── preflight_data_free.py       # Verifies no ROMs/game data/builds are staged in Git
    └── verify_rom.py                # Verifies user-supplied Emerald (USA, Europe) Rev 0 ROM
```

Before pushing a commit, check that the repo is still asset-free:

```sh
python3 scripts/preflight_data_free.py
```

---

## Legal

This is an unofficial fan port, made for education and preservation.

- Pokémon, Pokémon Emerald, and all related characters, names, sprites, audio and trademarks belong to Nintendo, Creatures Inc. and GAME FREAK inc.
- The project is not affiliated with, endorsed by, sponsored by or supported by Nintendo, Creatures Inc. or GAME FREAK inc.
- No copyrighted ROMs, Nintendo or Game Freak game data, audio, graphics or compiled VPK binaries are hosted or distributed here. You must supply your own legally dumped Pokémon Emerald ROM and build locally.
- The repo has separate licenses for separate parts; there is no single MIT grant over everything. The original PS Vita runtime, PPU, audio, test and build code, along with the retained `pokeemerald-multiplatform` port modifications, is MIT. The 3D voxel subsystem under `port/src/platform/voxel/*` (ported from `ZallaxDev/emerald-3ds >= 0.5.0`, excluding the MIT PS Vita shim files listed in `LICENSE`) and [port/docs/VITA_VOXEL_VISUAL_LIMITATIONS.md](port/docs/VITA_VOXEL_VISUAL_LIMITATIONS.md) stay under CC BY-NC-SA 4.0. Dependencies linked during local builds keep their own licenses: `vitaGL` is `LGPL-3.0-or-later` / `GPL-3.0`, and `SDL2` is `zlib`. See [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).
- Nothing here grants rights over Nintendo IP, and nothing relicenses upstream `pret/pokeemerald` or `pokeemerald-multiplatform` sources. That includes the unmodified `GPL-2.0` / `MPL-2.0` `bios.c` and the context lines of the upstream hook patch in [port/patches/upstream-vita-hooks.patch](port/patches/upstream-vita-hooks.patch).

---

## Credits

- [pret/pokeemerald](https://github.com/pret/pokeemerald): the Pokémon Emerald disassembly and decompilation project.
- `pokeemerald-multiplatform` (`Kurausukun`, `NT_x86`, `Normmatt`, `camthesaxman` and contributors): the original PC, SDL2 and Android multiplatform port, including the `ModManager` and portable audio foundation.
- `ZallaxDev` (`emerald-3ds`, `poke-corpus`, `pokeheartgold`): the 3D voxel diorama architecture (`buildings.bin`, `relief.bin`, `regions.bin`, `signposts.bin`, `trees.rgba5551`) and the Gen-4 style follower sprite corpus.
- [VitaSDK](https://vitasdk.org/), [vitaGL](https://github.com/Rinnegatamante/vitaGL) and SDL2: the open-source Vita toolchain, OpenGL backend, and input/audio layer.
