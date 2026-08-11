/*
 * pg_save — the record, encoded and decoded, and nothing else.
 *
 * No file, no clock, no allocator, no static state that survives a call except
 * the one diagnostic byte the store reads back immediately. That purity is
 * what the milestone's gates are built on: a corruption harness can flip every
 * bit of a record in memory at machine speed, and the Milestone 4 equivalence
 * assertion can be run *through the save format* with no filesystem involved.
 *
 * The format, the section table and the epoch-second encoding are documented
 * in pg_save.h. Read that first; this file is the mechanism.
 *
 * Two rules govern everything below:
 *
 *   - **A rejected decode never touches the destination.** Every decode builds
 *     a candidate on the stack, runs `pg_validate` against it, and publishes
 *     only on success. A half-decoded plant is not a plant.
 *   - **A decode that succeeds is canonical.** Re-encoding what we just
 *     decoded reproduces the same bytes, which is the property the harness
 *     asserts on every mutation it accepts. That is why the leaf and vine
 *     arrays are written in full rather than truncated to their counts: the
 *     mercy path lowers `leaf_count` without clearing the slots behind it
 *     (pg_sim.c), and a save is a photograph of the record, not a tidied
 *     version of one.
 */
#include "pg_save.h"

#include "pg_plant.h"
#include "pg_state.h"
#include "pg_time.h"

#include "kilix_state_codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ARCHITECTURE.md §6.2's own numbers, pinned. A field added to pg_plant
 * without a matching encoder changes one of these, and the build stops. */
_Static_assert(PG_SAVE_HDR_BYTES == 84u, "HDR size drifted from §6.2");
_Static_assert(PG_SAVE_PLNT_ENTRY_BYTES == 314u,
               "PLNT entry size drifted from §6.2");
_Static_assert(PG_SAVE_JRNL_BYTES == 338u, "JRNL size drifted from §6.2");
_Static_assert(PG_SAVE_WORST_CASE_BYTES <= PG_SAVE_MAX_PAYLOAD,
               "the record no longer fits the 4 KiB payload budget");
_Static_assert(PG_SETTINGS_BYTES <= PG_SETTINGS_MAX_PAYLOAD,
               "settings no longer fit their 256 B payload budget");
/* The store's scratch buffer is the only buffer either record is built in. */
_Static_assert(sizeof(((pg_store *)0)->scratch) >= PG_SAVE_MAX_PAYLOAD,
               "store scratch is smaller than the payload ceiling");
/* Structural cross-checks: the encoder walks these bounds by hand. */
_Static_assert(sizeof(((pg_plant *)0)->dli_ring)
               == (size_t)PG_DLI_RING * sizeof(uint16_t),
               "dli ring width drifted");
_Static_assert(sizeof(((pg_plant *)0)->name) == (size_t)PG_NAME_BYTES,
               "name width drifted");

/* A record with more sections than this is not one of ours. The bound exists
 * so that a corrupt section_count cannot make the decoder walk a long way
 * before failing. */
#define PG_SAVE_MAX_SECTIONS 16u

static const uint8_t PG_SAVE_MAGIC[PG_SAVE_MAGIC_BYTES] = { 'P', 'G', 'R', 'W' };

/* Why the last decode said no. Read immediately after a decode or not at
 * all. */
static uint8_t pg_save_reject_last;

uint8_t pg_save_last_reject(void)
{
    return pg_save_reject_last;
}

const char *pg_save_reject_name(uint8_t reject)
{
    switch ((pg_save_reject)reject) {
    case PG_SAVE_REJECT_NONE:          return "ok";
    case PG_SAVE_REJECT_ARGUMENT:      return "invalid argument";
    case PG_SAVE_REJECT_TRUNCATED:     return "truncated";
    case PG_SAVE_REJECT_MALFORMED:     return "malformed";
    case PG_SAVE_REJECT_INVALID:       return "invalid state";
    case PG_SAVE_REJECT_NEWER_VERSION: return "newer version";
    case PG_SAVE_REJECT_COUNT:
    default:                           return "unknown";
    }
}

/* ---- the two signed encodings ------------------------------------------- */

/* An epoch second, as the little-endian two's-complement bit pattern of the
 * value. See the header for why this shape and not another. */
