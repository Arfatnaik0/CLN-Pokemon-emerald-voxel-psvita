#!/usr/bin/env python3
"""Extract safe per-layout model subsets from a local VXB9 pack.

This is a private test-build helper. VBL4 preserves VXB9 placement conditions
and keeps ground-patch triangles separate from cutaway building geometry.
Its source-map snapshot lets the Vita runtime fall back to the generic
renderer whenever a model's map region changes.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct

VBL_HEADER = struct.Struct("<4s8I")
MODEL = struct.Struct("<8I")
CONDITION = struct.Struct("<I H B B")
VERTEX = struct.Struct("<6f")
VXB_PAGE = struct.Struct("<HHI")
VXB_MODEL = struct.Struct("<BBHIII")
VXB_PAGE_MODEL = struct.Struct("<HHhh")
VXB_PLACEMENT = struct.Struct("<6HI")
MAX_LAYOUT_SIDE = 256
MAX_MODELS = 1024
MAX_VERTICES = 150_000
MAX_OUTPUT_BYTES = 5 * 1024 * 1024
MAX_CONDITIONS = 4096
MAX_TEXTURE_WIDTH = 1024
MAX_TEXTURE_HEIGHT = 1024
MAX_TEXTURE_TEXELS = 512 * 512
WORLD_VERTEX_EPSILON = 0.01
UV_EDGE_EPSILON = 0.0001


def parse_reference(data: bytes):
    if len(data) < 28 or data[:4] != b"VXB9":
        raise ValueError("expected a local VXB9 building pack")
    pages_n, models_n, page_models_n, placements_n, heights_n, masks_n, vertices_n, variants_n, decks_n, conditions_n, _ = struct.unpack_from("<6HI4H", data, 4)
    offset = 28

    def rows(layout: struct.Struct, count: int):
        nonlocal offset
        end = offset + layout.size * count
        if end > len(data):
            raise ValueError("truncated VXB9 table")
        result = list(layout.iter_unpack(data[offset:end]))
        offset = end
        return result

    pages = rows(VXB_PAGE, pages_n)
    models = rows(VXB_MODEL, models_n)
    page_models = rows(VXB_PAGE_MODEL, page_models_n)
    placements = rows(VXB_PLACEMENT, placements_n)
    offset += heights_n + (heights_n & 1) + heights_n * 2 + masks_n * 32
    offset += heights_n + (heights_n & 1) + variants_n * 6 + decks_n * 8
    if offset + conditions_n * 6 > len(data):
        raise ValueError("truncated VXB9 conditions")
    conditions = {}
    for i in range(conditions_n):
        placement, metatile, x, y = struct.unpack_from("<H H B B", data, offset + i * 6)
        conditions.setdefault(placement, []).append((metatile, x, y))
    if any(placement >= len(placements) for placement in conditions):
        raise ValueError("VXB9 condition references an invalid placement")
    offset = (offset + conditions_n * 6 + 3) & ~3
    vertex_end = offset + vertices_n * VERTEX.size
    if vertex_end > len(data):
        raise ValueError("truncated VXB9 vertices")
    return pages, models, page_models, placements, conditions, vertices_n, offset, vertex_end


def morton_to_linear(data: bytes, tex_w: int, tex_h: int, texture_height: int) -> bytes:
    if tex_w % 8 or tex_h % 8 or texture_height < tex_h:
        raise ValueError("texture page dimensions must be multiples of 8")
    if len(data) != tex_w * tex_h * 2:
        raise ValueError("texture page length mismatch")
    source = struct.unpack("<" + "H" * (tex_w * tex_h), data)
    output = [0] * (tex_w * texture_height)
    for y in range(tex_h):
        for x in range(tex_w):
            index = (((y >> 3) * (tex_w >> 3)) + (x >> 3)) * 64
            index += (x & 1) + ((y & 1) << 1) + ((x & 2) << 1)
            index += ((y & 2) << 2) + ((x & 4) << 2) + ((y & 4) << 3)
            output[(texture_height - 1 - y) * tex_w + x] = source[index]
    return struct.pack("<" + "H" * len(output), *output)


def extract_layout(reference: bytes, tables, layout_id: int, layout: dict, map_data: bytes):
    pages, models, page_models, placements, conditions, vertex_count, vertex_offset, vertex_end = tables
    width, height = layout["width"], layout["height"]
    if layout_id <= 0 or layout_id > 0xFFFF or width <= 0 or height <= 0 or width > MAX_LAYOUT_SIDE or height > MAX_LAYOUT_SIDE:
        raise ValueError("layout id or dimensions exceed safe bounds")
    if len(map_data) != width * height * 2:
        raise ValueError("local map snapshot size does not match layout dimensions")
    selected = [(index, p) for index, p in enumerate(placements) if p[0] == layout_id]
    if not selected:
        return None
    if len(selected) > MAX_MODELS:
        return None

    # Keep every well-formed placement. VXB9 marks script-switched models in
    # a separate condition table and stores per-placement ground patches as
    # their own vertex range; VBL4 carries both into the runtime. Unsupported
    # or malformed placements keep the generic mesher, and overlapping models
    # are omitted with them so a fallback cannot draw over a custom model.
    omitted_rects = []
    candidates = []
    for placement_index, placement in selected:
        if placement[5] > vertex_count or placement[6] > vertex_count - placement[5]:
            return None
        if placement[1] >= len(page_models):
            return None
        model_id = page_models[placement[1]][0]
        if model_id >= len(models):
            return None
        model_w, model_h, _, first, count_and_flags, _ = models[model_id]
        count = count_and_flags & 0x7FFFFFFF
        if (not model_w or not model_h or count % 3 or placement[5] % 6
                or first > vertex_count or count > vertex_count - first):
            return None
        rect = (max(0, placement[2]), max(0, placement[3]),
                min(width, placement[2] + model_w),
                min(height, placement[3] + model_h))
        page_model = page_models[placement[1]]
        model_id, page_id, _, _ = page_model
        if model_id >= len(models) or page_id >= len(pages):
            return None
        page_w, page_h, _ = pages[page_id]
        page_h &= 0x7FFF
        page_texture_height = 1 << (page_h - 1).bit_length() if page_h else 0
        model = models[model_id]
        model_count = model[4] & 0x7FFFFFFF
        model_first = model[3]
        if model_first > vertex_count or model_count > vertex_count - model_first:
            return None
        # Skip only the unsafe placement. Other non-overlapping placements on
        # this layout remain usable. The enclosing generic geometry is kept
        # over its footprint, and intersecting models are omitted below.
        if (page_w == 0 or page_h == 0 or page_w > MAX_TEXTURE_WIDTH
                or page_h > MAX_TEXTURE_HEIGHT):
            omitted_rects.append(rect)
            continue
        # Some VXB9 layouts contain placeholder placements with neither model
        # triangles nor a ground patch. They draw nothing, so omit just those
        # records instead of dropping an otherwise complete layout.
        if model_count == 0 and placement[5] == 0:
            continue
        if model_count % 3 or placement[5] % 6:
            omitted_rects.append(rect)
            continue
        safe = True
        min_x = min_z = math.inf
        max_x = max_z = -math.inf
        for first_vertex, count_vertices, uv_page_x, uv_page_y, bounds_w, bounds_h in (
                (model_first, model_count, page_model[2], page_model[3], page_w, page_texture_height),
                (placement[6], placement[5], 0, 0, page_w, page_texture_height)):
            for vertex_index in range(first_vertex, first_vertex + count_vertices):
                x, y, z, u, v, shade = VERTEX.unpack_from(
                    reference, vertex_offset + vertex_index * VERTEX.size)
                if not all(math.isfinite(value) for value in (x, y, z, u, v, shade)):
                    safe = False
                    break
                # Preserve overrun UVs for interpolation. GL_CLAMP_TO_EDGE
                # clamps fragments after interpolation, as in the reference.
                if (y < -1 or y > 32
                        or uv_page_x + u < -bounds_w - UV_EDGE_EPSILON
                        or uv_page_x + u > 2 * bounds_w + UV_EDGE_EPSILON
                        or uv_page_y + v < -bounds_h - UV_EDGE_EPSILON
                        or uv_page_y + v > 2 * bounds_h + UV_EDGE_EPSILON
                        or shade < 0 or shade > 2):
                    safe = False
                    break
                world_x = placement[2] + x
                world_z = placement[3] + z
                if (world_x < -1.0 - WORLD_VERTEX_EPSILON
                        or world_x > width + 1.0 + WORLD_VERTEX_EPSILON
                        or world_z < -1.0 - WORLD_VERTEX_EPSILON
                        or world_z > height + 1.0 + WORLD_VERTEX_EPSILON):
                    safe = False
                    break
                min_x = min(min_x, world_x)
                max_x = max(max_x, world_x)
                min_z = min(min_z, world_z)
                max_z = max(max_z, world_z)
            if not safe:
                break
        for metatile, cx, cy in conditions.get(placement_index, ()):
            if (metatile > 0x3FF or cx >= model_w or cy >= model_h
                    or placement[2] + cx >= width or placement[3] + cy >= height):
                safe = False
                break
        if not safe:
            omitted_rects.append(rect)
            continue
        # Include the triangles' actual horizontal extent. This clips border
        # placements safely while ensuring overhanging surfaces suppress the
        # generic geometry that would otherwise show through them.
        if min_x != math.inf:
            epsilon = 0.00001
            rect = (max(0, min(rect[0], math.floor(min_x + epsilon))),
                    max(0, min(rect[1], math.floor(min_z + epsilon))),
                    min(width, max(rect[2], math.ceil(max_x - epsilon))),
                    min(height, max(rect[3], math.ceil(max_z - epsilon))))
        if rect[0] >= rect[2] or rect[1] >= rect[3]:
            continue
        candidates.append((placement_index, placement, rect))

    selected = []
    for placement_index, placement, rect in candidates:
        x0, y0, x1, y1 = rect
        if any(x0 < ox1 and ox0 < x1 and y0 < oy1 and oy0 < y1
               for ox0, oy0, ox1, oy1 in omitted_rects):
            continue
        selected.append((placement_index, placement, rect))
    if not selected or len(selected) > MAX_MODELS:
        return None

    page_ids = set()
    output_models, output_vertices, output_conditions = [], [], []
    for placement_index, placement, rect in selected:
        if placement[1] >= len(page_models):
            return None
        model_id, page_id, texture_x, texture_y = page_models[placement[1]]
        if model_id >= len(models) or page_id >= len(pages):
            return None
        page_ids.add(page_id)
        if len(page_ids) != 1:
            return None
        model_w, model_h, _, first, count_and_flags, _ = models[model_id]
        count = count_and_flags & 0x7FFFFFFF
        patch_count = placement[5]
        patch_first = placement[6]
        if (not model_w or not model_h or (count == 0 and patch_count == 0)
                or count % 3 or patch_count % 6
                or first > vertex_count or count > vertex_count - first):
            return None
        if count + patch_count > MAX_VERTICES - len(output_vertices):
            return None
        tex_w, page_h, _ = pages[page_id]
        page_h &= 0x7FFF
        if not tex_w or not page_h:
            return None
        tex_height = 1 << (page_h - 1).bit_length()
        first_output = len(output_vertices)
        for source_first, source_count, uv_x, uv_y in (
                (first, count, texture_x, texture_y),):
            for i in range(source_count):
                x, y, z, u, v, shade = VERTEX.unpack_from(
                    reference, vertex_offset + (source_first + i) * VERTEX.size)
                # VXB9's model UVs are relative to their page-model origin;
                # patch UVs are already page-relative and may have a tiny
                # floating-point excursion at a clamped texture edge.
                page_u = uv_x + u
                page_v = uv_y + v
                if not all(math.isfinite(value) for value in (x, y, z, u, v, shade)):
                    return None
                world_x = placement[2] + x
                world_z = placement[3] + z
                if (world_x < -1.0 - WORLD_VERTEX_EPSILON
                        or world_x > width + 1.0 + WORLD_VERTEX_EPSILON
                        or world_z < -1.0 - WORLD_VERTEX_EPSILON
                        or world_z > height + 1.0 + WORLD_VERTEX_EPSILON
                        or y < -1 or y > 32
                        or page_u < -tex_w - UV_EDGE_EPSILON
                        or page_u > 2 * tex_w + UV_EDGE_EPSILON
                        or page_v < -tex_height - UV_EDGE_EPSILON
                        or page_v > 2 * tex_height + UV_EDGE_EPSILON
                        or shade < 0 or shade > 2):
                    return None
                output_vertices.append((x + placement[2], y, z + placement[3],
                                        page_u / tex_w,
                                        1.0 - page_v / tex_height, shade))
        base_count = len(output_vertices) - first_output
        patch_first_output = len(output_vertices)
        for i in range(patch_count):
            x, y, z, u, v, shade = VERTEX.unpack_from(
                reference, vertex_offset + (patch_first + i) * VERTEX.size)
            page_u, page_v = u, v
            if not all(math.isfinite(value) for value in (x, y, z, u, v, shade)):
                return None
            world_x = placement[2] + x
            world_z = placement[3] + z
            if (world_x < -1.0 - WORLD_VERTEX_EPSILON
                    or world_x > width + 1.0 + WORLD_VERTEX_EPSILON
                    or world_z < -1.0 - WORLD_VERTEX_EPSILON
                    or world_z > height + 1.0 + WORLD_VERTEX_EPSILON
                    or y < -1 or y > 32
                    or page_u < -tex_w - UV_EDGE_EPSILON
                    or page_u > 2 * tex_w + UV_EDGE_EPSILON
                    or page_v < -tex_height - UV_EDGE_EPSILON
                    or page_v > 2 * tex_height + UV_EDGE_EPSILON
                    or shade < 0 or shade > 2):
                return None
            output_vertices.append((x + placement[2], y, z + placement[3],
                                    page_u / tex_w,
                                    1.0 - page_v / tex_height, shade))
        output_models.append((rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1],
                              first_output, base_count, patch_first_output, patch_count))
        for condition in conditions.get(placement_index, ()):
            metatile, cx, cy = condition
            if (metatile > 0x3FF or cx >= model_w or cy >= model_h
                    or placement[2] + cx >= width or placement[3] + cy >= height):
                return None
            output_conditions.append((len(output_models) - 1, metatile,
                                      placement[2] + cx, placement[3] + cy))

    if (not output_vertices or len(output_vertices) > MAX_VERTICES
            or len(output_conditions) > MAX_CONDITIONS):
        return None
    page_id = next(iter(page_ids))
    tex_w, page_h, page_offset = pages[page_id]
    page_h &= 0x7FFF
    if (not tex_w or not page_h or tex_w > MAX_TEXTURE_WIDTH
            or page_h > MAX_TEXTURE_HEIGHT):
        return None
    tex_height = 1 << (page_h - 1).bit_length()
    if tex_w * tex_height > MAX_TEXTURE_TEXELS:
        return None
    tex_bytes = tex_w * tex_height * 2
    if page_offset + tex_w * page_h * 2 > len(reference):
        return None
    expected_bytes = (VBL_HEADER.size + len(map_data)
                      + len(output_models) * MODEL.size
                      + len(output_conditions) * CONDITION.size
                      + len(output_vertices) * VERTEX.size + tex_bytes)
    if expected_bytes > MAX_OUTPUT_BYTES:
        return None
    texture = morton_to_linear(reference[page_offset:page_offset + tex_w * page_h * 2], tex_w, page_h, tex_height)
    payload = bytearray(VBL_HEADER.pack(b"VBL4", layout_id, width, height, tex_w,
                                        tex_height, len(output_models), len(output_vertices),
                                        len(output_conditions)))
    payload.extend(map_data)
    for model in output_models:
        payload.extend(MODEL.pack(*model))
    for condition in output_conditions:
        payload.extend(CONDITION.pack(*condition))
    for vertex in output_vertices:
        payload.extend(VERTEX.pack(*vertex))
    payload.extend(texture)
    return bytes(payload)


def extract_all(reference_path: Path, game_root: Path, output_dir: Path):
    reference = reference_path.read_bytes()
    tables = parse_reference(reference)
    layouts = json.loads((game_root / "data/layouts/layouts.json").read_text())["layouts"]
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = output_dir / "manifest.json"
    previous_ids = set()
    if manifest_path.is_file():
        previous = json.loads(manifest_path.read_text())
        if previous.get("format") not in ("VBL2", "VBL3", "VBL4") or not isinstance(previous.get("layout_ids"), list):
            raise ValueError("output directory has an unrecognized manifest; refusing to overwrite it")
        for layout_id in previous["layout_ids"]:
            if not isinstance(layout_id, int) or layout_id <= 0 or layout_id > 0xFFFF:
                raise ValueError("previous manifest contains an invalid layout ID")
            previous_ids.add(layout_id)
    unowned = [p.name for p in output_dir.glob("*.vbl")
               if not p.stem.isdigit() or int(p.stem) not in previous_ids]
    if unowned:
        raise ValueError("output directory contains untracked .vbl files: " + ", ".join(unowned[:8]))
    accepted, skipped, bytes_written = 0, 0, 0
    accepted_ids = []
    unresolved_ids = set()
    for layout_id, layout in enumerate(layouts, start=1):
        map_path = game_root / layout["blockdata_filepath"]
        if not map_path.is_file():
            skipped += 1
            unresolved_ids.add(layout_id)
            continue
        map_data = map_path.read_bytes()
        expected_map_bytes = layout["width"] * layout["height"] * 2
        # Some map snapshots are rounded to a 32-bit boundary after their
        # 16-bit metatile array. Trim only the resulting zero halfword; any
        # other length/content mismatch stays unresolved and fails closed.
        if (expected_map_bytes % 4 == 2 and len(map_data) == expected_map_bytes + 2
                and map_data[expected_map_bytes:] == b"\x00\x00"):
            map_data = map_data[:expected_map_bytes]
        if len(map_data) != expected_map_bytes:
            skipped += 1
            unresolved_ids.add(layout_id)
            continue
        payload = extract_layout(reference, tables, layout_id, layout, map_data)
        if payload is None:
            skipped += 1
            continue
        target = output_dir / f"{layout_id:03d}.vbl"
        target.write_bytes(payload)
        accepted += 1
        bytes_written += len(payload)
        accepted_ids.append(layout_id)
    # If a previous asset's source-map snapshot is temporarily unavailable,
    # retain it. The runtime compares its embedded map against the live map and
    # will fail closed if it no longer applies. A present, valid snapshot that
    # now has no eligible geometry still makes the old asset stale.
    preserved_ids = {
        layout_id for layout_id in (previous_ids & unresolved_ids)
        if (output_dir / f"{layout_id:03d}.vbl").is_file()
    }
    if not accepted and not preserved_ids:
        raise ValueError("no eligible static layout models found")
    accepted_set = set(accepted_ids)
    for old_id in previous_ids - accepted_set - preserved_ids:
        old_path = output_dir / f"{old_id:03d}.vbl"
        if old_path.is_file():
            old_path.unlink()
    layout_ids = sorted(accepted_set | preserved_ids)
    bytes_written += sum((output_dir / f"{layout_id:03d}.vbl").stat().st_size
                         for layout_id in preserved_ids)
    manifest = {"format": "VBL4", "layouts": len(layout_ids),
                "generated": accepted, "preserved": len(preserved_ids),
                "layout_ids": layout_ids, "skipped": skipped, "bytes": bytes_written,
                "source": "local game layouts plus local VXB9 reference asset; private test data"}
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference_buildings", type=Path)
    parser.add_argument("game_root", type=Path, help="local game source snapshot root")
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()
    result = extract_all(args.reference_buildings, args.game_root, args.output_dir)
    print("Wrote private voxel model subsets:", result)


if __name__ == "__main__":
    main()
