/*
 * pg_care — see pg_care.h.
 */
#include "pg_care.h"

#include "pg_calendar.h"
#include "pg_content.h"
#include "pg_plant.h"
#include "pg_rng.h"

#include <stdio.h>
#include <string.h>

/* The two layers are the top third and the bottom two thirds of the mix
 * (GAME_DESIGN.md §3), so the same amount of water leaving the top moves its
 * LEVEL three times as far. These four constants convert each loss from a
 * share of the whole pot's water into a level change in the layer it leaves:
 *
 *   surface evaporation  all from the top      -> x3.00 on the top
 *   side evaporation     by volume, both       -> x1.00 on each, equally,
 *                                                which is why terracotta dries
 *                                                the whole root ball evenly
 *   root uptake          0.35 top / 0.65 bottom -> x1.05 and x0.975
 *
 * They are chosen so that (top/3 + bottom*2/3) always loses exactly the base
 * rate: whatever the pot, the WHOLE pot still reaches the species' thirst
 * threshold in the tabled interval, and the pot decides only where the water
 * comes from. That is the calibration of PLANT_CARE.md §4.1 kept intact while
 * the two-layer model is added on top of it. */
#define PG_SURFACE_TO_TOP_Q12   12288u   /* x3.000 */
#define PG_SIDE_TO_LAYER_Q12     4096u   /* x1.000 on both layers */
#define PG_UPTAKE_TO_TOP_Q12     4301u   /* x1.050 = 3 x 0.35 */
#define PG_UPTAKE_TO_BOTTOM_Q12  3994u   /* x0.975 = 1.5 x 0.65 */

/* The top layer is a third of the mix; the whole-pot moisture the interval is
 * calibrated against is the volume-weighted mean of the two. */
#define PG_TOP_VOLUME_SHARE 1u
#define PG_LAYER_VOLUMES    3u

/* A thorough watering flushes 15 % of the salt; a flush takes 70 %
 * (GAME_DESIGN.md §4.2 row 12). */
#define PG_WATER_SALT_FLUSH_Q12 614u    /* keeps 0.85 */
#define PG_FLUSH_SALT_KEEP_Q12  1229u   /* keeps 0.30 */

/* Dust cuts effective light by up to ~15 %, modelled at the low end because
 * the ceiling is [MED] and the direction is [HIGH] (PLANT_CARE.md §8 item 10). */
#define PG_DUST_LIGHT_LOSS_Q12 410u     /* 0.10 */

/* Cold wet soil is much worse than warm wet soil (GAME_DESIGN.md §4.2 row 3). */
#define PG_COLD_SOIL_C_X10      180
#define PG_VERY_COLD_SOIL_C_X10 140
#define PG_COLD_ROT_Q12         6144u   /* x1.5 */
#define PG_VERY_COLD_ROT_Q12    9011u   /* x2.2 */

#define PG_MOISTURE_WET_SURFACE_L 7000u  /* fungus gnats want a chronically wet top */
#define PG_MITE_RH_PCT            35u
#define PG_MITE_TEMP_C_X10        240

#define PG_GROWTH_POINTS_PER_DAY 100u

/* ---- fixed point -------------------------------------------------------- */

static uint16_t pg_clamp_level(int32_t value)
{
    if (value < 0) {
        return 0u;
    }
    if (value > PG_LEVEL_MAX) {
        return (uint16_t)PG_LEVEL_MAX;
    }
    return (uint16_t)value;
}

uint16_t pg_q12_mul(uint16_t a_q12, uint16_t b_q12)
{
    uint32_t product = ((uint32_t)a_q12 * (uint32_t)b_q12 + 2048u) >> 12;
    if (product > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)product;
}

uint16_t pg_level_scale(uint16_t value_l, uint16_t multiplier_q12)
{
    uint32_t scaled = ((uint32_t)value_l * (uint32_t)multiplier_q12 + 2048u) >> 12;
    return pg_clamp_level((int32_t)scaled);
}

uint32_t pg_q12_step(uint32_t rate_q12, uint64_t tick_index)
{
    uint64_t before = ((uint64_t)rate_q12 * tick_index) >> 12;
    uint64_t after = ((uint64_t)rate_q12 * (tick_index + 1u)) >> 12;
    return (uint32_t)(after - before);
}

/* Linear interpolation on the level scale: t_l = 0 gives `low`, PG_LEVEL_MAX
 * gives `high`. */
static uint16_t pg_lerp_l(uint16_t low, uint16_t high, uint16_t t_l)
{
    int32_t span = (int32_t)high - (int32_t)low;
    int32_t moved = (span * (int32_t)t_l) / PG_LEVEL_MAX;
    int32_t result = (int32_t)low + moved;
    if (result < 0) {
        return 0u;
    }
    if (result > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)result;
}

static int16_t pg_lerp_temp(int16_t low, int16_t high, uint16_t t_l)
{
    int32_t span = (int32_t)high - (int32_t)low;
    int32_t result = (int32_t)low + (span * (int32_t)t_l) / PG_LEVEL_MAX;
    if (result < INT16_MIN) {
        return INT16_MIN;
    }
    if (result > INT16_MAX) {
        return INT16_MAX;
    }
    return (int16_t)result;
}

uint16_t pg_care_moisture_whole_l(const pg_plant *plant)
{
    uint32_t whole;

    if (plant == NULL) {
        return 0u;
    }
    /* The top layer is a third of the mix. This is the number the tabled
     * watering interval is calibrated against, and the number the "lift the
     * pot" verb reports, because weight is a property of the whole root ball
     * and not of the first three centimetres. */
    whole = ((uint32_t)plant->moisture_top * PG_TOP_VOLUME_SHARE
             + (uint32_t)plant->moisture_bottom
               * (PG_LAYER_VOLUMES - PG_TOP_VOLUME_SHARE))
          / PG_LAYER_VOLUMES;
    return (uint16_t)whole;
}

/* ---- what a place delivers --------------------------------------------- */

pg_care_env pg_care_env_for(uint8_t spot_id, uint16_t ordinal,
                            uint16_t local_minutes, uint64_t seed)
{
    pg_care_env env;
    const pg_spot *spot = pg_content_spot(spot_id);
    uint16_t sunrise, sunset;
    uint16_t cloud_l, heat_l;
    uint16_t dli_clear_c;
    int16_t day_temp, night_temp;

    memset(&env, 0, sizeof env);
    if (ordinal < 1u) {
        ordinal = 1u;
    }
    if (ordinal > PG_ORDINAL_MAX) {
        ordinal = PG_ORDINAL_MAX;
    }
    if (local_minutes >= PG_MINUTES_PER_DAY) {
        local_minutes = (uint16_t)(PG_MINUTES_PER_DAY - 1);
    }

    env.ordinal = ordinal;
    env.local_minutes = local_minutes;
    env.season_l = pg_calendar_season_l(ordinal);
    env.effective_season_l = pg_calendar_effective_season_l(env.season_l);
    env.daylight_minutes = pg_calendar_daylight_minutes(ordinal);
    env.is_growing_season = pg_calendar_is_growing(env.season_l);

    sunrise = pg_calendar_sunrise_minutes(ordinal);
    sunset = pg_calendar_sunset_minutes(ordinal);
    env.is_daylight = (local_minutes >= sunrise && local_minutes < sunset);

    if (spot == NULL) {
        return env;
    }

    env.light_band = spot->light_band;
    env.airflow_q12 = spot->airflow_q12;

    /* Weather, derived from (ordinal, seed) so a replay produces the same
     * July it produced the first time. */
    cloud_l = pg_rng_weather_l(seed, ordinal, (uint8_t)PG_WEATHER_CLOUD);
    heat_l = pg_rng_weather_l(seed, ordinal, (uint8_t)PG_WEATHER_HEAT);

    /* The spot's own annual curve first, then the weather on top of it. */
    dli_clear_c = pg_lerp_l(spot->dli_floor_c, spot->dli_peak_c, env.season_l);
    /* Overcast costs about half the day's light; the multiplier runs 1.0 down
     * to 0.5 with cloud cover. */
    env.dli_day_c = pg_level_scale(dli_clear_c,
                                   (uint16_t)(PG_Q12_ONE
                                              - (uint16_t)(((uint32_t)cloud_l
                                                            * 2048u) / PG_LEVEL_MAX)));

    day_temp = pg_lerp_temp(spot->temp_winter_day_c_x10,
                            spot->temp_summer_day_c_x10, env.season_l);
    night_temp = pg_lerp_temp(spot->temp_winter_night_c_x10,
                              spot->temp_summer_night_c_x10, env.season_l);
    env.air_temp_c_x10 = env.is_daylight ? day_temp : night_temp;
    if (env.is_growing_season && heat_l > 9000u) {
        /* A July heatwave, which is what puts the x1.3 temperature term on the
         * drain chain (GAME_DESIGN.md §8.2). */
        env.air_temp_c_x10 = (int16_t)(env.air_temp_c_x10 + 40);
    }

    env.rh_pct = (uint8_t)pg_lerp_l(spot->rh_winter_pct, spot->rh_summer_pct,
                                    env.season_l);
    return env;
}

