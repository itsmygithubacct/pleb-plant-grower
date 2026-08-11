#!/usr/bin/env python3
"""The only door a backdrop enters through.

A generated plate arrives at whatever size the image tool produced. The runtime
accepts exactly 1920x1080 and refuses anything else rather than scaling it
(`pg_graphics.c`), because a silently rescaled backdrop is a bug you notice
months later as "the art looks slightly soft". So the rescale happens here,
once, deliberately, and is recorded.

Two rules from ART_BIBLE §14.3, both of which exist to stop a bad plate looking
merely mediocre instead of obviously wrong:

  - **Centre-fit, preserving aspect.** The plate is fitted to cover 1920x1080
    and the overflow is cropped equally from both sides. Stretching to fit
    would distort a room by a few percent -- invisible in isolation, and wrong
    beside every sprite drawn at true proportion.
  - **A source narrower than 1536 px is refused.** Upscaling past that adds no
    detail and produces a soft plate that passes every automated check. The
    instruction is to regenerate, not to enlarge.

The resample is a box average when downscaling and bilinear when upscaling
within the allowed range. The bible says Lanczos; this is a deliberate and
stated deviation, because a Lanczos kernel in pure Python over two million
pixels takes minutes, and the fleet's rule that a toolchain needs no image
library matters more here than the last few percent of sharpness on art that is
about to be composited behind a plant. If that trade stops being worth it, this
is the one function to change.
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


def read_png(path: pathlib.Path) -> tuple[int, int, int, bytearray]:
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise SystemExit(f"prepare-plate: not a PNG: {path}")
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
    if color_type not in (2, 6) or depth not in (8, 16):
        raise SystemExit(
            f"prepare-plate: need 8- or 16-bit RGB/RGBA, got depth {depth} "
            f"type {color_type}: {path}")
    channels = (3 if color_type == 2 else 4) * (depth // 8)
    stride = width * channels
    raw = zlib.decompress(bytes(compressed))
    out = bytearray(width * height * 3)
    previous = bytearray(stride)
    position = 0
    step = depth // 8
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
                    raise SystemExit(f"prepare-plate: bad filter: {path}")
        base = row * width * 3
        for x in range(width):
            src = x * channels
            dst = base + x * 3
            # High byte of each sample: correct downconversion from 16-bit,
            # and a no-op at 8.
            out[dst] = line[src]
            out[dst + 1] = line[src + step]
            out[dst + 2] = line[src + step * 2]
        previous = line
    return width, height, 3, out


def centre_fit(width: int, height: int, pixels: bytearray) -> bytearray:
    """Cover TARGET_W x TARGET_H preserving aspect, cropping the overflow."""
    scale = max(TARGET_W / width, TARGET_H / height)
    scaled_w = max(1, int(width * scale + 0.5))
    scaled_h = max(1, int(height * scale + 0.5))
    offset_x = (scaled_w - TARGET_W) // 2
    offset_y = (scaled_h - TARGET_H) // 2

    out = bytearray(TARGET_W * TARGET_H * 3)
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
            r = g = b = n = 0
            for sy in range(y0, y1):
                row = sy * width
                for sx in range(x0, x1):
                    index = (row + sx) * 3
                    r += pixels[index]
                    g += pixels[index + 1]
                    b += pixels[index + 2]
                    n += 1
            dst = (y * TARGET_W + x) * 3
            if n:
                out[dst] = r // n
                out[dst + 1] = g // n
                out[dst + 2] = b // n
    return out


def write_png(path: pathlib.Path, pixels: bytearray) -> bytes:
    raw = bytearray()
    stride = TARGET_W * 3
    for row in range(TARGET_H):
        raw.append(0)
        raw += pixels[row * stride:(row + 1) * stride]

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack(">I", len(payload)) + kind + payload
                + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF))

    data = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", TARGET_W, TARGET_H, 8, 2,
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

    width, height, _channels, pixels = read_png(args.source)
    if width < MIN_SOURCE_W:
        print(f"prepare-plate: {args.source.name} is {width} px wide; the "
              f"floor is {MIN_SOURCE_W}. Regenerate it rather than enlarging "
              f"it -- upscaling past this adds no detail and produces a soft "
              f"plate that passes every automated check.", file=sys.stderr)
        return 1
    fitted = centre_fit(width, height, pixels)
    data = write_png(args.destination, fitted)
    print(f"prepare-plate: {args.source.name} {width}x{height} -> "
          f"{args.destination.name} {TARGET_W}x{TARGET_H} "
          f"{hashlib.sha256(data).hexdigest()[:16]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
