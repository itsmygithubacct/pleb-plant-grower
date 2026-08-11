/*
 * pg_sim — the catch-up engine.
 *
 * A player closes the lid for a week and comes back to a week of consequences.
 * That is the whole feature, and it has exactly three properties it must have.
 *
 *   DETERMINISTIC. `pg_advance` is a pure function of (saved record, now).
 *   Anchors and plant state live in one record written by one save, so a crash
 *   half way through a catch-up recomputes the identical result from the
 *   identical anchor (ARCHITECTURE.md §5.5). Nothing here reads a clock, no
 *   float enters the plant record, and the catch-up draws nothing from the
 *   persisted RNG stream: weather is a pure function of (day ordinal, seed)
 *   through the stateless hash, so a replay produces the same July it produced
 *   the first time. The stream belongs to the verbs, whose order of
 *   consumption really is part of the history.
 *
 *   BOUNDED. Credit is capped at 30 days (pg_time.c), 30 days is 2880 care
 *   ticks, and a gap beyond the cap is resolved once instead of simulated. So
 *   the worst case for any gap — an hour, a fortnight, four years — is 2880
 *   tick steps plus 30 day steps (D-029, D-043), which is microseconds.
 *
 *   HONEST. The fine tier's 900 s resolution over the first 72 h is what lets
 *   the away report say "Wednesday 14:00 — collapsed" rather than "something
 *   happened this week" (GAME_DESIGN.md §10.5).
 *
 * The one thing worth being explicit about, because it is the difference
 * between this engine and the usual idle-game catch-up: **the tiers differ in
 * how finely the gap is observed, never in how coarsely it is computed.** Both
 * integrate the same 900 s care tick. A coarse day is not an approximation of
 * a day's ticks; it is a day's ticks, with one summary line instead of
 * ninety-six chances to emit one. That is what makes the save-equals-ticks
 * equivalence gate an identity rather than a tolerance, and it is why the
 * plan's own worst case is quoted in *fine ticks* (2880 = 30 days × 96).
 *
 * Transactionality follows the campaign-advance shape already proven on this
 * fleet: pre-loop overflow guard, whole-state snapshot, per-step validate,
 * all-or-nothing rollback.
 */
#include "pg_sim.h"

#include "pg_actions.h"
#include "pg_calendar.h"
#include "pg_care.h"
#include "pg_content.h"
#include "pg_plant.h"
#include "pg_state.h"
#include "pg_ui.h"
#include "pg_time.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Growth spends itself on visible things: a leaf per so many growth points, a
 * vine node rather sooner, and the peace lily's spathe steps on a weekly
 * cadence so that it is a season's achievement rather than a two-day one. */
#define PG_POINTS_PER_LEAF 1500u
#define PG_POINTS_PER_VINE_NODE 800u
#define PG_SPATHE_STEP_DAYS 7u

/* The tick at which a poisoned advance corrupts itself. Test seam only; zero
 * in every shipping path. */
static uint32_t pg_sim_poison_tick;

void pg_sim_poison_after(uint32_t after_ticks)
{
    pg_sim_poison_tick = after_ticks;
}

/* ---- lifecycle ---------------------------------------------------------- */

size_t pg_state_size(void)
{
    return sizeof(pg_state);
}

void pg_init(pg_state *state, uint64_t seed)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof *state);
    state->save_sequence = 1u;
    pg_time_anchor_init(&state->anchor);
    /* The stream and the weather seed start from the same number and then
     * diverge on purpose: one advances, the other never does. */
    pg_rng_seed(&state->rng, seed);
    state->weather_seed = seed;
    state->hemisphere = (uint8_t)PG_HEMISPHERE_NORTHERN;
    state->time_scale = 1u;
    state->plant_count = 1u;
    /* A plant arrives from the shop in its nursery pot; the chooser replaces
     * all three of these before the first advance if the player wants. */
    pg_plant_init(&state->plants[0], (uint8_t)PG_SPECIES_POTHOS,
                  (uint8_t)PG_POT_NURSERY, (uint8_t)PG_SPOT_DEFAULT, 0);
    pg_ui_init(&state->ui);
}

static bool pg_fail_state(char *error, size_t error_size, const char *what)
{
    if (error != NULL && error_size > 0u) {
        (void)snprintf(error, error_size, "%s", what);
    }
    return false;
}

static bool pg_fail_plant(char *error, size_t error_size, const char *what,
                          size_t index)
{
    if (error != NULL && error_size > 0u) {
        (void)snprintf(error, error_size, "plant %u: %s", (unsigned)index, what);
    }
    return false;
}

/* Every level is a level, every DLI is a DLI, and every id names something
 * that exists. This runs after every step of a catch-up, so it is the thing
 * that decides whether a gap is credited at all. */
bool pg_validate(const pg_state *state, char *error, size_t error_size)
{
    size_t index;

    if (error != NULL && error_size > 0u) {
        error[0] = '\0';
    }
    if (state == NULL) {
        return pg_fail_state(error, error_size, "state is NULL");
    }
    if (state->plant_count > (uint8_t)PG_PLANT_MAX) {
        return pg_fail_state(error, error_size, "plant_count out of range");
    }
    if (state->hemisphere > (uint8_t)PG_HEMISPHERE_SOUTHERN) {
        return pg_fail_state(error, error_size, "hemisphere out of range");
    }
    if (state->time_scale == 0u) {
        return pg_fail_state(error, error_size, "time_scale is zero");
    }
    if (state->clock.care_residual_s >= (uint32_t)PG_CARE_TICK_SECONDS) {
        return pg_fail_state(error, error_size, "care residual is a whole tick");
    }
    /* The residual is the total modulo one tick, always. If it is not, ticks
     * have been credited by some path other than pg_time_credit. */
    if (state->clock.care_seconds_total % (uint64_t)PG_CARE_TICK_SECONDS
        != (uint64_t)state->clock.care_residual_s) {
        return pg_fail_state(error, error_size, "care clock is inconsistent");
    }
    if (state->anchor.tz_offset_minutes
        != pg_time_clamp_tz_minutes(state->anchor.tz_offset_minutes)) {
        return pg_fail_state(error, error_size, "tz offset out of range");
    }
    if (state->journal_head >= (uint8_t)PG_JOURNAL_RING
        || state->journal_count > (uint8_t)PG_JOURNAL_RING) {
        return pg_fail_state(error, error_size, "journal ring out of range");
    }

    for (index = 0u; index < (size_t)state->plant_count; ++index) {
        const pg_plant *plant = &state->plants[index];
        const uint16_t *levels[13];
        size_t axis;
        size_t leaf;

        if (plant->species_id >= (uint8_t)PG_SPECIES_COUNT
            || pg_content_species(plant->species_id) == NULL) {
            return pg_fail_plant(error, error_size, "species id", index);
        }
        if (plant->pot_id >= (uint8_t)PG_POT_COUNT
            || pg_content_pot(plant->pot_id) == NULL) {
            return pg_fail_plant(error, error_size, "pot id", index);
        }
        if (plant->spot_id >= (uint8_t)PG_SPOT_COUNT
            || pg_content_spot(plant->spot_id) == NULL) {
            return pg_fail_plant(error, error_size, "spot id", index);
        }
        if (plant->scene_id >= (uint8_t)PG_SCENE_COUNT) {
            return pg_fail_plant(error, error_size, "scene id", index);
        }
        if (plant->life_state >= (uint8_t)PG_LIFE_STATE_COUNT) {
            return pg_fail_plant(error, error_size, "life state", index);
        }
        if (plant->growth_stage >= (uint8_t)PG_STAGE_COUNT) {
            return pg_fail_plant(error, error_size, "growth stage", index);
        }
        if (plant->spathe_state > (uint8_t)PG_SPATHE_FADING) {
            return pg_fail_plant(error, error_size, "spathe state", index);
        }
        if (plant->pending_action > (uint8_t)PG_PENDING_FLUSH) {
            return pg_fail_plant(error, error_size, "pending action", index);
        }
        if (plant->leaf_count > (uint8_t)PG_LEAF_MAX) {
            return pg_fail_plant(error, error_size, "leaf count", index);
        }
        if (plant->vine_count > (uint8_t)PG_VINE_MAX) {
            return pg_fail_plant(error, error_size, "vine count", index);
        }
        if (plant->dli_ring_head >= (uint8_t)PG_DLI_RING) {
            return pg_fail_plant(error, error_size, "dli ring head", index);
        }

        /* Every `_l` quantity is a level and lives in [0, 10000] (D-083). */
        levels[0] = &plant->moisture_top;
        levels[1] = &plant->moisture_bottom;
        levels[2] = &plant->light_debt;
        levels[3] = &plant->nutrition;
        levels[4] = &plant->salt;
        levels[5] = &plant->purity_load;
        levels[6] = &plant->root_health;
        levels[7] = &plant->root_capacity;
        levels[8] = &plant->root_bound;
        levels[9] = &plant->turgor;
        levels[10] = &plant->scorch_dose;
        levels[11] = &plant->crisp_dose;
        levels[12] = &plant->acclimation;
        for (axis = 0u; axis < sizeof levels / sizeof levels[0]; ++axis) {
            if (*levels[axis] > (uint16_t)PG_LEVEL_MAX) {
                return pg_fail_plant(error, error_size, "level out of range",
                                     index);
            }
        }
        if (plant->shock > (uint16_t)PG_LEVEL_MAX
            || plant->pest_gnats > (uint16_t)PG_LEVEL_MAX
            || plant->pest_mites > (uint16_t)PG_LEVEL_MAX
            || plant->pest_mealy > (uint16_t)PG_LEVEL_MAX) {
            return pg_fail_plant(error, error_size, "level out of range", index);
        }
        if (plant->root_health > plant->root_capacity) {
            return pg_fail_plant(error, error_size,
                                 "root health above its ceiling", index);
        }
        /* `_c` DLI lives in [0, 3000] (D-083). */
        if (plant->dli_today > 3000u) {
            return pg_fail_plant(error, error_size, "dli today out of range",
                                 index);
        }
        for (axis = 0u; axis < (size_t)PG_DLI_RING; ++axis) {
            if (plant->dli_ring[axis] > 3000u) {
                return pg_fail_plant(error, error_size, "dli ring out of range",
                                     index);
            }
        }
        for (leaf = 0u; leaf < (size_t)plant->leaf_count; ++leaf) {
            if (plant->leaves[leaf].slot >= (uint8_t)PG_LEAF_MAX) {
                return pg_fail_plant(error, error_size, "leaf slot", index);
            }
        }
        if (plant->name[PG_NAME_BYTES - 1] != '\0') {
            return pg_fail_plant(error, error_size, "name is not terminated",
                                 index);
        }
    }
    return true;
}