static bool write_epoch_s(kilixstate_writer *writer, int64_t value)
{
    return kilixstate_write_u64(writer, (uint64_t)value);
}

static bool read_epoch_s(kilixstate_reader *reader, int64_t *value)
{
    uint64_t raw = 0u;

    if (!kilixstate_read_u64(reader, &raw)) {
        return false;
    }
    /* The negative branch never forms an out-of-range signed intermediate:
     * for raw > INT64_MAX, UINT64_MAX - raw is at most INT64_MAX - 1. */
    *value = (raw <= (uint64_t)INT64_MAX)
           ? (int64_t)raw
           : -(int64_t)(UINT64_MAX - raw) - 1;
    return true;
}

/* The local offset, through u16 and back through int16_t, then clamped to
 * [-720, +840] with anything outside treated as 0 (D-079). */
static bool write_tz_minutes(kilixstate_writer *writer, int32_t minutes)
{
    return kilixstate_write_u16(writer, (uint16_t)(int16_t)minutes);
}

static bool read_tz_minutes(kilixstate_reader *reader, int32_t *minutes)
{
    uint16_t raw = 0u;

    if (!kilixstate_read_u16(reader, &raw)) {
        return false;
    }
    *minutes = pg_time_clamp_tz_minutes((int32_t)(int16_t)raw);
    return true;
}

/* ---- the TLV envelope ---------------------------------------------------- */

static bool write_section_header(kilixstate_writer *writer, uint16_t tag,
                                 bool required, uint32_t length)
{
    return kilixstate_write_u16(writer, tag)
        && kilixstate_write_u16(writer,
                                required ? (uint16_t)PG_SAVE_SECTION_REQUIRED
                                         : (uint16_t)0u)
        && kilixstate_write_u32(writer, length);
}

static bool section_flags_are_wrong(uint16_t tag, uint16_t flags)
{
    switch ((pg_save_section)tag) {
    case PG_SAVE_SECTION_HDR:
    case PG_SAVE_SECTION_PLNT:
        return flags != (uint16_t)PG_SAVE_SECTION_REQUIRED;
    case PG_SAVE_SECTION_JRNL:
    case PG_SAVE_SECTION_STRY:
        return flags != 0u;
    default:
        return false;   /* an unknown tag may say whatever it likes */
    }
}

/* Every section is a fixed size this file computes, so an encoder that wrote
 * the wrong number of bytes is a defect that must not reach a file. */
static bool section_is_exactly(const kilixstate_writer *writer, size_t before,
                               uint32_t expected)
{
    return kilixstate_writer_size(writer) - before == (size_t)expected;
}

/* ---- HDR ---------------------------------------------------------------- */

static bool encode_hdr(kilixstate_writer *writer, const pg_state *state,
                       uint64_t sequence)
{
    const size_t before = kilixstate_writer_size(writer);

    if (!kilixstate_write_u64(writer, sequence)
        || !kilixstate_write_u64(writer, state->clock.care_seconds_total)
        || !kilixstate_write_u32(writer, state->clock.care_residual_s)
        || !write_epoch_s(writer, state->anchor.last_wall_s)
        || !write_epoch_s(writer, state->anchor.last_boot_s)
        || !write_epoch_s(writer, state->first_run_wall_s)
        || !kilixstate_write_bytes(writer, state->anchor.boot_id,
                                   (size_t)PG_BOOT_ID_BYTES)
        || !kilixstate_write_bool(writer, state->anchor.established)
        || !write_tz_minutes(writer, state->anchor.tz_offset_minutes)
        || !kilixstate_write_u16(writer, state->clock_anomaly_count)
        || !kilixstate_write_u64(writer, state->rng.state)
        || !kilixstate_write_u64(writer, state->weather_seed)
        || !kilixstate_write_u8(writer, state->hemisphere)
        || !kilixstate_write_u8(writer, state->time_scale)
        || !kilixstate_write_u8(writer, state->plant_count)) {
        return false;
    }
    return section_is_exactly(writer, before, PG_SAVE_HDR_BYTES);
}

