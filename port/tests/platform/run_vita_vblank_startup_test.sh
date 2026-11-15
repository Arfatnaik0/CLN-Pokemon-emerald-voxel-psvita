#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

${CC:-cc} -O2 -std=gnu99 -Wall -Wextra -Werror \
    -DPORTABLE -DNONMATCHING -DMODERN=1 -DUBFIX \
    -DPLATFORM_VITA -DPLATFORM_SDL2 -DRENDERER_EASY_DRAW \
    -iquote "$ROOT" -iquote "$ROOT/include" -iquote "$ROOT/gflib" \
    -ffunction-sections -fdata-sections \
    "$ROOT/src/gpu_regs.c" \
    "$ROOT/tests/platform/vita_vblank_startup_test.c" \
    -Wl,--gc-sections \
    -o "$TEMP_DIR/vita_vblank_startup_test"

"$TEMP_DIR/vita_vblank_startup_test"
python3 "$ROOT/tests/platform/vita_vblank_startup_source_test.py"
echo "Vita VBlank startup bootstrap passed."