/* ---- the event ring ----------------------------------------------------- */

static const char *const PG_JOURNAL_LINES[PG_JOURNAL_KIND_COUNT] = {
    "",
    "got thirsty",
    "collapsed",
    "picked itself back up",
    "the soil was still wet",
    "the roots took damage",
    "the soak came up to time",
    "the flush ran through",
    "a new leaf opened",
    "put on some size",
    "a spathe",
    "the jug on the sill filled",
    "the day passed",
    "settled into dormancy",
    "started moving again",
    "having a hard time",
    "in real trouble now",
    "did not make it",
    "a month on its own",
    "the calendar on the wall looks confused",
    "watered",
    "fed",
    "repotted",
    "moved"
};

const char *pg_journal_kind_line(uint8_t kind)
{
    if (kind >= (uint8_t)PG_JOURNAL_KIND_COUNT) {
        return "";
    }
    return PG_JOURNAL_LINES[kind];
}

bool pg_journal_detail_is_hour(uint8_t kind)
{
    switch ((pg_journal_kind)kind) {
    case PG_JOURNAL_DAY_SUMMARY:   /* the health index the day ended on */
    case PG_JOURNAL_LEFT_ALONE:    /* the health index it came back at  */
    case PG_JOURNAL_NEW_LEAF:      /* how many leaves there are now     */
    case PG_JOURNAL_GREW:          /* the new growth stage              */
    case PG_JOURNAL_SPATHE:        /* the new spathe state              */
    case PG_JOURNAL_RAIN_JUG_FILLED: /* how hard it rained              */
        return false;
    default:
        return true;
    }
}

void pg_journal_push(pg_state *state, uint32_t care_day, uint8_t kind,
                     uint8_t detail, uint8_t plant_index)
{
    pg_journal_entry *entry;

    if (state == NULL || kind == (uint8_t)PG_JOURNAL_NONE
        || kind >= (uint8_t)PG_JOURNAL_KIND_COUNT) {
        return;
    }
    entry = &state->journal[state->journal_head];
    entry->care_day = care_day;
    entry->kind = kind;
    entry->detail = detail;
    entry->plant_index = plant_index;
    state->journal_head = (uint8_t)((state->journal_head + 1u)
                                    % (uint8_t)PG_JOURNAL_RING);
    if (state->journal_count < (uint8_t)PG_JOURNAL_RING) {
        state->journal_count = (uint8_t)(state->journal_count + 1u);
    }
}

size_t pg_journal_recent(const pg_state *state, pg_journal_entry *out,
                         size_t capacity)
{
    size_t count;
    size_t start;
    size_t index;

    if (state == NULL || out == NULL || capacity == 0u) {
        return 0u;
    }
    count = (size_t)state->journal_count;
    if (count > capacity) {
        count = capacity;
    }
    start = ((size_t)state->journal_head + (size_t)PG_JOURNAL_RING - count)
          % (size_t)PG_JOURNAL_RING;
    for (index = 0u; index < count; ++index) {
        out[index] = state->journal[(start + index) % (size_t)PG_JOURNAL_RING];
    }
    return count;
}

/* ---- where and when ----------------------------------------------------- */

static int64_t pg_sim_local_day(int64_t wall_s, int32_t tz_minutes)
{
    int64_t local = pg_time_saturating_add(wall_s, (int64_t)tz_minutes * 60);
    int64_t day = local / PG_SECONDS_PER_DAY;

    if (local < 0 && (local % PG_SECONDS_PER_DAY) != 0) {
        day -= 1;
    }
    return day;
}

uint32_t pg_sim_care_day(const pg_state *state, int64_t wall_s)
{
    int64_t days;

    if (state == NULL) {
        return 0u;
    }
    days = pg_sim_local_day(wall_s, state->anchor.tz_offset_minutes)
         - pg_sim_local_day(state->first_run_wall_s,
                            state->anchor.tz_offset_minutes);
    if (days < 0) {
        return 0u;
    }
    if (days > (int64_t)UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)days;
}

pg_care_env pg_sim_env_at(const pg_state *state, uint8_t plant_index,
                          int64_t wall_s)
{
    pg_care_env env;
    pg_date date;
    uint16_t ordinal = 1u;
    uint16_t minutes = 0u;
    uint8_t spot = (uint8_t)PG_SPOT_DEFAULT;

    memset(&env, 0, sizeof env);
    if (state == NULL || plant_index >= (uint8_t)PG_PLANT_MAX) {
        return env;
    }
    spot = state->plants[plant_index].spot_id;
    /* A corrupt or saturated anchor must not be able to stop the simulation:
     * an unrepresentable second is clamped into the calendar's range and the
     * day is simulated at that clamped point. */
    wall_s = pg_time_clamp_i64(wall_s, PG_UNIX_MIN, PG_UNIX_MAX);
    if (pg_calendar_local(wall_s, state->anchor.tz_offset_minutes, &date)) {
        ordinal = pg_calendar_season_ordinal(&date, state->hemisphere);
        minutes = pg_calendar_local_minutes(&date);
    }
    return pg_care_env_for(spot, ordinal, minutes, state->weather_seed);
}

pg_care_env pg_sim_env_now(const pg_state *state, uint8_t plant_index)
{
    if (state == NULL) {
        pg_care_env env;
        memset(&env, 0, sizeof env);
        return env;
    }
    return pg_sim_env_at(state, plant_index, state->anchor.last_wall_s);
}

static uint8_t pg_sim_local_hour(const pg_state *state, int64_t wall_s)
{
    pg_date date;

    wall_s = pg_time_clamp_i64(wall_s, PG_UNIX_MIN, PG_UNIX_MAX);
    if (!pg_calendar_local(wall_s, state->anchor.tz_offset_minutes, &date)) {
        return 0u;
    }
    return date.hour;
}

/* ---- what the player is told -------------------------------------------- */

typedef struct pg_sim_ctx {
    pg_state *state;
    pg_catchup_report *report;
    uint32_t care_day;
} pg_sim_ctx;

static void pg_sim_emit(pg_sim_ctx *ctx, uint8_t kind, uint8_t detail,
                        uint8_t plant_index)
{
    pg_journal_push(ctx->state, ctx->care_day, kind, detail, plant_index);
    if (ctx->report != NULL
        && ctx->report->entry_count < (uint16_t)PG_JOURNAL_REPORT_MAX) {
        pg_journal_entry *entry =
            &ctx->report->entries[ctx->report->entry_count];
        entry->care_day = ctx->care_day;
        entry->kind = kind;
        entry->detail = detail;
        entry->plant_index = plant_index;
        ctx->report->entry_count = (uint16_t)(ctx->report->entry_count + 1u);
    }
}

/* The handful of readings an away report is written from. Compared before and
 * after a step; a change is an entry. */
typedef struct pg_watch {
    uint8_t health;
    uint8_t life_state;
    uint8_t growth_stage;
    uint8_t leaf_count;
    uint8_t spathe_state;
    bool collapsed;
    bool thirsty;
    bool soggy;
} pg_watch;

static pg_watch pg_sim_watch(const pg_plant *plant)
{
    const pg_species *species = pg_content_species(plant->species_id);
    pg_watch watch;

    watch.health = pg_plant_health(plant);
    watch.life_state = plant->life_state;
    watch.growth_stage = plant->growth_stage;
    watch.leaf_count = plant->leaf_count;
    watch.spathe_state = plant->spathe_state;
    watch.collapsed = pg_plant_is_collapsed(plant);
    watch.thirsty = plant->turgor < 5000u;
    watch.soggy = species != NULL
               && plant->soggy_ticks > species->saturation_tolerance_ticks;
    return watch;
}