static bool decode_hdr(kilixstate_reader *reader, pg_state *state)
{
    bool established = false;

    if (!kilixstate_read_u64(reader, &state->save_sequence)
        || !kilixstate_read_u64(reader, &state->clock.care_seconds_total)
        || !kilixstate_read_u32(reader, &state->clock.care_residual_s)
        || !read_epoch_s(reader, &state->anchor.last_wall_s)
        || !read_epoch_s(reader, &state->anchor.last_boot_s)
        || !read_epoch_s(reader, &state->first_run_wall_s)
        || !kilixstate_read_bytes(reader, state->anchor.boot_id,
                                  (size_t)PG_BOOT_ID_BYTES)
        || !kilixstate_read_bool(reader, &established)
        || !read_tz_minutes(reader, &state->anchor.tz_offset_minutes)
        || !kilixstate_read_u16(reader, &state->clock_anomaly_count)
        || !kilixstate_read_u64(reader, &state->rng.state)
        || !kilixstate_read_u64(reader, &state->weather_seed)
        || !kilixstate_read_u8(reader, &state->hemisphere)
        || !kilixstate_read_u8(reader, &state->time_scale)
        || !kilixstate_read_u8(reader, &state->plant_count)) {
        return false;
    }
    state->anchor.established = established;
    return kilixstate_reader_require_finished(reader);
}

/* ---- PLNT --------------------------------------------------------------- */

static bool encode_plant(kilixstate_writer *writer, const pg_plant *plant)
{
    const size_t before = kilixstate_writer_size(writer);
    size_t index;

    if (!write_epoch_s(writer, plant->planted_wall_s)
        || !kilixstate_write_bytes(writer, plant->name,
                                   (size_t)PG_NAME_BYTES)
        || !kilixstate_write_u8(writer, plant->species_id)
        || !kilixstate_write_u8(writer, plant->pot_id)
        || !kilixstate_write_u8(writer, plant->scene_id)
        || !kilixstate_write_u8(writer, plant->spot_id)
        || !kilixstate_write_u16(writer, plant->moisture_top)
        || !kilixstate_write_u16(writer, plant->moisture_bottom)
        || !kilixstate_write_u16(writer, plant->light_debt)
        || !kilixstate_write_u16(writer, plant->nutrition)
        || !kilixstate_write_u16(writer, plant->salt)
        || !kilixstate_write_u16(writer, plant->purity_load)
        || !kilixstate_write_u16(writer, plant->root_health)
        || !kilixstate_write_u16(writer, plant->root_capacity)
        || !kilixstate_write_u16(writer, plant->root_bound)
        || !kilixstate_write_u16(writer, plant->turgor)
        || !kilixstate_write_u32(writer, plant->soggy_ticks)
        || !kilixstate_write_u16(writer, plant->scorch_dose)
        || !kilixstate_write_u16(writer, plant->crisp_dose)
        || !kilixstate_write_u16(writer, plant->acclimation)
        || !kilixstate_write_u16(writer, plant->shock)
        || !kilixstate_write_u16(writer, plant->dli_today)) {
        return false;
    }
    for (index = 0u; index < (size_t)PG_DLI_RING; ++index) {
        if (!kilixstate_write_u16(writer, plant->dli_ring[index])) {
            return false;
        }
    }
    if (!kilixstate_write_u8(writer, plant->dli_ring_head)
        || !kilixstate_write_u16(writer, plant->pest_gnats)
        || !kilixstate_write_u16(writer, plant->pest_mites)
        || !kilixstate_write_u16(writer, plant->pest_mealy)
        || !kilixstate_write_u16(writer, plant->pest_egg_timer)
        || !kilixstate_write_u8(writer, plant->growth_stage)
        || !kilixstate_write_u32(writer, plant->growth_points)
        || !kilixstate_write_u8(writer, plant->life_state)
        || !kilixstate_write_u8(writer, plant->spathe_state)
        || !kilixstate_write_u8(writer, plant->flower_count)
        || !kilixstate_write_u8(writer, plant->leaf_count)) {
        return false;
    }
    for (index = 0u; index < (size_t)PG_LEAF_MAX; ++index) {
        const pg_leaf *leaf = &plant->leaves[index];

        if (!kilixstate_write_u16(writer, leaf->birth_care_day)
            || !kilixstate_write_u8(writer, leaf->slot)
            || !kilixstate_write_u8(writer, leaf->form_flags)
            || !kilixstate_write_u8(writer, leaf->damage_mask)
            || !kilixstate_write_u8(writer, leaf->pose)) {
            return false;
        }
    }
    if (!kilixstate_write_u8(writer, plant->vine_count)) {
        return false;
    }
    for (index = 0u; index < (size_t)PG_VINE_MAX; ++index) {
        const pg_vine_node *node = &plant->vine[index];

        if (!kilixstate_write_u8(writer, node->internode)
            || !kilixstate_write_u8(writer, node->leaf_form)
            || !kilixstate_write_u8(writer, node->damage_mask)) {
            return false;
        }
    }
    if (!kilixstate_write_u64(writer, plant->last_watered_care_s)
        || !kilixstate_write_u64(writer, plant->last_fed_care_s)
        || !kilixstate_write_u64(writer, plant->last_repotted_care_s)
        || !kilixstate_write_u64(writer, plant->last_rotated_care_s)
        || !kilixstate_write_u64(writer, plant->last_drained_care_s)
        || !kilixstate_write_u8(writer, plant->pending_action)
        || !kilixstate_write_u64(writer, plant->pending_action_care_s)
        || !kilixstate_write_u16(writer, plant->streak_on_time)
        || !kilixstate_write_u16(writer, plant->missed_events)) {
        return false;
    }
    return section_is_exactly(writer, before, PG_SAVE_PLNT_ENTRY_BYTES);
}

