#!/bin/sh
# Build a private SDL2 2.30.7 Vita package with the 42060->44100 stereo
# resampler patch. The upstream zlib license remains in the copied source.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
game_root=$(CDPATH= cd -- "$script_dir/../.." && pwd)
sdl_source=${SDL2_SOURCE:-${1:-}}
sdl_source=${sdl_source:?Set SDL2_SOURCE (or pass path as first argument) to the clean SDL2 2.30.7 source checkout}
vitasdk=${VITASDK:?Set VITASDK to the VitaSDK installation}
prefix=${SDL2_OPT_PREFIX:-"$game_root/build/vita-sdl2-audio-opt"}
expected_c=90bbc5f43efe3e61ab951a76b3186780fc22615c80cee2271bdd7ec3521073f5
expected_filter=5563c9cda44193fcd5f5677f817d014d54352a2db17ea94d276bcbea9525f967
printf '%s  %s\n%s  %s\n' "$expected_c" "$sdl_source/src/audio/SDL_audiocvt.c" "$expected_filter" "$sdl_source/src/audio/SDL_audio_resampler_filter.h" | sha256sum --check --status || { echo 'Unsupported SDL2 source or modified resampler.' >&2; exit 1; }
work=$(mktemp -d "${TMPDIR:-/tmp}/pokevita-sdl2-build.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/src"
tar -C "$sdl_source" --exclude=.git -cf - . | tar -C "$work/src" -xf -
patch -d "$work/src" -p1 < "$script_dir/../patches/sdl2-vita-audio-42060.patch"
cmake -S "$work/src" -B "$work/build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$vitasdk/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_LIBSAMPLERATE=OFF \
  -DCMAKE_C_FLAGS='-DPOKEMERALD_VITA_RESAMPLE=1 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -marm' \
  -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build "$work/build" --parallel "${BUILD_JOBS:-4}"
cmake --install "$work/build"
"$vitasdk/bin/arm-vita-eabi-nm" -a "$prefix/lib/libSDL2.a" | grep -q 'PokeVitaResampleStereo' || { echo 'Optimized kernel missing from library.' >&2; exit 1; }
printf 'Optimized SDL2_DIR: %s/lib/cmake/SDL2\n' "$prefix"
sha256sum "$prefix/lib/libSDL2.a"