static void pg_sim_report_changes(pg_sim_ctx *ctx, uint8_t plant_index,
                                  const pg_watch *before, const pg_watch *after,
                                  uint8_t detail)
{
    if (!before->thirsty && after->thirsty) {
        pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_THIRSTY, detail, plant_index);
    }
    if (!before->collapsed && after->collapsed) {
        pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_COLLAPSED, detail, plant_index);
    }
    if (before->collapsed && !after->collapsed) {
        pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_PERKED_UP, detail, plant_index);
    }
    if (!before->soggy && after->soggy) {
        pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_SOIL_STAYED_WET, detail,
                    plant_index);
    }
    if (after->leaf_count > before->leaf_count) {
        pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_NEW_LEAF, after->leaf_count,
                    plant_index);
    }
    if (after->growth_stage > before->growth_stage) {
        pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_GREW, after->growth_stage,
                    plant_index);
    }
    if (after->spathe_state != before->spathe_state
        && after->spathe_state != (uint8_t)PG_SPATHE_NONE) {
        pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_SPATHE, after->spathe_state,
                    plant_index);
    }
    if (after->life_state != before->life_state) {
        switch ((pg_life_state)after->life_state) {
        case PG_LIFE_DORMANT:
            pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_WENT_DORMANT, detail,
                        plant_index);
            break;
        case PG_LIFE_ACTIVE:
            if (before->life_state == (uint8_t)PG_LIFE_DORMANT) {
                pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_WOKE_UP, detail,
                            plant_index);
            }
            break;
        case PG_LIFE_AILING:
            pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_AILING, detail, plant_index);
            break;
        case PG_LIFE_TERMINAL:
            pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_TERMINAL, detail, plant_index);
            break;
        case PG_LIFE_DEAD:
            pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_DIED, detail, plant_index);
            break;
        case PG_LIFE_ESTABLISHING:
        case PG_LIFE_STATE_COUNT:
        default:
            break;
        }
    }
}

/* ---- the day step ------------------------------------------------------- */

/* Damage bits are set and never cleared, and this is the only place the
 * simulation sets them: each accumulator, once it is past the rung the player
 * can see, marks the leaves that were out at the time (D-030). */
static void pg_sim_mark_damage(pg_plant *plant)
{
    if (plant->crisp_dose > 3000u) {
        pg_plant_mark_damage(plant, (uint8_t)PG_DAMAGE_TIP_CRISP);
    }
    if (plant->scorch_dose > 3000u) {
        pg_plant_mark_damage(plant, (uint8_t)PG_DAMAGE_BLEACH);
    }
    if (plant->salt > 7000u) {
        pg_plant_mark_damage(plant, (uint8_t)PG_DAMAGE_MARGIN_BROWN);
    }
    if (plant->pest_mites > 5000u) {
        pg_plant_mark_damage(plant, (uint8_t)PG_DAMAGE_STIPPLE);
    }
    if (plant->root_health < 2000u && plant->moisture_bottom > 6000u) {
        pg_plant_mark_damage(plant, (uint8_t)PG_DAMAGE_ROT_BLOTCH);
    }
    if (plant->nutrition < 1200u) {
        pg_plant_mark_damage(plant, (uint8_t)PG_DAMAGE_YELLOW);
    }
}

/* Life state is latched, not derived, because the transitions are the story:
 * a plant that has been terminal is a plant that was warned. Death is the one
 * transition with no way back, and it needs both an empty root system and a
 * turgor that has already lagged all the way down — which is weeks after the
 * mercy offer appears at rung 5 of the rot ladder (D-031, D-022). */
static void pg_sim_life_state(pg_plant *plant, const pg_care_env *env)
{
    uint8_t rot;
    uint8_t drought;

    if (plant->life_state == (uint8_t)PG_LIFE_DEAD) {
        return;
    }
    if (plant->root_health == 0u && plant->turgor < 500u) {
        plant->life_state = (uint8_t)PG_LIFE_DEAD;
        return;
    }
    if (pg_plant_mercy_offered(plant) || plant->root_health < 1000u
        || plant->turgor < 800u) {
        plant->life_state = (uint8_t)PG_LIFE_TERMINAL;
        return;
    }
    rot = pg_plant_ladder_rung(plant, PG_LADDER_OVERWATER);
    drought = pg_plant_ladder_rung(plant, PG_LADDER_UNDERWATER);
    if (rot >= 3u || drought >= 4u || plant->root_health < 3000u
        || plant->light_debt > 8500u) {
        plant->life_state = (uint8_t)PG_LIFE_AILING;
        return;
    }
    if (plant->shock >= 1000u) {
        plant->life_state = (uint8_t)PG_LIFE_ESTABLISHING;
        return;
    }
    plant->life_state = env->is_growing_season ? (uint8_t)PG_LIFE_ACTIVE
                                               : (uint8_t)PG_LIFE_DORMANT;
}

/* Growth spent on visible things. Every threshold is read off growth_points
 * rather than counted in a field of its own, so the day step stays a pure
 * function of the record and a replay cannot drift. */
static void pg_sim_grow_structure(pg_plant *plant, uint32_t care_day)
{
    const pg_species *species = pg_content_species(plant->species_id);
    uint32_t wanted;

    if (species == NULL) {
        return;
    }

    wanted = 2u + plant->growth_points / PG_POINTS_PER_LEAF;
    if (wanted > (uint32_t)PG_LEAF_MAX) {
        wanted = (uint32_t)PG_LEAF_MAX;
    }
    if ((uint32_t)plant->leaf_count < wanted) {
        /* One a day at most: a plant does not unfurl a flush overnight. The
         * leaf's form freezes here, from the conditions as they are now. */
        (void)pg_plant_add_leaf(plant, (uint16_t)(care_day & 0xFFFFu));
    }

    if ((species->flags & (uint8_t)PG_FLAG_TRAILING) != 0u) {
        wanted = plant->growth_points / PG_POINTS_PER_VINE_NODE;
        if (wanted > (uint32_t)PG_VINE_MAX) {
            wanted = (uint32_t)PG_VINE_MAX;
        }
        if ((uint32_t)plant->vine_count < wanted) {
            pg_plant_vine_extend(plant, plant->light_debt);
        }
    }

    /* The peace lily's whole achievement, and it is never promised: it steps
     * once a week, only while the light and the maturity are actually there,
     * and it fades back to nothing afterwards with one more flower on the
     * count. */
    if ((care_day % PG_SPATHE_STEP_DAYS) == 0u) {
        switch ((pg_spathe_state)plant->spathe_state) {
        case PG_SPATHE_NONE:
            if (pg_plant_spathe_possible(plant)) {
                plant->spathe_state = (uint8_t)PG_SPATHE_BUDDING;
            }
            break;
        case PG_SPATHE_BUDDING:
            plant->spathe_state = pg_plant_spathe_possible(plant)
                                ? (uint8_t)PG_SPATHE_OPEN
                                : (uint8_t)PG_SPATHE_NONE;
            break;
        case PG_SPATHE_OPEN:
            plant->spathe_state = (uint8_t)PG_SPATHE_FADING;
            break;
        case PG_SPATHE_FADING:
        default:
            plant->spathe_state = (uint8_t)PG_SPATHE_NONE;
            if (plant->flower_count < UINT8_MAX) {
                plant->flower_count = (uint8_t)(plant->flower_count + 1u);
            }
            break;
        }
    }
}

/* ---- pending timed actions ---------------------------------------------- */

static void pg_sim_pending_notice(pg_sim_ctx *ctx, pg_plant *plant,
                                  uint8_t plant_index, uint64_t care_before,
                                  uint64_t care_after, uint8_t detail)
{
    if (plant->pending_action == (uint8_t)PG_PENDING_NONE) {
        return;
    }
    if (care_before >= plant->pending_action_care_s
        || care_after < plant->pending_action_care_s) {
        return;                      /* the edge is elsewhere */
    }
    /* The deadline tells the player; it does not lift the pot out of the
     * water. Leaving it in is exactly how a soak becomes a bog, and it costs
     * nothing here to model because the ordinary saturation path does it. */
    pg_sim_emit(ctx,
                plant->pending_action == (uint8_t)PG_PENDING_BOTTOM_SOAK
                    ? (uint8_t)PG_JOURNAL_SOAK_FINISHED
                    : (uint8_t)PG_JOURNAL_FLUSH_FINISHED,
                detail, plant_index);
}

/* Abandonment does not rescue a running soak (D-089): the pending action is
 * force-completed to its worst outcome first, and only then does the tolerance
 * table run. */
static void pg_sim_force_complete_pending(pg_plant *plant)
{
    const pg_species *species = pg_content_species(plant->species_id);
    uint32_t tolerance = (species != NULL)
                       ? (uint32_t)species->saturation_tolerance_ticks : 1u;

    if (plant->pending_action == (uint8_t)PG_PENDING_NONE) {
        return;
    }
    if (tolerance == 0u) {
        tolerance = 1u;
    }
    plant->moisture_top = (uint16_t)PG_LEVEL_MAX;
    plant->moisture_bottom = (uint16_t)PG_LEVEL_MAX;
    if (plant->soggy_ticks < tolerance * 4u) {
        plant->soggy_ticks = tolerance * 4u;
    }
    if (plant->pending_action == (uint8_t)PG_PENDING_FLUSH) {
        /* Three or four pot volumes with nobody to stop it: the salt goes, and
         * so does everything else that was soluble. */
        plant->salt = 0u;
        plant->purity_load = 0u;
        plant->nutrition = (uint16_t)(plant->nutrition / 4u);
    }
    plant->root_health = (plant->root_health > 3000u)
                       ? (uint16_t)(plant->root_health - 3000u) : 0u;
    if (plant->root_capacity > 500u) {
        plant->root_capacity = (uint16_t)(plant->root_capacity - 500u);
    }
    if (plant->root_health > plant->root_capacity) {
        plant->root_health = plant->root_capacity;
    }
    plant->pending_action = (uint8_t)PG_PENDING_NONE;
    plant->pending_action_care_s = 0u;
}

