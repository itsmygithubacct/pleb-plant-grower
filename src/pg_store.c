/*
 * pg_store — kilix-state's side of persistence: the A/B generation pair, the
 * fail-soft settings record, the sticky save_failed flag, and the failure
 * matrix of ARCHITECTURE.md §6.4.
 *
 * We reimplement none of the durability chain. kilix-state already writes
 * through an `openat(O_CREAT|O_EXCL|O_NOFOLLOW, 0600)` temporary in the same
 * directory, fsyncs it, renames it, fsyncs the directory, and fsyncs newly
 * created directory components so even the first save is durable. **A
 * KILIXSTATE_OK load therefore means the bytes are exactly the ones we wrote**,
 * and every remaining failure is semantic and belongs to pg_save.c's decoder.
 *
 * What is left for this file is the part kilix-state deliberately does not
 * have an opinion about: which of two records is the live one.
 *
 *   - **A save always goes to the generation that is not live.** A crash
 *     mid-write can therefore cost the newest record and never the plant,
 *     which is the only crash a houseplant kept for months actually fears.
 *   - **A damaged generation is where the next write goes**, so the readable
 *     one survives for a post-mortem. Two damaged generations are never
 *     silently wiped: they raise a screen and wait for a keypress.
 *   - **A record from a version we do not know is never overwritten.** An old
 *     binary must not clobber a newer save; it goes read-only and says so.
 *   - **Re-load before every save.** A second window tending the same plant is
 *     detected by a save_sequence that moved underneath us (D-070); detection
 *     is enough, and an advisory lock in a shared module is not.
 */
#include "pg_save.h"

#include "pg_actions.h"
#include "pg_content.h"
#include "pg_plant.h"
#include "pg_state.h"

#include "kilix_state.h"
#include "kilix_state_codec.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PG_STORE_APP_ID "pleb-plant-grower"
#define PG_STORE_SETTINGS_FILE "settings.state"
#define PG_STORE_CONFIG_ENV "PLEB_PLANT_CONFIG_HOME"

static const char *const PG_STORE_PLANT_FILES[2] = {
    "plant-a.state", "plant-b.state"
};

/* ---- the write-failure seam ---------------------------------------------
 *
 * A full disk and a read-only home are failure paths a player really hits, and
 * PG_STORE_IO_ERROR is the whole of the game's answer to them: a sticky banner
 * and a plant you can keep watering. Neither can be provoked from outside the
 * process -- the store writes through descriptors it already holds, so making
 * the directory read-only after open does not bite, and filling the disk is
 * not a test. Without a seam the branch is unreachable and therefore untested,
 * which is how a fail-soft path quietly becomes a crash.
 *
 * Same shape and same justification as pg_sim_poison_after: one file-scope
 * counter, consumed by the write that trips it, and it can only ever cause the
 * failure the code already handles. */
static uint32_t pg_store_forced_failures = 0u;

void pg_store_fail_next_writes(uint32_t count)
{
    pg_store_forced_failures = count;
}

static bool pg_store_take_forced_failure(void)
{
    if (pg_store_forced_failures == 0u) {
        return false;
    }
    pg_store_forced_failures -= 1u;
    return true;
}

/* ---- lifecycle ---------------------------------------------------------- */

static uint8_t store_status_for(kilixstate_result result)
{
    switch (result) {
    case KILIXSTATE_OK:        return (uint8_t)PG_STORE_READY;
    case KILIXSTATE_NOT_FOUND: return (uint8_t)PG_STORE_FIRST_RUN;
    case KILIXSTATE_CORRUPT:   return (uint8_t)PG_STORE_DAMAGED;
    case KILIXSTATE_IO_ERROR:  return (uint8_t)PG_STORE_IO_ERROR;
    case KILIXSTATE_SECURITY:  return (uint8_t)PG_STORE_UNAVAILABLE;
    default:                   return (uint8_t)PG_STORE_UNAVAILABLE;
    }
}

/* PLEB_PLANT_CONFIG_HOME names a directory, so the record path is built from
 * it; `base_directory` is the embedding host's data root and keeps the app_id
 * component. One option field distinguishes the two and there is no code fork
 * (ARCHITECTURE.md §6.1). A config home that is not absolute is ignored rather
 * than fatal: a stray environment variable must not stop the game. */
static bool store_open_one(kilixstate_store *store, const char *config_home,
                           const char *base_directory, const char *filename,
                           size_t max_payload)
{
    char path[KILIXSTATE_PATH_CAPACITY];
    kilixstate_options options;
    int length;

    kilixstate_options_init(&options);
    options.max_payload = max_payload;
    options.format = KILIXSTATE_FORMAT_CRC32;
    options.app_id = PG_STORE_APP_ID;
    options.filename = filename;
    if (config_home != NULL) {
        length = snprintf(path, sizeof path, "%s/%s", config_home, filename);
        if (length < 0 || (size_t)length >= sizeof path) {
            return false;
        }
        options.absolute_path = path;
    } else if (base_directory != NULL && base_directory[0] == '/') {
        options.base_directory = base_directory;
    }
    return kilixstate_store_init(store, &options) == KILIXSTATE_OK;
}

bool pg_store_open(pg_store *store, const char *base_directory)
{
    const char *config_home;
    size_t index;

    if (store == NULL) {
        return false;
    }
    memset(store, 0, sizeof *store);
    store->generation = PG_STORE_GENERATION_NONE;
    store->damaged_generation = PG_STORE_GENERATION_NONE;
    store->status = (uint8_t)PG_STORE_UNOPENED;

    config_home = getenv(PG_STORE_CONFIG_ENV);
    if (config_home != NULL && config_home[0] != '/') {
        config_home = NULL;
    }
    for (index = 0u; index < 2u; ++index) {
        if (!store_open_one(&store->plant[index], config_home, base_directory,
                            PG_STORE_PLANT_FILES[index],
                            (size_t)PG_SAVE_MAX_PAYLOAD)) {
            while (index > 0u) {
                index -= 1u;
                kilixstate_store_close(&store->plant[index]);
            }
            store->status = (uint8_t)PG_STORE_UNAVAILABLE;
            return false;
        }
    }
    store->plant_ready = true;
    /* Settings are allowed to be unavailable. Persistence can never prevent
     * play, and a preference is not a plant. */
    store->settings_ready = store_open_one(&store->settings, config_home,
                                           base_directory,
                                           PG_STORE_SETTINGS_FILE,
                                           (size_t)PG_SETTINGS_MAX_PAYLOAD);
    return true;
}

void pg_store_close(pg_store *store)
{
    if (store == NULL) {
        return;
    }
    if (store->plant_ready) {
        kilixstate_store_close(&store->plant[0]);
        kilixstate_store_close(&store->plant[1]);
        store->plant_ready = false;
    }
    if (store->settings_ready) {
        kilixstate_store_close(&store->settings);
        store->settings_ready = false;
    }
}

uint8_t pg_store_last_status(const pg_store *store)
{
    return (store == NULL) ? (uint8_t)PG_STORE_UNOPENED : store->status;
}

bool pg_store_save_failed(const pg_store *store)
{
    return (store != NULL) && store->save_failed;
}

