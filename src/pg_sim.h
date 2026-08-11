/*
 * pg_sim — the catch-up engine's internal seam.
 *
 * `pg_advance` and `pg_next_wall_deadline` are public (pleb_plant_grower.h).
 * What is declared here is what the rest of the core and the tests need in
 * order to talk about a *moment*: the environment at a wall second, the
 * bounds the two tiers promise, the field-wise digest the selftest prints, and
 * the one deliberate test seam that lets a rollback be provoked without
 * corrupting the shipping code path.
 */
#ifndef PG_SIM_H
#define PG_SIM_H

#include "pleb_plant_grower.h"

#include "pg_care.h"
#include "pg_state.h"

#include <stdbool.h>
#include <stdint.h>

/* The two tiers, in ticks and days (ARCHITECTURE.md §5.4).
 *
 * Both tiers integrate the SAME 900 s care tick: the tier decides how finely
 * the gap is *observed*, never how coarsely it is computed. That is what makes
 * the save-equals-ticks equivalence gate (ARCHITECTURE.md §10.1 assertion 2)
 * an identity rather than a tolerance, and it is why the worst case is stated
 * as "2880 fine ticks plus 30 day steps" (D-029, D-043) — 30 days is 2880
 * ticks, and the 30-day credit cap is what bounds it. */
#define PG_SIM_FINE_TICKS 288u    /* 72 h at full journal fidelity */
#define PG_SIM_MAX_TICKS 2880u    /* 30 days is 2880 care ticks (D-029) */
#define PG_SIM_MAX_DAYS 30u       /* local midnights inside a 30-day credit */

/* The season ordinal and local minute of a wall second, under the offset and
 * hemisphere this save is running with, and the environment that follows from
 * them. Out-of-range wall seconds are clamped rather than rejected: a corrupt
 * anchor must not be able to stop the simulation. */
pg_care_env pg_sim_env_at(const pg_state *state, uint8_t plant_index,
                          int64_t wall_s);

/* The environment right now, i.e. at the anchor. This is the one every verb's
 * legality check is evaluated against. */
pg_care_env pg_sim_env_now(const pg_state *state, uint8_t plant_index);

/* Local days since this save's first run, which is what a journal entry's
 * care_day counts. Never negative. */
uint32_t pg_sim_care_day(const pg_state *state, int64_t wall_s);

/* A field-wise FNV-1a digest of the simulated state. Field-wise rather than a
 * memcmp of the bytes, because padding is not part of the simulation and must
 * never be part of the number a test compares. */
uint64_t pg_state_digest(const pg_state *state);

/* Test seam. When `after_ticks` is non-zero the next pg_advance corrupts the
 * state after that many ticks, which must make pg_validate() fail and the
 * whole advance roll back to its entry state. One-shot: the counter is
 * consumed by the advance that trips it. */
void pg_sim_poison_after(uint32_t after_ticks);

/* Headless diagnostic behind --selftest: the seven assertions of
 * ARCHITECTURE.md §10.1, then a digest line. Output is a pure function of
 * (seed, sim_days) — no clock, no pointer, no address is ever printed, which
 * is what makes two runs byte-identical. Returns 0 when every assertion
 * held. */
int pg_sim_run_selftest(uint64_t seed, uint32_t sim_days);

#endif /* PG_SIM_H */
