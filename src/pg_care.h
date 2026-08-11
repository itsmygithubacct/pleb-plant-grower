/*
 * pg_care — the care model.
 *
 * Per-tick and per-day integration of the moisture, light, humidity, nutrition
 * and root axes against species x pot x spot x season tables. All integer, on
 * the one fixed-point convention of ARCHITECTURE.md §6.2 (D-083).
 *
 * The coupling this module exists to get right, and the one most plant games
 * miss: **light and temperature drive the drying rate.** The watering interval
 * is therefore derived — base_interval x pot_drain x light x temperature x
 * humidity x airflow x season x root_mass — and never a constant. A dim cold
 * room stretches the interval, which is exactly why a fixed "water every seven
 * days" schedule kills plants, and why low light causes *overwatering* rather
 * than merely slower growth.
 *
 * There is no "days until thirsty" anywhere in this file. Moisture is stored
 * and drained; the interval is a consequence.
 */
#ifndef PG_CARE_H
#define PG_CARE_H

#include "pleb_plant_grower.h"

#include "pg_content.h"
#include "pg_plant.h"

#include <stdbool.h>
#include <stdint.h>

/* ---- fixed-point helpers ------------------------------------------------ */

/* a * b with 4096 = 1.0, rounded to nearest, saturating at 16 bits. */
uint16_t pg_q12_mul(uint16_t a_q12, uint16_t b_q12);

/* value * multiplier_q12, rounded to nearest. Levels stay levels. */
uint16_t pg_level_scale(uint16_t value_l, uint16_t multiplier_q12);

/* The per-tick integer step of a Q12 rate, dithered against the care-tick
 * index so the long-run mean is exact.
 *
 * A rate of, say, 7.97 level units per tick cannot be spent as an integer
 * without either losing the fraction or persisting a residual the save schema
 * does not have. Differencing the running total at t and t+1 spends the whole
 * rate over any span, drifts by at most one unit, and is a pure function of
 * state the save file already carries — care_seconds_total is what supplies
 * the index. */
uint32_t pg_q12_step(uint32_t rate_q12, uint64_t tick_index);

/* The volume-weighted moisture of the whole root ball: the top layer is a
 * third of the mix and the bottom two thirds. This is the number the tabled
 * watering interval is calibrated against, and it is why "lift the pot" is a
 * different reading from "check the soil" rather than a second opinion on it. */
uint16_t pg_care_moisture_whole_l(const pg_plant *plant);

/* ---- what a place delivers, this tick ---------------------------------- */

typedef struct pg_care_env {
    uint16_t ordinal;            /* season ordinal 1..366, hemisphere applied  */
    uint16_t local_minutes;      /* minutes since local midnight               */
    uint16_t season_l;           /* the growth scalar, level scale             */
    uint16_t effective_season_l; /* max(season, 0.30) — recoveries only (D-085) */
    uint16_t daylight_minutes;
    int16_t  air_temp_c_x10;
    uint8_t  rh_pct;
    uint16_t airflow_q12;
    uint16_t dli_day_c;          /* the whole day's DLI at this spot, today     */
    uint8_t  light_band;
    bool     is_daylight;
    bool     is_growing_season;
} pg_care_env;

/* Weather is derived deterministically from (day ordinal, seed) so a replayed
 * catch-up produces the same July it produced the first time
 * (GAME_DESIGN.md §8.2). Cloud scales the day's DLI; a heatwave adds the
 * temperature term; rain fills the jug on the sill. */
pg_care_env pg_care_env_for(uint8_t spot_id, uint16_t ordinal,
                            uint16_t local_minutes, uint64_t seed);

/* The instantaneous share of the day's DLI that lands in one care tick, in
 * centi-mol. Zero outside the daylight window: light is a place *and* a time. */
uint16_t pg_care_tick_dli_c(const pg_care_env *env);

/* ---- the multiplier chain (PLANT_CARE.md §4.2) -------------------------- */

uint16_t pg_care_light_q12(uint8_t light_band);
uint16_t pg_care_temp_q12(int16_t temp_c_x10);
uint16_t pg_care_humidity_q12(uint8_t rh_pct);
uint16_t pg_care_season_drain_q12(uint16_t season_l);
uint16_t pg_care_root_mass_q12(uint16_t root_bound_l);

/* The whole chain for one plant in one place, so a caller cannot forget a
 * term. Includes the pot. */
uint16_t pg_care_drain_multiplier_q12(const pg_plant *plant,
                                      const pg_care_env *env);

/* The derived watering interval, in care ticks, for display and for the
 * calendar's honest forecast. This is a *consequence* of conditions and moves
 * when they do; it is never the thing that is stored. */
uint32_t pg_care_watering_interval_ticks(const pg_plant *plant,
                                         const pg_care_env *env);

/* ---- integration -------------------------------------------------------- */

/* One 900 s care tick: the drain chain, percolation, the cachepot floor, the
 * soggy-tick accumulator, turgor's lag, the DLI accumulation, the damage doses
 * and the pests. tick_index is the plant's care-tick ordinal and is what makes
 * the Q12 dither reproducible. */
void pg_care_tick(pg_plant *plant, const pg_care_env *env, uint64_t tick_index);