/* ---- abandonment: resolved once, never simulated ------------------------ */

/* GAME_DESIGN.md §10.1 and D-043, which is the same sentence twice: Sarge is
 * fine and faintly smug, Goldie is a crisped survivor, Ophelia is collapsed
 * and permanently scarred but alive if the pot held any water, Nyx is dead.
 * One sentence that teaches the whole difficulty ordering. */
static void pg_sim_resolve_abandonment(pg_sim_ctx *ctx, uint8_t plant_index)
{
    pg_plant *plant = &ctx->state->plants[plant_index];
    const pg_pot *pot = pg_content_pot(plant->pot_id);
    bool held_water;
    bool was_soaking;

    if (plant->life_state == (uint8_t)PG_LIFE_DEAD) {
        return;
    }
    was_soaking = plant->pending_action != (uint8_t)PG_PENDING_NONE;
    pg_sim_force_complete_pending(plant);

    /* "If the pot held any water" means exactly one thing after a month with
     * nobody in the room: a pot with no hole, whose perched water table sits
     * above saturation and never drains — or a soak that was still running.
     * Every pot that drains is bone dry, whatever its volume. */
    held_water = was_soaking
              || (pot != NULL
                  && (!pot->has_drainage
                      || pot->bottom_floor_l >= pg_content_saturation_level()));

    /* A month with nobody in the room: the soil is gone, whatever the species
     * does about it. The cachepot and a bog are the two exceptions, and they
     * are exceptions in the plant's favour and against its roots at once. */
    if (!held_water) {
        plant->moisture_top = 0u;
        plant->moisture_bottom = (pot != NULL) ? pot->bottom_floor_l : 0u;
    }
    plant->pest_gnats = 0u;

    switch ((pg_species_id)plant->species_id) {
    case PG_SPECIES_SNAKE:
        /* A CAM succulent barely noticed. */
        plant->turgor = 8500u;
        plant->life_state = (uint8_t)PG_LIFE_ACTIVE;
        break;
    case PG_SPECIES_POTHOS:
        /* A crisped survivor with three living leaves. */
        if (plant->leaf_count > 3u) {
            plant->leaf_count = 3u;
        }
        if (plant->crisp_dose < 7000u) {
            plant->crisp_dose = 7000u;
        }
        pg_plant_mark_damage(plant, (uint8_t)PG_DAMAGE_TIP_CRISP);
        pg_plant_mark_damage(plant, (uint8_t)PG_DAMAGE_MARGIN_BROWN);
        plant->turgor = 2500u;
        plant->life_state = (uint8_t)PG_LIFE_AILING;
        break;
    case PG_SPECIES_PEACE_LILY:
        if (held_water) {
            /* Collapsed, permanently scarred, alive. */
            if (plant->crisp_dose < 6000u) {
                plant->crisp_dose = 6000u;
            }
            pg_plant_mark_damage(plant, (uint8_t)PG_DAMAGE_MARGIN_BROWN);
            plant->turgor = 1200u;
            plant->root_health = (uint16_t)(plant->root_health / 2u);
            plant->life_state = (uint8_t)PG_LIFE_TERMINAL;
        } else {
            plant->turgor = 0u;
            plant->life_state = (uint8_t)PG_LIFE_DEAD;
        }
        break;
    case PG_SPECIES_CALATHEA:
    default:
        plant->turgor = 0u;
        pg_plant_mark_damage(plant, (uint8_t)PG_DAMAGE_TIP_CRISP);
        pg_plant_mark_damage(plant, (uint8_t)PG_DAMAGE_MARGIN_BROWN);
        plant->life_state = (uint8_t)PG_LIFE_DEAD;
        break;
    }

    if (plant->life_state == (uint8_t)PG_LIFE_DEAD) {
        pg_plant_mark_damage(plant, (uint8_t)PG_DAMAGE_YELLOW);
        /* No fail screen. The pot stays, the calendar stays, and the replant
         * flow opens (GAME_DESIGN.md §10.2). */
        ctx->state->replant_offered = true;
        pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_DIED, 0u, plant_index);
    }
    pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_LEFT_ALONE,
                pg_plant_health(plant), plant_index);
}

/* ---- the advance -------------------------------------------------------- */

static void pg_sim_day_step(pg_sim_ctx *ctx, uint8_t plant_index,
                            const pg_care_env *env, uint8_t detail, bool fine)
{
    pg_plant *plant = &ctx->state->plants[plant_index];
    pg_watch before = pg_sim_watch(plant);
    pg_watch after;
    uint16_t rain_l;

    pg_care_day(plant, env);
    if (plant->life_state != (uint8_t)PG_LIFE_DEAD) {
        pg_sim_grow_structure(plant, ctx->care_day);
        pg_sim_mark_damage(plant);
    }
    pg_sim_life_state(plant, env);
    if (plant->life_state == (uint8_t)PG_LIFE_DEAD) {
        ctx->state->replant_offered = true;
    }

    after = pg_sim_watch(plant);
    pg_sim_report_changes(ctx, plant_index, &before, &after, detail);

    /* The jug on the sill. Weather is a pure function of (ordinal, seed), so
     * the rain in a replayed week is the rain that fell in it. */
    rain_l = pg_rng_weather_l(ctx->state->weather_seed, env->ordinal,
                              (uint8_t)PG_WEATHER_RAIN);
    if (rain_l > 8000u && plant_index == 0u) {
        pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_RAIN_JUG_FILLED,
                    (uint8_t)(rain_l / 100u), plant_index);
    }

    /* Beyond the fine window the day is the unit of the report: one summary
     * line, carrying the health index the day ended on. */
    if (!fine) {
        pg_sim_emit(ctx, (uint8_t)PG_JOURNAL_DAY_SUMMARY, after.health,
                    plant_index);
    }
}