static bool decode_plant(kilixstate_reader *reader, pg_plant *plant)
{
    size_t index;

    if (!read_epoch_s(reader, &plant->planted_wall_s)
        || !kilixstate_read_bytes(reader, plant->name, (size_t)PG_NAME_BYTES)
        || !kilixstate_read_u8(reader, &plant->species_id)
        || !kilixstate_read_u8(reader, &plant->pot_id)
        || !kilixstate_read_u8(reader, &plant->scene_id)
        || !kilixstate_read_u8(reader, &plant->spot_id)
        || !kilixstate_read_u16(reader, &plant->moisture_top)
        || !kilixstate_read_u16(reader, &plant->moisture_bottom)
        || !kilixstate_read_u16(reader, &plant->light_debt)
        || !kilixstate_read_u16(reader, &plant->nutrition)
        || !kilixstate_read_u16(reader, &plant->salt)
        || !kilixstate_read_u16(reader, &plant->purity_load)
        || !kilixstate_read_u16(reader, &plant->root_health)
        || !kilixstate_read_u16(reader, &plant->root_capacity)
        || !kilixstate_read_u16(reader, &plant->root_bound)
        || !kilixstate_read_u16(reader, &plant->turgor)
        || !kilixstate_read_u32(reader, &plant->soggy_ticks)
        || !kilixstate_read_u16(reader, &plant->scorch_dose)
        || !kilixstate_read_u16(reader, &plant->crisp_dose)
        || !kilixstate_read_u16(reader, &plant->acclimation)
        || !kilixstate_read_u16(reader, &plant->shock)
        || !kilixstate_read_u16(reader, &plant->dli_today)) {
        return false;
    }
    for (index = 0u; index < (size_t)PG_DLI_RING; ++index) {
        if (!kilixstate_read_u16(reader, &plant->dli_ring[index])) {
            return false;
        }
    }
    if (!kilixstate_read_u8(reader, &plant->dli_ring_head)
        || !kilixstate_read_u16(reader, &plant->pest_gnats)
        || !kilixstate_read_u16(reader, &plant->pest_mites)
        || !kilixstate_read_u16(reader, &plant->pest_mealy)
        || !kilixstate_read_u16(reader, &plant->pest_egg_timer)
        || !kilixstate_read_u8(reader, &plant->growth_stage)
        || !kilixstate_read_u32(reader, &plant->growth_points)
        || !kilixstate_read_u8(reader, &plant->life_state)
        || !kilixstate_read_u8(reader, &plant->spathe_state)
        || !kilixstate_read_u8(reader, &plant->flower_count)
        || !kilixstate_read_u8(reader, &plant->leaf_count)) {
        return false;
    }
    for (index = 0u; index < (size_t)PG_LEAF_MAX; ++index) {
        pg_leaf *leaf = &plant->leaves[index];

        if (!kilixstate_read_u16(reader, &leaf->birth_care_day)
            || !kilixstate_read_u8(reader, &leaf->slot)
            || !kilixstate_read_u8(reader, &leaf->form_flags)
            || !kilixstate_read_u8(reader, &leaf->damage_mask)
            || !kilixstate_read_u8(reader, &leaf->pose)) {
            return false;
        }
    }
    if (!kilixstate_read_u8(reader, &plant->vine_count)) {
        return false;
    }
    for (index = 0u; index < (size_t)PG_VINE_MAX; ++index) {
        pg_vine_node *node = &plant->vine[index];

        if (!kilixstate_read_u8(reader, &node->internode)
            || !kilixstate_read_u8(reader, &node->leaf_form)
            || !kilixstate_read_u8(reader, &node->damage_mask)) {
            return false;
        }
    }
    return kilixstate_read_u64(reader, &plant->last_watered_care_s)
        && kilixstate_read_u64(reader, &plant->last_fed_care_s)
        && kilixstate_read_u64(reader, &plant->last_repotted_care_s)
        && kilixstate_read_u64(reader, &plant->last_rotated_care_s)
        && kilixstate_read_u64(reader, &plant->last_drained_care_s)
        && kilixstate_read_u8(reader, &plant->pending_action)
        && kilixstate_read_u64(reader, &plant->pending_action_care_s)
        && kilixstate_read_u16(reader, &plant->streak_on_time)
        && kilixstate_read_u16(reader, &plant->missed_events);
}