uint16_t pg_care_tick_dli_c(const pg_care_env *env)
{
    uint16_t sunrise;
    uint32_t daylight_ticks;
    uint32_t rate_q12;
    uint64_t tick_of_light;

    if (env == NULL || !env->is_daylight || env->daylight_minutes == 0u) {
        return 0u;
    }
    sunrise = pg_calendar_sunrise_minutes(env->ordinal);
    if (env->local_minutes < sunrise) {
        return 0u;
    }
    daylight_ticks = (uint32_t)env->daylight_minutes
                   / (PG_CARE_TICK_SECONDS / 60);
    if (daylight_ticks == 0u) {
        return 0u;
    }
    tick_of_light = (uint64_t)((env->local_minutes - sunrise)
                               / (PG_CARE_TICK_SECONDS / 60));
    /* Dithered so the whole day's DLI is spent exactly, even when a tick's
     * share is a fraction of one centi-mol. */
    rate_q12 = ((uint32_t)env->dli_day_c * PG_Q12_ONE) / daylight_ticks;
    return (uint16_t)pg_q12_step(rate_q12, tick_of_light);
}

/* ---- the multiplier chain (PLANT_CARE.md §4.2) -------------------------- */

uint16_t pg_care_light_q12(uint8_t light_band)
{
    const pg_multipliers *m = pg_content_multipliers();

    if (light_band >= (uint8_t)PG_LIGHT_BAND_COUNT) {
        return (uint16_t)PG_Q12_ONE;
    }
    return m->light[light_band];
}

uint16_t pg_care_temp_q12(int16_t temp_c_x10)
{
    const pg_multipliers *m = pg_content_multipliers();

    if (temp_c_x10 > 260) {
        return m->temp_above_26;
    }
    if (temp_c_x10 >= 200) {
        return m->temp_20_26;
    }
    if (temp_c_x10 >= 160) {
        return m->temp_16_20;
    }
    return m->temp_below_16;
}

uint16_t pg_care_humidity_q12(uint8_t rh_pct)
{
    const pg_multipliers *m = pg_content_multipliers();

    if (rh_pct < 35u) {
        return m->rh_below_35;
    }
    if (rh_pct <= 60u) {
        return m->rh_35_60;
    }
    return m->rh_above_60;
}

uint16_t pg_care_season_drain_q12(uint16_t season_l)
{
    const pg_multipliers *m = pg_content_multipliers();

    return pg_lerp_l(m->season_dormant, m->season_growing, season_l);
}

uint16_t pg_care_root_mass_q12(uint16_t root_bound_l)
{
    const pg_multipliers *m = pg_content_multipliers();

    if (root_bound_l >= 7000u) {
        return pg_lerp_l(m->root_established, m->root_bound,
                         (uint16_t)(((uint32_t)(root_bound_l - 7000u)
                                     * PG_LEVEL_MAX) / 3000u));
    }
    return pg_lerp_l(m->root_sparse, m->root_established,
                     (uint16_t)(((uint32_t)root_bound_l * PG_LEVEL_MAX) / 7000u));
}

/* The species' own seasonal term.
 *
 * PLANT_CARE.md §4.2 gives one species-independent Season row, x1.0 -> x0.45,
 * and §9 gives each species BOTH a growing and a dormant interval. The two do
 * not agree — 0.45 would stretch the calathea's 5.5 d to 12.2 d against her
 * tabled 8.5 — and §9 is the direct transcription target, so the per-species
 * pair is what binds here. The four species' own ratios are 0.57, 0.41, 0.65
 * and 0.65, which bracket the generic 0.45; pg_care_season_drain_q12() keeps
 * the generic row available and --care-test asserts the bracketing, so the two
 * statements stay tied together rather than drifting apart.
 */
static uint16_t pg_care_species_season_q12(const pg_species *species,
                                           uint16_t season_l)
{
    uint32_t dormant;

    if (species == NULL || species->base_drain_growing_q12l == 0u) {
        return (uint16_t)PG_Q12_ONE;
    }
    dormant = ((uint32_t)species->base_drain_dormant_q12l * PG_Q12_ONE)
            / species->base_drain_growing_q12l;
    if (dormant > UINT16_MAX) {
        dormant = UINT16_MAX;
    }
    return pg_lerp_l((uint16_t)dormant, (uint16_t)PG_Q12_ONE, season_l);
}

uint16_t pg_care_drain_multiplier_q12(const pg_plant *plant,
                                      const pg_care_env *env)
{
    const pg_species *species;
    const pg_pot *pot;
    uint16_t chain;

    if (plant == NULL || env == NULL) {
        return (uint16_t)PG_Q12_ONE;
    }
    species = pg_content_species(plant->species_id);
    pot = pg_content_pot(plant->pot_id);
    if (species == NULL || pot == NULL) {
        return (uint16_t)PG_Q12_ONE;
    }

    chain = pot->drain_multiplier_q12;
    chain = pg_q12_mul(chain, pg_care_light_q12(env->light_band));
    chain = pg_q12_mul(chain, pg_care_temp_q12(env->air_temp_c_x10));
    /* A species the research calls genuinely indifferent to humidity drops the
     * RH term entirely rather than being given a band it does not have. */
    if (species->rh_ideal_low != 0u || species->rh_ideal_high != 0u) {
        chain = pg_q12_mul(chain, pg_care_humidity_q12(env->rh_pct));
    }
    chain = pg_q12_mul(chain, env->airflow_q12 == 0u
                              ? (uint16_t)PG_Q12_ONE : env->airflow_q12);
    chain = pg_q12_mul(chain, pg_care_species_season_q12(species, env->season_l));
    chain = pg_q12_mul(chain, pg_care_root_mass_q12(plant->root_bound));
    return chain;
}

