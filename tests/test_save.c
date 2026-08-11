/*
 * test_save — persistence.
 *
 * `--save-test` is the sweep: every bit of the record, every truncation, every
 * semantic case. These are the assertions that must not be allowed to drift,
 * and four of them carry the milestone:
 *
 *   - **the record is ARCHITECTURE.md §6.2 in full.** Every field of HDR and of
 *     PLNT is written with a distinct value and read back and compared, one at
 *     a time. A field quietly dropped from the encoder is the one save bug that
 *     cannot be detected after the fact, so it is detected here.
 *   - **the wire layout is pinned by offset.** The version, the magic, the
 *     section count, the envelope sequence and the first three HDR fields are
 *     asserted at the byte offsets they occupy, so a reordered field fails a
 *     test rather than silently reinterpreting every existing save.
 *   - **save equals ticks, through the save file.** Milestone 4's equivalence
 *     assertion again, with an encode/decode round trip inserted between every
 *     care tick of one of the two paths.
 *   - **nothing accepted, nothing changed.** A corrupt, truncated, or
 *     future-version record never publishes anything into the caller's state.
 */
#include "pg_save.h"

#include "pg_actions.h"
#include "pg_content.h"
#include "pg_plant.h"
#include "pg_sim.h"
#include "pg_state.h"
#include "pg_time.h"

#include "kilix_state.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
                      __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

#define T0 INT64_C(1767225600)      /* 2026-01-01T00:00:00Z */
#define B0 INT64_C(100000)

static pg_now at(int64_t wall_s, int64_t boot_s, int32_t tz_minutes)
{
    pg_now now;
    size_t index;

    memset(&now, 0, sizeof now);
    now.wall_s = wall_s;
    now.boot_s = boot_s;
    now.tz_offset_minutes = tz_minutes;
    for (index = 0u; index < (size_t)PG_BOOT_ID_BYTES; ++index) {
        now.boot_id[index] = (uint8_t)(0x11u + index);
    }
    return now;
}

/* Two plants, a fortnight of weather, a wrapped journal and a soak in flight.
 * A round trip that only ever sees a fresh seedling proves nothing. */
static void fixture(pg_state *state, uint64_t seed)
{
    pg_catchup_report report;
    uint32_t tick;

    pg_init(state, seed);
    state->plant_count = 2u;
    pg_plant_init(&state->plants[0], (uint8_t)PG_SPECIES_POTHOS,
                  (uint8_t)PG_POT_CACHEPOT, (uint8_t)PG_SPOT_SILL_SOUTH, T0);
    pg_plant_init(&state->plants[1], (uint8_t)PG_SPECIES_CALATHEA,
                  (uint8_t)PG_POT_GLAZED, (uint8_t)PG_SPOT_BATH_SHELF, T0);
    (void)pg_advance(state, at(T0, B0, 120), &report);
    for (tick = 1u; tick <= 14u * 96u; ++tick) {
        int64_t offset = (int64_t)tick * PG_CARE_TICK_SECONDS;

        (void)pg_advance(state, at(T0 + offset, B0 + offset, 120), &report);
        if (tick % 160u == 0u) {
            (void)pg_actions_apply(state, 0u, PG_VERB_WATER_THOROUGHLY, 0u,
                                   NULL);
            (void)pg_actions_apply(state, 1u, PG_VERB_MIST, 0u, NULL);
        }
    }
    (void)pg_actions_apply(state, 1u, PG_VERB_BOTTOM_SOAK, 0u, NULL);
}

