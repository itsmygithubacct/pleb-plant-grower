/*
 * test_care — the care model's load-bearing arithmetic.
 *
 * --care-test is the readable report; these are the assertions that must not
 * be allowed to drift. The three that matter most, because everything else in
 * the game is downstream of them:
 *
 *   - the drain chain is a PRODUCT of conditions, so a dim cold room really
 *     does stretch the interval and low light really does cause overwatering;
 *   - the calibration of PLANT_CARE.md §4.1 survives the two-layer pot model,
 *     so the tabled interval comes back out at the baseline;
 *   - the cachepot's soggy ticks are strictly monotone, which is the single
 *     justification for it being lethal rather than merely suboptimal.
 */
#include "pg_care.h"

#include "pg_calendar.h"
#include "pg_content.h"
#include "pg_plant.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
                      __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

/* The PLANT_CARE.md §1.3 baseline, with every multiplier at exactly 1.0. */
static pg_care_env baseline_env(void)
{
    pg_care_env env;

    memset(&env, 0, sizeof env);
    env.ordinal = 172u;
    env.local_minutes = 720u;
    env.season_l = pg_calendar_season_l(172u);
    env.effective_season_l = pg_calendar_effective_season_l(env.season_l);
    env.daylight_minutes = pg_calendar_daylight_minutes(172u);
    env.is_growing_season = true;
    env.is_daylight = true;
    env.air_temp_c_x10 = 210;
    env.rh_pct = 45u;
    env.airflow_q12 = (uint16_t)PG_Q12_ONE;
    env.light_band = (uint8_t)PG_LIGHT_BRIGHT_INDIRECT;
    env.dli_day_c = 1250u;
    return env;
}

static pg_plant baseline_plant(uint8_t species_id, uint8_t pot_id)
{
    pg_plant plant;

    pg_plant_init(&plant, species_id, pot_id, (uint8_t)PG_SPOT_DEFAULT, 0);
    plant.moisture_top = (uint16_t)PG_LEVEL_MAX;
    plant.moisture_bottom = (uint16_t)PG_LEVEL_MAX;
    plant.root_bound = 7000u;   /* "roots fill but do not circle the pot" */
    return plant;
}

static uint32_t ticks_to_thirst(uint8_t species_id, uint8_t pot_id,
                                const pg_care_env *env)
{
    pg_plant plant = baseline_plant(species_id, pot_id);
    const pg_species *species = pg_content_species(species_id);
    uint32_t tick;

    for (tick = 0u; tick < 100000u; ++tick) {
        pg_care_tick(&plant, env, tick);
        if (pg_care_moisture_whole_l(&plant) <= species->thirst_threshold_l) {
            return tick + 1u;
        }
    }
    return UINT32_MAX;
}

static bool test_fixed_point(void)
{
    CHECK(pg_q12_mul((uint16_t)PG_Q12_ONE, (uint16_t)PG_Q12_ONE)
          == (uint16_t)PG_Q12_ONE);
    CHECK(pg_q12_mul(0u, (uint16_t)PG_Q12_ONE) == 0u);
    CHECK(pg_q12_mul(2048u, 2048u) == 1024u);            /* 0.5 x 0.5 */
    CHECK(pg_level_scale(10000u, (uint16_t)PG_Q12_ONE) == 10000u);
    CHECK(pg_level_scale(10000u, 2048u) == 5000u);
    CHECK(pg_level_scale(10000u, 8192u) == 10000u);      /* clamps at full */

    /* The dither must spend a fractional rate exactly over a long run: this is
     * what lets the save record hold a bare u16 and still replay bit-exactly. */
    {
        uint64_t tick;
        uint64_t total = 0u;
        const uint32_t rate = 32627u;    /* the pothos's growing rate */
        for (tick = 0u; tick < 10000u; ++tick) {
            total += pg_q12_step(rate, tick);
        }
        CHECK(total == ((uint64_t)rate * 10000u) >> 12);
    }
    /* And it never drifts by more than a single unit at any point. */
    {
        uint64_t tick;
        uint64_t total = 0u;
        for (tick = 0u; tick < 5000u; ++tick) {
            uint64_t exact;
            total += pg_q12_step(7u * 4096u + 2000u, tick);
            exact = ((uint64_t)(7u * 4096u + 2000u) * (tick + 1u)) >> 12;
            CHECK(total == exact);
        }
    }
    return true;
}