uint32_t pg_care_watering_interval_ticks(const pg_plant *plant,
                                         const pg_care_env *env)
{
    const pg_species *species;
    uint32_t rate_q12;
    uint32_t span_l;

    if (plant == NULL || env == NULL) {
        return 0u;
    }
    species = pg_content_species(plant->species_id);
    if (species == NULL) {
        return 0u;
    }
    rate_q12 = (uint32_t)((((uint64_t)species->base_drain_growing_q12l)
                           * (uint64_t)pg_care_drain_multiplier_q12(plant, env))
                          >> 12);
    if (rate_q12 == 0u) {
        return UINT32_MAX;
    }
    span_l = (uint32_t)PG_LEVEL_MAX - (uint32_t)species->thirst_threshold_l;
    return (uint32_t)(((uint64_t)span_l << 12) / rate_q12);
}

/* ---- one care tick ------------------------------------------------------ */

static uint16_t pg_care_soil_temp(const pg_plant *plant, const pg_care_env *env)
{
    const pg_pot *pot = pg_content_pot(plant->pot_id);
    int32_t soil = env->air_temp_c_x10;

    if (pot != NULL) {
        soil += pot->soil_temp_offset_c_x10;
    }
    if (soil < 0) {
        soil = 0;
    }
    if (soil > INT16_MAX) {
        soil = INT16_MAX;
    }
    return (uint16_t)soil;
}

static void pg_care_drain(pg_plant *plant, const pg_care_env *env,
                          uint64_t tick_index)
{
    const pg_species *species = pg_content_species(plant->species_id);
    const pg_pot *pot = pg_content_pot(plant->pot_id);
    uint32_t rate_q12;
    uint32_t top_rate, bottom_rate;
    uint32_t top_share, bottom_share;
    int32_t top, bottom;
    int32_t transfer;

    if (species == NULL || pot == NULL) {
        return;
    }

    rate_q12 = (uint32_t)((((uint64_t)species->base_drain_growing_q12l)
                           * (uint64_t)pg_care_drain_multiplier_q12(plant, env))
                          >> 12);

    /* The pot decides where the water leaves from. The whole-pot rate is
     * always the same; only its distribution between the layers changes. */
    top_share = (((uint32_t)pot->surface_share_q12 * PG_SURFACE_TO_TOP_Q12) >> 12)
              + (((uint32_t)pot->side_evaporation_share_q12
                  * PG_SIDE_TO_LAYER_Q12) >> 12)
              + (((uint32_t)pot->uptake_share_q12 * PG_UPTAKE_TO_TOP_Q12) >> 12);
    bottom_share = (((uint32_t)pot->side_evaporation_share_q12
                     * PG_SIDE_TO_LAYER_Q12) >> 12)
                 + (((uint32_t)pot->uptake_share_q12
                     * PG_UPTAKE_TO_BOTTOM_Q12) >> 12);

    top_rate = (uint32_t)(((uint64_t)rate_q12 * top_share) >> 12);
    bottom_rate = (uint32_t)(((uint64_t)rate_q12 * bottom_share) >> 12);

    top = (int32_t)plant->moisture_top - (int32_t)pg_q12_step(top_rate, tick_index);
    bottom = (int32_t)plant->moisture_bottom
           - (int32_t)pg_q12_step(bottom_rate, tick_index);

    /* Once the top third is bone dry the surface has nothing left to give, but
     * the wall and the roots have not stopped: the unspent loss moves to the
     * bottom, where the water actually is. Water is conserved across the
     * spill, so the whole root ball keeps drying at exactly the calibrated
     * rate no matter how the pot splits it. */
    if (top < 0) {
        /* Half of a level unit is not representable, so the odd half is spent
         * on odd ticks. Truncating it away instead would quietly stretch every
         * interval by a few percent — the exact drift the whole Q12 dither
         * exists to keep out. */
        int32_t half = top / 2;
        if ((top % 2) != 0 && (tick_index & 1u) != 0u) {
            half -= 1;
        }
        bottom += half;
        top = 0;
    }

    /* Gravity moves water down, which is why a sip wets the top and the bottom
     * stays dry rather than the two averaging out. Water is conserved, not
     * level: a third of the mix losing `transfer` fills two thirds of it by
     * half as much. */
    if (top > bottom) {
        transfer = ((top - bottom) * (int32_t)pg_content_percolation_q12())
                 / PG_Q12_ONE;
        top -= transfer;
        bottom += transfer / 2;
    }

    plant->moisture_top = pg_clamp_level(top);
    plant->moisture_bottom = pg_clamp_level(bottom);

    /* The perched water table. In a pot that drains this is 0 and does
     * nothing; in the cachepot it is 0.92, strictly above saturation, which is
     * what makes the soggy hours accumulate forever (D-082). */
    if (plant->moisture_bottom < pot->bottom_floor_l) {
        plant->moisture_bottom = pot->bottom_floor_l;
    }
}

static void pg_care_soggy(pg_plant *plant)
{
    if (plant->moisture_bottom >= pg_content_saturation_level()) {
        if (plant->soggy_ticks < UINT32_MAX) {
            plant->soggy_ticks += 1u;
        }
    } else {
        plant->soggy_ticks = (uint32_t)(((uint64_t)plant->soggy_ticks * 98u) / 100u);
    }
}

static void pg_care_turgor(pg_plant *plant, const pg_species *species)
{
    uint16_t available = pg_plant_available_water_l(plant);
    uint16_t threshold = species->thirst_threshold_l;
    uint32_t target;
    int32_t delta;
    int32_t step;
    uint16_t tau = species->drought_recovery_ticks;

    if (threshold < 500u) {
        threshold = 500u;
    }
    target = ((uint32_t)available * (uint32_t)PG_LEVEL_MAX) / threshold;
    if (target > (uint32_t)PG_LEVEL_MAX) {
        target = (uint32_t)PG_LEVEL_MAX;
    }
    if (tau == 0u) {
        tau = 1u;
    }
    delta = (int32_t)target - (int32_t)plant->turgor;
    step = delta / (int32_t)tau;
    if (step == 0 && delta != 0) {
        step = (delta > 0) ? 1 : -1;
    }
    plant->turgor = pg_clamp_level((int32_t)plant->turgor + step);
}

static void pg_care_roots(pg_plant *plant, const pg_species *species,
                          const pg_care_env *env)
{
    const pg_pot *pot = pg_content_pot(plant->pot_id);
    uint16_t soil_temp = pg_care_soil_temp(plant, env);
    uint32_t tolerance = species->saturation_tolerance_ticks;
    uint16_t severity_q12 = (uint16_t)PG_Q12_ONE;
    int32_t damage;

    if (tolerance == 0u) {
        tolerance = 1u;
    }

    if (plant->soggy_ticks > tolerance) {
        if (soil_temp < PG_VERY_COLD_SOIL_C_X10) {
            severity_q12 = (uint16_t)PG_VERY_COLD_ROT_Q12;
        } else if (soil_temp < PG_COLD_SOIL_C_X10) {
            severity_q12 = (uint16_t)PG_COLD_ROT_Q12;
        }
        if (pot != NULL && !pot->has_drainage) {
            severity_q12 = pg_q12_mul(severity_q12, 5325u);   /* x1.3 */
        }
        damage = (int32_t)(((uint64_t)(plant->soggy_ticks - tolerance) * 8u)
                           / tolerance) + 1;
        damage = (damage * (int32_t)severity_q12) / PG_Q12_ONE;
        plant->root_health = pg_clamp_level((int32_t)plant->root_health - damage);

        /* Severe rot lowers the permanent ceiling. Root capacity never rises. */
        if (plant->root_health < 2000u && plant->root_capacity > 0u) {
            plant->root_capacity = pg_clamp_level((int32_t)plant->root_capacity - 1);
        }
    } else if (plant->root_health < plant->root_capacity) {
        /* Repair runs at metabolic speed, which is why a winter mistake
         * genuinely costs more (D-085 floors how much more). */
        int32_t repair = 1 + (int32_t)env->effective_season_l / 4000;
        plant->root_health = pg_clamp_level((int32_t)plant->root_health + repair);
        if (plant->root_health > plant->root_capacity) {
            plant->root_health = plant->root_capacity;
        }
    }
}

