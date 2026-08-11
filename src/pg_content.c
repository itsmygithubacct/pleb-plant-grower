/*
 * pg_content — see pg_content.h.
 *
 * The only translation unit that includes the generated header. Everything the
 * tables assert about themselves is asserted twice: once at compile time by
 * _Static_assert, where the constant is knowable then, and once at run time by
 * pg_content_validate(), which --care-test and --rules-test both call before
 * they simulate anything.
 */
#include "pg_content.h"

#include "pg_calendar.h"

#include <stdio.h>
#include <string.h>

#include "pg_content_generated.h"

/* The compiled content and the hand-written constants must agree. These are
 * the four places the same number is stated twice, and a static assert is
 * cheaper than discovering the disagreement as a wrong sunset. */
_Static_assert(PG_CONTENT_SPECIES_COUNT == PG_SPECIES_COUNT,
               "content/plants.json must carry exactly PG_SPECIES_COUNT species");
_Static_assert(PG_CONTENT_POT_COUNT == PG_POT_COUNT,
               "content/pots.json must carry exactly PG_POT_COUNT pots");
_Static_assert(PG_CONTENT_SPOT_COUNT == PG_SPOT_COUNT,
               "content/spots.json must carry exactly PG_SPOT_COUNT spots");
_Static_assert(PG_CONTENT_SOLAR_NOON_LOCAL_MINUTES == PG_SOLAR_NOON_LOCAL_MINUTES,
               "solar noon is authored in content/seasons.json and restated in "
               "the public header; they must not drift");
_Static_assert(PG_CONTENT_HEMISPHERE_OFFSET_DAYS == PG_HEMISPHERE_OFFSET_DAYS,
               "the southern-hemisphere ordinal offset is authored once");
_Static_assert(PG_CONTENT_GROWING_THRESHOLD_L == PG_SEASON_GROWING_L,
               "the 0.45 season threshold is authored once");
_Static_assert(PG_CONTENT_FIRST_GROWING_ORDINAL == PG_SEASON_FIRST_GROWING_ORDINAL,
               "the growing window is stated as ordinals, in one place");
_Static_assert(PG_CONTENT_LAST_GROWING_ORDINAL == PG_SEASON_LAST_GROWING_ORDINAL,
               "the growing window is stated as ordinals, in one place");
_Static_assert(PG_SATURATION_L < PG_LEVEL_MAX,
               "the saturation threshold must sit inside the level scale");
_Static_assert(PG_CARE_TICK_SECONDS * PG_CARE_TICKS_PER_HOUR == 3600,
               "4 care ticks = 1 hour is the conversion every hour-denominated "
               "number in PLANT_CARE.md §9 goes through");
_Static_assert(PG_CARE_TICK_SECONDS * PG_CARE_TICKS_PER_DAY == 86400,
               "96 care ticks = 1 day");

size_t pg_content_species_count(void)
{
    return (size_t)PG_CONTENT_SPECIES_COUNT;
}

size_t pg_content_pot_count(void)
{
    return (size_t)PG_CONTENT_POT_COUNT;
}

size_t pg_content_spot_count(void)
{
    return (size_t)PG_CONTENT_SPOT_COUNT;
}

const pg_species *pg_content_species(uint8_t species_id)
{
    if (species_id >= (uint8_t)PG_CONTENT_SPECIES_COUNT) {
        return NULL;
    }
    return &PG_CONTENT_SPECIES[species_id];
}

const pg_pot *pg_content_pot(uint8_t pot_id)
{
    if (pot_id >= (uint8_t)PG_CONTENT_POT_COUNT) {
        return NULL;
    }
    return &PG_CONTENT_POTS[pot_id];
}

const pg_spot *pg_content_spot(uint8_t spot_id)
{
    if (spot_id >= (uint8_t)PG_CONTENT_SPOT_COUNT) {
        return NULL;
    }
    return &PG_CONTENT_SPOTS[spot_id];
}

