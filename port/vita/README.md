# Native Vita 2D build

This target builds the original 2D game for PlayStation Vita with follower
Pokémon support. SDL2 handles GXM display presentation, audio and controller
input. The build compiles the game's C sources through the repository
preprocessor, assembles generated map, script and music data, then creates an
ELF, `eboot.bin` and a private local VPK.

## Requirements

- VitaSDK with its CMake toolchain and VPK tools.
- `python3` for `prepare_livearea_assets.sh`, which generates all four indexed `PNG8` LiveArea package images (converting custom `../assets/` artwork via ImageMagick `convert` when present, or generating clean geometric `PNG8` placeholders automatically when absent).
- SDL2 2.30.7 built for Vita with audio and joystick backends enabled. Install
  it in the VitaSDK prefix or set `VITA_SDL2_ROOT`
  to its install prefix. `SDL2_DIR` may point directly to `SDL2Config.cmake`'s
  directory.
- The local FMN1 follower pack. Set `FOLLOWER_ASSETS_DIR` to its directory;
  configure validates every expected sprite and the English text bank, then
  packages that external directory under `followers/` without copying it into
  the source tree. Use `VITA_ENABLE_FOLLOWER=OFF` to build without followers.
- The host tools and generated game assets from an M0 Linux host build. The
  build script compiles the small host tools and uses `scaninc` plus Make's
  question mode to fail if generated inputs are missing or stale.

## Display controls

Triangle toggles between filling the Vita screen (`vitaStretch=1`) and a
centered 3:2 viewport (`vitaStretch=0`), saved in
`ux0:data/pokeemerald/pokeemerald.cfg` (older configs default to fill).

## Build

```sh
export VITASDK=/path/to/vitasdk
export VITA_SDL2_ROOT=/path/to/vita-sdl2-prefix # omit if installed in VitaSDK
export FOLLOWER_ASSETS_DIR=/path/to/local/follower-pack
./vita/build.sh
```

The source checkout intentionally does not contain the follower pack or a VPK.
Build outputs and package contents remain local and ignored. If the sibling
LiveArea source art is unavailable, the script reuses the existing validated
package images instead of rewriting them.

The freshness check does not require host multilib packages. If generated
assets need rebuilding, set `VITA_M0_DOCKER_IMAGE` to an image with the M0
32-bit SDL2, SDL2_image, GL and GLU build dependencies. The script mounts this
checkout into `/workspace` and runs `make -f Makefile_pc linux` in that image
before configuring the Vita build. With no image configured, stale inputs fail
with a message explaining how to refresh them.

The build script rebuilds host tools, configures the Vita toolchain, and writes
the artifacts to `build/vita/`. Override the output directory with
`VITA_BUILD_DIR` and parallelism with `VITA_BUILD_JOBS`. Set
`VITA_M0_BUILD_JOBS` to control the container refresh job count.

The optional `pokeemerald_vita_livearea_diag.vpk` uses TITLE_ID `PKEV00002`
and carries the same LiveArea template and artwork under a new ID for
side-by-side comparison with the existing `PKEV00001` install. Build it with
`VITA_BUILD_LIVEAREA_DIAGNOSTIC=ON ./vita/build.sh`. Inspect its LiveArea card
without pressing Start: both packages run the same game and use the same save
path. The build validates the referenced assets in each VPK, including PNG
format and dimensions; this checks packaging only and does not prove how a
Vita renders the card.

The unstripped ELF and linker map are kept alongside `eboot.bin` and
`pokeemerald_vita.vpk` for symbolication. A successful cross-build establishes
compilation and packaging only; device boot, controls, audio and save/reload
still require a Vita smoke test.

Per-stage timing and `ux0:data/pokeemerald/perf.log` are enabled by default for
device profiling. To produce a comparison build without profiler counters,
spinlock accounting and periodic log writes, set `VITA_ENABLE_PERF=OFF`; this
does not change game timing or simulation behavior:

```sh
VITA_ENABLE_PERF=OFF VITA_BUILD_DIR=build/vita-no-perf ./vita/build.sh
```

The Vita renderer presents every serviced game tick by default. For a cadence
A/B comparison, turn off only per-tick presentation:

```sh
VITASDK=/path/to/vitasdk \
VITA_PRESENT_EACH_SERVICED_TICK=OFF \
VITA_BUILD_DIR=build/vita-present-baseline ./vita/build.sh
```

For the original streaming-texture and once-per-loop behavior, turn both
options off. CMake rejects per-tick presentation without texture synchronization:

```sh
VITA_PRESENT_EACH_SERVICED_TICK=OFF VITA_SYNC_FRAME_TEXTURE_UPDATES=OFF VITA_BUILD_DIR=build/vita-original-baseline ./vita/build.sh
```
