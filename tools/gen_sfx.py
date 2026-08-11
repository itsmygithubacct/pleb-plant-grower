#!/usr/bin/env python3
"""Generate the SFX bank, byte-reproducibly.

Every sample is computed with **integer arithmetic only**. There is no `import
math` in this file and there is no floating point in any signal path, which is
deliberate and is the kilix-pong lesson written down: a bank generated through
libm transcendentals is reproducible on the machine that made it and nowhere
else, because `sin` is not required to be correctly rounded and glibc's result
has changed between versions. A bank whose hashes drift cannot be checked into
a repository and cannot gate a build.

So the oscillators here are triangle, square and a fixed-point sine built from
one integer table, and the noise is an LCG with a written-down constant. All of
it is exactly reproducible on any machine with any Python 3.

Output is pcm-mixer's strict format and nothing else: **PCM, mono, 16-bit,
44100 Hz**. The mixer rejects anything else rather than resampling, which is
the right call — a silently resampled cue is a bug you hear months later.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import struct
import sys

SAMPLE_RATE = 44100
CHANNELS = 1
BITS = 16

# ---------------------------------------------------------------------------
# integer signal primitives
# ---------------------------------------------------------------------------

SINE_TABLE_BITS = 8
SINE_TABLE_SIZE = 1 << SINE_TABLE_BITS
SINE_SCALE = 1 << 15


def build_sine_table() -> list[int]:
    """A quarter-wave sine in Q15, from the integer recurrence

        s[n+1] = s[n] + (c[n] >> k),  c[n+1] = c[n] - (s[n+1] >> k)

    which is the standard fixed-point resonator. It converges to a sine to
    within a few LSBs and, being pure integer, gives the same table on every
    machine forever. That is the only property that matters here: this is not
    a precision problem, it is a reproducibility problem.
    """
    table = [0] * SINE_TABLE_SIZE
    # k chosen so the oscillator completes exactly one cycle over the table.
    # 2*pi/SIZE in Q15 -> the shift below approximates it closely enough that
    # the endpoint error is under one LSB after normalisation.
    sine = 0
    cosine = SINE_SCALE
    step = 804  # round(2*pi/256 * 32768) = 804
    for index in range(SINE_TABLE_SIZE):
        table[index] = sine
        sine += (cosine * step) >> 15
        cosine -= (sine * step) >> 15
    peak = max(abs(value) for value in table) or 1
    return [(value * SINE_SCALE) // peak for value in table]


SINE = build_sine_table()


def sine_at(phase: int) -> int:
    """Q15 sine of a Q16 phase accumulator."""
    return SINE[(phase >> 8) & (SINE_TABLE_SIZE - 1)]


def triangle_at(phase: int) -> int:
    """Q15 triangle. Cheap, integer, and rich enough for UI blips."""
    position = (phase >> 8) & 0xFF
    if position < 128:
        return (position * 2 - 128) * 256
    return ((255 - position) * 2 - 128) * 256


class Noise:
    """A named LCG. The constants are Numerical Recipes' and are written here
    rather than imported so the bank cannot change when a library does."""

    def __init__(self, seed: int) -> None:
        self.state = seed & 0xFFFFFFFF

    def next(self) -> int:
        self.state = (self.state * 1664525 + 1013904223) & 0xFFFFFFFF
        return ((self.state >> 16) & 0xFFFF) - 0x8000


def envelope(index: int, count: int, attack: int, release: int) -> int:
    """Q15 attack/release envelope, linear and integer."""
    if index < attack:
        return (index * SINE_SCALE) // max(attack, 1)
    remaining = count - index
    if remaining < release:
        return (remaining * SINE_SCALE) // max(release, 1)
    return SINE_SCALE


def clamp16(value: int) -> int:
    if value > 32767:
        return 32767
    if value < -32768:
        return -32768
    return value


def seconds(count: float) -> int:
    """Sample count for a duration given in whole milliseconds."""
    return int(SAMPLE_RATE * count)


# ---------------------------------------------------------------------------
# the cues
# ---------------------------------------------------------------------------

def cue_water(seed: int) -> list[int]:
    """Pouring: filtered noise with a rising body, then a trailing trickle."""
    noise = Noise(seed)
    count = seconds(0.55)
    out = []
    low = 0
    for index in range(count):
        sample = noise.next()
        low += (sample - low) >> 3            # one-pole low pass
        body = low
        gain = envelope(index, count, seconds(0.02), seconds(0.25))
        # a slow rise in brightness as the pot fills
        tilt = SINE_SCALE - (index * (SINE_SCALE // 3)) // count
        out.append(clamp16((body * gain >> 15) * tilt >> 15))
    return out


def cue_mist(seed: int) -> list[int]:
    """A short hiss. Deliberately unsatisfying: the game never recommends it."""
    noise = Noise(seed)
    count = seconds(0.35)
    out = []
    high = 0
    previous = 0
    for index in range(count):
        sample = noise.next()
        high = sample - previous                # one-pole high pass
        previous = sample
        gain = envelope(index, count, seconds(0.03), seconds(0.2))
        out.append(clamp16((high >> 1) * gain >> 15))
    return out


def cue_feed(seed: int) -> list[int]:
    """Granular: sparse noise grains over a soft body."""
    noise = Noise(seed)
    count = seconds(0.4)
    out = []
    for index in range(count):
        grain = noise.next() if (index % 37) < 6 else 0
        gain = envelope(index, count, seconds(0.01), seconds(0.2))
        out.append(clamp16((grain >> 2) * gain >> 15))
    return out


def cue_snip(seed: int) -> list[int]:
    """Two quick metallic ticks: scissors close, then release."""
    noise = Noise(seed)
    count = seconds(0.18)
    out = []
    phase = 0
    for index in range(count):
        phase += (2600 << 16) // SAMPLE_RATE
        tick = 1 if index < seconds(0.02) or (
            seconds(0.05) <= index < seconds(0.065)) else 0
        body = (sine_at(phase) >> 2) + (noise.next() >> 3)
        gain = envelope(index, count, 32, seconds(0.09))
        out.append(clamp16(((body * gain) >> 15) * tick))
    return out


def cue_pot_set(seed: int) -> list[int]:
    """A pot going down on a shelf: a low thud with a short ring."""
    noise = Noise(seed)
    count = seconds(0.3)
    out = []
    phase = 0
    for index in range(count):
        phase += (150 << 16) // SAMPLE_RATE
        body = (sine_at(phase) >> 1) + (noise.next() >> 4)
        gain = envelope(index, count, 16, seconds(0.22))
        out.append(clamp16((body * gain) >> 15))
    return out


def cue_rotate(seed: int) -> list[int]:
    """A quarter turn: soft ceramic scrape."""
    noise = Noise(seed)
    count = seconds(0.25)
    out = []
    low = 0
    for index in range(count):
        low += (noise.next() - low) >> 4
        gain = envelope(index, count, seconds(0.05), seconds(0.12))
        out.append(clamp16((low >> 1) * gain >> 15))
    return out


def cue_drain(seed: int) -> list[int]:
    """Tipping a saucer out: a short gurgle."""
    noise = Noise(seed)
    count = seconds(0.45)
    out = []
    low = 0
    phase = 0
    for index in range(count):
        phase += (90 << 16) // SAMPLE_RATE
        low += (noise.next() - low) >> 3
        wobble = sine_at(phase) >> 3
        gain = envelope(index, count, seconds(0.03), seconds(0.25))
        out.append(clamp16((((low >> 1) + wobble) * gain) >> 15))
    return out


def cue_soil_press(seed: int) -> list[int]:
    """A finger into soil. The most-used verb in the game, so it is short,
    soft, and will not tire after the thousandth press."""
    noise = Noise(seed)
    count = seconds(0.14)
    out = []
    low = 0
    for index in range(count):
        low += (noise.next() - low) >> 5
        gain = envelope(index, count, seconds(0.01), seconds(0.1))
        out.append(clamp16((low >> 1) * gain >> 15))
    return out


def blip(seed: int, frequency: int, length: float, shape: str) -> list[int]:
    count = seconds(length)
    out = []
    phase = 0
    for index in range(count):
        phase += (frequency << 16) // SAMPLE_RATE
        value = triangle_at(phase) if shape == "tri" else sine_at(phase)
        gain = envelope(index, count, 48, count // 2)
        out.append(clamp16((value >> 2) * gain >> 15))
    return out


def cue_ui_move(seed: int) -> list[int]:
    return blip(seed, 520, 0.045, "tri")


def cue_ui_accept(seed: int) -> list[int]:
    first = blip(seed, 620, 0.05, "sine")
    second = blip(seed, 830, 0.07, "sine")
    return first + second


def cue_ui_reject(seed: int) -> list[int]:
    return blip(seed, 220, 0.09, "tri")


def cue_growth(seed: int) -> list[int]:
    """A new leaf. Three rising tones, quiet and warm -- the reward sound."""
    out: list[int] = []
    for frequency in (523, 659, 784):
        out += blip(seed, frequency, 0.09, "sine")
    return out


def cue_day_roll(seed: int) -> list[int]:
    return blip(seed, 392, 0.12, "sine")


def cue_wilt(seed: int) -> list[int]:
    """A falling tone. Sad, not alarming: the plant is not a failure state."""
    count = seconds(0.5)
    out = []
    phase = 0
    for index in range(count):
        frequency = 330 - (index * 120) // count
        phase += (frequency << 16) // SAMPLE_RATE
        gain = envelope(index, count, seconds(0.02), seconds(0.3))
        out.append(clamp16((sine_at(phase) >> 2) * gain >> 15))
    return out


def cue_rain(seed: int) -> list[int]:
    """Rain on the window, which is also when the jug fills."""
    noise = Noise(seed)
    count = seconds(1.2)
    out = []
    low = 0
    for index in range(count):
        low += (noise.next() - low) >> 4
        gain = envelope(index, count, seconds(0.3), seconds(0.4))
        out.append(clamp16((low >> 1) * gain >> 15))
    return out


def cue_spathe(seed: int) -> list[int]:
    """The peace lily flowers. The rarest sound in the game, so it may be the
    prettiest: a soft major arpeggio."""
    out: list[int] = []
    for frequency in (523, 659, 784, 1046):
        out += blip(seed, frequency, 0.11, "sine")
    return out


def cue_fold(seed: int) -> list[int]:
    """Nyctinasty at dusk: a very soft rustle, once a night."""
    noise = Noise(seed)
    count = seconds(0.4)
    out = []
    low = 0
    for index in range(count):
        low += (noise.next() - low) >> 6
        gain = envelope(index, count, seconds(0.1), seconds(0.25))
        out.append(clamp16((low >> 2) * gain >> 15))
    return out


def cue_leaf_rustle(seed: int) -> list[int]:
    noise = Noise(seed)
    count = seconds(0.3)
    out = []
    low = 0
    previous = 0
    for index in range(count):
        sample = noise.next()
        band = sample - previous
        previous = sample
        low += (band - low) >> 2
        gain = envelope(index, count, seconds(0.04), seconds(0.18))
        out.append(clamp16((low >> 2) * gain >> 15))
    return out


# Name -> (generator, seed). Names match the pg_audio_event_kind enumerators in
# include/pleb_plant_grower.h, lowercased, which is what pg_audio.c maps.
CUES = {
    "water": (cue_water, 0x5741_5445),
    "mist": (cue_mist, 0x4D49_5354),
    "feed": (cue_feed, 0x4645_4544),
    "snip": (cue_snip, 0x534E_4950),
    "pot_set": (cue_pot_set, 0x504F_5453),
    "rotate": (cue_rotate, 0x524F_5441),
    "drain": (cue_drain, 0x4452_4149),
    "soil_press": (cue_soil_press, 0x534F_494C),
    "ui_move": (cue_ui_move, 0x5549_4D56),
    "ui_accept": (cue_ui_accept, 0x5549_4143),
    "ui_reject": (cue_ui_reject, 0x5549_524A),
    "growth": (cue_growth, 0x4752_4F57),
    "day_roll": (cue_day_roll, 0x4441_5952),
    "wilt": (cue_wilt, 0x5749_4C54),
    "rain": (cue_rain, 0x5241_494E),
    "spathe": (cue_spathe, 0x5350_4154),
    "fold": (cue_fold, 0x464F_4C44),
    "leaf_rustle_1": (cue_leaf_rustle, 0x4C45_4131),
    "leaf_rustle_2": (cue_leaf_rustle, 0x4C45_4132),
    "leaf_rustle_3": (cue_leaf_rustle, 0x4C45_4133),
}


def encode_wav(samples: list[int]) -> bytes:
    """The WAV bytes for a cue, without touching the filesystem.

    Split out from write_wav because --check must not write: it used to
    regenerate every cue and then compare the regenerated bytes against the
    manifest, which verifies the generator and says nothing at all about the
    files on disk. A corrupted or tampered cue passed, and was silently
    repaired by the check that should have reported it.
    """
    payload = struct.pack("<%dh" % len(samples), *samples)
    block_align = CHANNELS * BITS // 8
    header = b"RIFF" + struct.pack("<I", 36 + len(payload)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, CHANNELS, SAMPLE_RATE,
                                    SAMPLE_RATE * block_align, block_align,
                                    BITS)
    header += b"data" + struct.pack("<I", len(payload))
    return header + payload


def write_wav(path: pathlib.Path, samples: list[int]) -> bytes:
    payload = struct.pack("<%dh" % len(samples), *samples)
    block_align = CHANNELS * BITS // 8
    header = b"RIFF" + struct.pack("<I", 36 + len(payload)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, CHANNELS, SAMPLE_RATE,
                                    SAMPLE_RATE * block_align, block_align,
                                    BITS)
    header += b"data" + struct.pack("<I", len(payload))
    data = header + payload
    path.write_bytes(data)
    return data


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="assets/sfx")
    parser.add_argument("--manifest", default="docs/audio-provenance.json")
    parser.add_argument("--check", action="store_true",
                        help="regenerate and compare against the manifest")
    args = parser.parse_args(argv[1:])

    out = pathlib.Path(args.out)
    manifest_path = pathlib.Path(args.manifest)
    out.mkdir(parents=True, exist_ok=True)

    entries = []
    for name in sorted(CUES):
        generator, seed = CUES[name]
        samples = generator(seed)
        target = out / f"{name}.wav"
        # In check mode nothing is written: the point is to inspect what is
        # there, not to replace it and then agree with ourselves.
        data = encode_wav(samples) if args.check else write_wav(target,
                                                                samples)
        entries.append({
            "cue": name,
            "file": f"{out.name}/{name}.wav",
            "command": f"tools/gen_sfx.py --out {args.out}",
            "generator_package": "pleb-plant-grower/tools/gen_sfx.py",
            "seed": seed,
            "sample_rate": SAMPLE_RATE,
            "channels": CHANNELS,
            "bits_per_sample": BITS,
            "duration_seconds": round(len(samples) / SAMPLE_RATE, 6),
            "final_sha256": hashlib.sha256(data).hexdigest(),
        })

    manifest = {
        "schema_version": 1,
        "_purpose": ("Per-cue provenance in the kilix-lander format. "
                     "tools/gen_sfx.py --check regenerates every cue and "
                     "compares, so a drifted bank fails make test."),
        "_reproducibility": ("Integer arithmetic only: no math module, no "
                             "floating point in any signal path. A bank made "
                             "with libm transcendentals reproduces on the "
                             "machine that made it and nowhere else."),
        "format": {"encoding": "PCM", "channels": CHANNELS,
                   "bits_per_sample": BITS, "sample_rate": SAMPLE_RATE},
        "entries": entries,
    }

    if args.check:
        if not manifest_path.exists():
            print("check-sfx: no manifest; run make sfx", file=sys.stderr)
            return 1
        recorded = json.loads(manifest_path.read_text())
        old = {e["cue"]: e for e in recorded.get("entries", [])}
        new = {e["cue"]: e for e in entries}
        failures = []
        for cue in sorted(set(old) | set(new)):
            if cue not in old:
                failures.append(f"{cue}: generated but not in the manifest")
                continue
            if cue not in new:
                failures.append(f"{cue}: in the manifest but not generated")
                continue
            # The generator still reproduces what the manifest recorded.
            if old[cue]["final_sha256"] != new[cue]["final_sha256"]:
                failures.append(
                    f"{cue}: the generator no longer reproduces the manifest\n"
                    f"    manifest {old[cue]['final_sha256']}\n"
                    f"    now      {new[cue]['final_sha256']}")
            # AND the file on disk is that file. This half was missing, so a
            # corrupted cue passed and was silently overwritten.
            shipped = out / f"{cue}.wav"
            if not shipped.is_file():
                failures.append(f"{cue}: missing from {out}; run make sfx")
                continue
            digest = hashlib.sha256(shipped.read_bytes()).hexdigest()
            if digest != old[cue]["final_sha256"]:
                failures.append(
                    f"{cue}: the file on disk does not match the manifest\n"
                    f"    manifest {old[cue]['final_sha256']}\n"
                    f"    on disk  {digest}")
        if failures:
            for failure in failures:
                print(f"check-sfx: {failure}", file=sys.stderr)
            return 1
        print(f"check-sfx: PASS ({len(entries)} cues byte-identical to "
              f"{manifest_path})")
        return 0

    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    total = sum(e["duration_seconds"] for e in entries)
    print(f"gen-sfx: wrote {len(entries)} cues ({total:.2f}s) to {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
