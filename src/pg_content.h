/*
 * pg_content — accessors over the compiled content tables.
 *
 * content/plants.json, pots.json, spots.json and seasons.json are compiled by
 * tools/compile_content.py into build/pg_content_generated.h, which this
 * module is the only translation unit to include. Everything else asks by
 * stable id and gets a const pointer; nothing else ever sees a table.
 *
 * The species record below is PLANT_CARE.md §9's pg_species struct field for
 * field, suffixes included, plus four fields the compiler derives and marks as
 * derived. That section declares itself the direct transcription target, so
 * the field names here and the JSON keys there are deliberately identical.
 *
 * Every number is fixed point on the one convention of ARCHITECTURE.md §6.2
 * (D-083): `_l` levels 0..10000, `_q12` multipliers with 4096 = x1.0, `_c`
 * centi-units of DLI, and whole hours, days, months or care ticks for
 * intervals. Read the suffixes: a threshold is a level, because it is compared
 * against a level, while a sensitivity is a multiplier, because it multiplies
 * something.
 */
#ifndef PG_CONTENT_H
#define PG_CONTENT_H

#include "pleb_plant_grower.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Four care ticks to the hour, ninety-six to the day. Stated once here because
 * every hour-denominated number in PLANT_CARE.md §9 goes through it, and a
 * stray factor of four is the failure this constant exists to prevent. */
#define PG_CARE_TICKS_PER_HOUR 4u
#define PG_CARE_TICKS_PER_DAY  96u

/* Light is a band, not a number: the species table states a floor, an
 * optimum and a ceiling in these terms, and the spot table states what a place
 * delivers in the same terms (PLANT_CARE.md §4.4). */
typedef enum pg_light_band {
    PG_LIGHT_LOW = 0,
    PG_LIGHT_MEDIUM = 1,
    PG_LIGHT_BRIGHT_INDIRECT = 2,
    PG_LIGHT_DIRECT = 3,
    PG_LIGHT_BAND_COUNT = 4
} pg_light_band;

typedef enum pg_propagation_mode {
    PG_PROPAGATION_NODE_CUTTING = 0,
    PG_PROPAGATION_DIVISION = 1
} pg_propagation_mode;

/* Species flags. Each one is a mechanic somewhere: nyctinasty is the calathea's
 * fold, variegation is what low light destroys permanently, CAM is why the
 * snake plant barely drinks, flowering is the peace lily's spathe, trailing is
 * the pothos's vine chain, and a pattern is a health channel of its own. */
typedef enum pg_species_flag {
    PG_FLAG_NYCTINASTY = 1u << 0,
    PG_FLAG_VARIEGATED = 1u << 1,
    PG_FLAG_CAM = 1u << 2,
    PG_FLAG_FLOWERS = 1u << 3,
    PG_FLAG_TRAILING = 1u << 4,
    PG_FLAG_PATTERNED = 1u << 5
} pg_species_flag;

/* The advisory month table. Nothing in the simulation branches on a month —
 * the season is derived from the daylight curve (PLANT_CARE.md §4.3) — but the
 * calendar screen and the notebook need somewhere honest to read from. */
typedef enum pg_month_water { PG_WATER_MINIMUM = 0, PG_WATER_LOW, PG_WATER_RISING,
    PG_WATER_NEAR_PEAK, PG_WATER_PEAK, PG_WATER_FALLING } pg_month_water;
typedef enum pg_month_feed { PG_FEED_NONE = 0, PG_FEED_QUARTER, PG_FEED_HALF,
    PG_FEED_LAST_FULL } pg_month_feed;
typedef enum pg_month_repot { PG_REPOT_DISCOURAGED = 0, PG_REPOT_GOOD,
    PG_REPOT_BEST, PG_REPOT_CLOSING, PG_REPOT_LAST } pg_month_repot;

typedef struct pg_species {
    const char *id;                    /* stable, append-only                 */
    const char *common_name;
    const char *botanical_name;
    const char *default_plant_name;
    uint8_t  numeric_id;
    uint8_t  difficulty;               /* 1 gentle .. 5 devoted               */

    /* water */
    uint16_t water_interval_growing_dh;  /* deci-days at the §1.3 baseline    */
    uint16_t water_interval_dormant_dh;
    uint16_t thirst_threshold_l;         /* moisture LEVEL at which it wants water */
    uint16_t saturation_tolerance_hours; /* soggy hours before root damage begins */
    uint16_t drought_recovery_hours;     /* turgor time constant after watering */

    /* light */
    uint8_t  light_min, light_ideal, light_max;   /* PG_LIGHT_LOW..PG_LIGHT_DIRECT */
    uint16_t dli_maintenance_c, dli_thriving_c;   /* centi-mol/m2/day         */
    uint16_t dli_flower_c;                        /* 0 = does not flower      */

    /* air.
     * rh_ideal_low == rh_ideal_high == 0 is the sentinel for "genuinely
     * indifferent", and the simulation drops the RH term entirely for that
     * species rather than applying a band. */
    uint8_t  rh_ideal_low, rh_ideal_high;         /* percent                  */
    uint8_t  rh_damage_below;                     /* 0 = never damaged by dry air */
    int8_t   temp_ideal_low, temp_ideal_high;     /* degrees C                */
    int8_t   temp_damage_below;

    /* feeding and water quality */
    uint16_t feed_interval_days;
    uint16_t feed_strength_q12;                   /* of label full strength   */
    uint16_t salt_sensitivity_q12;
    uint16_t water_purity_sensitivity_q12;

    /* roots */
    uint16_t repot_interval_months;
    uint16_t rootbound_tolerance_l;

    /* damage and rescue */
    uint16_t scar_permanence_q12;                 /* 1.0 = nothing ever comes back */
    uint16_t rot_survival_l;                      /* odds a cutting/division takes */
    uint8_t  propagation_mode;                    /* NODE_CUTTING | DIVISION  */
    uint8_t  flags;                               /* pg_species_flag bits     */

    /* Derived by the content compiler from PLANT_CARE.md §4.1's calibration,
     * never authored: base_drain_per_hour = (1 - thirst)/(interval_days * 24),
     * expressed per 900 s care tick on the level scale, in Q12. At the
     * baseline with every multiplier at 1.0 this reproduces the tabled
     * interval exactly; off baseline it does the right thing automatically. */
    uint32_t base_drain_growing_q12l;
    uint32_t base_drain_dormant_q12l;
    uint16_t saturation_tolerance_ticks;
    uint16_t drought_recovery_ticks;
} pg_species;