uint8_t pg_store_generation(const pg_store *store)
{
    return (store == NULL) ? PG_STORE_GENERATION_NONE : store->generation;
}

/* In the plant's register, and never in the player's. "Could not write" is a
 * fact about a disk; "you forgot to save" would be a lie and a scolding. */
const char *pg_store_status_line(uint8_t status)
{
    switch ((pg_store_status)status) {
    case PG_STORE_UNOPENED:      return "";
    case PG_STORE_READY:         return "";
    case PG_STORE_FIRST_RUN:     return "a clean pot and a bag of compost";
    case PG_STORE_RECOVERED:     return "one page of the notebook was smudged;"
                                        " the other one held";
    case PG_STORE_DAMAGED:       return "the notebook is unreadable. Nothing"
                                        " has been thrown away";
    case PG_STORE_NEWER_VERSION: return "this plant is being tended by a newer"
                                        " version. Looking, not touching";
    case PG_STORE_CONFLICT:      return "another window is tending this plant";
    case PG_STORE_IO_ERROR:      return "the notebook will not take ink right"
                                        " now. Still growing";
    case PG_STORE_UNAVAILABLE:   return "there is nowhere to keep the notebook."
                                        " Still growing";
    case PG_STORE_STATUS_COUNT:
    default:                     return "";
    }
}

/* ---- load ---------------------------------------------------------------
 * Both generations are read, both are decoded into a candidate, and the higher
 * save_sequence wins. Nothing is published into the caller's state until one
 * candidate has passed pg_validate inside the decoder. */
bool pg_store_load(pg_state *state, pg_store *store, bool *recovered)
{
    pg_state chosen;
    pg_state candidate;
    uint64_t best_sequence = 0u;
    uint8_t best = PG_STORE_GENERATION_NONE;
    uint8_t failed = PG_STORE_GENERATION_NONE;
    bool any_missing = false;
    bool any_failed = false;
    bool any_newer = false;
    bool io_error = false;
    size_t index;

    if (recovered != NULL) {
        *recovered = false;
    }
    if (state == NULL || store == NULL) {
        return false;
    }
    if (!store->plant_ready) {
        store->status = (uint8_t)PG_STORE_UNAVAILABLE;
        return false;
    }
    memset(&chosen, 0, sizeof chosen);

    for (index = 0u; index < 2u; ++index) {
        size_t size = 0u;
        kilixstate_result result = kilixstate_load(&store->plant[index],
                                                   store->scratch,
                                                   sizeof store->scratch,
                                                   &size);

        if (result == KILIXSTATE_NOT_FOUND) {
            any_missing = true;
            continue;
        }
        if (result != KILIXSTATE_OK) {
            any_failed = true;
            failed = (uint8_t)index;
            io_error = io_error || result == KILIXSTATE_IO_ERROR;
            continue;
        }
        if (!pg_save_decode(&candidate, store->scratch, size)) {
            if (pg_save_last_reject()
                == (uint8_t)PG_SAVE_REJECT_NEWER_VERSION) {
                any_newer = true;
            } else {
                any_failed = true;
                failed = (uint8_t)index;
            }
            continue;
        }
        if (best == PG_STORE_GENERATION_NONE
            || candidate.save_sequence > best_sequence) {
            best_sequence = candidate.save_sequence;
            best = (uint8_t)index;
            chosen = candidate;
        }
    }

    /* An old binary must never clobber a newer save, even if the other
     * generation happens to be one it can read. */
    store->read_only = store->read_only || any_newer;

    if (best != PG_STORE_GENERATION_NONE) {
        store->generation = best;
        store->sequence = best_sequence;
        store->damaged_generation = failed;
        if (recovered != NULL) {
            *recovered = any_failed;
        }
        store->status = any_newer ? (uint8_t)PG_STORE_NEWER_VERSION
                      : any_failed ? (uint8_t)PG_STORE_RECOVERED
                                   : (uint8_t)PG_STORE_READY;
        *state = chosen;
        return true;
    }

    store->generation = PG_STORE_GENERATION_NONE;
    if (any_newer) {
        store->status = (uint8_t)PG_STORE_NEWER_VERSION;
    } else if (any_failed) {
        /* Do not silently wipe. The damaged-slot screen owns what happens
         * next, and the next write goes into the generation that failed. */
        store->status = io_error ? (uint8_t)PG_STORE_IO_ERROR
                                 : (uint8_t)PG_STORE_DAMAGED;
        store->damaged_generation = failed;
    } else if (any_missing) {
        store->status = (uint8_t)PG_STORE_FIRST_RUN;
        store->sequence = 0u;
    } else {
        store->status = (uint8_t)PG_STORE_UNAVAILABLE;
    }
    return false;
}

/* ---- save ---------------------------------------------------------------
 * pg_store_save takes a const pg_state, which is deliberate: the record's
 * save_sequence is a property of the *file*, not of the simulation, and the
 * store is what owns it. `state->save_sequence` is refreshed the next time the
 * record is loaded. */
static bool store_peek_conflict(pg_store *store)
{
    size_t index;

    for (index = 0u; index < 2u; ++index) {
        size_t size = 0u;
        uint64_t sequence = 0u;

        if (kilixstate_load(&store->plant[index], store->scratch,
                            sizeof store->scratch, &size) != KILIXSTATE_OK) {
            continue;
        }
        if (pg_save_peek_sequence(store->scratch, size, &sequence)
            && sequence > store->sequence) {
            return true;
        }
    }
    return false;
}

bool pg_store_save(const pg_state *state, pg_store *store)
{
    size_t written = 0u;
    uint64_t sequence;
    uint8_t target;
    kilixstate_result result;

    if (state == NULL || store == NULL) {
        return false;
    }
    if (!store->plant_ready) {
        store->status = (uint8_t)PG_STORE_UNAVAILABLE;
        store->save_failed = true;
        return false;
    }
    if (store->read_only) {
        store->status = (uint8_t)PG_STORE_NEWER_VERSION;
        store->save_failed = true;
        return false;
    }
    if (store_peek_conflict(store)) {
        store->status = (uint8_t)PG_STORE_CONFLICT;
        store->save_failed = true;
        return false;
    }
    sequence = (state->save_sequence > store->sequence) ? state->save_sequence
                                                        : store->sequence;
    if (sequence == UINT64_MAX) {
        store->status = (uint8_t)PG_STORE_UNAVAILABLE;
        store->save_failed = true;
        return false;
    }
    sequence += 1u;

    /* The generation that failed to load, if there is one — so the readable
     * record survives for the post-mortem — otherwise the one that is not
     * live. */
    if (store->damaged_generation != PG_STORE_GENERATION_NONE) {
        target = store->damaged_generation;
    } else if (store->generation == PG_STORE_GENERATION_NONE) {
        target = 0u;
    } else {
        target = (uint8_t)(store->generation == 0u ? 1u : 0u);
    }

    if (!pg_save_encode_with_sequence(state, sequence, store->scratch,
                                      sizeof store->scratch, &written)) {
        store->status = (uint8_t)PG_STORE_UNAVAILABLE;
        store->save_failed = true;
        return false;
    }
    result = pg_store_take_forced_failure()
           ? KILIXSTATE_IO_ERROR
           : kilixstate_save(&store->plant[target], store->scratch, written);
    if (result != KILIXSTATE_OK) {
        store->status = store_status_for(result);
        if (store->status == (uint8_t)PG_STORE_FIRST_RUN
            || store->status == (uint8_t)PG_STORE_READY) {
            store->status = (uint8_t)PG_STORE_IO_ERROR;
        }
        store->save_failed = true;
        return false;
    }
    store->generation = target;
    store->damaged_generation = PG_STORE_GENERATION_NONE;
    store->sequence = sequence;
    store->status = (uint8_t)PG_STORE_READY;
    store->save_failed = false;
    return true;
}