static bool test_baseline_reproduces_the_tabled_interval(void)
{
    pg_care_env env = baseline_env();
    uint8_t species_id;

    /* PLANT_CARE.md §4.1: at the baseline with every multiplier at 1.0, the
     * moisture falls from full to the thirst threshold in exactly the tabled
     * interval. This is the calibration the whole content pipeline exists to
     * preserve. */
    for (species_id = 0u; species_id < (uint8_t)PG_SPECIES_COUNT; ++species_id) {
        const pg_species *species = pg_content_species(species_id);
        uint32_t got = ticks_to_thirst(species_id, (uint8_t)PG_POT_GLAZED, &env);
        uint32_t want = ((uint32_t)species->water_interval_growing_dh
                         * PG_CARE_TICKS_PER_DAY) / 10u;
        CHECK(got != UINT32_MAX);
        CHECK(got * 100u > want * 98u);
        CHECK(got * 100u < want * 102u);
    }
    return true;
}

static bool test_the_pot_changes_the_schedule_more_than_the_plant(void)
{
    pg_care_env env = baseline_env();
    uint32_t terracotta = ticks_to_thirst((uint8_t)PG_SPECIES_POTHOS,
                                          (uint8_t)PG_POT_TERRACOTTA, &env);
    uint32_t glazed = ticks_to_thirst((uint8_t)PG_SPECIES_POTHOS,
                                      (uint8_t)PG_POT_GLAZED, &env);
    uint32_t nursery = ticks_to_thirst((uint8_t)PG_SPECIES_POTHOS,
                                       (uint8_t)PG_POT_NURSERY, &env);
    uint32_t cachepot = ticks_to_thirst((uint8_t)PG_SPECIES_POTHOS,
                                        (uint8_t)PG_POT_CACHEPOT, &env);

    CHECK(terracotta < nursery);
    CHECK(nursery < glazed);
    /* The cachepot's bottom layer is floored above saturation, so the whole
     * root ball never reaches the thirst threshold at all. It does not dry
     * slowly; it does not dry. */
    CHECK(cachepot == UINT32_MAX);
    return true;
}

static bool test_the_glazed_pot_lies_and_terracotta_does_not(void)
{
    pg_care_env env = baseline_env();
    pg_plant glazed = baseline_plant((uint8_t)PG_SPECIES_POTHOS,
                                     (uint8_t)PG_POT_GLAZED);
    pg_plant clay = baseline_plant((uint8_t)PG_SPECIES_POTHOS,
                                   (uint8_t)PG_POT_TERRACOTTA);
    uint32_t tick;
    int32_t glazed_split, clay_split;

    for (tick = 0u; tick < 3u * PG_CARE_TICKS_PER_DAY; ++tick) {
        pg_care_tick(&glazed, &env, tick);
        pg_care_tick(&clay, &env, tick);
    }
    glazed_split = (int32_t)glazed.moisture_bottom - (int32_t)glazed.moisture_top;
    clay_split = (int32_t)clay.moisture_bottom - (int32_t)clay.moisture_top;

    /* Zero side evaporation means the top dries while the bottom stays wet,
     * which is exactly why a player who learned "water when the top is dry" on
     * clay starts rotting roots the day they repot into glazed. Porous walls
     * dry both layers together, so the finger test stays honest. */
    CHECK(glazed_split > clay_split);
    CHECK(clay_split >= 0);
    return true;
}

static bool test_light_and_temperature_drive_the_drying_rate(void)
{
    pg_care_env bright = baseline_env();
    pg_care_env dim = baseline_env();
    pg_care_env cold = baseline_env();
    pg_care_env humid = baseline_env();
    uint32_t bright_ticks, dim_ticks, cold_ticks, humid_ticks;

    dim.light_band = (uint8_t)PG_LIGHT_LOW;
    cold.air_temp_c_x10 = 140;
    humid.rh_pct = 70u;

    bright_ticks = ticks_to_thirst((uint8_t)PG_SPECIES_POTHOS,
                                   (uint8_t)PG_POT_GLAZED, &bright);
    dim_ticks = ticks_to_thirst((uint8_t)PG_SPECIES_POTHOS,
                                (uint8_t)PG_POT_GLAZED, &dim);
    cold_ticks = ticks_to_thirst((uint8_t)PG_SPECIES_POTHOS,
                                 (uint8_t)PG_POT_GLAZED, &cold);
    humid_ticks = ticks_to_thirst((uint8_t)PG_SPECIES_POTHOS,
                                  (uint8_t)PG_POT_GLAZED, &humid);

    /* The coupling most plant games miss: a dim room, a cold room and a humid
     * room all stretch the interval, which is why a fixed weekly watering day
     * kills plants. */
    CHECK(dim_ticks > bright_ticks);
    CHECK(cold_ticks > bright_ticks);
    CHECK(humid_ticks > bright_ticks);

    /* A rootbound plant dries roughly twice as fast, which is the real
     * diagnostic for repotting. */
    {
        pg_plant loose = baseline_plant((uint8_t)PG_SPECIES_POTHOS,
                                        (uint8_t)PG_POT_GLAZED);
        pg_plant tight = loose;
        loose.root_bound = 7000u;
        tight.root_bound = (uint16_t)PG_LEVEL_MAX;
        CHECK(pg_care_drain_multiplier_q12(&tight, &bright)
              > pg_care_drain_multiplier_q12(&loose, &bright));
        CHECK(pg_care_watering_interval_ticks(&tight, &bright)
              < pg_care_watering_interval_ticks(&loose, &bright));
    }

    /* The snake plant is genuinely indifferent to humidity, so the RH term is
     * dropped for it entirely rather than applied as a band it does not have. */
    {
        pg_plant sarge = baseline_plant((uint8_t)PG_SPECIES_SNAKE,
                                        (uint8_t)PG_POT_GLAZED);
        CHECK(pg_care_drain_multiplier_q12(&sarge, &humid)
              == pg_care_drain_multiplier_q12(&sarge, &bright));
    }
    return true;
}

