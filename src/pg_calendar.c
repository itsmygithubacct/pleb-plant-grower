/*
 * pg_calendar — epoch seconds in, civil date and season out.
 *
 * The Gregorian leap rule lives in pg_days_from_civil and pg_civil_from_days,
 * once. Everything above them — day of year, weekday, month grids, the DLI
 * roll boundary, due-date projection — is arithmetic on their results.
 *
 * The daylight table below is baked rather than computed, for three reasons:
 * the care model is integer end to end (D-083), a baked table has no libm and
 * therefore no platform-dependent rounding, and a fixed table is what makes
 * "the growing window is ordinals 72..272" an assertion instead of a claim.
 */
#include "pg_calendar.h"

#include "pg_time.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Daylight minutes at 45 degrees north, sampled at each ordinal of a leap
 * year from PLANT_CARE.md §4.3:
 *
 *     daylight_hours(d) = 12.2 + 3.3 * cos(2 * PI * (d - 172) / 365)
 *
 * rounded to whole minutes. Index 0 is ordinal 1. The extremes the table
 * actually contains are 930 minutes (15h30, ordinal 172) and 534 minutes
 * (8h54, ordinal 351), which are the two worked cases in ARCHITECTURE.md §5.8.
 */
static const uint16_t pg_daylight_minutes_table[PG_ORDINAL_MAX] = {
     538,  539,  539,  540,  541,  542,  543,  544,  545,  546,  547,  549,
     550,  551,  553,  554,  556,  557,  559,  561,  562,  564,  566,  568,
     570,  572,  574,  576,  578,  580,  582,  585,  587,  589,  592,  594,
     597,  599,  602,  604,  607,  610,  612,  615,  618,  620,  623,  626,
     629,  632,  635,  638,  641,  644,  647,  650,  653,  656,  660,  663,
     666,  669,  672,  676,  679,  682,  686,  689,  692,  696,  699,  702,
     706,  709,  712,  716,  719,  723,  726,  729,  733,  736,  740,  743,
     746,  750,  753,  757,  760,  763,  767,  770,  773,  777,  780,  783,
     787,  790,  793,  796,  800,  803,  806,  809,  812,  815,  818,  822,
     825,  828,  831,  833,  836,  839,  842,  845,  848,  850,  853,  856,
     858,  861,  864,  866,  869,  871,  874,  876,  878,  880,  883,  885,
     887,  889,  891,  893,  895,  897,  899,  901,  902,  904,  906,  907,
     909,  910,  912,  913,  915,  916,  917,  918,  920,  921,  922,  923,
     923,  924,  925,  926,  926,  927,  928,  928,  929,  929,  929,  930,
     930,  930,  930,  930,  930,  930,  930,  930,  929,  929,  929,  928,
     928,  927,  926,  926,  925,  924,  923,  923,  922,  921,  920,  918,
     917,  916,  915,  913,  912,  910,  909,  907,  906,  904,  902,  901,
     899,  897,  895,  893,  891,  889,  887,  885,  883,  880,  878,  876,
     874,  871,  869,  866,  864,  861,  858,  856,  853,  850,  848,  845,
     842,  839,  836,  833,  831,  828,  825,  822,  818,  815,  812,  809,
     806,  803,  800,  796,  793,  790,  787,  783,  780,  777,  773,  770,
     767,  763,  760,  757,  753,  750,  746,  743,  740,  736,  733,  729,
     726,  723,  719,  716,  712,  709,  706,  702,  699,  696,  692,  689,
     686,  682,  679,  676,  672,  669,  666,  663,  660,  656,  653,  650,
     647,  644,  641,  638,  635,  632,  629,  626,  623,  620,  618,  615,
     612,  610,  607,  604,  602,  599,  597,  594,  592,  589,  587,  585,
     582,  580,  578,  576,  574,  572,  570,  568,  566,  564,  562,  561,
     559,  557,  556,  554,  553,  551,  550,  549,  547,  546,  545,  544,
     543,  542,  541,  540,  539,  539,  538,  537,  537,  536,  536,  535,
     535,  535,  534,  534,  534,  534,  534,  534,  534,  534,  535,  535,
     535,  536,  536,  537,  537,  538
};

