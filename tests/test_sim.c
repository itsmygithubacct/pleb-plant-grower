/*
 * test_sim — the catch-up engine.
 *
 * --selftest is the readable report; these are the assertions that must not be
 * allowed to drift. Three of them carry the whole milestone:
 *
 *   - **save equals ticks.** The same elapsed time, simulated tick by tick
 *     with the game open, in one step with the game closed, and in ragged
 *     chunks that are not tick multiples, must produce a bit-identical plant.
 *     That is what proves offline progression is a pure function of (saved
 *     record, now) rather than an approximation of one.
 *   - **bounded work.** No gap, however large, costs more than 2880 care ticks
 *     plus 30 day steps, because credit is capped at 30 days and a gap past
 *     the cap is resolved rather than simulated.
 *   - **all or nothing.** A step that fails validation restores the entry
 *     state byte for byte and says so, so a half-simulated fortnight can never
 *     be saved.
 */
#include "pg_sim.h"

#include "pg_actions.h"
#include "pg_calendar.h"
#include "pg_care.h"
#include "pg_content.h"
#include "pg_plant.h"
#include "pg_state.h"
#include "pg_time.h"

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

#define T0 INT64_C(1767225600)      /* 2026-01-01T00:00:00Z */
#define B0 INT64_C(100000)
#define DAY INT64_C(86400)

