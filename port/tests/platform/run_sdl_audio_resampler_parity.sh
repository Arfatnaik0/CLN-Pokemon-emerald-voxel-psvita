#!/bin/sh
# Compare optimized and original SDL 2.30.7 audio streams on the host.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
sdl_source=${SDL2_SOURCE:?Set SDL2_SOURCE to the clean SDL2 2.30.7 source checkout}
expected_c=90bbc5f43efe3e61ab951a76b3186780fc22615c80cee2271bdd7ec3521073f5
expected_filter=5563c9cda44193fcd5f5677f817d014d54352a2db17ea94d276bcbea9525f967
printf '%s  %s\n%s  %s\n' "$expected_c" "$sdl_source/src/audio/SDL_audiocvt.c" "$expected_filter" "$sdl_source/src/audio/SDL_audio_resampler_filter.h" | sha256sum --check --status || { echo 'Unsupported SDL2 source or modified resampler.' >&2; exit 1; }
work=$(mktemp -d "${TMPDIR:-/tmp}/pokevita-sdl2-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/src"
tar -C "$sdl_source" --exclude=.git -cf - . | tar -C "$work/src" -xf -
patch -d "$work/src" -p1 < "$root/vita/patches/sdl2-vita-audio-42060.patch"
for flavor in base opt; do
  flags=''
  [ "$flavor" = base ] || flags='-DPOKEMERALD_VITA_RESAMPLE=1'
  cmake -S "$work/src" -B "$work/$flavor" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_VIDEO=OFF -DSDL_LIBSAMPLERATE=OFF \
    -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_SENSOR=OFF -DSDL_HIDAPI=OFF \
    -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DCMAKE_C_FLAGS="$flags" >"$work/cmake-$flavor.log"
  cmake --build "$work/$flavor" --parallel "${BUILD_JOBS:-4}" >"$work/build-$flavor.log"
  for test in stream thread; do
    cc -O3 -pthread -std=c11 -D_POSIX_C_SOURCE=200809L -I"$work/src/include" \
      "$root/tests/platform/sdl_audio_resampler_${test}_test.c" \
      -L"$work/$flavor" -Wl,-rpath,"$work/$flavor" -lSDL2-2.0 -o "$work/$test-$flavor"
  done
  "$work/stream-$flavor" "$work/audio-$flavor.bin"
  "$work/thread-$flavor" > "$work/thread-$flavor.txt"
done
cmp "$work/audio-base.bin" "$work/audio-opt.bin"
cmp "$work/thread-base.txt" "$work/thread-opt.txt"
echo 'SDL2 stream and concurrent resampler outputs match byte-for-byte.'
