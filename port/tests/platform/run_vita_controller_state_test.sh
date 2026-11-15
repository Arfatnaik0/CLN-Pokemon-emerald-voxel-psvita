#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

${CC:-cc} -std=gnu99 -Wall -Wextra -Werror -I"$ROOT/include" \
    "$ROOT/tests/platform/vita_controller_state_test.c" \
    -o "$TEMP_DIR/vita_controller_state_test"
"$TEMP_DIR/vita_controller_state_test"
echo "Vita controller shortcut disconnect reset passed."
