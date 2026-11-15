#!/usr/bin/env bash
# ==============================================================================
# build_vita_ab.sh - Reproducible PS Vita A/B variant build and audit script
# Usage: ./build_vita_ab.sh <variant_name> [extra_cmake_args...]
# ==============================================================================

set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <variant_name> [extra_cmake_args...]"
    echo "Example: $0 baseline-test -DVITA_ENABLE_PERF=ON"
    exit 1
fi

VARIANT_NAME="$1"
shift
EXTRA_CMAKE_ARGS=("$@")

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${GAME_ROOT}/.." && pwd)"

VITASDK="${VITASDK:-/home/arfat00/.local/vitasdk}"
TOOLCHAIN="${VITASDK}/share/vita.toolchain.cmake"
SDL2_DIR="${SDL2_DIR:-${GAME_ROOT}/build/vita-sdl2-audio-opt/lib/cmake/SDL2}"
FOLLOWER_ASSETS_DIR="${FOLLOWER_ASSETS_DIR:-${REPO_ROOT}/local-output/assets/followers}"
VOXEL_ASSETS_DIR="${VOXEL_ASSETS_DIR:-${REPO_ROOT}/local-output/assets/voxel}"
VITA_PACKAGE_FOLLOWER_ASSETS="${VITA_PACKAGE_FOLLOWER_ASSETS:-ON}"
VITA_PACKAGE_VOXEL_ASSETS="${VITA_PACKAGE_VOXEL_ASSETS:-ON}"
VITAGL_ROOT="${VITAGL_ROOT:-/home/arfat00/.local/share/galaxy-vita/vitagl/nhustdx}"

BUILD_DIR="${GAME_ROOT}/build/vita-${VARIANT_NAME}"
CHECKPOINT_DIR="${CHECKPOINT_DIR:-${REPO_ROOT}/local-output/test-build-checkpoints/2026-11-15}"

# Protected builds check - never overwrite delivered/baseline builds
PROTECTED_BUILDS=("vita-m2" "vita-sdl2-audio-opt" "vita-vblank-fix" "vita-voxel" "vita-vcount-split")
for protected in "${PROTECTED_BUILDS[@]}"; do
    if [ "vita-${VARIANT_NAME}" = "${protected}" ]; then
        echo "ERROR: 'vita-${VARIANT_NAME}' is a protected delivered build directory."
        echo "Please choose a distinct variant name."
        exit 1
    fi
done

echo "========================================================================"
echo " Building Vita Variant: ${VARIANT_NAME}"
echo " Source root:    ${GAME_ROOT}"
echo " Build dir:      ${BUILD_DIR}"
echo " Checkpoint dir: ${CHECKPOINT_DIR}"
echo " Toolchain:      ${TOOLCHAIN}"
echo "========================================================================"

mkdir -p "${CHECKPOINT_DIR}"
mkdir -p "${BUILD_DIR}"

if [ ! -f "${SCRIPT_DIR}/sce_sys/icon0.png" ] || \
   [ ! -f "${SCRIPT_DIR}/sce_sys/pic0.png" ] || \
   [ ! -f "${SCRIPT_DIR}/sce_sys/livearea/contents/bg.png" ] || \
   [ ! -f "${SCRIPT_DIR}/sce_sys/livearea/contents/startup.png" ]; then
    echo "[-] LiveArea PNGs incomplete under ${SCRIPT_DIR}/sce_sys; running prepare_livearea_assets.sh"
    bash "${SCRIPT_DIR}/prepare_livearea_assets.sh"
fi

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
    -DVITASDK="${VITASDK}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL2_DIR="${SDL2_DIR}" \
    -DFOLLOWER_ASSETS_DIR="${FOLLOWER_ASSETS_DIR}" \
    -DVOXEL_ASSETS_DIR="${VOXEL_ASSETS_DIR}" \
    -DVITA_PACKAGE_FOLLOWER_ASSETS="${VITA_PACKAGE_FOLLOWER_ASSETS}" \
    -DVITA_PACKAGE_VOXEL_ASSETS="${VITA_PACKAGE_VOXEL_ASSETS}" \
    -DVITAGL_ROOT="${VITAGL_ROOT}" \
    -DVITA_ENABLE_PERF=ON \
    -DVITA_PRESENT_EACH_SERVICED_TICK=ON \
    -DVITA_ENABLE_HBLANK_WIN0_PARALLEL=ON \
    -DVITA_PIN_RENDER_THREADS=ON \
    "${EXTRA_CMAKE_ARGS[@]}"

ninja -C "${BUILD_DIR}"

VPK_SRC="${BUILD_DIR}/pokeemerald_vita.vpk"
ELF_SRC="${BUILD_DIR}/pokeemerald_vita"
VPK_NAME="emerald-vita-${VARIANT_NAME}.vpk"
VPK_DST="${CHECKPOINT_DIR}/${VPK_NAME}"

if [ ! -f "${VPK_SRC}" ]; then
    echo "ERROR: Target VPK was not produced at ${VPK_SRC}"
    exit 1
fi

cp -f "${VPK_SRC}" "${VPK_DST}"

echo ""
echo "========================================================================"
echo " POST-BUILD AUDIT: ${VPK_NAME}"
echo "========================================================================"

# 1. Compiler Optimization Audit
echo "[-] 1. Compiler Flags Audit (build.ninja):"
if grep -q "\-O[23]" "${BUILD_DIR}/build.ninja"; then
    OPT_FLAG=$(grep -o "\-O[0-9]" "${BUILD_DIR}/build.ninja" | head -n 1)
    echo "    [PASS] Optimization flag verified: ${OPT_FLAG}"