bool pg_advance(pg_state *state, pg_now now, pg_catchup_report *report)
{
    pg_state snapshot;
    pg_time_gap gap;
    pg_sim_ctx ctx;
    char error[96];
    int64_t cursor;
    uint64_t tick_ordinal;
    uint32_t ticks;
    uint32_t step;
    uint32_t days_done = 0u;
    uint32_t poison = pg_sim_poison_tick;

    if (report != NULL) {
        memset(report, 0, sizeof *report);
    }
    if (state == NULL) {
        return false;
    }
    pg_sim_poison_tick = 0u;          /* one-shot: consumed by this advance */

    ctx.state = state;
    ctx.report = report;
    ctx.care_day = 0u;

    pg_time_reconcile(&state->anchor, now, &gap);

    if (gap.first_run) {
        /* No anchor to measure against, so no gap and no anomaly. This is also
         * where the calendar gets its origin. */
        size_t index;
        state->first_run_wall_s = now.wall_s;
        for (index = 0u; index < (size_t)state->plant_count; ++index) {
            if (state->plants[index].planted_wall_s == 0) {
                state->plants[index].planted_wall_s = now.wall_s;
            }
        }
        pg_time_anchor_set(&state->anchor, now);
        return true;
    }

    /* The whole state, byte for byte, so a rollback is a restore and not a
     * repair. memcpy rather than assignment because the rollback assertion
     * compares the bytes, padding included. */
    memcpy(&snapshot, state, sizeof snapshot);

    if (report != NULL) {
        report->credited_seconds = gap.credited_s;
        report->abandoned = gap.abandoned;
        report->clock_backwards = gap.clock_backwards;
        report->clock_jumped = gap.clock_jumped;
    }
    if (gap.counts_as_anomaly && state->clock_anomaly_count < UINT16_MAX) {
        state->clock_anomaly_count =
            (uint16_t)(state->clock_anomaly_count + 1u);
    }

    /* The care clock is monotone and the credit is capped, so this is the only
     * line in the game that moves biology time forward. */
    tick_ordinal = state->clock.care_seconds_total
                 / (uint64_t)PG_CARE_TICK_SECONDS;
    cursor = pg_time_saturating_sub(state->anchor.last_wall_s,
                                    (int64_t)state->clock.care_residual_s);
    ticks = pg_time_credit(&state->clock, gap.credited_s);
    ctx.care_day = pg_sim_care_day(state, cursor);

    if (gap.abandoned) {
        /* Do not simulate. Resolve once, deterministically, from species
         * tolerance — after force-completing any pending timed action to its
         * worst outcome (D-089). The entry is dated the day you came back,
         * which is the only day anybody can honestly put on it. */
        uint8_t index;
        ctx.care_day = pg_sim_care_day(state, now.wall_s);
        for (index = 0u; index < state->plant_count; ++index) {
            pg_sim_resolve_abandonment(&ctx, index);
        }
        ticks = 0u;
    }

    for (step = 0u; step < ticks; ++step) {
        bool fine = step < PG_SIM_FINE_TICKS;
        int64_t day_before = pg_sim_local_day(cursor,
                                              state->anchor.tz_offset_minutes);
        int64_t day_after;
        uint64_t care_before = tick_ordinal * (uint64_t)PG_CARE_TICK_SECONDS;
        uint64_t care_after = care_before + (uint64_t)PG_CARE_TICK_SECONDS;
        uint8_t detail;
        uint8_t index;

        cursor = pg_time_saturating_add(cursor, PG_CARE_TICK_SECONDS);
        day_after = pg_sim_local_day(cursor, state->anchor.tz_offset_minutes);
        ctx.care_day = pg_sim_care_day(state, cursor);
        detail = pg_sim_local_hour(state, cursor);

        for (index = 0u; index < state->plant_count; ++index) {
            pg_plant *plant = &state->plants[index];
            pg_care_env env = pg_sim_env_at(state, index, cursor);
            pg_watch before;
            pg_watch after;

            before = pg_sim_watch(plant);
            pg_care_tick(plant, &env, tick_ordinal);
            pg_sim_pending_notice(&ctx, plant, index, care_before, care_after,
                                  detail);
            if (fine) {
                after = pg_sim_watch(plant);
                pg_sim_report_changes(&ctx, index, &before, &after, detail);
            }
            if (day_after != day_before) {
                pg_sim_day_step(&ctx, index, &env, detail, fine);
            }
        }
        if (day_after != day_before) {
            days_done += 1u;
        }
        tick_ordinal += 1u;

        if (poison != 0u && step + 1u == poison) {
            /* Test seam: make the next validate fail on purpose. */
            state->plants[0].turgor = (uint16_t)(PG_LEVEL_MAX + 1);
        }
        if (!pg_validate(state, error, sizeof error)) {
            /* All or nothing: the entry state comes back exactly, and the
             * caller is told the gap was not credited. */
            memcpy(state, &snapshot, sizeof *state);
            if (report != NULL) {
                /* Nothing happened, and the report says nothing happened:
                 * ticks that were rolled back are not ticks the player lived
                 * through. */
                report->rolled_back = true;
                report->care_ticks = 0u;
                report->days_coarse = 0u;
                report->credited_seconds = 0;
                report->entry_count = 0u;
            }
            return false;
        }
    }

    if (report != NULL) {
        /* days_coarse counts the local midnights the gap crossed, which is the
         * second half of the cost bound ("2880 fine ticks plus 30 day steps").
         * A day boundary always runs its day step; what the fine window
         * decides is whether the day is reported tick by tick or as one line
         * (ARCHITECTURE.md §5.4). */
        report->care_ticks = ticks;
        report->days_coarse = days_done;
    }

    /* Anchoring is unconditional and last: it is what stops a backwards clock
     * re-firing on every launch, and what makes a second advance at the same
     * instant credit nothing (ARCHITECTURE.md §5.5, §5.7 rule 5). */
    if (gap.counts_as_anomaly) {
        ctx.care_day = pg_sim_care_day(state, now.wall_s);
        pg_sim_emit(&ctx, (uint8_t)PG_JOURNAL_CLOCK_UNSETTLED,
                    gap.clock_backwards ? 1u : 2u, 0u);
    }
    /* save_sequence belongs to the save path, not to this one: an advance that
     * credited nothing must leave the record it was handed untouched, which is
     * what makes idempotence testable by digest. */
    pg_time_anchor_set(&state->anchor, now);
    return true;
}

int64_t pg_next_wall_deadline(const pg_state *state)
{
    if (state == NULL) {
        return INT64_MAX;
    }
    return pg_time_next_tick_wall_s(&state->anchor, &state->clock);
}

/* ---- the digest --------------------------------------------------------- */

static uint64_t pg_fnv(uint64_t hash, const void *bytes, size_t size)
{
    const uint8_t *cursor = (const uint8_t *)bytes;
    size_t index;

    for (index = 0u; index < size; ++index) {
        hash ^= (uint64_t)cursor[index];
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}

uint64_t pg_state_digest(const pg_state *state)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    size_t index;

    if (state == NULL) {
        return hash;
    }
    /* Field-wise, and deliberately without the journal: the event ring is what
     * the player was TOLD, and the two tiers exist precisely to tell it at
     * different resolutions. The simulation is the plants and the clocks. */
    hash = pg_fnv(hash, &state->clock, sizeof state->clock);
    hash = pg_fnv(hash, &state->anchor.last_wall_s,
                  sizeof state->anchor.last_wall_s);
    hash = pg_fnv(hash, &state->anchor.last_boot_s,
                  sizeof state->anchor.last_boot_s);
    hash = pg_fnv(hash, &state->anchor.tz_offset_minutes,
                  sizeof state->anchor.tz_offset_minutes);
    hash = pg_fnv(hash, &state->first_run_wall_s,
                  sizeof state->first_run_wall_s);
    hash = pg_fnv(hash, &state->clock_anomaly_count,
                  sizeof state->clock_anomaly_count);
    hash = pg_fnv(hash, &state->rng.state, sizeof state->rng.state);
    hash = pg_fnv(hash, &state->weather_seed, sizeof state->weather_seed);
    hash = pg_fnv(hash, &state->hemisphere, sizeof state->hemisphere);
    hash = pg_fnv(hash, &state->plant_count, sizeof state->plant_count);
    hash = pg_fnv(hash, &state->replant_offered, sizeof state->replant_offered);
    for (index = 0u; index < (size_t)state->plant_count; ++index) {
        const pg_plant *plant = &state->plants[index];
        size_t leaf;

        hash = pg_fnv(hash, plant->name, sizeof plant->name);
        hash = pg_fnv(hash, &plant->species_id, sizeof plant->species_id);
        hash = pg_fnv(hash, &plant->pot_id, sizeof plant->pot_id);
        hash = pg_fnv(hash, &plant->spot_id, sizeof plant->spot_id);
        hash = pg_fnv(hash, &plant->moisture_top, sizeof plant->moisture_top);
        hash = pg_fnv(hash, &plant->moisture_bottom,
                      sizeof plant->moisture_bottom);
        hash = pg_fnv(hash, &plant->light_debt, sizeof plant->light_debt);
        hash = pg_fnv(hash, &plant->nutrition, sizeof plant->nutrition);
        hash = pg_fnv(hash, &plant->salt, sizeof plant->salt);
        hash = pg_fnv(hash, &plant->purity_load, sizeof plant->purity_load);
        hash = pg_fnv(hash, &plant->root_health, sizeof plant->root_health);
        hash = pg_fnv(hash, &plant->root_capacity, sizeof plant->root_capacity);
        hash = pg_fnv(hash, &plant->root_bound, sizeof plant->root_bound);
        hash = pg_fnv(hash, &plant->turgor, sizeof plant->turgor);
        hash = pg_fnv(hash, &plant->soggy_ticks, sizeof plant->soggy_ticks);
        hash = pg_fnv(hash, &plant->scorch_dose, sizeof plant->scorch_dose);
        hash = pg_fnv(hash, &plant->crisp_dose, sizeof plant->crisp_dose);
        hash = pg_fnv(hash, &plant->acclimation, sizeof plant->acclimation);
        hash = pg_fnv(hash, &plant->shock, sizeof plant->shock);
        hash = pg_fnv(hash, &plant->dli_today, sizeof plant->dli_today);
        hash = pg_fnv(hash, plant->dli_ring, sizeof plant->dli_ring);
        hash = pg_fnv(hash, &plant->dli_ring_head, sizeof plant->dli_ring_head);
        hash = pg_fnv(hash, &plant->pest_gnats, sizeof plant->pest_gnats);
        hash = pg_fnv(hash, &plant->pest_mites, sizeof plant->pest_mites);
        hash = pg_fnv(hash, &plant->pest_mealy, sizeof plant->pest_mealy);
        hash = pg_fnv(hash, &plant->pest_egg_timer,
                      sizeof plant->pest_egg_timer);
        hash = pg_fnv(hash, &plant->growth_stage, sizeof plant->growth_stage);
        hash = pg_fnv(hash, &plant->growth_points, sizeof plant->growth_points);
        hash = pg_fnv(hash, &plant->life_state, sizeof plant->life_state);
        hash = pg_fnv(hash, &plant->spathe_state, sizeof plant->spathe_state);
        hash = pg_fnv(hash, &plant->flower_count, sizeof plant->flower_count);
        hash = pg_fnv(hash, &plant->leaf_count, sizeof plant->leaf_count);
        for (leaf = 0u; leaf < (size_t)plant->leaf_count; ++leaf) {
            hash = pg_fnv(hash, &plant->leaves[leaf].birth_care_day,
                          sizeof plant->leaves[leaf].birth_care_day);
            hash = pg_fnv(hash, &plant->leaves[leaf].slot,
                          sizeof plant->leaves[leaf].slot);
            hash = pg_fnv(hash, &plant->leaves[leaf].form_flags,
                          sizeof plant->leaves[leaf].form_flags);
            hash = pg_fnv(hash, &plant->leaves[leaf].damage_mask,
                          sizeof plant->leaves[leaf].damage_mask);
            hash = pg_fnv(hash, &plant->leaves[leaf].pose,
                          sizeof plant->leaves[leaf].pose);
        }
        hash = pg_fnv(hash, &plant->vine_count, sizeof plant->vine_count);
        for (leaf = 0u; leaf < (size_t)plant->vine_count; ++leaf) {
            hash = pg_fnv(hash, &plant->vine[leaf], sizeof plant->vine[leaf]);
        }
        hash = pg_fnv(hash, &plant->last_watered_care_s,
                      sizeof plant->last_watered_care_s);
        hash = pg_fnv(hash, &plant->last_fed_care_s,
                      sizeof plant->last_fed_care_s);
        hash = pg_fnv(hash, &plant->last_repotted_care_s,
                      sizeof plant->last_repotted_care_s);
        hash = pg_fnv(hash, &plant->pending_action,
                      sizeof plant->pending_action);
        hash = pg_fnv(hash, &plant->pending_action_care_s,
                      sizeof plant->pending_action_care_s);
        hash = pg_fnv(hash, &plant->streak_on_time,
                      sizeof plant->streak_on_time);
        hash = pg_fnv(hash, &plant->missed_events, sizeof plant->missed_events);
    }
    return hash;
}

