#!/usr/bin/env python3
"""The door a FOREGROUND plate enters through.

`prepare_plate.py` is the door for backdrops and writes colour type 2 -- RGB,
no alpha -- which is correct for something drawn behind everything and fatal
for something drawn in front. A front layer without alpha is an opaque
rectangle over the whole scene: layer 7 would hide the plant, the pot and the
room in one go.

So this is a separate tool rather than a flag, because the two differ in more
than a switch:

  - **The key becomes alpha.** The generator paints the non-subject area flat
    magenta (#ff00ff), which is keyed out here. Despill follows, because a
    magenta-lit edge pixel left in place is a pink fringe on the object
    nearest the camera -- the most visible place in the frame to get it wrong.
  - **The fit is the same 1920x1080 the loader demands**, centre-fit and
    cropped, so a front plate lines up with the backdrop it sits over. The
    loader gates both on exact size.
  - **The output is colour type 6.** That is the whole point.

Everything else -- the 1536 px source floor, box-average downscale, the refusal
to enlarge -- matches `prepare_plate.py`, and for the same reasons.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import sys
import zlib

TARGET_W = 1920
TARGET_H = 1080
MIN_SOURCE_W = 1536
KEY = (255, 0, 255)
# How close to the key a pixel must be to become transparent. Generous,
# because the generator antialiases its own edges into the key and a tight
# threshold leaves a magenta halo that reads as a pink outline in game.
KEY_TOLERANCE = 70


def _unfilter(raw: bytes, width: int, height: int, stride: int,
              bpp: int) -> bytearray:
    out = bytearray(height * stride)
    previous = bytearray(stride)
    position = 0
    for row in range(height):
        filter_type = raw[position]
        position += 1
        line = bytearray(raw[position:position + stride])
        position += stride
        if filter_type:
            for index in range(stride):
                left = line[index - bpp] if index >= bpp else 0
                up = previous[index]
                upleft = previous[index - bpp] if index >= bpp else 0
                if filter_type == 1:
                    line[index] = (line[index] + left) & 0xFF
                elif filter_type == 2:
                    line[index] = (line[index] + up) & 0xFF
                elif filter_type == 3:
                    line[index] = (line[index] + ((left + up) >> 1)) & 0xFF
                elif filter_type == 4:
                    p = left + up - upleft
                    pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                    pred = left if (pa <= pb and pa <= pc) else (
                        up if pb <= pc else upleft)
                    line[index] = (line[index] + pred) & 0xFF
                else:
                    raise SystemExit("prepare-front: bad filter")
        out[row * stride:(row + 1) * stride] = line
        previous = line
    return out


def _read_palette(path, width, height, depth, palette, trns, compressed):
    """Decode colour type 3 at bit depth 1, 2, 4 or 8."""
    if depth not in (1, 2, 4, 8):
        raise SystemExit(f"prepare-front: palette depth {depth}: {path}")
    stride = (width * depth + 7) // 8
    raw = zlib.decompress(bytes(compressed))
    rows = _unfilter(raw, width, height, stride, 1)
    out = bytearray(width * height * 4)
    per_byte = 8 // depth
    mask = (1 << depth) - 1
    for y in range(height):
        base = y * stride
        dst = y * width * 4
        for x in range(width):
            byte = rows[base + x // per_byte]
            shift = 8 - depth * (x % per_byte + 1)
            entry = (byte >> shift) & mask
            i = entry * 3
            out[dst + x * 4] = palette[i] if i < len(palette) else 0
            out[dst + x * 4 + 1] = palette[i + 1] if i + 1 < len(palette) else 0
            out[dst + x * 4 + 2] = palette[i + 2] if i + 2 < len(palette) else 0
            out[dst + x * 4 + 3] = (trns[entry] if entry < len(trns) else 255)
    return width, height, out


def read_png(path: pathlib.Path) -> tuple[int, int, bytearray]:
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise SystemExit(f"prepare-front: not a PNG: {path}")
    offset = 8
    width = height = depth = colour = None
    compressed = bytearray()
    palette = b""
    trns = b""
    while offset + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        offset += 8
        payload = data[offset:offset + length]
        offset += length + 4
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB",
                                                         payload[:10])
        elif kind == b"PLTE":
            palette = payload
        elif kind == b"tRNS":
            trns = payload
        elif kind == b"IDAT":
            compressed += payload
        elif kind == b"IEND":
            break
    # Palette images are decoded rather than refused: the image tool returns
    # colour type 3 for flat art often enough that rejecting it just means
    # regenerating until it happens to pick a different encoding.
    if colour == 3:
        if not palette:
            raise SystemExit(f"prepare-front: paletted with no PLTE: {path}")
        return _read_palette(path, width, height, depth, palette, trns,
                             compressed)
    if colour not in (2, 6) or depth != 8:
        raise SystemExit(
            f"prepare-front: need 8-bit RGB, RGBA or palette, got depth "
            f"{depth} type {colour}: {path}")
    channels = 3 if colour == 2 else 4
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
                upleft = (previous[index - channels]
                          if index >= channels else 0)
                if filter_type == 1:
                    line[index] = (line[index] + left) & 0xFF
                elif filter_type == 2:
                    line[index] = (line[index] + up) & 0xFF
                elif filter_type == 3:
                    line[index] = (line[index] + ((left + up) >> 1)) & 0xFF
                elif filter_type == 4:
                    p = left + up - upleft
                    pa, pb, pc = abs(p - left), abs(p - up), abs(p - upleft)
                    pred = left if (pa <= pb and pa <= pc) else (
                        up if pb <= pc else upleft)
                    line[index] = (line[index] + pred) & 0xFF
                else:
                    raise SystemExit(f"prepare-front: bad filter: {path}")
        base = row * width * 4
        for x in range(width):
            src = x * channels
            dst = base + x * 4
            out[dst] = line[src]
            out[dst + 1] = line[src + 1]
            out[dst + 2] = line[src + 2]
            out[dst + 3] = line[src + 3] if channels == 4 else 255
        previous = line
    return width, height, out


def key_and_despill(width: int, height: int, pixels: bytearray) -> int:
    """Magenta becomes transparent; what survives gets its green restored."""
    keyed = 0
    for index in range(0, width * height * 4, 4):
        r, g, b = pixels[index], pixels[index + 1], pixels[index + 2]
        if r >= KEY[0] - KEY_TOLERANCE and b >= KEY[2] - KEY_TOLERANCE \
                and g <= KEY[1] + KEY_TOLERANCE:
            pixels[index + 3] = 0
            keyed += 1
            continue
        # Despill: a surviving pixel that still leans magenta had the key
        # bleed into it. Pull the red and blue back toward the green they
        # would have had, which is what stops a pink rim on the near edge.
        if r > g and b > g:
            excess = min(r - g, b - g)
            pixels[index] = r - excess // 2
            pixels[index + 2] = b - excess // 2
    return keyed


def centre_fit(width: int, height: int, pixels: bytearray) -> bytearray:
    scale = max(TARGET_W / width, TARGET_H / height)
    scaled_w = max(1, int(width * scale + 0.5))
    scaled_h = max(1, int(height * scale + 0.5))
    offset_x = (scaled_w - TARGET_W) // 2
    offset_y = (scaled_h - TARGET_H) // 2

    out = bytearray(TARGET_W * TARGET_H * 4)
    step_x = width / scaled_w
    step_y = height / scaled_h
    for y in range(TARGET_H):
        sy0 = (y + offset_y) * step_y
        sy1 = max(sy0 + 1.0, (y + offset_y + 1) * step_y)
        y0, y1 = int(sy0), min(int(sy1), height)
        if y1 <= y0:
            y1 = min(y0 + 1, height)
        for x in range(TARGET_W):
            sx0 = (x + offset_x) * step_x
            sx1 = max(sx0 + 1.0, (x + offset_x + 1) * step_x)
            x0, x1 = int(sx0), min(int(sx1), width)
            if x1 <= x0:
                x1 = min(x0 + 1, width)
            r = g = b = a = n = 0
            for sy in range(y0, y1):
                row = sy * width
                for sx in range(x0, x1):
                    i = (row + sx) * 4
                    alpha = pixels[i + 3]
                    # Weight colour by alpha. Averaging transparent pixels'
                    # colour into an edge is what haloes a keyed sprite.
                    r += pixels[i] * alpha
                    g += pixels[i + 1] * alpha
                    b += pixels[i + 2] * alpha
                    a += alpha
                    n += 1
            dst = (y * TARGET_W + x) * 4
            if n and a:
                out[dst] = r // a
                out[dst + 1] = g // a
                out[dst + 2] = b // a
                out[dst + 3] = a // n
    return out


def write_png(path: pathlib.Path, pixels: bytearray) -> bytes:
    raw = bytearray()
    stride = TARGET_W * 4
    for row in range(TARGET_H):
        raw.append(0)
        raw += pixels[row * stride:(row + 1) * stride]

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    data = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", TARGET_W, TARGET_H, 8, 6,
                                         0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return data


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("destination", type=pathlib.Path)
    args = parser.parse_args(argv[1:])

    width, height, pixels = read_png(args.source)
    if width < MIN_SOURCE_W:
        print(f"prepare-front: {args.source.name} is {width} px wide; the "
              f"floor is {MIN_SOURCE_W}. Regenerate it rather than enlarging "
              f"it.", file=sys.stderr)
        return 1
    keyed = key_and_despill(width, height, pixels)
    if keyed == 0:
        print(f"prepare-front: {args.source.name} has no magenta to key. A "
              f"front plate with no transparency is an opaque rectangle over "
              f"the whole scene -- refusing.", file=sys.stderr)
        return 1
    if keyed == width * height:
        print(f"prepare-front: {args.source.name} keyed away completely.",
              file=sys.stderr)
        return 1
    fitted = centre_fit(width, height, pixels)
    data = write_png(args.destination, fitted)
    opaque = sum(1 for i in range(3, len(fitted), 4) if fitted[i] >= 128)
    print(f"prepare-front: {args.source.name} {width}x{height} -> "
          f"{args.destination.name} {TARGET_W}x{TARGET_H} RGBA "
          f"{100.0 * opaque / (TARGET_W * TARGET_H):.1f}% opaque "
          f"{hashlib.sha256(data).hexdigest()[:16]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