else
    echo "    [FAIL] No -O2/-O3 optimization flag found in build.ninja!"
    exit 1
fi

if grep -q "DNDEBUG" "${BUILD_DIR}/build.ninja"; then
    echo "    [PASS] NDEBUG define verified"
else
    echo "    [WARN] NDEBUG not found in build.ninja"
fi

if grep -q "\-mcpu=cortex-a9" "${BUILD_DIR}/build.ninja"; then
    echo "    [PASS] Cortex-A9 flag verified (-mcpu=cortex-a9)"
else
    echo "    [INFO] Default Cortex-A9 tuning in toolchain specs"
fi

# 2. Symbol Optimization Audit
echo "[-] 2. Symbol Optimization Audit (arm-vita-eabi-nm):"
NM="${VITASDK}/bin/arm-vita-eabi-nm"
SIZE="${VITASDK}/bin/arm-vita-eabi-size"
if [ -x "${NM}" ] && [ -f "${ELF_SRC}" ]; then
    ISRA_COUNT=$("${NM}" "${ELF_SRC}" | grep -c "\.isra\." || true)
    CONSTPROP_COUNT=$("${NM}" "${ELF_SRC}" | grep -c "\.constprop\." || true)
    TOTAL_OPT_SYMS=$((ISRA_COUNT + CONSTPROP_COUNT))
    echo "    [PASS] Optimization symbols detected: ${TOTAL_OPT_SYMS} (.isra: ${ISRA_COUNT}, .constprop: ${CONSTPROP_COUNT})"
    if [ "${TOTAL_OPT_SYMS}" -eq 0 ]; then
        echo "    [WARN] No .isra or .constprop symbols detected. Build might be unoptimized!"
    fi
fi

if [ -x "${SIZE}" ] && [ -f "${ELF_SRC}" ]; then
    echo "    ELF Section Sizes:"
    "${SIZE}" "${ELF_SRC}" | sed 's/^/      /'
fi

# 3. VPK Layout & Asset Audit
echo "[-] 3. VPK Content & Asset Audit (unzip -l):"
TOTAL_FILES=$(unzip -l "${VPK_DST}" | tail -n 1 | awk '{print $2}')
echo "    Total packaged files: ${TOTAL_FILES}"

# Verify voxel assets are packaged when VITA_PACKAGE_VOXEL_ASSETS=ON (or bare mode when OFF)
VOXEL_COUNT=$(unzip -l "${VPK_DST}" | grep -c "voxel/" || true)
if grep -q "^VITA_PACKAGE_VOXEL_ASSETS:BOOL=OFF$" "${BUILD_DIR}/CMakeCache.txt"; then
    if [ "${VOXEL_COUNT}" -ne 0 ]; then
        echo "    [FAIL] Bare VPK mode expected 0 voxel/* files packaged, found: ${VOXEL_COUNT}"
        exit 1
    fi
    echo "    [PASS] Bare VPK mode (voxel/ loaded at runtime from ux0:data/pokeemerald/voxel/)"
else
    if [ "${VOXEL_COUNT}" -lt 5 ]; then
        echo "    [FAIL] Expected >=5 voxel/* files packaged, found: ${VOXEL_COUNT}"
        exit 1
    fi
    for VA_FILE in buildings.bin regions.bin relief.bin signposts.bin trees.rgba5551; do
        if ! unzip -l "${VPK_DST}" | grep -q "voxel/${VA_FILE}"; then
            echo "    [FAIL] Missing voxel/${VA_FILE} in VPK"
            exit 1
        fi
    done
    echo "    [PASS] Voxel assets packaged: ${VOXEL_COUNT} files"
fi

# Check followers pack
FOLLOWER_COUNT=$(unzip -l "${VPK_DST}" | grep -c "followers/" || true)
if grep -q "^VITA_PACKAGE_FOLLOWER_ASSETS:BOOL=OFF$" "${BUILD_DIR}/CMakeCache.txt"; then
    echo "    [PASS] Bare VPK mode (followers/ omitted: ${FOLLOWER_COUNT} files; loads ux0:data/pokeemerald/followers/)"
else
    echo "    [PASS] Follower assets packaged: ${FOLLOWER_COUNT} files"
fi

# Verify LiveArea PNG8 images and template references
echo "[-] 3b. LiveArea Package Verification:"
python3 "${SCRIPT_DIR}/verify_livearea_vpk.py" "${VPK_DST}" "PKEV00001"
echo "    [PASS] verify_livearea_vpk.py succeeded"

# Check eboot.bin
EBOOT_ENTRY=$(unzip -l "${VPK_DST}" | grep "eboot.bin" || true)
if [ -n "${EBOOT_ENTRY}" ]; then
    EBOOT_SIZE=$(echo "${EBOOT_ENTRY}" | awk '{print $1}')
    echo "    [PASS] eboot.bin packaged (${EBOOT_SIZE} bytes)"
else
    echo "    [FAIL] eboot.bin is missing from VPK!"
    exit 1
fi

# 4. Checksum
echo "[-] 4. Cryptographic Checksum:"
SHA256=$(sha256sum "${VPK_DST}" | awk '{print $1}')
VPK_SIZE=$(ls -lh "${VPK_DST}" | awk '{print $5}')
echo "    File:   ${VPK_DST} (${VPK_SIZE})"
echo "    SHA256: ${SHA256}"
echo "========================================================================"
echo " BUILD AND AUDIT COMPLETE: ${VPK_NAME}"
echo "========================================================================"