/* The two ends of the normalisation that turns daylight into a growth scalar:
 * season_scalar = clamp01((daylight_hours - 8.5) / 7.0), in whole minutes. */
#define PG_SEASON_FLOOR_MINUTES 510   /*  8.5 h */
#define PG_SEASON_SPAN_MINUTES  420   /*  7.0 h */

/* Recovery divides by max(scalar, 0.30), never the raw scalar (D-085). */
#define PG_SEASON_RECOVERY_FLOOR_L 3000

static const uint8_t pg_month_lengths[12] = {
    31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u
};

static int64_t floor_div(int64_t numerator, int64_t denominator)
{
    int64_t quotient = numerator / denominator;
    if ((numerator % denominator != 0) && ((numerator < 0) != (denominator < 0)))
        --quotient;
    return quotient;
}

bool pg_calendar_is_leap_year(int32_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

uint8_t pg_calendar_days_in_month(int32_t year, uint8_t month)
{
    if (month < 1u || month > 12u) return 0u;
    if (month == 2u && pg_calendar_is_leap_year(year)) return 29u;
    return pg_month_lengths[month - 1u];
}

uint16_t pg_calendar_days_in_year(int32_t year)
{
    return pg_calendar_is_leap_year(year) ? 366u : 365u;
}

/* Days from 1970-01-01 to a proleptic-Gregorian civil date. The era shift by
 * 719468 moves the epoch to 0000-03-01, which puts the leap day at the end of
 * the year and removes every special case from the arithmetic below. */
int64_t pg_days_from_civil(int64_t year, int64_t month, int64_t day)
{
    int64_t era, year_of_era, day_of_year, day_of_era;

    year -= month <= 2 ? 1 : 0;
    era = floor_div(year, 400);
    year_of_era = year - era * 400;                       /* [0, 399] */
    day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 +
                 day_of_year;                             /* [0, 146096] */
    return era * 146097 + day_of_era - 719468;
}

void pg_civil_from_days(int64_t days, int64_t *year, int64_t *month,
                        int64_t *day)
{
    int64_t shifted, era, day_of_era, year_of_era, day_of_year, month_prime;
    int64_t out_year, out_month, out_day;

    shifted = days + 719468;
    era = floor_div(shifted, 146097);
    day_of_era = shifted - era * 146097;                  /* [0, 146096] */
    year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
                   day_of_era / 146096) / 365;            /* [0, 399] */
    out_year = year_of_era + era * 400;
    day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 -
                                year_of_era / 100);       /* [0, 365] */
    month_prime = (5 * day_of_year + 2) / 153;            /* [0, 11] */
    out_day = day_of_year - (153 * month_prime + 2) / 5 + 1;
    out_month = month_prime + (month_prime < 10 ? 3 : -9);
    out_year += month_prime >= 10 ? 1 : 0;

    if (year != NULL) *year = out_year;
    if (month != NULL) *month = out_month;
    if (day != NULL) *day = out_day;
}

bool pg_date_from_unix(int64_t unix_seconds, int32_t offset_seconds,
                       pg_date *date)
{
    int64_t local, days, seconds_of_day, year, month, day, january_first;
    pg_date built;

    if (date == NULL) return false;
    if (offset_seconds > 0 && unix_seconds > INT64_MAX - offset_seconds)
        return false;
    if (offset_seconds < 0 && unix_seconds < INT64_MIN - offset_seconds)
        return false;
    local = unix_seconds + offset_seconds;
    if (local < PG_UNIX_MIN || local > PG_UNIX_MAX) return false;

    days = floor_div(local, PG_SECONDS_PER_DAY);
    seconds_of_day = local - days * PG_SECONDS_PER_DAY;
    pg_civil_from_days(days, &year, &month, &day);
    january_first = pg_days_from_civil(year, 1, 1);

    memset(&built, 0, sizeof built);
    built.year = (int32_t)year;
    built.month = (uint8_t)month;
    built.day = (uint8_t)day;
    built.hour = (uint8_t)(seconds_of_day / 3600);
    built.minute = (uint8_t)(seconds_of_day / 60 % 60);
    built.second = (uint8_t)(seconds_of_day % 60);
    /* 1970-01-01 was a Thursday, which is index 3 in a Monday-first week. */
    built.weekday = (uint8_t)(((days % 7) + 7 + 3) % 7);
    built.day_of_year = (uint16_t)(days - january_first + 1);

    *date = built;
    return true;
}