/* One coarse day: the DLI roll, light debt, nutrition, growth points, root
 * mass, acclimation and shock decay. Called after the day's ticks in the fine
 * tier, and once per day on its own in the coarse tier. */
void pg_care_day(pg_plant *plant, const pg_care_env *env);

/* ---- the verbs, and the only four refusals in the game ------------------ */

typedef enum pg_verb {
    PG_VERB_CHECK_SOIL = 0,
    PG_VERB_LIFT_POT,
    PG_VERB_LOOK_CLOSELY,
    PG_VERB_READ_LABEL,

    PG_VERB_WATER_THOROUGHLY,
    PG_VERB_WATER_SIP,
    PG_VERB_BOTTOM_SOAK,
    PG_VERB_EMPTY_SAUCER,
    PG_VERB_SLEEVE_LIFT,
    PG_VERB_SLEEVE_RETURN,
    PG_VERB_WATER_SOURCE,

    PG_VERB_FEED_QUARTER,
    PG_VERB_FEED_HALF,
    PG_VERB_FEED_FULL,
    PG_VERB_FLUSH,

    PG_VERB_ROTATE,
    PG_VERB_WIPE,
    PG_VERB_TRIM,
    PG_VERB_MOVE,
    PG_VERB_REPOT,
    PG_VERB_REPOT_TWO_STEP,
    PG_VERB_REPOT_GRAVEL,
    PG_VERB_CUTTING,
    PG_VERB_TREAT_PESTS,
    PG_VERB_MIST,
    PG_VERB_COUNT
} pg_verb;

/* An action that is wrong right now stays enabled with a stated reason rather
 * than greyed out. A refusal is the one response that teaches nothing, so
 * there are exactly four of them and they are all under Feed. */
typedef enum pg_legality {
    PG_LEGAL_ALLOWED = 0,
    PG_LEGAL_WARNED = 1,     /* offered, with a truthful reason, and it happens */
    PG_LEGAL_REFUSED = 2
} pg_legality;

/* PLANT_CARE.md §6.4. These four, and no others, in the whole game. */
typedef enum pg_feed_refusal {
    PG_FEED_REFUSAL_NONE = 0,
    PG_FEED_REFUSAL_DORMANT = 1,
    PG_FEED_REFUSAL_DRY_SOIL = 2,
    PG_FEED_REFUSAL_RECENTLY_REPOTTED = 3,
    PG_FEED_REFUSAL_VISIBLY_SICK = 4,
    PG_FEED_REFUSAL_COUNT = 5
} pg_feed_refusal;

/* Feeding within this window of a repot is refusal 3. The research says 4-6
 * weeks; the game takes the cautious end. */
#define PG_FEED_AFTER_REPOT_DAYS 42u

pg_feed_refusal pg_care_feed_refusal(const pg_plant *plant,
                                     const pg_care_env *env,
                                     uint64_t now_care_s);

/* The spoken reason. Never scolding, and it always says what to do instead. */
const char *pg_care_refusal_reason(pg_feed_refusal refusal);

/* Legality of any verb. `reason` is filled for WARNED and REFUSED and left
 * alone for ALLOWED; it may be NULL. */
pg_legality pg_care_verb_legality(pg_verb verb, const pg_plant *plant,
                                  const pg_care_env *env, uint64_t now_care_s,
                                  const char **reason);

const char *pg_care_verb_name(pg_verb verb);

/* Whether a verb belongs to the Feed group — the group the four refusals live
 * in, and the assertion --rules-test enumerates. */
bool pg_care_verb_is_feed(pg_verb verb);

/* ---- effects the care model owns ---------------------------------------- */

/* Water until it runs from the hole: both layers to full, 15 % of the salt
 * flushed, the saucer filled. In a pot with no drainage nothing runs out — the
 * action still SUCCEEDS, no salt is flushed, and the bottom stays at its floor.
 * That is not a refusal; the notebook explains the perched water afterwards. */
void pg_care_water_thoroughly(pg_plant *plant, uint64_t now_care_s);

/* The classic real failure: the top wets, the bottom stays dry, salts
 * concentrate. It visibly succeeds and the plant does not improve. */
void pg_care_water_sip(pg_plant *plant, uint64_t now_care_s);

/* 3-4 pot volumes. Only possible in a pot that can be flushed, which is the
 * whole of the cachepot's salt problem. */
void pg_care_flush(pg_plant *plant, uint64_t now_care_s);

void pg_care_feed(pg_plant *plant, uint16_t strength_q12, uint64_t now_care_s);
void pg_care_repot(pg_plant *plant, uint8_t new_pot_id, bool two_step,
                   bool gravel_layer, uint64_t now_care_s);

/* Dust genuinely cuts effective light; modelled at the low end of the research
 * range, which is the honest reading of a [MED] ceiling. */
void pg_care_wipe_leaves(pg_plant *plant);

/* Headless diagnostic behind --care-test. Prints the per-species ladders and
 * asserts, among others, the two the plan names: 200 care ticks at rest in a
 * cachepot is exactly 200 soggy ticks, strictly monotone; and no recovery,
 * shock or fair-warning interval exceeds 4x its growing-season value at any
 * day ordinal. Returns 0 when every assertion held. */
int pg_care_run_test(void);

#endif /* PG_CARE_H */
