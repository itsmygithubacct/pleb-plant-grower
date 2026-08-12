#!/usr/bin/env python3
"""Derive the runtime atlases from the accepted chroma masters.

    source/<species>-<health>-chroma.png   1672 x 941, four cells of 418 x 941
              |   key removed, despilled, edge-contracted, cell resampled
              v
    atlases/<species>.png                  640 x 960, 4 columns x 6 rows of 160

Growth stage picks the column, health picks the row (ART_BIBLE.md §4.1), which
is the order pg_render indexes them in.

Three parts of this are worth explaining, because each is a place where the
obvious implementation looks right and is wrong:

**Despill.** Keying on colour distance alone leaves a rim of the key colour on
every edge pixel, because the generator antialiased the subject against it. A
sprite with a magenta fringe looks fine at 1672 px and looks like a mistake at
160. So any pixel whose key channel exceeds the others is pulled back toward
the neutral of its remaining channels before alpha is decided.

**Edge contract.** After keying, the outermost ring of surviving pixels is
still part-key. Alpha is therefore taken to zero on pixels whose neighbourhood
is mostly transparent, which trims that ring by one.

**Box-average downsample with alpha weighting.** Averaging RGB across a cell
without weighting by alpha drags transparent pixels' colour into the edge, so
a leaf against a keyed background acquires a halo of the key even after
despill. Weighting by alpha is the whole difference.

stdlib only, integer arithmetic, byte-reproducible: two runs from one set of
masters produce identical atlases.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import sys
import zlib

CELL = 160
COLUMNS = 4
HEALTH_ROWS = ("thriving", "healthy", "thirsty", "distressed", "critical",
               "dead")
SPECIES = ("pothos", "snake-plant", "peace-lily", "calathea")

# Distance in squared RGB below which a pixel is the background.
KEY_TOLERANCE = 92 * 92


def read_png(path: pathlib.Path) -> tuple[int, int, int, bytearray]:
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise SystemExit(f"prepare-graphics: not a PNG: {path}")
    offset = 8
    width = height = depth = color_type = None
    compressed = bytearray()
    while offset + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        offset += 8
        payload = data[offset:offset + length]
        offset += length + 4
        if kind == b"IHDR":
            width, height, depth, color_type = struct.unpack(">IIBB",
                                                             payload[:10])
        elif kind == b"IDAT":
            compressed += payload
        elif kind == b"IEND":
            break
    if depth != 8 or color_type not in (2, 6):
        raise SystemExit(
            f"prepare-graphics: need 8-bit RGB or RGBA, got depth {depth} "
            f"type {color_type}: {path}. Down-convert the master first.")
    channels = 3 if color_type == 2 else 4
    stride = width * channels
    raw = zlib.decompress(bytes(compressed))
    out = bytearray(width * height * 4)
    previous = bytearray(stride)
    position = 0
    for row in range(height):
        filter_type = raw[position]
        position += 1
        line = bytearray(raw[position:position + stride])
        position += stride
        if filter_type:
            for index in range(stride):
                left = line[index - channels] if index >= channels else 0
                up = previous[index]
                upleft = previous[index - channels] if index >= channels else 0
                if filter_type == 1:
                    line[index] = (line[index] + left) & 0xFF
                elif filter_type == 2:
                    line[index] = (line[index] + up) & 0xFF
                elif filter_type == 3:
                    line[index] = (line[index] + ((left + up) >> 1)) & 0xFF
                elif filter_type == 4:
                    p = left + up - upleft
                    pa, pb, pc = abs(p-left), abs(p-up), abs(p-upleft)
                    pred = left if (pa <= pb and pa <= pc) else (
                        up if pb <= pc else upleft)
                    line[index] = (line[index] + pred) & 0xFF
                else:
                    raise SystemExit(f"prepare-graphics: bad filter: {path}")
        base = row * width * 4
        for x in range(width):
            src = x * channels
            dst = base + x * 4
            out[dst] = line[src]
            out[dst + 1] = line[src + 1]
            out[dst + 2] = line[src + 2]
            out[dst + 3] = line[src + 3] if channels == 4 else 255
        previous = line
    return width, height, 4, out


def key_and_despill(width: int, height: int, pixels: bytearray,
                    key: tuple[int, int, int]) -> None:
    kr, kg, kb = key
    magenta = kr > 200 and kb > 200 and kg < 80
    for index in range(0, len(pixels), 4):
        r, g, b = pixels[index], pixels[index + 1], pixels[index + 2]
        dr, dg, db = r - kr, g - kg, b - kb
        if dr*dr + dg*dg + db*db <= KEY_TOLERANCE:
            pixels[index + 3] = 0
            continue
        # Despill: pull the key's channels back toward the others, so an
        # antialiased edge does not keep a rim of the key colour.
        if magenta:
            limit = max(g, (r + b) // 2 if False else g)
            if r > limit and b > limit:
                pixels[index] = limit
                pixels[index + 2] = limit
        else:                                    # cyan key
            limit = r
            if g > limit and b > limit:
                pixels[index + 1] = limit
                pixels[index + 2] = limit


def contract_edges(width: int, height: int, pixels: bytearray) -> None:
    """Zero alpha where the neighbourhood is mostly transparent.

    The ring of pixels immediately inside a key edge is part background even
    after despill; trimming one pixel is what stops a sprite carrying a faint
    outline of its own key.
    """
    doomed = []
    for y in range(height):
        for x in range(width):
            index = (y * width + x) * 4
            if pixels[index + 3] == 0:
                continue
            clear = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    nx, ny = x + dx, y + dy
                    if not (0 <= nx < width and 0 <= ny < height):
                        clear += 1
                    elif pixels[(ny * width + nx) * 4 + 3] == 0:
                        clear += 1
            if clear >= 5:
                doomed.append(index)
    for index in doomed:
        pixels[index + 3] = 0


def segment_columns(width: int, height: int, pixels: bytearray,
                    count: int, y0: int = 0,
                    y1: int | None = None) -> list[tuple[int, int]]:
    """Split a strip into `count` subjects by the empty columns between them.

    Slicing at fixed quarters is the obvious implementation and it is wrong:
    the generator places four plants that lean and overhang, so a quarter
    boundary cuts through a neighbour and every cell ends up with a sliver of
    the next plant glued to its edge. Segmenting on columns that contain no
    opaque pixel finds the real gaps.

    Falls back to equal slices only if the strip does not have the expected
    number of gaps, and says so, because a silent fallback here would put the
    sliver back.
    """
    occupied = bytearray(width)
    for y in range(y0, height if y1 is None else y1):
        row = y * width
        for x in range(width):
            if pixels[(row + x) * 4 + 3]:
                occupied[x] = 1

    runs = []
    start = None
    for x in range(width):
        if occupied[x]:
            if start is None:
                start = x
        elif start is not None:
            runs.append((start, x - start))
            start = None
    if start is not None:
        runs.append((start, width - start))

    if len(runs) == count:
        return runs
    # Merge the smallest neighbours until the count matches: a plant split by
    # an internal gap (a gap between leaves) shows up as an extra run.
    while len(runs) > count:
        gaps = [(runs[i + 1][0] - (runs[i][0] + runs[i][1]), i)
                for i in range(len(runs) - 1)]
        _, index = min(gaps)
        left, right = runs[index], runs[index + 1]
        merged = (left[0], right[0] + right[1] - left[0])
        runs = runs[:index] + [merged] + runs[index + 2:]
    if len(runs) != count:
        print(f"prepare-graphics: found {len(runs)} subjects, expected "
              f"{count}; falling back to equal slices", file=sys.stderr)
        step = width // count
        return [(i * step, step) for i in range(count)]
    return runs


def alpha_bounds(src_w: int, pixels: bytearray, x0: int, y0: int,
                 cw: int, chh: int) -> tuple[int, int, int, int] | None:
    """The tight box around the opaque pixels of one source cell."""
    left, top, right, bottom = cw, chh, -1, -1
    for y in range(chh):
        row = (y0 + y) * src_w
        for x in range(cw):
            if pixels[(row + x0 + x) * 4 + 3]:
                if x < left: left = x
                if x > right: right = x
                if y < top: top = y
                if y > bottom: bottom = y
    if right < 0:
        return None
    return x0 + left, y0 + top, right - left + 1, bottom - top + 1


def resample_panel(src_w: int, pixels: bytearray, x0: int, y0: int,
                   cw: int, chh: int, out_w: int, out_h: int) -> bytearray:
    """Box-average a whole source panel into out_w x out_h.

    The counterpart to resample_cell, and the difference is the entire point:
    resample_cell fits the subject's alpha bounding box into the cell, which
    is right for an object that sits on a ground line and fatal for one that
    has to continue past its own edge. A vine segment is tiled head to tail,
    so its stem must leave the top edge exactly where the next copy's stem
    enters the bottom. Cropping to the bbox insets it and the chain breaks --
    which is what happened, and it made every one of the sixteen cells empty
    at both seams.
    """
    out = bytearray(out_w * out_h * 4)
    for y in range(out_h):
        sy0 = y0 + y * chh // out_h
        sy1 = max(sy0 + 1, y0 + (y + 1) * chh // out_h)
        for x in range(out_w):
            sx0 = x0 + x * cw // out_w
            sx1 = max(sx0 + 1, x0 + (x + 1) * cw // out_w)
            r = g = b = a = n = 0
            for sy in range(sy0, sy1):
                row = sy * src_w
                for sx in range(sx0, sx1):
                    i = (row + sx) * 4
                    alpha = pixels[i + 3]
                    # weight colour by alpha so transparent pixels do not
                    # drag a halo into the average (the leaf-halo bug)
                    r += pixels[i] * alpha
                    g += pixels[i + 1] * alpha
                    b += pixels[i + 2] * alpha
                    a += alpha
                    n += 1
            o = (y * out_w + x) * 4
            if n and a:
                out[o] = r // a
                out[o + 1] = g // a
                out[o + 2] = b // a
                out[o + 3] = a // n
    return out


def resample_cell(src_w: int, pixels: bytearray, x0: int, y0: int,
                  cw: int, chh: int, size: int) -> bytearray:
    """Box-average one source cell into size x size, preserving aspect.

    The source cell is 418 x 941 and the destination is square, so squashing
    the whole quarter into it would compress every plant vertically by more
    than half -- which looked plausible in isolation and is simply the wrong
    shape. Instead the subject's own alpha bounding box is fitted into the
    cell at ART_BIBLE's 70 % of cell height, centred horizontally and anchored
    to the bottom, because a plant stands on a ground line rather than
    floating in the middle of its cell.
    """
    out = bytearray(size * size * 4)
    bounds = alpha_bounds(src_w, pixels, x0, y0, cw, chh)
    if bounds is None:
        return out
    bx, by, bw, bh = bounds

    target_h = max(1, (size * 70) // 100)
    scale = min(target_h / bh, (size - 8) / bw)
    draw_w = max(1, int(bw * scale))
    draw_h = max(1, int(bh * scale))
    pad_x = (size - draw_w) // 2
    pad_y = size - draw_h - 4          # bottom-anchored, four pixels of floor

    step_x = bw / draw_w
    step_y = bh / draw_h
    x0, y0, cw, chh = bx, by, bw, bh
    size_x, size_y = draw_w, draw_h
    for y in range(size_y):
        sy0 = y0 + int(y * step_y)
        sy1 = y0 + max(int(y * step_y) + 1, int((y + 1) * step_y))
        for x in range(size_x):
            sx0 = x0 + int(x * step_x)
            sx1 = x0 + max(int(x * step_x) + 1, int((x + 1) * step_x))
            r = g = b = a = weight = count = 0
            for sy in range(sy0, sy1):
                row = sy * src_w
                for sx in range(sx0, sx1):
                    index = (row + sx) * 4
                    alpha = pixels[index + 3]
                    a += alpha
                    count += 1
                    if alpha:
                        r += pixels[index] * alpha
                        g += pixels[index + 1] * alpha
                        b += pixels[index + 2] * alpha
                        weight += alpha
            if not count:
                continue
            dst = ((pad_y + y) * size + pad_x + x) * 4
            if not (0 <= pad_y + y < size and 0 <= pad_x + x < size):
                continue
            if weight:
                out[dst] = r // weight
                out[dst + 1] = g // weight
                out[dst + 2] = b // weight
            # Binary-ish alpha: the runtime treats below 8 as transparent, and
            # a soft edge at 160 px reads as dirt rather than as softness.
            mean = a // count
            out[dst + 3] = 255 if mean >= 128 else 0
    return out


def write_png(path: pathlib.Path, width: int, height: int,
              pixels: bytearray) -> bytes:
    raw = bytearray()
    stride = width * 4
    for row in range(height):
        raw.append(0)
        raw += pixels[row * stride:(row + 1) * stride]

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    data = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6,
                                         0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return data


def build_atlas(species: str, source: pathlib.Path,
                out: pathlib.Path) -> tuple[pathlib.Path, str]:
    atlas = bytearray(CELL * COLUMNS * CELL * len(HEALTH_ROWS) * 4)
    atlas_w = CELL * COLUMNS
    key = (0, 255, 255) if species == "calathea" else (255, 0, 255)

    for row_index, health in enumerate(HEALTH_ROWS):
        master = source / f"{species}-{health}-chroma.png"
        if not master.is_file():
            raise SystemExit(f"prepare-graphics: missing master {master}")
        width, height, _channels, pixels = read_png(master)
        key_and_despill(width, height, pixels, key)
        contract_edges(width, height, pixels)
        spans = segment_columns(width, height, pixels, COLUMNS)
        for column, (span_x, span_w) in enumerate(spans):
            cell = resample_cell(width, pixels, span_x, 0,
                                 span_w, height, CELL)
            for y in range(CELL):
                dst = ((row_index * CELL + y) * atlas_w + column * CELL) * 4
                src = y * CELL * 4
                atlas[dst:dst + CELL * 4] = cell[src:src + CELL * 4]

    target = out / f"{species}.png"
    data = write_png(target, atlas_w, CELL * len(HEALTH_ROWS), atlas)
    return target, hashlib.sha256(data).hexdigest()


# The three non-plant sheets. Each arrives as a 4x4 grid on a 1254x1254 canvas
# and becomes a runtime atlas with its own cell size, which is why they cannot
# share the plant path: a pot cell is 176x96 and a tool cell is 64x64, and both
# are wrong for the other.
SHEETS = {
    "pots":   {"columns": 4, "rows": 4, "cell": (176, 96),  "key": (255, 0, 255)},
    "tools":  {"columns": 4, "rows": 4, "cell": (64, 64),   "key": (255, 0, 255)},
    "decals": {"columns": 4, "rows": 4, "cell": (64, 64),   "key": (255, 0, 255)},
    # The peace lily's spathe is drawn apart from its body so a bloom can be
    # laid over any growth stage without reauthoring the plant; four stages
    # across, open and spent down.
    "spathe": {"columns": 4, "rows": 2, "cell": (64, 64),   "key": (255, 0, 255)},
    # Pothos trailers. Eight lengths across, two rows so a pot can carry a
    # long and a short vine without both sides matching.
    "vines":  {"columns": 8, "rows": 2, "cell": (48, 48),   "key": (255, 0, 255),
               "tile": True, "tips": 2},
}


# calathea-night is a plant strip with one row instead of six: the same four
# growth stages, all at the nyctinastic fold, swapped in by the realtime clock
# after dusk. It uses the strip path -- gap segmentation, alpha bounds, bottom
# anchoring -- because it is a strip; only the row count differs.
SPECIAL_STRIPS = {
    "calathea-night": {"key": (0, 255, 255)},
}


def build_special(name: str, source: pathlib.Path,
                  out: pathlib.Path) -> tuple[pathlib.Path, str]:
    spec = SPECIAL_STRIPS[name]
    master = source / f"{name}-chroma.png"
    if not master.is_file():
        raise SystemExit(f"prepare-graphics: missing master {master}")

    width, height, _channels, pixels = read_png(master)
    key_and_despill(width, height, pixels, spec["key"])
    contract_edges(width, height, pixels)

    atlas_w = CELL * COLUMNS
    atlas = bytearray(atlas_w * CELL * 4)
    spans = segment_columns(width, height, pixels, COLUMNS)
    for column, (span_x, span_w) in enumerate(spans):
        cell = resample_cell(width, pixels, span_x, 0, span_w, height, CELL)
        for y in range(CELL):
            dst = (y * atlas_w + column * CELL) * 4
            src = y * CELL * 4
            atlas[dst:dst + CELL * 4] = cell[src:src + CELL * 4]

    target = out / f"{name}.png"
    data = write_png(target, atlas_w, CELL, atlas)
    return target, hashlib.sha256(data).hexdigest()


def _rebuild_one(job: tuple) -> tuple:
    """Rebuild one atlas in a worker process and return its digest.

    Module level and picklable because it has to cross a process boundary.
    """
    kind, name, source, scratch = job
    target = pathlib.Path(scratch) / kind / name
    target.mkdir(parents=True, exist_ok=True)
    try:
        if kind == "sheet":
            _, digest = build_sheet(name, pathlib.Path(source), target)
        else:
            _, digest = build_atlas(name, pathlib.Path(source), target)
        return (kind, name, digest, None)
    except SystemExit as error:
        return (kind, name, None, str(error))


def check_tiling(name: str, atlas: pathlib.Path) -> list[str]:
    """Verify a tiling sheet actually chains, seam to seam.

    ART_BIBLE states the vine segments are "tiled downward by the renderer so
    vine length is a free realtime progress bar", and nothing enforced it. The
    assembler was insetting every cell to its alpha bounding box, so all
    sixteen cells were empty at both seams and no two segments could ever
    join -- and the sheet still passed every check there was, because the only
    question anyone asked was whether the bytes matched the master.

    Two properties, both of which the broken sheet failed:
      - a segment carries opaque pixels on its top AND bottom edge, so the
        stem leaves the cell rather than stopping inside it;
      - consecutive segments overlap in x at the shared seam, so the stem does
        not jump sideways where they meet.

    Tip cells are exempt from the second property and inverted on the first:
    a tip must enter from the top and must NOT continue past the bottom, which
    is what makes it a tip.
    """
    spec = SHEETS[name]
    if not spec.get("tile"):
        return []
    cell_w, cell_h = spec["cell"]
    columns, rows = spec["columns"], spec["rows"]
    tips = spec.get("tips", 2)
    segments = columns - tips
    width, height, _channels, pixels = read_png(atlas)
    if (width, height) != (cell_w * columns, cell_h * rows):
        return [f"{name}: atlas is {width}x{height}, expected "
                f"{cell_w * columns}x{cell_h * rows}"]

    def edge(cx: int, cy: int, ry: int) -> list[int]:
        base = (cy * cell_h + ry) * width
        return [x for x in range(cell_w)
                if pixels[(base + cx * cell_w + x) * 4 + 3] >= 128]

    problems = []
    for cy in range(rows):
        for cx in range(columns):
            top, bottom = edge(cx, cy, 0), edge(cx, cy, cell_h - 1)
            if cx >= segments:
                if not top:
                    problems.append(
                        f"{name}: tip r{cy}c{cx} has no stem entering its top "
                        f"edge, so it cannot attach to a segment")
                if bottom:
                    problems.append(
                        f"{name}: tip r{cy}c{cx} runs off its bottom edge; a "
                        f"tip has to end the vine")
                continue
            if not top or not bottom:
                where = "top" if not top else "bottom"
                problems.append(
                    f"{name}: segment r{cy}c{cx} is empty at its {where} "
                    f"edge, so tiling it leaves a gap")
                continue
            if cx + 1 < segments:
                nxt = edge(cx + 1, cy, 0)
                if nxt and not (min(nxt) <= max(bottom)
                                and min(bottom) <= max(nxt)):
                    problems.append(
                        f"{name}: segment r{cy}c{cx} leaves at x="
                        f"{min(bottom)}-{max(bottom)} but r{cy}c{cx + 1} "
                        f"enters at x={min(nxt)}-{max(nxt)}; the stem jumps")
    return problems


def build_sheet(name: str, source: pathlib.Path,
                out: pathlib.Path) -> tuple[pathlib.Path, str]:
    spec = SHEETS[name]
    cell_w, cell_h = spec["cell"]
    columns, rows = spec["columns"], spec["rows"]
    master = source / f"{name}-chroma.png"
    if not master.is_file():
        raise SystemExit(f"prepare-graphics: missing master {master}")

    width, height, _channels, pixels = read_png(master)
    key_and_despill(width, height, pixels, spec["key"])
    if not spec.get("tile"):
        # contract_edges trims the ring just inside a key edge, which is what
        # stops a sprite carrying an outline of its own key. On a tiling sheet
        # that ring IS the seam, so trimming it guarantees the break this
        # mode exists to avoid.
        contract_edges(width, height, pixels)

    atlas_w, atlas_h = cell_w * columns, cell_h * rows
    atlas = bytearray(atlas_w * atlas_h * 4)
    src_w, src_h = width // columns, height // rows
    # Fixed quarters were assumed safe here on the grounds that these sheets
    # are grids of isolated objects with real gutters. That is not reliably
    # true: the spathe master puts its fourth object across x=940, which is
    # exactly where the fourth quarter cuts, so cell 3 kept a sliver of its
    # neighbour and cell 4 lost its subject. Find the real gaps per row band,
    # the same way the plant strips do.
    #
    # Tiling sheets are exempt by construction: their segments run edge to
    # edge on purpose, so there are no gaps to find and quarters are correct.
    row_spans = None
    if not spec.get("tile"):
        row_spans = [segment_columns(width, height, pixels, columns,
                                     row * src_h, (row + 1) * src_h)
                     for row in range(rows)]
    for row in range(rows):
        for column in range(columns):
            # Fixed quarters here, unlike the plant strips: these sheets are
            # authored as a grid of isolated objects with real gutters, so a
            # cell boundary does not cut through a neighbour. The plant strips
            # needed gap-finding because four plants lean into each other.
            if row_spans is not None:
                start, span = row_spans[row][column]
            else:
                start, span = column * src_w, src_w
            if spec.get("tile"):
                panel = resample_panel(width, pixels,
                                       start, row * src_h,
                                       span, src_h, cell_w, cell_h)
                for y in range(cell_h):
                    dst = ((row * cell_h + y) * atlas_w
                           + column * cell_w) * 4
                    src = y * cell_w * 4
                    atlas[dst:dst + cell_w * 4] = \
                        panel[src:src + cell_w * 4]
                continue
            cell = resample_cell(width, pixels, start, row * src_h,
                                 span, src_h, max(cell_w, cell_h))
            size = max(cell_w, cell_h)
            for y in range(cell_h):
                sy = y + (size - cell_h) // 2
                if sy < 0 or sy >= size:
                    continue
                for x in range(cell_w):
                    sx = x + (size - cell_w) // 2
                    if sx < 0 or sx >= size:
                        continue
                    dst = ((row * cell_h + y) * atlas_w
                           + column * cell_w + x) * 4
                    src = (sy * size + sx) * 4
                    atlas[dst:dst + 4] = cell[src:src + 4]

    target = out / f"{name}.png"
    data = write_png(target, atlas_w, atlas_h, atlas)
    return target, hashlib.sha256(data).hexdigest()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path,
                        default=pathlib.Path("assets/graphics/source"))
    parser.add_argument("--out", type=pathlib.Path,
                        default=pathlib.Path("assets/graphics/atlases"))
    parser.add_argument("--species", action="append")
    parser.add_argument("--special", action="append",
                        choices=sorted(SPECIAL_STRIPS),
                        help="calathea-night")
    parser.add_argument("--sheet", action="append",
                        choices=sorted(SHEETS), help="pots, tools or decals")
    parser.add_argument("--check", action="store_true",
                        help="verify the built atlases match their masters "
                             "rather than rewriting them")
    args = parser.parse_args(argv[1:])

    root = pathlib.Path(__file__).resolve().parent.parent
    source = (root / args.source).resolve()
    out = (root / args.out).resolve()

    # --check verifies what the MANIFEST declares, not a hard-coded list.
    # It did the latter, which meant `make test-assets` announced that it was
    # validating the manifest while never opening it: a manifest could name
    # one atlas, or twelve, or none of the four, and the check would demand
    # exactly the same four either way.
    wanted = args.species
    if wanted is None and args.check:
        manifest_path = root / "assets/graphics/manifest.json"
        declared = []
        declared_sheets = []
        if manifest_path.is_file():
            try:
                entries = json.loads(manifest_path.read_text()).get("entries",
                                                                    [])
            except json.JSONDecodeError as error:
                print(f"prepare-graphics: manifest is not valid JSON: {error}",
                      file=sys.stderr)
                return 1
            for entry in entries:
                stem = pathlib.PurePosixPath(str(entry.get("path", ""))).stem
                if stem in SPECIES and stem not in declared:
                    declared.append(stem)
                elif stem in SHEETS and stem not in declared_sheets:
                    declared_sheets.append(stem)
        wanted = declared
        sheets_wanted = declared_sheets
        if not wanted and not sheets_wanted:
            print("prepare-graphics: the manifest declares no plant atlases, "
                  "nothing to check")
            return 0
    if wanted is None:
        wanted = list(SPECIES)

    # --check was accepted and ignored, so it rebuilt the atlases and reported
    # success -- a check that performs the action it is meant to verify, and
    # can therefore never fail. It builds into a temporary directory now and
    # compares, leaving the tree untouched.
    if args.check:
        import concurrent.futures
        import os
        import tempfile
        failures = []
        sheets_to_check = list(args.sheet or sheets_wanted)
        # Rebuilding every atlas from its masters is the strong form of this
        # check -- it catches an atlas edited by hand, which comparing the
        # manifest hash to the file cannot. It is also minutes of pure-Python
        # resampling, and a gate slow enough to skip is a gate nobody runs. The
        # work is one independent rebuild per atlas, so it goes wide instead of
        # being traded away for a weaker check.
        jobs = ([("sheet", s, str(source), None) for s in sheets_to_check]
                + [("species", s, str(source), None) for s in wanted])
        with tempfile.TemporaryDirectory() as scratch:
            jobs = [(k, n, s, scratch) for k, n, s, _ in jobs]
            workers = max(1, min(len(jobs), (os.cpu_count() or 2) - 1))
            digests = {}
            with concurrent.futures.ProcessPoolExecutor(workers) as pool:
                for kind, name, digest, error in pool.map(_rebuild_one, jobs):
                    digests[(kind, name)] = (digest, error)
            for sheet in sheets_to_check:
                built = out / f"{sheet}.png"
                if not built.is_file():
                    failures.append(f"{sheet}: no atlas; run make graphics")
                    continue
                digest, error = digests[("sheet", sheet)]
                if error:
                    failures.append(f"{sheet}: {error}")
                    continue
                have = hashlib.sha256(built.read_bytes()).hexdigest()
                if have != digest:
                    failures.append(
                        f"{sheet}: atlas drifted from its master\n"
                        f"    committed {have[:16]}\n"
                        f"    rebuilt   {digest[:16]}")
                # Matching the master is not enough for a tiling sheet: the
                # broken vines atlas matched its master exactly and still
                # could not tile, because the fault was in the assembler that
                # both sides of the comparison shared.
                failures.extend(check_tiling(sheet, built))
            for species in wanted:
                built = out / f"{species}.png"
                if not built.is_file():
                    failures.append(f"{species}: no atlas; run make graphics")
                    continue
                digest, error = digests[("species", species)]
                if error:
                    failures.append(f"{species}: {error}")
                    continue
                have = hashlib.sha256(built.read_bytes()).hexdigest()
                if have != digest:
                    failures.append(
                        f"{species}: atlas drifted from its masters\n"
                        f"    committed {have[:16]}\n"
                        f"    rebuilt   {digest[:16]}")
        if failures:
            for failure in failures:
                print(f"prepare-graphics: {failure}", file=sys.stderr)
            return 1
        total = len(wanted) + len(args.sheet or sheets_wanted)
        print(f"prepare-graphics: PASS ({total} atlas"
              f"{'es' if total != 1 else ''} match their masters)")
        return 0

    results = {}
    for special in (args.special or []):
        target, digest = build_special(special, source, out)
        results[special] = digest
        print(f"prepare-graphics: {target.name} {CELL*COLUMNS}x{CELL} "
              f"{digest[:16]}")
    for sheet in (args.sheet or []):
        target, digest = build_sheet(sheet, source, out)
        results[sheet] = digest
        spec = SHEETS[sheet]
        print(f"prepare-graphics: {target.name} "
              f"{spec['cell'][0]*spec['columns']}x"
              f"{spec['cell'][1]*spec['rows']} {digest[:16]}")
    if (args.sheet or args.special) and not args.species:
        print(json.dumps(results, indent=2))
        return 0

    for species in wanted:
        if species not in SPECIES:
            raise SystemExit(f"prepare-graphics: unknown species {species}")
        target, digest = build_atlas(species, source, out)
        results[species] = digest
        print(f"prepare-graphics: {target.name} "
              f"{CELL*COLUMNS}x{CELL*len(HEALTH_ROWS)} {digest[:16]}")
    print(json.dumps(results, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