const pg_species *pg_content_species_by_key(const char *key)
{
    size_t i;

    if (key == NULL) {
        return NULL;
    }
    for (i = 0; i < (size_t)PG_CONTENT_SPECIES_COUNT; ++i) {
        if (strcmp(PG_CONTENT_SPECIES[i].id, key) == 0) {
            return &PG_CONTENT_SPECIES[i];
        }
    }
    return NULL;
}

const pg_pot *pg_content_pot_by_key(const char *key)
{
    size_t i;

    if (key == NULL) {
        return NULL;
    }
    for (i = 0; i < (size_t)PG_CONTENT_POT_COUNT; ++i) {
        if (strcmp(PG_CONTENT_POTS[i].id, key) == 0) {
            return &PG_CONTENT_POTS[i];
        }
    }
    return NULL;
}

const pg_spot *pg_content_spot_by_key(const char *key)
{
    size_t i;

    if (key == NULL) {
        return NULL;
    }
    for (i = 0; i < (size_t)PG_CONTENT_SPOT_COUNT; ++i) {
        if (strcmp(PG_CONTENT_SPOTS[i].id, key) == 0) {
            return &PG_CONTENT_SPOTS[i];
        }
    }
    return NULL;
}

const pg_multipliers *pg_content_multipliers(void)
{
    return &PG_CONTENT_MULTIPLIERS;
}

const pg_season_month *pg_content_month(uint8_t month)
{
    if (month < 1u || month > (uint8_t)PG_CONTENT_MONTH_COUNT) {
        return NULL;
    }
    return &PG_CONTENT_MONTHS[month - 1u];
}

uint16_t pg_content_saturation_level(void)
{
    return (uint16_t)PG_SATURATION_L;
}

uint16_t pg_content_percolation_q12(void)
{
    return (uint16_t)PG_PERCOLATION_Q12;
}

uint16_t pg_content_recovery_floor_q12(void)
{
    return (uint16_t)PG_RECOVERY_SCALAR_FLOOR_Q12;
}

size_t pg_content_string_count(void)
{
    return (size_t)PG_CONTENT_STRING_COUNT;
}

const char *pg_content_string(size_t index)
{
    if (index >= (size_t)PG_CONTENT_STRING_COUNT) {
        return NULL;
    }
    return PG_CONTENT_STRINGS[index];
}

static bool pg_content_fail(char *error, size_t error_size, const char *what)
{
    if (error != NULL && error_size > 0u) {
        (void)snprintf(error, error_size, "%s", what);
    }
    return false;
}

