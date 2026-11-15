#!/bin/sh
set -eu

if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != "--bench" ]; }; then
    echo "usage: $0 [--bench]" >&2
    exit 2
fi

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
cc=${CC:-cc}

common_flags="-std=gnu99 -O3 -DPORTABLE -DMODERN=1 -DPLATFORM_SDL2 -DUBFIX"
include_flags="-I$repo_root -I$repo_root/gflib -I$repo_root/sound -iquote $repo_root/include"

# Build the direct-division reference and the production cache from the same
# generator source, then compare real generated stereo float samples.
# Flags are intentionally split here so CC can name a single compiler binary.
$cc $common_flags $include_flags \
    -DCGB_AUDIO_DISABLE_PHASE_STEP_CACHE \
    "$repo_root/tests/platform/cgb_audio_phase_cache_test.c" \
    "$repo_root/src/platform/cgb_audio.c" -o "$tmp_dir/cgb_audio_reference"
$cc $common_flags $include_flags \
    "$repo_root/tests/platform/cgb_audio_phase_cache_test.c" \
    "$repo_root/src/platform/cgb_audio.c" -o "$tmp_dir/cgb_audio_cached"

"$tmp_dir/cgb_audio_reference" > "$tmp_dir/reference.raw"
"$tmp_dir/cgb_audio_cached" > "$tmp_dir/cached.raw"
cmp "$tmp_dir/reference.raw" "$tmp_dir/cached.raw"
bytes=$(wc -c < "$tmp_dir/reference.raw" | tr -d '[:space:]')
if [ "$bytes" != 67648 ]; then
    echo "unexpected audio sample output size: $bytes bytes" >&2
    exit 1
fi
echo "CGB phase cache byte parity passed ($bytes bytes; rates 42060, 42048, 44100, 32, 33 Hz, including same-register rate reset)."

if [ "$#" -eq 1 ]; then
    $cc $common_flags $include_flags \
        -DCGB_AUDIO_DISABLE_PHASE_STEP_CACHE \
        "$repo_root/tests/platform/cgb_audio_phase_cache_bench.c" \
        "$repo_root/src/platform/cgb_audio.c" -o "$tmp_dir/bench_reference"
    $cc $common_flags $include_flags \
        "$repo_root/tests/platform/cgb_audio_phase_cache_bench.c" \
        "$repo_root/src/platform/cgb_audio.c" -o "$tmp_dir/bench_cached"
    for workload in stable dynamic; do
        if [ "$workload" = dynamic ]; then
            set -- dynamic
        else
            set --
        fi
        echo "Host CGB generation benchmark (1200 x 701 samples; $workload registers):"
        for run in 1 2 3; do
            printf "  reference: "
            "$tmp_dir/bench_reference" "$@"
            printf "  cached:    "
            "$tmp_dir/bench_cached" "$@"
        done
    done
fi
