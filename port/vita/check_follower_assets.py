#!/usr/bin/env python3
"""Validate a private HGSS follower asset pack without modifying or copying it."""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

TEXT_COUNT = 759
EXPECTED_MON_FILES = {
    *(f"{species:03d}.bin" for species in range(1, 387)),
    *(f"201_{letter:02d}.bin" for letter in range(1, 28)),
    *(f"{national:03d}_f.bin" for national in (3, 25, 154, 202, 208, 214)),
}


def validate_sprite(path: Path) -> str | None:
    try:
        data = path.read_bytes()
    except OSError as error:
        return str(error)

    if len(data) < 8 or data[:4] != b"FMN1":
        return "missing or invalid FMN1 header"
    width, height, frames = data[4:7]
    if (width, height) not in ((32, 32), (64, 64)):
        return f"unsupported dimensions {width}x{height}; expected 32x32 or 64x64"
    if frames != 8:
        return f"has {frames} frames; expected 8"
    minimum_size = 8 + (2 * 16 * 2) + frames * (width * height // 2)
    if len(data) < minimum_size:
        return f"is {len(data)} bytes; expected at least {minimum_size}"
    return None


def validate_text(path: Path) -> str | None:
    try:
        data = path.read_bytes()
    except OSError as error:
        return str(error)

    minimum_size = 8 + 4 * TEXT_COUNT
    if len(data) < minimum_size or data[:4] != b"FMT1":
        return "missing or invalid FMT1 header/table"
    if struct.unpack_from("<H", data, 4)[0] != TEXT_COUNT:
        return f"contains {struct.unpack_from('<H', data, 4)[0]} messages; expected {TEXT_COUNT}"
    if data[-1] != 0xFF:
        return "last byte is not the Emerald end-of-string marker (0xFF)"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path, help="external assets/followers directory")
    args = parser.parse_args()
    root = args.assets.expanduser().resolve()
    problems: list[str] = []

    if not root.is_dir():
        print(f"Follower assets are unavailable: directory does not exist: {root}", file=sys.stderr)
        return 1
    if not (root / "sources.txt").is_file():
        problems.append("missing sources.txt provenance file")

    mon_dir = root / "mon"
    missing_names: list[str] = []
    for filename in sorted(EXPECTED_MON_FILES):
        path = mon_dir / filename
        if not path.is_file():
            missing_names.append(filename)
            continue
        error = validate_sprite(path)
        if error:
            problems.append(f"mon/{filename}: {error}")
    if missing_names:
        problems.append(
            f"missing {len(missing_names)} of {len(EXPECTED_MON_FILES)} required species/form sprites: "
            + ", ".join(missing_names)
        )

    text_path = root / "text" / "en.bin"
    if not text_path.is_file():
        problems.append("missing text/en.bin (English follower message bank)")
    else:
        error = validate_text(text_path)
        if error:
            problems.append(f"text/en.bin: {error}")

    if problems:
        print(f"Follower asset validation failed for {root}:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        print(
            "Provide the pinned, locally available FMN1/FMT1 follower asset pack; "
            "the build does not download or embed assets from this source tree.",
            file=sys.stderr,
        )
        return 1

    print(f"Validated {len(EXPECTED_MON_FILES)} follower sprites and the English message bank in {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