bool pg_content_validate(char *error, size_t error_size)
{
    size_t i;

    if (error != NULL && error_size > 0u) {
        error[0] = '\0';
    }

    for (i = 0; i < (size_t)PG_CONTENT_SPECIES_COUNT; ++i) {
        const pg_species *sp = &PG_CONTENT_SPECIES[i];
        if ((size_t)sp->numeric_id != i) {
            return pg_content_fail(error, error_size,
                                   "species numeric_id does not match its slot");
        }
        if (sp->thirst_threshold_l >= (uint16_t)PG_LEVEL_MAX) {
            return pg_content_fail(error, error_size,
                                   "thirst threshold is at or above full");
        }
        if (sp->dli_maintenance_c >= sp->dli_thriving_c) {
            return pg_content_fail(error, error_size,
                                   "dli_maintenance_c must sit below dli_thriving_c");
        }
        if (sp->light_min > sp->light_ideal || sp->light_ideal > sp->light_max) {
            return pg_content_fail(error, error_size,
                                   "light_min <= light_ideal <= light_max is violated");
        }
        if ((sp->rh_ideal_low == 0u) != (sp->rh_ideal_high == 0u)) {
            return pg_content_fail(error, error_size,
                                   "the RH indifference sentinel must be set on both ends");
        }
        if (sp->base_drain_growing_q12l == 0u ||
            sp->base_drain_dormant_q12l == 0u) {
            return pg_content_fail(error, error_size,
                                   "a base drain rate of zero would never make the plant thirsty");
        }
        if (sp->base_drain_dormant_q12l >= sp->base_drain_growing_q12l) {
            return pg_content_fail(error, error_size,
                                   "a dormant plant must drink more slowly than a growing one");
        }
        if (sp->saturation_tolerance_ticks !=
            sp->saturation_tolerance_hours * (uint16_t)PG_CARE_TICKS_PER_HOUR) {
            return pg_content_fail(error, error_size,
                                   "saturation tolerance ticks are not hours x 4");
        }
    }

    for (i = 0; i < (size_t)PG_CONTENT_POT_COUNT; ++i) {
        const pg_pot *pot = &PG_CONTENT_POTS[i];
        uint32_t shares;

        if ((size_t)pot->numeric_id != i) {
            return pg_content_fail(error, error_size,
                                   "pot numeric_id does not match its slot");
        }
        shares = (uint32_t)pot->side_evaporation_share_q12
               + (uint32_t)pot->surface_share_q12
               + (uint32_t)pot->uptake_share_q12;
        if (shares != (uint32_t)PG_Q12_ONE) {
            return pg_content_fail(error, error_size,
                                   "the three drain shares must sum to exactly 1.0");
        }
        if (pot->has_drainage && pot->bottom_floor_l != 0u) {
            return pg_content_fail(error, error_size,
                                   "a pot that drains has no perched water table");
        }
        if (!pot->has_drainage &&
            pot->bottom_floor_l <= (uint16_t)PG_SATURATION_L) {
            /* D-082: strictly above, or the soggy ticks decay at rest and the
             * cachepot stops being lethal. */
            return pg_content_fail(error, error_size,
                                   "a pot with no drainage must floor its bottom layer "
                                   "strictly above the saturation threshold");
        }
        if (!pot->has_drainage && pot->can_be_flushed) {
            return pg_content_fail(error, error_size,
                                   "a pot with no drainage cannot be flushed");
        }
    }

    for (i = 0; i < (size_t)PG_CONTENT_SPOT_COUNT; ++i) {
        const pg_spot *spot = &PG_CONTENT_SPOTS[i];
        if ((size_t)spot->numeric_id != i) {
            return pg_content_fail(error, error_size,
                                   "spot numeric_id does not match its slot");
        }
        if (spot->dli_floor_c >= spot->dli_peak_c) {
            return pg_content_fail(error, error_size,
                                   "a spot's midwinter floor must sit below its peak");
        }
    }

    /* D-084, amended by D-093. A threshold nobody can reach is a promise the
     * game cannot keep, so this is asserted in the binary as well as in
     * tools/check_care_schedule.py. */
    for (i = 0; i < (size_t)PG_CONTENT_SPECIES_COUNT; ++i) {
        const pg_species *sp = &PG_CONTENT_SPECIES[i];
        bool reachable = false;
        size_t j;

        for (j = 0; j < (size_t)PG_CONTENT_SPOT_COUNT; ++j) {
            const pg_spot *spot = &PG_CONTENT_SPOTS[j];
            if (spot->light_band > sp->light_max) {
                continue;
            }
            if (spot->dli_peak_c < sp->dli_thriving_c) {
                continue;
            }
            if (sp->dli_flower_c != 0u && spot->dli_peak_c < sp->dli_flower_c) {
                continue;
            }
            if (spot->dli_floor_c < sp->dli_maintenance_c) {
                continue;
            }
            reachable = true;
            break;
        }
        if (!reachable) {
            return pg_content_fail(error, error_size,
                                   "a species threshold is unreachable from every spot "
                                   "inside its light ceiling");
        }
    }

    for (i = 0; i < (size_t)PG_CONTENT_MONTH_COUNT; ++i) {
        if (PG_CONTENT_MONTHS[i].month != (uint8_t)(i + 1u)) {
            return pg_content_fail(error, error_size, "the month table is out of order");
        }
    }

    return true;
}
