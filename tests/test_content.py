#!/usr/bin/env python3
"""Authored-data policy: counts, ids, and the myth blocklist.

Three jobs, in order of how badly each failure would hurt:

  1. **Ids may never move.** A moved id silently corrupts every existing save
     and is the one save bug that cannot be detected after the fact, so the
     numbers in content/*.json are checked against the enumerations in the
     public header — which is what the encoder and every shipped save already
     agree on — and against content/id-ledger.json whenever that is populated.
  2. **Exactly four species and four pots**, and six spots, because the counts
     are compiled into the public header as constants.
  3. **The game must never say any of the blocklisted myths.** Every authored
     string in content/ is greped for them, including the string tables.

tools/check_care_schedule.py owns the ranges; this file owns identity and
voice.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONTENT = ROOT / "content"
HEADER = ROOT / "include" / "pleb_plant_grower.h"

EXPECTED_COUNTS = {
    "plants.json": ("PG_SPECIES_COUNT", 4),
    "pots.json": ("PG_POT_COUNT", 4),
    "spots.json": ("PG_SPOT_COUNT", 6),
}

# content id -> the enumerator in include/pleb_plant_grower.h that must carry
# the same number. These pairings are the append-only contract (D-044, D-081).
ID_TO_ENUM = {
    "plants.json": {
        "golden-pothos": "PG_SPECIES_POTHOS",
        "snake-plant-laurentii": "PG_SPECIES_SNAKE",
        "peace-lily": "PG_SPECIES_PEACE_LILY",
        "calathea": "PG_SPECIES_CALATHEA",
    },
    "pots.json": {
        "terracotta": "PG_POT_TERRACOTTA",
        "glazed": "PG_POT_GLAZED",
        "nursery": "PG_POT_NURSERY",
        "cachepot": "PG_POT_CACHEPOT",
    },
    "spots.json": {
        "sill-south": "PG_SPOT_SILL_SOUTH",
        "one-metre": "PG_SPOT_ONE_METRE",
        "two-metre": "PG_SPOT_TWO_METRE",
        "north-shelf": "PG_SPOT_NORTH_SHELF",
        "radiator-shelf": "PG_SPOT_RADIATOR_SHELF",
        "bath-shelf": "PG_SPOT_BATH_SHELF",
    },
}

# A spot's suggested plate is a scene id, and scene ids are a SEPARATE
# numbering: no code may assume spot_id == scene_id (D-081).
SCENE_TO_ENUM = {
    "sunny-sill": "PG_SCENE_SUNNY_SILL",
    "bright-corner": "PG_SCENE_BRIGHT_CORNER",
    "study-desk": "PG_SCENE_STUDY_DESK",
    "plain-studio": "PG_SCENE_PLAIN_STUDIO",
    "kitchen-shelf": "PG_SCENE_KITCHEN_SHELF",
    "steamy-bath": "PG_SCENE_STEAMY_BATH",
}

# PLANT_CARE.md §7. Nothing on this list may be asserted by the game in any
# voice. Each myth stays available as an action and behaves truthfully; what is
# forbidden is the game recommending it.
MYTHS = {
    "improves drainage": "a gravel or crock layer raises the perched water table",
    "improve drainage": "a gravel or crock layer raises the perched water table",
    "for drainage": "gravel 'for drainage' makes drainage worse",
    "raises humidity": "misting raises local RH for 10-30 minutes and no more",
    "raise humidity": "misting raises local RH for 10-30 minutes and no more",
    "pebble tray": "the effect is small and very local",
    "purif": "the chamber studies do not scale to real rooms",
    "cleans the air": "the chamber studies do not scale to real rooms",
    "much bigger pot": "excess wet mix with no roots in it rots the root ball",
    "bigger pot helps": "excess wet mix with no roots in it rots the root ball",
    "ice cube": "cold shock plus chronic underwatering",
    "yellow leaves mean": "yellowing lower leaves usually means the opposite",
    "needs more water": "yellowing lower leaves usually means the opposite",
    "watering day": "a fixed weekly watering day is the anti-message of the game",
    "once a week": "a fixed weekly watering day is the anti-message of the game",
    "weekly watering": "a fixed weekly watering day is the anti-message of the game",
    "droplets cause": "not reproducible on flat leaves in normal conditions",
    "helps it recover": "feeding a sick plant adds osmotic stress",
}


def load(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def header_constants() -> dict[str, int]:
    """The #defines and enumerators of the public header, by name.

    Enumerators in this header are all written with an explicit value, which is
    the point: a stable id that depends on its position in a list is not
    stable.
    """
    text = HEADER.read_text(encoding="utf-8")
    values: dict[str, int] = {}
    for name, value in re.findall(r"^\s*#define\s+(PG_[A-Z0-9_]+)\s+(\d+)\s*$",
                                  text, re.MULTILINE):
        values[name] = int(value)
    for name, value in re.findall(r"\b(PG_[A-Z0-9_]+)\s*=\s*(\d+)\s*[,}]", text):
        values[name] = int(value)
    return values


def check_ids(failures: list[str], constants: dict[str, int]) -> None:
    for filename, mapping in ID_TO_ENUM.items():
        data = load(CONTENT / filename)
        entries = data["entries"]
        seen: dict[str, int] = {}
        for index, entry in enumerate(entries):
            key = entry.get("id")
            numeric = entry.get("numeric_id")
            seen[key] = numeric
            if numeric != index:
                failures.append(
                    f"{filename}: '{key}' has numeric_id {numeric} at position "
                    f"{index}; ids are append-only and may never be reordered")
            enum_name = mapping.get(key)
            if enum_name is None:
                failures.append(
                    f"{filename}: '{key}' has no enumerator in the public "
                    f"header; a content id with no compiled counterpart cannot "
                    f"be persisted")
                continue
            if enum_name not in constants:
                failures.append(
                    f"include/pleb_plant_grower.h: {enum_name} is missing")
            elif constants[enum_name] != numeric:
                failures.append(
                    f"{filename}: '{key}' is {numeric} but {enum_name} is "
                    f"{constants[enum_name]} — a moved id silently corrupts "
                    f"every existing save")
        missing = sorted(set(mapping) - set(seen))
        if missing:
            failures.append(f"{filename}: missing authored ids: "
                            f"{', '.join(missing)}")


def check_counts(failures: list[str], constants: dict[str, int]) -> None:
    for filename, (constant, expected) in EXPECTED_COUNTS.items():
        entries = load(CONTENT / filename)["entries"]
        if len(entries) != expected:
            failures.append(f"{filename}: {len(entries)} entries, expected "
                            f"{expected}")
        if constants.get(constant) != expected:
            failures.append(f"include/pleb_plant_grower.h: {constant} is "
                            f"{constants.get(constant)}, expected {expected}")


def check_scene_ids_are_a_separate_numbering(failures: list[str],
                                             constants: dict[str, int]) -> None:
    """Every spot's suggested plate must name a real scene (D-041, D-081).

    Scene ids and spot ids are separate numberings. The plate is cosmetic and
    the spot is the simulation object, so the two must be resolvable by NAME —
    a spot whose default_scene the header cannot name would force a consumer to
    fall back on the index, which is exactly the assumption D-081 forbids.
    """
    header = HEADER.read_text(encoding="utf-8")
    for spot in load(CONTENT / "spots.json")["entries"]:
        scene = spot.get("default_scene")
        enum_name = SCENE_TO_ENUM.get(scene)
        if enum_name is None:
            failures.append(f"spots.json: '{spot.get('id')}' names a scene "
                            f"'{scene}' the public header does not carry")
        elif enum_name not in constants:
            failures.append(f"include/pleb_plant_grower.h: {enum_name} is missing")

    # The two defaults deliberately differ: the neutral plate is plain-studio
    # while the safe light band is one-metre. Pairing them by index would start
    # every new player on the north shelf, in a survival-only band.
    if not re.search(r"#define\s+PG_SCENE_DEFAULT\s+PG_SCENE_PLAIN_STUDIO",
                     header):
        failures.append("include/pleb_plant_grower.h: PG_SCENE_DEFAULT must be "
                        "the neutral plate, plain-studio")
    if not re.search(r"#define\s+PG_SPOT_DEFAULT\s+PG_SPOT_ONE_METRE", header):
        failures.append("include/pleb_plant_grower.h: PG_SPOT_DEFAULT must be "
                        "the safe light band, one-metre")


def check_ledger(failures: list[str]) -> None:
    ledger_path = CONTENT / "id-ledger.json"
    if not ledger_path.exists():
        failures.append("content/id-ledger.json is missing")
        return
    ledger = load(ledger_path)
    assigned = ledger.get("assigned", [])
    if not assigned:
        # An empty ledger is a FAILURE, not a skip. While it was tolerated the
        # advertised "ids unmoved" assertion was vacuous: nothing would have
        # caught a renumbered species, and D-044 makes that undetectable once a
        # save exists in the wild. A gate that waves through its own empty
        # input is not a gate.
        failures.append(
            "content/id-ledger.json carries no assignments, so the "
            "append-only id guarantee (D-044) is unenforced — populate it "
            "from content/*.json")
        return
    by_kind: dict[str, dict[str, int]] = {}
    for record in assigned:
        by_kind.setdefault(record["kind"], {})[record["id"]] = record["numeric_id"]
    next_id = ledger.get("next_id", {})
    for filename, kind in (("plants.json", "species"), ("pots.json", "pot"),
                           ("spots.json", "spot")):
        recorded = by_kind.get(kind, {})
        seen_numeric: dict[int, str] = {}
        for entry in load(CONTENT / filename)["entries"]:
            key, numeric = entry.get("id"), entry.get("numeric_id")
            if key not in recorded:
                # The direction that actually matters: new content that never
                # claimed its number.
                failures.append(
                    f"{filename}: '{key}' is not in the id ledger — every id "
                    f"compiled into a save must be recorded there (D-044)")
            elif recorded[key] != numeric:
                failures.append(
                    f"{filename}: '{key}' is {numeric} but the id ledger "
                    f"records {recorded[key]}")
            if numeric in seen_numeric:
                failures.append(
                    f"{filename}: {kind} id {numeric} is used by both "
                    f"'{seen_numeric[numeric]}' and '{key}'")
            seen_numeric[numeric] = key
        # next_id must be past everything assigned, or the next author
        # silently reuses a live number.
        limit = next_id.get(kind) if isinstance(next_id, dict) else None
        if limit is None:
            failures.append(f"id-ledger.json: no next_id for '{kind}'")
        elif recorded and limit <= max(recorded.values()):
            failures.append(
                f"id-ledger.json: next_id['{kind}'] is {limit}, which is not "
                f"past the highest assigned id {max(recorded.values())}")


def walk_strings(node: object, path: str, out: list[tuple[str, str]]) -> None:
    if isinstance(node, dict):
        for key, value in node.items():
            if key.startswith("_"):
                continue        # documentation, not player-facing
            walk_strings(value, f"{path}.{key}", out)
    elif isinstance(node, list):
        for index, value in enumerate(node):
            walk_strings(value, f"{path}[{index}]", out)
    elif isinstance(node, str):
        out.append((path, node))


def c_string_literals(path: Path) -> list[tuple[str, str]]:
    """Every "..." literal in a C file, with a file:line label.

    Crude on purpose: it over-reports (an #include path is a literal too) and
    over-reporting costs nothing here, while under-reporting would let a myth
    ship. The blocklist needles are English phrases, so a path or a format
    string never matches one by accident.
    """
    out: list[tuple[str, str]] = []
    for number, line in enumerate(path.read_text().splitlines(), start=1):
        cursor = 0
        while True:
            start = line.find('"', cursor)
            if start < 0:
                break
            end = start + 1
            while end < len(line):
                if line[end] == "\\":
                    end += 2
                    continue
                if line[end] == '"':
                    break
                end += 1
            if end >= len(line):
                break
            out.append((f"{path.name}:{number}", line[start + 1:end]))
            cursor = end + 1
    return out


def check_myths(failures: list[str]) -> int:
    """The blocklist, over every player-visible string the game can produce.

    That deliberately includes src/pg_advice.c. The instruction surface is
    exactly where a myth would be most damaging and most plausible -- it is
    the file whose whole job is to sound authoritative -- and leaving it to
    review would be leaving it to the one check a well-written myth passes.

    The advice strings are C rather than JSON because they are chosen by code
    that reads the care axes, not looked up by key. Scanning the source is the
    honest way to cover them; moving them to JSON purely to satisfy a test
    would be the tail wagging the dog.
    """
    checked = 0
    sources: list[tuple[str, str]] = []
    for path in sorted(CONTENT.rglob("*.json")):
        data = load(path)
        strings: list[tuple[str, str]] = []
        walk_strings(data, path.name, strings)
        sources.extend(strings)
    advice = ROOT / "src" / "pg_advice.c"
    if not advice.exists():
        failures.append("src/pg_advice.c is missing; the instruction surface "
                        "is not being checked against the myth blocklist")
    else:
        sources.extend(c_string_literals(advice))
    for where, text in sources:
        checked += 1
        lowered = text.lower()
        for needle, reality in MYTHS.items():
            if needle in lowered:
                failures.append(
                    f"{where}: says \"{needle}\" — blocklisted "
                    f"(PLANT_CARE.md §7: {reality})")
    return checked


def main() -> int:
    failures: list[str] = []
    constants = header_constants()

    check_counts(failures, constants)
    check_ids(failures, constants)
    check_scene_ids_are_a_separate_numbering(failures, constants)
    check_ledger(failures)
    checked = check_myths(failures)

    if failures:
        for item in failures:
            print(f"test-content: {item}", file=sys.stderr)
        return 1
    print(f"test-content: PASS (4 species, 4 pots, 6 spots, ids unmoved, "
          f"{checked} authored strings free of blocklisted myths)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
