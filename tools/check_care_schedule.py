#!/usr/bin/env python3
"""Care-schedule validation: every authored number against a cited range.

PLANT_CARE.md is the factual authority for the project — "a number that is not
in this document is not allowed in the game" — and this is the gate that makes
that literal. Two things are asserted:

  1. Every numeric field of every content entry sits inside a range that carries
     a citation to the section it came from. A field with no cited range is an
     ERROR, not a pass: silence is how an unsourced number gets in.
  2. The reachability assertion (D-084, amended by D-093): for every species
     there must be at least one spot inside that species' light ceiling whose
     growing-season DLI peak meets `dli_thriving_c` — and any non-zero
     `dli_flower_c` — and whose midwinter DLI floor meets `dli_maintenance_c`.
     A threshold nobody can reach is a promise the game cannot keep.

The expected satisfying spot set per species is pinned, not merely counted,
because PLANT_CARE.md §9 states which spot answers for which species precisely
so that a test author cannot pick a failing one.

Run standalone, or imported by tools/compile_content.py, which validates before
it emits so a bad number can never reach the generated header.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONTENT = ROOT / "content"

# ---------------------------------------------------------------------------
# Enumerations. A string field is range-checked against its enumeration exactly
# the way a numeric field is range-checked against its interval.
# ---------------------------------------------------------------------------

LIGHT_BANDS = ("LOW", "MEDIUM", "BRIGHT_INDIRECT", "DIRECT")
LIGHT_ORDER = {band: index for index, band in enumerate(LIGHT_BANDS)}
FLAGS = ("NYCTINASTY", "VARIEGATED", "CAM", "FLOWERS", "TRAILING", "PATTERNED")
PROPAGATION_MODES = ("NODE_CUTTING", "DIVISION")
WATER_LEVELS = ("MINIMUM", "LOW", "RISING", "NEAR_PEAK", "PEAK", "FALLING")
FEED_LEVELS = ("NONE", "QUARTER", "HALF", "LAST_FULL")
REPOT_LEVELS = ("DISCOURAGED", "GOOD", "BEST", "CLOSING", "LAST")

LEVEL_MAX = 10000
Q12_ONE = 4096

# ---------------------------------------------------------------------------
# The cited ranges. (low, high, citation) for numbers; (tuple, citation) for
# enumerations; ("str", citation) for free text; ("bool", citation) for flags.
# ---------------------------------------------------------------------------

SPECIES_RANGES = {
    "numeric_id": (0, 3, "ARCHITECTURE.md §6.2 — stable append-only ids, four species"),
    "id": ("str", "PLANT_CARE.md §9 — pg_species.id, stable and append-only"),
    "common_name": ("str", "PLANT_CARE.md §3 — the common name as the research names it"),
    "botanical_name": ("str", "PLANT_CARE.md §3 — the botanical name as the research names it"),
    "default_plant_name": ("str", "PLANT_CARE.md §3 — the game's default plant name"),
    "difficulty": (1, 5, "PLANT_CARE.md §2 — difficulty 1 (pothos) to 5 (calathea)"),

    "water_interval_growing_dh": (
        40, 210,
        "PLANT_CARE.md §2 and §9 — 4-7 d (calathea) to 14-21 d (snake plant) at the §1.3 baseline, [MED] on the day-count"),
    "water_interval_dormant_dh": (
        70, 560,
        "PLANT_CARE.md §2 and §9 — 7-10 d (calathea) to 28-56 d (snake plant), [MED] on the day-count"),
    "thirst_threshold_l": (
        0, 6000,
        "PLANT_CARE.md §9 — 0.05 (snake plant, dry all the way through) to 0.55 (calathea, never dry through)"),
    "saturation_tolerance_hours": (
        24, 168,
        "PLANT_CARE.md §4.5 — about 36 h (snake plant) to about 120 h (pothos) above the saturation threshold"),
    "drought_recovery_hours": (
        2, 48,
        "PLANT_CARE.md §6.3 — 2 h (peace lily) to 48 h (snake plant) to full turgor"),

    "light_min": (LIGHT_BANDS, "PLANT_CARE.md §9 — PG_LIGHT_LOW..PG_LIGHT_DIRECT"),
    "light_ideal": (LIGHT_BANDS, "PLANT_CARE.md §9 — PG_LIGHT_LOW..PG_LIGHT_DIRECT"),
    "light_max": (LIGHT_BANDS, "PLANT_CARE.md §9 — PG_LIGHT_LOW..PG_LIGHT_DIRECT; a ceiling, not a preference"),
    "dli_maintenance_c": (
        100, 300,
        "PLANT_CARE.md §9 with D-093 — 1.5 (snake plant, calathea) to 2 (pothos, peace lily) mol/m2/day"),
    "dli_thriving_c": (
        400, 1200,
        "PLANT_CARE.md §9 with D-084 — 5 (calathea) to 10 (snake plant) mol/m2/day"),
    "dli_flower_c": (
        0, 1500,
        "PLANT_CARE.md §9 — 0 for a species that does not flower; 12 mol/m2/day for the peace lily"),

    "rh_ideal_low": (
        0, 70,
        "PLANT_CARE.md §9 — 40 % (pothos) to 60 % (calathea); 0 is the genuinely-indifferent sentinel"),
    "rh_ideal_high": (
        0, 80,
        "PLANT_CARE.md §9 — 60 % to 75 %; 0 is the genuinely-indifferent sentinel"),
    "rh_damage_below": (
        0, 60,
        "PLANT_CARE.md §9 — 30 % (pothos) to 50 % (calathea); 0 means never damaged by dry air"),
    "temp_ideal_low": (10, 22, "PLANT_CARE.md §3 — 18 C across all four"),
    "temp_ideal_high": (22, 32, "PLANT_CARE.md §3 — 26 C (calathea) to 27 C"),
    "temp_damage_below": (5, 18, "PLANT_CARE.md §3 — 10 C (pothos, snake plant) to 15 C (calathea)"),

    "feed_interval_days": (
        21, 84,
        "PLANT_CARE.md §3 — every 4 weeks (calathea) to 2-3 times a season (snake plant)"),
    "feed_strength_q12": (
        1024, Q12_ONE,
        "PLANT_CARE.md §3 — quarter to half label strength; never above full"),
    "salt_sensitivity_q12": (
        0, Q12_ONE,
        "PLANT_CARE.md §9 — 0.3 (pothos) to 0.9 (calathea)"),
    "water_purity_sensitivity_q12": (
        0, Q12_ONE,
        "PLANT_CARE.md §9 — 0.1 (pothos, snake plant) to 1.0 (calathea, which wants distilled water)"),

    "repot_interval_months": (
        12, 60,
        "PLANT_CARE.md §3 — every 12-24 months, and 3-5 years for the snake plant"),
    "rootbound_tolerance_l": (
        0, LEVEL_MAX,
        "PLANT_CARE.md §9 — 0.5 (peace lily, calathea) to 0.9 (snake plant, which likes being tight)"),

    "scar_permanence_q12": (
        0, Q12_ONE,
        "PLANT_CARE.md §9 and §4.6 — 0.2 (pothos) to 1.0 (calathea, whose damage is total)"),
    "rot_survival_l": (
        0, LEVEL_MAX,
        "PLANT_CARE.md §9 and §6.3 — 0.35 to 0.6 odds a cutting or division takes; a real gamble, not a free undo"),
    "propagation_mode": (PROPAGATION_MODES, "PLANT_CARE.md §3 — node cutting (pothos) or division"),
    "flags": (FLAGS, "PLANT_CARE.md §9 — the flag column of the species table"),
}

POT_RANGES = {
    "numeric_id": (0, 3, "ARCHITECTURE.md §6.2 — stable append-only ids, four pots"),
    "id": ("str", "GAME_DESIGN.md §3.1 — the stable pot id"),
    "display_name": ("str", "GAME_DESIGN.md §3.1 — the pot's name in the fiction"),
    "material": ("str", "PLANT_CARE.md §5 — the four archetypes"),

    "drain_multiplier_q12": (
        2662, 5734,
        "PLANT_CARE.md §5 — x0.65 (cachepot) to x1.4 (terracotta); the pots span x2.15 end to end"),
    "side_evaporation_share_q12": (
        0, 2048,
        "GAME_DESIGN.md §3.1 — 0.00 (glazed, cachepot) to 0.45 (terracotta, porous walls); design"),
    "reservoir_q12": (
        3277, 4915,
        "GAME_DESIGN.md §3.1 — relative volume 0.95 to 1.20; design"),
    "bottom_floor_l": (
        0, LEVEL_MAX,
        "PLANT_CARE.md §4.5 and D-082 — 0 for a pot that drains; 0.92 for the cachepot, strictly above saturation"),
    "soil_temp_offset_c_x10": (
        -20, 10,
        "PLANT_CARE.md §5 — terracotta runs 1-2 C cooler [MED]; thin plastic runs slightly warmer; design elsewhere"),
    "salt_retention_q12": (
        2867, 5734,
        "GAME_DESIGN.md §3.1 — x0.7 (terracotta wicks it out) to x1.4 (cachepot cannot be flushed)"),

    "has_drainage": ("bool", "PLANT_CARE.md §5 — only the cachepot has none"),
    "is_sleeve_capable": ("bool", "PLANT_CARE.md §5 — the cachepot's correct use is as a sleeve"),
    "is_sleeve_liner": ("bool", "PLANT_CARE.md §5 — the nursery pot is what goes inside the sleeve"),
    "can_be_flushed": ("bool", "PLANT_CARE.md §5 — a cachepot cannot be flushed, so salts only accumulate"),
    "shows_salt_crust": ("bool", "PLANT_CARE.md §5 — terracotta grows a white mineral crust on its outside"),
    "shows_roots_at_holes": ("bool", "PLANT_CARE.md §5 — the earliest and clearest rootbound signal available"),
    "tip_risk_l": (0, LEVEL_MAX, "GAME_DESIGN.md §3.1 — stability, inverted; design"),
    "style": (1, 5, "GAME_DESIGN.md §3.1 — cosmetic style rating; design"),
}

POT_TOP_LEVEL_RANGES = {
    "saturation_level": (
        8500, 8500,
        "PLANT_CARE.md §4.5 and D-082 — PG_SATURATION_L is 0.85, species-independent, authored exactly once"),
    "uptake_share_q12": (
        1024, 2458,
        "GAME_DESIGN.md §4.1 — root uptake's share of the drain chain; design"),
    "percolation_q12": (
        0, 1024,
        "GAME_DESIGN.md §4.1 — gravity transfer from the top layer to the bottom; design"),
}

SPOT_RANGES = {
    "numeric_id": (0, 5, "GAME_DESIGN.md §8.4 — six stable, append-only spot ids"),
    "id": ("str", "GAME_DESIGN.md §8.4 — the stable spot id"),
    "display_name": ("str", "GAME_DESIGN.md §8.4 — the spot's name in the fiction"),
    "light_band": (LIGHT_BANDS, "PLANT_CARE.md §4.4 — the spot's light category"),
    "dli_peak_c": (
        100, 2000,
        "PLANT_CARE.md §4.4 — 3.0 (north shelf) to 18.0 (south sill) mol/m2/day, growing-season maximum"),
    "dli_floor_c": (
        50, 800,
        "PLANT_CARE.md §4.4 — 1.0 (north shelf) to 6.0 (south sill) mol/m2/day, midwinter minimum"),
    "default_scene": ("str", "GAME_DESIGN.md §8.4 — the suggested plate; cosmetic only (D-041)"),

    "temp_summer_day_c_x10": (150, 300, "PLANT_CARE.md §1.3 — 20-22 C baseline room, warmer in a sun patch or over a radiator"),
    "temp_summer_night_c_x10": (100, 260, "PLANT_CARE.md §1.3 with GAME_DESIGN.md §8.4 — the sill runs 10 C colder at night"),
    "temp_winter_day_c_x10": (140, 300, "PLANT_CARE.md §1.3 — a heated room in winter; the radiator shelf runs hottest"),
    "temp_winter_night_c_x10": (80, 260, "PLANT_CARE.md §3 — cold damage thresholds run 10-15 C, so a sill must be able to reach them"),
    "rh_summer_pct": (20, 80, "PLANT_CARE.md §1.3 and §4.2 — 45 % baseline; a bathroom is the humid end"),
    "rh_winter_pct": (15, 70, "PLANT_CARE.md §3.4 — heating drops indoor RH to 20-30 %"),
    "airflow_q12": (
        3277, 4915,
        "PLANT_CARE.md §4.2 — draughty or a fan is x1.2; still air is x1.0"),
    "cold_glass": ("bool", "PLANT_CARE.md §6.1 — a leaf resting against cold window glass overnight"),
}

SEASON_TOP_LEVEL_RANGES = {
    "solar_noon_local_minutes": (
        720, 840,
        "PLANT_CARE.md §4.3 and D-080 — 13:00 local, range 720-840, a modelling choice [MED]"),
    "hemisphere_offset_days": (
        182, 182,
        "PLANT_CARE.md §4.3 — southern hemisphere offsets the day ordinal by 182"),
}

SEASON_CURVE_RANGES = {
    "mean_hours": (12.2, 12.2, "PLANT_CARE.md §4.3 — daylight_hours(d) = 12.2 + 3.3*cos(...) at ~45 N"),
    "amplitude_hours": (3.3, 3.3, "PLANT_CARE.md §4.3 — the same curve"),
    "peak_ordinal": (172, 172, "PLANT_CARE.md §4.3 — the June solstice ordinal in the curve"),
    "period_days": (365, 365, "PLANT_CARE.md §4.3 — the curve's period"),
}

SEASON_SCALAR_RANGES = {
    "offset_hours": (8.5, 8.5, "PLANT_CARE.md §4.3 — season_scalar = clamp01((daylight_hours - 8.5) / 7.0)"),
    "span_hours": (7.0, 7.0, "PLANT_CARE.md §4.3 — the same expression"),
    "growing_threshold_l": (
        4500, 4500,
        "PLANT_CARE.md §4.3 and §8 item 3 — 0.45 is a modelling choice chosen to reproduce the traditional feeding window"),
    "first_growing_ordinal": (72, 72, "PLANT_CARE.md §4.3 — season_scalar > 0.45 holds from ordinal 72"),
    "last_growing_ordinal": (272, 272, "PLANT_CARE.md §4.3 — ...through ordinal 272 inclusive"),
}

SEASON_RECOVERY_RANGES = {
    "effective_scalar_floor_q12": (
        1229, 1229,
        "PLANT_CARE.md §4.3 and D-085 — effective_scalar = max(growth_scalar, 0.30), so no recovery exceeds 3.33x"),
}

MULTIPLIER_RANGES = {
    "light": {
        "DIRECT": (5530, 5530, "PLANT_CARE.md §4.2 — direct light x1.35 [MED]"),
        "BRIGHT_INDIRECT": (4096, 4096, "PLANT_CARE.md §4.2 — bright indirect x1.0, the baseline"),
        "MEDIUM": (3072, 3072, "PLANT_CARE.md §4.2 — medium light x0.75 [MED]"),
        "LOW": (2253, 2253, "PLANT_CARE.md §4.2 — low light x0.55 [MED]; low light causes overwatering"),
    },
    "temperature": {
        "above_26_c": (5325, 5325, "PLANT_CARE.md §4.2 — above 26 C x1.3 [MED]"),
        "band_20_26_c": (4096, 4096, "PLANT_CARE.md §4.2 — 20-26 C x1.0, the baseline"),
        "band_16_20_c": (3072, 3072, "PLANT_CARE.md §4.2 — 16-20 C x0.75 [MED]"),
        "below_16_c": (2253, 2253, "PLANT_CARE.md §4.2 — below 16 C x0.55 [MED]"),
    },
    "humidity": {
        "below_35_pct": (4915, 4915, "PLANT_CARE.md §4.2 — below 35 % RH x1.2 [MED]"),
        "band_35_60_pct": (4096, 4096, "PLANT_CARE.md §4.2 — 35-60 % RH x1.0, the baseline"),
        "above_60_pct": (3277, 3277, "PLANT_CARE.md §4.2 — above 60 % RH x0.8 [MED]"),
    },
    "season": {
        "growing_q12": (4096, 4096, "PLANT_CARE.md §4.2 — growth scalar 1.0 is x1.0"),
        "dormant_q12": (1843, 1843, "PLANT_CARE.md §4.2 — the drain multiplier falls to x0.45 at midwinter"),
    },
    "root_mass": {
        "sparse": (2867, 2867, "PLANT_CARE.md §4.2 — sparse roots x0.7 [MED]"),
        "established": (4096, 4096, "PLANT_CARE.md §4.2 — established roots x1.0, the baseline"),
        "rootbound": (6554, 6554, "PLANT_CARE.md §4.2 — rootbound x1.6; a rootbound plant dries roughly twice as fast"),
    },
}

MONTH_RANGES = {
    "month": (1, 12, "the research's month-by-month care calendar, northern hemisphere"),
    "water": (WATER_LEVELS, "the research's month-by-month care calendar — minimum in January, peak in July"),
    "feed": (FEED_LEVELS, "PLANT_CARE.md §3 and the research's calendar — start in March at quarter, nothing November-February"),
    "repot": (REPOT_LEVELS, "the research's calendar — March is the best repotting month; do not repot in December"),
}

# ---------------------------------------------------------------------------
# The §9 transcription table, pinned exactly. A range catches a wild number; an
# exact table catches a transposed one, which is the failure mode a direct
# transcription actually has.
# ---------------------------------------------------------------------------

SPECIES_EXACT = {
    "golden-pothos": {
        "difficulty": 1,
        "water_interval_growing_dh": 85, "water_interval_dormant_dh": 150,
        "thirst_threshold_l": 3500, "saturation_tolerance_hours": 120,
        "drought_recovery_hours": 4,
        "light_min": "LOW", "light_ideal": "BRIGHT_INDIRECT", "light_max": "DIRECT",
        "dli_maintenance_c": 200, "dli_thriving_c": 800, "dli_flower_c": 0,
        "rh_ideal_low": 40, "rh_ideal_high": 60, "rh_damage_below": 30,
        "temp_ideal_low": 18, "temp_ideal_high": 27, "temp_damage_below": 10,
        "feed_interval_days": 35, "feed_strength_q12": 2048,
        "salt_sensitivity_q12": 1229, "water_purity_sensitivity_q12": 410,
        "repot_interval_months": 21, "rootbound_tolerance_l": 6000,
        "scar_permanence_q12": 819, "rot_survival_l": 6000,
        "propagation_mode": "NODE_CUTTING",
    },
    "snake-plant-laurentii": {
        "difficulty": 1,
        "water_interval_growing_dh": 170, "water_interval_dormant_dh": 420,
        "thirst_threshold_l": 500, "saturation_tolerance_hours": 36,
        "drought_recovery_hours": 48,
        "light_min": "LOW", "light_ideal": "BRIGHT_INDIRECT", "light_max": "DIRECT",
        "dli_maintenance_c": 150, "dli_thriving_c": 1000, "dli_flower_c": 0,
        "rh_ideal_low": 0, "rh_ideal_high": 0, "rh_damage_below": 0,
        "temp_ideal_low": 18, "temp_ideal_high": 27, "temp_damage_below": 10,
        "feed_interval_days": 70, "feed_strength_q12": 2048,
        "salt_sensitivity_q12": 1638, "water_purity_sensitivity_q12": 410,
        "repot_interval_months": 48, "rootbound_tolerance_l": 9000,
        "scar_permanence_q12": 2458, "rot_survival_l": 3500,
        "propagation_mode": "DIVISION",
    },
    "peace-lily": {
        "difficulty": 3,
        "water_interval_growing_dh": 65, "water_interval_dormant_dh": 100,
        "thirst_threshold_l": 5000, "saturation_tolerance_hours": 96,
        "drought_recovery_hours": 3,
        "light_min": "LOW", "light_ideal": "MEDIUM", "light_max": "BRIGHT_INDIRECT",
        "dli_maintenance_c": 200, "dli_thriving_c": 800, "dli_flower_c": 1200,
        "rh_ideal_low": 50, "rh_ideal_high": 60, "rh_damage_below": 40,
        "temp_ideal_low": 18, "temp_ideal_high": 27, "temp_damage_below": 13,
        "feed_interval_days": 50, "feed_strength_q12": 2048,
        "salt_sensitivity_q12": 2867, "water_purity_sensitivity_q12": 2458,
        "repot_interval_months": 18, "rootbound_tolerance_l": 5000,
        "scar_permanence_q12": 2048, "rot_survival_l": 4500,
        "propagation_mode": "DIVISION",
    },
    "calathea": {
        "difficulty": 5,
        "water_interval_growing_dh": 55, "water_interval_dormant_dh": 85,
        "thirst_threshold_l": 5500, "saturation_tolerance_hours": 72,
        "drought_recovery_hours": 12,
        "light_min": "MEDIUM", "light_ideal": "MEDIUM", "light_max": "BRIGHT_INDIRECT",
        # D-093: maintenance is 1.5, not 3. Thriving stays 5 (D-084).
        "dli_maintenance_c": 150, "dli_thriving_c": 500, "dli_flower_c": 0,
        "rh_ideal_low": 60, "rh_ideal_high": 75, "rh_damage_below": 50,
        "temp_ideal_low": 18, "temp_ideal_high": 26, "temp_damage_below": 15,
        "feed_interval_days": 30, "feed_strength_q12": 1434,
        "salt_sensitivity_q12": 3686, "water_purity_sensitivity_q12": 4096,
        "repot_interval_months": 18, "rootbound_tolerance_l": 5000,
        "scar_permanence_q12": 4096, "rot_survival_l": 3500,
        "propagation_mode": "DIVISION",
    },
}

SPECIES_FLAGS_EXACT = {
    "golden-pothos": ["TRAILING", "VARIEGATED"],
    "snake-plant-laurentii": ["CAM", "VARIEGATED"],
    "peace-lily": ["FLOWERS"],
    "calathea": ["NYCTINASTY", "PATTERNED"],
}

POT_DRAIN_EXACT = {
    "terracotta": 5734, "glazed": 4096, "nursery": 4301, "cachepot": 2662,
}

SPOT_DLI_EXACT = {
    "sill-south": (1800, 600),
    "one-metre": (1250, 400),
    "two-metre": (500, 180),
    "north-shelf": (300, 100),
    "radiator-shelf": (900, 300),
    "bath-shelf": (400, 140),
}

# PLANT_CARE.md §9 names which spots satisfy the reachability assertion for
# which species, precisely so that a test author cannot pick a failing one.
#
# The doc's own four-row table has the "needs" columns of its peace-lily and
# snake-plant rows transposed against the species values tabled directly above
# them (it asks the peace lily for peak 10 / floor 1.5, which are the snake
# plant's numbers, and vice versa). The species table is the transcription
# target and therefore the authority, so the sets below are derived from it —
# and they preserve every claim the doc's prose makes: the calathea's set
# contains spot 2, "the design's intended home", which is the whole of D-093;
# and the peace lily's set is spot 1 alone, "the only spot inside her light
# ceiling that gets there", which is why her flower is achievable, slow and
# light-gated rather than promised.
REACHABILITY_EXPECTED = {
    "golden-pothos": {0, 1, 4},
    "snake-plant-laurentii": {0, 1},
    "peace-lily": {1},
    "calathea": {1, 2, 4},
}

REACHABILITY_MUST_INCLUDE = {
    "calathea": (2, "GAME_DESIGN.md §8.4 calls the 2 m spot Nyx's happy place, and D-093 exists to make it satisfy both halves"),
    "peace-lily": (1, "PLANT_CARE.md §9 — the 1 m spot is the only place inside her light ceiling that reaches dli_flower"),
}


class Failures:
    def __init__(self) -> None:
        self.items: list[str] = []

    def add(self, message: str) -> None:
        self.items.append(message)

    def __bool__(self) -> bool:
        return bool(self.items)


def _load(name: str) -> dict:
    with (CONTENT / name).open("r", encoding="utf-8") as handle:
        return json.load(handle)


def _authored(mapping: dict) -> dict:
    """Keys that are data. A leading underscore marks documentation."""
    return {k: v for k, v in mapping.items() if not k.startswith("_")}


def _check_field(fail: Failures, where: str, key: str, value: object,
                 ranges: dict) -> None:
    spec = ranges.get(key)
    if spec is None:
        fail.add(f"{where}: field '{key}' has no cited range — an unsourced "
                 f"number is an error, not a pass (PLANT_CARE.md §preamble)")
        return
    kind = spec[0]
    if kind == "str":
        if not isinstance(value, str) or not value:
            fail.add(f"{where}.{key}: expected a non-empty string ({spec[1]})")
        return
    if kind == "bool":
        if not isinstance(value, bool):
            fail.add(f"{where}.{key}: expected a boolean ({spec[1]})")
        return
    if isinstance(kind, tuple):
        values = value if isinstance(value, list) else [value]
        for item in values:
            if item not in kind:
                fail.add(f"{where}.{key}: '{item}' is not one of "
                         f"{', '.join(kind)} ({spec[1]})")
        return
    low, high, citation = spec
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        fail.add(f"{where}.{key}: expected a number ({citation})")
        return
    if not low <= value <= high:
        fail.add(f"{where}.{key}: {value} is outside the cited range "
                 f"[{low}, {high}] ({citation})")


def _check_entry(fail: Failures, where: str, entry: dict, ranges: dict,
                 required: bool = True) -> None:
    authored = _authored(entry)
    for key, value in authored.items():
        _check_field(fail, where, key, value, ranges)
    if required:
        missing = sorted(set(ranges) - set(authored))
        if missing:
            fail.add(f"{where}: missing required fields: {', '.join(missing)}")


def _check_exact(fail: Failures, where: str, entry: dict, expected: dict) -> None:
    for key, want in expected.items():
        got = entry.get(key)
        if got != want:
            fail.add(f"{where}.{key}: {got!r} is not the PLANT_CARE.md §9 "
                     f"value {want!r}")


def check_species(fail: Failures, plants: dict) -> None:
    entries = plants["entries"]
    if len(entries) != 4:
        fail.add(f"content/plants.json: {len(entries)} species, expected exactly 4 "
                 f"(GAME_DESIGN.md §2 — the four are fixed)")
    seen_ids = set()
    for index, entry in enumerate(entries):
        where = f"plants[{index}]"
        _check_entry(fail, where, entry, SPECIES_RANGES)
        key = entry.get("id")
        if key in seen_ids:
            fail.add(f"{where}.id: '{key}' is duplicated")
        seen_ids.add(key)
        if entry.get("numeric_id") != index:
            fail.add(f"{where}.numeric_id: {entry.get('numeric_id')} does not "
                     f"match its position {index}; ids are append-only (D-044)")
        if key in SPECIES_EXACT:
            _check_exact(fail, where, entry, SPECIES_EXACT[key])
            flags = sorted(entry.get("flags", []))
            if flags != SPECIES_FLAGS_EXACT[key]:
                fail.add(f"{where}.flags: {flags} is not the PLANT_CARE.md §9 "
                         f"flag set {SPECIES_FLAGS_EXACT[key]}")
        elif key is not None:
            fail.add(f"{where}.id: '{key}' is not one of the four species "
                     f"PLANT_CARE.md §9 tables")

        # The RH sentinel is a pair, and half of it is a contradiction.
        low, high = entry.get("rh_ideal_low"), entry.get("rh_ideal_high")
        if (low == 0) != (high == 0):
            fail.add(f"{where}: rh_ideal_low/high — 0 is the "
                     f"genuinely-indifferent sentinel and must be set on both "
                     f"(PLANT_CARE.md §9 footnote 3)")
        if low and high and low > high:
            fail.add(f"{where}: rh_ideal_low {low} exceeds rh_ideal_high {high}")
        if entry.get("temp_ideal_low", 0) > entry.get("temp_ideal_high", 0):
            fail.add(f"{where}: temp_ideal_low exceeds temp_ideal_high")
        if entry.get("temp_damage_below", 0) >= entry.get("temp_ideal_low", 0):
            fail.add(f"{where}: temp_damage_below must sit below temp_ideal_low")
        if entry.get("dli_maintenance_c", 0) >= entry.get("dli_thriving_c", 0):
            fail.add(f"{where}: dli_maintenance_c must sit below dli_thriving_c")
        if LIGHT_ORDER.get(entry.get("light_min"), 0) > \
           LIGHT_ORDER.get(entry.get("light_ideal"), 0):
            fail.add(f"{where}: light_min is above light_ideal")
        if LIGHT_ORDER.get(entry.get("light_ideal"), 0) > \
           LIGHT_ORDER.get(entry.get("light_max"), 0):
            fail.add(f"{where}: light_ideal is above light_max")


def check_pots(fail: Failures, pots: dict) -> None:
    for key, value in _authored(pots).items():
        if key in ("schema_version", "entries"):
            continue
        _check_field(fail, "pots", key, value, POT_TOP_LEVEL_RANGES)
    entries = pots["entries"]
    if len(entries) != 4:
        fail.add(f"content/pots.json: {len(entries)} pots, expected exactly 4 "
                 f"(GAME_DESIGN.md §3 — the four are fixed)")
    saturation = pots.get("saturation_level")
    for index, entry in enumerate(entries):
        where = f"pots[{index}]"
        _check_entry(fail, where, entry, POT_RANGES)
        key = entry.get("id")
        if entry.get("numeric_id") != index:
            fail.add(f"{where}.numeric_id: {entry.get('numeric_id')} does not "
                     f"match its position {index}; ids are append-only (D-044)")
        want = POT_DRAIN_EXACT.get(key)
        if want is None:
            fail.add(f"{where}.id: '{key}' is not one of the four pots "
                     f"PLANT_CARE.md §5 tables")
        elif entry.get("drain_multiplier_q12") != want:
            fail.add(f"{where}.drain_multiplier_q12: "
                     f"{entry.get('drain_multiplier_q12')} is not the "
                     f"PLANT_CARE.md §5 value {want}")
        floor = entry.get("bottom_floor_l", 0)
        if entry.get("has_drainage") and floor != 0:
            fail.add(f"{where}: a pot with a drainage hole has no perched water "
                     f"table, so bottom_floor_l must be 0")
        if not entry.get("has_drainage", True):
            if floor <= saturation:
                fail.add(f"{where}: bottom_floor_l {floor} must sit STRICTLY "
                         f"above saturation_level {saturation}, or the soggy "
                         f"ticks decay at rest and D-082's whole justification "
                         f"for the cachepot being lethal fails")
            if entry.get("can_be_flushed"):
                fail.add(f"{where}: a pot with no drainage cannot be flushed "
                         f"(PLANT_CARE.md §5)")
    shares = pots.get("uptake_share_q12", 0)
    for index, entry in enumerate(entries):
        side = entry.get("side_evaporation_share_q12", 0)
        if side + shares > Q12_ONE:
            fail.add(f"pots[{index}]: side evaporation plus root uptake exceeds "
                     f"the whole drain chain")


def check_spots(fail: Failures, spots: dict) -> None:
    entries = spots["entries"]
    if len(entries) != 6:
        fail.add(f"content/spots.json: {len(entries)} spots, expected exactly 6 "
                 f"(GAME_DESIGN.md §8.4)")
    for index, entry in enumerate(entries):
        where = f"spots[{index}]"
        _check_entry(fail, where, entry, SPOT_RANGES)
        if entry.get("numeric_id") != index:
            fail.add(f"{where}.numeric_id: {entry.get('numeric_id')} does not "
                     f"match its position {index}; ids are append-only (D-081)")
        key = entry.get("id")
        want = SPOT_DLI_EXACT.get(key)
        if want is None:
            fail.add(f"{where}.id: '{key}' is not one of the six spots "
                     f"PLANT_CARE.md §4.4 tables")
        elif (entry.get("dli_peak_c"), entry.get("dli_floor_c")) != want:
            fail.add(f"{where}: DLI peak/floor "
                     f"{entry.get('dli_peak_c')}/{entry.get('dli_floor_c')} is "
                     f"not the PLANT_CARE.md §4.4 pair {want[0]}/{want[1]}")
        if entry.get("dli_floor_c", 0) >= entry.get("dli_peak_c", 0):
            fail.add(f"{where}: the midwinter floor must sit below the "
                     f"growing-season peak")
        if entry.get("temp_summer_night_c_x10", 0) > \
           entry.get("temp_summer_day_c_x10", 0):
            fail.add(f"{where}: summer night is warmer than summer day")
        if entry.get("temp_winter_night_c_x10", 0) > \
           entry.get("temp_winter_day_c_x10", 0):
            fail.add(f"{where}: winter night is warmer than winter day")


def check_seasons(fail: Failures, seasons: dict) -> None:
    for key, value in _authored(seasons).items():
        if key in ("schema_version", "daylight_curve", "season_scalar",
                   "recovery", "multiplier_chain", "months"):
            continue
        _check_field(fail, "seasons", key, value, SEASON_TOP_LEVEL_RANGES)
    _check_entry(fail, "seasons.daylight_curve",
                 seasons["daylight_curve"], SEASON_CURVE_RANGES)
    _check_entry(fail, "seasons.season_scalar",
                 seasons["season_scalar"], SEASON_SCALAR_RANGES)
    _check_entry(fail, "seasons.recovery",
                 seasons["recovery"], SEASON_RECOVERY_RANGES)

    chain = _authored(seasons["multiplier_chain"])
    for group, values in chain.items():
        ranges = MULTIPLIER_RANGES.get(group)
        if ranges is None:
            fail.add(f"seasons.multiplier_chain: group '{group}' has no cited "
                     f"range (PLANT_CARE.md §4.2)")
            continue
        _check_entry(fail, f"seasons.multiplier_chain.{group}",
                     values, ranges)
    missing = sorted(set(MULTIPLIER_RANGES) - set(chain))
    if missing:
        fail.add(f"seasons.multiplier_chain: missing groups: "
                 f"{', '.join(missing)}")

    months = seasons["months"]
    if len(months) != 12:
        fail.add(f"seasons.months: {len(months)} rows, expected 12")
    for index, row in enumerate(months):
        _check_entry(fail, f"seasons.months[{index}]", row, MONTH_RANGES)
        if row.get("month") != index + 1:
            fail.add(f"seasons.months[{index}]: month {row.get('month')} is out "
                     f"of order")
    # The feeding window must fall out of the curve, not contradict it.
    for row in months:
        if row.get("month") in (11, 12, 1, 2) and row.get("feed") != "NONE":
            fail.add(f"seasons.months: month {row.get('month')} feeds during "
                     f"dormancy, which is hard refusal 1 (PLANT_CARE.md §6.4)")


def check_reachability(fail: Failures, plants: dict, spots: dict) -> None:
    """D-084, amended by D-093. The assertion the whole spot table exists for."""
    entries = spots["entries"]
    for species in plants["entries"]:
        key = species.get("id")
        ceiling = LIGHT_ORDER.get(species.get("light_max"), -1)
        thriving = species.get("dli_thriving_c", 0)
        maintenance = species.get("dli_maintenance_c", 0)
        flower = species.get("dli_flower_c", 0)

        satisfying = set()
        for spot in entries:
            if LIGHT_ORDER.get(spot.get("light_band"), 99) > ceiling:
                continue
            if spot.get("dli_peak_c", 0) < thriving:
                continue
            if flower and spot.get("dli_peak_c", 0) < flower:
                continue
            if spot.get("dli_floor_c", 0) < maintenance:
                continue
            satisfying.add(spot.get("numeric_id"))

        if not satisfying:
            fail.add(f"reachability: '{key}' can reach dli_thriving_c "
                     f"{thriving}"
                     + (f" / dli_flower_c {flower}" if flower else "")
                     + f" and hold dli_maintenance_c {maintenance} at NO spot "
                     f"inside light_max {species.get('light_max')} — a "
                     f"threshold nobody can reach is a promise the game cannot "
                     f"keep (D-084)")
        expected = REACHABILITY_EXPECTED.get(key)
        if expected is not None and satisfying != expected:
            fail.add(f"reachability: '{key}' is satisfied by spots "
                     f"{sorted(satisfying)}, but PLANT_CARE.md §9 names "
                     f"{sorted(expected)}")
        must = REACHABILITY_MUST_INCLUDE.get(key)
        if must is not None and must[0] not in satisfying:
            fail.add(f"reachability: '{key}' must be satisfied at spot "
                     f"{must[0]} — {must[1]}")


def validate() -> list[str]:
    fail = Failures()
    plants = _load("plants.json")
    pots = _load("pots.json")
    spots = _load("spots.json")
    seasons = _load("seasons.json")

    check_species(fail, plants)
    check_pots(fail, pots)
    check_spots(fail, spots)
    check_seasons(fail, seasons)
    check_reachability(fail, plants, spots)
    return fail.items


def main() -> int:
    failures = validate()
    if failures:
        for item in failures:
            print(f"check-care-schedule: {item}", file=sys.stderr)
        return 1
    print("check-care-schedule: PASS "
          "(4 species, 4 pots, 6 spots, 12 months, every number cited, "
          "every threshold reachable)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