/* ---- JRNL ---------------------------------------------------------------
 * The whole ring, verbatim, plus its head and its count. Past-day calendar
 * marks are the away report and the post-mortem; losing them to a crash would
 * lose the only account of what happened while nobody was watching. */
static bool encode_jrnl(kilixstate_writer *writer, const pg_state *state)
{
    const size_t before = kilixstate_writer_size(writer);
    size_t index;

    if (!kilixstate_write_u8(writer, state->journal_head)
        || !kilixstate_write_u8(writer, state->journal_count)) {
        return false;
    }
    for (index = 0u; index < (size_t)PG_JOURNAL_RING; ++index) {
        const pg_journal_entry *entry = &state->journal[index];

        if (!kilixstate_write_u32(writer, entry->care_day)
            || !kilixstate_write_u8(writer, entry->kind)
            || !kilixstate_write_u8(writer, entry->detail)
            || !kilixstate_write_u8(writer, entry->plant_index)) {
            return false;
        }
    }
    return section_is_exactly(writer, before, PG_SAVE_JRNL_BYTES);
}

static bool decode_jrnl(kilixstate_reader *reader, pg_state *state)
{
    size_t index;

    if (!kilixstate_read_u8(reader, &state->journal_head)
        || !kilixstate_read_u8(reader, &state->journal_count)) {
        return false;
    }
    for (index = 0u; index < (size_t)PG_JOURNAL_RING; ++index) {
        pg_journal_entry *entry = &state->journal[index];

        if (!kilixstate_read_u32(reader, &entry->care_day)
            || !kilixstate_read_u8(reader, &entry->kind)
            || !kilixstate_read_u8(reader, &entry->detail)
            || !kilixstate_read_u8(reader, &entry->plant_index)) {
            return false;
        }
    }
    return kilixstate_reader_require_finished(reader);
}

/* ---- encode ------------------------------------------------------------- */

bool pg_save_encode_with_sequence(const pg_state *state, uint64_t sequence,
                                  uint8_t *bytes, size_t capacity,
                                  size_t *written)
{
    kilixstate_writer writer;
    uint16_t section_count;
    bool with_journal;
    size_t index;

    if (written != NULL) {
        *written = 0u;
    }
    if (state == NULL || bytes == NULL || capacity == 0u) {
        return false;
    }
    /* Refuse to write what we would refuse to read. A record that cannot be
     * loaded back is worse than no record at all: it presents as damage. */
    if (!pg_validate(state, NULL, 0u)) {
        return false;
    }
    with_journal = state->journal_count > 0u;
    section_count = with_journal ? 3u : 2u;

    kilixstate_writer_init(&writer, bytes, capacity);
    if (!kilixstate_write_u32(&writer, PG_SAVE_VERSION)
        || !kilixstate_write_bytes(&writer, PG_SAVE_MAGIC,
                                   sizeof PG_SAVE_MAGIC)
        || !kilixstate_write_u16(&writer, section_count)
        || !kilixstate_write_u64(&writer, sequence)) {
        return false;
    }
    if (!write_section_header(&writer, (uint16_t)PG_SAVE_SECTION_HDR, true,
                              PG_SAVE_HDR_BYTES)
        || !encode_hdr(&writer, state, sequence)) {
        return false;
    }
    if (!write_section_header(&writer, (uint16_t)PG_SAVE_SECTION_PLNT, true,
                              (uint32_t)state->plant_count
                              * PG_SAVE_PLNT_ENTRY_BYTES)) {
        return false;
    }
    for (index = 0u; index < (size_t)state->plant_count; ++index) {
        if (!encode_plant(&writer, &state->plants[index])) {
            return false;
        }
    }
    if (with_journal
        && (!write_section_header(&writer, (uint16_t)PG_SAVE_SECTION_JRNL,
                                  false, PG_SAVE_JRNL_BYTES)
            || !encode_jrnl(&writer, state))) {
        return false;
    }
    if (kilixstate_writer_result(&writer) != KILIXSTATE_CODEC_OK) {
        return false;
    }
    if (written != NULL) {
        *written = kilixstate_writer_size(&writer);
    }
    return true;
}

