#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT HUP INT TERM

${CC:-cc} -O2 -std=gnu99 -Wall -Wextra -Werror \
    -I"$ROOT/include" \
    "$ROOT/tests/platform/vita_audio_queue_metrics_test.c" \
    -o "$TEMP_DIR/vita_audio_queue_metrics_test"

"$TEMP_DIR/vita_audio_queue_metrics_test"
echo "Vita audio queue gap metric passed."
