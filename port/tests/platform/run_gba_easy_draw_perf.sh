#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

${CC:-cc} -O2 -std=gnu99 -fno-builtin -fno-strict-aliasing \
    -iquote "$ROOT/include" -iquote "$ROOT/gflib" \
    -DPORTABLE -DPLATFORM_SDL2 -DMODERN=1 -DUBFIX -DEASY_DRAW_TEST_PARALLEL \
    "$ROOT/tests/platform/gba_easy_draw_perf.c" \
    "$ROOT/src/platform/gba_easy_draw_workers.c" -pthread \
    -o "$TEMP_DIR/gba_easy_draw_perf"

echo "Running EASY draw checks with the pthread worker backend"
"$TEMP_DIR/gba_easy_draw_perf"

SDL2_RUNTIME=${SDL2_RUNTIME:-/lib/x86_64-linux-gnu/libSDL2-2.0.so.0}
if [ -r "$SDL2_RUNTIME" ]; then
    # The production worker code links the platform's affinity shim. Keep the
    # host test on its no-op branch so it can exercise the SDL worker backend
    # without requiring Vita kernel stubs.
    ${CC:-cc} -O2 -std=gnu99 -Wall -Wextra -Werror \
        -c "$ROOT/src/platform/vita_thread_affinity.c" \
        -o "$TEMP_DIR/vita_thread_affinity_host.o"
    ${CC:-cc} -O2 -std=gnu99 -fno-builtin -fno-strict-aliasing \
        -iquote "$ROOT/include" -iquote "$ROOT/gflib" \
        -I"$ROOT/tests/platform/sdl2_abi" \
        -DPORTABLE -DPLATFORM_SDL2 -DMODERN=1 -DUBFIX -DEASY_DRAW_TEST_PARALLEL \
        -DPLATFORM_VITA -c "$ROOT/src/platform/gba_easy_draw_workers.c" \
        -o "$TEMP_DIR/gba_easy_draw_workers_sdl2.o"
    ${CC:-cc} -O2 -std=gnu99 -fno-builtin -fno-strict-aliasing \
        -iquote "$ROOT/include" -iquote "$ROOT/gflib" \
        -DPORTABLE -DPLATFORM_SDL2 -DMODERN=1 -DUBFIX -DEASY_DRAW_TEST_PARALLEL \
        "$ROOT/tests/platform/gba_easy_draw_perf.c" \
        "$TEMP_DIR/gba_easy_draw_workers_sdl2.o" \
        "$TEMP_DIR/vita_thread_affinity_host.o" -pthread "$SDL2_RUNTIME" \
        -o "$TEMP_DIR/gba_easy_draw_perf_sdl2"
    echo "Running EASY draw checks with the production SDL worker code path and native SDL2 runtime"
    "$TEMP_DIR/gba_easy_draw_perf_sdl2"
else
    echo "SKIP: SDL2 runtime not found at $SDL2_RUNTIME; production SDL worker branch was not exercised"
fi
