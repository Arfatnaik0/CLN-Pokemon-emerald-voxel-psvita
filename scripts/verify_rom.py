#!/usr/bin/env python3
"""Verify a user-supplied Pokemon Emerald (USA, Europe) .gba ROM locally."""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

EXPECTED_SHA1 = "f3ae088181bf583e55daf962a92bb46f4f1d07b7"
EXPECTED_TITLE = b"POKEMON EMER"
EXPECTED_GAME_CODE = b"BPEE"
EXPECTED_SIZE = 16 * 1024 * 1024


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path, help="Path to local Pokemon Emerald .gba ROM")
    args = parser.parse_args()

    rom_path: Path = args.rom
    if not rom_path.is_file():
        print(f"ERROR: ROM file not found: {rom_path}", file=sys.stderr)
        return 2

    data = rom_path.read_bytes()
    sha1 = hashlib.sha1(data).hexdigest()
    title = data[0xA0:0xAC].rstrip(b"\x00")
    game_code = data[0xAC:0xB0]
    revision = data[0xBC] if len(data) > 0xBC else -1

    print(f"File:      {rom_path}")
    print(f"Size:      {len(data)} bytes")
    print(f"Header:    title={title!r} code={game_code!r} rev={revision}")
    print(f"SHA-1:     {sha1}")

    ok = (
        len(data) == EXPECTED_SIZE
        and title == EXPECTED_TITLE
        and game_code == EXPECTED_GAME_CODE
        and revision == 0
        and sha1 == EXPECTED_SHA1
    )
    if not ok:
        print(
            f"ERROR: Expected Pokemon Emerald (USA, Europe) Rev 0 (BPEE, {EXPECTED_SIZE} bytes, SHA-1 {EXPECTED_SHA1}).",
            file=sys.stderr,
        )
        return 1

    print("OK: Verified Pokemon - Emerald Version (USA, Europe) (Rev 0 / BPEE).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
