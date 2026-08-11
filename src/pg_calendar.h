/*
 * pg_calendar — one number in, everything else derived.
 *
 * The calendar stores exactly one thing: int64_t epoch seconds. The civil
 * date, the local hour, the season, sunrise, sunset, the nyctinasty fold, the
 * due-date projection and the month grid are all functions of it plus the
 * injected tz_offset_minutes (D-036, D-079). The Gregorian leap rule is
 * written once, in pg_days_from_civil / pg_civil_from_days, and nowhere else:
 * the anti-pattern is a game that stores a struct of fields and ends up
 * writing that rule three times.
 *
 * Nothing here calls a libc time function, so the whole module is a pure
 * function of its arguments and --calendar-test is byte-identical under any
 * TZ.
 */
#ifndef PG_CALENDAR_H
#define PG_CALENDAR_H

#include "pleb_plant_grower.h"

#include <stdbool.h>
#include <stdint.h>

/* Proleptic-Gregorian decomposition, mirroring the kilix_game_date contract of
 * ARCHITECTURE.md §11.1 field for field so that the kit helper can replace the
 * local implementation without touching a caller. */
typedef struct pg_date {
    int32_t year;
    uint8_t month, day;        /* 1-based */
    uint8_t hour, minute, second;
    uint8_t weekday;           /* 0 = Monday */
    uint16_t day_of_year;      /* 1-based */
} pg_date;

/* The representable span: 0001-01-01T00:00:00Z .. 9999-12-31T23:59:59Z, after
 * the offset has been applied. */
#define PG_UNIX_MIN INT64_C(-62135596800)
#define PG_UNIX_MAX INT64_C(253402300799)

#define PG_SECONDS_PER_DAY INT64_C(86400)
#define PG_MINUTES_PER_DAY 1440

/* Season: the daylight curve is sampled at integer ordinals from a baked
 * 366-entry table, so the growing window is stated as ordinals and a test can
 * assert it (PLANT_CARE.md §4.3). `season_scalar > 0.45` holds for ordinals
 * 72..272 inclusive. */
#define PG_SEASON_GROWING_L 4500
#define PG_SEASON_FIRST_GROWING_ORDINAL 72
#define PG_SEASON_LAST_GROWING_ORDINAL 272
#define PG_ORDINAL_MAX 366

/* Southern-hemisphere saves rotate the ordinal by half a year (D-075). */
#define PG_HEMISPHERE_OFFSET_DAYS 182

/* Nyctinasty hangs off sunset and sunrise, which hang off solar noon (D-080). */
#define PG_NYCTINASTY_FOLD_DELAY_MINUTES 60
#define PG_NYCTINASTY_OPEN_DELAY_MINUTES 30

#define PG_MONTH_GRID_COLUMNS 7
#define PG_MONTH_GRID_ROWS 6
#define PG_MONTH_GRID_CELLS (PG_MONTH_GRID_COLUMNS * PG_MONTH_GRID_ROWS)
#define PG_MONTH_GRID_NO_TODAY 255

typedef struct pg_month_cell {
    int32_t year;
    uint16_t day_of_year;
    uint8_t month;
    uint8_t day;
    bool in_month;      /* false for the leading and trailing filler cells */
} pg_month_cell;

typedef struct pg_month_grid {
    int32_t year;
    uint8_t month;
    uint8_t day_count;   /* days in the month itself */
    uint8_t lead;        /* filler cells before the 1st, Monday-first, 0..6 */
    uint8_t row_count;   /* 4..6 populated rows */
    uint8_t today_index; /* index of today, or PG_MONTH_GRID_NO_TODAY */
    pg_month_cell cells[PG_MONTH_GRID_CELLS];
} pg_month_grid;

/* ---- the leap rule, written once ---- */
bool pg_calendar_is_leap_year(int32_t year);
uint8_t pg_calendar_days_in_month(int32_t year, uint8_t month);
uint16_t pg_calendar_days_in_year(int32_t year);

/* Days since 1970-01-01 for a civil date, and its exact inverse. Both are
 * pure integer arithmetic over the proleptic Gregorian calendar. */