/* ---- settings -----------------------------------------------------------
 * Fail-soft on every path: defaults are written first and success is returned
 * whatever happens, because a lost preference must never be able to stop a
 * plant from being watered. */
bool pg_settings_load(pg_settings *out, pg_store *store)
{
    uint8_t bytes[PG_SETTINGS_MAX_PAYLOAD];
    size_t size = 0u;

    if (out == NULL) {
        return true;
    }
    pg_settings_defaults(out);
    if (store == NULL || !store->settings_ready) {
        return true;
    }
    if (kilixstate_load(&store->settings, bytes, sizeof bytes, &size)
        != KILIXSTATE_OK) {
        return true;
    }
    if (!pg_settings_decode(out, bytes, size)) {
        pg_settings_defaults(out);
    }
    return true;
}

bool pg_settings_save(const pg_settings *in, pg_store *store)
{
    uint8_t bytes[PG_SETTINGS_MAX_PAYLOAD];
    size_t written = 0u;

    if (in == NULL || store == NULL || !store->settings_ready) {
        return false;
    }
    if (!pg_settings_encode(in, bytes, sizeof bytes, &written)) {
        return false;
    }
    return kilixstate_save(&store->settings, bytes, written)
           == KILIXSTATE_OK;
}

/* =========================================================================
 * --save-test: the round trip, then the ported corruption harness.
 *
 * Every case is deterministic and every number printed is a pure function of
 * the fixtures, so a regression is one changed line. The rolling FNV hash at
 * the end covers every decision the harness made, which is what makes "one
 * changed number" true rather than aspirational.
 * ========================================================================= */

#define PG_TEST_FNV_OFFSET UINT32_C(2166136261)
#define PG_TEST_FNV_PRIME UINT32_C(16777619)
#define PG_TEST_MUTATION_SEED UINT32_C(0x504c4e54)
#define PG_TEST_RECORD_HEADER_BYTES 16u

typedef struct pg_test_report {
    uint32_t payload_mutations;
    uint32_t payload_accepted;
    uint32_t payload_rejected;
    uint32_t length_cases;
    uint32_t length_field_cases;
    uint32_t semantic_cases;
    uint32_t record_bit_flips;
    uint32_t record_truncations;
    uint32_t oversize_records;
    uint32_t round_trips;
    uint32_t failures;
    uint32_t hash;
} pg_test_report;

static uint32_t hash_u32(uint32_t hash, uint32_t value)
{
    unsigned int shift;

    for (shift = 0u; shift < 32u; shift += 8u) {
        hash ^= (value >> shift) & UINT32_C(0xff);
        hash *= PG_TEST_FNV_PRIME;
    }
    return hash;
}

static uint32_t next_random(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    *state = value;
    return value;
}

static bool test_fail(pg_test_report *report, const char *what, size_t index)
{
    report->failures += 1u;
    (void)fprintf(stderr, "save-test: %s (case %zu)\n", what, index);
    return false;
}

/* ---- fixtures ----------------------------------------------------------- */

#define PG_TEST_T0 INT64_C(1767225600)   /* 2026-01-01T00:00:00Z */
#define PG_TEST_B0 INT64_C(100000)

static pg_now test_now(int64_t wall_s, int64_t boot_s, int32_t tz_minutes)
{
    pg_now now;
    size_t index;

    memset(&now, 0, sizeof now);
    now.wall_s = wall_s;
    now.boot_s = boot_s;
    now.tz_offset_minutes = tz_minutes;
    for (index = 0u; index < (size_t)PG_BOOT_ID_BYTES; ++index) {
        now.boot_id[index] = (uint8_t)(0x40u + index);
    }
    return now;
}

/* A plant with history: two species on the shelf, a fortnight of weather, a
 * journal that has wrapped, damage on the leaves and a soak still pending. A
 * round trip that only ever sees a freshly potted seedling proves nothing. */
static void test_fixture(pg_state *state, uint64_t seed)
{
    pg_catchup_report report;
    uint32_t tick;

    pg_init(state, seed);
    state->plant_count = 2u;
    state->hemisphere = (uint8_t)PG_HEMISPHERE_SOUTHERN;
    pg_plant_init(&state->plants[0], (uint8_t)PG_SPECIES_POTHOS,
                  (uint8_t)PG_POT_CACHEPOT, (uint8_t)PG_SPOT_SILL_SOUTH,
                  PG_TEST_T0);
    pg_plant_init(&state->plants[1], (uint8_t)PG_SPECIES_PEACE_LILY,
                  (uint8_t)PG_POT_TERRACOTTA, (uint8_t)PG_SPOT_NORTH_SHELF,
                  PG_TEST_T0);
    (void)memcpy(state->plants[0].name, "Goldie", 7u);
    (void)memcpy(state->plants[1].name, "Nyx", 4u);
    (void)pg_advance(state, test_now(PG_TEST_T0, PG_TEST_B0, -330), &report);

    for (tick = 1u; tick <= 14u * 96u; ++tick) {
        int64_t at = (int64_t)tick * PG_CARE_TICK_SECONDS;

        (void)pg_advance(state, test_now(PG_TEST_T0 + at, PG_TEST_B0 + at,
                                         -330), &report);
        if (tick % 192u == 0u) {
            (void)pg_actions_apply(state, 0u, PG_VERB_WATER_THOROUGHLY, 0u,
                                   NULL);
            (void)pg_actions_apply(state, 1u, PG_VERB_WATER_SIP, 0u, NULL);
        }
        if (tick % 480u == 0u) {
            (void)pg_actions_apply(state, 1u, PG_VERB_FEED_HALF, 0u, NULL);
            (void)pg_actions_apply(state, 0u, PG_VERB_ROTATE, 0u, NULL);
        }
    }
    /* A timed action still in flight, because a soak that survives a crash is
     * the whole reason the deadline is in care time (D-089). */
    (void)pg_actions_apply(state, 0u, PG_VERB_BOTTOM_SOAK, 0u, NULL);
}

