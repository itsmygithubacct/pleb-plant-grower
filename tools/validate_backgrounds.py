#!/usr/bin/env python3
"""Verify the backdrop plates the game will actually try to load.

This tool did not exist, and `test-backgrounds` was written as::

    if [ -x tools/validate_backgrounds.py ]; then ... else
        echo "test-backgrounds: no backgrounds yet"; fi

which keys the result off whether *this file* is executable rather than off
whether any backgrounds are present. It printed "no backgrounds yet" and
passed on an empty tree, and went on printing exactly the same thing after six
plates landed -- a gate whose output is independent of the thing it checks
cannot fail, and this one never had.

What is actually checked:

  - **The scene list comes from the C**, not from a list kept here. The
    renderer resolves ``backgrounds/<id>/<id>.png`` for each id compiled into
    `pg_graphics.c`; if this file kept its own copy, the two would drift and
    the check would pass while the game loaded nothing.
  - **Exactly 1920x1080.** `pg_graphics.c` refuses any other size outright
    rather than scaling, so a plate that is one pixel off is not a slightly
    wrong backdrop, it is an absent one. The failure is silent in game -- you
    get the empty room -- which is the kind of thing a build should catch.
  - **8-bit RGB or RGBA, non-interlaced**, because that is what the loader
    decodes.
  - **A front layer, if present, matches the same rules.** ``<id>-front.png``
    is optional; when it exists it is drawn as the occluder in layer 7.

Exit status is 1 on any failure, and every failure names the file and what was
wrong with it.
"""

from __future__ import annotations

import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GRAPHICS = ROOT / "src/pg_graphics.c"
BACKGROUNDS = ROOT / "assets/graphics/backgrounds"
PLATE_W = 1920
PLATE_H = 1080


def scene_ids() -> list[str]:
    """The scene ids the renderer compiles in, read from the C itself."""
    source = GRAPHICS.read_text()
    # the table is a run of brace-wrapped string literals in the scene desc
    block = re.search(r"pg_scene_desc\s+\w*scenes?\w*\[\][^{]*\{(.*?)\n\};",
                      source, re.S)
    if block is None:
        # fall back to any quoted id that has a directory beside it, so a
        # refactor of the table shape degrades to a weaker check rather than
        # a false pass
        found = sorted({m for m in re.findall(r'"([a-z][a-z0-9-]{3,})"',
                                              source)
                        if (BACKGROUNDS / m).is_dir()})
        if not found:
            raise SystemExit(
                "validate-backgrounds: could not find the scene table in "
                f"{GRAPHICS.name}; refusing to report success")
        return found
    return re.findall(r'"([a-z][a-z0-9-]+)"', block.group(1))


def png_header(path: pathlib.Path) -> tuple[int, int, int, int, int]:
    data = path.read_bytes()[:33]
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("not a PNG")
    length, kind = struct.unpack(">I4s", data[8:16])
    if kind != b"IHDR":
        raise ValueError("first chunk is not IHDR")
    width, height, depth, colour, _comp, _filt, interlace = struct.unpack(
        ">IIBBBBB", data[16:29])
    return width, height, depth, colour, interlace


def check_plate(path: pathlib.Path, label: str) -> list[str]:
    problems = []
    try:
        width, height, depth, colour, interlace = png_header(path)
    except (ValueError, OSError, struct.error) as error:
        return [f"{label}: unreadable ({error})"]
    if (width, height) != (PLATE_W, PLATE_H):
        problems.append(
            f"{label}: {width}x{height}, must be exactly {PLATE_W}x{PLATE_H} "
            f"-- the loader refuses any other size, so this plate would not "
            f"appear at all")
    if depth != 8 or colour not in (2, 6):
        problems.append(
            f"{label}: depth {depth} colour-type {colour}; need 8-bit RGB or "
            f"RGBA")
    if interlace:
        problems.append(f"{label}: interlaced; the loader reads sequential")
    return problems


def main() -> int:
    ids = scene_ids()
    if not ids:
        print("validate-backgrounds: no scenes declared in pg_graphics.c",
              file=sys.stderr)
        return 1

    failures = []
    checked = 0
    fronts = 0
    for scene in ids:
        plate = BACKGROUNDS / scene / f"{scene}.png"
        if not plate.is_file():
            failures.append(
                f"{scene}: no plate at {plate.relative_to(ROOT)}; the scene "
                f"is selectable in game and would draw an empty room")
            continue
        failures.extend(check_plate(plate, scene))
        checked += 1
        front = BACKGROUNDS / scene / f"{scene}-front.png"
        if front.is_file():
            failures.extend(check_plate(front, f"{scene}-front"))
            fronts += 1

    if failures:
        for failure in failures:
            print(f"validate-backgrounds: {failure}", file=sys.stderr)
        return 1
    print(f"test-backgrounds: PASS ({checked} plates at "
          f"{PLATE_W}x{PLATE_H}, {fronts} front layers, scene ids read from "
          f"{GRAPHICS.name})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