bool pg_save_encode(const pg_state *state, uint8_t *bytes, size_t capacity,
                    size_t *written)
{
    if (state == NULL) {
        if (written != NULL) {
            *written = 0u;
        }
        return false;
    }
    return pg_save_encode_with_sequence(state, state->save_sequence, bytes,
                                        capacity, written);
}

/* ---- decode ------------------------------------------------------------- */

typedef struct pg_save_decode_context {
    pg_state *output;
} pg_save_decode_context;

static bool decode_v1(kilixstate_reader *reader, void *context_bytes)
{
    pg_save_decode_context *context = (pg_save_decode_context *)context_bytes;
    pg_state candidate;
    uint8_t magic[PG_SAVE_MAGIC_BYTES];
    uint16_t section_count = 0u;
    uint16_t section;
    uint64_t envelope_sequence = 0u;
    size_t plant_entries = 0u;
    size_t entry;
    bool seen_hdr = false;
    bool seen_plnt = false;
    bool seen_jrnl = false;
    bool seen_stry = false;

    if (context == NULL || context->output == NULL) {
        pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_ARGUMENT;
        return false;
    }
    /* Zero, not pg_init: a decode must not depend on defaults it did not
     * read, and the fields the format deliberately omits — the audio queue,
     * the sticky save_failed flag, the replant prompt — are transient host
     * plumbing that must come back empty. */
    memset(&candidate, 0, sizeof candidate);

    if (!kilixstate_read_bytes(reader, magic, sizeof magic)
        || memcmp(magic, PG_SAVE_MAGIC, sizeof magic) != 0
        || !kilixstate_read_u16(reader, &section_count)
        || section_count == 0u || section_count > PG_SAVE_MAX_SECTIONS
        || !kilixstate_read_u64(reader, &envelope_sequence)) {
        pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_MALFORMED;
        return false;
    }

    for (section = 0u; section < section_count; ++section) {
        kilixstate_reader body;
        uint16_t tag = 0u;
        uint16_t flags = 0u;
        uint32_t length = 0u;
        bool valid = false;

        if (!kilixstate_read_u16(reader, &tag)
            || !kilixstate_read_u16(reader, &flags)
            || !kilixstate_read_u32(reader, &length)
            || (flags & (uint16_t)~(uint16_t)PG_SAVE_SECTION_REQUIRED) != 0u
            || (size_t)length > kilixstate_reader_remaining(reader)) {
            pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_MALFORMED;
            return false;
        }
        kilixstate_reader_init(&body, reader->bytes + reader->offset,
                               (size_t)length);
        /* A known tag carries exactly the flags this format gives it. HDR and
         * PLNT are REQUIRED and JRNL and STRY are not; a record that says
         * otherwise is not one of ours, whoever wrote it. */
        if (section_flags_are_wrong(tag, flags)) {
            pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_MALFORMED;
            return false;
        }
        if (tag == (uint16_t)PG_SAVE_SECTION_HDR && !seen_hdr) {
            valid = decode_hdr(&body, &candidate);
            seen_hdr = true;
        } else if (tag == (uint16_t)PG_SAVE_SECTION_PLNT && !seen_plnt) {
            seen_plnt = true;
            if (length % PG_SAVE_PLNT_ENTRY_BYTES != 0u
                || (size_t)length
                   > (size_t)PG_PLANT_MAX * PG_SAVE_PLNT_ENTRY_BYTES) {
                pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_MALFORMED;
                return false;
            }
            plant_entries = (size_t)length / PG_SAVE_PLNT_ENTRY_BYTES;
            valid = true;
            for (entry = 0u; valid && entry < plant_entries; ++entry) {
                valid = decode_plant(&body, &candidate.plants[entry]);
            }
            valid = valid && kilixstate_reader_require_finished(&body);
        } else if (tag == (uint16_t)PG_SAVE_SECTION_JRNL && !seen_jrnl) {
            valid = decode_jrnl(&body, &candidate);
            seen_jrnl = true;
        } else if (tag == (uint16_t)PG_SAVE_SECTION_STRY && !seen_stry) {
            /* Known, reserved, and not consumed by this version. Skipping it
             * is the TLV envelope working exactly as designed. */
            valid = true;
            seen_stry = true;
        } else if (tag == (uint16_t)PG_SAVE_SECTION_HDR
                   || tag == (uint16_t)PG_SAVE_SECTION_PLNT
                   || tag == (uint16_t)PG_SAVE_SECTION_JRNL
                   || tag == (uint16_t)PG_SAVE_SECTION_STRY
                   || (flags & (uint16_t)PG_SAVE_SECTION_REQUIRED) != 0u) {
            /* A duplicated known tag, or a required section this version does
             * not understand. Both are fatal, and for the same reason: we
             * cannot claim to have read the record. */
            pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_MALFORMED;
            return false;
        } else {
            valid = true;   /* unknown and optional: skip */
        }
        if (!valid || !kilixstate_skip(reader, (size_t)length)) {
            pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_MALFORMED;
            return false;
        }
    }

    if (!seen_hdr || !seen_plnt
        || envelope_sequence != candidate.save_sequence
        || plant_entries != (size_t)candidate.plant_count
        || !kilixstate_reader_require_finished(reader)) {
        pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_MALFORMED;
        return false;
    }
    if (!pg_validate(&candidate, NULL, 0u)) {
        pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_INVALID;
        return false;
    }
    *context->output = candidate;
    return true;
}

