# Vita voxel visual limitations

The voxel renderer is experimental. Its original tile classifier infers walls,
roofs, and furniture primarily from collision and selected metatile IDs; it
cannot reconstruct all buildings or interiors reliably. A cutaway check now
also requires horizontal overlap with the player, to avoid cutting away roofs of
buildings off to either side, but this is not a full building reconstruction.
These issues need geometry and tileset-specific validation on actual maps.

An optional set of per-layout reference models can replace generic buildings
on layouts whose placements fit the runtime limits and use one texture page.
Generate private VBL4 files with `tools/extract_voxel_layout_models.py`, passing
the local reference `buildings.bin`, this local game source root, and an output
directory. VBL4 preserves script-switched placements as exact metatile
conditions and stores ground patches separately from the cutaway building mesh.
A generated file embeds the exact map blocks. Runtime checks each model's
expanded geometry footprint and cardinal neighbor ring against the live map;
only a matching condition cell can differ from the snapshot, while other
changes keep the generic fallback. VBL1/VBL2/VBL3 assets remain loadable.
Configure `VITA_VOXEL_MODELS_DIR` to the local output directory to include the
subsets in a private test VPK. The compatibility option
`VITA_LITTLEROOT_MODELS_FILE` still supports the earlier VBL1 Littleroot file.

In the tested local data snapshot, the VBL4 exporter produced assets for 270 of
441 layouts. The 171 skipped layouts had no reference placements, so they keep
generic geometry. For odd-cell maps, the exporter trims a single trailing zero
16-bit alignment word when it rounds the snapshot to a 32-bit boundary; other
snapshot size mismatches remain unsupported.
Route110's 999 source placements are included as 944 geometry records after
empty placements are skipped. Texture pages may be up to 1024 pixels on either
side while staying within a 512×512-texel budget; each layout is limited to
1024 model records, 150,000 vertices, and a 5 MiB asset. Unsupported
multi-page layouts or malformed/unsafe geometry keep generic geometry. The
runtime caches up to six distinct visible layouts to bound GPU use; additional
visible layouts use generic geometry for that frame. This remains partial model
coverage, not full parity with the reference.
Assets come from game-derived data and must stay local; do not commit them or
distribute the resulting VPK. Visual alignment and performance still need Vita
hardware testing.

Cardinal jump ledges now expose a textured drop face on the direction selected
by the game's `MB_JUMP_*` behavior, including when the destination is void.
VXL4 relief now has an experimental, opt-in reader and terrain mesh. Connected
map instances receive a pixel-aligned offset from their shared edge heights;
the VXL4 south-edge ease is applied to terrain Z, and the camera and actor
ground transforms sample the same height field. Buildings from VBL assets and
generic structures receive a rigid lift/shift only when every support cell is
flat and shares one transform. General tree ground cards follow the 5x5 relief
grid; tree crowns use their anchor tile's sampled transform. Sloped building
or structure support remains visible at its previous elevation and is logged
once, because this renderer does not yet deform those object meshes to follow a
slope. VXS3 sign grounds follow relief, and their extruded silhouettes use the
base tile's sampled transform. The full VXL4 cut/edge and shadow behavior is
not ported, and relief remains OFF by default until Vita visual and performance
validation.

The atlas learns newly used live metatile IDs while Vita game logic is blocked
at VBlank, so an opening door or changed map block no longer necessarily points
at an uninitialized atlas slot. The atlas still uses tiles and palettes from
the original tilesets: animated tile graphics and live palette cycling are not
recomposed. A correct implementation needs bounded, GPU-safe partial updates;
refreshing this 512x512 RGBA atlas through the current vitaGL can copy the full
1 MiB texture per update frame and requires performance testing.

The outdoor border pass samples the current layout's 2x2 border metatiles with
the same grid phase as the 2D field renderer and skips cells occupied by
connected maps. It draws a bounded 33x33 region around the player. Cells and
triangle vertices are cached until the player changes tile or the map/connection
set changes. The camera can expose the clear color outside this bounded region.

For General-tileset borders whose four metatiles match a known 2x2 tree, an
optional local 128x128 `trees.rgba5551` atlas adds transparent, leaning tree
crowns and ground, modeled after the atlas and tree geometry in
[HctrRB/pokeemerald-vita-3ds](https://github.com/HctrRB/pokeemerald-vita-3ds),
itself based on Emerald 3DS. Configure the local test build with
`-DVITA_TREE_ATLAS_FILE=/path/to/trees.rgba5551`. The atlas is game-derived data:
keep it outside the public repository and do not distribute it with a public
build. Current testing uses the atlas from the local reference checkout; a
reproducible extractor from a user's own dump is still needed. If the atlas is
absent or a map uses an unsupported border/tileset, the pass falls back to
flat, repeated border metatiles. The reference's nearest-compatible-map border
selection is not integrated, so some connections may differ from the 2D view.

Inside maps, the dedicated transparent tree-card pass handles the General
tileset's supported large 2x2 tree parts and their matching atlas ground. Small
trees, secondary-tileset variants, long grass and other unsupported patterns
still use the generic classifier. The reference port has broader
tileset-specific IDs, overlapping crowns and other ground layers; those remain
future work.

Indoor, secret-base and underground clear colors are black. These changes and
their effect on frame rate require physical Vita testing; a successful compile
is not evidence that voxel graphics are correct or that any frame-rate target
has been met.