int64_t pg_date_to_unix(const pg_date *date, int32_t offset_seconds)
{
    int64_t year, month, months, days, seconds;

    if (date == NULL) return 0;
    /* Normalise an out-of-range month the way mktime would, so callers may do
     * date arithmetic by addition and hand the result straight back. */
    months = (int64_t)date->month - 1;
    year = (int64_t)date->year + floor_div(months, 12);
    month = months - floor_div(months, 12) * 12 + 1;

    days = pg_days_from_civil(year, month, 1) + (int64_t)date->day - 1;
    seconds = days * PG_SECONDS_PER_DAY + (int64_t)date->hour * 3600 +
              (int64_t)date->minute * 60 + (int64_t)date->second;
    return pg_time_saturating_sub(seconds, (int64_t)offset_seconds);
}

bool pg_calendar_local(int64_t wall_s, int32_t tz_offset_minutes,
                       pg_date *date)
{
    int32_t offset = pg_time_clamp_tz_minutes(tz_offset_minutes);
    return pg_date_from_unix(wall_s, offset * 60, date);
}

uint16_t pg_calendar_local_minutes(const pg_date *date)
{
    if (date == NULL) return 0u;
    return (uint16_t)((uint16_t)date->hour * 60u + date->minute);
}

int64_t pg_calendar_local_midnight_s(int64_t wall_s, int32_t tz_offset_minutes)
{
    int64_t offset = (int64_t)pg_time_clamp_tz_minutes(tz_offset_minutes) * 60;
    int64_t local = pg_time_saturating_add(wall_s, offset);
    int64_t midnight_local = floor_div(local, PG_SECONDS_PER_DAY) *
                             PG_SECONDS_PER_DAY;
    return pg_time_saturating_sub(midnight_local, offset);
}

uint16_t pg_calendar_season_ordinal(const pg_date *date, uint8_t hemisphere)
{
    uint16_t days_in_year, ordinal;

    if (date == NULL) return 1u;
    days_in_year = pg_calendar_days_in_year(date->year);
    ordinal = date->day_of_year;
    if (ordinal < 1u) ordinal = 1u;
    if (ordinal > days_in_year) ordinal = days_in_year;
    if (hemisphere == (uint8_t)PG_HEMISPHERE_SOUTHERN) {
        ordinal = (uint16_t)(((ordinal - 1u + PG_HEMISPHERE_OFFSET_DAYS) %
                              days_in_year) + 1u);
    }
    return ordinal;
}

uint16_t pg_calendar_daylight_minutes(uint16_t ordinal)
{
    if (ordinal < 1u) ordinal = 1u;
    if (ordinal > (uint16_t)PG_ORDINAL_MAX) ordinal = (uint16_t)PG_ORDINAL_MAX;
    return pg_daylight_minutes_table[ordinal - 1u];
}

uint16_t pg_calendar_season_l(uint16_t ordinal)
{
    int32_t minutes = (int32_t)pg_calendar_daylight_minutes(ordinal);
    int32_t level;

    if (minutes <= PG_SEASON_FLOOR_MINUTES) return 0u;
    level = (minutes - PG_SEASON_FLOOR_MINUTES) * PG_LEVEL_MAX /
            PG_SEASON_SPAN_MINUTES;
    if (level > PG_LEVEL_MAX) level = PG_LEVEL_MAX;
    return (uint16_t)level;
}

bool pg_calendar_is_growing(uint16_t season_l)
{
    return season_l > (uint16_t)PG_SEASON_GROWING_L;
}

uint16_t pg_calendar_effective_season_l(uint16_t season_l)
{
    return season_l < (uint16_t)PG_SEASON_RECOVERY_FLOOR_L
               ? (uint16_t)PG_SEASON_RECOVERY_FLOOR_L
               : season_l;
}

uint16_t pg_calendar_sunrise_minutes(uint16_t ordinal)
{
    return (uint16_t)(PG_SOLAR_NOON_LOCAL_MINUTES -
                      pg_calendar_daylight_minutes(ordinal) / 2u);
}

uint16_t pg_calendar_sunset_minutes(uint16_t ordinal)
{
    return (uint16_t)(PG_SOLAR_NOON_LOCAL_MINUTES +
                      pg_calendar_daylight_minutes(ordinal) / 2u);
}

