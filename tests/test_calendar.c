/*
 * test_calendar — one number in, everything else derived.
 *
 * The assertions here are the ones that stop the calendar acquiring a second
 * copy of the Gregorian leap rule, or a season that snaps at a month boundary,
 * or a local hour that quietly comes from libc.
 */
#include "pg_calendar.h"

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

static bool test_leap_rule(void)
{
    CHECK(pg_calendar_is_leap_year(2024));
    CHECK(pg_calendar_is_leap_year(2000));
    CHECK(!pg_calendar_is_leap_year(1900));
    CHECK(!pg_calendar_is_leap_year(2100));
    CHECK(!pg_calendar_is_leap_year(2023));
    CHECK(pg_calendar_days_in_year(2024) == 366u);
    CHECK(pg_calendar_days_in_year(1900) == 365u);

    CHECK(pg_calendar_days_in_month(2024, 2u) == 29u);
    CHECK(pg_calendar_days_in_month(1900, 2u) == 28u);
    CHECK(pg_calendar_days_in_month(2025, 1u) == 31u);
    CHECK(pg_calendar_days_in_month(2025, 4u) == 30u);
    CHECK(pg_calendar_days_in_month(2025, 12u) == 31u);
    CHECK(pg_calendar_days_in_month(2025, 0u) == 0u);
    CHECK(pg_calendar_days_in_month(2025, 13u) == 0u);
    return true;
}

static bool test_civil_serial_round_trip(void)
{
    int64_t serial;

    CHECK(pg_days_from_civil(1970, 1, 1) == 0);
    CHECK(pg_days_from_civil(1969, 12, 31) == -1);
    CHECK(pg_days_from_civil(2000, 3, 1) - pg_days_from_civil(2000, 2, 28) == 2);
    CHECK(pg_days_from_civil(1900, 3, 1) - pg_days_from_civil(1900, 2, 28) == 1);
    CHECK(pg_days_from_civil(2024, 12, 31) -
          pg_days_from_civil(2024, 1, 1) == 365);
    CHECK(pg_days_from_civil(2023, 12, 31) -
          pg_days_from_civil(2023, 1, 1) == 364);

    /* The serial and the civil date are exact inverses, including across the
     * epoch and across a century that is not a leap year. */
    for (serial = -30000; serial <= 30000; ++serial) {
        int64_t year, month, day;
        pg_civil_from_days(serial, &year, &month, &day);
        CHECK(month >= 1 && month <= 12);
        CHECK(day >= 1 && day <= 31);
        CHECK(pg_days_from_civil(year, month, day) == serial);
    }
    return true;
}

static bool test_date_from_unix(void)
{
    pg_date date;

    CHECK(pg_date_from_unix(0, 0, &date));
    CHECK(date.year == 1970 && date.month == 1u && date.day == 1u);
    CHECK(date.hour == 0u && date.minute == 0u && date.second == 0u);
    CHECK(date.weekday == 3u);       /* the epoch was a Thursday */
    CHECK(date.day_of_year == 1u);

    /* The offset is applied first, so the same instant is a different local
     * day nine hours east and twelve hours west. */
    CHECK(pg_date_from_unix(0, 9 * 3600, &date));
    CHECK(date.day == 1u && date.hour == 9u);
    CHECK(pg_date_from_unix(0, -12 * 3600, &date));
    CHECK(date.year == 1969 && date.month == 12u && date.day == 31u);
    CHECK(date.hour == 12u && date.day_of_year == 365u);

    /* One second before the epoch is the day before, not the day after. */
    CHECK(pg_date_from_unix(-1, 0, &date));
    CHECK(date.year == 1969 && date.month == 12u && date.day == 31u);
    CHECK(date.hour == 23u && date.minute == 59u && date.second == 59u);

    /* The leap day exists in 2000 and 2024, and does not in 1900 or 2100. */
    CHECK(pg_date_from_unix(INT64_C(951782400), 0, &date));
    CHECK(date.year == 2000 && date.month == 2u && date.day == 29u);
    CHECK(date.day_of_year == 60u);
    CHECK(pg_date_from_unix(INT64_C(-2203891200), 0, &date));
    CHECK(date.year == 1900 && date.month == 3u && date.day == 1u);
    CHECK(date.day_of_year == 60u);
    CHECK(pg_date_from_unix(INT64_C(4107542400), 0, &date));
    CHECK(date.year == 2100 && date.month == 3u && date.day == 1u);
    CHECK(date.day_of_year == 60u);

    /* The last second of a leap year is ordinal 366. */
    CHECK(pg_date_from_unix(INT64_C(1735689599), 0, &date));
    CHECK(date.day_of_year == 366u && date.weekday == 1u);

    /* Unrepresentable inputs fail without touching the caller's date. */
    {
        pg_date untouched;
        memset(&untouched, 0, sizeof untouched);
        untouched.year = 4242;
        date = untouched;
        CHECK(!pg_date_from_unix(INT64_MAX, 0, &date));
        CHECK(date.year == 4242);
        CHECK(!pg_date_from_unix(INT64_MIN, 0, &date));
        CHECK(date.year == 4242);
        CHECK(!pg_date_from_unix(INT64_MAX, 3600, &date));
        CHECK(!pg_date_from_unix(INT64_MIN, -3600, &date));
        CHECK(!pg_date_from_unix(0, 0, NULL));
    }
    return true;
}

