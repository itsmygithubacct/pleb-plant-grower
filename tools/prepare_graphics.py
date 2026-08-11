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
                    count: int) -> list[tuple[int, int]]:
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
    for y in range(height):
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
}


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
    contract_edges(width, height, pixels)

    atlas_w, atlas_h = cell_w * columns, cell_h * rows
    atlas = bytearray(atlas_w * atlas_h * 4)
    src_w, src_h = width // columns, height // rows
    for row in range(rows):
        for column in range(columns):
            # Fixed quarters here, unlike the plant strips: these sheets are
            # authored as a grid of isolated objects with real gutters, so a
            # cell boundary does not cut through a neighbour. The plant strips
            # needed gap-finding because four plants lean into each other.
            cell = resample_cell(width, pixels, column * src_w, row * src_h,
                                 src_w, src_h, max(cell_w, cell_h))
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
        import tempfile
        failures = []
        with tempfile.TemporaryDirectory() as scratch:
            for sheet in (args.sheet or sheets_wanted if args.check else []):
                built = out / f"{sheet}.png"
                if not built.is_file():
                    failures.append(f"{sheet}: no atlas; run make graphics")
                    continue
                _, digest = build_sheet(sheet, source, pathlib.Path(scratch))
                have = hashlib.sha256(built.read_bytes()).hexdigest()
                if have != digest:
                    failures.append(
                        f"{sheet}: atlas drifted from its master\n"
                        f"    committed {have[:16]}\n"
                        f"    rebuilt   {digest[:16]}")
            for species in wanted:
                built = out / f"{species}.png"
                if not built.is_file():
                    failures.append(f"{species}: no atlas; run make graphics")
                    continue
                try:
                    _, digest = build_atlas(species, source,
                                            pathlib.Path(scratch))
                except SystemExit as error:
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
    for sheet in (args.sheet or []):
        target, digest = build_sheet(sheet, source, out)
        results[sheet] = digest
        spec = SHEETS[sheet]
        print(f"prepare-graphics: {target.name} "
              f"{spec['cell'][0]*spec['columns']}x"
              f"{spec['cell'][1]*spec['rows']} {digest[:16]}")
    if args.sheet and not args.species:
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
