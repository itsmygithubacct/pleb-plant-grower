/*
 * pg_time — the whole tamper policy, in one file.
 *
 * Stance (ARCHITECTURE.md §5.7): survive broken clocks, do not try to defeat a
 * determined cheater. The real causes are dual-boot RTCs, VM restores, NTP
 * steps and travel, and every one of them must leave a plant that is still
 * worth tending.
 *
 * The rules, all implemented here and nowhere else:
 *   1. care_seconds_total is monotone; no path decreases it.
 *   2. Forward gaps cap at 30 days.
 *   3. Within one boot, CLOCK_BOOTTIME is authoritative.
 *   4. Ignore wall/boot divergence under 300 s — ordinary NTP correction.
 *   5. On a backwards clock, re-anchor to the new earlier wall time so the
 *      anomaly does not re-fire on every launch.
 *   6. tz_offset_minutes is a simulation input, latched per advance; a shift of
 *      60 minutes or more suppresses "the clock jumped" and is not an anomaly.
 *   7. Anomalies are surfaced in character, never punitively.
 */
#include "pg_time.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int64_t pg_time_saturating_sub(int64_t minuend, int64_t subtrahend)
{
    if (subtrahend < 0) {
        if (minuend > INT64_MAX + subtrahend) return INT64_MAX;
    } else {
        if (minuend < INT64_MIN + subtrahend) return INT64_MIN;
    }
    return minuend - subtrahend;
}

int64_t pg_time_saturating_add(int64_t left, int64_t right)
{
    if (right > 0 && left > INT64_MAX - right) return INT64_MAX;
    if (right < 0 && left < INT64_MIN - right) return INT64_MIN;
    return left + right;
}