static bool test_date_to_unix(void)
{
    pg_date date;
    int64_t stamp;

    for (stamp = -400000000; stamp <= 400000000; stamp += 999983) {
        CHECK(pg_date_from_unix(stamp, 0, &date));
        CHECK(pg_date_to_unix(&date, 0) == stamp);
        CHECK(pg_date_from_unix(stamp, 540 * 60, &date));
        CHECK(pg_date_to_unix(&date, 540 * 60) == stamp);
    }

    /* Out-of-range fields normalise the way mktime's do, which is what lets a
     * caller do date arithmetic by addition. */
    memset(&date, 0, sizeof date);
    date.year = 2025;
    date.month = 13u;
    date.day = 1u;
    CHECK(pg_date_to_unix(&date, 0) == pg_days_from_civil(2026, 1, 1) * 86400);
    date.month = 12u;
    date.day = 32u;
    CHECK(pg_date_to_unix(&date, 0) == pg_days_from_civil(2026, 1, 1) * 86400);
    date.month = 2u;
    date.day = 29u;              /* 2025 is not a leap year */
    CHECK(pg_date_to_unix(&date, 0) == pg_days_from_civil(2025, 3, 1) * 86400);
    CHECK(pg_date_to_unix(NULL, 0) == 0);
    return true;
}

static bool test_local_helpers(void)
{
    const int64_t instant = INT64_C(1750000000);   /* 2025-06-15T15:06:40Z */
    pg_date utc;
    pg_date tokyo;

    CHECK(pg_calendar_local(instant, 0, &utc));
    CHECK(pg_calendar_local(instant, 540, &tokyo));
    CHECK(utc.day == 15u && utc.hour == 15u && utc.minute == 6u);
    CHECK(tokyo.day == 16u && tokyo.hour == 0u && tokyo.minute == 6u);
    CHECK(pg_calendar_local_minutes(&utc) == 906u);
    CHECK(pg_calendar_local_minutes(&tokyo) == 6u);
    CHECK(pg_calendar_local_minutes(NULL) == 0u);

    /* A corrupt offset is treated as 0, not clamped to the nearest zone. */
    {
        pg_date corrupt;
        CHECK(pg_calendar_local(instant, 99999, &corrupt));
        CHECK(corrupt.hour == utc.hour && corrupt.minute == utc.minute);
    }

    /* Local midnight is the DLI roll boundary and it really moves. */
    CHECK(instant - pg_calendar_local_midnight_s(instant, 0) == 906 * 60 + 40);
    CHECK(instant - pg_calendar_local_midnight_s(instant, 540) == 6 * 60 + 40);
    CHECK(instant - pg_calendar_local_midnight_s(instant, -300) ==
          (906 - 300) * 60 + 40);
    /* Midnight itself is its own boundary, and the second before is not. */
    {
        int64_t midnight = pg_calendar_local_midnight_s(instant, 0);
        CHECK(pg_calendar_local_midnight_s(midnight, 0) == midnight);
        CHECK(pg_calendar_local_midnight_s(midnight - 1, 0) == midnight - 86400);
    }
    return true;
}

