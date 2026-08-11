#!/usr/bin/env python3
"""The procedural atlases — ART_BIBLE.md §4.7.

Three atlases are authored here rather than generated, and the reason is in the
bible: model-generated art at 8–16 px is mush. A nine-slice frame needs its
corners to be exactly the corners, a 16 px glyph needs every pixel to carry
meaning, and a 32 px particle needs a silhouette that survives being drawn
forty times a frame at alpha 0.3. None of that survives a diffusion model.

The second reason is drift. The UI skin shares its palette with the renderer,
so an atlas cell and a drawn rectangle cannot disagree — the same argument
kilix-fantasy's native_ui_skin.py makes, and it is why the palette below is
stated once and used by both.

stdlib only, integer only, byte-reproducible: `make art` twice gives identical
files, and `--check` proves it.
"""
from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import sys
import zlib

# The shared eight. Stated here and mirrored in src/pg_render.c's stage
# palette; a cell and a rectangle drawn beside it must be the same colour.
PALETTE = {
    "ink":        (0x12, 0x10, 0x0E, 0xFF),
    "panel":      (0x1B, 0x22, 0x2C, 0xF2),
    "border":     (0x4A, 0x5A, 0x63, 0xFF),
    "text":       (0xF2, 0xE9, 0xD8, 0xFF),
    "muted":      (0x8A, 0x92, 0x99, 0xFF),
    "accent":     (0x6A, 0xB7, 0xFF, 0xFF),
    "leaf":       (0x5F, 0xA8, 0x4E, 0xFF),
    "warm":       (0xFF, 0xC6, 0x5A, 0xFF),
}
CLEAR = (0, 0, 0, 0)


class Image:
    """A tiny RGBA canvas. No PIL: the toolchain must not need one to build."""

    def __init__(self, width: int, height: int) -> None:
        self.width = width
        self.height = height
        self.pixels = [CLEAR] * (width * height)

    def put(self, x: int, y: int, rgba) -> None:
        if 0 <= x < self.width and 0 <= y < self.height:
            self.pixels[y * self.width + x] = rgba

    def rect(self, x: int, y: int, w: int, h: int, rgba) -> None:
        for row in range(y, y + h):
            for column in range(x, x + w):
                self.put(column, row, rgba)

    def frame(self, x: int, y: int, w: int, h: int, rgba) -> None:
        for column in range(x, x + w):
            self.put(column, y, rgba)
            self.put(column, y + h - 1, rgba)
        for row in range(y, y + h):
            self.put(x, row, rgba)
            self.put(x + w - 1, row, rgba)

    def disc(self, cx: int, cy: int, radius: int, rgba) -> None:
        for row in range(cy - radius, cy + radius + 1):
            for column in range(cx - radius, cx + radius + 1):
                dx = column - cx
                dy = row - cy
                if dx * dx + dy * dy <= radius * radius:
                    self.put(column, row, rgba)

    def to_png(self) -> bytes:
        raw = bytearray()
        for row in range(self.height):
            raw.append(0)
            for column in range(self.width):
                raw.extend(self.pixels[row * self.width + column])

        def chunk(kind: bytes, data: bytes) -> bytes:
            return (struct.pack(">I", len(data)) + kind + data +
                    struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

        header = struct.pack(">IIBBBBB", self.width, self.height, 8, 6, 0, 0, 0)
        return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
                chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
                chunk(b"IEND", b""))


# ---------------------------------------------------------------------------
# ui-skin.png — 8x4 cells of 8x8, the nine-slice frame
# ---------------------------------------------------------------------------