uint16_t pg_calendar_fold_minutes(uint16_t ordinal)
{
    return (uint16_t)(pg_calendar_sunset_minutes(ordinal) +
                      PG_NYCTINASTY_FOLD_DELAY_MINUTES);
}

uint16_t pg_calendar_open_minutes(uint16_t ordinal)
{
    return (uint16_t)(pg_calendar_sunrise_minutes(ordinal) +
                      PG_NYCTINASTY_OPEN_DELAY_MINUTES);
}

bool pg_calendar_is_folded(uint16_t ordinal, uint16_t local_minutes)
{
    uint16_t fold = pg_calendar_fold_minutes(ordinal);
    uint16_t open = pg_calendar_open_minutes(ordinal);
    return local_minutes >= fold || local_minutes < open;
}

int64_t pg_calendar_project_wall_s(int64_t now_wall_s, uint64_t now_care_s,
                                   uint64_t due_care_s)
{
    uint64_t remaining;

    if (due_care_s <= now_care_s) return now_wall_s;
    remaining = due_care_s - now_care_s;
    if (remaining > (uint64_t)INT64_MAX) return INT64_MAX;
    return pg_time_saturating_add(now_wall_s, (int64_t)remaining);
}

int32_t pg_calendar_days_between(int64_t from_wall_s, int64_t to_wall_s,
                                 int32_t tz_offset_minutes)
{
    int64_t from_midnight = pg_calendar_local_midnight_s(from_wall_s,
                                                         tz_offset_minutes);
    int64_t to_midnight = pg_calendar_local_midnight_s(to_wall_s,
                                                       tz_offset_minutes);
    int64_t days = floor_div(pg_time_saturating_sub(to_midnight, from_midnight),
                             PG_SECONDS_PER_DAY);
    if (days > INT32_MAX) return INT32_MAX;
    if (days < INT32_MIN) return INT32_MIN;
    return (int32_t)days;
}

bool pg_calendar_month_grid(int32_t year, uint8_t month, int64_t today_wall_s,
                            int32_t tz_offset_minutes, pg_month_grid *grid)
{
    int64_t first_serial, origin_serial, today_serial;
    pg_date today;
    uint8_t lead, day_count, index;

    if (grid == NULL) return false;
    if (month < 1u || month > 12u) return false;
    if (year < 1 || year > 9999) return false;

    memset(grid, 0, sizeof *grid);
    grid->year = year;
    grid->month = month;
    day_count = pg_calendar_days_in_month(year, month);
    grid->day_count = day_count;

    first_serial = pg_days_from_civil(year, month, 1);
    lead = (uint8_t)(((first_serial % 7) + 7 + 3) % 7);
    grid->lead = lead;
    grid->row_count = (uint8_t)((lead + day_count + 6) / 7);
    grid->today_index = (uint8_t)PG_MONTH_GRID_NO_TODAY;
    origin_serial = first_serial - lead;

    for (index = 0u; index < (uint8_t)PG_MONTH_GRID_CELLS; ++index) {
        pg_date cell_date;
        pg_month_cell *cell = &grid->cells[index];
        int64_t serial = origin_serial + index;
        if (!pg_date_from_unix(serial * PG_SECONDS_PER_DAY, 0, &cell_date))
            return false;
        cell->year = cell_date.year;
        cell->month = cell_date.month;
        cell->day = cell_date.day;
        cell->day_of_year = cell_date.day_of_year;
        cell->in_month = cell_date.year == year && cell_date.month == month;
    }

    if (pg_calendar_local(today_wall_s, tz_offset_minutes, &today)) {
        today_serial = pg_days_from_civil(today.year, today.month, today.day);
        if (today_serial >= origin_serial &&
            today_serial < origin_serial + PG_MONTH_GRID_CELLS)
            grid->today_index = (uint8_t)(today_serial - origin_serial);
    }
    return true;
}

/* ------------------------------------------------------------------------- *
 * --calendar-test
 *
 * Every number printed here is derived from an explicit (wall_s, offset) pair,
 * so the output is a pure function of its inputs. That is exactly what the
 * TZ=UTC vs TZ=Asia/Tokyo comparison in the milestone gate asserts: if any
 * libc time-zone state reached this file, the two runs would differ.
 * ------------------------------------------------------------------------- */