static void pg_care_light_damage(pg_plant *plant, const pg_species *species,
                                 const pg_care_env *env)
{
    int32_t over;

    if (!env->is_daylight) {
        return;
    }
    if (env->light_band <= species->light_max) {
        return;
    }
    /* Acclimation is real: the same plant moved in stages over 2-3 weeks does
     * not burn, and that is a skill rather than a binary light slot. */
    over = (int32_t)env->light_band - (int32_t)species->light_max;
    over = over * 6;
    over -= (int32_t)plant->acclimation / 2000;
    if (over < 1) {
        over = 1;
    }
    plant->scorch_dose = pg_clamp_level((int32_t)plant->scorch_dose + over);
}

static void pg_care_humidity_damage(pg_plant *plant, const pg_species *species,
                                    const pg_care_env *env)
{
    int32_t deficit;

    if (species->rh_damage_below == 0u) {
        return;                    /* never damaged by dry air */
    }
    if (env->rh_pct >= species->rh_damage_below) {
        if (plant->crisp_dose > 0u) {
            /* The dose stops climbing; the marks it already made do not heal.
             * Recovery is visible only as new leaves. */
            plant->crisp_dose = (uint16_t)(plant->crisp_dose
                                           - (plant->crisp_dose > 1u ? 1u : 0u));
        }
        return;
    }
    deficit = (int32_t)species->rh_damage_below - (int32_t)env->rh_pct;
    plant->crisp_dose = pg_clamp_level((int32_t)plant->crisp_dose
                                       + 1 + deficit / 4);
}

static void pg_care_cold_damage(pg_plant *plant, const pg_species *species,
                                const pg_care_env *env)
{
    const pg_spot *spot = pg_content_spot(plant->spot_id);

    if (spot == NULL || !spot->cold_glass || env->is_daylight) {
        return;
    }
    if (env->air_temp_c_x10 >= (int16_t)species->temp_damage_below * 10) {
        return;
    }
    /* A leaf resting against cold window glass overnight. Water-soaked
     * translucent patches within a day, and they are permanent. */
    plant->crisp_dose = pg_clamp_level((int32_t)plant->crisp_dose + 4);
}

static void pg_care_pests(pg_plant *plant, const pg_care_env *env)
{
    if (plant->moisture_top >= PG_MOISTURE_WET_SURFACE_L) {
        /* Fungus gnats are harmless to the plant and are a direct visible
         * readout of overwatering, which is the whole reason they are here. */
        plant->pest_gnats = pg_clamp_level((int32_t)plant->pest_gnats + 3);
    } else {
        plant->pest_gnats = pg_clamp_level((int32_t)plant->pest_gnats - 2);
    }

    if (env->rh_pct < PG_MITE_RH_PCT && env->air_temp_c_x10 > PG_MITE_TEMP_C_X10) {
        plant->pest_mites = pg_clamp_level((int32_t)plant->pest_mites + 2);
    } else if (plant->pest_egg_timer == 0u) {
        plant->pest_mites = pg_clamp_level((int32_t)plant->pest_mites - 1);
    }

    if (plant->pest_egg_timer > 0u) {
        plant->pest_egg_timer = (uint16_t)(plant->pest_egg_timer - 1u);
        if (plant->pest_egg_timer == 0u && plant->pest_mites > 0u) {
            /* The eggs hatched. A single treatment always fails. */
            plant->pest_mites = pg_clamp_level((int32_t)plant->pest_mites + 1500);
        }
    }
}

void pg_care_tick(pg_plant *plant, const pg_care_env *env, uint64_t tick_index)
{
    const pg_species *species;
    uint16_t gained;

    if (plant == NULL || env == NULL || plant->life_state == PG_LIFE_DEAD) {
        return;
    }
    species = pg_content_species(plant->species_id);
    if (species == NULL) {
        return;
    }

    pg_care_drain(plant, env, tick_index);
    pg_care_soggy(plant);
    pg_care_turgor(plant, species);
    pg_care_roots(plant, species, env);
    pg_care_light_damage(plant, species, env);
    pg_care_humidity_damage(plant, species, env);
    pg_care_cold_damage(plant, species, env);
    pg_care_pests(plant, env);

    gained = pg_care_tick_dli_c(env);
    if (gained > 0u) {
        uint32_t total = (uint32_t)plant->dli_today + gained;
        plant->dli_today = (uint16_t)(total > UINT16_MAX ? UINT16_MAX : total);
    }

    if (plant->pending_action != PG_PENDING_NONE) {
        /* The deadline itself is care time, so a soak survives a crash, a
         * closed lid and a catch-up identically (D-089). The fine tier is what
         * resolves it; nothing here needs a wall clock. */
        plant->moisture_bottom = (uint16_t)PG_LEVEL_MAX;
    }
}

/* ---- one coarse day ----------------------------------------------------- */

void pg_care_day(pg_plant *plant, const pg_care_env *env)
{
    const pg_species *species;
    const pg_pot *pot;
    uint16_t mean_dli;
    uint16_t limiting;
    uint32_t gain;
    int32_t acclimation_target;

    if (plant == NULL || env == NULL || plant->life_state == PG_LIFE_DEAD) {
        return;
    }
    species = pg_content_species(plant->species_id);
    pot = pg_content_pot(plant->pot_id);
    if (species == NULL || pot == NULL) {
        return;
    }

    /* The DLI ring rolls at LOCAL midnight, which is why the calendar clock
     * and not the care clock owns this boundary. */
    plant->dli_ring[plant->dli_ring_head] = plant->dli_today;
    plant->dli_ring_head = (uint8_t)((plant->dli_ring_head + 1u) % PG_DLI_RING);
    plant->dli_today = 0u;
    mean_dli = pg_plant_mean_dli_c(plant);

    if (mean_dli < species->dli_maintenance_c) {
        uint32_t shortfall = (uint32_t)species->dli_maintenance_c - mean_dli;
        plant->light_debt = pg_clamp_level((int32_t)plant->light_debt
                                           + (int32_t)(shortfall * 4u));
    } else {
        plant->light_debt = pg_clamp_level((int32_t)plant->light_debt - 60);
    }

    /* Growth is the minimum of what the plant has: water, light, food and a
     * root system to move them. */
    limiting = plant->turgor;
    if (plant->root_health < limiting) {
        limiting = plant->root_health;
    }
    if (plant->nutrition < limiting) {
        limiting = plant->nutrition;
    }
    if (mean_dli >= species->dli_thriving_c) {
        /* light is not limiting */
        ;
    } else if (mean_dli <= species->dli_maintenance_c) {
        limiting = 0u;
    } else {
        uint16_t light_l = (uint16_t)(((uint32_t)(mean_dli
                                                  - species->dli_maintenance_c)
                                       * PG_LEVEL_MAX)
                                      / (uint32_t)(species->dli_thriving_c
                                                   - species->dli_maintenance_c));
        if (light_l < limiting) {
            limiting = light_l;
        }
    }
    /* Transplant shock suppresses growth while it lasts. */
    if (plant->shock > 0u) {
        limiting = pg_level_scale(limiting,
                                  (uint16_t)(PG_Q12_ONE
                                             - (uint16_t)(((uint32_t)plant->shock
                                                           * 3072u)
                                                          / PG_LEVEL_MAX)));
    }

    /* Growth RATE is not floored: a plant really does stop growing in
     * midwinter, and that is the mechanic rather than a bug (D-085). */
    gain = ((uint32_t)PG_GROWTH_POINTS_PER_DAY * env->season_l) / PG_LEVEL_MAX;
    gain = (gain * limiting) / PG_LEVEL_MAX;
    if (gain > 0u) {
        plant->growth_points += gain;
        plant->growth_stage = pg_plant_stage_for_points(plant->growth_points);
        /* Nutrition falls WITH GROWTH, not with time. */
        plant->nutrition = pg_clamp_level((int32_t)plant->nutrition
                                          - (int32_t)((gain + 3u) / 4u));
        /* Root mass grows with growth points, divided by pot volume. */
        plant->root_bound = pg_clamp_level(
            (int32_t)plant->root_bound
            + (int32_t)((gain * PG_Q12_ONE) / (pot->reservoir_q12 * 6u + 1u)));
    }

    /* Acclimation: +0.05/day toward current exposure, -0.02/day away. */
    acclimation_target = ((int32_t)env->light_band * PG_LEVEL_MAX)
                       / ((int32_t)PG_LIGHT_BAND_COUNT - 1);
    if ((int32_t)plant->acclimation < acclimation_target) {
        plant->acclimation = pg_clamp_level((int32_t)plant->acclimation + 500);
    } else if ((int32_t)plant->acclimation > acclimation_target) {
        plant->acclimation = pg_clamp_level((int32_t)plant->acclimation - 200);
    }

    /* Shock decays over 1-3 weeks, divided by the EFFECTIVE scalar, so a
     * midwinter repot costs at most 3.33x a midsummer one (D-085). */
    if (plant->shock > 0u) {
        int32_t decay = (PG_LEVEL_MAX / 14) * (int32_t)env->effective_season_l
                      / PG_LEVEL_MAX;
        if (decay < 1) {
            decay = 1;
        }
        plant->shock = pg_clamp_level((int32_t)plant->shock - decay);
    }

    /* Salt from hard water and from feeding accumulates; the pot decides
     * whether it can ever leave. */
    if (plant->purity_load > 0u) {
        plant->salt = pg_clamp_level(
            (int32_t)plant->salt
            + (int32_t)(pg_level_scale(plant->purity_load,
                                       species->water_purity_sensitivity_q12) / 200));
    }
}

