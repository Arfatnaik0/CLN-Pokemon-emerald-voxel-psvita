#!/usr/bin/env bash
set -euo pipefail

VITA_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
GAME_ROOT="$(cd -- "${VITA_DIR}/.." && pwd)"

if [[ -z "${VITASDK:-}" || ! -f "${VITASDK}/share/vita.toolchain.cmake" ]]; then
    printf 'Set VITASDK to a VitaSDK install containing share/vita.toolchain.cmake.\n' >&2
    exit 2
fi

ASSET_DIR="$(cd -- "${GAME_ROOT}/.." && pwd)/assets"
if [[ -f "${ASSET_DIR}/BG1.png" && -f "${ASSET_DIR}/start-button.jpg" ]]; then
    "${VITA_DIR}/prepare_livearea_assets.sh"
elif [[ -f "${VITA_DIR}/sce_sys/livearea/contents/bg.png" \
     && -f "${VITA_DIR}/sce_sys/livearea/contents/startup.png" ]]; then
    printf 'Using the existing validated LiveArea package images; source artwork is unavailable.\n'
else
    "${VITA_DIR}/prepare_livearea_assets.sh"
fi

BUILD_DIR="${VITA_BUILD_DIR:-${GAME_ROOT}/build/vita}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
BUILD_JOBS="${VITA_BUILD_JOBS:-$(nproc)}"

cmake_args=(
    "-DCMAKE_TOOLCHAIN_FILE=${VITASDK}/share/vita.toolchain.cmake"
    "-DVITASDK=${VITASDK}"
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
)
if [[ -n "${VITA_SDL2_ROOT:-}" ]]; then
    cmake_args+=("-DVITA_SDL2_ROOT=${VITA_SDL2_ROOT}")
fi
if [[ -n "${SDL2_DIR:-}" ]]; then
    cmake_args+=("-DSDL2_DIR=${SDL2_DIR}")
fi
cmake_args+=("-DVITA_ENABLE_FOLLOWER=${VITA_ENABLE_FOLLOWER:-ON}")
if [[ -n "${FOLLOWER_ASSETS_DIR:-}" ]]; then
    cmake_args+=("-DFOLLOWER_ASSETS_DIR=${FOLLOWER_ASSETS_DIR}")
fi
if [[ -n "${VITA_M0_DOCKER_IMAGE:-}" ]]; then
    cmake_args+=("-DVITA_M0_DOCKER_IMAGE=${VITA_M0_DOCKER_IMAGE}")
fi
cmake_args+=("-DVITA_M0_BUILD_JOBS=${VITA_M0_BUILD_JOBS:-${BUILD_JOBS}}")
cmake_args+=("-DVITA_ENABLE_PERF=${VITA_ENABLE_PERF:-ON}")
cmake_args+=("-DVITA_PRESENT_EACH_SERVICED_TICK=${VITA_PRESENT_EACH_SERVICED_TICK:-ON}")
cmake_args+=("-DVITA_BUILD_LIVEAREA_DIAGNOSTIC=${VITA_BUILD_LIVEAREA_DIAGNOSTIC:-OFF}")
if [[ -n "${CMAKE_GENERATOR:-}" ]]; then
    cmake_args+=(-G "${CMAKE_GENERATOR}")
elif command -v ninja >/dev/null 2>&1; then
    cmake_args+=(-G Ninja)
fi

make -C "${GAME_ROOT}" -f make_tools.mk tools
cmake -S "${VITA_DIR}" -B "${BUILD_DIR}" "${cmake_args[@]}"
cmake --build "${BUILD_DIR}" --parallel "${BUILD_JOBS}"

ELF_FILE="${BUILD_DIR}/pokeemerald_vita"
VPK_FILE="${BUILD_DIR}/pokeemerald_vita.vpk"
LIVEAREA_DIAG_VPK_FILE="${BUILD_DIR}/pokeemerald_vita_livearea_diag.vpk"
ELF_SHA256="$(sha256sum "${ELF_FILE}" | cut -d ' ' -f 1)"
VPK_SHA256="$(sha256sum "${VPK_FILE}" | cut -d ' ' -f 1)"
python3 "${VITA_DIR}/verify_livearea_vpk.py" "${VPK_FILE}" PKEV00001
printf 'Build ID: PKEV00001-01.00-%s\n' "${ELF_SHA256:0:12}"
printf 'ELF SHA-256: %s\n' "${ELF_SHA256}"
printf 'VPK SHA-256: %s\n' "${VPK_SHA256}"
if [[ "${VITA_BUILD_LIVEAREA_DIAGNOSTIC:-OFF}" == "ON" ]]; then
    LIVEAREA_DIAG_SHA256="$(sha256sum "${LIVEAREA_DIAG_VPK_FILE}" | cut -d ' ' -f 1)"
    python3 "${VITA_DIR}/verify_livearea_vpk.py" "${LIVEAREA_DIAG_VPK_FILE}" PKEV00002
    printf 'LiveArea diagnostic VPK: %s\n' "${LIVEAREA_DIAG_VPK_FILE}"
    printf 'LiveArea diagnostic VPK SHA-256: %s\n' "${LIVEAREA_DIAG_SHA256}"
fi
