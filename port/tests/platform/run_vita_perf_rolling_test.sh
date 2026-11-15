#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

${CC:-cc} -O2 -std=gnu99 -Wall -Wextra -Werror \
    -iquote "$ROOT/include" \
    "$ROOT/tests/platform/vita_perf_rolling_test.c" \
    -o "$TEMP_DIR/vita_perf_rolling_test"

"$TEMP_DIR/vita_perf_rolling_test"
echo "Vita rolling frame metrics passed."
