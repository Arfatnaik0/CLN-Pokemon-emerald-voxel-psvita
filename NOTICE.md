# Third-Party Notices & Provenance

This repository distributes a **data-free PS Vita port overlay** (`port/`, `scripts/`, documentation, and host tests). Its runtime started from `gradenGnostic/pokeemerald-multiplatform`, and its 3D `vitaGL` voxel diorama subsystem, day/night clock, and voxel compatibility shims were directly integrated from **`HctrRB/pokeemerald-vita-3ds`** (which in turn ported **`ZallaxDev/emerald-3ds`** to PS Vita). Each component retains its respective upstream license and attribution requirements.

---

## 1. `HctrRB/pokeemerald-vita-3ds` — PS Vita `vitaGL` Voxel Port, Shims & Day/Night (`MIT` & `CC BY-NC-SA 4.0`)

- **Upstream**: [https://github.com/HctrRB/pokeemerald-vita-3ds](https://github.com/HctrRB/pokeemerald-vita-3ds)
- **Role**: Directly provided the PS Vita (`vitaGL`) port of `ZallaxDev/emerald-3ds`'s voxel engine (`src/platform/voxel/*`), `vita_daynight.c`, and the Vita compatibility shims and game hooks used to wire the 3D overworld renderer into this port's multiplatform PPU/SDL2 loop.
- **Files from `HctrRB/pokeemerald-vita-3ds` under `CC BY-NC-SA 4.0` (derived from `ZallaxDev/emerald-3ds`)**:
  - **Verbatim from `HctrRB/pokeemerald-vita-3ds`** (30 `CC BY-NC-SA 4.0` files): `3ds_field_view.c/.h`, `voxel_arena.c/.h`, `voxel_atlas.c/.h`, `voxel_battle.c/.h`, `voxel_building.c/.h`, `voxel_camera.c/.h`, `voxel_entities.h`, `voxel_grade.c/.h`, `voxel_lighting.c/.h`, `voxel_mesh_builder.c/.h`, `voxel_regions.c/.h`, `voxel_relief.c/.h`, `voxel_sign.c/.h`, `voxel_sprite_slots.c/.h`, `voxel_swoop.c/.h`, `voxel_tree.h`, `voxel_tuning.h`, `voxel_unswizzle.h`.
  - **Adapted from `HctrRB/pokeemerald-vita-3ds`** (5 `CC BY-NC-SA 4.0` files): `voxel_renderer.c`, `voxel_entities.c`, `voxel_tree.c`, `voxel_world.c/.h`.
- **Files / hooks from `HctrRB/pokeemerald-vita-3ds` under `MIT`**:
  - **Verbatim shim headers** under `port/src/platform/voxel/`: `3ds_platform.h`, `3ds_video.h`, `port_log.h`, `port_platform.h`.
  - **Adapted shim headers** under `port/src/platform/voxel/`: `3ds_log.h`, `voxel_file.h` (changed override path from `ux0:data/POKEVITA/` to `ux0:data/pokeemerald/`), `voxel_renderer.h`.
  - **Day/Night module & interface stubs**: `port/src/platform/vita/vita_daynight.c` (verbatim from `HctrRB/pokeemerald-vita-3ds`), plus compatible interface/stub headers derived from `HctrRB`'s declarations in `port/include/platform/{framedraw.h, ui.h, vita_daynight.h, vita_extras.h, voxel_snapshot.h}` and `port/src/platform/vita/vita_perf.h`.
  - **Game hook hunks** in [port/patches/upstream-vita-hooks.patch](port/patches/upstream-vita-hooks.patch): `PortDoor_Get` / `PortDoor_GetTwin` / `PortDoor_Forget` (`src/field_door.c`, `src/field_camera.c`), `sMapMutationLock` / `Overworld_IsReturningToField` (`src/overworld.c`), `UpdateShadowFieldEffect` voxel check (`src/field_effect_helpers.c`), and `VoxelRenderer_NotifyTilesetAnimWrite` (`src/tileset_anims.c`).

---

## 2. ZallaxDev / `emerald-3ds` — 3D Voxel Diorama Engine (`CC BY-NC-SA 4.0`)

- **Upstream**: [https://github.com/ZallaxDev/emerald-3ds](https://github.com/ZallaxDev/emerald-3ds) (versions `>= 0.5.0`, see `LICENSE-PORT.md` and `NOTICE.md` upstream)
- **License**: [Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (`CC BY-NC-SA 4.0`)](https://creativecommons.org/licenses/by-nc-sa/4.0/)
- **Required Attribution**: `Zallax and Emerald 3DS contributors` ([ZallaxDev/emerald-3ds](https://github.com/ZallaxDev/emerald-3ds)) and `HctrRB` ([HctrRB/pokeemerald-vita-3ds](https://github.com/HctrRB/pokeemerald-vita-3ds)).
- **Per-File Classification under `port/src/platform/voxel/`**:
  - **Originally created in `emerald-3ds` (`>= 0.5.0`, `CC BY-NC-SA 4.0`) and ported to `vitaGL` by `HctrRB/pokeemerald-vita-3ds`**: `voxel_renderer.c`, `voxel_tuning.h`, `voxel_arena.c/.h`, `voxel_battle.c/.h`, `voxel_building.c/.h`, `voxel_grade.c/.h`, `voxel_lighting.c/.h`, `voxel_regions.c/.h`, `voxel_relief.c/.h`, `voxel_sign.c/.h`, `voxel_sprite_slots.c/.h`, `voxel_swoop.c/.h`, `voxel_tree.c/.h`, `voxel_unswizzle.h`, `3ds_field_view.c/.h`.
  - **Mixed Provenance (`pokeemerald-multiplatform` MIT → `emerald-3ds >= 0.5.0` `CC BY-NC-SA 4.0` → `HctrRB/pokeemerald-vita-3ds` → this repository)**: `voxel_world.c/.h`, `voxel_camera.c/.h`, `voxel_mesh_builder.c/.h`, `voxel_atlas.c/.h`, `voxel_entities.c/.h`. Header comments inside those files record their early MIT-licensed multiplatform ancestry (`voxel_world.c`, `voxel_camera.c`, `voxel_mesh.c`, `voxel_renderer.c`) prior to being rewritten for `emerald-3ds` (`ctr_voxel.c`) under `CC BY-NC-SA 4.0`.
- **Additional changes made in this repository on top of `HctrRB/pokeemerald-vita-3ds`**:
  - Added `Pass_Overlay2D()` scanline synchronization (`RunFrameLineEvents`) against this port's multi-core `gba_easy_draw.c` PPU so GBA overworld audio stays continuous in 3D mode.
  - Added a 6-frame `vglMemalign` dynamic vertex stream ring in `voxel_renderer.c` for `vitaGL` `-DDRAW_SPEEDHACK` compatibility, plus 3D `MB_TALL_GRASS` / `MB_ASHGRASS` dual-row blade clusters and rustling animations in `voxel_tree.c` / `voxel_entities.c` / `voxel_world.c`.
  - Added [port/src/platform/voxel/voxel_vita_bridge.c](port/src/platform/voxel/voxel_vita_bridge.c) (`MIT`) to bridge `_newlib_heap_size_user` and config/log calls to `sdl2.c` and `vita_perf.c`.
- **NonCommercial & ShareAlike Notice**: All `CC BY-NC-SA 4.0` code and adaptations remain strictly non-commercial and must retain attribution and ShareAlike (`CC BY-NC-SA 4.0`) terms if redistributed or modified.

---

## 3. `gradenGnostic/pokeemerald-multiplatform` (`MIT`)

- **Upstream**: [https://github.com/gradenGnostic/pokeemerald-multiplatform](https://github.com/gradenGnostic/pokeemerald-multiplatform) (and prior PC/SDL port contributors: `Kurausukun`, `NT_x86`, `Normmatt`, `camthesaxman`, et al.)
- **Notice Preserved**: `Copyright (c) 2026 pokeemerald-multiplatform contributors` (MIT License covering `Port Modifications`, excluding upstream `pret/pokeemerald`, third-party software, and Nintendo/Creatures/GAME FREAK IP — reproduced in [LICENSE](LICENSE)).
- **Role**: Base multiplatform SDL2 backend (`sdl2.c`), M4A/CGB audio mixer foundation (`cgb_audio.c`), `gba_easy_draw.c` PPU framework, follower sprite hook compatibility, and multiplatform build integration.
- **Original additions authored for this repository (`MIT`)**: Multi-core `gba_easy_draw.c` NEON/scanline optimizations and `gba_easy_draw_workers.*`, `vita_perf.c`, `vita_thread_affinity.*`, `frame_pacing.h`, `port/vita/CMakeLists.txt` and build/LiveArea scripts, `sdl2-vita-audio-42060.patch`, `port/tests/platform/*`, `port/tools/extract_voxel_layout_models.py`, and connected-map NPC spawn/removal bounds in `event_object_movement.c`.
- **Note on upstream files omitted from `port/`**: Unmodified upstream platform files (including `src/platform/bios.c` containing **VBA-M** `GPL-2.0` and **mGBA** `MPL-2.0` implementations, `src/platform/gba_fast_draw.c`, `src/platform/dma.c`, `src/platform/win32.c`, `src/platform/nostd.c`, and `src/platform/mods/*` including MIT-licensed `cJSON`) are omitted from `port/` so developers supply them directly from their local `pokeemerald` checkout.

---

## 4. `pret/pokeemerald` & Nintendo / Creatures Inc. / GAME FREAK inc.

- **Upstream**: [https://github.com/pret/pokeemerald](https://github.com/pret/pokeemerald)
- Minimal integration hook diffs against our local pre-overlay baseline (`c6ac631`..`15c5805`, a private locally integrated `pokeemerald-multiplatform` + follower + Vita tree not hosted on GitHub) live in [port/patches/upstream-vita-hooks.patch](port/patches/upstream-vita-hooks.patch) to connect the PS Vita display, input, audio, door state, and voxel camera hooks. Only newly authored `+` additions are MIT-licensed (per Sections 1(A) and 1(C) of [LICENSE](LICENSE)); upstream context and replaced lines remain under upstream terms.
- Pokémon and Pokémon Emerald copyrights, trademarks, assets, graphics, audio, and game content remain the exclusive property of **Nintendo**, **Creatures Inc.**, and **GAME FREAK inc.**

---

## 5. Toolchain & External Dependencies (Linked Locally at Build Time)

- **vitaGL**: [https://github.com/Rinnegatamante/vitaGL](https://github.com/Rinnegatamante/vitaGL) — `Copyright 2017, 2018, 2019, 2020 Rinnegatamante`, licensed under **GNU Lesser General Public License v3.0 or later (`LGPL-3.0-or-later`)** with **GPL-3.0** components in the upstream repository.
- **SDL2 `2.30.7`**: [https://libsdl.org/](https://libsdl.org/) — licensed under the **zlib License**. Built locally with [port/vita/patches/sdl2-vita-audio-42060.patch](port/vita/patches/sdl2-vita-audio-42060.patch).
- **VitaSDK**: [https://vitasdk.org/](https://vitasdk.org/) — community PS Vita toolchain (`MIT` / `BSD` / `GPL` components).