/* ------------------------------------------------------------------------- *
 * --selftest <seed> <sim-days>: the seven assertions of ARCHITECTURE.md §10.1.
 *
 * Output is a pure function of the two arguments. No clock is read, no pointer
 * is printed, nothing is measured in wall time, which is what makes two runs
 * byte-identical and the DONE condition's `cmp` meaningful.
 * ------------------------------------------------------------------------- */

#define PG_SELFTEST_T0 INT64_C(1767225600)   /* 2026-01-01T00:00:00Z */
#define PG_SELFTEST_B0 INT64_C(100000)
#define PG_SELFTEST_DAY INT64_C(86400)

static int pg_selftest_failed;

static void pg_selftest_check(bool condition, const char *what)
{
    if (!condition) {
        pg_selftest_failed += 1;
        (void)printf("  FAIL  %s\n", what);
    }
}

static pg_now pg_selftest_now(int64_t wall_s, int64_t boot_s, uint8_t boot_tag,
                              int32_t tz_minutes)
{
    pg_now now;
    size_t index;

    memset(&now, 0, sizeof now);
    now.wall_s = wall_s;
    now.boot_s = boot_s;
    now.tz_offset_minutes = tz_minutes;
    for (index = 0u; index < (size_t)PG_BOOT_ID_BYTES; ++index) {
        now.boot_id[index] = (uint8_t)(boot_tag + (uint8_t)index);
    }
    return now;
}

/* A save that has already been anchored once, so every case below measures a
 * real gap rather than a first run. */
static void pg_selftest_state(pg_state *state, uint64_t seed,
                              uint8_t species_id, uint8_t pot_id,
                              uint8_t spot_id)
{
    pg_catchup_report report;

    pg_init(state, seed);
    pg_plant_init(&state->plants[0], species_id, pot_id, spot_id,
                  PG_SELFTEST_T0);
    (void)pg_advance(state, pg_selftest_now(PG_SELFTEST_T0, PG_SELFTEST_B0, 1u,
                                            0),
                     &report);
}

/* Assertion 2: the same elapsed time, arrived at three different ways.
 *
 * The plant record, the care clock and the anchors must come out bit-identical
 * whether the game was open the whole time, closed the whole time, or opened
 * at ragged intervals. The journal ring is deliberately NOT compared: its
 * resolution is the tier's whole purpose, and a report that said the same
 * thing about a fortnight as about an afternoon would mean the fine tier had
 * no reason to exist. */
static void pg_selftest_equivalence(uint64_t seed, uint32_t days)
{
    uint8_t species;

    if (days == 0u) {
        days = 1u;
    }
    if (days > PG_SIM_MAX_DAYS) {
        days = PG_SIM_MAX_DAYS;
    }

    for (species = 0u; species < (uint8_t)PG_SPECIES_COUNT; ++species) {
        pg_state live;
        pg_state saved;
        pg_state ragged;
        pg_catchup_report report;
        uint32_t tick;
        uint32_t total_ticks = days * 96u;
        int64_t elapsed = 0;
        bool ok;

        pg_selftest_state(&live, seed, species, (uint8_t)(species % 4u),
                          (uint8_t)(species % (uint8_t)PG_SPOT_COUNT));
        memcpy(&saved, &live, sizeof saved);
        memcpy(&ragged, &live, sizeof ragged);

        /* Open the whole time: one advance per care tick. */
        for (tick = 1u; tick <= total_ticks; ++tick) {
            int64_t at = (int64_t)tick * PG_CARE_TICK_SECONDS;
            (void)pg_advance(&live,
                             pg_selftest_now(PG_SELFTEST_T0 + at,
                                             PG_SELFTEST_B0 + at, 1u, 0),
                             &report);
        }
        /* Closed the whole time: one advance for the lot. */
        (void)pg_advance(&saved,
                         pg_selftest_now(PG_SELFTEST_T0
                                         + (int64_t)total_ticks
                                           * PG_CARE_TICK_SECONDS,
                                         PG_SELFTEST_B0
                                         + (int64_t)total_ticks
                                           * PG_CARE_TICK_SECONDS,
                                         1u, 0),
                         &report);
        /* Ragged: chunks that are not tick multiples, so the residual has to
         * carry the remainder correctly. */
        {
            static const int64_t chunk[6] = { 137, 4001, 900, 59, 86399, 613 };
            size_t which = 0u;
            int64_t target = (int64_t)total_ticks * PG_CARE_TICK_SECONDS;
            while (elapsed < target) {
                int64_t step_s = chunk[which % 6u];
                which += 1u;
                if (elapsed + step_s > target) {
                    step_s = target - elapsed;
                }
                elapsed += step_s;
                (void)pg_advance(&ragged,
                                 pg_selftest_now(PG_SELFTEST_T0 + elapsed,
                                                 PG_SELFTEST_B0 + elapsed, 1u,
                                                 0),
                                 &report);
            }
        }

        ok = memcmp(live.plants, saved.plants, sizeof live.plants) == 0
          && memcmp(live.plants, ragged.plants, sizeof live.plants) == 0
          && live.clock.care_seconds_total == saved.clock.care_seconds_total
          && live.clock.care_seconds_total == ragged.clock.care_seconds_total
          && pg_state_digest(&live) == pg_state_digest(&saved)
          && pg_state_digest(&live) == pg_state_digest(&ragged);
        (void)printf("  equivalence %-12s ticks=%-6" PRIu32
                     " digest=%016" PRIx64 " %s\n",
                     pg_content_species(species)->id, total_ticks,
                     pg_state_digest(&saved), ok ? "ok" : "MISMATCH");
        pg_selftest_check(ok, "live ticks == one saved gap == ragged chunks");
    }
}

/* The payoff screen, printed because it is the whole reason the fine tier
 * exists: a week away, told back at 900 s resolution for the first 72 h and
 * one line a day after that. Deterministic, like everything else here. */
static void pg_selftest_away_report(uint64_t seed)
{
    pg_state state;
    pg_catchup_report report;
    uint16_t index;

    pg_selftest_state(&state, seed, (uint8_t)PG_SPECIES_PEACE_LILY,
                      (uint8_t)PG_POT_TERRACOTTA, (uint8_t)PG_SPOT_SILL_SOUTH);
    (void)pg_advance(&state,
                     pg_selftest_now(PG_SELFTEST_T0 + 7 * PG_SELFTEST_DAY,
                                     PG_SELFTEST_B0 + 7 * PG_SELFTEST_DAY, 1u,
                                     0), &report);
    (void)printf("  while you were away: %" PRIu32 " ticks, %" PRIu32
                 " days, %" PRIu16 " entries\n", report.care_ticks,
                 report.days_coarse, report.entry_count);
    for (index = 0u; index < report.entry_count && index < 12u; ++index) {
        const pg_journal_entry *entry = &report.entries[index];
        if (pg_journal_detail_is_hour(entry->kind)) {
            (void)printf("    day %-3" PRIu32 " %02u:00  %s\n", entry->care_day,
                         (unsigned)entry->detail,
                         pg_journal_kind_line(entry->kind));
        } else {
            (void)printf("    day %-3" PRIu32 "         %s (%u)\n",
                         entry->care_day, pg_journal_kind_line(entry->kind),
                         (unsigned)entry->detail);
        }
    }
    pg_selftest_check(report.entry_count > 0u,
                      "a week away produced something to say");
}

/* Assertions 3 and 4: a long soak of randomly ordered gaps, including negative
 * ones and gaps past the abandonment cap, with the invariants checked after
 * every single advance. */