bool pg_save_decode(pg_state *state, const uint8_t *bytes, size_t size)
{
    static const kilixstate_migration migrations[] = {
        /* One row today; a table from day one, because the day a second row is
         * needed is not the day to design the mechanism (D-044). A zero
         * payload_size accepts any length: our sections are fixed but their
         * number is not. */
        { PG_SAVE_VERSION, 0u, false, decode_v1 }
    };
    pg_save_decode_context context;
    kilixstate_codec_result result;

    pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_NONE;
    if (state == NULL || (bytes == NULL && size != 0u)) {
        pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_ARGUMENT;
        return false;
    }
    context.output = state;
    result = kilixstate_migrate(bytes, size, migrations,
                                sizeof migrations / sizeof migrations[0],
                                &context, NULL);
    if (result == KILIXSTATE_CODEC_OK) {
        pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_NONE;
        return true;
    }
    /* decode_v1 has already said why when it ran at all; the cases below are
     * the ones it never saw. */
    switch (result) {
    case KILIXSTATE_CODEC_UNKNOWN_VERSION:
        pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_NEWER_VERSION;
        break;
    case KILIXSTATE_CODEC_TRUNCATED:
        if (pg_save_reject_last == (uint8_t)PG_SAVE_REJECT_NONE) {
            pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_TRUNCATED;
        }
        break;
    case KILIXSTATE_CODEC_INVALID_ARGUMENT:
        pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_ARGUMENT;
        break;
    default:
        if (pg_save_reject_last == (uint8_t)PG_SAVE_REJECT_NONE) {
            pg_save_reject_last = (uint8_t)PG_SAVE_REJECT_MALFORMED;
        }
        break;
    }
    return false;
}