static bool test_the_cachepot_never_stops_accumulating(void)
{
    pg_care_env env = baseline_env();
    pg_plant plant;
    uint32_t tick;

    pg_plant_init(&plant, (uint8_t)PG_SPECIES_SNAKE, (uint8_t)PG_POT_CACHEPOT,
                  (uint8_t)PG_SPOT_DEFAULT, 0);
    plant.moisture_top = 0u;
    plant.moisture_bottom = 0u;

    /* D-082, stated exactly: 200 care ticks at rest is 200 soggy ticks, and
     * every single one of them is +1 — never a decay, not once. */
    for (tick = 0u; tick < 200u; ++tick) {
        uint32_t before = plant.soggy_ticks;
        pg_care_tick(&plant, &env, tick);
        CHECK(plant.soggy_ticks == before + 1u);
    }
    CHECK(plant.soggy_ticks == 200u);
    CHECK(plant.moisture_bottom > pg_content_saturation_level());

    /* A pot that drains does decay them, which is what makes the difference a
     * mechanic rather than a rounding accident. */
    {
        pg_plant draining;
        pg_plant_init(&draining, (uint8_t)PG_SPECIES_SNAKE,
                      (uint8_t)PG_POT_TERRACOTTA, (uint8_t)PG_SPOT_DEFAULT, 0);
        draining.moisture_top = 0u;
        draining.moisture_bottom = 0u;
        draining.soggy_ticks = 100u;
        for (tick = 0u; tick < 10u; ++tick) {
            pg_care_tick(&draining, &env, tick);
        }
        CHECK(draining.soggy_ticks < 100u);
    }
    return true;
}

static bool test_the_recovery_floor_holds_at_every_ordinal(void)
{
    uint16_t ordinal;

    /* D-085: every recovery, shock and fair-warning division uses
     * max(growth_scalar, 0.30), so no interval is ever stretched past 3.34x —
     * comfortably inside the 4x the plan asks for, at every day of the year. */
    for (ordinal = 1u; ordinal <= PG_ORDINAL_MAX; ++ordinal) {
        uint16_t season = pg_calendar_season_l(ordinal);
        uint16_t effective = pg_calendar_effective_season_l(season);
        CHECK(effective >= 3000u);
        CHECK(effective >= season);
        CHECK(((uint32_t)PG_LEVEL_MAX * 1000u) / effective <= 4000u);
    }
    return true;
}

static bool test_shock_costs_more_in_winter_but_not_a_year(void)
{
    pg_care_env summer = baseline_env();
    pg_care_env winter = baseline_env();
    pg_plant hot, cold;
    uint32_t summer_days = 0u, winter_days = 0u;
    uint32_t day;

    winter.ordinal = 355u;
    winter.season_l = pg_calendar_season_l(355u);
    winter.effective_season_l = pg_calendar_effective_season_l(winter.season_l);
    winter.is_growing_season = false;

    hot = baseline_plant((uint8_t)PG_SPECIES_POTHOS, (uint8_t)PG_POT_GLAZED);
    cold = hot;
    hot.shock = (uint16_t)PG_LEVEL_MAX;
    cold.shock = (uint16_t)PG_LEVEL_MAX;

    for (day = 0u; day < 400u; ++day) {
        if (hot.shock > 0u) {
            pg_care_day(&hot, &summer);
            summer_days += 1u;
        }
        if (cold.shock > 0u) {
            pg_care_day(&cold, &winter);
            winter_days += 1u;
        }
    }
    CHECK(hot.shock == 0u);
    CHECK(cold.shock == 0u);
    /* A winter mistake genuinely costs more... */
    CHECK(winter_days > summer_days);
    /* ...and at most 3.34x more, which is what turns the floor from a nicety
     * into the thing that keeps the fair-warning rule a promise. */
    CHECK(winter_days * 1000u <= summer_days * 3340u);
    return true;
}

