#!/usr/bin/env python3
"""Assertions about the render fixtures that belong outside the binary.

The C harness owns everything it can see from the inside — the golden hashes,
that rendering mutated nothing, that the local hour reaches the screen. What is
left for here is the shape of what actually landed on disk, which the process
that wrote it is the worst possible witness for:

  1. **Exactly the fixtures §10.5 specifies**, by name, not merely by count. A
     count alone passes if a fixture is written twice under different names.
  2. **A valid P6 header and the exact payload length.** A truncated write is
     the failure a hash of the in-memory buffer cannot see.
  3. **Not blank.** At least 16 distinct colours per frame, independently of
     the harness's own check, because "the renderer drew a flat rectangle" is
     the bug a golden freezes rather than catches.
  4. **Distinctness**, by byte comparison rather than by hash: adjacent health
     states, the four species and the four pots must all differ. These are the
     ways a renderer looks plausible and is wrong.
"""
from __future__ import annotations

import pathlib
import sys

WIDTH, HEIGHT = 480, 270
SPECIES = 4
STAGES = 4
HEALTH = 6
POTS = 4
SCENES = ("sunny-sill", "bright-corner", "study-desk", "plain-studio",
          "kitchen-shelf", "steamy-bath")


def expected_names() -> set[str]:
    names = {f"plant-{s}-{g}-{h}"
             for s in range(SPECIES) for g in range(STAGES)
             for h in range(HEALTH)}
    names |= {f"pot-{p}" for p in range(POTS)}
    names |= {f"background-{scene}" for scene in SCENES}
    names.add("background-off")
    names |= {"localhour-utc", "localhour-plus540"}
    names |= {f"screen-{s}" for s in
              ("calendar", "chooser", "journal", "away", "damaged",
               "replant")}
    return names


def read_ppm(path: pathlib.Path) -> bytes:
    """Return the pixel payload, asserting a canonical P6 header."""
    data = path.read_bytes()
    if not data.startswith(b"P6\n"):
        raise AssertionError(f"{path.name}: not a P6 PPM")
    header_end = 0
    fields = []
    cursor = 3
    while len(fields) < 3:
        end = data.index(b"\n", cursor)
        fields.extend(data[cursor:end].split())
        cursor = end + 1
    header_end = cursor
    width, height, maximum = (int(f) for f in fields[:3])
    if (width, height) != (WIDTH, HEIGHT):
        raise AssertionError(
            f"{path.name}: {width}x{height}, expected {WIDTH}x{HEIGHT}")
    if maximum != 255:
        raise AssertionError(f"{path.name}: maxval {maximum}, expected 255")
    payload = data[header_end:]
    expected = WIDTH * HEIGHT * 3
    if len(payload) != expected:
        raise AssertionError(
            f"{path.name}: {len(payload)} payload bytes, expected {expected}")
    return payload


def distinct_colours(payload: bytes, cap: int = 64) -> int:
    seen = set()
    for offset in range(0, len(payload) - 2, 3):
        seen.add(payload[offset:offset + 3])
        if len(seen) >= cap:
            break
    return len(seen)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: test_render.py <fixture-dir>", file=sys.stderr)
        return 2
    directory = pathlib.Path(argv[1])
    failures: list[str] = []

    found = {p.stem for p in directory.glob("*.ppm")}
    expected = expected_names()
    for missing in sorted(expected - found):
        failures.append(f"missing fixture: {missing}")
    for extra in sorted(found - expected):
        failures.append(f"unexpected fixture: {extra}")

    payloads: dict[str, bytes] = {}
    for name in sorted(found & expected):
        try:
            payload = read_ppm(directory / f"{name}.ppm")
        except AssertionError as error:
            failures.append(str(error))
            continue
        payloads[name] = payload
        if distinct_colours(payload) < 16:
            failures.append(f"{name}: fewer than 16 distinct colours (blank?)")

    def differ(a: str, b: str, why: str) -> None:
        if a in payloads and b in payloads and payloads[a] == payloads[b]:
            failures.append(f"{why}: {a} and {b} are byte-identical")

    for stage in range(STAGES):
        for health in range(1, HEALTH):
            differ(f"plant-0-{stage}-{health - 1}", f"plant-0-{stage}-{health}",
                   "adjacent health states must differ")
    for species in range(1, SPECIES):
        differ(f"plant-{species - 1}-2-1", f"plant-{species}-2-1",
               "species must differ")
    for pot in range(1, POTS):
        differ(f"pot-{pot - 1}", f"pot-{pot}", "pots must differ")
    differ("localhour-utc", "localhour-plus540",
           "gate 8b: the local hour must reach the screen")

    if failures:
        for failure in failures:
            print(f"test-render: {failure}", file=sys.stderr)
        print(f"test-render: FAILED ({len(failures)} problems)",
              file=sys.stderr)
        return 1
    print(f"test-render: PASS ({len(payloads)} fixtures, {WIDTH}x{HEIGHT}, "
          f"headers and payloads exact, all distinctness checks held)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