/* ---- the four hard refusals -------------------------------------------- */

pg_feed_refusal pg_care_feed_refusal(const pg_plant *plant,
                                     const pg_care_env *env,
                                     uint64_t now_care_s)
{
    const pg_species *species;
    uint64_t since_repot;

    if (plant == NULL || env == NULL) {
        return PG_FEED_REFUSAL_NONE;
    }
    species = pg_content_species(plant->species_id);
    if (species == NULL) {
        return PG_FEED_REFUSAL_NONE;
    }

    /* 1. In dormancy. A plant that is not growing does not take up nutrients;
     *    the fertiliser stays in the mix as salt. */
    if (!env->is_growing_season) {
        return PG_FEED_REFUSAL_DORMANT;
    }
    /* 2. Into dry soil. Fertiliser in dry soil burns roots directly. */
    if (plant->moisture_bottom <= species->thirst_threshold_l) {
        return PG_FEED_REFUSAL_DRY_SOIL;
    }
    /* 3. Within 4-6 weeks of repotting. */
    since_repot = (now_care_s >= plant->last_repotted_care_s)
                ? (now_care_s - plant->last_repotted_care_s)
                : 0u;
    if (plant->last_repotted_care_s != 0u
        && since_repot < (uint64_t)PG_FEED_AFTER_REPOT_DAYS * 86400u) {
        return PG_FEED_REFUSAL_RECENTLY_REPOTTED;
    }
    /* 4. When the plant is visibly sick. */
    if (plant->life_state == PG_LIFE_AILING
        || plant->life_state == PG_LIFE_TERMINAL
        || plant->root_health < 3000u) {
        return PG_FEED_REFUSAL_VISIBLY_SICK;
    }
    return PG_FEED_REFUSAL_NONE;
}

const char *pg_care_refusal_reason(pg_feed_refusal refusal)
{
    switch (refusal) {
    case PG_FEED_REFUSAL_NONE:
        return "";
    case PG_FEED_REFUSAL_DORMANT:
        return "It is not growing right now, so it cannot take the food up. "
               "It would sit in the soil as salt. Wait for the light to come back.";
    case PG_FEED_REFUSAL_DRY_SOIL:
        return "The soil is dry, and food in dry soil burns roots. "
               "Water it first, then feed.";
    case PG_FEED_REFUSAL_RECENTLY_REPOTTED:
        return "It was repotted recently. Fresh mix already carries what it "
               "needs, and the roots are still mending.";
    case PG_FEED_REFUSAL_VISIBLY_SICK:
        return "The roots are struggling, and food would add to the load on a "
               "plant that already cannot drink.";
    case PG_FEED_REFUSAL_COUNT:
    default:
        return "";
    }
}

/* ---- verb legality ------------------------------------------------------ */

static const char *const PG_VERB_NAMES[PG_VERB_COUNT] = {
    "check the soil", "lift the pot", "look closely", "read the label",
    "water thoroughly", "a sip", "bottom-soak", "empty the saucer",
    "lift out of the sleeve", "put it back in the sleeve", "water source",
    "feed at quarter strength", "feed at half strength",
    "feed at full strength", "flush the pot",
    "rotate a quarter turn", "wipe the leaves", "trim damaged tissue",
    "move it", "repot", "repot two sizes up", "add a gravel layer",
    "take a cutting", "treat pests", "mist"
};

const char *pg_care_verb_name(pg_verb verb)
{
    if ((size_t)verb >= (size_t)PG_VERB_COUNT) {
        return "";
    }
    return PG_VERB_NAMES[verb];
}

bool pg_care_verb_is_feed(pg_verb verb)
{
    return verb == PG_VERB_FEED_QUARTER || verb == PG_VERB_FEED_HALF
        || verb == PG_VERB_FEED_FULL;
}