/* Encode, decode, re-encode: the bytes and the record must both come back. */
static bool round_trip(const pg_state *state, pg_test_report *report,
                       uint32_t *hash, size_t case_index)
{
    uint8_t first[PG_SAVE_MAX_PAYLOAD];
    uint8_t second[PG_SAVE_MAX_PAYLOAD];
    pg_state restored;
    size_t first_size = 0u;
    size_t second_size = 0u;

    memset(&restored, 0, sizeof restored);
    if (!pg_save_encode(state, first, sizeof first, &first_size)) {
        return test_fail(report, "encode refused a valid state", case_index);
    }
    if (!pg_save_decode(&restored, first, first_size)) {
        return test_fail(report, "decode refused its own bytes", case_index);
    }
    if (!pg_save_encode(&restored, second, sizeof second, &second_size)
        || second_size != first_size
        || memcmp(first, second, first_size) != 0) {
        return test_fail(report, "round trip is not canonical", case_index);
    }
    if (memcmp(restored.plants, state->plants, sizeof restored.plants) != 0
        || restored.clock.care_seconds_total != state->clock.care_seconds_total
        || restored.clock.care_residual_s != state->clock.care_residual_s
        || restored.anchor.last_wall_s != state->anchor.last_wall_s
        || restored.anchor.last_boot_s != state->anchor.last_boot_s
        || restored.anchor.established != state->anchor.established
        || restored.anchor.tz_offset_minutes
           != state->anchor.tz_offset_minutes
        || memcmp(restored.anchor.boot_id, state->anchor.boot_id,
                  (size_t)PG_BOOT_ID_BYTES) != 0
        || restored.first_run_wall_s != state->first_run_wall_s
        || restored.rng.state != state->rng.state
        || restored.weather_seed != state->weather_seed
        || restored.hemisphere != state->hemisphere
        || restored.time_scale != state->time_scale
        || restored.plant_count != state->plant_count
        || restored.clock_anomaly_count != state->clock_anomaly_count
        || restored.journal_head != state->journal_head
        || restored.journal_count != state->journal_count
        || memcmp(restored.journal, state->journal,
                  sizeof restored.journal) != 0) {
        return test_fail(report, "round trip lost a field", case_index);
    }
    report->round_trips += 1u;
    *hash = hash_u32(*hash, (uint32_t)first_size);
    *hash = hash_u32(*hash, kilixstate_crc32(first, first_size));
    return true;
}

/* The Milestone 4 equivalence assertion, run through the save format. The
 * simulation must not be able to tell that it was written to disk and read
 * back between two care ticks. */
static bool test_equivalence_through_the_save(pg_test_report *report,
                                              uint32_t *hash)
{
    uint8_t species;

    for (species = 0u; species < (uint8_t)PG_SPECIES_COUNT; ++species) {
        pg_state live;
        pg_state saved;
        pg_catchup_report catchup;
        uint8_t live_bytes[PG_SAVE_MAX_PAYLOAD];
        uint8_t saved_bytes[PG_SAVE_MAX_PAYLOAD];
        size_t live_size = 0u;
        size_t saved_size = 0u;
        uint32_t tick;

        pg_init(&live, UINT64_C(0x9e3779b97f4a7c15) + species);
        pg_plant_init(&live.plants[0], species,
                      (uint8_t)(species % (uint8_t)PG_POT_COUNT),
                      (uint8_t)(species % (uint8_t)PG_SPOT_COUNT), PG_TEST_T0);
        (void)pg_advance(&live, test_now(PG_TEST_T0, PG_TEST_B0, 60),
                         &catchup);
        saved = live;

        for (tick = 1u; tick <= 3u * 96u; ++tick) {
            int64_t at = (int64_t)tick * PG_CARE_TICK_SECONDS;
            pg_state reloaded;
            uint8_t bytes[PG_SAVE_MAX_PAYLOAD];
            size_t size = 0u;

            (void)pg_advance(&live, test_now(PG_TEST_T0 + at,
                                             PG_TEST_B0 + at, 60), &catchup);
            (void)pg_advance(&saved, test_now(PG_TEST_T0 + at,
                                              PG_TEST_B0 + at, 60), &catchup);
            /* Close the lid and open it again, every single tick. */
            memset(&reloaded, 0, sizeof reloaded);
            if (!pg_save_encode(&saved, bytes, sizeof bytes, &size)
                || !pg_save_decode(&reloaded, bytes, size)) {
                return test_fail(report, "a mid-run save did not round trip",
                                 (size_t)tick);
            }
            saved = reloaded;
        }
        if (!pg_save_encode(&live, live_bytes, sizeof live_bytes, &live_size)
            || !pg_save_encode(&saved, saved_bytes, sizeof saved_bytes,
                               &saved_size)
            || live_size != saved_size
            || memcmp(live_bytes, saved_bytes, live_size) != 0) {
            return test_fail(report, "the saved path diverged from the live "
                                     "one", (size_t)species);
        }
        *hash = hash_u32(*hash, kilixstate_crc32(live_bytes, live_size));
        report->round_trips += 1u;
    }
    return true;
}

/* ---- the in-memory mutation sweeps --------------------------------------- */

static bool state_unchanged(const pg_state *state, const pg_state *sentinel)
{
    return memcmp(state, sentinel, sizeof *state) == 0;
}

/* An accepted mutation must still be a state the game can keep: it re-encodes
 * to itself and passes validation. Anything else means the decoder let a
 * corrupt record through. */
static bool accepted_is_canonical(const pg_state *state, uint32_t *checksum)
{
    uint8_t first[PG_SAVE_MAX_PAYLOAD];
    uint8_t second[PG_SAVE_MAX_PAYLOAD];
    pg_state restored;
    size_t first_size = 0u;
    size_t second_size = 0u;

    memset(&restored, 0, sizeof restored);
    if (!pg_validate(state, NULL, 0u)
        || !pg_save_encode(state, first, sizeof first, &first_size)
        || !pg_save_decode(&restored, first, first_size)
        || !pg_save_encode(&restored, second, sizeof second, &second_size)
        || first_size != second_size
        || memcmp(first, second, first_size) != 0) {
        return false;
    }
    if (checksum != NULL) {
        *checksum = kilixstate_crc32(first, first_size);
    }
    return true;
}

static bool payload_mutation_sweep(const uint8_t *baseline, size_t size,
                                   const pg_state *sentinel,
                                   pg_test_report *report, uint32_t *hash)
{
    uint8_t mutation[PG_SAVE_MAX_PAYLOAD];
    uint32_t random = PG_TEST_MUTATION_SEED;
    const uint32_t bit_count_total = (uint32_t)size * 8u;
    uint32_t index;

    for (index = 0u; index < bit_count_total * 2u; ++index) {
        uint32_t bits[4] = { 0u, 0u, 0u, 0u };
        uint32_t bit_count = 1u;
        uint32_t bit;
        uint32_t checksum = 0u;
        pg_state destination;
        bool accepted;

        memcpy(&destination, sentinel, sizeof destination);
        memcpy(mutation, baseline, size);
        bits[0] = index % bit_count_total;
        /* The first pass is every single-bit flip. The second is deterministic
         * multi-bit flips with duplicate rejection, because a CRC-free payload
         * decoder that survives one flipped bit can still fall over on two. */
        if (index >= bit_count_total) {
            bit_count += 1u + index % 3u;
        }
        for (bit = 1u; bit < bit_count; ++bit) {
            bool duplicate;

            do {
                uint32_t prior;

                bits[bit] = next_random(&random) % bit_count_total;
                duplicate = false;
                for (prior = 0u; prior < bit; ++prior) {
                    if (bits[prior] == bits[bit]) {
                        duplicate = true;
                    }
                }
            } while (duplicate);
        }
        for (bit = 0u; bit < bit_count; ++bit) {
            mutation[bits[bit] / 8u] ^= (uint8_t)(1u << (bits[bit] % 8u));
        }

        accepted = pg_save_decode(&destination, mutation, size);
        report->payload_mutations += 1u;
        if (accepted) {
            if (!accepted_is_canonical(&destination, &checksum)) {
                return test_fail(report, "an accepted mutation is not a state "
                                         "the game can keep", (size_t)index);
            }
            report->payload_accepted += 1u;
        } else {
            if (!state_unchanged(&destination, sentinel)) {
                return test_fail(report, "a rejected mutation changed the "
                                         "destination", (size_t)index);
            }
            report->payload_rejected += 1u;
        }
        *hash = hash_u32(*hash, index);
        *hash = hash_u32(*hash, accepted ? 1u : 0u);
        *hash = hash_u32(*hash, checksum);
    }
    return true;
}