int64_t pg_time_clamp_i64(int64_t value, int64_t low, int64_t high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

int32_t pg_time_clamp_tz_minutes(int32_t minutes)
{
    /* Out of range is treated as 0 rather than clamped to the nearest legal
     * offset: a value outside [-12:00, +14:00] is a corrupt record, not a
     * place (D-079). */
    if (minutes < PG_TZ_OFFSET_MIN || minutes > PG_TZ_OFFSET_MAX) return 0;
    return minutes;
}

void pg_time_anchor_init(pg_time_anchor *anchor)
{
    if (anchor == NULL) return;
    memset(anchor, 0, sizeof *anchor);
}

void pg_time_anchor_set(pg_time_anchor *anchor, pg_now now)
{
    if (anchor == NULL) return;
    anchor->last_wall_s = now.wall_s;
    anchor->last_boot_s = now.boot_s;
    memcpy(anchor->boot_id, now.boot_id, sizeof anchor->boot_id);
    anchor->tz_offset_minutes = pg_time_clamp_tz_minutes(now.tz_offset_minutes);
    anchor->established = true;
}

static int64_t magnitude_of(int64_t value)
{
    if (value >= 0) return value;
    if (value == INT64_MIN) return INT64_MAX;
    return -value;
}

void pg_time_reconcile(const pg_time_anchor *anchor, pg_now now,
                       pg_time_gap *gap)
{
    int32_t now_tz;
    int64_t trusted;

    if (gap == NULL) return;
    memset(gap, 0, sizeof *gap);
    if (anchor == NULL) return;

    now_tz = pg_time_clamp_tz_minutes(now.tz_offset_minutes);

    if (!anchor->established) {
        /* First run. There is no anchor to measure against, so no gap exists
         * to credit and no clock can be said to have misbehaved. */
        gap->first_run = true;
        return;
    }

    gap->tz_delta_minutes = now_tz - anchor->tz_offset_minutes;
    gap->tz_shifted =
        magnitude_of((int64_t)gap->tz_delta_minutes) >= PG_TIME_TZ_SHIFT_MINUTES;

    gap->wall_gap_s = pg_time_saturating_sub(now.wall_s, anchor->last_wall_s);
    gap->boot_gap_s = pg_time_saturating_sub(now.boot_s, anchor->last_boot_s);
    gap->same_boot = memcmp(now.boot_id, anchor->boot_id,
                            sizeof anchor->boot_id) == 0;
    gap->clock_backwards = gap->wall_gap_s < 0;

    /* A boot clock cannot rewind inside one boot. If it appears to have, the
     * record disagrees with itself — most plausibly a boot id that was
     * unavailable at both ends and read as all zeroes — so fall back to the
     * clamped wall gap, which is the same treatment a genuine reboot gets.
     * This can only ever reduce credit, never manufacture it. */
    if (gap->same_boot && gap->boot_gap_s >= 0) {
        if (gap->wall_gap_s < 0) {
            /* Rule 3: within one boot the boot clock is authoritative, so a
             * wall clock that moved backwards costs the player nothing. */
            trusted = gap->boot_gap_s;
        } else {
            trusted = gap->wall_gap_s < gap->boot_gap_s ? gap->wall_gap_s
                                                        : gap->boot_gap_s;
            /* Rule 4 with rule 6's suppression: divergence is only meaningful
             * when both clocks ran forwards, and a change of place explains it
             * more often than a broken clock does. */
            if (!gap->tz_shifted) {
                int64_t divergence = magnitude_of(
                    pg_time_saturating_sub(gap->wall_gap_s, gap->boot_gap_s));
                gap->clock_jumped = divergence > PG_TIME_NTP_SLACK_SECONDS;
            }
        }
    } else {
        /* No cross-check is possible across a reboot, so the wall gap is all
         * there is. It is floored at zero but deliberately NOT capped here:
         * capping trusted before the abandonment test would make abandonment
         * unreachable across a reboot, which is the commonest way of all to
         * come back after two months. The cap belongs to `credited`, two lines
         * below, exactly as the policy's own last two lines have it. */
        trusted = gap->wall_gap_s;
    }

    if (trusted < 0) trusted = 0;
    gap->trusted_s = trusted;
    gap->credited_s = pg_time_clamp_i64(trusted, 0, PG_GAP_CREDIT_MAX_SECONDS);
    gap->abandoned = trusted > PG_GAP_CREDIT_MAX_SECONDS;
    gap->counts_as_anomaly = gap->clock_backwards || gap->clock_jumped;
}

uint32_t pg_time_credit(pg_care_clock *clock, int64_t credited_s)
{
    int64_t pending;
    uint32_t ticks;

    if (clock == NULL) return 0u;
    credited_s = pg_time_clamp_i64(credited_s, 0, PG_GAP_CREDIT_MAX_SECONDS);
    /* Residual is always below one care tick and credit is capped at 30 days,
     * so the sum cannot leave int64_t and the casts below cannot narrow. */
    pending = (int64_t)clock->care_residual_s + credited_s;
    ticks = (uint32_t)(pending / PG_CARE_TICK_SECONDS);
    clock->care_residual_s = (uint32_t)(pending % PG_CARE_TICK_SECONDS);
    clock->care_seconds_total += (uint64_t)credited_s;
    return ticks;
}

int64_t pg_time_next_tick_wall_s(const pg_time_anchor *anchor,
                                 const pg_care_clock *clock)
{
    int64_t remaining = PG_CARE_TICK_SECONDS;

    if (anchor == NULL || clock == NULL) return INT64_MAX;
    if (clock->care_residual_s < (uint32_t)PG_CARE_TICK_SECONDS)
        remaining = PG_CARE_TICK_SECONDS - (int64_t)clock->care_residual_s;
    return pg_time_saturating_add(anchor->last_wall_s, remaining);
}

/* ------------------------------------------------------------------------- *
 * --time-test: the gap matrix.
 *
 * Every row is one of the situations the policy exists for, with the
 * classification it must produce. Printed as well as asserted, so a
 * regression is legible in a diff and not just a non-zero exit.
 * ------------------------------------------------------------------------- */

#define PG_TIME_TEST_DAY INT64_C(86400)

typedef struct pg_time_case {
    const char *name;
    bool established;
    int64_t anchor_wall_s;
    int64_t anchor_boot_s;
    uint8_t anchor_boot_tag;
    int32_t anchor_tz_minutes;
    int64_t now_wall_s;
    int64_t now_boot_s;
    uint8_t now_boot_tag;
    int32_t now_tz_minutes;
    int64_t expect_trusted_s;
    int64_t expect_credited_s;
    bool expect_abandoned;
    bool expect_backwards;
    bool expect_jumped;
    bool expect_tz_shifted;
    bool expect_anomaly;
} pg_time_case;

static void fill_boot_id(uint8_t out[PG_BOOT_ID_BYTES], uint8_t tag)
{
    size_t index;
    for (index = 0u; index < (size_t)PG_BOOT_ID_BYTES; ++index)
        out[index] = (uint8_t)(tag == 0u ? 0u : (tag + (uint8_t)index));
}

static const pg_time_case pg_time_cases[] = {
    /* name                       est    a_wall            a_boot   a_tag a_tz  n_wall             n_boot          n_tag n_tz   trusted            credited          aband  back   jump   tz     anom */
    { "first-run",                false, 0,                0,       0u,   0,    INT64_C(1750000000), 120,           1u,   0,     0,                 0,                false, false, false, false, false },
    { "idle-one-hour",            true,  INT64_C(1000000), 1000,    1u,   0,    INT64_C(1003600),    4600,          1u,   0,     3600,              3600,             false, false, false, false, false },
    { "laptop-slept-14h",         true,  INT64_C(1000000), 1000,    1u,   0,    INT64_C(1050400),    51400,         1u,   0,     50400,             50400,            false, false, false, false, false },
    { "wall-ran-ahead-a-day",     true,  INT64_C(1000000), 1000,    1u,   0,    INT64_C(1086400),    1600,          1u,   0,     600,               600,              false, false, true,  false, true  },
    { "wall-fell-back-an-hour",   true,  INT64_C(1000000), 1000,    1u,   0,    INT64_C(996400),     1600,          1u,   0,     600,               600,              false, true,  false, false, true  },
    { "ntp-nudge-under-slack",    true,  INT64_C(1000000), 1000,    1u,   0,    INT64_C(1001200),    2000,          1u,   0,     1000,              1000,             false, false, false, false, false },
    { "reboot-two-days",          true,  INT64_C(1000000), 90000,   1u,   0,    INT64_C(1172800),    300,           2u,   0,     172800,            172800,           false, false, false, false, false },
    { "reboot-into-2031",         true,  INT64_C(1000000), 90000,   1u,   0,    INT64_C(1000000) + 400 * PG_TIME_TEST_DAY, 300, 2u, 0, 400 * PG_TIME_TEST_DAY, PG_GAP_CREDIT_MAX_SECONDS, true,  false, false, false, false },
    { "reboot-backwards",         true,  INT64_C(1000000), 90000,   1u,   0,    INT64_C(913600),     300,           2u,   0,     0,                 0,                false, true,  false, false, true  },
    { "flew-to-tokyo",            true,  INT64_C(1000000), 1000,    1u,   0,    INT64_C(1003600),    4600,          1u,   540,   3600,              3600,             false, false, false, true,  false },
    { "flew-and-rtc-was-wrong",   true,  INT64_C(1000000), 1000,    1u,   0,    INT64_C(1086400),    1600,          1u,   540,   600,               600,              false, false, false, true,  false },
    { "boot-clock-rewound",       true,  INT64_C(1000000), 1000,    1u,   0,    INT64_C(1003600),    950,           1u,   0,     3600,              3600,             false, false, false, false, false },
    { "relaunch-same-instant",    true,  INT64_C(1000000), 1000,    1u,   0,    INT64_C(1000000),    1000,          1u,   0,     0,                 0,                false, false, false, false, false },
    { "int64-min-to-int64-max",   true,  INT64_MIN,        INT64_MIN, 1u, 0,    INT64_MAX,           INT64_MAX,     1u,   0,     INT64_MAX,         PG_GAP_CREDIT_MAX_SECONDS, true, false, false, false, false },
    { "corrupt-tz-becomes-zero",  true,  INT64_C(1000000), 1000,    1u,   0,    INT64_C(1003600),    4600,          1u,   99999, 3600,              3600,             false, false, false, false, false }
};

static bool run_gap_case(const pg_time_case *test)
{
    pg_time_anchor anchor;
    pg_time_gap gap;
    pg_now now;
    bool ok;

    pg_time_anchor_init(&anchor);
    anchor.last_wall_s = test->anchor_wall_s;
    anchor.last_boot_s = test->anchor_boot_s;
    fill_boot_id(anchor.boot_id, test->anchor_boot_tag);
    anchor.tz_offset_minutes = test->anchor_tz_minutes;
    anchor.established = test->established;

    memset(&now, 0, sizeof now);
    now.wall_s = test->now_wall_s;
    now.boot_s = test->now_boot_s;
    fill_boot_id(now.boot_id, test->now_boot_tag);
    now.tz_offset_minutes = test->now_tz_minutes;

    pg_time_reconcile(&anchor, now, &gap);

    ok = gap.trusted_s == test->expect_trusted_s &&
         gap.credited_s == test->expect_credited_s &&
         gap.abandoned == test->expect_abandoned &&
         gap.clock_backwards == test->expect_backwards &&
         gap.clock_jumped == test->expect_jumped &&
         gap.tz_shifted == test->expect_tz_shifted &&
         gap.counts_as_anomaly == test->expect_anomaly;

    (void)printf("%-24s wall=%-21" PRId64 " boot=%-21" PRId64
                 " trusted=%-21" PRId64 " credited=%-9" PRId64
                 " aband=%d back=%d jump=%d tz=%d anom=%d %s\n",
                 test->name, gap.wall_gap_s, gap.boot_gap_s, gap.trusted_s,
                 gap.credited_s, gap.abandoned ? 1 : 0,
                 gap.clock_backwards ? 1 : 0, gap.clock_jumped ? 1 : 0,
                 gap.tz_shifted ? 1 : 0, gap.counts_as_anomaly ? 1 : 0,
                 ok ? "ok" : "MISMATCH");
    return ok;
}

int pg_time_run_test(void)
{
    size_t index;
    bool ok = true;
    pg_care_clock clock;
    pg_time_anchor anchor;
    pg_now now;
    uint32_t ticks;

    (void)puts("time: gap matrix");
    for (index = 0u;
         index < sizeof pg_time_cases / sizeof pg_time_cases[0];
         ++index) {
        if (!run_gap_case(&pg_time_cases[index])) ok = false;
    }

    /* The care clock: whole ticks come out, the remainder stays behind, and
     * the total only ever climbs. */
    {
        bool clock_ok;
        memset(&clock, 0, sizeof clock);
        ticks = pg_time_credit(&clock, 3600);
        clock_ok = ticks == 4u && clock.care_residual_s == 0u &&
                   clock.care_seconds_total == 3600u;
        ticks = pg_time_credit(&clock, 899);
        clock_ok = clock_ok && ticks == 0u && clock.care_residual_s == 899u;
        ticks = pg_time_credit(&clock, 1);
        clock_ok = clock_ok && ticks == 1u && clock.care_residual_s == 0u &&
                   clock.care_seconds_total == 4500u;
        ticks = pg_time_credit(&clock, -100000);
        clock_ok = clock_ok && ticks == 0u && clock.care_seconds_total == 4500u;
        (void)printf("care-clock              total=%" PRIu64 " residual=%"
                     PRIu32 " %s\n", clock.care_seconds_total,
                     clock.care_residual_s, clock_ok ? "ok" : "MISMATCH");
        ok = ok && clock_ok;
    }

    /* Re-anchoring is what stops a backwards clock re-firing: the second look
     * at the same instant credits nothing. */
    pg_time_anchor_init(&anchor);
    memset(&now, 0, sizeof now);
    now.wall_s = INT64_C(1700000000);
    now.boot_s = 500;
    now.tz_offset_minutes = 60;
    pg_time_anchor_set(&anchor, now);
    {
        pg_time_gap first;
        pg_time_gap second;
        pg_now earlier = now;
        bool anchor_ok;
        earlier.wall_s -= 7 * PG_TIME_TEST_DAY;
        earlier.boot_s += 30;
        pg_time_reconcile(&anchor, earlier, &first);
        pg_time_anchor_set(&anchor, earlier);
        pg_time_reconcile(&anchor, earlier, &second);
        anchor_ok = first.clock_backwards && !second.clock_backwards &&
                    second.credited_s == 0;
        (void)printf("re-anchor               first_back=%d second_back=%d "
                     "second_credited=%" PRId64 " %s\n",
                     first.clock_backwards ? 1 : 0,
                     second.clock_backwards ? 1 : 0, second.credited_s,
                     anchor_ok ? "ok" : "MISMATCH");
        ok = ok && anchor_ok;
    }

    (void)puts(ok ? "time: PASS" : "time: FAIL");
    return ok ? 0 : 1;
}