static uint16_t read_u16_le(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | (uint16_t)((uint16_t)bytes[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8)
         | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *bytes)
{
    return (uint64_t)read_u32_le(bytes)
         | ((uint64_t)read_u32_le(bytes + 4) << 32);
}

/* ---- the record itself --------------------------------------------------- */

/* Byte offsets, asserted rather than assumed. A field that moves breaks this
 * test, which is the entire point: every existing save is read at these
 * offsets forever. */
static bool test_the_wire_layout_is_where_it_says(void)
{
    pg_state state;
    uint8_t bytes[PG_SAVE_MAX_PAYLOAD];
    size_t size = 0u;

    fixture(&state, 0xa11ceu);
    CHECK(pg_save_encode(&state, bytes, sizeof bytes, &size));

    /* version, magic, section count, envelope sequence */
    CHECK(read_u32_le(bytes) == PG_SAVE_VERSION);
    CHECK(memcmp(bytes + 4, "PGRW", 4) == 0);
    CHECK(read_u16_le(bytes + 8) == 3u);          /* HDR, PLNT, JRNL */
    CHECK(read_u64_le(bytes + 10) == state.save_sequence);

    /* first section header: HDR, required, 84 bytes */
    CHECK(read_u16_le(bytes + 18) == (uint16_t)PG_SAVE_SECTION_HDR);
    CHECK(read_u16_le(bytes + 20) == (uint16_t)PG_SAVE_SECTION_REQUIRED);
    CHECK(read_u32_le(bytes + 22) == PG_SAVE_HDR_BYTES);

    /* HDR body: sequence, then the care clock, which is an HDR field and not a
     * per-plant one, and a later session must not move it. */
    CHECK(read_u64_le(bytes + 26) == state.save_sequence);
    CHECK(read_u64_le(bytes + 34) == state.clock.care_seconds_total);
    CHECK(read_u32_le(bytes + 42) == state.clock.care_residual_s);

    /* second section header: PLNT, required, one entry per plant */
    CHECK(read_u16_le(bytes + 26 + PG_SAVE_HDR_BYTES)
          == (uint16_t)PG_SAVE_SECTION_PLNT);
    CHECK(read_u32_le(bytes + 30 + PG_SAVE_HDR_BYTES)
          == (uint32_t)state.plant_count * PG_SAVE_PLNT_ENTRY_BYTES);

    /* and the whole thing is the size §6.2 predicted */
    CHECK(size == 18u + 8u + PG_SAVE_HDR_BYTES
                + 8u + 2u * PG_SAVE_PLNT_ENTRY_BYTES
                + 8u + PG_SAVE_JRNL_BYTES);
    CHECK(size <= PG_SAVE_MAX_PAYLOAD);
    return true;
}

/* Every field of §6.2, one at a time. */
static bool test_every_field_of_the_record_survives(void)
{
    pg_state state;
    pg_state restored;
    pg_plant *plant;
    uint8_t bytes[PG_SAVE_MAX_PAYLOAD];
    size_t size = 0u;
    size_t index;

    pg_init(&state, 0u);
    state.save_sequence = UINT64_C(0x0102030405060708);
    state.clock.care_seconds_total = 900u * 4321u + 777u;
    state.clock.care_residual_s = 777u;
    state.anchor.last_wall_s = INT64_C(-8640000000);
    state.anchor.last_boot_s = INT64_C(1234567);
    state.anchor.established = true;
    state.anchor.tz_offset_minutes = -330;
    state.first_run_wall_s = INT64_C(1767225600);
    state.clock_anomaly_count = 4242u;
    state.rng.state = UINT64_C(0xfeedfacecafebeef);
    state.weather_seed = UINT64_C(0x0badc0ffee0ddf00);
    state.hemisphere = (uint8_t)PG_HEMISPHERE_SOUTHERN;
    state.time_scale = 7u;
    state.plant_count = 2u;
    for (index = 0u; index < (size_t)PG_BOOT_ID_BYTES; ++index) {
        state.anchor.boot_id[index] = (uint8_t)(0xf0u - index);
    }

    for (index = 0u; index < 2u; ++index) {
        size_t inner;

        plant = &state.plants[index];
        pg_plant_init(plant, (uint8_t)PG_SPECIES_PEACE_LILY,
                      (uint8_t)PG_POT_NURSERY, (uint8_t)PG_SPOT_TWO_METRE, 0);
        plant->planted_wall_s = (index == 0u) ? INT64_MIN + 1 : INT64_MAX - 1;
        (void)snprintf(plant->name, sizeof plant->name, "plant-%u",
                       (unsigned)index);
        plant->species_id = (uint8_t)PG_SPECIES_SNAKE;
        plant->pot_id = (uint8_t)PG_POT_GLAZED;
        plant->scene_id = (uint8_t)PG_SCENE_KITCHEN_SHELF;
        plant->spot_id = (uint8_t)PG_SPOT_RADIATOR_SHELF;
        plant->moisture_top = 1111u;
        plant->moisture_bottom = 2222u;
        plant->light_debt = 3333u;
        plant->nutrition = 4444u;
        plant->salt = 5555u;
        plant->purity_load = 6666u;
        plant->root_capacity = 8888u;
        plant->root_health = 7777u;
        plant->root_bound = 9999u;
        plant->turgor = 10000u;
        plant->soggy_ticks = 123456u;
        plant->scorch_dose = 121u;
        plant->crisp_dose = 232u;
        plant->acclimation = 343u;
        plant->shock = 454u;
        plant->dli_today = 2999u;
        for (inner = 0u; inner < (size_t)PG_DLI_RING; ++inner) {
            plant->dli_ring[inner] = (uint16_t)(100u + inner);
        }
        plant->dli_ring_head = 13u;
        plant->pest_gnats = 61u;
        plant->pest_mites = 62u;
        plant->pest_mealy = 63u;
        plant->pest_egg_timer = 64u;
        plant->growth_stage = (uint8_t)PG_STAGE_SPECIMEN;
        plant->growth_points = 987654u;
        plant->life_state = (uint8_t)PG_LIFE_DORMANT;
        plant->spathe_state = (uint8_t)PG_SPATHE_OPEN;
        plant->flower_count = 3u;
        plant->leaf_count = (uint8_t)PG_LEAF_MAX;
        for (inner = 0u; inner < (size_t)PG_LEAF_MAX; ++inner) {
            plant->leaves[inner].birth_care_day = (uint16_t)(500u + inner);
            plant->leaves[inner].slot = (uint8_t)inner;
            plant->leaves[inner].form_flags = (uint8_t)(0x81u + inner);
            plant->leaves[inner].damage_mask = (uint8_t)(1u + inner);
            plant->leaves[inner].pose = (uint8_t)(9u + inner);
        }
        plant->vine_count = (uint8_t)PG_VINE_MAX;
        for (inner = 0u; inner < (size_t)PG_VINE_MAX; ++inner) {
            plant->vine[inner].internode = (uint8_t)(3u + inner);
            plant->vine[inner].leaf_form = (uint8_t)(40u + inner);
            plant->vine[inner].damage_mask = (uint8_t)(0x40u + inner);
        }
        plant->last_watered_care_s = UINT64_C(1000000000001);
        plant->last_fed_care_s = UINT64_C(1000000000002);
        plant->last_repotted_care_s = UINT64_C(1000000000003);
        plant->last_rotated_care_s = UINT64_C(1000000000004);
        plant->last_drained_care_s = UINT64_C(1000000000005);
        plant->pending_action = (uint8_t)PG_PENDING_FLUSH;
        plant->pending_action_care_s = UINT64_C(1000000000006);
        plant->streak_on_time = 31u;
        plant->missed_events = 17u;
    }
    /* A journal with a wrapped ring, because past-day marks survive a crash. */
    state.journal_head = 7u;
    state.journal_count = (uint8_t)PG_JOURNAL_RING;
    for (index = 0u; index < (size_t)PG_JOURNAL_RING; ++index) {
        state.journal[index].care_day = (uint32_t)(1000u + index);
        state.journal[index].kind = (uint8_t)(1u + index % 20u);
        state.journal[index].detail = (uint8_t)(index % 24u);
        state.journal[index].plant_index = (uint8_t)(index % 2u);
    }

    CHECK(pg_validate(&state, NULL, 0u));
    memset(&restored, 0, sizeof restored);
    CHECK(pg_save_encode(&state, bytes, sizeof bytes, &size));
    CHECK(pg_save_decode(&restored, bytes, size));

    CHECK(restored.save_sequence == state.save_sequence);
    CHECK(restored.clock.care_seconds_total == state.clock.care_seconds_total);
    CHECK(restored.clock.care_residual_s == state.clock.care_residual_s);
    CHECK(restored.anchor.last_wall_s == state.anchor.last_wall_s);
    CHECK(restored.anchor.last_boot_s == state.anchor.last_boot_s);
    CHECK(restored.anchor.established == state.anchor.established);
    CHECK(restored.anchor.tz_offset_minutes == state.anchor.tz_offset_minutes);
    CHECK(memcmp(restored.anchor.boot_id, state.anchor.boot_id,
                 (size_t)PG_BOOT_ID_BYTES) == 0);
    CHECK(restored.first_run_wall_s == state.first_run_wall_s);
    CHECK(restored.clock_anomaly_count == state.clock_anomaly_count);
    CHECK(restored.rng.state == state.rng.state);
    CHECK(restored.weather_seed == state.weather_seed);
    CHECK(restored.hemisphere == state.hemisphere);
    CHECK(restored.time_scale == state.time_scale);
    CHECK(restored.plant_count == state.plant_count);
    CHECK(restored.journal_head == state.journal_head);
    CHECK(restored.journal_count == state.journal_count);
    CHECK(memcmp(restored.journal, state.journal, sizeof state.journal) == 0);
    CHECK(memcmp(restored.plants, state.plants, sizeof state.plants) == 0);

    /* Field by field on the plants as well, so that a memcmp passing on
     * padding cannot hide a dropped field. */
    for (index = 0u; index < 2u; ++index) {
        const pg_plant *a = &state.plants[index];
        const pg_plant *b = &restored.plants[index];
        size_t inner;

        CHECK(a->planted_wall_s == b->planted_wall_s);
        CHECK(memcmp(a->name, b->name, sizeof a->name) == 0);
        CHECK(a->species_id == b->species_id && a->pot_id == b->pot_id);
        CHECK(a->scene_id == b->scene_id && a->spot_id == b->spot_id);
        CHECK(a->moisture_top == b->moisture_top);
        CHECK(a->moisture_bottom == b->moisture_bottom);
        CHECK(a->light_debt == b->light_debt);
        CHECK(a->nutrition == b->nutrition && a->salt == b->salt);
        CHECK(a->purity_load == b->purity_load);
        CHECK(a->root_health == b->root_health);
        CHECK(a->root_capacity == b->root_capacity);
        CHECK(a->root_bound == b->root_bound && a->turgor == b->turgor);
        CHECK(a->soggy_ticks == b->soggy_ticks);
        CHECK(a->scorch_dose == b->scorch_dose);
        CHECK(a->crisp_dose == b->crisp_dose);
        CHECK(a->acclimation == b->acclimation && a->shock == b->shock);
        CHECK(a->dli_today == b->dli_today);
        CHECK(a->dli_ring_head == b->dli_ring_head);
        for (inner = 0u; inner < (size_t)PG_DLI_RING; ++inner) {
            CHECK(a->dli_ring[inner] == b->dli_ring[inner]);
        }
        CHECK(a->pest_gnats == b->pest_gnats && a->pest_mites == b->pest_mites);
        CHECK(a->pest_mealy == b->pest_mealy);
        CHECK(a->pest_egg_timer == b->pest_egg_timer);
        CHECK(a->growth_stage == b->growth_stage);
        CHECK(a->growth_points == b->growth_points);
        CHECK(a->life_state == b->life_state);
        CHECK(a->spathe_state == b->spathe_state);
        CHECK(a->flower_count == b->flower_count);
        CHECK(a->leaf_count == b->leaf_count);
        for (inner = 0u; inner < (size_t)PG_LEAF_MAX; ++inner) {
            CHECK(a->leaves[inner].birth_care_day
                  == b->leaves[inner].birth_care_day);
            CHECK(a->leaves[inner].slot == b->leaves[inner].slot);
            CHECK(a->leaves[inner].form_flags == b->leaves[inner].form_flags);
            CHECK(a->leaves[inner].damage_mask
                  == b->leaves[inner].damage_mask);
            CHECK(a->leaves[inner].pose == b->leaves[inner].pose);
        }
        CHECK(a->vine_count == b->vine_count);
        for (inner = 0u; inner < (size_t)PG_VINE_MAX; ++inner) {
            CHECK(a->vine[inner].internode == b->vine[inner].internode);
            CHECK(a->vine[inner].leaf_form == b->vine[inner].leaf_form);
            CHECK(a->vine[inner].damage_mask == b->vine[inner].damage_mask);
        }
        CHECK(a->last_watered_care_s == b->last_watered_care_s);
        CHECK(a->last_fed_care_s == b->last_fed_care_s);
        CHECK(a->last_repotted_care_s == b->last_repotted_care_s);
        CHECK(a->last_rotated_care_s == b->last_rotated_care_s);
        CHECK(a->last_drained_care_s == b->last_drained_care_s);
        CHECK(a->pending_action == b->pending_action);
        CHECK(a->pending_action_care_s == b->pending_action_care_s);
        CHECK(a->streak_on_time == b->streak_on_time);
        CHECK(a->missed_events == b->missed_events);
    }
    return true;
}

/* The codec has no i64, so epoch seconds have an explicit encoding, and the
 * whole range of one has to survive it. */
static bool test_epoch_seconds_survive_the_whole_range(void)
{
    static const int64_t cases[8] = {
        0, 1, -1, INT64_C(1767225600), INT64_C(-1767225600),
        INT64_MAX, INT64_MIN + 1, INT64_MIN
    };
    size_t index;

    for (index = 0u; index < sizeof cases / sizeof cases[0]; ++index) {
        pg_state state;
        pg_state restored;
        uint8_t bytes[PG_SAVE_MAX_PAYLOAD];
        size_t size = 0u;

        pg_init(&state, 1u);
        state.anchor.last_wall_s = cases[index];
        state.anchor.last_boot_s = cases[(index + 3u) % 8u];
        state.first_run_wall_s = cases[(index + 5u) % 8u];
        state.plants[0].planted_wall_s = cases[(index + 1u) % 8u];
        memset(&restored, 0, sizeof restored);
        CHECK(pg_save_encode(&state, bytes, sizeof bytes, &size));
        CHECK(pg_save_decode(&restored, bytes, size));
        CHECK(restored.anchor.last_wall_s == state.anchor.last_wall_s);
        CHECK(restored.anchor.last_boot_s == state.anchor.last_boot_s);
        CHECK(restored.first_run_wall_s == state.first_run_wall_s);
        CHECK(restored.plants[0].planted_wall_s
              == state.plants[0].planted_wall_s);
    }
    return true;
}

/* tz_offset_minutes goes through u16 because the codec has no i16. The sign
 * has to come back, and anything outside [-720, +840] is 0 (D-079). */
static bool test_the_local_offset_comes_back_signed(void)
{
    static const int32_t cases[6] = { 0, 60, -330, 840, -720, 120 };
    size_t index;

    for (index = 0u; index < sizeof cases / sizeof cases[0]; ++index) {
        pg_state state;
        pg_state restored;
        uint8_t bytes[PG_SAVE_MAX_PAYLOAD];
        size_t size = 0u;

        pg_init(&state, 1u);
        state.anchor.tz_offset_minutes = cases[index];
        memset(&restored, 0, sizeof restored);
        CHECK(pg_save_encode(&state, bytes, sizeof bytes, &size));
        CHECK(pg_save_decode(&restored, bytes, size));
        CHECK(restored.anchor.tz_offset_minutes == cases[index]);
    }
    /* An out-of-range offset in the bytes decodes to 0 rather than being
     * refused: a plant is not lost to a bad clock. */
    {
        pg_state state;
        pg_state restored;
        uint8_t bytes[PG_SAVE_MAX_PAYLOAD];
        size_t size = 0u;
        size_t offset = 26u + 8u + 8u + 4u + 8u + 8u + 8u
                      + (size_t)PG_BOOT_ID_BYTES + 1u;

        pg_init(&state, 1u);
        state.anchor.tz_offset_minutes = 60;
        CHECK(pg_save_encode(&state, bytes, sizeof bytes, &size));
        CHECK(read_u16_le(bytes + offset) == 60u);
        bytes[offset] = 0x00u;
        bytes[offset + 1u] = 0x7fu;         /* +32512 minutes */
        memset(&restored, 0, sizeof restored);
        CHECK(pg_save_decode(&restored, bytes, size));
        CHECK(restored.anchor.tz_offset_minutes == 0);
    }
    return true;
}

/* Milestone 4's equivalence assertion, through the save format. */
static bool test_save_equals_ticks_through_the_save(void)
{
    uint8_t species;

    for (species = 0u; species < (uint8_t)PG_SPECIES_COUNT; ++species) {
        pg_state live;
        pg_state saved;
        pg_catchup_report report;
        uint8_t live_bytes[PG_SAVE_MAX_PAYLOAD];
        uint8_t saved_bytes[PG_SAVE_MAX_PAYLOAD];
        size_t live_size = 0u;
        size_t saved_size = 0u;
        uint32_t tick;

        pg_init(&live, 0x5eed0000u + species);
        pg_plant_init(&live.plants[0], species,
                      (uint8_t)(species % (uint8_t)PG_POT_COUNT),
                      (uint8_t)(species % (uint8_t)PG_SPOT_COUNT), T0);
        (void)pg_advance(&live, at(T0, B0, -60), &report);
        saved = live;

        for (tick = 1u; tick <= 2u * 96u; ++tick) {
            int64_t offset = (int64_t)tick * PG_CARE_TICK_SECONDS;
            pg_state reloaded;
            uint8_t bytes[PG_SAVE_MAX_PAYLOAD];
            size_t size = 0u;

            (void)pg_advance(&live, at(T0 + offset, B0 + offset, -60),
                             &report);
            (void)pg_advance(&saved, at(T0 + offset, B0 + offset, -60),
                             &report);
            memset(&reloaded, 0, sizeof reloaded);
            CHECK(pg_save_encode(&saved, bytes, sizeof bytes, &size));
            CHECK(pg_save_decode(&reloaded, bytes, size));
            saved = reloaded;
        }
        CHECK(pg_save_encode(&live, live_bytes, sizeof live_bytes,
                             &live_size));
        CHECK(pg_save_encode(&saved, saved_bytes, sizeof saved_bytes,
                             &saved_size));
        CHECK(live_size == saved_size);
        CHECK(memcmp(live_bytes, saved_bytes, live_size) == 0);
        CHECK(memcmp(live.plants, saved.plants, sizeof live.plants) == 0);
    }
    return true;
}

/* ---- what must be refused ------------------------------------------------ */

static bool test_a_corrupt_or_truncated_record_changes_nothing(void)
{
    pg_state state;
    pg_state sentinel;
    pg_state destination;
    uint8_t bytes[PG_SAVE_MAX_PAYLOAD];
    uint8_t mutation[PG_SAVE_MAX_PAYLOAD];
    size_t size = 0u;
    size_t length;

    fixture(&state, 7u);
    fixture(&sentinel, 8u);
    sentinel.plants[0].turgor = 4321u;
    CHECK(pg_save_encode(&state, bytes, sizeof bytes, &size));

    /* Every truncation, and none of them may publish. */
    for (length = 0u; length < size; ++length) {
        memcpy(&destination, &sentinel, sizeof destination);
        CHECK(!pg_save_decode(&destination, bytes, length));
        CHECK(memcmp(&destination, &sentinel, sizeof sentinel) == 0);
    }
    /* Corruption in each of the three sections. */
    {
        static const size_t offsets[4] = { 4u, 30u, 130u, 900u };
        size_t index;

        for (index = 0u; index < 4u; ++index) {
            memcpy(mutation, bytes, size);
            mutation[offsets[index]] = (uint8_t)(mutation[offsets[index]]
                                                 ^ 0xffu);
            memcpy(&destination, &sentinel, sizeof destination);
            if (pg_save_decode(&destination, mutation, size)) {
                /* Accepted is allowed — the durable record's CRC is the
                 * integrity check — but it must still be a state the game can
                 * keep. */
                CHECK(pg_validate(&destination, NULL, 0u));
            } else {
                CHECK(memcmp(&destination, &sentinel, sizeof sentinel) == 0);
            }
        }
    }
    return true;
}

static bool test_a_newer_version_is_refused_by_name(void)
{
    pg_state state;
    pg_state sentinel;
    pg_state destination;
    uint8_t bytes[PG_SAVE_MAX_PAYLOAD];
    size_t size = 0u;

    fixture(&state, 11u);
    fixture(&sentinel, 12u);
    CHECK(pg_save_encode(&state, bytes, sizeof bytes, &size));
    bytes[0] = (uint8_t)(PG_SAVE_VERSION + 1u);
    memcpy(&destination, &sentinel, sizeof destination);
    CHECK(!pg_save_decode(&destination, bytes, size));
    CHECK(pg_save_last_reject() == (uint8_t)PG_SAVE_REJECT_NEWER_VERSION);
    CHECK(memcmp(&destination, &sentinel, sizeof sentinel) == 0);
    CHECK(strcmp(pg_save_reject_name(pg_save_last_reject()),
                 "newer version") == 0);
    /* Version 0 is not ours either, and is not a "newer" one. */
    bytes[0] = 0u;
    CHECK(!pg_save_decode(&destination, bytes, size));
    return true;
}

/* An unknown optional section is skipped; an unknown REQUIRED one is fatal;
 * a duplicated known one is fatal. That is the whole reason the envelope
 * exists — a later field is a section, not a version. */
static bool test_the_tlv_envelope_behaves(void)
{
    pg_state state;
    pg_state restored;
    uint8_t bytes[PG_SAVE_MAX_PAYLOAD];
    uint8_t grown[PG_SAVE_MAX_PAYLOAD];
    size_t size = 0u;
    size_t grown_size;

    fixture(&state, 13u);
    CHECK(pg_save_encode(&state, bytes, sizeof bytes, &size));

    /* A section from the future: unknown tag, optional, four bytes of body. */
    memcpy(grown, bytes, size);
    grown[8] = (uint8_t)(grown[8] + 1u);          /* section_count 3 -> 4 */
    grown[size + 0u] = 0x00u;                     /* tag 0x7f00 */
    grown[size + 1u] = 0x7fu;
    grown[size + 2u] = 0x00u;                     /* flags: optional */
    grown[size + 3u] = 0x00u;
    grown[size + 4u] = 0x04u;                     /* length 4 */
    grown[size + 5u] = 0x00u;
    grown[size + 6u] = 0x00u;
    grown[size + 7u] = 0x00u;
    memset(grown + size + 8u, 0xab, 4u);
    grown_size = size + 12u;
    memset(&restored, 0, sizeof restored);
    CHECK(pg_save_decode(&restored, grown, grown_size));
    CHECK(memcmp(restored.plants, state.plants, sizeof state.plants) == 0);

    /* The same section marked REQUIRED is fatal: we cannot claim to have read
     * a record whose author said we must understand it. */
    grown[size + 2u] = 0x01u;
    CHECK(!pg_save_decode(&restored, grown, grown_size));

    /* A duplicated known tag: reject, never last-one-wins. */
    memcpy(grown, bytes, size);
    grown[26u + PG_SAVE_HDR_BYTES] = (uint8_t)PG_SAVE_SECTION_HDR;
    grown[27u + PG_SAVE_HDR_BYTES] = 0u;
    CHECK(!pg_save_decode(&restored, grown, size));

    /* A known tag with the wrong REQUIRED bit is not one of ours. */
    memcpy(grown, bytes, size);
    grown[20u] = 0u;
    CHECK(!pg_save_decode(&restored, grown, size));
    return true;
}

/* A record that decodes but describes a plant the simulation could never
 * produce is rejected, and the destination is untouched. */
static bool test_a_decodable_but_invalid_record_is_refused(void)
{
    pg_state state;
    pg_state sentinel;
    pg_state destination;
    uint8_t bytes[PG_SAVE_MAX_PAYLOAD];
    size_t size = 0u;
    size_t moisture_offset = 26u + PG_SAVE_HDR_BYTES + 8u + 8u
                           + (size_t)PG_NAME_BYTES + 4u;

    fixture(&state, 17u);
    fixture(&sentinel, 18u);
    CHECK(pg_save_encode(&state, bytes, sizeof bytes, &size));
    /* moisture_top above the level ceiling: a level is a level (D-083). */
    bytes[moisture_offset] = 0xffu;
    bytes[moisture_offset + 1u] = 0xffu;
    memcpy(&destination, &sentinel, sizeof destination);
    CHECK(!pg_save_decode(&destination, bytes, size));
    CHECK(pg_save_last_reject() == (uint8_t)PG_SAVE_REJECT_INVALID);
    CHECK(memcmp(&destination, &sentinel, sizeof sentinel) == 0);

    /* And an invalid state is never written either: a record we would refuse
     * to read must not reach a file. */
    state.plants[0].moisture_top = 60000u;
    CHECK(!pg_save_encode(&state, bytes, sizeof bytes, &size));
    CHECK(size == 0u);
    return true;
}

static bool test_a_short_buffer_is_refused(void)
{
    pg_state state;
    uint8_t small[64];
    size_t size = 1u;

    fixture(&state, 19u);
    CHECK(!pg_save_encode(&state, small, sizeof small, &size));
    CHECK(size == 0u);
    return true;
}

/* ---- the durable store --------------------------------------------------- */

static bool temp_directory(char *path, size_t capacity)
{
    const char *base = getenv("TMPDIR");
    int length;

    if (base == NULL || base[0] != '/') {
        base = "/tmp";
    }
    length = snprintf(path, capacity, "%s/pleb-plant-save-XXXXXX", base);
    if (length < 0 || (size_t)length >= capacity) {
        return false;
    }
    return mkdtemp(path) != NULL;
}

static void remove_directory(const char *path)
{
    static const char *const names[3] = {
        "plant-a.state", "plant-b.state", "settings.state"
    };
    char child[512];
    size_t index;

    for (index = 0u; index < 3u; ++index) {
        if (snprintf(child, sizeof child, "%s/pleb-plant-grower/%s", path,
                     names[index]) < (int)sizeof child) {
            (void)unlink(child);
        }
    }
    if (snprintf(child, sizeof child, "%s/pleb-plant-grower", path)
        < (int)sizeof child) {
        (void)rmdir(child);
    }
    (void)rmdir(path);
}

/* First run, a save, a crash mid-write, recovery, two damaged generations, and
 * a second window — the failure matrix of ARCHITECTURE.md §6.4, end to end. */
static bool test_the_store_survives_the_failure_matrix(void)
{
    char directory[256];
    pg_store store;
    pg_state expected;
    pg_state sentinel;
    pg_state loaded;
    bool recovered = true;
    bool ok = false;

    if (!temp_directory(directory, sizeof directory)) {
        (void)fputs("save: no writable temporary directory; skipping the "
                    "durable cases\n", stderr);
        return true;
    }
    fixture(&expected, 21u);
    fixture(&sentinel, 22u);
    sentinel.plants[0].turgor = 999u;

    if (!pg_store_open(&store, directory)) {
        (void)fputs("save: the store would not open\n", stderr);
        goto done;
    }

    /* First run: nothing on disk, nothing published. */
    memcpy(&loaded, &sentinel, sizeof loaded);
    if (pg_store_load(&loaded, &store, &recovered)
        || pg_store_last_status(&store) != (uint8_t)PG_STORE_FIRST_RUN
        || recovered
        || memcmp(&loaded, &sentinel, sizeof sentinel) != 0) {
        (void)fputs("save: a first run did not report itself\n", stderr);
        goto close;
    }

    /* Save, save again, and the generations alternate. */
    if (!pg_store_save(&expected, &store) || pg_store_generation(&store) != 0u
        || pg_store_save_failed(&store)) {
        (void)fputs("save: the first save did not land\n", stderr);
        goto close;
    }
    if (!pg_store_save(&expected, &store)
        || pg_store_generation(&store) != 1u) {
        (void)fputs("save: the generations did not alternate\n", stderr);
        goto close;
    }
    memcpy(&loaded, &sentinel, sizeof loaded);
    if (!pg_store_load(&loaded, &store, &recovered) || recovered
        || pg_store_last_status(&store) != (uint8_t)PG_STORE_READY
        || memcmp(loaded.plants, expected.plants, sizeof loaded.plants) != 0
        || loaded.clock.care_seconds_total
           != expected.clock.care_seconds_total) {
        (void)fputs("save: a clean pair did not load\n", stderr);
        goto close;
    }
    /* The record's sequence is the store's, and it advances on every write. */
    if (loaded.save_sequence <= expected.save_sequence) {
        (void)fputs("save: the sequence did not advance\n", stderr);
        goto close;
    }

    /* A crash mid-write: truncate the live generation and recover the other. */
    {
        char path[KILIXSTATE_PATH_CAPACITY];
        FILE *handle;

        if (kilixstate_store_path(&store.plant[1], path, sizeof path)
            != KILIXSTATE_OK) {
            (void)fputs("save: no record path\n", stderr);
            goto close;
        }
        handle = fopen(path, "wb");
        if (handle == NULL || fwrite("KST1", 1u, 4u, handle) != 4u
            || fclose(handle) != 0) {
            (void)fputs("save: the crash fixture could not be written\n",
                        stderr);
            goto close;
        }
    }
    memcpy(&loaded, &sentinel, sizeof loaded);
    if (!pg_store_load(&loaded, &store, &recovered) || !recovered
        || pg_store_last_status(&store) != (uint8_t)PG_STORE_RECOVERED
        || pg_store_generation(&store) != 0u
        || memcmp(loaded.plants, expected.plants,
                  sizeof loaded.plants) != 0) {
        (void)fputs("save: a crash mid-write lost the plant\n", stderr);
        goto close;
    }
    /* And the repair goes into the generation that failed. */
    if (!pg_store_save(&expected, &store)
        || pg_store_generation(&store) != 1u) {
        (void)fputs("save: the repair went to the wrong generation\n", stderr);
        goto close;
    }

    /* Both generations damaged: refuse, and never wipe. */
    {
        char path[KILIXSTATE_PATH_CAPACITY];
        size_t index;

        for (index = 0u; index < 2u; ++index) {
            FILE *handle;

            if (kilixstate_store_path(&store.plant[index], path, sizeof path)
                != KILIXSTATE_OK) {
                (void)fputs("save: no record path\n", stderr);
                goto close;
            }
            handle = fopen(path, "wb");
            if (handle == NULL || fwrite("junk", 1u, 4u, handle) != 4u
                || fclose(handle) != 0) {
                (void)fputs("save: the damage fixture could not be written\n",
                            stderr);
                goto close;
            }
        }
    }
    memcpy(&loaded, &sentinel, sizeof loaded);
    if (pg_store_load(&loaded, &store, &recovered)
        || pg_store_last_status(&store) != (uint8_t)PG_STORE_DAMAGED
        || memcmp(&loaded, &sentinel, sizeof sentinel) != 0) {
        (void)fputs("save: two damaged generations were not reported\n",
                    stderr);
        goto close;
    }
    if (pg_store_status_line(pg_store_last_status(&store))[0] == '\0') {
        (void)fputs("save: a damaged slot has nothing to say\n", stderr);
        goto close;
    }

    /* A second window: the sequence on disk moved underneath us. */
    pg_store_close(&store);
    if (!pg_store_open(&store, directory)) {
        (void)fputs("save: the store would not reopen\n", stderr);
        goto done;
    }
    if (!pg_store_save(&expected, &store)) {
        (void)fputs("save: the conflict fixture could not be built\n", stderr);
        goto close;
    }
    {
        uint8_t rival[PG_SAVE_MAX_PAYLOAD];
        size_t rival_size = 0u;

        if (!pg_save_encode_with_sequence(&expected, store.sequence + 9u,
                                          rival, sizeof rival, &rival_size)
            || kilixstate_save(&store.plant[1], rival, rival_size)
               != KILIXSTATE_OK) {
            (void)fputs("save: the rival record could not be written\n",
                        stderr);
            goto close;
        }
        if (pg_store_save(&expected, &store)
            || pg_store_last_status(&store) != (uint8_t)PG_STORE_CONFLICT
            || !pg_store_save_failed(&store)) {
            (void)fputs("save: a second window was not detected\n", stderr);
            goto close;
        }
    }
    ok = true;

close:
    pg_store_close(&store);
done:
    remove_directory(directory);
    return ok;
}

/* Persistence can never prevent play. */
static bool test_settings_are_fail_soft(void)
{
    pg_settings defaults;
    pg_settings loaded;
    pg_settings written;
    uint8_t bytes[PG_SETTINGS_MAX_PAYLOAD];
    size_t size = 0u;

    pg_settings_defaults(&defaults);
    CHECK(defaults.master_gain_l <= (uint16_t)PG_LEVEL_MAX);
    CHECK(defaults.faces_enabled && defaults.backdrops_enabled);
    CHECK(defaults.backdrop_scene_id == (uint8_t)PG_SCENE_DEFAULT);

    /* No store at all is still a success, with defaults. */
    memset(&loaded, 0xa5, sizeof loaded);
    CHECK(pg_settings_load(&loaded, NULL));
    CHECK(memcmp(&loaded, &defaults, sizeof defaults) == 0);

    written = defaults;
    written.master_gain_l = 1234u;
    written.faces_enabled = false;
    written.backdrop_scene_id = (uint8_t)PG_SCENE_SUNNY_SILL;
    CHECK(pg_settings_encode(&written, bytes, sizeof bytes, &size));
    CHECK(size == (size_t)PG_SETTINGS_BYTES);
    memset(&loaded, 0, sizeof loaded);
    CHECK(pg_settings_decode(&loaded, bytes, size));
    CHECK(memcmp(&loaded, &written, sizeof written) == 0);

    /* Nonsense is refused by the codec and defaulted by the loader, never
     * accepted. */
    bytes[4] = 0xffu;
    bytes[5] = 0xffu;
    CHECK(!pg_settings_decode(&loaded, bytes, size));
    CHECK(!pg_settings_decode(&loaded, bytes, size - 1u));
    return true;
}

static bool test_null_arguments_are_survivable(void)
{
    pg_state state;
    pg_store store;
    uint8_t bytes[PG_SAVE_MAX_PAYLOAD];
    size_t size = 1u;
    uint64_t sequence = 1u;

    fixture(&state, 23u);
    CHECK(!pg_save_encode(NULL, bytes, sizeof bytes, &size));
    CHECK(size == 0u);
    CHECK(!pg_save_encode(&state, NULL, sizeof bytes, &size));
    CHECK(!pg_save_decode(NULL, bytes, sizeof bytes));
    CHECK(!pg_save_decode(&state, NULL, 8u));
    /* A peek that fails always leaves a zero behind, never a stale number. */
    CHECK(!pg_save_peek_sequence(NULL, 0u, &sequence));
    CHECK(sequence == 0u);
    sequence = 1u;
    CHECK(!pg_save_peek_sequence(bytes, 0u, &sequence));
    CHECK(sequence == 0u);
    CHECK(!pg_save_peek_sequence(bytes, 8u, NULL));
    CHECK(!pg_store_open(NULL, NULL));
    pg_store_close(NULL);
    CHECK(pg_store_last_status(NULL) == (uint8_t)PG_STORE_UNOPENED);
    CHECK(!pg_store_save_failed(NULL));
    CHECK(pg_store_generation(NULL) == PG_STORE_GENERATION_NONE);
    CHECK(pg_store_status_line(0xffu)[0] == '\0');
    CHECK(!pg_store_save(&state, NULL));
    CHECK(!pg_store_load(&state, NULL, NULL));
    memset(&store, 0, sizeof store);
    CHECK(!pg_store_load(&state, &store, NULL));
    CHECK(pg_store_last_status(&store) == (uint8_t)PG_STORE_UNAVAILABLE);
    CHECK(!pg_store_save(&state, &store));
    CHECK(pg_store_save_failed(&store));
    pg_settings_defaults(NULL);
    CHECK(pg_settings_load(NULL, NULL));
    CHECK(!pg_settings_save(NULL, NULL));
    return true;
}

int main(void)
{
    char error[128];

    if (!pg_content_validate(error, sizeof error)) {
        (void)fprintf(stderr, "save: content invalid: %s\n", error);
        return 1;
    }
    if (!test_the_wire_layout_is_where_it_says()) return 1;
    if (!test_every_field_of_the_record_survives()) return 1;
    if (!test_epoch_seconds_survive_the_whole_range()) return 1;
    if (!test_the_local_offset_comes_back_signed()) return 1;
    if (!test_save_equals_ticks_through_the_save()) return 1;
    if (!test_a_corrupt_or_truncated_record_changes_nothing()) return 1;
    if (!test_a_newer_version_is_refused_by_name()) return 1;
    if (!test_the_tlv_envelope_behaves()) return 1;
    if (!test_a_decodable_but_invalid_record_is_refused()) return 1;
    if (!test_a_short_buffer_is_refused()) return 1;
    if (!test_the_store_survives_the_failure_matrix()) return 1;
    if (!test_settings_are_fail_soft()) return 1;
    if (!test_null_arguments_are_survivable()) return 1;
    (void)puts("save: PASS");
    return 0;
}