static bool test_watering_effects_are_truthful(void)
{
    pg_plant clay = baseline_plant((uint8_t)PG_SPECIES_PEACE_LILY,
                                   (uint8_t)PG_POT_TERRACOTTA);
    pg_plant trap = baseline_plant((uint8_t)PG_SPECIES_PEACE_LILY,
                                   (uint8_t)PG_POT_CACHEPOT);
    uint16_t clay_salt_before, trap_salt_before;

    clay.salt = 8000u;
    trap.salt = 8000u;
    clay_salt_before = clay.salt;
    trap_salt_before = trap.salt;

    pg_care_water_thoroughly(&clay, 100u);
    pg_care_water_thoroughly(&trap, 100u);

    /* Water until it runs from the hole carries salt out with it. In a pot
     * with no drainage nothing runs out, so nothing is carried out — and the
     * action still succeeds, because there are exactly four refusals in the
     * game and they are all under Feed. */
    CHECK(clay.salt < clay_salt_before);
    CHECK(trap.salt == trap_salt_before);
    CHECK(clay.moisture_bottom == (uint16_t)PG_LEVEL_MAX);
    CHECK(trap.moisture_bottom == (uint16_t)PG_LEVEL_MAX);
    CHECK(clay.last_watered_care_s == 100u);

    /* A sip visibly succeeds and does not help: the top wets, the bottom stays
     * where it was, and the salts concentrate. */
    {
        pg_plant sipped = baseline_plant((uint8_t)PG_SPECIES_PEACE_LILY,
                                         (uint8_t)PG_POT_GLAZED);
        uint16_t bottom_before;
        sipped.moisture_top = 1000u;
        sipped.moisture_bottom = 1000u;
        sipped.salt = 3000u;
        bottom_before = sipped.moisture_bottom;
        pg_care_water_sip(&sipped, 200u);
        CHECK(sipped.moisture_top == (uint16_t)PG_LEVEL_MAX);
        CHECK(sipped.moisture_bottom == bottom_before);
        CHECK(sipped.salt > 3000u);
    }

    /* A flush is the salt fix, and only a pot that can be flushed gets it. */
    {
        pg_plant flushable = baseline_plant((uint8_t)PG_SPECIES_CALATHEA,
                                            (uint8_t)PG_POT_NURSERY);
        pg_plant sealed = baseline_plant((uint8_t)PG_SPECIES_CALATHEA,
                                         (uint8_t)PG_POT_CACHEPOT);
        flushable.salt = 9000u;
        sealed.salt = 9000u;
        pg_care_flush(&flushable, 300u);
        pg_care_flush(&sealed, 300u);
        CHECK(flushable.salt < 4000u);
        CHECK(sealed.salt == 9000u);
    }
    return true;
}

static bool test_the_four_hard_refusals(void)
{
    pg_care_env growing = baseline_env();
    pg_care_env dormant = baseline_env();
    pg_plant plant = baseline_plant((uint8_t)PG_SPECIES_POTHOS,
                                    (uint8_t)PG_POT_GLAZED);
    const uint64_t now = (uint64_t)90 * 86400u;

    dormant.is_growing_season = false;

    CHECK(pg_care_feed_refusal(&plant, &growing, now) == PG_FEED_REFUSAL_NONE);
    CHECK(pg_care_feed_refusal(&plant, &dormant, now) == PG_FEED_REFUSAL_DORMANT);

    plant.moisture_bottom = 0u;
    CHECK(pg_care_feed_refusal(&plant, &growing, now) == PG_FEED_REFUSAL_DRY_SOIL);
    plant.moisture_bottom = (uint16_t)PG_LEVEL_MAX;

    plant.last_repotted_care_s = now - 86400u;
    CHECK(pg_care_feed_refusal(&plant, &growing, now)
          == PG_FEED_REFUSAL_RECENTLY_REPOTTED);
    /* Past the window it is allowed again, without anything else changing. */
    plant.last_repotted_care_s = now
                              - (uint64_t)(PG_FEED_AFTER_REPOT_DAYS + 1u) * 86400u;
    CHECK(pg_care_feed_refusal(&plant, &growing, now) == PG_FEED_REFUSAL_NONE);

    plant.root_health = 500u;
    CHECK(pg_care_feed_refusal(&plant, &growing, now)
          == PG_FEED_REFUSAL_VISIBLY_SICK);

    /* Every refusal says something, and every one of them says what to do
     * instead: a refusal that only says no is the response that teaches
     * nothing. */
    {
        int refusal;
        for (refusal = 1; refusal < (int)PG_FEED_REFUSAL_COUNT; ++refusal) {
            const char *reason =
                pg_care_refusal_reason((pg_feed_refusal)refusal);
            CHECK(reason != NULL);
            CHECK(reason[0] != '\0');
        }
    }
    return true;
}