int64_t pg_days_from_civil(int64_t year, int64_t month, int64_t day);
void pg_civil_from_days(int64_t days, int64_t *year, int64_t *month,
                        int64_t *day);

/* Decompose a Unix timestamp with offset_seconds applied first. Returns false
 * only for an unrepresentable result, leaving `date` unmodified. */
bool pg_date_from_unix(int64_t unix_seconds, int32_t offset_seconds,
                       pg_date *date);

/* The inverse. Out-of-range fields normalize the way mktime's do — month 13
 * becomes January of the next year — without mktime's time-zone dependence. */
int64_t pg_date_to_unix(const pg_date *date, int32_t offset_seconds);

/* The local civil date for a wall second, given the injected offset. */
bool pg_calendar_local(int64_t wall_s, int32_t tz_offset_minutes,
                       pg_date *date);

/* Minutes since local midnight. */
uint16_t pg_calendar_local_minutes(const pg_date *date);

/* The wall second of the local midnight that opens `wall_s`'s local day. This
 * is the DLI roll boundary. */
int64_t pg_calendar_local_midnight_s(int64_t wall_s, int32_t tz_offset_minutes);

/* ---- season, daylight and the local-hour rules ---- */

/* The ordinal the season curve is sampled at: the local day of the year,
 * rotated by half a year for a southern-hemisphere save. */
uint16_t pg_calendar_season_ordinal(const pg_date *date, uint8_t hemisphere);

/* Minutes of daylight at an ordinal, from the baked table. Ordinals outside
 * 1..366 are clamped. */
uint16_t pg_calendar_daylight_minutes(uint16_t ordinal);

/* The growth scalar on the level scale, 0..10000 (D-083). */
uint16_t pg_calendar_season_l(uint16_t ordinal);

/* Growing season, as the care rules mean it. */
bool pg_calendar_is_growing(uint16_t season_l);

/* Recovery divides by max(growth_scalar, 0.30) and never by the raw scalar,
 * so a midwinter mistake costs at most 3.33x its summer value (D-085). */
uint16_t pg_calendar_effective_season_l(uint16_t season_l);

uint16_t pg_calendar_sunrise_minutes(uint16_t ordinal);
uint16_t pg_calendar_sunset_minutes(uint16_t ordinal);
uint16_t pg_calendar_fold_minutes(uint16_t ordinal);   /* nyctinasty closes */
uint16_t pg_calendar_open_minutes(uint16_t ordinal);   /* nyctinasty opens */

/* Whether local_minutes falls in the folded part of the day. */
bool pg_calendar_is_folded(uint16_t ordinal, uint16_t local_minutes);

/* ---- due dates, projected out of care time ---- */

/* Due dates are held in care seconds, not wall seconds (ARCHITECTURE.md §6.2).
 * This projects one onto the wall clock for display, on the honest assumption
 * that from now on care time runs at wall speed. Saturating. */
int64_t pg_calendar_project_wall_s(int64_t now_wall_s, uint64_t now_care_s,
                                   uint64_t due_care_s);

/* Whole local days from one wall second to another; negative when the target
 * is in the past. Counts calendar-day boundaries, not 86400-second blocks, so
 * "tomorrow" is 1 at any hour. */
int32_t pg_calendar_days_between(int64_t from_wall_s, int64_t to_wall_s,
                                 int32_t tz_offset_minutes);

/* ---- the month grid the calendar widget draws ---- */

/* Builds a Monday-first six-row grid for (year, month), including the leading
 * and trailing filler cells, and marks today when today's local date falls
 * inside it. Returns false for an unrepresentable month. */
bool pg_calendar_month_grid(int32_t year, uint8_t month, int64_t today_wall_s,
                            int32_t tz_offset_minutes, pg_month_grid *grid);

/* Headless diagnostic behind --calendar-test: prints date arithmetic, the
 * season window, the local-hour rules and a month grid, and returns 0 when
 * every assertion held. Its output is a pure function of its inputs, which is
 * what the TZ=UTC vs TZ=Asia/Tokyo comparison asserts. */
int pg_calendar_run_test(void);

#endif /* PG_CALENDAR_H */