static const char *const pg_weekday_names[7] = {
    "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
};

typedef struct pg_date_case {
    int64_t wall_s;
    int32_t tz_offset_minutes;
    int32_t year;
    uint8_t month, day, hour, minute, second, weekday;
    uint16_t day_of_year;
} pg_date_case;

static const pg_date_case pg_date_cases[] = {
    /* the epoch itself, a Thursday */
    { 0, 0, 1970, 1u, 1u, 0u, 0u, 0u, 3u, 1u },
    /* the same instant seen from Tokyo: same epoch, later local day */
    { 0, 540, 1970, 1u, 1u, 9u, 0u, 0u, 3u, 1u },
    /* and from the other side of the date line */
    { 0, -720, 1969, 12u, 31u, 12u, 0u, 0u, 2u, 365u },
    /* before the epoch */
    { -1, 0, 1969, 12u, 31u, 23u, 59u, 59u, 2u, 365u },
    /* 2000 was a leap year; 29 February exists */
    { INT64_C(951782400), 0, 2000, 2u, 29u, 0u, 0u, 0u, 1u, 60u },
    /* 1900 was not, so 1 March follows 28 February */
    { INT64_C(-2203891200), 0, 1900, 3u, 1u, 0u, 0u, 0u, 3u, 60u },
    /* 2100 will not be either */
    { INT64_C(4107542400), 0, 2100, 3u, 1u, 0u, 0u, 0u, 0u, 60u },
    /* a leap day in a leap year divisible by four only */
    { INT64_C(1709164800), 0, 2024, 2u, 29u, 0u, 0u, 0u, 3u, 60u },
    /* the last ordinal of a leap year */
    { INT64_C(1735689599), 0, 2024, 12u, 31u, 23u, 59u, 59u, 1u, 366u },
    /* a midsummer afternoon, the case the season table is about */
    { INT64_C(1750000000), 0, 2025, 6u, 15u, 15u, 6u, 40u, 6u, 166u }
};

typedef struct pg_season_case {
    uint16_t ordinal;
    uint8_t hemisphere;
} pg_season_case;

static bool report_dates(void)
{
    size_t index;
    bool ok = true;

    (void)puts("calendar: civil dates");
    for (index = 0u; index < sizeof pg_date_cases / sizeof pg_date_cases[0];
         ++index) {
        const pg_date_case *want = &pg_date_cases[index];
        pg_date got;
        bool matched;
        int64_t round_trip;

        if (!pg_calendar_local(want->wall_s, want->tz_offset_minutes, &got)) {
            (void)printf("  %-21" PRId64 " offset=%-5" PRId32 " UNREPRESENTABLE\n",
                         want->wall_s, want->tz_offset_minutes);
            ok = false;
            continue;
        }
        round_trip = pg_date_to_unix(&got, want->tz_offset_minutes * 60);
        matched = got.year == want->year && got.month == want->month &&
                  got.day == want->day && got.hour == want->hour &&
                  got.minute == want->minute && got.second == want->second &&
                  got.weekday == want->weekday &&
                  got.day_of_year == want->day_of_year &&
                  round_trip == want->wall_s;
        if (!matched) ok = false;
        (void)printf("  %-21" PRId64 " offset=%-5" PRId32
                     " %04" PRId32 "-%02u-%02u %02u:%02u:%02u %s doy=%3u %s\n",
                     want->wall_s, want->tz_offset_minutes, got.year,
                     (unsigned)got.month, (unsigned)got.day, (unsigned)got.hour,
                     (unsigned)got.minute, (unsigned)got.second,
                     pg_weekday_names[got.weekday % 7u],
                     (unsigned)got.day_of_year, matched ? "ok" : "MISMATCH");
    }
    return ok;
}