/* Every truncation length, plus one byte too many. Exactly one length may be
 * accepted, and it is the one we wrote. */
static bool truncation_sweep(const uint8_t *baseline, size_t size,
                             const pg_state *sentinel,
                             pg_test_report *report, uint32_t *hash)
{
    uint8_t extended[PG_SAVE_MAX_PAYLOAD + 1u];
    uint32_t accepted_count = 0u;
    size_t length;
    pg_state destination;

    memcpy(&destination, sentinel, sizeof destination);
    memcpy(extended, baseline, size);
    extended[size] = 0u;
    /* The NULL contract belongs in the same sweep: it is the same promise. */
    if (pg_save_decode(NULL, baseline, size)
        || pg_save_decode(&destination, NULL, size)
        || !state_unchanged(&destination, sentinel)) {
        return test_fail(report, "the NULL decode contract differs", 0u);
    }
    for (length = 0u; length <= size + 1u; ++length) {
        uint32_t checksum = 0u;
        bool accepted;

        memcpy(&destination, sentinel, sizeof destination);
        accepted = pg_save_decode(&destination, extended, length);
        report->length_cases += 1u;
        if (accepted) {
            if (length != size
                || !accepted_is_canonical(&destination, &checksum)) {
                return test_fail(report, "a truncation was accepted", length);
            }
            accepted_count += 1u;
        } else if (!state_unchanged(&destination, sentinel)) {
            return test_fail(report, "a rejected truncation changed the "
                                     "destination", length);
        }
        *hash = hash_u32(*hash, (uint32_t)length);
        *hash = hash_u32(*hash, accepted ? 1u : 0u);
        *hash = hash_u32(*hash, checksum);
    }
    if (accepted_count != 1u) {
        return test_fail(report, "the truncation sweep accepted more than the "
                                 "record", (size_t)accepted_count);
    }
    return true;
}

static void put_u32_le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
    bytes[2] = (uint8_t)((value >> 16) & 0xffu);
    bytes[3] = (uint8_t)((value >> 24) & 0xffu);
}

static void put_u16_le(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
}

/* The length fields are the one part of a TLV envelope that can make a decoder
 * walk off the end of a buffer, so they get their own sweep rather than
 * trusting the bit flips to find them. Offset 18 is the first section header:
 * 4 version + 4 magic + 2 count + 8 sequence. */
#define PG_TEST_FIRST_SECTION 18u

static bool length_field_sweep(const uint8_t *baseline, size_t size,
                               const pg_state *sentinel,
                               pg_test_report *report, uint32_t *hash)
{
    static const uint32_t lengths[8] = {
        0u, 1u, 83u, 85u, 0xffffu, 0x7fffffffu, 0xfffffffeu, 0xffffffffu
    };
    uint8_t mutation[PG_SAVE_MAX_PAYLOAD];
    size_t index;

    for (index = 0u; index < sizeof lengths / sizeof lengths[0]; ++index) {
        pg_state destination;

        memcpy(&destination, sentinel, sizeof destination);
        memcpy(mutation, baseline, size);
        put_u32_le(mutation + PG_TEST_FIRST_SECTION + 4u, lengths[index]);
        report->length_field_cases += 1u;
        if (pg_save_decode(&destination, mutation, size)) {
            return test_fail(report, "a mutated section length was accepted",
                             index);
        }
        if (!state_unchanged(&destination, sentinel)) {
            return test_fail(report, "a mutated section length changed the "
                                     "destination", index);
        }
        *hash = hash_u32(*hash, lengths[index]);
    }
    return true;
}

/* The semantic cases: things that are perfectly well-formed bytes and still
 * must not be accepted. */
static bool semantic_sweep(const uint8_t *baseline, size_t size,
                           const pg_state *sentinel, pg_test_report *report,
                           uint32_t *hash)
{
    uint8_t mutation[PG_SAVE_MAX_PAYLOAD + PG_SAVE_HDR_BYTES + 8u];
    pg_state destination;
    size_t index;

    for (index = 0u; index < 6u; ++index) {
        size_t mutated_size = size;

        memcpy(&destination, sentinel, sizeof destination);
        memcpy(mutation, baseline, size);
        switch (index) {
        case 0u:
            /* A version this binary does not know. Read-only, never rewritten. */
            put_u32_le(mutation, PG_SAVE_VERSION + 1u);
            break;
        case 1u:
            /* The envelope sequence and the HDR sequence must agree. */
            mutation[10] = (uint8_t)(mutation[10] ^ 0x01u);
            break;
        case 2u:
            /* A duplicated known tag: reject, never last-one-wins. */
            put_u16_le(mutation + PG_TEST_FIRST_SECTION
                       + 8u + PG_SAVE_HDR_BYTES,
                       (uint16_t)PG_SAVE_SECTION_HDR);
            break;
        case 3u:
            /* A missing required section: the count says two, the second one
             * is now an unknown optional tag. */
            put_u16_le(mutation + PG_TEST_FIRST_SECTION, 0x7f00u);
            put_u16_le(mutation + PG_TEST_FIRST_SECTION + 2u, 0u);
            break;
        case 4u:
            /* Wrong magic. */
            mutation[4] = (uint8_t)'X';
            break;
        default:
            /* Trailing rubbish after the last section. */
            mutation[size] = 0xa5u;
            mutated_size = size + 1u;
            break;
        }
        report->semantic_cases += 1u;
        if (pg_save_decode(&destination, mutation, mutated_size)) {
            return test_fail(report, "a semantic case was accepted", index);
        }
        if (!state_unchanged(&destination, sentinel)) {
            return test_fail(report, "a semantic case changed the "
                                     "destination", index);
        }
        if (index == 0u
            && pg_save_last_reject()
               != (uint8_t)PG_SAVE_REJECT_NEWER_VERSION) {
            return test_fail(report, "a newer version was not reported as "
                                     "one", index);
        }
        *hash = hash_u32(*hash, (uint32_t)index);
        *hash = hash_u32(*hash, (uint32_t)pg_save_last_reject());
    }
    return true;
}

/* ---- the durable sweeps -------------------------------------------------- */

static bool write_file(const char *path, const uint8_t *bytes, size_t count)
{
    size_t offset = 0u;
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC
                                | O_NOFOLLOW, 0600);

    if (descriptor < 0) {
        return false;
    }
    while (offset < count) {
        ssize_t written = write(descriptor, bytes + offset, count - offset);

        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            (void)close(descriptor);
            return false;
        }
        offset += (size_t)written;
    }
    return close(descriptor) == 0;
}