pg_legality pg_care_verb_legality(pg_verb verb, const pg_plant *plant,
                                  const pg_care_env *env, uint64_t now_care_s,
                                  const char **reason)
{
    const pg_pot *pot;

    if (plant == NULL || env == NULL || (size_t)verb >= (size_t)PG_VERB_COUNT) {
        return PG_LEGAL_ALLOWED;
    }
    pot = pg_content_pot(plant->pot_id);

    /* The four and only four hard refusals, all under Feed. A refusal is the
     * one response that teaches nothing, so everything else is offered with a
     * stated reason and behaves truthfully. */
    if (pg_care_verb_is_feed(verb)) {
        pg_feed_refusal refusal = pg_care_feed_refusal(plant, env, now_care_s);
        if (refusal != PG_FEED_REFUSAL_NONE) {
            if (reason != NULL) {
                *reason = pg_care_refusal_reason(refusal);
            }
            return PG_LEGAL_REFUSED;
        }
        if (verb == PG_VERB_FEED_FULL) {
            if (reason != NULL) {
                *reason = "Full strength is more than it can use. The surplus "
                          "stays behind as salt.";
            }
            return PG_LEGAL_WARNED;
        }
        return PG_LEGAL_ALLOWED;
    }

    switch (verb) {
    case PG_VERB_WATER_THOROUGHLY:
        if (pot != NULL && !pot->has_drainage) {
            if (reason != NULL) {
                *reason = "Nothing will run out of this one — the water stays "
                          "in the bottom.";
            }
            return PG_LEGAL_WARNED;
        }
        return PG_LEGAL_ALLOWED;
    case PG_VERB_FLUSH:
        if (pot != NULL && !pot->can_be_flushed) {
            if (reason != NULL) {
                *reason = "There is no hole for the water to leave by, so "
                          "nothing would be carried out.";
            }
            return PG_LEGAL_WARNED;
        }
        return PG_LEGAL_ALLOWED;
    case PG_VERB_REPOT_TWO_STEP:
        if (reason != NULL) {
            *reason = "That is two sizes up. The extra mix will hold water "
                      "with no roots in it to drink it.";
        }
        return PG_LEGAL_WARNED;
    case PG_VERB_REPOT_GRAVEL:
        if (reason != NULL) {
            *reason = "A layer at the bottom will hold a wet band just above "
                      "it.";
        }
        return PG_LEGAL_WARNED;
    case PG_VERB_REPOT:
        if (!env->is_growing_season) {
            if (reason != NULL) {
                *reason = "It is not growing, so it will sit in the shock "
                          "longer than it would in spring.";
            }
            return PG_LEGAL_WARNED;
        }
        return PG_LEGAL_ALLOWED;
    case PG_VERB_SLEEVE_LIFT:
    case PG_VERB_SLEEVE_RETURN:
        if (pot != NULL && !pot->is_sleeve_capable) {
            if (reason != NULL) {
                *reason = "There is nothing to lift it out of.";
            }
            return PG_LEGAL_WARNED;
        }
        return PG_LEGAL_ALLOWED;
    case PG_VERB_MIST:
        if (reason != NULL) {
            *reason = "The air around it will be damp for about twenty "
                      "minutes.";
        }
        return PG_LEGAL_WARNED;
    case PG_VERB_WATER_SIP:
        if (reason != NULL) {
            *reason = "That will wet the top and stop there.";
        }
        return PG_LEGAL_WARNED;
    case PG_VERB_CUTTING:
        if (!pg_plant_mercy_offered(plant)) {
            if (reason != NULL) {
                *reason = "You can take a piece any time; it does not need to "
                          "be in trouble first.";
            }
            return PG_LEGAL_WARNED;
        }
        return PG_LEGAL_ALLOWED;
    case PG_VERB_TREAT_PESTS:
        if (reason != NULL) {
            *reason = "One treatment will not finish it — the eggs are still "
                      "to hatch.";
        }
        return PG_LEGAL_WARNED;
    default:
        return PG_LEGAL_ALLOWED;
    }
}

/* ---- effects ------------------------------------------------------------ */

void pg_care_water_thoroughly(pg_plant *plant, uint64_t now_care_s)
{
    const pg_pot *pot;

    if (plant == NULL) {
        return;
    }
    pot = pg_content_pot(plant->pot_id);
    plant->moisture_top = (uint16_t)PG_LEVEL_MAX;
    plant->moisture_bottom = (uint16_t)PG_LEVEL_MAX;
    plant->last_watered_care_s = now_care_s;
    /* In a pot with no drainage nothing runs out, so no salt leaves. The
     * action still succeeds — there are exactly four refusals and they are all
     * under Feed. */
    if (pot != NULL && pot->has_drainage) {
        plant->salt = pg_level_scale(plant->salt, (uint16_t)(PG_Q12_ONE
                                                             - PG_WATER_SALT_FLUSH_Q12));
    }
}

void pg_care_water_sip(pg_plant *plant, uint64_t now_care_s)
{
    if (plant == NULL) {
        return;
    }
    /* The top wets, the bottom stays dry, salts concentrate. It visibly
     * succeeds and the plant does not improve. */
    plant->moisture_top = (uint16_t)PG_LEVEL_MAX;
    plant->last_watered_care_s = now_care_s;
    plant->salt = pg_clamp_level((int32_t)plant->salt + 40);
}

void pg_care_flush(pg_plant *plant, uint64_t now_care_s)
{
    const pg_pot *pot;

    if (plant == NULL) {
        return;
    }
    pot = pg_content_pot(plant->pot_id);
    plant->moisture_top = (uint16_t)PG_LEVEL_MAX;
    plant->moisture_bottom = (uint16_t)PG_LEVEL_MAX;
    plant->last_watered_care_s = now_care_s;
    if (pot != NULL && pot->can_be_flushed) {
        plant->salt = pg_level_scale(plant->salt, (uint16_t)PG_FLUSH_SALT_KEEP_Q12);
        plant->purity_load = pg_level_scale(plant->purity_load,
                                            (uint16_t)PG_FLUSH_SALT_KEEP_Q12);
    }
}

void pg_care_feed(pg_plant *plant, uint16_t strength_q12, uint64_t now_care_s)
{
    const pg_species *species;
    const pg_pot *pot;
    uint16_t surplus_q12;

    if (plant == NULL) {
        return;
    }
    species = pg_content_species(plant->species_id);
    pot = pg_content_pot(plant->pot_id);
    if (species == NULL || pot == NULL) {
        return;
    }

    plant->nutrition = pg_clamp_level((int32_t)plant->nutrition
                                      + (int32_t)pg_level_scale(6000u, strength_q12));
    plant->last_fed_care_s = now_care_s;

    /* Anything beyond what the plant can use stays behind as salt, and the pot
     * decides how much of it stays. */
    surplus_q12 = (strength_q12 > species->feed_strength_q12)
                ? (uint16_t)(strength_q12 - species->feed_strength_q12)
                : 0u;
    plant->salt = pg_clamp_level(
        (int32_t)plant->salt
        + (int32_t)pg_level_scale((uint16_t)(300u + surplus_q12 / 4u),
                                  pot->salt_retention_q12));
}

void pg_care_repot(pg_plant *plant, uint8_t new_pot_id, bool two_step,
                   bool gravel_layer, uint64_t now_care_s)
{
    const pg_pot *pot;

    if (plant == NULL) {
        return;
    }
    pot = pg_content_pot(new_pot_id);
    if (pot != NULL) {
        plant->pot_id = new_pot_id;
    }
    plant->root_bound = 0u;
    plant->shock = (uint16_t)PG_LEVEL_MAX;
    plant->nutrition = pg_clamp_level((int32_t)plant->nutrition + 2500);
    plant->last_repotted_care_s = now_care_s;

    /* Both are allowed, warned and truthful. "Repotting into a much bigger
     * pot" is itself on the myth blocklist, so refusing it would teach the
     * player the opposite of the truth. */
    if (two_step) {
        plant->moisture_bottom = (uint16_t)PG_LEVEL_MAX;
    }
    if (gravel_layer) {
        plant->moisture_bottom = (uint16_t)PG_LEVEL_MAX;
    }
}

void pg_care_wipe_leaves(pg_plant *plant)
{
    if (plant == NULL) {
        return;
    }
    /* Dust cuts effective light; wiping it off gives that light back, which is
     * why the most affectionate verb in the game has a real effect. */
    plant->light_debt = pg_clamp_level(
        (int32_t)plant->light_debt
        - (int32_t)pg_level_scale(plant->light_debt, (uint16_t)PG_DUST_LIGHT_LOSS_Q12));
}

/* ---- --care-test -------------------------------------------------------- */

static int pg_care_checks_failed;

static void pg_care_check(bool condition, const char *what)
{
    if (!condition) {
        pg_care_checks_failed += 1;
        (void)printf("  FAIL  %s\n", what);
    }
}

/* A plant sitting in a room at the safe default spot, at midsummer, with
 * everything else at the §1.3 baseline. */
