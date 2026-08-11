/*
 * pg_save — the persisted record, and the seam between the pure codec and the
 * durable store.
 *
 * Two files implement what is declared here, and the split is the point:
 *
 *   - `pg_save.c` is **pure**. It turns a pg_state into bytes and bytes back
 *     into a pg_state, and it touches no file, no clock and no allocator. That
 *     is what lets the corruption harness flip every bit of a record in memory
 *     at machine speed, and what lets the Milestone 4 equivalence assertion run
 *     *through the save format* without a filesystem being involved.
 *   - `pg_store.c` owns kilix-state: the A/B generation pair, the fail-soft
 *     settings record, the sticky `save_failed` flag and the failure matrix of
 *     ARCHITECTURE.md §6.4.
 *
 * ---------------------------------------------------------------------------
 * The record is ARCHITECTURE.md §6.2 in full. Leading little-endian `u32`
 * version consumed by `kilixstate_migrate` against a migration table that
 * exists from day one, then kilix-punch-club's TLV section envelope: unknown
 * tags skip, duplicated known tags reject, and a REQUIRED bit makes a missing
 * section fatal. A later field costs one optional section, not a new version.
 *
 *   version u32
 *   magic "PGRW"                                   (4)
 *   section_count u16                              (2)
 *   envelope_sequence u64                          (8)   == HDR.save_sequence
 *   section*   { tag u16, flags u16, length u32, body[length] }
 *
 * ---------------------------------------------------------------------------
 * **Epoch seconds have an explicit encoding, and this is the comment the
 * milestone asks for.** kilix-state's codec offers u8/u16/u32/u64/i32/bool/
 * bytes/zeroes and *no* i64 — the `kilixstate_write_i64` of ARCHITECTURE.md
 * §11.5 was proposed but has not landed, and this milestone does not add it.
 * Every wall anchor we persist is an `int64_t`, so each one is written as the
 * **little-endian two's-complement bit pattern of the value in a u64**:
 *
 *     write:  kilixstate_write_u64(w, (uint64_t)value)
 *     read:   raw <= INT64_MAX ? (int64_t)raw : -(int64_t)(UINT64_MAX - raw) - 1
 *
 * Both directions are fully defined C: conversion to `uint64_t` is modular by
 * definition, and the read reconstructs the negative case without ever forming
 * an out-of-range signed value. Two's complement in little-endian order is
 * *precisely* the byte sequence §11.5 specifies for `write_i64`, so when that
 * scalar does land it can be dropped in with **no format change and no version
 * bump** — which is why this encoding was chosen over zigzag or a biased
 * offset, both of which would have made the future contribution a migration.
 *
 * `tz_offset_minutes` is the one signed field that is *not* an epoch second: it
 * goes through `u16` and is sign-restored through `int16_t` on read, then
 * clamped to [-720, +840] (D-079, and the §6.2 reconciliation note). The codec
 * has no `i16` and none is being added.
 */
#ifndef PG_SAVE_H
#define PG_SAVE_H

#include "pleb_plant_grower.h"

#include "pg_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- format identity ----------------------------------------------------
 * Versioned from day one. A record whose version this binary does not know is
 * never rewritten: an old binary must not clobber a newer save. */
#define PG_SAVE_VERSION UINT32_C(1)
#define PG_SAVE_MAGIC_BYTES 4u

/* Section tags. Append-only, exactly like the content ids (D-044): a reordered
 * tag silently misreads every existing save. */
typedef enum pg_save_section {
    PG_SAVE_SECTION_HDR = 1,
    PG_SAVE_SECTION_PLNT = 2,
    PG_SAVE_SECTION_JRNL = 3,
    /* Reserved for kilix-story's flag and counter words. Nothing writes it in
     * 0.1 because no story state exists yet; the decoder already knows the tag
     * so that a duplicate is rejected rather than skipped as unknown, and the
     * milestone that adds story state adds a body, not a version. */
    PG_SAVE_SECTION_STRY = 4
} pg_save_section;

#define PG_SAVE_SECTION_REQUIRED 1u

/* Encoded sizes. `_Static_assert`-ed in pg_save.c against the same numbers
 * ARCHITECTURE.md §6.2 quotes, and checked again at runtime after each section
 * is written, so schema drift fails at build time or at the first encode
 * rather than as a KILIXSTATE_TOO_LARGE months later. */
#define PG_SAVE_HDR_BYTES        84u
#define PG_SAVE_PLNT_ENTRY_BYTES 314u
#define PG_SAVE_JRNL_BYTES       (2u + (unsigned)PG_JOURNAL_RING * 7u)
#define PG_SAVE_SECTION_HEADER_BYTES 8u
#define PG_SAVE_ENVELOPE_BYTES (4u + PG_SAVE_MAGIC_BYTES + 2u + 8u)

