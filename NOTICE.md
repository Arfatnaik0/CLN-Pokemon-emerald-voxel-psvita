# Third-Party Notices & Provenance

This repository distributes a **data-free PS Vita port overlay** (`port/`, `scripts/`, documentation, and host tests). It relies on—and adapts code from—several upstream projects, each governed by its own license and attribution requirements.

---

## 1. ZallaxDev / `emerald-3ds` — 3D Voxel Diorama Engine (`CC BY-NC-SA 4.0`)

- **Upstream**: [https://github.com/ZallaxDev/emerald-3ds](https://github.com/ZallaxDev/emerald-3ds) (versions `>= 0.5.0`, see `LICENSE-PORT.md` upstream)
- **License**: [Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (`CC BY-NC-SA 4.0`)](https://creativecommons.org/licenses/by-nc-sa/4.0/)
- **Per-File Classification under `port/src/platform/voxel/`**:
  - **Ported from `emerald-3ds` (`>= 0.5.0`, `CC BY-NC-SA 4.0`)**: `voxel_renderer.c`, `voxel_tuning.h`, `voxel_arena.c/.h`, `voxel_battle.c/.h`, `voxel_building.c/.h`, `voxel_grade.c/.h`, `voxel_lighting.c/.h`, `voxel_regions.c/.h`, `voxel_relief.c/.h`, `voxel_sign.c/.h`, `voxel_sprite_slots.c/.h`, `voxel_swoop.c/.h`, `voxel_tree.c/.h`, `voxel_unswizzle.h`, `3ds_field_view.c/.h`, and [port/docs/VITA_VOXEL_VISUAL_LIMITATIONS.md](port/docs/VITA_VOXEL_VISUAL_LIMITATIONS.md).
  - **Mixed Provenance (`pokeemerald-multiplatform` MIT → `emerald-3ds >= 0.5.0` `CC BY-NC-SA 4.0` → PS Vita)**: `voxel_world.c/.h`, `voxel_camera.c/.h`, `voxel_mesh_builder.c/.h`, `voxel_atlas.c/.h`, `voxel_entities.c/.h`. Header comments inside those files record their early MIT-licensed multiplatform ancestry (`voxel_world.c`, `voxel_camera.c`, `voxel_mesh.c`, `voxel_renderer.c`) prior to being rewritten for `emerald-3ds` (`ctr_voxel.c`) under `CC BY-NC-SA 4.0`.
  - **Original PS Vita Glue / Bridge Shims (`MIT`)**: `3ds_log.h`, `3ds_platform.h`, `3ds_video.h`, `port_log.h`, `port_platform.h`, `voxel_file.h`, `voxel_renderer.h`, and `voxel_vita_bridge.c`.
- **Changes made for PS Vita**:
  - Replaced 3DS `citro3d` / `libctru` rendering calls with `vitaGL` fixed-function OpenGL ES (`glDrawArrays`, ping-pong uniform compatibility, `128`-byte-aligned chunk VBO streaming).
  - Moved chunk mesh rebuilding and Morton texture unswizzling onto dedicated worker threads (`VOXEL_CHUNK_WORKER`) and eliminated floating sky motes (`VOXEL_MOTE_COUNT`) that caused vertex artifacts on Vita.
  - Upgraded Tall Grass (`MB_TALL_GRASS`) and Route 113 Ash Grass (`MB_ASHGRASS`) 3D blade clusters (two varied cutout variants packed in `y=112..127` of `trees.rgba5551`) and added upright 3D rustling/leaf-burst animations in `voxel_entities.c` synced to `FLDEFFOBJ_TALL_GRASS` and `FLDEFFOBJ_LONG_GRASS`.
  - Added `Pass_Overlay2D()` scanline synchronization (`RunFrameLineEvents`) so GBA overworld audio stays continuous in 3D mode.
- **NonCommercial & ShareAlike Notice**: All `CC BY-NC-SA 4.0` code and adaptations remain strictly non-commercial and must retain attribution and ShareAlike (`CC BY-NC-SA 4.0`) terms if redistributed or modified.

---

## 2. `gradenGnostic/pokeemerald-multiplatform` (`MIT`)

- **Upstream**: [https://github.com/gradenGnostic/pokeemerald-multiplatform](https://github.com/gradenGnostic/pokeemerald-multiplatform) (and prior PC/SDL port contributors: `Kurausukun`, `NT_x86`, `Normmatt`, `camthesaxman`, et al.)
- **Notice Preserved**: `Copyright (c) 2026 pokeemerald-multiplatform contributors` (MIT License covering `Port Modifications`, excluding upstream `pret/pokeemerald`, third-party software, and Nintendo/Creatures/GAME FREAK IP — reproduced in [LICENSE](LICENSE)).
- **Role**: Base multiplatform SDL2 backend, M4A/CGB audio mixer foundation, `gba_easy_draw` PPU framework, follower sprite hook compatibility, and multiplatform build integration.
- **Note on upstream files omitted from `port/`**: Unmodified upstream platform files (including `src/platform/bios.c` containing **VBA-M** `GPL-2.0` and **mGBA** `MPL-2.0` implementations, `src/platform/gba_fast_draw.c`, `src/platform/dma.c`, `src/platform/win32.c`, `src/platform/nostd.c`, and `src/platform/mods/*` including MIT-licensed `cJSON`) are omitted from `port/` so developers supply them directly from their local `pokeemerald` checkout.

---

## 3. `pret/pokeemerald` & Nintendo / Creatures Inc. / GAME FREAK inc.

- **Upstream**: [https://github.com/pret/pokeemerald](https://github.com/pret/pokeemerald)
- Minimal integration hook diffs against our local pre-overlay baseline (`c6ac631`..`15c5805`, a private locally integrated `pokeemerald-multiplatform` + follower + Vita tree not hosted on GitHub) live in [port/patches/upstream-vita-hooks.patch](port/patches/upstream-vita-hooks.patch) to connect the PS Vita display, input, audio, door state, and voxel camera hooks. Only the newly authored `+` additions in that patch are MIT-licensed under Section 1(A) of [LICENSE](LICENSE); upstream context and replaced lines remain under upstream terms.
- Pokémon and Pokémon Emerald copyrights, trademarks, assets, graphics, audio, and game content remain the exclusive property of **Nintendo**, **Creatures Inc.**, and **GAME FREAK inc.**

---

## 4. Toolchain & External Dependencies (Linked Locally at Build Time)

- **vitaGL**: [https://github.com/Rinnegatamante/vitaGL](https://github.com/Rinnegatamante/vitaGL) — `Copyright 2017, 2018, 2019, 2020 Rinnegatamante`, licensed under **GNU Lesser General Public License v3.0 or later (`LGPL-3.0-or-later`)** with **GPL-3.0** components in the upstream repository. The local reference library (`VITAGL_ROOT`) is compiled with fixed-function/shader-cache flags (`-DHAVE_SHADER_CACHE -DSKIP_SPLASHSCREEN -DSTRICT_DRAW_COMPLIANCE -DUNIFORM_PINGPONG -DALIGNED_POOL -DDRAW_SPEEDHACK -DINDICES_SPEEDHACK -DSAMPLERS_SPEEDHACK -DGXM_STATE_CACHE -DHAVE_SCRATCH_MEMORY ...`).
- **SDL2 `2.30.7`**: [https://libsdl.org/](https://libsdl.org/) — licensed under the **zlib License**. Built locally with [port/vita/patches/sdl2-vita-audio-42060.patch](port/vita/patches/sdl2-vita-audio-42060.patch).
- **VitaSDK**: [https://vitasdk.org/](https://vitasdk.org/) — community PS Vita toolchain (`MIT` / `BSD` / `GPL` components).
