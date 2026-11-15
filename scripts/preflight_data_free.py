#!/usr/bin/env python3
"""Check Git candidate paths against the data-free repository policy.

This script inspects path names only. It never opens candidate file contents.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN_DIRS = {
    "pokeemerald",
    "pokeemerald-multiplatform",
    "upstream",
    "source-snapshot",
    "source-snapshots",
    "rom",
    "roms",
    "rom_dump",
    "rom-dump",
    "rom_dumps",
    "rom-dumps",
    "dumps",
    "dump",
    "extracted_assets",
    "extracted-assets",
    "extracted_data",
    "extracted-data",
    "game_assets",
    "game-assets",
    "game_data",
    "game-data",
    "rom_assets",
    "rom-assets",
    "rom_data",
    "rom-data",
    "assets_from_dump",
    "assets-from-dump",
    "screenshots",
    "screen_shots",
    "photos",
    "captures",
    "device-captures",
    "device_captures",
    "saves",
    "vita-saves",
    "vita_saves",
    "logs",
    "crashdumps",
    "crash-dumps",
    "user-data",
    "user_data",
    "device-data",
    "device_data",
    "private-assets",
    "user-assets",
    "generated",
    "generated-assets",
    "port-data",
    "local-output",
    "user-output",
}

FORBIDDEN_SUFFIXES = {
    ".gba", ".gbc", ".gb", ".rom", ".iso", ".nds", ".nes", ".sfc", ".smc",
    ".n64", ".z64", ".v64", ".3ds", ".cia", ".img", ".cso", ".pbp", ".pkg",
    ".sav", ".srm", ".dsv", ".gci",
    ".4bpp", ".1bpp", ".gbapal", ".sdat", ".lz", ".lz77", ".raw",
    ".bin", ".o", ".a", ".so", ".dll", ".dylib", ".exe", ".out",
    ".elf", ".velf", ".self", ".sprx", ".skprx", ".suprx", ".vpk", ".pkg", ".map",
    ".dmp", ".psp2dmp", ".core", ".log",
    ".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", ".tif", ".tiff",
    ".heic", ".avif", ".ico", ".icns", ".svg",
    ".wav", ".mp3", ".ogg", ".flac", ".m4a", ".aac", ".opus", ".mid", ".midi",
    ".mp4", ".mov", ".avi", ".mkv", ".webm",
    ".obj", ".mtl", ".fbx", ".blend", ".gltf", ".glb", ".dds", ".ktx",
    ".ttf", ".otf", ".woff", ".woff2",
    ".zip", ".7z", ".rar", ".tar", ".tgz", ".gz", ".bz2", ".xz",
}

FORBIDDEN_NAMES = {
    ".env",
    "pokeemerald.cfg",
    "pokeemerald.sav",
    "vita.cfg",
    "param.sfo",
}
CORE_DUMP_BASENAME = re.compile(r"core(?:\.\d+)?", re.IGNORECASE)

# Add an exact lowercase path only after verifying that the resource is original
# and safe to distribute. These paths bypass only the media-extension check.
REVIEWED_SAFE_MEDIA_PATHS: set[str] = set()
REVIEWABLE_MEDIA_SUFFIXES = {
    ".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", ".tif", ".tiff",
    ".heic", ".avif", ".ico", ".icns", ".svg", ".wav", ".mp3", ".ogg",
    ".flac", ".m4a", ".aac", ".opus", ".mid", ".midi", ".mp4", ".mov",
    ".avi", ".mkv", ".webm", ".obj", ".mtl", ".fbx", ".blend", ".gltf",
    ".glb", ".dds", ".ktx", ".ttf", ".otf", ".woff", ".woff2",
}


def main() -> int:
    result = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        detail = result.stderr.decode("utf-8", "replace").strip()
        print(f"Cannot list Git candidates: {detail or 'git ls-files failed'}", file=sys.stderr)
        return 2

    candidates = sorted({entry for entry in result.stdout.split(b"\0") if entry})
    findings: list[str] = []
    for entry in candidates:
        relative = entry.decode("utf-8", "surrogateescape")
        path = PurePosixPath(relative)
        parts = [part.casefold() for part in path.parts]
        suffix = path.suffix.casefold()
        basename = path.name.casefold()

        reasons = []
        if FORBIDDEN_DIRS.intersection(parts[:-1]):
            reasons.append("forbidden data directory")
        reviewed_media = (
            suffix in REVIEWABLE_MEDIA_SUFFIXES
            and relative.casefold() in REVIEWED_SAFE_MEDIA_PATHS
        )
        if suffix in FORBIDDEN_SUFFIXES and not reviewed_media:
            reasons.append(f"forbidden candidate extension {suffix}")
        if basename in FORBIDDEN_NAMES or basename.startswith(".env."):
            reasons.append("private dump/device configuration")
        if CORE_DUMP_BASENAME.fullmatch(path.name):
            reasons.append("core dump filename")
        if basename.startswith(("screenshot", "photo", "crashdump", "vita_log")):
            reasons.append("possible screenshot, photo, or device log")
        if reasons:
            findings.append(f"{relative}: {', '.join(reasons)}")

    if findings:
        print("Data-free policy candidates found:", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1

    print(f"Data-free path preflight passed ({len(candidates)} Git candidate paths; names only).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