static bool read_file(const char *path, uint8_t *bytes, size_t capacity,
                      size_t *size)
{
    struct stat status;
    size_t offset = 0u;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    *size = 0u;
    if (descriptor < 0) {
        return false;
    }
    if (fstat(descriptor, &status) != 0 || status.st_size < 0
        || (size_t)status.st_size > capacity) {
        (void)close(descriptor);
        return false;
    }
    while (offset < (size_t)status.st_size) {
        ssize_t received = read(descriptor, bytes + offset,
                                (size_t)status.st_size - offset);

        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            (void)close(descriptor);
            return false;
        }
        offset += (size_t)received;
    }
    *size = offset;
    return close(descriptor) == 0;
}

/* A load that must fail, through the real entry point, leaving the caller's
 * state exactly as it was. */
static bool durable_reject(pg_store *store, const pg_state *sentinel,
                           uint8_t expected_status, size_t case_index,
                           const char *label, pg_test_report *report)
{
    pg_state destination;
    bool recovered = true;

    memcpy(&destination, sentinel, sizeof destination);
    if (pg_store_load(&destination, store, &recovered)) {
        return test_fail(report, label, case_index);
    }
    if (pg_store_last_status(store) != expected_status) {
        return test_fail(report, label, case_index);
    }
    if (recovered || !state_unchanged(&destination, sentinel)) {
        return test_fail(report, label, case_index);
    }
    return true;
}