static void pg_selftest_soak(uint64_t seed, uint32_t days, uint8_t species_id)
{
    pg_state state;
    pg_catchup_report report;
    uint64_t previous_total = 0u;
    uint32_t step;
    uint32_t deaths = 0u;
    uint32_t replants = 0u;
    uint32_t abandonments = 0u;
    int64_t wall = PG_SELFTEST_T0;
    int64_t boot = PG_SELFTEST_B0;
    uint8_t previous_stage;
    bool monotone = true;
    bool valid = true;
    bool stage_ok = true;
    char error[96];

    pg_selftest_state(&state, seed, species_id, (uint8_t)PG_POT_TERRACOTTA,
                      (uint8_t)PG_SPOT_ONE_METRE);
    previous_stage = state.plants[0].growth_stage;

    for (step = 0u; step < days; ++step) {
        /* A private stream, seeded from the arguments: the gaps must be the
         * same on every machine and every run, and they must not disturb the
         * save's own stream, which is part of what is being tested. */
        pg_rng dice;
        uint32_t roll;
        int64_t gap;
        bool died_here = false;

        pg_rng_seed(&dice, seed ^ ((uint64_t)step << 16));
        roll = pg_rng_next_u32(&dice);
        if ((step % 89u) == 88u) {
            /* Rare, because an abandonment credits thirty days that are never
             * simulated: too many of them and the soak stops being a soak. */
            gap = (31 + (int64_t)(roll % 14u)) * PG_SELFTEST_DAY;  /* abandoned */
        } else if ((step % 11u) == 5u) {
            gap = -(int64_t)(roll % 7200u);                         /* backwards */
        } else {
            gap = (int64_t)(roll % 172800u);                        /* 0..48 h */
        }
        wall += gap;
        boot += (gap > 0) ? gap : 0;

        (void)pg_advance(&state, pg_selftest_now(wall, boot, 1u, 0), &report);
        if (report.abandoned) {
            abandonments += 1u;
        }
        if (state.clock.care_seconds_total < previous_total) {
            monotone = false;
        }
        previous_total = state.clock.care_seconds_total;
        if (!pg_validate(&state, error, sizeof error)) {
            valid = false;
        }
        if (state.plants[0].life_state == (uint8_t)PG_LIFE_DEAD) {
            died_here = true;
            deaths += 1u;
        }
        if (state.plants[0].growth_stage < previous_stage && !died_here) {
            stage_ok = false;
        }
        previous_stage = state.plants[0].growth_stage;

        /* The replant flow: the pot and the calendar stay, the plant is new.
         * This is the one transition growth_stage is allowed to fall through
         * (assertion 4). */
        if (state.replant_offered
            && state.plants[0].life_state == (uint8_t)PG_LIFE_DEAD) {
            pg_plant_init(&state.plants[0], species_id, state.plants[0].pot_id,
                          state.plants[0].spot_id, wall);
            state.replant_offered = false;
            replants += 1u;
            previous_stage = state.plants[0].growth_stage;
        } else {
            /* Somebody does live here, and they are competent: they water when
             * the soil says to and feed a fortnight apart. That keeps the soak
             * from being a study of four dead plants, gives the growth-stage
             * assertion something to bite on, and exercises pg_actions on the
             * way through. */
            const pg_species *species =
                pg_content_species(state.plants[0].species_id);
            pg_action_result result;

            if (species != NULL
                && pg_care_moisture_whole_l(&state.plants[0])
                   <= species->thirst_threshold_l) {
                (void)pg_actions_apply(&state, 0u, PG_VERB_WATER_THOROUGHLY, 0u,
                                       &result);
            }
            if ((step % 14u) == 13u) {
                (void)pg_actions_apply(&state, 0u, PG_VERB_FEED_HALF, 0u,
                                       &result);
            }
        }
    }

    (void)printf("  soak %-12s steps=%-5" PRIu32 " care_s=%-12" PRIu64
                 " aband=%-4" PRIu32 " died=%-4" PRIu32 " replanted=%-4" PRIu32
                 " points=%-7" PRIu32 " stage=%u health=%u leaves=%u"
                 " digest=%016" PRIx64 "\n",
                 pg_content_species(species_id)->id, days,
                 state.clock.care_seconds_total, abandonments, deaths,
                 replants, state.plants[0].growth_points,
                 (unsigned)state.plants[0].growth_stage,
                 (unsigned)pg_plant_health(&state.plants[0]),
                 (unsigned)state.plants[0].leaf_count,
                 pg_state_digest(&state));
    pg_selftest_check(monotone, "care_seconds_total never decreased");
    pg_selftest_check(valid, "pg_validate held after every advance");
    pg_selftest_check(stage_ok,
                      "growth_stage fell only through death or replanting");
}

/* Assertion 5: no gap, however large, costs more than 2880 fine ticks plus 30
 * day steps. */
static void pg_selftest_bounds(uint64_t seed)
{
    static const int64_t gaps[5] = { 1, 3 * PG_SELFTEST_DAY,
                                     30 * PG_SELFTEST_DAY,
                                     400 * PG_SELFTEST_DAY,
                                     4000 * PG_SELFTEST_DAY };
    size_t index;

    for (index = 0u; index < 5u; ++index) {
        pg_state state;
        pg_catchup_report report;
        bool ok;

        pg_selftest_state(&state, seed, (uint8_t)PG_SPECIES_POTHOS,
                          (uint8_t)PG_POT_TERRACOTTA,
                          (uint8_t)PG_SPOT_ONE_METRE);
        (void)pg_advance(&state,
                         pg_selftest_now(PG_SELFTEST_T0 + gaps[index],
                                         PG_SELFTEST_B0 + gaps[index], 1u, 0),
                         &report);
        ok = report.care_ticks <= PG_SIM_MAX_TICKS
          && report.days_coarse <= PG_SIM_MAX_DAYS
          && (!report.abandoned || report.care_ticks == 0u);
        (void)printf("  bound gap=%-10" PRId64 " ticks=%-5" PRIu32
                     " days=%-3" PRIu32 " credited=%-9" PRId64 " aband=%d %s\n",
                     gaps[index], report.care_ticks, report.days_coarse,
                     report.credited_seconds, report.abandoned ? 1 : 0,
                     ok ? "ok" : "MISMATCH");
        pg_selftest_check(ok, "gap cost stayed inside 2880 ticks + 30 days");
    }
}

/* Assertion 6: a poisoned intermediate leaves the entry state exactly
 * restored, byte for byte, and says so. */
static void pg_selftest_rollback(uint64_t seed)
{
    pg_state state;
    pg_state entry;
    pg_catchup_report report;
    bool advanced;
    bool ok;

    pg_selftest_state(&state, seed, (uint8_t)PG_SPECIES_CALATHEA,
                      (uint8_t)PG_POT_GLAZED, (uint8_t)PG_SPOT_BATH_SHELF);
    memcpy(&entry, &state, sizeof entry);

    pg_sim_poison_after(17u);
    advanced = pg_advance(&state, pg_selftest_now(PG_SELFTEST_T0
                                                  + PG_SELFTEST_DAY,
                                                  PG_SELFTEST_B0
                                                  + PG_SELFTEST_DAY, 1u, 0),
                          &report);
    ok = !advanced && report.rolled_back
      && memcmp(&state, &entry, sizeof state) == 0;
    (void)printf("  rollback poisoned=17 advanced=%d rolled_back=%d "
                 "restored=%d %s\n", advanced ? 1 : 0,
                 report.rolled_back ? 1 : 0,
                 memcmp(&state, &entry, sizeof state) == 0 ? 1 : 0,
                 ok ? "ok" : "MISMATCH");
    pg_selftest_check(ok, "a poisoned advance restored the entry state");

    /* And the very next advance, unpoisoned, works normally: a rollback is not
     * a wedged save. */
    advanced = pg_advance(&state, pg_selftest_now(PG_SELFTEST_T0
                                                  + PG_SELFTEST_DAY,
                                                  PG_SELFTEST_B0
                                                  + PG_SELFTEST_DAY, 1u, 0),
                          &report);
    pg_selftest_check(advanced && !report.rolled_back,
                      "the advance after a rollback succeeded");
}

/* Assertion 7: the clock matrix, including the one D-089 case — a gap past the
 * cap with a soak running force-completes the soak to a bog before the
 * tolerance table resolves anything. */