static bool report_round_trip(void)
{
    /* Every day for four centuries around the epoch, at three times of day,
     * plus every day of two leap years at second resolution boundaries. */
    int64_t serial;
    bool ok = true;

    for (serial = -73000; serial <= 73000; ++serial) {
        static const int64_t offsets_of_day[3] = { 0, 43200, 86399 };
        size_t slot;
        for (slot = 0u; slot < 3u; ++slot) {
            int64_t stamp = serial * PG_SECONDS_PER_DAY + offsets_of_day[slot];
            pg_date date;
            if (!pg_date_from_unix(stamp, 0, &date)) { ok = false; break; }
            if (pg_date_to_unix(&date, 0) != stamp) { ok = false; break; }
            if (date.day_of_year < 1u ||
                date.day_of_year > pg_calendar_days_in_year(date.year)) {
                ok = false;
                break;
            }
            if (date.day > pg_calendar_days_in_month(date.year, date.month)) {
                ok = false;
                break;
            }
        }
        if (!ok) break;
    }
    (void)printf("calendar: round trip over 146001 days x 3 times %s\n",
                 ok ? "ok" : "MISMATCH");
    return ok;
}

static bool report_season(void)
{
    uint16_t ordinal;
    uint16_t first_growing = 0u;
    uint16_t last_growing = 0u;
    bool ok = true;

    for (ordinal = 1u; ordinal <= (uint16_t)PG_ORDINAL_MAX; ++ordinal) {
        if (pg_calendar_is_growing(pg_calendar_season_l(ordinal))) {
            if (first_growing == 0u) first_growing = ordinal;
            last_growing = ordinal;
        }
    }
    ok = first_growing == (uint16_t)PG_SEASON_FIRST_GROWING_ORDINAL &&
         last_growing == (uint16_t)PG_SEASON_LAST_GROWING_ORDINAL;
    (void)printf("calendar: growing window ordinals %u..%u %s\n",
                 (unsigned)first_growing, (unsigned)last_growing,
                 ok ? "ok" : "MISMATCH");

    (void)puts("calendar: the year at 30-day intervals");
    for (ordinal = 1u; ordinal <= (uint16_t)PG_ORDINAL_MAX; ordinal =
             (uint16_t)(ordinal + 30u)) {
        uint16_t daylight = pg_calendar_daylight_minutes(ordinal);
        uint16_t season = pg_calendar_season_l(ordinal);
        (void)printf("  ordinal %3u daylight=%3um season_l=%5u %-8s "
                     "sunrise=%02u:%02u sunset=%02u:%02u fold=%02u:%02u "
                     "open=%02u:%02u\n",
                     (unsigned)ordinal, (unsigned)daylight, (unsigned)season,
                     pg_calendar_is_growing(season) ? "growing" : "dormant",
                     (unsigned)(pg_calendar_sunrise_minutes(ordinal) / 60u),
                     (unsigned)(pg_calendar_sunrise_minutes(ordinal) % 60u),
                     (unsigned)(pg_calendar_sunset_minutes(ordinal) / 60u),
                     (unsigned)(pg_calendar_sunset_minutes(ordinal) % 60u),
                     (unsigned)(pg_calendar_fold_minutes(ordinal) / 60u),
                     (unsigned)(pg_calendar_fold_minutes(ordinal) % 60u),
                     (unsigned)(pg_calendar_open_minutes(ordinal) / 60u),
                     (unsigned)(pg_calendar_open_minutes(ordinal) % 60u));
    }

    /* The two worked extremes of ARCHITECTURE.md §5.8. */
    {
        bool extremes = pg_calendar_daylight_minutes(172u) == 930u &&
                        pg_calendar_sunrise_minutes(172u) == 315u &&
                        pg_calendar_sunset_minutes(172u) == 1245u &&
                        pg_calendar_fold_minutes(172u) == 1305u &&
                        pg_calendar_daylight_minutes(351u) == 534u &&
                        pg_calendar_sunrise_minutes(351u) == 513u &&
                        pg_calendar_sunset_minutes(351u) == 1047u &&
                        pg_calendar_fold_minutes(351u) == 1107u;
        (void)printf("calendar: midsummer 05:15/20:45/21:45 and "
                     "midwinter 08:33/17:27/18:27 %s\n",
                     extremes ? "ok" : "MISMATCH");
        ok = ok && extremes;
    }

    /* The hemisphere flag rotates the ordinal by half a year, so a southern
     * midwinter is a northern midsummer (D-075). */
    {
        pg_date midsummer;
        uint16_t northern, southern;
        bool rotated;
        (void)pg_date_from_unix(INT64_C(1750000000), 0, &midsummer);
        northern = pg_calendar_season_ordinal(&midsummer,
                                              (uint8_t)PG_HEMISPHERE_NORTHERN);
        southern = pg_calendar_season_ordinal(&midsummer,
                                              (uint8_t)PG_HEMISPHERE_SOUTHERN);
        rotated = northern == 166u && southern == 348u &&
                  pg_calendar_is_growing(pg_calendar_season_l(northern)) &&
                  !pg_calendar_is_growing(pg_calendar_season_l(southern));
        (void)printf("calendar: hemisphere ordinal north=%u south=%u %s\n",
                     (unsigned)northern, (unsigned)southern,
                     rotated ? "ok" : "MISMATCH");
        ok = ok && rotated;
    }

    /* The recovery floor never lets the divisor reach the raw midwinter
     * scalar (D-085). */
    {
        uint16_t raw = pg_calendar_season_l(351u);
        uint16_t floored = pg_calendar_effective_season_l(raw);
        bool floor_ok = raw < (uint16_t)PG_SEASON_RECOVERY_FLOOR_L &&
                        floored == (uint16_t)PG_SEASON_RECOVERY_FLOOR_L &&
                        pg_calendar_effective_season_l(10000u) == 10000u;
        (void)printf("calendar: recovery floor raw=%u effective=%u %s\n",
                     (unsigned)raw, (unsigned)floored,
                     floor_ok ? "ok" : "MISMATCH");
        ok = ok && floor_ok;
    }
    return ok;
}