static bool durable_sweeps(const char *directory, const pg_state *expected,
                           const pg_state *sentinel, pg_test_report *report,
                           uint32_t *hash)
{
    uint8_t record[PG_SAVE_MAX_PAYLOAD + PG_TEST_RECORD_HEADER_BYTES];
    uint8_t mutation[PG_SAVE_MAX_PAYLOAD + PG_TEST_RECORD_HEADER_BYTES + 1u];
    char path[KILIXSTATE_PATH_CAPACITY];
    pg_store store;
    pg_state loaded;
    size_t record_size = 0u;
    size_t index;
    bool recovered = false;
    bool valid = false;

    if (!pg_store_open(&store, directory)) {
        return test_fail(report, "the store would not open", 0u);
    }

    /* First run: both generations absent, nothing published, and the state the
     * caller brought is untouched. */
    if (!durable_reject(&store, sentinel, (uint8_t)PG_STORE_FIRST_RUN, 0u,
                        "first run did not report itself", report)) {
        goto cleanup;
    }

    /* Two saves: A, then B, and the newer sequence wins. */
    if (!pg_store_save(expected, &store) || pg_store_generation(&store) != 0u) {
        (void)test_fail(report, "the first save did not land in generation A",
                        0u);
        goto cleanup;
    }
    if (!pg_store_save(expected, &store) || pg_store_generation(&store) != 1u) {
        (void)test_fail(report, "the second save did not alternate", 0u);
        goto cleanup;
    }
    memcpy(&loaded, sentinel, sizeof loaded);
    if (!pg_store_load(&loaded, &store, &recovered) || recovered
        || pg_store_last_status(&store) != (uint8_t)PG_STORE_READY
        || pg_store_generation(&store) != 1u
        || memcmp(loaded.plants, expected->plants,
                  sizeof loaded.plants) != 0) {
        (void)test_fail(report, "a clean pair did not load the newer record",
                        0u);
        goto cleanup;
    }
    report->round_trips += 1u;

    if (kilixstate_store_path(&store.plant[1], path, sizeof path)
        != KILIXSTATE_OK
        || !read_file(path, record, sizeof record, &record_size)
        || record_size < PG_TEST_RECORD_HEADER_BYTES
        || memcmp(record, "KST1", 4u) != 0) {
        (void)test_fail(report, "the durable record is not the one kilix-state"
                                " documents", 0u);
        goto cleanup;
    }

    /* A crash mid-write: the newer generation is half a file. The pair must
     * recover the older record, say so, and aim the next write at the
     * generation that failed. */
    if (!write_file(path, record, record_size / 2u)) {
        (void)test_fail(report, "the crash fixture could not be written", 0u);
        goto cleanup;
    }
    memcpy(&loaded, sentinel, sizeof loaded);
    if (!pg_store_load(&loaded, &store, &recovered) || !recovered
        || pg_store_last_status(&store) != (uint8_t)PG_STORE_RECOVERED
        || pg_store_generation(&store) != 0u
        || memcmp(loaded.plants, expected->plants,
                  sizeof loaded.plants) != 0) {
        (void)test_fail(report, "a crash mid-write lost the plant", 0u);
        goto cleanup;
    }
    if (!pg_store_save(expected, &store) || pg_store_generation(&store) != 1u) {
        (void)test_fail(report, "the repair did not overwrite the damaged "
                                "generation", 0u);
        goto cleanup;
    }
    report->round_trips += 1u;

    /* Both generations damaged: never a silent wipe. */
    {
        char other[KILIXSTATE_PATH_CAPACITY];

        if (kilixstate_store_path(&store.plant[0], other, sizeof other)
            != KILIXSTATE_OK
            || !write_file(other, record, 3u)
            || !write_file(path, record, record_size - 1u)) {
            (void)test_fail(report, "the damaged fixture could not be "
                                    "written", 0u);
            goto cleanup;
        }
        if (!durable_reject(&store, sentinel, (uint8_t)PG_STORE_DAMAGED, 0u,
                            "two damaged generations were not reported",
                            report)) {
            goto cleanup;
        }
    }

    /* A save from a future version: read-only, and the file is not touched. */
    {
        uint8_t future[PG_SAVE_MAX_PAYLOAD];
        size_t future_size = 0u;
        char other[KILIXSTATE_PATH_CAPACITY];

        if (!pg_save_encode(expected, future, sizeof future, &future_size)) {
            (void)test_fail(report, "the future fixture could not be built",
                            0u);
            goto cleanup;
        }
        put_u32_le(future, PG_SAVE_VERSION + 1u);
        if (kilixstate_store_path(&store.plant[0], other, sizeof other)
            != KILIXSTATE_OK) {
            (void)test_fail(report, "the store path is unavailable", 0u);
            goto cleanup;
        }
        pg_store_close(&store);
        if (!pg_store_open(&store, directory)) {
            (void)test_fail(report, "the store would not reopen", 0u);
            goto cleanup;
        }
        if (kilixstate_save(&store.plant[0], future, future_size)
                != KILIXSTATE_OK
            || kilixstate_save(&store.plant[1], future, future_size)
               != KILIXSTATE_OK) {
            (void)test_fail(report, "the future fixture could not be saved",
                            0u);
            goto cleanup;
        }
        if (!durable_reject(&store, sentinel,
                            (uint8_t)PG_STORE_NEWER_VERSION, 0u,
                            "a newer version was not refused", report)) {
            goto cleanup;
        }
        if (pg_store_save(expected, &store)
            || pg_store_last_status(&store) != (uint8_t)PG_STORE_NEWER_VERSION
            || !pg_store_save_failed(&store)) {
            (void)test_fail(report, "an old binary overwrote a newer save",
                            0u);
            goto cleanup;
        }
        {
            uint8_t after[PG_SAVE_MAX_PAYLOAD
                          + PG_TEST_RECORD_HEADER_BYTES];
            size_t after_size = 0u;

            if (!read_file(other, after, sizeof after, &after_size)
                || after_size != future_size + PG_TEST_RECORD_HEADER_BYTES) {
                (void)test_fail(report, "the newer record changed size", 0u);
                goto cleanup;
            }
        }
    }

    /* A second window: the sequence on disk moved underneath us. */
    pg_store_close(&store);
    if (!pg_store_open(&store, directory)) {
        (void)test_fail(report, "the store would not reopen", 0u);
        goto cleanup;
    }
    if (kilixstate_remove(&store.plant[0]) != KILIXSTATE_OK
        || kilixstate_remove(&store.plant[1]) != KILIXSTATE_OK
        || !pg_store_save(expected, &store)) {
        (void)test_fail(report, "the conflict fixture could not be built", 0u);
        goto cleanup;
    }
    {
        uint8_t rival[PG_SAVE_MAX_PAYLOAD];
        size_t rival_size = 0u;

        if (!pg_save_encode_with_sequence(expected, store.sequence + 100u,
                                          rival, sizeof rival, &rival_size)
            || kilixstate_save(&store.plant[1], rival, rival_size)
               != KILIXSTATE_OK) {
            (void)test_fail(report, "the rival record could not be written",
                            0u);
            goto cleanup;
        }
        if (pg_store_save(expected, &store)
            || pg_store_last_status(&store) != (uint8_t)PG_STORE_CONFLICT) {
            (void)test_fail(report, "a second window was not detected", 0u);
            goto cleanup;
        }
    }

    /* A full disk, or a home that will not take ink. The save must not land,
     * the banner must be sticky, BOTH records on disk must be untouched, and
     * the next good save must land and clear the banner — the plant is still
     * there to be watered, which is the whole promise of the fail-soft path.
     * Driven through the write seam because neither real condition can be
     * reached from outside the process (pg_save.h). */
    pg_store_close(&store);
    if (!pg_store_open(&store, directory)) {
        (void)test_fail(report, "the store would not reopen", 0u);
        goto cleanup;
    }
    if (kilixstate_remove(&store.plant[0]) != KILIXSTATE_OK
        || kilixstate_remove(&store.plant[1]) != KILIXSTATE_OK
        || !pg_store_save(expected, &store)) {
        (void)test_fail(report, "the io-error fixture could not be built", 0u);
        goto cleanup;
    }
    {
        uint8_t before[2][PG_SAVE_MAX_PAYLOAD + PG_TEST_RECORD_HEADER_BYTES];
        uint8_t after[PG_SAVE_MAX_PAYLOAD + PG_TEST_RECORD_HEADER_BYTES];
        char record_path[2][KILIXSTATE_PATH_CAPACITY];
        size_t before_size[2] = { 0u, 0u };
        size_t after_size = 0u;
        bool present[2];
        size_t generation;

        for (generation = 0u; generation < 2u; ++generation) {
            if (kilixstate_store_path(&store.plant[generation],
                                      record_path[generation],
                                      sizeof record_path[generation])
                != KILIXSTATE_OK) {
                (void)test_fail(report, "a store path is unavailable",
                                generation);
                goto cleanup;
            }
            present[generation] = read_file(record_path[generation],
                                            before[generation],
                                            sizeof before[generation],
                                            &before_size[generation]);
        }

        pg_store_fail_next_writes(1u);
        if (pg_store_save(expected, &store)) {
            pg_store_fail_next_writes(0u);
            (void)test_fail(report, "a write that failed reported success",
                            0u);
            goto cleanup;
        }
        if (pg_store_last_status(&store) != (uint8_t)PG_STORE_IO_ERROR) {
            (void)test_fail(report,
                            "a failed write was not reported as an io error",
                            0u);
            goto cleanup;
        }
        if (!pg_store_save_failed(&store)) {
            (void)test_fail(report, "the failed-save banner is not sticky",
                            0u);
            goto cleanup;
        }
        for (generation = 0u; generation < 2u; ++generation) {
            bool now_present = read_file(record_path[generation], after,
                                         sizeof after, &after_size);
            if (now_present != present[generation]
                || (now_present
                    && (after_size != before_size[generation]
                        || memcmp(after, before[generation],
                                  after_size) != 0))) {
                (void)test_fail(report,
                                "a failed write changed the record on disk",
                                generation);
                goto cleanup;
            }
        }

        /* And the game carries on: the next write lands and the banner goes. */
        if (!pg_store_save(expected, &store)
            || pg_store_last_status(&store) != (uint8_t)PG_STORE_READY
            || pg_store_save_failed(&store)) {
            (void)test_fail(report,
                            "the save after an io error did not recover", 0u);
            goto cleanup;
        }
    }

    /* The sweeps. One generation only, so every case is decided by the record
     * under test and nothing else. */
    pg_store_close(&store);
    if (!pg_store_open(&store, directory)) {
        (void)test_fail(report, "the store would not reopen", 0u);
        goto cleanup;
    }
    if (kilixstate_remove(&store.plant[1]) != KILIXSTATE_OK
        || kilixstate_save(&store.plant[0], record
                           + PG_TEST_RECORD_HEADER_BYTES,
                           record_size - PG_TEST_RECORD_HEADER_BYTES)
           != KILIXSTATE_OK
        || kilixstate_store_path(&store.plant[0], path, sizeof path)
           != KILIXSTATE_OK
        || !read_file(path, record, sizeof record, &record_size)) {
        (void)test_fail(report, "the sweep fixture could not be built", 0u);
        goto cleanup;
    }

    for (index = 0u; index < record_size * 8u; ++index) {
        memcpy(mutation, record, record_size);
        mutation[index / 8u] ^= (uint8_t)(1u << (index % 8u));
        if (!write_file(path, mutation, record_size)
            || !durable_reject(&store, sentinel, (uint8_t)PG_STORE_DAMAGED,
                               index, "a flipped record bit was accepted",
                               report)) {
            goto cleanup;
        }
        report->record_bit_flips += 1u;
        *hash = hash_u32(*hash, (uint32_t)index);
    }
    for (index = 0u; index < record_size; ++index) {
        if (!write_file(path, record, index)
            || !durable_reject(&store, sentinel, (uint8_t)PG_STORE_DAMAGED,
                               index, "a truncated record was accepted",
                               report)) {
            goto cleanup;
        }
        report->record_truncations += 1u;
        *hash = hash_u32(*hash, (uint32_t)index);
    }
    memcpy(mutation, record, record_size);
    mutation[record_size] = 0xa5u;
    if (!write_file(path, mutation, record_size + 1u)
        || !durable_reject(&store, sentinel, (uint8_t)PG_STORE_DAMAGED, 0u,
                           "a record with a trailing byte was accepted",
                           report)) {
        goto cleanup;
    }
    report->oversize_records += 1u;

    /* And the record itself still loads, byte for byte, at the end of all
     * that. */
    memcpy(&loaded, sentinel, sizeof loaded);
    if (!write_file(path, record, record_size)
        || !pg_store_load(&loaded, &store, &recovered)
        || memcmp(loaded.plants, expected->plants,
                  sizeof loaded.plants) != 0) {
        (void)test_fail(report, "the intact record did not survive the sweep",
                        0u);
        goto cleanup;
    }
    report->round_trips += 1u;
    *hash = hash_u32(*hash, kilixstate_crc32(record, record_size));
    valid = true;

cleanup:
    pg_store_close(&store);
    return valid;
}