/* Everything this version can emit, at the largest plant count. */
#define PG_SAVE_WORST_CASE_BYTES \
    (PG_SAVE_ENVELOPE_BYTES \
     + PG_SAVE_SECTION_HEADER_BYTES + PG_SAVE_HDR_BYTES \
     + PG_SAVE_SECTION_HEADER_BYTES \
       + (unsigned)PG_PLANT_MAX * PG_SAVE_PLNT_ENTRY_BYTES \
     + PG_SAVE_SECTION_HEADER_BYTES + PG_SAVE_JRNL_BYTES)

/* The payload ceilings handed to kilix-state. The record uses about a quarter
 * of the plant budget; the rest is deliberate headroom for the optional
 * sections the TLV envelope exists to allow (ARCHITECTURE.md §6.1). */
#define PG_SAVE_MAX_PAYLOAD 4096u
#define PG_SETTINGS_MAX_PAYLOAD 256u

/* settings.state: version u32, three gains, two toggles, one scene id. */
#define PG_SETTINGS_VERSION UINT32_C(1)
#define PG_SETTINGS_BYTES 13u

/* ---- the pure codec (pg_save.c) ----------------------------------------- */

/* `pg_save_encode` / `pg_save_decode` are the public pair
 * (pleb_plant_grower.h). Encode writes the state's own `save_sequence`;
 * the store needs to write a *different* one, because pg_store_save takes a
 * const pg_state and the A/B tiebreak needs the number to advance on every
 * write. That is the only difference between the two. */
bool pg_save_encode_with_sequence(const pg_state *state, uint64_t sequence,
                                  uint8_t *bytes, size_t capacity,
                                  size_t *written);

/* The envelope's sequence without decoding the body — what "re-load before
 * every save" costs. False for anything that is not a record of a version we
 * know. */
bool pg_save_peek_sequence(const uint8_t *bytes, size_t size,
                           uint64_t *sequence);

/* Why the last decode said no. Reset on entry to every decode, so it is only
 * meaningful immediately afterwards. Separated from the bool so that the store
 * can tell "a newer version wrote this" (refuse to write, read-only) from
 * "these bytes are damaged" (use the other generation). */
typedef enum pg_save_reject {
    PG_SAVE_REJECT_NONE = 0,
    PG_SAVE_REJECT_ARGUMENT = 1,
    PG_SAVE_REJECT_TRUNCATED = 2,
    PG_SAVE_REJECT_MALFORMED = 3,   /* magic, tags, lengths, duplicates */
    PG_SAVE_REJECT_INVALID = 4,     /* decodes, but pg_validate refuses it */
    PG_SAVE_REJECT_NEWER_VERSION = 5,
    PG_SAVE_REJECT_COUNT = 6
} pg_save_reject;

uint8_t pg_save_last_reject(void);
const char *pg_save_reject_name(uint8_t reject);

/* Settings are their own tiny record, and their own tiny codec. */
void pg_settings_defaults(pg_settings *settings);
bool pg_settings_encode(const pg_settings *settings, uint8_t *bytes,
                        size_t capacity, size_t *written);
bool pg_settings_decode(pg_settings *settings, const uint8_t *bytes,
                        size_t size);

/* ---- the durable store (pg_store.c) -------------------------------------
 * `pg_store_save` / `pg_store_load` / `pg_settings_load` / `pg_settings_save`
 * are public. Lifecycle is internal: only a frontend opens a store, and both
 * frontends are inside this repository.
 *
 * `base_directory` is the embedding host's data root, or NULL standalone.
 * PLEB_PLANT_CONFIG_HOME overrides both. One option field distinguishes
 * embedded from standalone; there is no code fork (ARCHITECTURE.md §6.1). */
bool pg_store_open(pg_store *store, const char *base_directory);
void pg_store_close(pg_store *store);

/* What the last load or save concluded (ARCHITECTURE.md §6.4), and the line
 * the renderer shows for it. The line is in the plant's register and never
 * blames the player. */
uint8_t pg_store_last_status(const pg_store *store);
const char *pg_store_status_line(uint8_t status);

/* Sticky: set by any save that did not land, cleared only by one that did. A
 * toast would be wiped by the very next screen change. */
bool pg_store_save_failed(const pg_store *store);

/* Which generation a fresh plant must be written into after a damaged load —
 * the failed one, so the readable one survives for a post-mortem. */
uint8_t pg_store_generation(const pg_store *store);

/* Headless diagnostic behind `--save-test <dir>`: the round trip, then the
 * ported corruption harness — every single-bit flip across the record,
 * deterministic multi-bit flips, every truncation length, length-field
 * mutations, and the semantic cases. One rolling FNV hash over the whole run,
 * so a regression is one changed number. Returns 0 when every case behaved.
 * `directory` must be absolute and is used as a kilix-state base directory. */
int pg_save_run_test(const char *directory);

#endif /* PG_SAVE_H */