static bool test_nothing_outside_feed_is_ever_refused(void)
{
    pg_care_env env = baseline_env();
    pg_plant plant = baseline_plant((uint8_t)PG_SPECIES_SNAKE,
                                    (uint8_t)PG_POT_CACHEPOT);
    int verb;

    plant.moisture_bottom = 0u;
    plant.root_health = 200u;
    plant.life_state = (uint8_t)PG_LIFE_TERMINAL;
    env.is_growing_season = false;

    /* The worst state the model can produce, and still the only refusals in
     * it are the Feed ones. */
    for (verb = 0; verb < (int)PG_VERB_COUNT; ++verb) {
        const char *reason = NULL;
        pg_legality legality = pg_care_verb_legality((pg_verb)verb, &plant,
                                                     &env, 0u, &reason);
        if (legality == PG_LEGAL_REFUSED) {
            CHECK(pg_care_verb_is_feed((pg_verb)verb));
        }
        if (legality != PG_LEGAL_ALLOWED) {
            CHECK(reason != NULL && reason[0] != '\0');
        }
    }
    return true;
}

static bool test_null_arguments_are_survivable(void)
{
    pg_care_env env = baseline_env();
    pg_plant plant = baseline_plant(0u, 0u);

    pg_care_tick(NULL, &env, 0u);
    pg_care_tick(&plant, NULL, 0u);
    pg_care_day(NULL, &env);
    pg_care_day(&plant, NULL);
    pg_care_water_thoroughly(NULL, 0u);
    pg_care_water_sip(NULL, 0u);
    pg_care_flush(NULL, 0u);
    pg_care_feed(NULL, 2048u, 0u);
    pg_care_repot(NULL, 0u, false, false, 0u);
    pg_care_wipe_leaves(NULL);
    CHECK(pg_care_moisture_whole_l(NULL) == 0u);
    CHECK(pg_care_drain_multiplier_q12(NULL, &env) == (uint16_t)PG_Q12_ONE);
    CHECK(pg_care_watering_interval_ticks(NULL, &env) == 0u);
    CHECK(pg_care_feed_refusal(NULL, &env, 0u) == PG_FEED_REFUSAL_NONE);
    CHECK(pg_care_verb_legality(PG_VERB_WATER_THOROUGHLY, NULL, &env, 0u, NULL)
          == PG_LEGAL_ALLOWED);
    CHECK(pg_care_verb_name((pg_verb)PG_VERB_COUNT)[0] == '\0');
    CHECK(pg_care_refusal_reason(PG_FEED_REFUSAL_NONE)[0] == '\0');
    return true;
}

int main(void)
{
    char error[128];

    if (!pg_content_validate(error, sizeof error)) {
        (void)fprintf(stderr, "care: content invalid: %s\n", error);
        return 1;
    }
    if (!test_fixed_point()) return 1;
    if (!test_baseline_reproduces_the_tabled_interval()) return 1;
    if (!test_the_pot_changes_the_schedule_more_than_the_plant()) return 1;
    if (!test_the_glazed_pot_lies_and_terracotta_does_not()) return 1;
    if (!test_light_and_temperature_drive_the_drying_rate()) return 1;
    if (!test_the_cachepot_never_stops_accumulating()) return 1;
    if (!test_the_recovery_floor_holds_at_every_ordinal()) return 1;
    if (!test_shock_costs_more_in_winter_but_not_a_year()) return 1;
    if (!test_watering_effects_are_truthful()) return 1;
    if (!test_the_four_hard_refusals()) return 1;
    if (!test_nothing_outside_feed_is_ever_refused()) return 1;
    if (!test_null_arguments_are_survivable()) return 1;
    (void)puts("care: PASS");
    return 0;
}