/* Settings are their own record and their own promise: never fails, always
 * plays. */
static bool settings_sweep(const char *directory, pg_test_report *report,
                           uint32_t *hash)
{
    pg_store store;
    pg_settings written;
    pg_settings restored;
    pg_settings defaults;
    uint8_t bytes[PG_SETTINGS_MAX_PAYLOAD];
    size_t size = 0u;
    size_t index;
    bool valid = false;

    pg_settings_defaults(&defaults);
    if (!pg_store_open(&store, directory)) {
        return test_fail(report, "the settings store would not open", 0u);
    }
    /* Absent is not a failure. */
    pg_settings_defaults(&written);
    if (!pg_settings_load(&restored, &store)
        || memcmp(&restored, &defaults, sizeof defaults) != 0) {
        (void)test_fail(report, "an absent settings record did not default",
                        0u);
        goto cleanup;
    }
    written.master_gain_l = 4321u;
    written.music_gain_l = 0u;
    written.sfx_gain_l = (uint16_t)PG_LEVEL_MAX;
    written.faces_enabled = false;
    written.backdrops_enabled = false;
    written.backdrop_scene_id = (uint8_t)PG_SCENE_STEAMY_BATH;
    if (!pg_settings_save(&written, &store)
        || !pg_settings_load(&restored, &store)
        || memcmp(&restored, &written, sizeof written) != 0) {
        (void)test_fail(report, "settings did not round trip", 0u);
        goto cleanup;
    }
    if (!pg_settings_encode(&written, bytes, sizeof bytes, &size)
        || size != (size_t)PG_SETTINGS_BYTES) {
        (void)test_fail(report, "the settings record changed size", size);
        goto cleanup;
    }
    /* Every single-bit flip either decodes to something sane or defaults, and
     * neither outcome may fail. */
    for (index = 0u; index < size * 8u; ++index) {
        uint8_t mutation[PG_SETTINGS_MAX_PAYLOAD];
        pg_settings decoded;

        memcpy(mutation, bytes, size);
        mutation[index / 8u] ^= (uint8_t)(1u << (index % 8u));
        pg_settings_defaults(&decoded);
        if (pg_settings_decode(&decoded, mutation, size)
            && (decoded.master_gain_l > (uint16_t)PG_LEVEL_MAX
                || decoded.music_gain_l > (uint16_t)PG_LEVEL_MAX
                || decoded.sfx_gain_l > (uint16_t)PG_LEVEL_MAX
                || decoded.backdrop_scene_id >= (uint8_t)PG_SCENE_COUNT)) {
            (void)test_fail(report, "a mutated setting decoded out of range",
                            index);
            goto cleanup;
        }
        *hash = hash_u32(*hash, (uint32_t)index);
    }
    /* And a settings store that was never opened still hands back defaults. */
    {
        pg_store closed;

        memset(&closed, 0, sizeof closed);
        if (!pg_settings_load(&restored, &closed)
            || memcmp(&restored, &defaults, sizeof defaults) != 0
            || pg_settings_save(&written, &closed)) {
            (void)test_fail(report, "the fail-soft settings contract differs",
                            0u);
            goto cleanup;
        }
    }
    valid = true;

cleanup:
    pg_store_close(&store);
    return valid;
}

int pg_save_run_test(const char *directory)
{
    pg_test_report report;
    pg_state expected;
    pg_state sentinel;
    uint8_t baseline[PG_SAVE_MAX_PAYLOAD];
    size_t baseline_size = 0u;
    uint32_t hash = PG_TEST_FNV_OFFSET;
    char error[128];

    memset(&report, 0, sizeof report);
    if (directory == NULL || directory[0] != '/') {
        (void)fprintf(stderr, "save-test: an absolute directory is required\n");
        return 2;
    }
    if (!pg_content_validate(error, sizeof error)) {
        (void)fprintf(stderr, "save-test: content invalid: %s\n", error);
        return 1;
    }
    (void)printf("save-test %s\n", PG_VERSION);

    test_fixture(&expected, UINT64_C(0x50414e54));
    /* The sentinel is a different plant entirely, so "the destination did not
     * change" is a claim with teeth. */
    test_fixture(&sentinel, UINT64_C(0x53454544));
    sentinel.plants[0].turgor = 1234u;

    if (!pg_save_encode(&expected, baseline, sizeof baseline, &baseline_size)) {
        (void)fprintf(stderr, "save-test: the fixture would not encode\n");
        return 1;
    }
    (void)printf("  record: version %u, %zu bytes at %u plants "
                 "(HDR %u, PLNT %u each, JRNL %u)\n",
                 (unsigned)PG_SAVE_VERSION, baseline_size,
                 (unsigned)expected.plant_count, (unsigned)PG_SAVE_HDR_BYTES,
                 (unsigned)PG_SAVE_PLNT_ENTRY_BYTES,
                 (unsigned)PG_SAVE_JRNL_BYTES);

    if (!round_trip(&expected, &report, &hash, 0u)
        || !test_equivalence_through_the_save(&report, &hash)
        || !payload_mutation_sweep(baseline, baseline_size, &sentinel,
                                   &report, &hash)
        || !truncation_sweep(baseline, baseline_size, &sentinel, &report,
                             &hash)
        || !length_field_sweep(baseline, baseline_size, &sentinel, &report,
                               &hash)
        || !semantic_sweep(baseline, baseline_size, &sentinel, &report, &hash)
        || !durable_sweeps(directory, &expected, &sentinel, &report, &hash)
        || !settings_sweep(directory, &report, &hash)) {
        (void)fprintf(stderr, "save-test: FAILED after %u failures\n",
                      report.failures);
        return 1;
    }

    report.hash = hash;
    (void)printf("  round trips        %u\n", report.round_trips);
    /* An accepted payload mutation is not a failure and never was: the
     * durable record's CRC is the integrity check, and what this sweep proves
     * is that anything the decoder accepts is still a plant the game can keep
     * and re-encode to itself. The record sweep below is where corruption is
     * caught. */
    (void)printf("  payload mutations  %u (accepted %u, rejected %u)\n",
                 report.payload_mutations, report.payload_accepted,
                 report.payload_rejected);
    (void)printf("  truncations        %u\n", report.length_cases);
    (void)printf("  length fields      %u\n", report.length_field_cases);
    (void)printf("  semantic cases     %u\n", report.semantic_cases);
    (void)printf("  record bit flips   %u\n", report.record_bit_flips);
    (void)printf("  record truncations %u\n", report.record_truncations);
    (void)printf("  trailing bytes     %u\n", report.oversize_records);
    (void)printf("  hash               %08" PRIx32 "\n", report.hash);
    (void)puts("save-test: PASS");
    return 0;
}