static pg_care_env pg_care_baseline_env(uint16_t ordinal)
{
    pg_care_env env;

    memset(&env, 0, sizeof env);
    env.ordinal = ordinal;
    env.local_minutes = 720u;
    env.season_l = pg_calendar_season_l(ordinal);
    env.effective_season_l = pg_calendar_effective_season_l(env.season_l);
    env.daylight_minutes = pg_calendar_daylight_minutes(ordinal);
    env.is_growing_season = pg_calendar_is_growing(env.season_l);
    env.is_daylight = true;
    /* The baseline of PLANT_CARE.md §1.3: 20-22 C, 45 % RH, bright indirect
     * light, a glazed pot with a hole, still air, growing season. Every
     * multiplier is therefore exactly 1.0 and the tabled interval must come
     * back out. */
    env.air_temp_c_x10 = 210;
    env.rh_pct = 45u;
    env.airflow_q12 = (uint16_t)PG_Q12_ONE;
    env.light_band = (uint8_t)PG_LIGHT_BRIGHT_INDIRECT;
    env.dli_day_c = 1250u;
    return env;
}

/* Ticks from a thorough watering to the species' thirst threshold, measured on
 * the WHOLE root ball. UINT32_MAX means it never gets there — which is the
 * literal truth about a cachepot and is the point of the pot. */
static uint32_t pg_care_ticks_to_thirst(uint8_t species_id, uint8_t pot_id,
                                        const pg_care_env *env)
{
    pg_plant plant;
    const pg_species *species = pg_content_species(species_id);
    uint32_t ticks = 0u;

    pg_plant_init(&plant, species_id, pot_id, (uint8_t)PG_SPOT_DEFAULT, 0);
    plant.moisture_top = (uint16_t)PG_LEVEL_MAX;
    plant.moisture_bottom = (uint16_t)PG_LEVEL_MAX;
    /* "an established plant whose roots fill but do not circle the pot" is the
     * root-mass pivot of PLANT_CARE.md §1.3, so the root term is exactly 1.0. */
    plant.root_bound = 7000u;
    while (ticks < 100000u) {
        pg_care_drain(&plant, env, ticks);
        ticks += 1u;
        if (pg_care_moisture_whole_l(&plant) <= species->thirst_threshold_l) {
            return ticks;
        }
    }
    return UINT32_MAX;
}

static void pg_care_print_days(uint32_t ticks)
{
    if (ticks == UINT32_MAX) {
        (void)printf("never");
    } else {
        (void)printf("%.1fd", (double)ticks / (double)PG_CARE_TICKS_PER_DAY);
    }
}

static void pg_care_report_species(uint8_t species_id)
{
    const pg_species *species = pg_content_species(species_id);
    pg_care_env summer = pg_care_baseline_env(172u);
    pg_care_env winter = pg_care_baseline_env(355u);
    uint32_t summer_ticks, winter_ticks;
    uint32_t tabled_ticks;
    int32_t error_permille;
    uint8_t pot;

    summer_ticks = pg_care_ticks_to_thirst(species_id, (uint8_t)PG_POT_GLAZED,
                                           &summer);
    winter.light_band = (uint8_t)PG_LIGHT_BRIGHT_INDIRECT;
    winter.air_temp_c_x10 = 210;
    winter.rh_pct = 45u;
    winter.airflow_q12 = (uint16_t)PG_Q12_ONE;
    winter_ticks = pg_care_ticks_to_thirst(species_id, (uint8_t)PG_POT_GLAZED,
                                           &winter);

    tabled_ticks = ((uint32_t)species->water_interval_growing_dh
                    * PG_CARE_TICKS_PER_DAY) / 10u;
    error_permille = (int32_t)((int64_t)((int64_t)summer_ticks
                                         - (int64_t)tabled_ticks) * 1000
                               / (int64_t)tabled_ticks);

    (void)printf("%-8s growing ", species->default_plant_name);
    pg_care_print_days(summer_ticks);
    (void)printf(" (tabled %.1fd, %+d permille)  midwinter ",
                 (double)species->water_interval_growing_dh / 10.0,
                 error_permille);
    pg_care_print_days(winter_ticks);
    (void)printf(" (tabled dormant %.1fd)\n",
                 (double)species->water_interval_dormant_dh / 10.0);

    /* At the baseline with every multiplier at 1.0 the tabled interval must
     * come back out. Two percent of slack covers the integer dither, nothing
     * more. */
    pg_care_check(error_permille > -20 && error_permille < 20,
                  "baseline drain reproduces the tabled growing interval");
    /* And the midwinter interval must land inside the research's dormant band,
     * which is what the per-species season term exists to reproduce. */
    pg_care_check(winter_ticks != UINT32_MAX
                  && winter_ticks * 10u
                     > (uint32_t)species->water_interval_dormant_dh
                       * PG_CARE_TICKS_PER_DAY * 8u / 10u
                  && winter_ticks * 10u
                     < (uint32_t)species->water_interval_dormant_dh
                       * PG_CARE_TICKS_PER_DAY * 12u / 10u,
                  "the midwinter interval lands within a fifth of the tabled "
                  "dormant interval");

    (void)printf("    pots:");
    for (pot = 0; pot < (uint8_t)PG_POT_COUNT; ++pot) {
        uint32_t ticks = pg_care_ticks_to_thirst(species_id, pot, &summer);
        (void)printf(" %s ", pg_content_pot(pot)->id);
        pg_care_print_days(ticks);
    }
    (void)printf("\n");
}

static void pg_care_report_ladders(uint8_t species_id)
{
    const pg_species *species = pg_content_species(species_id);
    pg_care_env env = pg_care_baseline_env(172u);
    pg_plant dry, wet, dim, arid, salty, bound;
    uint32_t tick;

    pg_plant_init(&dry, species_id, (uint8_t)PG_POT_GLAZED,
                  (uint8_t)PG_SPOT_DEFAULT, 0);
    pg_plant_init(&wet, species_id, (uint8_t)PG_POT_CACHEPOT,
                  (uint8_t)PG_SPOT_DEFAULT, 0);
    pg_plant_init(&dim, species_id, (uint8_t)PG_POT_GLAZED,
                  (uint8_t)PG_SPOT_NORTH_SHELF, 0);
    pg_plant_init(&arid, species_id, (uint8_t)PG_POT_GLAZED,
                  (uint8_t)PG_SPOT_RADIATOR_SHELF, 0);
    pg_plant_init(&salty, species_id, (uint8_t)PG_POT_CACHEPOT,
                  (uint8_t)PG_SPOT_DEFAULT, 0);
    pg_plant_init(&bound, species_id, (uint8_t)PG_POT_GLAZED,
                  (uint8_t)PG_SPOT_DEFAULT, 0);

    dry.moisture_top = 0u;
    dry.moisture_bottom = 0u;
    bound.root_bound = (uint16_t)PG_LEVEL_MAX;
    salty.salt = (uint16_t)PG_LEVEL_MAX;
    /* The overwater cases start from a thorough watering, because that is how
     * a person actually arrives at them. */
    wet.moisture_top = (uint16_t)PG_LEVEL_MAX;
    wet.moisture_bottom = (uint16_t)PG_LEVEL_MAX;

    for (tick = 0u; tick < 7u * 96u; ++tick) {
        pg_care_tick(&dry, &env, tick);
        pg_care_tick(&wet, &env, tick);
        pg_care_tick(&salty, &env, tick);
    }
    {
        pg_care_env dry_air = env;
        dry_air.rh_pct = 22u;
        for (tick = 0u; tick < 4u * 96u; ++tick) {
            pg_care_tick(&arid, &dry_air, tick);
        }
    }
    {
        pg_care_env dark = env;
        dark.light_band = (uint8_t)PG_LIGHT_LOW;
        dark.dli_day_c = 100u;
        for (tick = 0u; tick < 14u * 96u; ++tick) {
            pg_care_tick(&dim, &dark, tick);
            if ((tick % 96u) == 95u) {
                pg_care_day(&dim, &dark);
            }
        }
    }

    (void)printf("    ladders: underwater %u  overwater %u  low-light %u  "
                 "low-humidity %u  salt %u  rootbound %u\n",
                 pg_plant_ladder_rung(&dry, PG_LADDER_UNDERWATER),
                 pg_plant_ladder_rung(&wet, PG_LADDER_OVERWATER),
                 pg_plant_ladder_rung(&dim, PG_LADDER_LOW_LIGHT),
                 pg_plant_ladder_rung(&arid, PG_LADDER_LOW_HUMIDITY),
                 pg_plant_ladder_rung(&salty, PG_LADDER_SALT),
                 pg_plant_ladder_rung(&bound, PG_LADDER_ROOTBOUND));

    pg_care_check(pg_plant_ladder_rung(&dry, PG_LADDER_UNDERWATER) > 0u,
                  "a week with no water shows on the underwater ladder");
    pg_care_check(pg_plant_ladder_rung(&wet, PG_LADDER_OVERWATER) > 0u,
                  "a week in a cachepot shows on the overwater ladder");
    pg_care_check(pg_plant_ladder_rung(&dim, PG_LADDER_LOW_LIGHT) > 0u,
                  "a fortnight on the north shelf shows on the low-light ladder");
    if (species->rh_damage_below != 0u) {
        pg_care_check(pg_plant_ladder_rung(&arid, PG_LADDER_LOW_HUMIDITY) > 0u,
                      "four days at 22 % RH shows on the low-humidity ladder");
    }
    pg_care_check(pg_plant_ladder_rung(&bound, PG_LADDER_ROOTBOUND) > 0u,
                  "a full root ball shows on the rootbound ladder");
}