static bool report_local_rules(void)
{
    /* One instant, two offsets. The epoch is untouched by the offset — no time
     * is created or destroyed — but every local-hour rule moves (D-079). */
    const int64_t instant = INT64_C(1750000000);
    pg_date utc;
    pg_date tokyo;
    bool ok;

    if (!pg_calendar_local(instant, 0, &utc)) return false;
    if (!pg_calendar_local(instant, 540, &tokyo)) return false;

    ok = pg_calendar_local_minutes(&utc) == 906u &&
         pg_calendar_local_minutes(&tokyo) == 6u &&
         utc.day == 15u && tokyo.day == 16u;

    (void)printf("calendar: one instant  utc=%02u:%02u day=%u  "
                 "tokyo=%02u:%02u day=%u %s\n",
                 (unsigned)utc.hour, (unsigned)utc.minute, (unsigned)utc.day,
                 (unsigned)tokyo.hour, (unsigned)tokyo.minute,
                 (unsigned)tokyo.day, ok ? "ok" : "MISMATCH");

    /* The DLI roll boundary is local midnight, and it really moves. */
    {
        int64_t midnight_utc = pg_calendar_local_midnight_s(instant, 0);
        int64_t midnight_tokyo = pg_calendar_local_midnight_s(instant, 540);
        bool rolled = instant - midnight_utc == 906 * 60 + 40 &&
                      instant - midnight_tokyo == 6 * 60 + 40 &&
                      midnight_tokyo > midnight_utc;
        (void)printf("calendar: local midnight utc=%" PRId64 " tokyo=%" PRId64
                     " %s\n", midnight_utc, midnight_tokyo,
                     rolled ? "ok" : "MISMATCH");
        ok = ok && rolled;
    }

    /* Nyctinasty: folded after the fold minute, open after the open minute. */
    {
        uint16_t ordinal = 172u;
        bool folded = pg_calendar_is_folded(ordinal, 1310u) &&
                      pg_calendar_is_folded(ordinal, 300u) &&
                      !pg_calendar_is_folded(ordinal, 1300u) &&
                      !pg_calendar_is_folded(ordinal, 720u) &&
                      !pg_calendar_is_folded(ordinal, 345u) &&
                      pg_calendar_is_folded(ordinal, 344u);
        (void)printf("calendar: nyctinasty fold=%u open=%u %s\n",
                     (unsigned)pg_calendar_fold_minutes(ordinal),
                     (unsigned)pg_calendar_open_minutes(ordinal),
                     folded ? "ok" : "MISMATCH");
        ok = ok && folded;
    }
    return ok;
}

