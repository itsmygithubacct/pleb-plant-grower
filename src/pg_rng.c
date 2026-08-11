/*
 * pg_rng — see pg_rng.h.
 *
 * splitmix64: one 64-bit add and three xor-shift-multiply rounds. It is chosen
 * over anything with a larger state because the whole state must fit in the
 * save header as a single u64 and must survive a decode with no seeding
 * ceremony, and because every operation is defined on unsigned integers with
 * no undefined shift, no float and no libm — the three properties a bit-exact
 * replay needs.
 */
#include "pg_rng.h"

#define PG_RNG_GAMMA UINT64_C(0x9E3779B97F4A7C15)

static uint64_t pg_rng_mix(uint64_t z)
{
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

void pg_rng_seed(pg_rng *rng, uint64_t seed)
{
    if (rng == NULL) {
        return;
    }
    /* A zero seed is legal and must not produce a degenerate stream: the mix
     * is bijective, so the additive step alone guarantees a full period. */
    rng->state = seed;
}

uint64_t pg_rng_next_u64(pg_rng *rng)
{
    if (rng == NULL) {
        return 0;
    }
    rng->state += PG_RNG_GAMMA;
    return pg_rng_mix(rng->state);
}

uint32_t pg_rng_next_u32(pg_rng *rng)
{
    return (uint32_t)(pg_rng_next_u64(rng) >> 32);
}

uint32_t pg_rng_below(pg_rng *rng, uint32_t bound)
{
    uint32_t limit;
    uint32_t draw;

    if (rng == NULL || bound == 0u) {
        return 0u;
    }
    /* Reject the short tail of the 32-bit range so the result is exactly
     * uniform rather than modulo-biased. */
    limit = UINT32_MAX - (UINT32_MAX % bound);
    do {
        draw = pg_rng_next_u32(rng);
    } while (draw >= limit);
    return draw % bound;
}

uint16_t pg_rng_level(pg_rng *rng)
{
    return (uint16_t)pg_rng_below(rng, (uint32_t)PG_LEVEL_MAX + 1u);
}

bool pg_rng_chance_l(pg_rng *rng, uint16_t probability_l)
{
    if (probability_l == 0u) {
        return false;
    }
    if (probability_l >= (uint16_t)PG_LEVEL_MAX) {
        return true;
    }
    return pg_rng_below(rng, (uint32_t)PG_LEVEL_MAX) < (uint32_t)probability_l;
}

uint16_t pg_rng_hash_level(uint64_t seed, uint64_t coordinate)
{
    uint64_t mixed = pg_rng_mix(seed ^ pg_rng_mix(coordinate + PG_RNG_GAMMA));
    return (uint16_t)((mixed >> 40) % ((uint64_t)PG_LEVEL_MAX + 1u));
}

uint16_t pg_rng_weather_l(uint64_t seed, uint32_t day_ordinal, uint8_t channel)
{
    uint64_t coordinate;

    if (channel >= (uint8_t)PG_WEATHER_CHANNEL_COUNT) {
        return 0u;
    }
    /* The channel occupies the high bits so two channels of the same day can
     * never collide into the same coordinate. */
    coordinate = ((uint64_t)channel << 40) | (uint64_t)day_ordinal;
    return pg_rng_hash_level(seed, coordinate);
}
