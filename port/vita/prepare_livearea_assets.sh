#!/usr/bin/env bash
set -euo pipefail

# Generate the four PlayStation Vita LiveArea 8-bit indexed PNGs (no tRNS chunk)
# expected by vita-mksfoex / vita-pack-vpk:
#   sce_sys/icon0.png                    128x128
#   sce_sys/pic0.png                     960x544
#   sce_sys/livearea/contents/bg.png     840x500
#   sce_sys/livearea/contents/startup.png 280x158
#
# If user-supplied sources exist under ../assets/ (outside any ROM/repo assets):
#   BG1.png, start-button.jpg, and optionally icon0.* / pic0.*
# and ImageMagick is installed, those sources are quantized to PNG8.
# Otherwise, clean non-copyrighted geometric PNG8 placeholders are generated
# directly with Python's standard library (no external tools required).

VITA_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
GAME_ROOT="$(cd -- "${VITA_DIR}/.." && pwd)"
ASSET_DIR="$(cd -- "${GAME_ROOT}/.." && pwd)/assets"
SYS_DIR="${VITA_DIR}/sce_sys"
CONTENTS_DIR="${SYS_DIR}/livearea/contents"

mkdir -p "${SYS_DIR}" "${CONTENTS_DIR}"

convert_with_magick() {
    local src="$1"
    local size="$2"
    local dst="$3"
    local im_bin=""

    if command -v magick >/dev/null 2>&1; then
        im_bin="magick"
    elif command -v convert >/dev/null 2>&1; then
        im_bin="convert"
    else
        return 1
    fi

    "${im_bin}" "${src}" \
        -alpha off \
        -resize "${size}^" \
        -gravity center \
        -extent "${size}" \
        -colors 256 \
        -strip \
        "PNG8:${dst}"
}

generate_fallback_png8() {
    local kind="$1"
    local width="$2"
    local height="$3"
    local dst="$4"

    python3 - "${kind}" "${width}" "${height}" "${dst}" <<'PYEOF'
import struct
import sys
import zlib

kind = sys.argv[1]
w = int(sys.argv[2])
h = int(sys.argv[3])
out_path = sys.argv[4]

# Palette (256 RGB entries; first 16 used)
palette = [
    (12, 24, 34),    # 0: deep teal-slate background
    (18, 42, 54),    # 1: subtle grid dark
    (24, 68, 76),    # 2: horizon emerald-teal
    (32, 110, 88),   # 3: emerald mid
    (48, 168, 112),  # 4: bright emerald
    (112, 224, 152), # 5: mint highlight
    (22, 52, 68),    # 6: isometric cube top
    (16, 38, 52),    # 7: isometric cube left
    (10, 26, 38),    # 8: isometric cube right
    (232, 244, 238), # 9: crisp ivory frame / border
    (72, 196, 136),  # 10: accent stripe
] + [(0, 0, 0)] * (256 - 11)

plte_bytes = bytes(c for rgb in palette for c in rgb)

rows = bytearray()
cx = w // 2
cy = h // 2
border = 4 if w >= 280 else 3

for y in range(h):
    rows.append(0)  # filter type 0 (None)
    row = bytearray(w)
    is_y_border = (y < border or y >= h - border)
    for x in range(w):
        if is_y_border or x < border or x >= w - border:
            row[x] = 9 if kind == "startup" else 4
            continue

        # Subtle architectural grid
        c = 1 if (x % 32 == 0 or y % 32 == 0) else 0
        if kind in ("bg", "pic0") and abs(y - int(h * 0.62)) < 2:
            c = 3

        # Isometric voxel diamond emblem in center
        dx = x - cx
        dy = y - cy
        radius = min(w, h) // (3 if kind == "icon" else 4)
        manhattan = abs(dx) + abs(dy * 2)
        if manhattan <= radius:
            if dy < -abs(dx) // 2:
                c = 5 if manhattan > radius - 4 else 4
            elif dx < 0:
                c = 3
            else:
                c = 2
        elif kind == "startup" and (abs(dx) < w // 3 and abs(dy) < h // 4):
            c = 6 if (x + y) % 16 < 8 else 7
            if abs(dy) <= 3:
                c = 10

        row[x] = c
    rows.extend(row)

def chunk(tag, data):
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )

ihdr = struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0)  # 8-bit indexed, no interlace
idat = zlib.compress(bytes(rows), 9)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"PLTE", plte_bytes) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")

with open(out_path, "wb") as f:
    f.write(png)
PYEOF
}

build_asset() {
    local kind="$1"
    local size="$2"
    local dst="$3"
    shift 3
    local w="${size%x*}"
    local h="${size#*x}"
    local src

    for src in "$@"; do
        if [ -f "${src}" ] && convert_with_magick "${src}" "${size}" "${dst}"; then
            echo "Generated ${dst} from $(basename -- "${src}") (${size} PNG8)"
            return 0
        fi
    done

    generate_fallback_png8 "${kind}" "${w}" "${h}" "${dst}"
    echo "Generated geometric fallback ${dst} (${size} PNG8)"
}

build_asset "icon" "128x128" "${SYS_DIR}/icon0.png" \
    "${ASSET_DIR}/icon0.png" "${ASSET_DIR}/icon0.jpg" "${ASSET_DIR}/BG1.png"
build_asset "pic0" "960x544" "${SYS_DIR}/pic0.png" \
    "${ASSET_DIR}/pic0.png" "${ASSET_DIR}/pic0.jpg" "${ASSET_DIR}/BG1.png"
build_asset "bg" "840x500" "${CONTENTS_DIR}/bg.png" \
    "${ASSET_DIR}/BG1.png" "${ASSET_DIR}/bg.png"
build_asset "startup" "280x158" "${CONTENTS_DIR}/startup.png" \
    "${ASSET_DIR}/start-button.jpg" "${ASSET_DIR}/start-button.png" "${ASSET_DIR}/startup.png"

echo "Prepared LiveArea PNG8 assets under ${SYS_DIR}"
