#!/usr/bin/env python3
"""Generate clean, data-free GitHub release notes for this PS Vita port."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def git_output(args: list[str]) -> str:
    return subprocess.check_output(["git", *args], text=True).strip()


def build_notes(tag: str, limit: int = 10) -> str:
    head_short = git_output(["rev-parse", "--short", "HEAD"])
    head_full = git_output(["rev-parse", "HEAD"])
    commit_log = git_output(["log", f"-n{limit}", "--pretty=format:- `%h` %s (%cs)"])

    return f"""Source-only data-free release of the **Pokémon Emerald Voxel PS Vita** port. **Work in progress**: the 3D voxel renderer is experimental and parts of the port are still being validated on hardware.

> No `.vpk` or game asset pack is distributed here: a playable package embeds game data derived from Nintendo's Pokémon Emerald. Build your own locally from **your own legally obtained** Pokémon Emerald (USA, Europe) Rev 0 (`BPEE`) `.gba` ROM using this repository.

## What's fixed in {tag}

- **Misplaced quest & transition NPCs (`TrySpawnObjectEvents`)**:
  - **Cause**: In 3D Voxel mode, `TrySpawnObjectEvents` pre-spawned NPCs from adjacent connected maps directly out of static `connHeader->events->objectEvents` (`map.json` ROM coordinates) before `LoadMapFromCameraTransition` -> `RunOnTransitionMapScript()` could run `setobjectxyperm` (for example, `OldaleTown` `LOCALID_FOOTPRINTS_MAN` at `(8, 9)` instead of `(1, 11)` and `PetalburgCity` `LOCALID_GYM_BOY` at `(12, 15)` instead of `(5, 11)`). Because those NPCs were already active in `gObjectEvents[]` under the destination `(mapGroup, mapNum)`, crossing the border skipped spawning the updated templates and left them stuck at the wrong positions.
  - **Fix**: Removed the connected-map `connHeader->events->objectEvents` pre-spawn block in [port/patches/upstream-vita-hooks.patch](https://github.com/Arfatnaik0/CLN-Pokemon-emerald-voxel-psvita/blob/main/port/patches/upstream-vita-hooks.patch) (`src/event_object_movement.c`), restoring `15c5805` / 2D behavior across all 49 `setobjectxyperm` map scripts (including 16 outdoor connected maps) so NPCs spawn from `gSaveBlock1Ptr->objectEventTemplates` after `ON_TRANSITION` finishes.
- **v1.0 / baseline highlights included in this release**:
  - **222 MHz balanced default ARM clock** on Vita startup ([sdl2.c](https://github.com/Arfatnaik0/CLN-Pokemon-emerald-voxel-psvita/blob/main/port/src/platform/sdl2.c)).
  - **3D animated tall grass & ash grass** swaying cleanly in voxel mode ([voxel_tree.c](https://github.com/Arfatnaik0/CLN-Pokemon-emerald-voxel-psvita/blob/main/port/src/platform/voxel/voxel_tree.c)).
  - **GPU crash fix on save load (`-DDRAW_SPEEDHACK`)**: non-VBO draws routed through a 6-slot `vglMemalign` GPU-mapped stream ring ([voxel_renderer.c](https://github.com/Arfatnaik0/CLN-Pokemon-emerald-voxel-psvita/blob/main/port/src/platform/voxel/voxel_renderer.c)).
  - **Clean `15c5805` overlay build**: `apply_port.sh` cleans obsolete `src/platform/voxel/` and `src/platform/vita/` files and `CMakeLists.txt` lists the active voxel C units explicitly.

## Latest tracked revision

- **Branch tip**: `{head_short}` (`{head_full}`)
- **Recent commits on `main`**:
{commit_log}

## How to build your own VPK

1. Download **Source code (zip)** or **Source code (tar.gz)** attached below (or clone `main`).
2. Supply your own legally obtained Pokémon Emerald (USA, Europe) Rev 0 `.gba` ROM and verify it with `python3 scripts/verify_rom.py`.
3. Follow the full build steps in [README.md](https://github.com/Arfatnaik0/CLN-Pokemon-emerald-voxel-psvita/blob/main/README.md) (`apply_port.sh --apply-patch`, host `make_tools.mk` / `Makefile_pc` M0 intermediates, `prepare_livearea_assets.sh`, patched SDL2 + `vitaGL`, and CMake/Ninja) and review [ARCHITECTURE.md](https://github.com/Arfatnaik0/CLN-Pokemon-emerald-voxel-psvita/blob/main/ARCHITECTURE.md).
4. Copy your locally built `voxel/` pack (generated via `ZallaxDev/emerald-3ds` scripts) and optional `followers/` pack to `ux0:data/pokeemerald/` (or package them in a private local VPK), then install with VitaShell. Saves live in `ux0:data/pokeemerald/pokeemerald.sav`.

> *Upgrading an existing save:* Save files persist active `gObjectEvents[]`. If an earlier build saved an NPC at the wrong tile in Oldale Town or Petalburg City, walk in and out of any building once to respawn `gObjectEvents` cleanly.

## Known limitations

- **Experimental 3D coverage**: Partial `VBL4` layout coverage (270/441 layouts in tested snapshots) and generic geometry fallback on unsupported buildings/interiors/tilesets; see [VITA_VOXEL_VISUAL_LIMITATIONS.md](https://github.com/Arfatnaik0/CLN-Pokemon-emerald-voxel-psvita/blob/main/port/docs/VITA_VOXEL_VISUAL_LIMITATIONS.md).
- **2D fallback toggle**: Press the button mapped to `SDL_CONTROLLER_BUTTON_X` on PS Vita to switch between 3D voxel mode and classic 2D mode on the fly.
- **No multiplayer / Wireless Adapter**: Link cable and wireless RFU features are stubbed out under `PORTABLE`.
- **Legal & licensing**: See [LICENSE](https://github.com/Arfatnaik0/CLN-Pokemon-emerald-voxel-psvita/blob/main/LICENSE), [NOTICE.md](https://github.com/Arfatnaik0/CLN-Pokemon-emerald-voxel-psvita/blob/main/NOTICE.md), and [SECURITY_OR_ASSET_POLICY.md](https://github.com/Arfatnaik0/CLN-Pokemon-emerald-voxel-psvita/blob/main/SECURITY_OR_ASSET_POLICY.md). This is an unofficial fan port; Pokémon and related properties belong to Nintendo / Creatures / GAME FREAK.
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True, help="Release tag name (e.g. v1.01)")
    parser.add_argument("--out", type=Path, help="Optional file path to write notes to")
    args = parser.parse_args()

    notes = build_notes(args.tag)
    if args.out is not None:
        args.out.write_text(notes, encoding="utf-8")
    else:
        sys.stdout.write(notes)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
