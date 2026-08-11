#!/usr/bin/env python3
"""content/*.json -> build/pg_content_generated.h.

Standard library only, and byte-for-byte deterministic: fixed field order, no
timestamps, no dict iteration order dependence, LF endings, and every emitted
number an integer on the one fixed-point convention of ARCHITECTURE.md §6.2 —
`_l` levels 0..10000, `_q12` multipliers with 4096 = x1.0, `_c` centi-units for
DLI, and whole hours, days, months or care ticks for intervals. No float
reaches the care model; that is what makes a 30-day catch-up byte-identical
across machines and what -ffp-contract=off is protecting.

`--check` recompiles into a temporary directory and compares, so a stale
generated header fails the build instead of silently disagreeing with the JSON
somebody just edited.

Validation runs first, always, in both modes: tools/check_care_schedule.py
asserts every authored number against a cited range and asserts the D-084
reachability of every species threshold. A number that has not been validated
never reaches the header.
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

# The validator is imported rather than shelled out to, so a number cannot
# reach the header without having been checked. Byte-compiling it would leave a
# __pycache__ directory in the shipped tree, which check-release-tree correctly
# treats as a finding, so the cache is off before the import happens.
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_care_schedule as schedule  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
CONTENT = ROOT / "content"
DEFAULT_OUT = ROOT / "build" / "pg_content_generated.h"

LEVEL_MAX = 10000
Q12_ONE = 4096
CARE_TICKS_PER_HOUR = 4
CARE_TICKS_PER_DAY = 96

LIGHT_BANDS = ("LOW", "MEDIUM", "BRIGHT_INDIRECT", "DIRECT")
FLAG_BITS = {
    "NYCTINASTY": "PG_FLAG_NYCTINASTY",
    "VARIEGATED": "PG_FLAG_VARIEGATED",
    "CAM": "PG_FLAG_CAM",
    "FLOWERS": "PG_FLAG_FLOWERS",
    "TRAILING": "PG_FLAG_TRAILING",
    "PATTERNED": "PG_FLAG_PATTERNED",
}
FLAG_ORDER = ("NYCTINASTY", "VARIEGATED", "CAM", "FLOWERS", "TRAILING",
              "PATTERNED")


def load(name: str) -> dict:
    with (CONTENT / name).open("r", encoding="utf-8") as handle:
        return json.load(handle)


def quote(text: str) -> str:
    escaped = text.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def light_enum(band: str) -> str:
    return f"PG_LIGHT_{band}"


def flags_expression(flags: list[str]) -> str:
    ordered = [FLAG_BITS[f] for f in FLAG_ORDER if f in flags]
    return " | ".join(ordered) if ordered else "0u"


def round_div(numerator: int, denominator: int) -> int:
    """Round half away from zero, in integers, so the result is portable."""
    if denominator < 0:
        numerator, denominator = -numerator, -denominator
    if numerator >= 0:
        return (numerator * 2 + denominator) // (denominator * 2)
    return -((-numerator * 2 + denominator) // (denominator * 2))


def base_drain_q12l(thirst_threshold_l: int, interval_dh: int) -> int:
    """PLANT_CARE.md §4.1: base_drain_per_hour = (1 - thirst)/(days * 24).

    Expressed per care tick, on the level scale, in Q12 — so the tick loop can
    carry the fraction in an integer and lose nothing over a 30-day catch-up.
    interval_dh is deci-days, and there are 96 care ticks in a day.
    """
    numerator = (LEVEL_MAX - thirst_threshold_l) * Q12_ONE * 10
    denominator = interval_dh * CARE_TICKS_PER_DAY
    return round_div(numerator, denominator)


def emit(out: list[str], line: str = "") -> None:
    out.append(line)


def build_header() -> str:
    plants = load("plants.json")
    pots = load("pots.json")
    spots = load("spots.json")
    seasons = load("seasons.json")

    species_entries = plants["entries"]
    pot_entries = pots["entries"]
    spot_entries = spots["entries"]
    months = seasons["months"]
    chain = seasons["multiplier_chain"]
    uptake_share = pots["uptake_share_q12"]

    strings: list[str] = []

    def record(text: str) -> str:
        if text not in strings:
            strings.append(text)
        return quote(text)

    out: list[str] = []
    emit(out, "/*")
    emit(out, " * GENERATED FILE — do not edit.")
    emit(out, " *")
    emit(out, " * tools/compile_content.py compiled this from content/plants.json,")
    emit(out, " * content/pots.json, content/spots.json and content/seasons.json.")
    emit(out, " * Edit those and rebuild; `make test-content` recompiles into a")
    emit(out, " * temporary directory and compares, so a stale copy fails the build.")
    emit(out, " *")
    emit(out, " * Every number here is a fixed-point integer on the one convention of")
    emit(out, " * ARCHITECTURE.md §6.2 (D-083). There are no floats in the care model.")
    emit(out, " */")
    emit(out, "#ifndef PG_CONTENT_GENERATED_H")
    emit(out, "#define PG_CONTENT_GENERATED_H")
    emit(out)
    emit(out, "/* Included only by src/pg_content.c, which defines the types first. */")
    emit(out)

    emit(out, "#define PG_CONTENT_SCHEMA_VERSION %d" % plants["schema_version"])
    emit(out, "#define PG_CONTENT_SPECIES_COUNT %d" % len(species_entries))
    emit(out, "#define PG_CONTENT_POT_COUNT %d" % len(pot_entries))
    emit(out, "#define PG_CONTENT_SPOT_COUNT %d" % len(spot_entries))
    emit(out, "#define PG_CONTENT_MONTH_COUNT %d" % len(months))
    emit(out)

    emit(out, "/* PLANT_CARE.md §4.5, D-082: one species-independent threshold, and the")
    emit(out, " * cachepot's floor sits strictly above it. */")
    emit(out, "#define PG_SATURATION_L %d" % pots["saturation_level"])
    emit(out, "#define PG_UPTAKE_SHARE_Q12 %d" % uptake_share)
    emit(out, "#define PG_PERCOLATION_Q12 %d" % pots["percolation_q12"])
    emit(out)
    emit(out, "/* D-085: every recovery, shock and fair-warning division uses")
    emit(out, " * effective_scalar = max(growth_scalar, 0.30). */")
    emit(out, "#define PG_RECOVERY_SCALAR_FLOOR_Q12 %d"
         % seasons["recovery"]["effective_scalar_floor_q12"])
    emit(out)
    emit(out, "/* Cross-checks against the constants the public header and pg_calendar")
    emit(out, " * already carry; src/pg_content.c static-asserts them. */")
    emit(out, "#define PG_CONTENT_SOLAR_NOON_LOCAL_MINUTES %d"
         % seasons["solar_noon_local_minutes"])
    emit(out, "#define PG_CONTENT_HEMISPHERE_OFFSET_DAYS %d"
         % seasons["hemisphere_offset_days"])
    emit(out, "#define PG_CONTENT_GROWING_THRESHOLD_L %d"
         % seasons["season_scalar"]["growing_threshold_l"])
    emit(out, "#define PG_CONTENT_FIRST_GROWING_ORDINAL %d"
         % seasons["season_scalar"]["first_growing_ordinal"])
    emit(out, "#define PG_CONTENT_LAST_GROWING_ORDINAL %d"
         % seasons["season_scalar"]["last_growing_ordinal"])
    emit(out)

    # ---- species ---------------------------------------------------------
    emit(out, "static const pg_species PG_CONTENT_SPECIES[PG_CONTENT_SPECIES_COUNT] = {")
    for entry in species_entries:
        growing = base_drain_q12l(entry["thirst_threshold_l"],
                                  entry["water_interval_growing_dh"])
        dormant = base_drain_q12l(entry["thirst_threshold_l"],
                                  entry["water_interval_dormant_dh"])
        emit(out, "    {")
        emit(out, "        .id = %s," % record(entry["id"]))
        emit(out, "        .common_name = %s," % record(entry["common_name"]))
        emit(out, "        .botanical_name = %s," % record(entry["botanical_name"]))
        emit(out, "        .default_plant_name = %s,"
             % record(entry["default_plant_name"]))
        emit(out, "        .numeric_id = %d," % entry["numeric_id"])
        emit(out, "        .difficulty = %d," % entry["difficulty"])
        emit(out, "        .water_interval_growing_dh = %d,"
             % entry["water_interval_growing_dh"])
        emit(out, "        .water_interval_dormant_dh = %d,"
             % entry["water_interval_dormant_dh"])
        emit(out, "        .thirst_threshold_l = %d," % entry["thirst_threshold_l"])
        emit(out, "        .saturation_tolerance_hours = %d,"
             % entry["saturation_tolerance_hours"])
        emit(out, "        .drought_recovery_hours = %d,"
             % entry["drought_recovery_hours"])
        emit(out, "        .light_min = %s," % light_enum(entry["light_min"]))
        emit(out, "        .light_ideal = %s," % light_enum(entry["light_ideal"]))
        emit(out, "        .light_max = %s," % light_enum(entry["light_max"]))
        emit(out, "        .dli_maintenance_c = %d," % entry["dli_maintenance_c"])
        emit(out, "        .dli_thriving_c = %d," % entry["dli_thriving_c"])
        emit(out, "        .dli_flower_c = %d," % entry["dli_flower_c"])
        emit(out, "        .rh_ideal_low = %d," % entry["rh_ideal_low"])
        emit(out, "        .rh_ideal_high = %d," % entry["rh_ideal_high"])
        emit(out, "        .rh_damage_below = %d," % entry["rh_damage_below"])
        emit(out, "        .temp_ideal_low = %d," % entry["temp_ideal_low"])
        emit(out, "        .temp_ideal_high = %d," % entry["temp_ideal_high"])
        emit(out, "        .temp_damage_below = %d," % entry["temp_damage_below"])
        emit(out, "        .feed_interval_days = %d," % entry["feed_interval_days"])
        emit(out, "        .feed_strength_q12 = %d," % entry["feed_strength_q12"])
        emit(out, "        .salt_sensitivity_q12 = %d," % entry["salt_sensitivity_q12"])
        emit(out, "        .water_purity_sensitivity_q12 = %d,"
             % entry["water_purity_sensitivity_q12"])
        emit(out, "        .repot_interval_months = %d," % entry["repot_interval_months"])
        emit(out, "        .rootbound_tolerance_l = %d," % entry["rootbound_tolerance_l"])
        emit(out, "        .scar_permanence_q12 = %d," % entry["scar_permanence_q12"])
        emit(out, "        .rot_survival_l = %d," % entry["rot_survival_l"])
        emit(out, "        .propagation_mode = PG_PROPAGATION_%s,"
             % entry["propagation_mode"])
        emit(out, "        .flags = (uint8_t)(%s)," % flags_expression(entry["flags"]))
        emit(out, "        /* derived from PLANT_CARE.md §4.1's calibration, never authored:")
        emit(out, "         * base_drain_per_hour = (1 - thirst_threshold) / (interval_days * 24),")
        emit(out, "         * expressed per 900 s care tick on the level scale, in Q12. */")
        emit(out, "        .base_drain_growing_q12l = %d," % growing)
        emit(out, "        .base_drain_dormant_q12l = %d," % dormant)
        emit(out, "        .saturation_tolerance_ticks = %d,"
             % (entry["saturation_tolerance_hours"] * CARE_TICKS_PER_HOUR))
        emit(out, "        .drought_recovery_ticks = %d,"
             % (entry["drought_recovery_hours"] * CARE_TICKS_PER_HOUR))
        emit(out, "    },")
    emit(out, "};")
    emit(out)

    # ---- pots ------------------------------------------------------------
    emit(out, "static const pg_pot PG_CONTENT_POTS[PG_CONTENT_POT_COUNT] = {")
    for entry in pot_entries:
        side = entry["side_evaporation_share_q12"]
        surface = Q12_ONE - side - uptake_share
        emit(out, "    {")
        emit(out, "        .id = %s," % record(entry["id"]))
        emit(out, "        .display_name = %s," % record(entry["display_name"]))
        emit(out, "        .material = %s," % record(entry["material"]))
        emit(out, "        .numeric_id = %d," % entry["numeric_id"])
        emit(out, "        .drain_multiplier_q12 = %d," % entry["drain_multiplier_q12"])
        emit(out, "        .side_evaporation_share_q12 = %d," % side)
        emit(out, "        /* derived: the pot only decides how the remainder splits. */")
        emit(out, "        .surface_share_q12 = %d," % surface)
        emit(out, "        .uptake_share_q12 = %d," % uptake_share)
        emit(out, "        .reservoir_q12 = %d," % entry["reservoir_q12"])
        emit(out, "        .bottom_floor_l = %d," % entry["bottom_floor_l"])
        emit(out, "        .soil_temp_offset_c_x10 = %d," % entry["soil_temp_offset_c_x10"])
        emit(out, "        .salt_retention_q12 = %d," % entry["salt_retention_q12"])
        emit(out, "        .tip_risk_l = %d," % entry["tip_risk_l"])
        emit(out, "        .style = %d," % entry["style"])
        emit(out, "        .has_drainage = %s," % ("true" if entry["has_drainage"] else "false"))
        emit(out, "        .is_sleeve_capable = %s," % ("true" if entry["is_sleeve_capable"] else "false"))
        emit(out, "        .is_sleeve_liner = %s," % ("true" if entry["is_sleeve_liner"] else "false"))
        emit(out, "        .can_be_flushed = %s," % ("true" if entry["can_be_flushed"] else "false"))
        emit(out, "        .shows_salt_crust = %s," % ("true" if entry["shows_salt_crust"] else "false"))
        emit(out, "        .shows_roots_at_holes = %s," % ("true" if entry["shows_roots_at_holes"] else "false"))
        emit(out, "    },")
    emit(out, "};")
    emit(out)

    # ---- spots -----------------------------------------------------------
    emit(out, "static const pg_spot PG_CONTENT_SPOTS[PG_CONTENT_SPOT_COUNT] = {")
    for entry in spot_entries:
        emit(out, "    {")
        emit(out, "        .id = %s," % record(entry["id"]))
        emit(out, "        .display_name = %s," % record(entry["display_name"]))
        emit(out, "        .default_scene = %s," % record(entry["default_scene"]))
        emit(out, "        .numeric_id = %d," % entry["numeric_id"])
        emit(out, "        .light_band = %s," % light_enum(entry["light_band"]))
        emit(out, "        .dli_peak_c = %d," % entry["dli_peak_c"])
        emit(out, "        .dli_floor_c = %d," % entry["dli_floor_c"])
        emit(out, "        .temp_summer_day_c_x10 = %d," % entry["temp_summer_day_c_x10"])
        emit(out, "        .temp_summer_night_c_x10 = %d," % entry["temp_summer_night_c_x10"])
        emit(out, "        .temp_winter_day_c_x10 = %d," % entry["temp_winter_day_c_x10"])
        emit(out, "        .temp_winter_night_c_x10 = %d," % entry["temp_winter_night_c_x10"])
        emit(out, "        .rh_summer_pct = %d," % entry["rh_summer_pct"])
        emit(out, "        .rh_winter_pct = %d," % entry["rh_winter_pct"])
        emit(out, "        .airflow_q12 = %d," % entry["airflow_q12"])
        emit(out, "        .cold_glass = %s," % ("true" if entry["cold_glass"] else "false"))
        emit(out, "    },")
    emit(out, "};")
    emit(out)

    # ---- the multiplier chain -------------------------------------------
    emit(out, "/* PLANT_CARE.md §4.2. Light and temperature drive the drying rate, which")
    emit(out, " * is why a dim cold room stretches the interval and why a fixed weekly")
    emit(out, " * watering day kills plants. */")
    emit(out, "static const pg_multipliers PG_CONTENT_MULTIPLIERS = {")
    emit(out, "    .light = {")
    for band in LIGHT_BANDS:
        emit(out, "        [%s] = %d," % (light_enum(band), chain["light"][band]))
    emit(out, "    },")
    emit(out, "    .temp_above_26 = %d," % chain["temperature"]["above_26_c"])
    emit(out, "    .temp_20_26 = %d," % chain["temperature"]["band_20_26_c"])
    emit(out, "    .temp_16_20 = %d," % chain["temperature"]["band_16_20_c"])
    emit(out, "    .temp_below_16 = %d," % chain["temperature"]["below_16_c"])
    emit(out, "    .rh_below_35 = %d," % chain["humidity"]["below_35_pct"])
    emit(out, "    .rh_35_60 = %d," % chain["humidity"]["band_35_60_pct"])
    emit(out, "    .rh_above_60 = %d," % chain["humidity"]["above_60_pct"])
    emit(out, "    .season_growing = %d," % chain["season"]["growing_q12"])
    emit(out, "    .season_dormant = %d," % chain["season"]["dormant_q12"])
    emit(out, "    .root_sparse = %d," % chain["root_mass"]["sparse"])
    emit(out, "    .root_established = %d," % chain["root_mass"]["established"])
    emit(out, "    .root_bound = %d," % chain["root_mass"]["rootbound"])
    emit(out, "};")
    emit(out)

    # ---- the month table -------------------------------------------------
    emit(out, "/* Advisory only: the calendar screen and the notebook read these, and")
    emit(out, " * nothing in the simulation branches on a month. The season itself is")
    emit(out, " * derived from the daylight curve (PLANT_CARE.md §4.3). */")
    emit(out, "static const pg_season_month PG_CONTENT_MONTHS[PG_CONTENT_MONTH_COUNT] = {")
    for entry in months:
        emit(out, "    { .month = %d, .water = PG_WATER_%s, .feed = PG_FEED_%s, "
                  ".repot = PG_REPOT_%s },"
             % (entry["month"], entry["water"], entry["feed"], entry["repot"]))
    emit(out, "};")
    emit(out)

    # ---- every string the compiled content can put on screen -------------
    emit(out, "/* Every string the compiled content can put in front of a player. The")
    emit(out, " * myth blocklist of PLANT_CARE.md §7 is asserted against this table, so")
    emit(out, " * the check needs no file I/O and holds inside the embed. */")
    emit(out, "#define PG_CONTENT_STRING_COUNT %d" % len(strings))
    emit(out, "static const char *const PG_CONTENT_STRINGS[PG_CONTENT_STRING_COUNT] = {")
    for text in strings:
        emit(out, "    %s," % quote(text))
    emit(out, "};")
    emit(out)
    emit(out, "#endif /* PG_CONTENT_GENERATED_H */")
    return "\n".join(out) + "\n"


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT,
                        help="generated header path (default: build/pg_content_generated.h)")
    parser.add_argument("--check", action="store_true",
                        help="recompile into a temporary directory and compare")
    args = parser.parse_args(argv[1:])

    failures = schedule.validate()
    if failures:
        for item in failures:
            print(f"compile-content: {item}", file=sys.stderr)
        return 1

    text = build_header()

    if not args.check:
        write(args.out, text)
        print(f"compile-content: wrote {args.out} ({len(text)} bytes)")
        return 0

    if not args.out.exists():
        print(f"compile-content: {args.out} does not exist; run the build first",
              file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        candidate = Path(tmp) / "pg_content_generated.h"
        write(candidate, text)
        current = args.out.read_bytes()
        fresh = candidate.read_bytes()
        if current != fresh:
            print(f"compile-content: {args.out} is stale — it does not match a "
                  f"fresh compile of content/*.json", file=sys.stderr)
            return 1
    print("compile-content: PASS (generated header matches content/*.json)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
