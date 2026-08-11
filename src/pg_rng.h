/*
 * pg_rng — the persisted deterministic PRNG.
 *
 * Catch-up must replay bit-exactly: the "while you were away" report is a
 * promise about what actually happened, so a plant that survives a 30-day gap
 * on one machine survives it on every machine and again after a crash. The
 * generator is therefore a fixed-width integer mix with no libm, no libc rand
 * and no platform state, and its whole state is one u64 that lives in the save
 * header (ARCHITECTURE.md §6.2).
 *
 * Two flavours, deliberately separate:
 *
 *   - the *stateful* stream, advanced by the simulation, for anything whose
 *     order of consumption is part of the history (a cutting taking, a pest
 *     arriving);
 *   - the *stateless* hash, for anything that must be a pure function of a
 *     coordinate rather than of history — weather is derived from
 *     (day_ordinal, seed) so a replayed catch-up produces the same July as the
 *     first pass did, no matter how many draws happened in between
 *     (GAME_DESIGN.md §8.2).
 */
#ifndef PG_RNG_H
#define PG_RNG_H

#include "pleb_plant_grower.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct pg_rng {
    uint64_t state;
} pg_rng;

/* Weather channels. Stable, because a save replays its own weather. */
typedef enum pg_weather_channel {
    PG_WEATHER_CLOUD = 0,      /* scales the day's DLI */
    PG_WEATHER_RAIN = 1,       /* fills the jug on the sill */
    PG_WEATHER_HEAT = 2,       /* a July heatwave adds the x1.3 temperature term */
    PG_WEATHER_CHANNEL_COUNT = 3
} pg_weather_channel;

void pg_rng_seed(pg_rng *rng, uint64_t seed);

uint64_t pg_rng_next_u64(pg_rng *rng);
uint32_t pg_rng_next_u32(pg_rng *rng);

/* Uniform over [0, bound); 0 for bound 0. Rejection-sampled, so the result
 * carries no modulo bias — which matters because rot_survival is a real
 * gamble the player is told the odds of. */
uint32_t pg_rng_below(pg_rng *rng, uint32_t bound);

/* Uniform on the level scale, 0..PG_LEVEL_MAX inclusive. */
uint16_t pg_rng_level(pg_rng *rng);

/* True with probability probability_l / PG_LEVEL_MAX. */
bool pg_rng_chance_l(pg_rng *rng, uint16_t probability_l);

/* The stateless hash: a pure function of (seed, coordinate), on the level
 * scale. Advancing the stream never changes it. */
uint16_t pg_rng_hash_level(uint64_t seed, uint64_t coordinate);

/* Deterministic weather for a day ordinal, on the level scale. */
uint16_t pg_rng_weather_l(uint64_t seed, uint32_t day_ordinal,
                          uint8_t channel);

#endif /* PG_RNG_H */
