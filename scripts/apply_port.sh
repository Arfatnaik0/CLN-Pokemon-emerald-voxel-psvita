#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PORT_DIR="${REPO_ROOT}/port"
TARGET_DIR="${REPO_ROOT}/pokeemerald"
APPLY_PATCH=0
RUN_TESTS=0

usage() {
    cat <<'EOF'
Usage: scripts/apply_port.sh [--target DIR] [--apply-patch] [--run-tests]

Syncs the data-free PS Vita port source, build scripts, and platform tests from
`port/` into a local `pokeemerald` source tree.

Options:
  --target DIR    Local pokeemerald source root (default: ./pokeemerald)
  --apply-patch   Also attempt `git apply` of port/patches/upstream-vita-hooks.patch
  --run-tests     Run host platform unit/parity tests after syncing
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target)
            TARGET_DIR="$2"
            shift 2
            ;;
        --apply-patch)
            APPLY_PATCH=1
            shift
            ;;
        --run-tests)
            RUN_TESTS=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ ! -d "${TARGET_DIR}" ]]; then
    echo "ERROR: Target pokeemerald checkout not found: ${TARGET_DIR}" >&2
    exit 2
fi

# Clear obsolete voxel/vita overlay files from older baselines before syncing.
rm -rf "${TARGET_DIR}/src/platform/voxel" "${TARGET_DIR}/src/platform/vita"
mkdir -p "${TARGET_DIR}/src/platform/voxel" "${TARGET_DIR}/src/platform/vita" "${TARGET_DIR}/include/platform" "${TARGET_DIR}/vita" "${TARGET_DIR}/tests/platform" "${TARGET_DIR}/tools" "${TARGET_DIR}/docs"

cp -a "${PORT_DIR}/src/platform/." "${TARGET_DIR}/src/platform/"
cp -a "${PORT_DIR}/include/platform/." "${TARGET_DIR}/include/platform/"
cp -a "${PORT_DIR}/vita/." "${TARGET_DIR}/vita/"
cp -a "${PORT_DIR}/tests/platform/." "${TARGET_DIR}/tests/platform/"
cp "${PORT_DIR}/tools/extract_voxel_layout_models.py" "${TARGET_DIR}/tools/extract_voxel_layout_models.py"
cp "${PORT_DIR}/docs/VITA_VOXEL_VISUAL_LIMITATIONS.md" "${TARGET_DIR}/docs/VITA_VOXEL_VISUAL_LIMITATIONS.md"

echo "Synced port overlay into ${TARGET_DIR}"

if [[ "${APPLY_PATCH}" -eq 1 ]]; then
    PATCH_FILE="${PORT_DIR}/patches/upstream-vita-hooks.patch"
    if git -C "${TARGET_DIR}" apply --check --reverse "${PATCH_FILE}" 2>/dev/null; then
        echo "upstream-vita-hooks.patch is already applied cleanly in ${TARGET_DIR}"
    elif git -C "${TARGET_DIR}" apply --check "${PATCH_FILE}" 2>/dev/null; then
        git -C "${TARGET_DIR}" apply "${PATCH_FILE}"
        echo "Applied upstream-vita-hooks.patch"
    else
        echo "ERROR: upstream-vita-hooks.patch does not apply cleanly to ${TARGET_DIR}." >&2
        echo "This patch targets the integrated multiplatform/follower baseline (commit 15c5805)." >&2
        echo "Rebase or merge the hook hunks manually if targeting a different upstream revision." >&2
        exit 1
    fi
fi

if [[ "${RUN_TESTS}" -eq 1 ]]; then
    for s in \
        run_cgb_audio_phase_cache.sh \
        run_gba_draw_parity.sh \
        run_gba_easy_draw_perf.sh \
        run_mod_manager_init_test.sh \
        run_sdl2_frame_pacing_test.sh \
        run_vita_audio_queue_metrics_test.sh \
        run_vita_controller_state_test.sh \
        run_vita_perf_rolling_test.sh \
        run_vita_vblank_startup_test.sh
    do
        echo "=== ${s} ==="
        (cd "${TARGET_DIR}" && bash "tests/platform/${s}")
    done
    (cd "${TARGET_DIR}" && python3 tests/platform/follower_spawn_safety_test.py)
    (cd "${TARGET_DIR}" && python3 tests/platform/vita_vblank_startup_source_test.py)
fi
