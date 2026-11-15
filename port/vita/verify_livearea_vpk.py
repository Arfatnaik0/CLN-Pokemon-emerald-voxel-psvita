#!/usr/bin/env python3
"""Check that a VPK contains the LiveArea assets referenced by its template."""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
LIVEAREA_PREFIX = "sce_sys/livearea/contents/"


def fail(message: str) -> None:
    raise SystemExit(f"LiveArea package check failed: {message}")


def inspect_png(data: bytes, name: str, expected_size: tuple[int, int]) -> None:
    if not data.startswith(PNG_SIGNATURE) or len(data) < 33:
        fail(f"{name} is not a complete PNG")

    width, height, bit_depth, color_type = struct.unpack(">IIBB", data[16:26])
    if (width, height) != expected_size:
        fail(f"{name} is {width}x{height}; expected {expected_size[0]}x{expected_size[1]}")
    if bit_depth != 8 or color_type != 3:
        fail(f"{name} must be 8-bit indexed PNG; found bit depth {bit_depth}, color type {color_type}")

    chunk_types: set[bytes] = set()
    offset = 8
    while offset + 12 <= len(data):
        chunk_length = struct.unpack(">I", data[offset : offset + 4])[0]
        chunk_type = data[offset + 4 : offset + 8]
        chunk_end = offset + 12 + chunk_length
        if chunk_end > len(data):
            fail(f"{name} has a truncated {chunk_type!r} chunk")
        chunk_types.add(chunk_type)
        offset = chunk_end
        if chunk_type == b"IEND":
            break

    if b"IEND" not in chunk_types:
        fail(f"{name} is missing its PNG IEND chunk")
    if b"tRNS" in chunk_types:
        fail(f"{name} contains palette transparency")


def verify_vpk(vpk_path: Path, expected_title_id: str) -> None:
    if not vpk_path.is_file():
        fail(f"VPK does not exist: {vpk_path}")

    try:
        with zipfile.ZipFile(vpk_path) as archive:
            bad_member = archive.testzip()
            if bad_member:
                fail(f"VPK CRC check failed for {bad_member}")

            members = set(archive.namelist())
            required = {
                "eboot.bin",
                "sce_sys/param.sfo",
                "sce_sys/icon0.png",
                "sce_sys/pic0.png",
                f"{LIVEAREA_PREFIX}template.xml",
            }
            missing = sorted(required - members)
            if missing:
                fail(f"VPK is missing required entries: {', '.join(missing)}")

            param_sfo = archive.read("sce_sys/param.sfo")
            if expected_title_id.encode("ascii") not in param_sfo:
                fail(f"param.sfo does not contain TITLE_ID {expected_title_id}")

            template = ET.fromstring(archive.read(f"{LIVEAREA_PREFIX}template.xml"))
            if template.tag != "livearea" or template.get("style") != "a1":
                fail("template.xml does not use the supported a1 LiveArea root")

            background_name = (template.findtext("./livearea-background/image") or "").strip()
            startup_name = (template.findtext("./gate/startup-image") or "").strip()
            if not background_name or Path(background_name).name != background_name:
                fail("template.xml background reference must be a local filename")
            if not startup_name or Path(startup_name).name != startup_name:
                fail("template.xml startup reference must be a local filename")

            background_path = f"{LIVEAREA_PREFIX}{background_name}"
            startup_path = f"{LIVEAREA_PREFIX}{startup_name}"
            if background_path not in members:
                fail(f"template background is not in the VPK: {background_path}")
            if startup_path not in members:
                fail(f"template startup image is not in the VPK: {startup_path}")

            inspect_png(archive.read(background_path), background_path, (840, 500))
            inspect_png(archive.read(startup_path), startup_path, (280, 158))
            digest = hashlib.sha256(vpk_path.read_bytes()).hexdigest()
    except (OSError, zipfile.BadZipFile, ET.ParseError, KeyError) as error:
        fail(f"cannot read {vpk_path}: {error}")

    print(f"LiveArea VPK verified: {vpk_path.name} TITLE_ID={expected_title_id} sha256={digest}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("vpk", type=Path)
    parser.add_argument("title_id")
    args = parser.parse_args()
    verify_vpk(args.vpk, args.title_id)


if __name__ == "__main__":
    try:
        main()
    except (UnicodeError, ValueError, struct.error) as error:
        fail(str(error))