static bool test_season_curve(void)
{
    uint16_t ordinal;
    uint16_t previous;
    uint16_t first = 0u;
    uint16_t last = 0u;

    /* The published window, on the baked table, asserted as ordinals. */
    for (ordinal = 1u; ordinal <= (uint16_t)PG_ORDINAL_MAX; ++ordinal) {
        if (pg_calendar_is_growing(pg_calendar_season_l(ordinal))) {
            if (first == 0u) first = ordinal;
            last = ordinal;
        }
    }
    CHECK(first == (uint16_t)PG_SEASON_FIRST_GROWING_ORDINAL);
    CHECK(last == (uint16_t)PG_SEASON_LAST_GROWING_ORDINAL);

    /* The window has no holes: growing is one contiguous run. */
    for (ordinal = first; ordinal <= last; ++ordinal)
        CHECK(pg_calendar_is_growing(pg_calendar_season_l(ordinal)));
    CHECK(!pg_calendar_is_growing(pg_calendar_season_l((uint16_t)(first - 1u))));
    CHECK(!pg_calendar_is_growing(pg_calendar_season_l((uint16_t)(last + 1u))));

    /* Interpolated, not snapped: the curve rises by at most a few minutes a
     * day and never steps by a month's worth. */
    previous = pg_calendar_daylight_minutes(1u);
    for (ordinal = 2u; ordinal <= (uint16_t)PG_ORDINAL_MAX; ++ordinal) {
        uint16_t current = pg_calendar_daylight_minutes(ordinal);
        int32_t delta = (int32_t)current - (int32_t)previous;
        CHECK(delta <= 4 && delta >= -4);
        previous = current;
    }

    /* Rises to a single midsummer maximum and falls to a midwinter minimum. */
    CHECK(pg_calendar_daylight_minutes(172u) == 930u);
    CHECK(pg_calendar_season_l(172u) == (uint16_t)PG_LEVEL_MAX);
    CHECK(pg_calendar_daylight_minutes(351u) == 534u);
    CHECK(pg_calendar_season_l(351u) < 1000u);

    /* Out-of-range ordinals clamp rather than reading past the table. */
    CHECK(pg_calendar_daylight_minutes(0u) == pg_calendar_daylight_minutes(1u));
    CHECK(pg_calendar_daylight_minutes(9999u) ==
          pg_calendar_daylight_minutes((uint16_t)PG_ORDINAL_MAX));

    /* The recovery floor, which is not the growth rate (D-085). */
    CHECK(pg_calendar_effective_season_l(0u) == 3000u);
    CHECK(pg_calendar_effective_season_l(2999u) == 3000u);
    CHECK(pg_calendar_effective_season_l(9000u) == 9000u);
    return true;
}

static bool test_hemisphere(void)
{
    pg_date midsummer;
    pg_date midwinter;
    uint16_t north, south;

    CHECK(pg_date_from_unix(INT64_C(1750000000), 0, &midsummer));
    north = pg_calendar_season_ordinal(&midsummer,
                                       (uint8_t)PG_HEMISPHERE_NORTHERN);
    south = pg_calendar_season_ordinal(&midsummer,
                                       (uint8_t)PG_HEMISPHERE_SOUTHERN);
    CHECK(north == 166u);
    CHECK(south == 348u);
    CHECK(pg_calendar_is_growing(pg_calendar_season_l(north)));
    CHECK(!pg_calendar_is_growing(pg_calendar_season_l(south)));

    /* And the other way round in January. */
    CHECK(pg_date_from_unix(INT64_C(1736000000), 0, &midwinter));
    CHECK(!pg_calendar_is_growing(pg_calendar_season_l(
        pg_calendar_season_ordinal(&midwinter,
                                   (uint8_t)PG_HEMISPHERE_NORTHERN))));
    CHECK(pg_calendar_is_growing(pg_calendar_season_l(
        pg_calendar_season_ordinal(&midwinter,
                                   (uint8_t)PG_HEMISPHERE_SOUTHERN))));

    /* The rotation wraps inside the year rather than running off the end. */
    {
        pg_date late;
        memset(&late, 0, sizeof late);
        late.year = 2025;
        late.month = 12u;
        late.day = 31u;
        late.day_of_year = 365u;
        CHECK(pg_calendar_season_ordinal(&late,
                                         (uint8_t)PG_HEMISPHERE_SOUTHERN) ==
              182u);
        CHECK(pg_calendar_season_ordinal(NULL,
                                         (uint8_t)PG_HEMISPHERE_NORTHERN) == 1u);
    }
    return true;
}