bool pg_save_peek_sequence(const uint8_t *bytes, size_t size,
                           uint64_t *sequence)
{
    kilixstate_reader reader;
    uint8_t magic[PG_SAVE_MAGIC_BYTES];
    uint32_t version = 0u;
    uint16_t section_count = 0u;

    if (sequence != NULL) {
        *sequence = 0u;
    }
    if (bytes == NULL || sequence == NULL) {
        return false;
    }
    kilixstate_reader_init(&reader, bytes, size);
    return kilixstate_read_u32(&reader, &version)
        && version == PG_SAVE_VERSION
        && kilixstate_read_bytes(&reader, magic, sizeof magic)
        && memcmp(magic, PG_SAVE_MAGIC, sizeof magic) == 0
        && kilixstate_read_u16(&reader, &section_count)
        && kilixstate_read_u64(&reader, sequence);
}

/* ---- settings ------------------------------------------------------------
 * A separate record with its own version, because it is fail-soft and the
 * plant record is not: a settings file that cannot be read costs a preference,
 * and a plant file that cannot be read costs a plant. */

void pg_settings_defaults(pg_settings *settings)
{
    if (settings == NULL) {
        return;
    }
    memset(settings, 0, sizeof *settings);
    settings->master_gain_l = (uint16_t)PG_LEVEL_MAX;
    settings->music_gain_l = 6000u;
    settings->sfx_gain_l = 8000u;
    settings->faces_enabled = true;
    settings->backdrops_enabled = true;
    settings->backdrop_scene_id = (uint8_t)PG_SCENE_DEFAULT;
}

static bool settings_is_sane(const pg_settings *settings)
{
    return settings->master_gain_l <= (uint16_t)PG_LEVEL_MAX
        && settings->music_gain_l <= (uint16_t)PG_LEVEL_MAX
        && settings->sfx_gain_l <= (uint16_t)PG_LEVEL_MAX
        && settings->backdrop_scene_id < (uint8_t)PG_SCENE_COUNT;
}

bool pg_settings_encode(const pg_settings *settings, uint8_t *bytes,
                        size_t capacity, size_t *written)
{
    kilixstate_writer writer;

    if (written != NULL) {
        *written = 0u;
    }
    if (settings == NULL || bytes == NULL || !settings_is_sane(settings)) {
        return false;
    }
    kilixstate_writer_init(&writer, bytes, capacity);
    if (!kilixstate_write_u32(&writer, PG_SETTINGS_VERSION)
        || !kilixstate_write_u16(&writer, settings->master_gain_l)
        || !kilixstate_write_u16(&writer, settings->music_gain_l)
        || !kilixstate_write_u16(&writer, settings->sfx_gain_l)
        || !kilixstate_write_bool(&writer, settings->faces_enabled)
        || !kilixstate_write_bool(&writer, settings->backdrops_enabled)
        || !kilixstate_write_u8(&writer, settings->backdrop_scene_id)
        || kilixstate_writer_size(&writer) != (size_t)PG_SETTINGS_BYTES) {
        return false;
    }
    if (written != NULL) {
        *written = kilixstate_writer_size(&writer);
    }
    return true;
}

static bool decode_settings_v1(kilixstate_reader *reader, void *context_bytes)
{
    pg_settings *output = (pg_settings *)context_bytes;
    pg_settings candidate;

    if (output == NULL) {
        return false;
    }
    pg_settings_defaults(&candidate);
    if (!kilixstate_read_u16(reader, &candidate.master_gain_l)
        || !kilixstate_read_u16(reader, &candidate.music_gain_l)
        || !kilixstate_read_u16(reader, &candidate.sfx_gain_l)
        || !kilixstate_read_bool(reader, &candidate.faces_enabled)
        || !kilixstate_read_bool(reader, &candidate.backdrops_enabled)
        || !kilixstate_read_u8(reader, &candidate.backdrop_scene_id)
        || !kilixstate_reader_require_finished(reader)
        || !settings_is_sane(&candidate)) {
        return false;
    }
    *output = candidate;
    return true;
}

bool pg_settings_decode(pg_settings *settings, const uint8_t *bytes,
                        size_t size)
{
    static const kilixstate_migration migrations[] = {
        { PG_SETTINGS_VERSION, (size_t)PG_SETTINGS_BYTES, false,
          decode_settings_v1 }
    };

    if (settings == NULL || (bytes == NULL && size != 0u)) {
        return false;
    }
    return kilixstate_migrate(bytes, size, migrations,
                              sizeof migrations / sizeof migrations[0],
                              settings, NULL) == KILIXSTATE_CODEC_OK;
}