def build_ui_skin() -> Image:
    image = Image(64, 32)
    panel = PALETTE["panel"]
    border = PALETTE["border"]
    accent = PALETTE["accent"]

    def cell(index: int) -> tuple[int, int]:
        return (index % 8) * 8, (index // 8) * 8

    # Row 0, cells 0..8: the nine slice, in ki_td_nine_slice order --
    # corners, edges, centre. The centre is the only translucent cell; a
    # translucent border would show the seam where two panels overlap.
    layouts = [
        ("corner", 0, 0), ("edge_h", 1, 0), ("corner", 2, 0),
        ("edge_v", 3, 0), ("centre", 4, 0), ("edge_v", 5, 0),
        ("corner", 6, 0), ("edge_h", 7, 0), ("corner", 0, 1),
    ]
    for order, (kind, _, _) in enumerate(layouts):
        x, y = cell(order)
        if kind == "centre":
            image.rect(x, y, 8, 8, panel)
        elif kind == "corner":
            image.rect(x, y, 8, 8, panel)
            image.frame(x, y, 8, 8, border)
        elif kind == "edge_h":
            image.rect(x, y, 8, 8, panel)
            for column in range(8):
                image.put(x + column, y, border)
        else:
            image.rect(x, y, 8, 8, panel)
            for row in range(8):
                image.put(x, y + row, border)

    # Cell 9: the focus cursor, a filled chevron.
    x, y = cell(9)
    for row in range(8):
        width = 4 - abs(row - 4)
        if width > 0:
            image.rect(x + 1, y + row, width, 1, accent)
    return image


# ---------------------------------------------------------------------------
# glyphs.png — 8x2 cells of 16x16
# ---------------------------------------------------------------------------

GLYPH_ORDER = ["droplet", "sun", "humidity", "thermometer", "leaf", "calendar",
               "warning", "skull", "clock", "sparkle", "flower", "scissors",
               "lamp", "rotate", "salt", "root"]


def build_glyphs() -> Image:
    image = Image(128, 32)
    ink = PALETTE["text"]
    accent = PALETTE["accent"]
    warm = PALETTE["warm"]
    leaf = PALETTE["leaf"]

    def origin(index: int) -> tuple[int, int]:
        return (index % 8) * 16, (index // 8) * 16

    for index, name in enumerate(GLYPH_ORDER):
        ox, oy = origin(index)
        if name == "droplet":
            for row in range(4, 13):
                half = (row - 3) // 2 if row < 9 else (13 - row)
                image.rect(ox + 8 - half, oy + row, half * 2 + 1, 1, accent)
        elif name == "sun":
            image.disc(ox + 8, oy + 8, 3, warm)
            for angle in ((0, -6), (0, 6), (-6, 0), (6, 0),
                          (-4, -4), (4, -4), (-4, 4), (4, 4)):
                image.put(ox + 8 + angle[0], oy + 8 + angle[1], warm)
        elif name == "humidity":
            for offset in (-4, 0, 4):
                for row in range(5, 12):
                    image.put(ox + 8 + offset, oy + row, accent)
                image.put(ox + 8 + offset - 1, oy + 11, accent)
                image.put(ox + 8 + offset + 1, oy + 11, accent)
        elif name == "thermometer":
            image.rect(ox + 7, oy + 3, 2, 8, ink)
            image.disc(ox + 8, oy + 12, 2, warm)
        elif name == "leaf":
            # An asymmetric blade with a midrib, not a diamond: at 16 px the
            # midrib is what makes it read as a leaf rather than a gem.
            for row in range(3, 14):
                span = min(row - 2, 14 - row) + 1
                image.rect(ox + 8 - span, oy + row, span * 2, 1, leaf)
            for row in range(4, 13):
                image.put(ox + 8, oy + row, (0x2E, 0x5E, 0x28, 0xFF))
            image.put(ox + 8, oy + 13, (0x2E, 0x5E, 0x28, 0xFF))
        elif name == "calendar":
            image.frame(ox + 3, oy + 4, 11, 10, ink)
            image.rect(ox + 3, oy + 4, 11, 3, ink)
            for row in (9, 12):
                for column in (5, 8, 11):
                    image.put(ox + column, oy + row, ink)
        elif name == "warning":
            for row in range(3, 13):
                half = (row - 2) // 2
                image.rect(ox + 8 - half, oy + row, half * 2 + 1, 1, warm)
            image.rect(ox + 8, oy + 7, 1, 3, PALETTE["ink"])
            image.put(ox + 8, oy + 11, PALETTE["ink"])
        elif name == "skull":
            # Cranium, two sockets, a nasal notch and a jaw. Sockets must be
            # two pixels wide or the face reads as a potato with dents.
            image.rect(ox + 4, oy + 3, 9, 8, ink)
            image.rect(ox + 3, oy + 4, 11, 6, ink)
            image.rect(ox + 5, oy + 11, 7, 2, ink)
            image.rect(ox + 5, oy + 6, 2, 3, PALETTE["ink"])
            image.rect(ox + 10, oy + 6, 2, 3, PALETTE["ink"])
            image.put(ox + 8, oy + 9, PALETTE["ink"])
            for column in (6, 8, 10):
                image.put(ox + column, oy + 12, PALETTE["ink"])
        elif name == "clock":
            image.frame(ox + 3, oy + 3, 11, 11, ink)
            image.rect(ox + 8, oy + 6, 1, 3, ink)
            image.rect(ox + 8, oy + 8, 3, 1, ink)
        elif name == "sparkle":
            image.rect(ox + 8, oy + 3, 1, 11, warm)
            image.rect(ox + 3, oy + 8, 11, 1, warm)
            for d in (-2, 2):
                image.put(ox + 8 + d, oy + 8 + d, warm)
                image.put(ox + 8 + d, oy + 8 - d, warm)
        elif name == "flower":
            for angle in ((0, -3), (0, 3), (-3, 0), (3, 0)):
                image.disc(ox + 8 + angle[0], oy + 8 + angle[1], 2,
                           PALETTE["text"])
            image.disc(ox + 8, oy + 8, 2, warm)
        elif name == "scissors":
            for row in range(4, 9):
                image.put(ox + 5 + row - 4, oy + row, ink)
                image.put(ox + 11 - row + 4, oy + row, ink)
            image.disc(ox + 5, oy + 11, 2, ink)
            image.disc(ox + 11, oy + 11, 2, ink)
        elif name == "lamp":
            for row in range(4, 9):
                half = row - 3
                image.rect(ox + 8 - half, oy + row, half * 2, 1, warm)
            image.rect(ox + 7, oy + 9, 2, 4, ink)
        elif name == "rotate":
            for row in range(4, 13):
                for column in range(3, 14):
                    dx = column - 8
                    dy = row - 8
                    distance = dx * dx + dy * dy
                    if 16 <= distance <= 25 and not (dy > 0 and dx > 2):
                        image.put(ox + column, oy + row, accent)
            image.rect(ox + 10, oy + 9, 3, 1, accent)
            image.rect(ox + 12, oy + 7, 1, 3, accent)
        elif name == "salt":
            for point in ((5, 6), (9, 5), (7, 9), (11, 10), (6, 11)):
                image.disc(ox + point[0], oy + point[1], 1, PALETTE["text"])
        elif name == "root":
            image.rect(ox + 8, oy + 3, 1, 5, PALETTE["muted"])
            for row in range(8, 14):
                spread = row - 7
                image.put(ox + 8 - spread, oy + row, PALETTE["muted"])
                image.put(ox + 8 + spread, oy + row, PALETTE["muted"])
    return image


# ---------------------------------------------------------------------------
# particles.png — 8x2 cells of 32x32
# ---------------------------------------------------------------------------

PARTICLE_ORDER = ["pour-a", "pour-b", "pour-c", "droplet", "splash",
                  "mist-a", "mist-b", "mist-c", "granule-a", "granule-b",
                  "sparkle-a", "sparkle-b", "sparkle-c", "gnat-a", "gnat-b",
                  "dust-mote"]


def build_particles() -> Image:
    image = Image(256, 64)
    water = PALETTE["accent"]
    warm = PALETTE["warm"]
    soil = (0x3B, 0x2F, 0x24, 0xFF)

    def origin(index: int) -> tuple[int, int]:
        return (index % 8) * 32, (index // 8) * 32

    for index, name in enumerate(PARTICLE_ORDER):
        ox, oy = origin(index)
        centre = (ox + 16, oy + 16)
        if name.startswith("pour"):
            width = 3 + PARTICLE_ORDER.index(name) % 3
            for row in range(4, 28):
                image.rect(centre[0] - width // 2, oy + row, width, 1, water)
        elif name == "droplet":
            for row in range(12, 21):
                half = (row - 11) // 2 if row < 17 else (21 - row)
                image.rect(centre[0] - half, oy + row, half * 2 + 1, 1, water)
        elif name == "splash":
            for point in ((-8, 2), (-4, -2), (0, -4), (4, -2), (8, 2)):
                image.disc(centre[0] + point[0], centre[1] + point[1], 2,
                           water)
        elif name.startswith("mist"):
            step = 3 + PARTICLE_ORDER.index(name) % 3
            for row in range(8, 24, step):
                for column in range(6, 26, step):
                    image.put(ox + column, oy + row, (*water[:3], 0x60))
        elif name.startswith("granule"):
            offset = 0 if name.endswith("a") else 3
            for point in ((-6, -4), (-1, 2), (5, -2), (2, 6), (-4, 7)):
                image.disc(centre[0] + point[0] + offset,
                           centre[1] + point[1], 1, warm)
        elif name.startswith("sparkle"):
            arm = 6 + PARTICLE_ORDER.index(name) % 3 * 2
            image.rect(centre[0], centre[1] - arm, 1, arm * 2, warm)
            image.rect(centre[0] - arm, centre[1], arm * 2, 1, warm)
        elif name.startswith("gnat"):
            lift = 0 if name.endswith("a") else 4
            image.put(centre[0], centre[1] - lift, PALETTE["ink"])
            image.put(centre[0] + 1, centre[1] - lift, PALETTE["ink"])
        else:  # dust-mote
            image.disc(centre[0], centre[1], 1, (*soil[:3], 0x80))
    return image


TARGETS = {
    "assets/graphics/atlases/ui-skin.png": build_ui_skin,
    "assets/graphics/atlases/glyphs.png": build_glyphs,
    "assets/graphics/atlases/particles.png": build_particles,
}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="regenerate and compare rather than write")
    args = parser.parse_args(argv[1:])

    root = pathlib.Path(__file__).resolve().parent.parent
    failures = []
    for relative, builder in sorted(TARGETS.items()):
        target = root / relative
        data = builder().to_png()
        digest = hashlib.sha256(data).hexdigest()[:16]
        if args.check:
            if not target.exists():
                failures.append(f"{relative}: missing; run make art")
            elif hashlib.sha256(target.read_bytes()).hexdigest()[:16] != digest:
                failures.append(f"{relative}: drifted from its generator")
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            print(f"gen-art: {relative} ({len(data)} bytes, {digest})")

    if failures:
        for failure in failures:
            print(f"art: {failure}", file=sys.stderr)
        return 1
    if args.check:
        print(f"art: PASS ({len(TARGETS)} procedural atlases match their "
              f"generators)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