static bool test_daylight_geometry(void)
{
    uint16_t ordinal;

    /* Sunrise and sunset are symmetric about the solar-noon constant, and the
     * two worked extremes of the design are exactly the documented clock
     * times. */
    for (ordinal = 1u; ordinal <= (uint16_t)PG_ORDINAL_MAX; ++ordinal) {
        uint16_t rise = pg_calendar_sunrise_minutes(ordinal);
        uint16_t set = pg_calendar_sunset_minutes(ordinal);
        CHECK(rise < set);
        CHECK(set < (uint16_t)PG_MINUTES_PER_DAY);
        CHECK((uint16_t)(rise + set) == 2u * PG_SOLAR_NOON_LOCAL_MINUTES);
        CHECK(pg_calendar_fold_minutes(ordinal) ==
              (uint16_t)(set + PG_NYCTINASTY_FOLD_DELAY_MINUTES));
        CHECK(pg_calendar_open_minutes(ordinal) ==
              (uint16_t)(rise + PG_NYCTINASTY_OPEN_DELAY_MINUTES));
        /* The fold must land before local midnight or it never happens. */
        CHECK(pg_calendar_fold_minutes(ordinal) < (uint16_t)PG_MINUTES_PER_DAY);
    }

    CHECK(pg_calendar_sunrise_minutes(172u) == 315u);   /* 05:15 */
    CHECK(pg_calendar_sunset_minutes(172u) == 1245u);   /* 20:45 */
    CHECK(pg_calendar_fold_minutes(172u) == 1305u);     /* 21:45 */
    CHECK(pg_calendar_sunrise_minutes(351u) == 513u);   /* 08:33 */
    CHECK(pg_calendar_sunset_minutes(351u) == 1047u);   /* 17:27 */
    CHECK(pg_calendar_fold_minutes(351u) == 1107u);     /* 18:27 */

    /* Folded through the night, open through the day. */
    CHECK(pg_calendar_is_folded(172u, 0u));
    CHECK(pg_calendar_is_folded(172u, 344u));
    CHECK(!pg_calendar_is_folded(172u, 345u));
    CHECK(!pg_calendar_is_folded(172u, 1304u));
    CHECK(pg_calendar_is_folded(172u, 1305u));
    CHECK(pg_calendar_is_folded(172u, 1439u));
    return true;
}

static bool test_due_date_projection(void)
{
    const int64_t now_wall = INT64_C(1750000000);
    int64_t late_evening;

    CHECK(pg_calendar_project_wall_s(now_wall, 1000u, 1000u + 86400u) ==
          now_wall + 86400);
    /* An overdue date projects to now, never into the past. */
    CHECK(pg_calendar_project_wall_s(now_wall, 5000u, 1000u) == now_wall);
    CHECK(pg_calendar_project_wall_s(now_wall, 5000u, 5000u) == now_wall);
    /* Saturating rather than wrapping at the ends. */
    CHECK(pg_calendar_project_wall_s(now_wall, 0u, UINT64_MAX) == INT64_MAX);
    CHECK(pg_calendar_project_wall_s(INT64_MAX, 0u, 86400u) == INT64_MAX);

    /* Whole local days, counted by calendar boundary, not by 86400 seconds. */
    CHECK(pg_calendar_days_between(now_wall, now_wall, 0) == 0);
    CHECK(pg_calendar_days_between(now_wall, now_wall + 7 * 86400, 0) == 7);
    CHECK(pg_calendar_days_between(now_wall + 7 * 86400, now_wall, 0) == -7);
    late_evening = pg_calendar_local_midnight_s(now_wall, 0) + 23 * 3600;
    CHECK(pg_calendar_days_between(late_evening, late_evening + 7200, 0) == 1);
    CHECK(pg_calendar_days_between(late_evening + 7200, late_evening, 0) == -1);
    /* And the same two instants are one local day nine hours east. */
    CHECK(pg_calendar_days_between(late_evening, late_evening + 7200, 540) == 0);
    return true;
}