typedef struct pg_pot {
    const char *id;
    const char *display_name;
    const char *material;
    uint8_t  numeric_id;

    uint16_t drain_multiplier_q12;
    /* Give the soil two layers and every real pot behaviour falls out of where
     * the water leaves from: the surface, the porous wall, or the roots. The
     * three shares sum to 1.0 (GAME_DESIGN.md §3). */
    uint16_t side_evaporation_share_q12;
    uint16_t surface_share_q12;
    uint16_t uptake_share_q12;
    uint16_t reservoir_q12;
    uint16_t bottom_floor_l;             /* the cachepot's perched water table */
    int16_t  soil_temp_offset_c_x10;
    uint16_t salt_retention_q12;
    uint16_t tip_risk_l;
    uint8_t  style;

    bool has_drainage;
    bool is_sleeve_capable;              /* the cachepot, used correctly       */
    bool is_sleeve_liner;                /* the nursery pot that goes inside it */
    bool can_be_flushed;
    bool shows_salt_crust;
    bool shows_roots_at_holes;
} pg_pot;

typedef struct pg_spot {
    const char *id;
    const char *display_name;
    const char *default_scene;           /* cosmetic suggestion only (D-041)   */
    uint8_t  numeric_id;
    uint8_t  light_band;

    uint16_t dli_peak_c;                 /* growing-season maximum             */
    uint16_t dli_floor_c;                /* midwinter minimum                  */

    int16_t  temp_summer_day_c_x10, temp_summer_night_c_x10;
    int16_t  temp_winter_day_c_x10, temp_winter_night_c_x10;
    uint8_t  rh_summer_pct, rh_winter_pct;
    uint16_t airflow_q12;
    bool     cold_glass;                 /* a leaf can rest against it overnight */
} pg_spot;

typedef struct pg_multipliers {
    uint16_t light[PG_LIGHT_BAND_COUNT];
    uint16_t temp_above_26, temp_20_26, temp_16_20, temp_below_16;
    uint16_t rh_below_35, rh_35_60, rh_above_60;
    uint16_t season_growing, season_dormant;
    uint16_t root_sparse, root_established, root_bound;
} pg_multipliers;

typedef struct pg_season_month {
    uint8_t month;
    uint8_t water;
    uint8_t feed;
    uint8_t repot;
} pg_season_month;

/* ---- lookup by stable id. NULL for an id nothing was authored for. ---- */

size_t pg_content_species_count(void);
size_t pg_content_pot_count(void);
size_t pg_content_spot_count(void);

const pg_species *pg_content_species(uint8_t species_id);
const pg_pot     *pg_content_pot(uint8_t pot_id);
const pg_spot    *pg_content_spot(uint8_t spot_id);

/* Lookup by the authored string id, for tools and tests rather than for the
 * simulation, which always holds the numeric id the save file carries. */
const pg_species *pg_content_species_by_key(const char *key);
const pg_pot     *pg_content_pot_by_key(const char *key);
const pg_spot    *pg_content_spot_by_key(const char *key);

const pg_multipliers *pg_content_multipliers(void);
const pg_season_month *pg_content_month(uint8_t month);   /* 1..12 */

/* The one species-independent saturation constant (D-082). */
uint16_t pg_content_saturation_level(void);
uint16_t pg_content_percolation_q12(void);
uint16_t pg_content_recovery_floor_q12(void);

/* Every string the compiled content can put in front of a player. --rules-test
 * asserts the myth blocklist against this, which is why the check needs no
 * file I/O and holds inside the embed. */
size_t pg_content_string_count(void);
const char *pg_content_string(size_t index);

/* Self-check over the compiled tables: counts, id ordering, the saturation
 * relationship, and the D-084 reachability of every species threshold. Writes
 * at most error_size bytes on failure. */
bool pg_content_validate(char *error, size_t error_size);

#endif /* PG_CONTENT_H */