static void pg_selftest_clocks(uint64_t seed)
{
    pg_state state;
    pg_catchup_report report;
    bool ok;

    /* A wall clock that fell backwards across a reboot has nothing to
     * cross-check against, so it credits nothing at all. */
    pg_selftest_state(&state, seed, (uint8_t)PG_SPECIES_POTHOS,
                      (uint8_t)PG_POT_TERRACOTTA, (uint8_t)PG_SPOT_ONE_METRE);
    (void)pg_advance(&state,
                     pg_selftest_now(PG_SELFTEST_T0 - 3 * PG_SELFTEST_DAY, 300,
                                     7u, 0), &report);
    ok = report.credited_seconds == 0 && report.clock_backwards
      && report.care_ticks == 0u;
    (void)printf("  clock backwards      credited=%-9" PRId64 " ticks=%-5"
                 PRIu32 " %s\n", report.credited_seconds, report.care_ticks,
                 ok ? "ok" : "MISMATCH");
    pg_selftest_check(ok, "a backwards clock credited nothing");

    /* Inside one boot the boot clock is authoritative, so the same backwards
     * wall clock costs the player nothing: it credits the boot gap, and the
     * re-anchor means it does not re-fire on the next launch either. */
    pg_selftest_state(&state, seed, (uint8_t)PG_SPECIES_POTHOS,
                      (uint8_t)PG_POT_TERRACOTTA, (uint8_t)PG_SPOT_ONE_METRE);
    (void)pg_advance(&state,
                     pg_selftest_now(PG_SELFTEST_T0 - 3 * PG_SELFTEST_DAY,
                                     PG_SELFTEST_B0 + 1800, 1u, 0), &report);
    ok = report.credited_seconds == 1800 && report.clock_backwards
      && report.care_ticks == 2u;
    (void)printf("  clock backwards/boot credited=%-9" PRId64 " ticks=%-5"
                 PRIu32 " %s\n", report.credited_seconds, report.care_ticks,
                 ok ? "ok" : "MISMATCH");
    pg_selftest_check(ok, "inside one boot the boot clock was authoritative");

    /* Same boot, wall ran ahead: the boot clock is authoritative. */
    pg_selftest_state(&state, seed, (uint8_t)PG_SPECIES_POTHOS,
                      (uint8_t)PG_POT_TERRACOTTA, (uint8_t)PG_SPOT_ONE_METRE);
    (void)pg_advance(&state,
                     pg_selftest_now(PG_SELFTEST_T0 + 10 * PG_SELFTEST_DAY,
                                     PG_SELFTEST_B0 + 3600, 1u, 0), &report);
    ok = report.credited_seconds == 3600 && report.clock_jumped;
    (void)printf("  clock jumped         credited=%-9" PRId64 " ticks=%-5"
                 PRIu32 " %s\n", report.credited_seconds, report.care_ticks,
                 ok ? "ok" : "MISMATCH");
    pg_selftest_check(ok, "a same-boot forward jump credited the boot gap");

    /* A new boot id has no cross-check, so the wall gap is all there is. */
    pg_selftest_state(&state, seed, (uint8_t)PG_SPECIES_POTHOS,
                      (uint8_t)PG_POT_TERRACOTTA, (uint8_t)PG_SPOT_ONE_METRE);
    (void)pg_advance(&state,
                     pg_selftest_now(PG_SELFTEST_T0 + 2 * PG_SELFTEST_DAY, 300,
                                     9u, 0), &report);
    ok = report.credited_seconds == 2 * PG_SELFTEST_DAY
      && report.care_ticks == 192u;
    (void)printf("  reboot               credited=%-9" PRId64 " ticks=%-5"
                 PRIu32 " %s\n", report.credited_seconds, report.care_ticks,
                 ok ? "ok" : "MISMATCH");
    pg_selftest_check(ok, "a reboot fell back to the wall gap");

    /* Past the cap: resolved, not simulated, and resolved the same way every
     * time. */
    {
        uint8_t species;
        for (species = 0u; species < (uint8_t)PG_SPECIES_COUNT; ++species) {
            pg_state again;
            pg_catchup_report second;

            pg_selftest_state(&state, seed, species, (uint8_t)PG_POT_TERRACOTTA,
                              (uint8_t)PG_SPOT_ONE_METRE);
            memcpy(&again, &state, sizeof again);
            (void)pg_advance(&state,
                             pg_selftest_now(PG_SELFTEST_T0
                                             + 90 * PG_SELFTEST_DAY,
                                             PG_SELFTEST_B0
                                             + 90 * PG_SELFTEST_DAY, 1u, 0),
                             &report);
            (void)pg_advance(&again,
                             pg_selftest_now(PG_SELFTEST_T0
                                             + 90 * PG_SELFTEST_DAY,
                                             PG_SELFTEST_B0
                                             + 90 * PG_SELFTEST_DAY, 1u, 0),
                             &second);
            ok = report.abandoned && report.care_ticks == 0u
              && pg_state_digest(&state) == pg_state_digest(&again);
            (void)printf("  abandoned %-12s life=%u health=%u ticks=%-3" PRIu32
                         " %s\n", pg_content_species(species)->id,
                         (unsigned)state.plants[0].life_state,
                         (unsigned)pg_plant_health(&state.plants[0]),
                         report.care_ticks, ok ? "ok" : "MISMATCH");
            pg_selftest_check(ok, "abandonment resolved without simulating");
        }
        /* The difficulty ordering, stated once and asserted here (D-043). */
        pg_selftest_state(&state, seed, (uint8_t)PG_SPECIES_SNAKE,
                          (uint8_t)PG_POT_TERRACOTTA,
                          (uint8_t)PG_SPOT_ONE_METRE);
        (void)pg_advance(&state,
                         pg_selftest_now(PG_SELFTEST_T0 + 90 * PG_SELFTEST_DAY,
                                         PG_SELFTEST_B0 + 90 * PG_SELFTEST_DAY,
                                         1u, 0), &report);
        pg_selftest_check(state.plants[0].life_state
                          != (uint8_t)PG_LIFE_DEAD,
                          "the snake plant survived a season alone");
        pg_selftest_state(&state, seed, (uint8_t)PG_SPECIES_CALATHEA,
                          (uint8_t)PG_POT_TERRACOTTA,
                          (uint8_t)PG_SPOT_ONE_METRE);
        (void)pg_advance(&state,
                         pg_selftest_now(PG_SELFTEST_T0 + 90 * PG_SELFTEST_DAY,
                                         PG_SELFTEST_B0 + 90 * PG_SELFTEST_DAY,
                                         1u, 0), &report);
        pg_selftest_check(state.plants[0].life_state == (uint8_t)PG_LIFE_DEAD
                          && state.replant_offered,
                          "the calathea did not, and the replant flow opened");
    }

    /* D-089: a soak that was running when the player disappeared becomes a
     * bog. Abandonment does not rescue it. */
    pg_selftest_state(&state, seed, (uint8_t)PG_SPECIES_PEACE_LILY,
                      (uint8_t)PG_POT_TERRACOTTA, (uint8_t)PG_SPOT_ONE_METRE);
    {
        pg_action_result result;
        const pg_species *species =
            pg_content_species(state.plants[0].species_id);
        uint32_t tolerance = (species != NULL)
                           ? (uint32_t)species->saturation_tolerance_ticks : 1u;

        state.plants[0].moisture_top = 1000u;
        state.plants[0].moisture_bottom = 1000u;
        (void)pg_actions_apply(&state, 0u, PG_VERB_BOTTOM_SOAK, 0u, &result);
        pg_selftest_check(state.plants[0].pending_action
                          == (uint8_t)PG_PENDING_BOTTOM_SOAK,
                          "a bottom-soak became a pending timed action");
        (void)pg_advance(&state,
                         pg_selftest_now(PG_SELFTEST_T0 + 45 * PG_SELFTEST_DAY,
                                         PG_SELFTEST_B0 + 45 * PG_SELFTEST_DAY,
                                         1u, 0), &report);
        ok = report.abandoned
          && state.plants[0].pending_action == (uint8_t)PG_PENDING_NONE
          && state.plants[0].soggy_ticks >= tolerance * 4u
          && state.plants[0].life_state != (uint8_t)PG_LIFE_DEAD;
        (void)printf("  soak abandoned       pending=%u soggy=%" PRIu32
                     " life=%u %s\n", (unsigned)state.plants[0].pending_action,
                     state.plants[0].soggy_ticks,
                     (unsigned)state.plants[0].life_state,
                     ok ? "ok" : "MISMATCH");
        pg_selftest_check(ok, "an abandoned soak force-completed to a bog");
    }

    /* Idempotence: the same `now` twice credits nothing the second time. */
    pg_selftest_state(&state, seed, (uint8_t)PG_SPECIES_POTHOS,
                      (uint8_t)PG_POT_TERRACOTTA, (uint8_t)PG_SPOT_ONE_METRE);
    (void)pg_advance(&state,
                     pg_selftest_now(PG_SELFTEST_T0 + 3600,
                                     PG_SELFTEST_B0 + 3600, 1u, 0), &report);
    {
        uint64_t before = pg_state_digest(&state);
        (void)pg_advance(&state,
                         pg_selftest_now(PG_SELFTEST_T0 + 3600,
                                         PG_SELFTEST_B0 + 3600, 1u, 0),
                         &report);
        ok = report.credited_seconds == 0 && report.care_ticks == 0u
          && pg_state_digest(&state) == before;
        (void)printf("  idempotent           credited=%-9" PRId64
                     " digest=%016" PRIx64 " %s\n", report.credited_seconds,
                     before, ok ? "ok" : "MISMATCH");
        pg_selftest_check(ok, "a repeated `now` credited nothing");
    }
}

int pg_sim_run_selftest(uint64_t seed, uint32_t sim_days)
{
    char error[128];
    uint8_t species;

    if (sim_days == 0u) {
        sim_days = 1u;
    }
    if (sim_days > 20000u) {
        sim_days = 20000u;           /* the printed number stays honest */
    }
    pg_selftest_failed = 0;

    if (!pg_content_validate(error, sizeof error)) {
        (void)printf("selftest: content invalid: %s\n", error);
        return 1;
    }

    (void)printf("selftest seed=%" PRIu64 " days=%" PRIu32 " state=%zu bytes\n",
                 seed, sim_days, pg_state_size());

    (void)puts("1 byte determinism: nothing below is measured, timed or "
               "addressed");

    (void)puts("2 equivalence of paths");
    pg_selftest_equivalence(seed, sim_days);
    pg_selftest_away_report(seed);

    (void)puts("3+4 monotonicity and invariants");
    for (species = 0u; species < (uint8_t)PG_SPECIES_COUNT; ++species) {
        pg_selftest_soak(seed, sim_days, species);
    }

    (void)puts("5 boundedness");
    pg_selftest_bounds(seed);

    (void)puts("6 rollback");
    pg_selftest_rollback(seed);

    (void)puts("7 clock matrix");
    pg_selftest_clocks(seed);

    (void)printf("selftest: %s\n", pg_selftest_failed == 0 ? "PASS" : "FAIL");
    return pg_selftest_failed == 0 ? 0 : 1;
}