static bool test_month_grid(void)
{
    pg_month_grid grid;
    uint8_t index;
    uint8_t in_month = 0u;

    /* February 2024: 29 days, starting on a Thursday. */
    CHECK(pg_calendar_month_grid(2024, 2u, INT64_C(1709164800), 0, &grid));
    CHECK(grid.year == 2024 && grid.month == 2u);
    CHECK(grid.day_count == 29u);
    CHECK(grid.lead == 3u);
    CHECK(grid.row_count == 5u);
    CHECK(grid.today_index == 31u);
    CHECK(grid.cells[0].day == 29u && grid.cells[0].month == 1u);
    CHECK(!grid.cells[0].in_month);
    CHECK(grid.cells[3].day == 1u && grid.cells[3].in_month);
    CHECK(grid.cells[31].day == 29u && grid.cells[31].in_month);
    CHECK(grid.cells[32].day == 1u && grid.cells[32].month == 3u);
    CHECK(!grid.cells[32].in_month);

    /* Exactly day_count cells belong to the month, and the days run in order
     * with no gap across the filler. */
    for (index = 0u; index < (uint8_t)PG_MONTH_GRID_CELLS; ++index) {
        if (grid.cells[index].in_month) ++in_month;
        if (index > 0u) {
            int64_t previous = pg_days_from_civil(grid.cells[index - 1u].year,
                                                  grid.cells[index - 1u].month,
                                                  grid.cells[index - 1u].day);
            int64_t current = pg_days_from_civil(grid.cells[index].year,
                                                 grid.cells[index].month,
                                                 grid.cells[index].day);
            CHECK(current - previous == 1);
        }
    }
    CHECK(in_month == 29u);

    /* March 2025 needs six rows and contains no today. */
    CHECK(pg_calendar_month_grid(2025, 3u, 0, 0, &grid));
    CHECK(grid.lead == 5u);
    CHECK(grid.day_count == 31u);
    CHECK(grid.row_count == 6u);
    CHECK(grid.today_index == (uint8_t)PG_MONTH_GRID_NO_TODAY);

    /* A month that starts on a Monday has no leading filler. */
    CHECK(pg_calendar_month_grid(2025, 9u, 0, 0, &grid));
    CHECK(grid.lead == 0u);
    CHECK(grid.cells[0].day == 1u && grid.cells[0].in_month);
    CHECK(grid.row_count == 5u);

    /* Today is found through the injected offset, not a libc lookup: the same
     * instant is 15 June in UTC and 16 June nine hours east. */
    CHECK(pg_calendar_month_grid(2025, 6u, INT64_C(1750000000), 0, &grid));
    CHECK(grid.cells[grid.today_index].day == 15u);
    CHECK(pg_calendar_month_grid(2025, 6u, INT64_C(1750000000), 540, &grid));
    CHECK(grid.cells[grid.today_index].day == 16u);

    CHECK(!pg_calendar_month_grid(2025, 0u, 0, 0, &grid));
    CHECK(!pg_calendar_month_grid(2025, 13u, 0, 0, &grid));
    CHECK(!pg_calendar_month_grid(0, 1u, 0, 0, &grid));
    CHECK(!pg_calendar_month_grid(2025, 1u, 0, 0, NULL));
    return true;
}

int main(void)
{
    if (!test_leap_rule()) return 1;
    if (!test_civil_serial_round_trip()) return 1;
    if (!test_date_from_unix()) return 1;
    if (!test_date_to_unix()) return 1;
    if (!test_local_helpers()) return 1;
    if (!test_season_curve()) return 1;
    if (!test_hemisphere()) return 1;
    if (!test_daylight_geometry()) return 1;
    if (!test_due_date_projection()) return 1;
    if (!test_month_grid()) return 1;
    (void)puts("calendar: PASS");
    return 0;
}