static void pg_care_report_feed(uint8_t species_id)
{
    pg_care_env growing = pg_care_baseline_env(172u);
    pg_care_env dormant = pg_care_baseline_env(355u);
    pg_plant plant;
    pg_feed_refusal refusal;

    pg_plant_init(&plant, species_id, (uint8_t)PG_POT_GLAZED,
                  (uint8_t)PG_SPOT_DEFAULT, 0);
    plant.moisture_bottom = (uint16_t)PG_LEVEL_MAX;
    plant.last_repotted_care_s = 0u;

    refusal = pg_care_feed_refusal(&plant, &growing, 0u);
    pg_care_check(refusal == PG_FEED_REFUSAL_NONE,
                  "a watered, settled, healthy plant in the growing season may be fed");

    refusal = pg_care_feed_refusal(&plant, &dormant, 0u);
    pg_care_check(refusal == PG_FEED_REFUSAL_DORMANT,
                  "feeding in dormancy is refused");

    plant.moisture_bottom = 0u;
    refusal = pg_care_feed_refusal(&plant, &growing, 0u);
    pg_care_check(refusal == PG_FEED_REFUSAL_DRY_SOIL,
                  "feeding into dry soil is refused");
}

int pg_care_run_test(void)
{
    char error[128];
    uint8_t species_id;
    uint16_t ordinal;
    pg_plant cachepot;
    pg_care_env env;
    uint32_t tick;
    uint32_t worst_stretch_permille = 0u;
    uint16_t worst_ordinal = 0u;

    pg_care_checks_failed = 0;

    if (!pg_content_validate(error, sizeof error)) {
        (void)printf("care: content invalid: %s\n", error);
        return 1;
    }
    (void)printf("content: 4 species, 4 pots, 6 spots, every threshold reachable\n");
    (void)printf("\n-- watering intervals, derived and never stored --\n");
    for (species_id = 0; species_id < (uint8_t)PG_SPECIES_COUNT; ++species_id) {
        pg_care_report_species(species_id);
        pg_care_report_ladders(species_id);
        pg_care_report_feed(species_id);
    }

    /* The four species' own growing/dormant ratios must bracket the generic
     * x0.45 Season row of PLANT_CARE.md §4.2, which is what keeps the
     * per-species pair of §9 and the generic row in the same document
     * consistent with one another. */
    {
        uint32_t lowest = UINT32_MAX;
        uint32_t highest = 0u;
        uint16_t generic = pg_care_season_drain_q12(0u);

        for (species_id = 0; species_id < (uint8_t)PG_SPECIES_COUNT; ++species_id) {
            const pg_species *sp = pg_content_species(species_id);
            uint32_t ratio = ((uint32_t)sp->base_drain_dormant_q12l * PG_Q12_ONE)
                           / sp->base_drain_growing_q12l;
            if (ratio < lowest) {
                lowest = ratio;
            }
            if (ratio > highest) {
                highest = ratio;
            }
        }
        (void)printf("\nseason: species dormant ratios %.2f..%.2f bracket the "
                     "generic x%.2f row\n",
                     (double)lowest / (double)PG_Q12_ONE,
                     (double)highest / (double)PG_Q12_ONE,
                     (double)generic / (double)PG_Q12_ONE);
        pg_care_check(lowest <= generic && generic <= highest,
                      "the generic season multiplier lies inside the species range");
    }

    /* D-082, stated exactly: at rest in a cachepot the soggy ticks are strictly
     * monotone and never decay, because the bottom floor sits strictly above
     * the saturation threshold. */
    env = pg_care_baseline_env(172u);
    pg_plant_init(&cachepot, (uint8_t)PG_SPECIES_SNAKE, (uint8_t)PG_POT_CACHEPOT,
                  (uint8_t)PG_SPOT_DEFAULT, 0);
    cachepot.moisture_top = 0u;
    cachepot.moisture_bottom = 0u;
    for (tick = 0u; tick < 200u; ++tick) {
        uint32_t before = cachepot.soggy_ticks;
        pg_care_tick(&cachepot, &env, tick);
        if (cachepot.soggy_ticks != before + 1u) {
            pg_care_check(false, "soggy ticks are strictly monotone in a cachepot");
            break;
        }
    }
    (void)printf("\ncachepot: 200 care ticks at rest -> soggy_ticks = %u\n",
                 cachepot.soggy_ticks);
    pg_care_check(cachepot.soggy_ticks == 200u,
                  "200 care ticks at rest in a cachepot is exactly 200 soggy ticks");

    /* D-085, stated exactly: no recovery, shock or fair-warning interval
     * exceeds 4x its growing-season value at any day ordinal, because every
     * such division uses max(growth_scalar, 0.30). */
    for (ordinal = 1u; ordinal <= PG_ORDINAL_MAX; ++ordinal) {
        uint16_t effective = pg_calendar_effective_season_l(
            pg_calendar_season_l(ordinal));
        uint32_t stretch_permille;

        if (effective == 0u) {
            pg_care_check(false, "the effective season scalar is never zero");
            break;
        }
        stretch_permille = ((uint32_t)PG_LEVEL_MAX * 1000u) / effective;
        if (stretch_permille > worst_stretch_permille) {
            worst_stretch_permille = stretch_permille;
            worst_ordinal = ordinal;
        }
    }
    (void)printf("recovery: worst stretch %.2fx at ordinal %u "
                 "(floor 0.30 caps it at 3.33x)\n",
                 (double)worst_stretch_permille / 1000.0, worst_ordinal);
    pg_care_check(worst_stretch_permille <= 4000u,
                  "no recovery interval exceeds 4x its growing-season value");

    (void)printf("\ncare: %s\n", pg_care_checks_failed == 0 ? "PASS" : "FAIL");
    return pg_care_checks_failed == 0 ? 0 : 1;
}
