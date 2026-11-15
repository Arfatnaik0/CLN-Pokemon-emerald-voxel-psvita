#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

${CC:-cc} -O3 -std=gnu99 -fno-builtin -fno-strict-aliasing \
    -iquote "$ROOT/include" -iquote "$ROOT/gflib" \
    -DPORTABLE -DPLATFORM_SDL2 -DMODERN=1 -DUBFIX \
    "$ROOT/tests/platform/gba_draw_renderer_easy.c" \
    "$ROOT/tests/platform/gba_draw_renderer_fast.c" \
    "$ROOT/tests/platform/gba_draw_parity.c" \
    -o "$TEMP_DIR/gba_draw_parity"

"$TEMP_DIR/gba_draw_parity" "$@"