static bool report_due_dates(void)
{
    const int64_t now_wall = INT64_C(1750000000);
    int64_t late_evening;
    bool ok;
    int64_t due;

    /* Due dates live in care seconds. Projecting one forward assumes care time
     * runs at wall speed from here, which is the only honest forecast. */
    due = pg_calendar_project_wall_s(now_wall, 100000u,
                                     100000u + 7u * 86400u);
    ok = due == now_wall + 7 * 86400;
    ok = ok && pg_calendar_project_wall_s(now_wall, 100000u, 50u) == now_wall;
    ok = ok && pg_calendar_days_between(now_wall, due, 0) == 7;
    /* Whole local days, not 86400-second blocks: 23:00 to 01:00 is tomorrow,
     * and the same two instants are the same day nine hours east. */
    late_evening = pg_calendar_local_midnight_s(now_wall, 0) + 23 * 3600;
    ok = ok && pg_calendar_days_between(late_evening, late_evening + 7200, 0) == 1;
    ok = ok && pg_calendar_days_between(late_evening + 7200, late_evening, 0) == -1;
    ok = ok && pg_calendar_days_between(late_evening, late_evening + 7200, 540) == 0;
    (void)printf("calendar: due date +7 care-days -> wall %" PRId64
                 " (%" PRId32 " local days) %s\n", due,
                 pg_calendar_days_between(now_wall, due, 0),
                 ok ? "ok" : "MISMATCH");
    return ok;
}

static bool report_month_grid(void)
{
    pg_month_grid grid;
    bool ok;
    uint8_t row, column;

    /* February 2024: a leap February that starts on a Thursday. */
    if (!pg_calendar_month_grid(2024, 2u, INT64_C(1709164800), 0, &grid))
        return false;
    ok = grid.day_count == 29u && grid.lead == 3u && grid.row_count == 5u &&
         grid.today_index == 31u && grid.cells[0].day == 29u &&
         grid.cells[0].month == 1u && !grid.cells[0].in_month &&
         grid.cells[3].day == 1u && grid.cells[3].in_month &&
         grid.cells[31].day == 29u && grid.cells[31].in_month &&
         grid.cells[32].day == 1u && grid.cells[32].month == 3u &&
         !grid.cells[32].in_month;

    (void)printf("calendar: month grid %04" PRId32 "-%02u lead=%u rows=%u "
                 "days=%u today=%u %s\n", grid.year, (unsigned)grid.month,
                 (unsigned)grid.lead, (unsigned)grid.row_count,
                 (unsigned)grid.day_count, (unsigned)grid.today_index,
                 ok ? "ok" : "MISMATCH");
    (void)puts("  Mon Tue Wed Thu Fri Sat Sun");
    for (row = 0u; row < (uint8_t)PG_MONTH_GRID_ROWS; ++row) {
        (void)fputs(" ", stdout);
        for (column = 0u; column < (uint8_t)PG_MONTH_GRID_COLUMNS; ++column) {
            const pg_month_cell *cell =
                &grid.cells[row * PG_MONTH_GRID_COLUMNS + column];
            char marker = ' ';
            if (row * PG_MONTH_GRID_COLUMNS + column == grid.today_index)
                marker = '*';
            else if (!cell->in_month)
                marker = '.';
            (void)printf("%c%3u", marker, (unsigned)cell->day);
        }
        (void)fputs("\n", stdout);
    }

    /* A month that needs six rows, and one with no today in it. */
    {
        pg_month_grid six;
        bool six_ok = pg_calendar_month_grid(2025, 3u, 0, 0, &six) &&
                      six.lead == 5u && six.day_count == 31u &&
                      six.row_count == 6u &&
                      six.today_index == (uint8_t)PG_MONTH_GRID_NO_TODAY;
        (void)printf("calendar: month grid 2025-03 lead=%u rows=%u today=%u %s\n",
                     (unsigned)six.lead, (unsigned)six.row_count,
                     (unsigned)six.today_index, six_ok ? "ok" : "MISMATCH");
        ok = ok && six_ok;
    }
    return ok;
}

int pg_calendar_run_test(void)
{
    bool ok = true;

    if (!report_dates()) ok = false;
    if (!report_round_trip()) ok = false;
    if (!report_season()) ok = false;
    if (!report_local_rules()) ok = false;
    if (!report_due_dates()) ok = false;
    if (!report_month_grid()) ok = false;

    (void)puts(ok ? "calendar: PASS" : "calendar: FAIL");
    return ok ? 0 : 1;
}