static pg_now at(int64_t wall_s, int64_t boot_s, uint8_t boot_tag,
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

/* An anchored save: the first advance establishes the anchor and credits
 * nothing, which is what every case below measures a real gap against. */
static void anchored(pg_state *state, uint8_t species_id, uint8_t pot_id,
                     uint8_t spot_id, int32_t tz_minutes)
{
    pg_catchup_report report;

    pg_init(state, 0x5eedu);
    pg_plant_init(&state->plants[0], species_id, pot_id, spot_id, T0);
    (void)pg_advance(state, at(T0, B0, 1u, tz_minutes), &report);
}

/* ---- the anchor -------------------------------------------------------- */

static bool test_first_run_credits_nothing(void)
{
    pg_state state;
    pg_catchup_report report;

    pg_init(&state, 7u);
    CHECK(!state.anchor.established);
    CHECK(pg_advance(&state, at(T0, B0, 1u, 0), &report));
    CHECK(report.credited_seconds == 0);
    CHECK(report.care_ticks == 0u);
    CHECK(!report.abandoned && !report.clock_backwards);
    CHECK(state.anchor.established);
    /* The calendar's origin, and the plant's own start date. */
    CHECK(state.first_run_wall_s == T0);
    CHECK(state.plants[0].planted_wall_s == T0);
    CHECK(state.clock.care_seconds_total == 0u);
    return true;
}

static bool test_a_repeated_now_credits_nothing(void)
{
    pg_state state;
    pg_catchup_report report;
    uint64_t digest;

    anchored(&state, (uint8_t)PG_SPECIES_POTHOS, (uint8_t)PG_POT_TERRACOTTA,
             (uint8_t)PG_SPOT_ONE_METRE, 0);
    CHECK(pg_advance(&state, at(T0 + 3600, B0 + 3600, 1u, 0), &report));
    CHECK(report.care_ticks == 4u);
    digest = pg_state_digest(&state);

    /* Idempotence is structural: the anchor moved with the credit, in the same
     * record, so a second look at the same instant is a zero-length gap. */
    CHECK(pg_advance(&state, at(T0 + 3600, B0 + 3600, 1u, 0), &report));
    CHECK(report.credited_seconds == 0);
    CHECK(report.care_ticks == 0u);
    CHECK(pg_state_digest(&state) == digest);
    return true;
}

/* ---- the headline assertion -------------------------------------------- */

/* Thirty days, three ways. The plant record, the care clock and the anchors
 * must agree exactly.
 *
 * The journal ring is deliberately not compared: the two tiers exist precisely
 * to describe a fortnight more coarsely than an afternoon, and a report that
 * said the same about both would mean the fine tier had no reason to exist. */
static bool test_save_equals_ticks(void)
{
    uint8_t species;

    for (species = 0u; species < (uint8_t)PG_SPECIES_COUNT; ++species) {
        pg_state live;
        pg_state saved;
        pg_state ragged;
        pg_catchup_report report;
        uint32_t total_ticks = 30u * 96u;
        uint32_t tick;
        int64_t elapsed = 0;
        static const int64_t chunk[6] = { 137, 4001, 900, 59, 86399, 613 };
        size_t which = 0u;
        int64_t target = (int64_t)total_ticks * PG_CARE_TICK_SECONDS;

        anchored(&live, species, (uint8_t)(species % (uint8_t)PG_POT_COUNT),
                 (uint8_t)(species % (uint8_t)PG_SPOT_COUNT), 0);
        memcpy(&saved, &live, sizeof saved);
        memcpy(&ragged, &live, sizeof ragged);

        for (tick = 1u; tick <= total_ticks; ++tick) {
            int64_t offset = (int64_t)tick * PG_CARE_TICK_SECONDS;
            CHECK(pg_advance(&live, at(T0 + offset, B0 + offset, 1u, 0),
                             &report));
            CHECK(report.care_ticks == 1u);
        }
        CHECK(pg_advance(&saved, at(T0 + target, B0 + target, 1u, 0), &report));
        CHECK(report.care_ticks == 2880u);
        CHECK(report.days_coarse == 30u);
        CHECK(!report.abandoned);

        while (elapsed < target) {
            int64_t step = chunk[which % 6u];
            which += 1u;
            if (elapsed + step > target) {
                step = target - elapsed;
            }
            elapsed += step;
            CHECK(pg_advance(&ragged, at(T0 + elapsed, B0 + elapsed, 1u, 0),
                             &report));
        }

        CHECK(memcmp(live.plants, saved.plants, sizeof live.plants) == 0);
        CHECK(memcmp(live.plants, ragged.plants, sizeof live.plants) == 0);
        CHECK(live.clock.care_seconds_total == saved.clock.care_seconds_total);
        CHECK(live.clock.care_seconds_total == ragged.clock.care_seconds_total);
        CHECK(live.clock.care_residual_s == ragged.clock.care_residual_s);
        CHECK(pg_state_digest(&live) == pg_state_digest(&saved));
        CHECK(pg_state_digest(&live) == pg_state_digest(&ragged));
        /* And it really did something: a month is not a no-op. */
        CHECK(live.clock.care_seconds_total == (uint64_t)target);
    }
    return true;
}

/* The fine tier is an observation window, not an arithmetic one: the same
 * thirty days seen entirely through the fine tier and seen mostly through the
 * coarse tier still produce the same plant, and differ only in how much was
 * said about it. */
static bool test_the_tiers_differ_only_in_what_they_say(void)
{
    pg_state live;
    pg_state saved;
    pg_catchup_report report;
    uint32_t tick;
    uint16_t one_step_entries;
    uint32_t live_entries = 0u;

    anchored(&live, (uint8_t)PG_SPECIES_CALATHEA, (uint8_t)PG_POT_GLAZED,
             (uint8_t)PG_SPOT_RADIATOR_SHELF, 0);
    memcpy(&saved, &live, sizeof saved);

    for (tick = 1u; tick <= 30u * 96u; ++tick) {
        int64_t offset = (int64_t)tick * PG_CARE_TICK_SECONDS;
        CHECK(pg_advance(&live, at(T0 + offset, B0 + offset, 1u, 0), &report));
        live_entries += report.entry_count;
    }
    CHECK(pg_advance(&saved, at(T0 + 30 * DAY, B0 + 30 * DAY, 1u, 0),
                     &report));
    one_step_entries = report.entry_count;

    CHECK(memcmp(live.plants, saved.plants, sizeof live.plants) == 0);
    /* One report is capped for display; the ring behind it is not, and the
     * live path saw every tick at fine resolution. */
    CHECK(one_step_entries <= (uint16_t)PG_JOURNAL_REPORT_MAX);
    CHECK(live_entries > 0u);
    return true;
}

/* ---- bounds ------------------------------------------------------------- */

static bool test_no_gap_costs_more_than_thirty_days(void)
{
    static const int64_t gaps[7] = { 1, 900, 3 * DAY, 29 * DAY, 30 * DAY,
                                     400 * DAY, 40000 * DAY };
    size_t index;

    for (index = 0u; index < 7u; ++index) {
        pg_state state;
        pg_catchup_report report;

        anchored(&state, (uint8_t)PG_SPECIES_POTHOS,
                 (uint8_t)PG_POT_TERRACOTTA, (uint8_t)PG_SPOT_ONE_METRE, 0);
        CHECK(pg_advance(&state, at(T0 + gaps[index], B0 + gaps[index], 1u, 0),
                         &report));
        CHECK(report.care_ticks <= PG_SIM_MAX_TICKS);
        CHECK(report.days_coarse <= PG_SIM_MAX_DAYS);
        CHECK(report.credited_seconds <= PG_GAP_CREDIT_MAX_SECONDS);
        if (report.abandoned) {
            /* Past the cap nothing is simulated at all: the outcome is
             * resolved once, from species tolerance. */
            CHECK(report.care_ticks == 0u);
            CHECK(report.days_coarse == 0u);
        }
    }
    /* The exact boundary: thirty days is 2880 ticks and thirty midnights. */
    {
        pg_state state;
        pg_catchup_report report;

        anchored(&state, (uint8_t)PG_SPECIES_POTHOS,
                 (uint8_t)PG_POT_TERRACOTTA, (uint8_t)PG_SPOT_ONE_METRE, 0);
        CHECK(pg_advance(&state, at(T0 + 30 * DAY, B0 + 30 * DAY, 1u, 0),
                         &report));
        CHECK(report.care_ticks == PG_SIM_MAX_TICKS);
        CHECK(report.days_coarse == PG_SIM_MAX_DAYS);
        CHECK(!report.abandoned);
    }
    return true;
}

static bool test_the_care_clock_is_monotone(void)
{
    pg_state state;
    pg_catchup_report report;
    pg_rng dice;
    uint64_t previous = 0u;
    int64_t wall = T0;
    int64_t boot = B0;
    uint32_t step;
    char error[96];

    anchored(&state, (uint8_t)PG_SPECIES_PEACE_LILY, (uint8_t)PG_POT_NURSERY,
             (uint8_t)PG_SPOT_TWO_METRE, 0);
    pg_rng_seed(&dice, 0xa11ceu);

    for (step = 0u; step < 600u; ++step) {
        uint32_t roll = pg_rng_next_u32(&dice);
        int64_t gap;

        if ((step % 17u) == 3u) {
            gap = -(int64_t)(roll % 200000u);          /* the clock fell back */
        } else if ((step % 53u) == 7u) {
            gap = (int64_t)(35u + roll % 900u) * DAY;  /* gone for months     */
        } else {
            gap = (int64_t)(roll % 200000u);
        }
        wall += gap;
        boot += (gap > 0) ? gap : 0;
        (void)pg_advance(&state, at(wall, boot, 1u, 0), &report);
        CHECK(state.clock.care_seconds_total >= previous);
        previous = state.clock.care_seconds_total;
        CHECK(pg_validate(&state, error, sizeof error));
        /* The residual is always the total modulo one tick; ticks never
         * drift. */
        CHECK(state.clock.care_seconds_total % (uint64_t)PG_CARE_TICK_SECONDS
              == (uint64_t)state.clock.care_residual_s);
        if (state.replant_offered
            && state.plants[0].life_state == (uint8_t)PG_LIFE_DEAD) {
            pg_plant_init(&state.plants[0], state.plants[0].species_id,
                          state.plants[0].pot_id, state.plants[0].spot_id,
                          wall);
            state.replant_offered = false;
        }
    }
    return true;
}

/* ---- transactionality --------------------------------------------------- */

static bool test_a_poisoned_step_rolls_all_the_way_back(void)
{
    pg_state state;
    pg_state entry;
    pg_catchup_report report;

    anchored(&state, (uint8_t)PG_SPECIES_CALATHEA, (uint8_t)PG_POT_CACHEPOT,
             (uint8_t)PG_SPOT_BATH_SHELF, 0);
    memcpy(&entry, &state, sizeof entry);

    pg_sim_poison_after(41u);
    CHECK(!pg_advance(&state, at(T0 + 5 * DAY, B0 + 5 * DAY, 1u, 0), &report));
    CHECK(report.rolled_back);
    CHECK(report.credited_seconds == 0);
    CHECK(report.entry_count == 0u);
    /* Byte for byte, padding included: a rollback is a restore, not a
     * repair. */
    CHECK(memcmp(&state, &entry, sizeof state) == 0);

    /* The poison is one-shot, and a rolled-back save is not a wedged one. */
    CHECK(pg_advance(&state, at(T0 + 5 * DAY, B0 + 5 * DAY, 1u, 0), &report));
    CHECK(!report.rolled_back);
    CHECK(report.care_ticks == 480u);
    return true;
}

static bool test_validate_rejects_what_the_simulation_cannot_produce(void)
{
    pg_state state;
    char error[96];

    anchored(&state, (uint8_t)PG_SPECIES_SNAKE, (uint8_t)PG_POT_GLAZED,
             (uint8_t)PG_SPOT_NORTH_SHELF, 0);
    CHECK(pg_validate(&state, error, sizeof error));

    state.plants[0].turgor = (uint16_t)(PG_LEVEL_MAX + 1);
    CHECK(!pg_validate(&state, error, sizeof error));
    CHECK(error[0] != '\0');
    state.plants[0].turgor = 5000u;

    state.plants[0].dli_today = 3001u;
    CHECK(!pg_validate(&state, error, sizeof error));
    state.plants[0].dli_today = 0u;

    state.plants[0].root_health = (uint16_t)(state.plants[0].root_capacity + 1u);
    CHECK(!pg_validate(&state, error, sizeof error));
    state.plants[0].root_health = state.plants[0].root_capacity;

    state.plants[0].species_id = 9u;
    CHECK(!pg_validate(&state, error, sizeof error));
    state.plants[0].species_id = (uint8_t)PG_SPECIES_SNAKE;

    state.clock.care_residual_s = 900u;
    CHECK(!pg_validate(&state, error, sizeof error));
    state.clock.care_residual_s = 0u;

    CHECK(pg_validate(&state, error, sizeof error));
    return true;
}

/* ---- abandonment -------------------------------------------------------- */

/* D-043 and GAME_DESIGN.md §10.1, which are the same sentence: Sarge is fine,
 * Goldie is a crisped survivor, Ophelia is collapsed and scarred but alive if
 * the pot held any water, Nyx is dead. One sentence, the whole difficulty
 * ordering. */
static bool test_the_tolerance_table(void)
{
    pg_state state;
    pg_catchup_report report;

    anchored(&state, (uint8_t)PG_SPECIES_SNAKE, (uint8_t)PG_POT_TERRACOTTA,
             (uint8_t)PG_SPOT_ONE_METRE, 0);
    CHECK(pg_advance(&state, at(T0 + 60 * DAY, B0 + 60 * DAY, 1u, 0), &report));
    CHECK(report.abandoned);
    CHECK(state.plants[0].life_state == (uint8_t)PG_LIFE_ACTIVE);
    CHECK(state.plants[0].turgor >= 8000u);

    anchored(&state, (uint8_t)PG_SPECIES_POTHOS, (uint8_t)PG_POT_TERRACOTTA,
             (uint8_t)PG_SPOT_ONE_METRE, 0);
    CHECK(pg_advance(&state, at(T0 + 60 * DAY, B0 + 60 * DAY, 1u, 0), &report));
    CHECK(state.plants[0].life_state == (uint8_t)PG_LIFE_AILING);
    CHECK(state.plants[0].leaf_count == 2u || state.plants[0].leaf_count == 3u);
    CHECK((state.plants[0].leaves[0].damage_mask
           & (uint8_t)PG_DAMAGE_TIP_CRISP) != 0u);

    /* The peace lily depends on whether the pot held any water at all, which
     * is the cachepot's one genuine virtue. */
    anchored(&state, (uint8_t)PG_SPECIES_PEACE_LILY, (uint8_t)PG_POT_TERRACOTTA,
             (uint8_t)PG_SPOT_ONE_METRE, 0);
    CHECK(pg_advance(&state, at(T0 + 60 * DAY, B0 + 60 * DAY, 1u, 0), &report));
    CHECK(state.plants[0].life_state == (uint8_t)PG_LIFE_DEAD);
    CHECK(state.replant_offered);

    anchored(&state, (uint8_t)PG_SPECIES_PEACE_LILY, (uint8_t)PG_POT_CACHEPOT,
             (uint8_t)PG_SPOT_ONE_METRE, 0);
    CHECK(pg_advance(&state, at(T0 + 60 * DAY, B0 + 60 * DAY, 1u, 0), &report));
    CHECK(state.plants[0].life_state == (uint8_t)PG_LIFE_TERMINAL);
    CHECK(pg_plant_is_collapsed(&state.plants[0]));

    anchored(&state, (uint8_t)PG_SPECIES_CALATHEA, (uint8_t)PG_POT_TERRACOTTA,
             (uint8_t)PG_SPOT_ONE_METRE, 0);
    CHECK(pg_advance(&state, at(T0 + 60 * DAY, B0 + 60 * DAY, 1u, 0), &report));
    CHECK(state.plants[0].life_state == (uint8_t)PG_LIFE_DEAD);
    /* No fail screen: the pot and the calendar stay, and the replant flow
     * opens. */
    CHECK(state.replant_offered);
    CHECK(state.plants[0].pot_id == (uint8_t)PG_POT_TERRACOTTA);
    CHECK(state.first_run_wall_s == T0);
    return true;
}

/* D-089: abandonment does not rescue a running soak. */
static bool test_an_abandoned_soak_becomes_a_bog(void)
{
    pg_state state;
    pg_catchup_report report;
    pg_action_result result;
    const pg_species *species;
    uint32_t tolerance;

    anchored(&state, (uint8_t)PG_SPECIES_POTHOS, (uint8_t)PG_POT_TERRACOTTA,
             (uint8_t)PG_SPOT_ONE_METRE, 0);
    species = pg_content_species(state.plants[0].species_id);
    CHECK(species != NULL);
    tolerance = (uint32_t)species->saturation_tolerance_ticks;

    state.plants[0].moisture_top = 800u;
    state.plants[0].moisture_bottom = 800u;
    CHECK(pg_actions_apply(&state, 0u, PG_VERB_BOTTOM_SOAK, 0u, &result));
    CHECK(state.plants[0].pending_action == (uint8_t)PG_PENDING_BOTTOM_SOAK);

    CHECK(pg_advance(&state, at(T0 + 45 * DAY, B0 + 45 * DAY, 1u, 0), &report));
    CHECK(report.abandoned);
    CHECK(state.plants[0].pending_action == (uint8_t)PG_PENDING_NONE);
    CHECK(state.plants[0].pending_action_care_s == 0u);
    /* A bog: saturated for far longer than the species tolerates, with the
     * root damage that follows from it. */
    CHECK(state.plants[0].soggy_ticks >= tolerance * 4u);
    CHECK(state.plants[0].root_health < 9000u);
    CHECK(state.plants[0].root_health <= state.plants[0].root_capacity);
    return true;
}

/* The soak's deadline is care time, so it survives being closed over. */
static bool test_a_soak_deadline_is_care_time(void)
{
    pg_state state;
    pg_catchup_report report;
    pg_action_result result;
    uint16_t index;
    bool told = false;

    anchored(&state, (uint8_t)PG_SPECIES_POTHOS, (uint8_t)PG_POT_TERRACOTTA,
             (uint8_t)PG_SPOT_ONE_METRE, 0);
    state.plants[0].moisture_top = 800u;
    state.plants[0].moisture_bottom = 800u;
    CHECK(pg_actions_apply(&state, 0u, PG_VERB_BOTTOM_SOAK, 0u, &result));

    /* Close the lid for two hours: the soak comes up to time inside the gap,
     * at 900 s resolution, and the report says so. */
    CHECK(pg_advance(&state, at(T0 + 7200, B0 + 7200, 1u, 0), &report));
    for (index = 0u; index < report.entry_count; ++index) {
        if (report.entries[index].kind == (uint8_t)PG_JOURNAL_SOAK_FINISHED) {
            told = true;
        }
    }
    CHECK(told);
    /* And it is still sitting in the water, because nobody took it out. That
     * is the mechanic, not an oversight. */
    CHECK(state.plants[0].pending_action == (uint8_t)PG_PENDING_BOTTOM_SOAK);
    CHECK(state.plants[0].moisture_bottom == (uint16_t)PG_LEVEL_MAX);

    CHECK(pg_actions_apply(&state, 0u, PG_VERB_EMPTY_SAUCER, 0u, &result));
    CHECK(state.plants[0].pending_action == (uint8_t)PG_PENDING_NONE);
    return true;
}

/* ---- the local offset is a simulation input (D-079) --------------------- */

static bool test_the_local_offset_changes_the_simulation(void)
{
    pg_state utc;
    pg_state tokyo;
    pg_catchup_report report;

    anchored(&utc, (uint8_t)PG_SPECIES_CALATHEA, (uint8_t)PG_POT_GLAZED,
             (uint8_t)PG_SPOT_SILL_SOUTH, 0);
    anchored(&tokyo, (uint8_t)PG_SPECIES_CALATHEA, (uint8_t)PG_POT_GLAZED,
             (uint8_t)PG_SPOT_SILL_SOUTH, 540);

    CHECK(pg_advance(&utc, at(T0 + 2 * DAY, B0 + 2 * DAY, 1u, 0), &report));
    CHECK(pg_advance(&tokyo, at(T0 + 2 * DAY, B0 + 2 * DAY, 1u, 540),
                     &report));

    /* Same epoch, same weather, different day: the sun arrives at a different
     * point in the gap, so the DLI and the temperatures do too. The calendar
     * itself is still a pure function of (wall_s, offset) — this is its
     * positive counterpart, not a contradiction of it. */
    CHECK(pg_state_digest(&utc) != pg_state_digest(&tokyo));
    CHECK(utc.clock.care_seconds_total == tokyo.clock.care_seconds_total);
    return true;
}

/* ---- the journal -------------------------------------------------------- */

static bool test_the_journal_ring_wraps_oldest_first(void)
{
    pg_state state;
    pg_journal_entry out[PG_JOURNAL_RING];
    size_t count;
    uint32_t index;

    pg_init(&state, 1u);
    CHECK(pg_journal_recent(&state, out, PG_JOURNAL_RING) == 0u);

    for (index = 0u; index < (uint32_t)PG_JOURNAL_RING + 5u; ++index) {
        pg_journal_push(&state, index, (uint8_t)PG_JOURNAL_WATERED,
                        (uint8_t)(index & 0xFFu), 0u);
    }
    count = pg_journal_recent(&state, out, PG_JOURNAL_RING);
    CHECK(count == (size_t)PG_JOURNAL_RING);
    /* Oldest first, newest last, and the five that fell off are gone. */
    CHECK(out[0].care_day == 5u);
    CHECK(out[count - 1u].care_day == (uint32_t)PG_JOURNAL_RING + 4u);
    CHECK(state.journal_count == (uint8_t)PG_JOURNAL_RING);

    /* A kind nothing ever emits is not written at all. */
    pg_journal_push(&state, 0u, (uint8_t)PG_JOURNAL_KIND_COUNT, 0u, 0u);
    CHECK(pg_journal_recent(&state, out, PG_JOURNAL_RING)
          == (size_t)PG_JOURNAL_RING);
    CHECK(out[count - 1u].care_day == (uint32_t)PG_JOURNAL_RING + 4u);

    /* Every kind has a line, and none of them blames anybody. */
    for (index = 1u; index < (uint32_t)PG_JOURNAL_KIND_COUNT; ++index) {
        const char *line = pg_journal_kind_line((uint8_t)index);
        CHECK(line != NULL && line[0] != '\0');
        CHECK(strstr(line, "you") == NULL);
    }
    return true;
}

/* ---- the verbs ---------------------------------------------------------- */

static bool test_the_verbs_that_the_catch_up_has_to_survive(void)
{
    pg_state state;
    pg_action_result result;
    pg_catchup_report report;
    const pg_species *species;

    anchored(&state, (uint8_t)PG_SPECIES_POTHOS, (uint8_t)PG_POT_TERRACOTTA,
             (uint8_t)PG_SPOT_ONE_METRE, 0);
    species = pg_content_species(state.plants[0].species_id);
    CHECK(species != NULL);

    /* Watering is thorough: both layers, and the salt that had somewhere to
     * go went. */
    state.plants[0].moisture_top = 100u;
    state.plants[0].moisture_bottom = 100u;
    state.plants[0].salt = 4000u;
    CHECK(pg_actions_apply(&state, 0u, PG_VERB_WATER_THOROUGHLY, 0u, &result));
    CHECK(result.happened);
    CHECK(state.plants[0].moisture_bottom == (uint16_t)PG_LEVEL_MAX);
    CHECK(state.plants[0].salt < 4000u);

    /* A sip wets the top and stops there. It succeeds; it does not help. */
    state.plants[0].moisture_top = 100u;
    state.plants[0].moisture_bottom = 100u;
    CHECK(pg_actions_apply(&state, 0u, PG_VERB_WATER_SIP, 0u, &result));
    CHECK(result.legality == PG_LEGAL_WARNED);
    CHECK(result.happened);
    CHECK(state.plants[0].moisture_top == (uint16_t)PG_LEVEL_MAX);
    CHECK(state.plants[0].moisture_bottom == 100u);

    /* The only refusals in the game are the four Feed cases, and the first of
     * January is squarely inside the first of them. */
    {
        pg_care_env env = pg_sim_env_now(&state, 0u);
        CHECK(!env.is_growing_season);
        CHECK(pg_care_feed_refusal(&state.plants[0], &env,
                                   pg_actions_care_now(&state))
              == PG_FEED_REFUSAL_DORMANT);
    }
    CHECK(!pg_actions_apply(&state, 0u, PG_VERB_FEED_HALF, 0u, &result));
    CHECK(result.legality == PG_LEGAL_REFUSED);
    CHECK(result.reason != NULL && result.reason[0] != '\0');
    CHECK(!result.happened);

    /* Everything else happens, with a reason where one is owed. */
    CHECK(pg_actions_apply(&state, 0u, PG_VERB_MIST, 0u, &result));
    CHECK(result.legality == PG_LEGAL_WARNED && result.happened);
    CHECK(pg_actions_apply(&state, 0u, PG_VERB_ROTATE, 0u, &result));
    CHECK(result.legality == PG_LEGAL_ALLOWED && result.happened);

    /* A cachepot's sleeve is its redemption, and the pot really changes. */
    state.plants[0].pot_id = (uint8_t)PG_POT_CACHEPOT;
    CHECK(pg_actions_apply(&state, 0u, PG_VERB_SLEEVE_LIFT, 0u, &result));
    CHECK(state.plants[0].pot_id == (uint8_t)PG_POT_NURSERY);
    CHECK(pg_actions_apply(&state, 0u, PG_VERB_SLEEVE_RETURN, 0u, &result));
    CHECK(state.plants[0].pot_id == (uint8_t)PG_POT_CACHEPOT);

    /* The eleven booleans are shortcuts for eleven verbs, and nothing else. */
    {
        pg_input input;
        memset(&input, 0, sizeof input);
        CHECK(pg_actions_verb_for_input(&input) == PG_VERB_COUNT);
        input.water = true;
        CHECK(pg_actions_verb_for_input(&input) == PG_VERB_WATER_THOROUGHLY);
        input.water = false;
        input.drain = true;
        CHECK(pg_actions_verb_for_input(&input) == PG_VERB_EMPTY_SAUCER);
    }

    /* And after all of that the record is still one a catch-up can advance. */
    CHECK(pg_advance(&state, at(T0 + 2 * DAY, B0 + 2 * DAY, 1u, 0), &report));
    CHECK(!report.rolled_back);
    return true;
}

/* ---- housekeeping ------------------------------------------------------- */

static bool test_the_next_deadline_is_a_wall_second(void)
{
    pg_state state;
    pg_catchup_report report;

    CHECK(pg_next_wall_deadline(NULL) == INT64_MAX);
    anchored(&state, (uint8_t)PG_SPECIES_POTHOS, (uint8_t)PG_POT_TERRACOTTA,
             (uint8_t)PG_SPOT_ONE_METRE, 0);
    CHECK(pg_next_wall_deadline(&state) == T0 + PG_CARE_TICK_SECONDS);

    /* Half a tick in, half a tick to go. */
    CHECK(pg_advance(&state, at(T0 + 450, B0 + 450, 1u, 0), &report));
    CHECK(report.care_ticks == 0u);
    CHECK(pg_next_wall_deadline(&state) == T0 + 900);
    return true;
}

static bool test_null_arguments_are_survivable(void)
{
    pg_catchup_report report;
    pg_state state;
    pg_action_result result;

    CHECK(!pg_advance(NULL, at(T0, B0, 1u, 0), &report));
    CHECK(!pg_validate(NULL, NULL, 0u));
    CHECK(pg_state_size() == sizeof(pg_state));
    pg_journal_push(NULL, 0u, (uint8_t)PG_JOURNAL_WATERED, 0u, 0u);
    CHECK(pg_journal_recent(NULL, NULL, 0u) == 0u);
    pg_audio_push(NULL, 0u, 1.0f, 1.0f);
    CHECK(pg_actions_verb_for_input(NULL) == PG_VERB_COUNT);
    CHECK(pg_actions_care_now(NULL) == 0u);

    anchored(&state, (uint8_t)PG_SPECIES_POTHOS, (uint8_t)PG_POT_TERRACOTTA,
             (uint8_t)PG_SPOT_ONE_METRE, 0);
    /* A report is optional; a plant index that does not exist is not fatal. */
    CHECK(pg_advance(&state, at(T0 + 900, B0 + 900, 1u, 0), NULL));
    CHECK(!pg_actions_apply(&state, 9u, PG_VERB_WATER_THOROUGHLY, 0u, &result));
    CHECK(!pg_actions_apply(&state, 0u, PG_VERB_COUNT, 0u, NULL));
    return true;
}

int main(void)
{
    char error[128];

    if (!pg_content_validate(error, sizeof error)) {
        (void)fprintf(stderr, "sim: content invalid: %s\n", error);
        return 1;
    }
    if (!test_first_run_credits_nothing()) return 1;
    if (!test_a_repeated_now_credits_nothing()) return 1;
    if (!test_save_equals_ticks()) return 1;
    if (!test_the_tiers_differ_only_in_what_they_say()) return 1;
    if (!test_no_gap_costs_more_than_thirty_days()) return 1;
    if (!test_the_care_clock_is_monotone()) return 1;
    if (!test_a_poisoned_step_rolls_all_the_way_back()) return 1;
    if (!test_validate_rejects_what_the_simulation_cannot_produce()) return 1;
    if (!test_the_tolerance_table()) return 1;
    if (!test_an_abandoned_soak_becomes_a_bog()) return 1;
    if (!test_a_soak_deadline_is_care_time()) return 1;
    if (!test_the_local_offset_changes_the_simulation()) return 1;
    if (!test_the_journal_ring_wraps_oldest_first()) return 1;
    if (!test_the_verbs_that_the_catch_up_has_to_survive()) return 1;
    if (!test_the_next_deadline_is_a_wall_second()) return 1;
    if (!test_null_arguments_are_survivable()) return 1;
    (void)puts("sim: PASS");
    return 0;
}
