/*
 * test_time — the tamper policy, case by case.
 *
 * The gap matrix printed by --time-test is the readable version of this; these
 * are the assertions that must not be allowed to drift, including the two the
 * whole module exists for: saturating subtraction at the ends of int64_t, and
 * a care clock that cannot be made to run backwards.
 */
#include "pg_time.h"

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

static void set_boot_id(uint8_t out[PG_BOOT_ID_BYTES], uint8_t tag)
{
    size_t index;
    for (index = 0u; index < (size_t)PG_BOOT_ID_BYTES; ++index)
        out[index] = (uint8_t)(tag + (uint8_t)index);
}

static pg_now make_now(int64_t wall_s, int64_t boot_s, uint8_t tag,
                       int32_t tz_minutes)
{
    pg_now now;
    memset(&now, 0, sizeof now);
    now.wall_s = wall_s;
    now.boot_s = boot_s;
    set_boot_id(now.boot_id, tag);
    now.tz_offset_minutes = tz_minutes;
    return now;
}

static pg_time_anchor make_anchor(int64_t wall_s, int64_t boot_s, uint8_t tag,
                                  int32_t tz_minutes)
{
    pg_time_anchor anchor;
    pg_time_anchor_init(&anchor);
    pg_time_anchor_set(&anchor, make_now(wall_s, boot_s, tag, tz_minutes));
    return anchor;
}

static bool test_saturating_arithmetic(void)
{
    /* The direct lesson of the signed-overflow defect in the kit's own loop:
     * a legal INT64_MIN -> INT64_MAX transition must not overflow. */
    CHECK(pg_time_saturating_sub(INT64_MAX, INT64_MIN) == INT64_MAX);
    CHECK(pg_time_saturating_sub(INT64_MIN, INT64_MAX) == INT64_MIN);
    CHECK(pg_time_saturating_sub(INT64_MIN, 1) == INT64_MIN);
    CHECK(pg_time_saturating_sub(INT64_MAX, -1) == INT64_MAX);
    CHECK(pg_time_saturating_sub(0, INT64_MIN) == INT64_MAX);
    CHECK(pg_time_saturating_sub(10, 4) == 6);
    CHECK(pg_time_saturating_sub(4, 10) == -6);

    CHECK(pg_time_saturating_add(INT64_MAX, 1) == INT64_MAX);
    CHECK(pg_time_saturating_add(INT64_MIN, -1) == INT64_MIN);
    CHECK(pg_time_saturating_add(INT64_MAX, INT64_MAX) == INT64_MAX);
    CHECK(pg_time_saturating_add(INT64_MIN, INT64_MIN) == INT64_MIN);
    CHECK(pg_time_saturating_add(-5, 5) == 0);

    CHECK(pg_time_clamp_i64(5, 0, 10) == 5);
    CHECK(pg_time_clamp_i64(-5, 0, 10) == 0);
    CHECK(pg_time_clamp_i64(50, 0, 10) == 10);
    return true;
}

static bool test_tz_clamp(void)
{
    CHECK(pg_time_clamp_tz_minutes(0) == 0);
    CHECK(pg_time_clamp_tz_minutes(540) == 540);
    CHECK(pg_time_clamp_tz_minutes(PG_TZ_OFFSET_MIN) == PG_TZ_OFFSET_MIN);
    CHECK(pg_time_clamp_tz_minutes(PG_TZ_OFFSET_MAX) == PG_TZ_OFFSET_MAX);
    /* Outside the legal band is a corrupt record, not a place. */
    CHECK(pg_time_clamp_tz_minutes(PG_TZ_OFFSET_MAX + 1) == 0);
    CHECK(pg_time_clamp_tz_minutes(PG_TZ_OFFSET_MIN - 1) == 0);
    CHECK(pg_time_clamp_tz_minutes(INT32_MAX) == 0);
    CHECK(pg_time_clamp_tz_minutes(INT32_MIN) == 0);
    return true;
}

static bool test_first_run(void)
{
    pg_time_anchor anchor;
    pg_time_gap gap;

    pg_time_anchor_init(&anchor);
    pg_time_reconcile(&anchor, make_now(INT64_C(1750000000), 300, 1u, 0), &gap);
    CHECK(gap.first_run);
    CHECK(gap.credited_s == 0);
    CHECK(gap.trusted_s == 0);
    CHECK(!gap.abandoned);
    CHECK(!gap.counts_as_anomaly);
    return true;
}

static bool test_same_boot_is_authoritative(void)
{
    pg_time_anchor anchor = make_anchor(INT64_C(1000000), 1000, 1u, 0);
    pg_time_gap gap;

    /* Both clocks agree: credit the whole gap. */
    pg_time_reconcile(&anchor, make_now(INT64_C(1003600), 4600, 1u, 0), &gap);
    CHECK(gap.same_boot);
    CHECK(gap.credited_s == 3600);
    CHECK(!gap.counts_as_anomaly);

    /* Wall ran ahead: the boot clock, which the user cannot move, wins. */
    pg_time_reconcile(&anchor, make_now(INT64_C(1086400), 1600, 1u, 0), &gap);
    CHECK(gap.credited_s == 600);
    CHECK(gap.clock_jumped);
    CHECK(gap.counts_as_anomaly);

    /* Wall fell back: the player still gets the boot clock's elapsed time. */
    pg_time_reconcile(&anchor, make_now(INT64_C(996400), 1600, 1u, 0), &gap);
    CHECK(gap.credited_s == 600);
    CHECK(gap.clock_backwards);
    CHECK(!gap.clock_jumped);
    CHECK(gap.counts_as_anomaly);

    /* Ordinary NTP correction is beneath notice. */
    pg_time_reconcile(&anchor, make_now(INT64_C(1001200), 2000, 1u, 0), &gap);
    CHECK(gap.credited_s == 1000);
    CHECK(!gap.clock_jumped);
    CHECK(!gap.counts_as_anomaly);

    /* Exactly at the slack boundary is still ordinary. */
    pg_time_reconcile(&anchor, make_now(INT64_C(1001300), 2000, 1u, 0), &gap);
    CHECK(gap.wall_gap_s - gap.boot_gap_s == PG_TIME_NTP_SLACK_SECONDS);
    CHECK(!gap.clock_jumped);
    return true;
}

static bool test_boot_change_falls_back_to_wall(void)
{
    pg_time_anchor anchor = make_anchor(INT64_C(1000000), 90000, 1u, 0);
    pg_time_gap gap;

    pg_time_reconcile(&anchor, make_now(INT64_C(1172800), 300, 2u, 0), &gap);
    CHECK(!gap.same_boot);
    CHECK(gap.credited_s == 172800);
    CHECK(!gap.abandoned);

    /* A backwards wall clock across a reboot cannot be cross-checked, so it
     * credits nothing rather than anything negative. */
    pg_time_reconcile(&anchor, make_now(INT64_C(913600), 300, 2u, 0), &gap);
    CHECK(gap.credited_s == 0);
    CHECK(gap.clock_backwards);
    CHECK(gap.counts_as_anomaly);

    /* A boot clock that appears to have rewound inside one boot means the
     * record disagrees with itself — most plausibly an unavailable boot id
     * read as zeroes at both ends — so the clamped wall gap is used. */
    {
        pg_time_anchor zeroed;
        pg_now now;
        pg_time_anchor_init(&zeroed);
        zeroed.last_wall_s = INT64_C(1000000);
        zeroed.last_boot_s = 900000;
        zeroed.established = true;
        now = make_now(INT64_C(1003600), 50, 0u, 0);
        memset(now.boot_id, 0, sizeof now.boot_id);
        pg_time_reconcile(&zeroed, now, &gap);
        CHECK(gap.same_boot);
        CHECK(gap.boot_gap_s < 0);
        CHECK(gap.credited_s == 3600);
    }
    return true;
}

static bool test_cap_and_abandonment(void)
{
    pg_time_anchor anchor = make_anchor(INT64_C(1000000), 90000, 1u, 0);
    pg_time_gap gap;

    /* Exactly at the cap is not abandonment. */
    pg_time_reconcile(&anchor,
                      make_now(INT64_C(1000000) + PG_GAP_CREDIT_MAX_SECONDS,
                               300, 2u, 0), &gap);
    CHECK(gap.credited_s == PG_GAP_CREDIT_MAX_SECONDS);
    CHECK(!gap.abandoned);

    /* One second past it is. */
    pg_time_reconcile(&anchor,
                      make_now(INT64_C(1000000) + PG_GAP_CREDIT_MAX_SECONDS + 1,
                               300, 2u, 0), &gap);
    CHECK(gap.abandoned);
    CHECK(gap.credited_s == PG_GAP_CREDIT_MAX_SECONDS);

    /* Jumping to 2031 gives exactly what waiting a month gives. */
    pg_time_reconcile(&anchor, make_now(INT64_MAX, INT64_MAX, 1u, 0), &gap);
    CHECK(gap.credited_s == PG_GAP_CREDIT_MAX_SECONDS);
    CHECK(gap.abandoned);
    return true;
}

static bool test_timezone_shift(void)
{
    pg_time_anchor anchor = make_anchor(INT64_C(1000000), 1000, 1u, 0);
    pg_time_gap gap;

    /* A flight: the offset moved, the epoch did not, and nothing is an
     * anomaly. */
    pg_time_reconcile(&anchor, make_now(INT64_C(1003600), 4600, 1u, 540), &gap);
    CHECK(gap.tz_shifted);
    CHECK(gap.tz_delta_minutes == 540);
    CHECK(gap.credited_s == 3600);
    CHECK(!gap.counts_as_anomaly);

    /* A flight plus a wrong RTC: the jump message is suppressed, but the
     * boot clock still bounds the credit. */
    pg_time_reconcile(&anchor, make_now(INT64_C(1086400), 1600, 1u, 540), &gap);
    CHECK(gap.tz_shifted);
    CHECK(!gap.clock_jumped);
    CHECK(!gap.counts_as_anomaly);
    CHECK(gap.credited_s == 600);

    /* A half-hour zone is under the threshold and does not suppress. */
    pg_time_reconcile(&anchor, make_now(INT64_C(1086400), 1600, 1u, 30), &gap);
    CHECK(!gap.tz_shifted);
    CHECK(gap.clock_jumped);

    /* Exactly 60 minutes is a shift; 59 is not. */
    pg_time_reconcile(&anchor, make_now(INT64_C(1003600), 4600, 1u, 60), &gap);
    CHECK(gap.tz_shifted);
    pg_time_reconcile(&anchor, make_now(INT64_C(1003600), 4600, 1u, 59), &gap);
    CHECK(!gap.tz_shifted);

    /* Westward too. */
    pg_time_reconcile(&anchor, make_now(INT64_C(1003600), 4600, 1u, -300), &gap);
    CHECK(gap.tz_shifted);
    CHECK(gap.tz_delta_minutes == -300);
    return true;
}

static bool test_backwards_clock_does_not_refire(void)
{
    pg_time_anchor anchor = make_anchor(INT64_C(1700000000), 500, 1u, 0);
    pg_time_gap gap;
    pg_now earlier = make_now(INT64_C(1700000000) - 7 * 86400, 530, 1u, 0);

    pg_time_reconcile(&anchor, earlier, &gap);
    CHECK(gap.clock_backwards);

    /* Rule 5: re-anchor to the new, earlier wall time. The second launch at
     * the same instant is unremarkable. */
    pg_time_anchor_set(&anchor, earlier);
    pg_time_reconcile(&anchor, earlier, &gap);
    CHECK(!gap.clock_backwards);
    CHECK(gap.credited_s == 0);
    CHECK(!gap.counts_as_anomaly);
    CHECK(anchor.last_wall_s == earlier.wall_s);
    return true;
}

static bool test_care_clock_is_monotone(void)
{
    pg_care_clock clock;
    uint64_t before;
    uint32_t ticks;

    memset(&clock, 0, sizeof clock);
    ticks = pg_time_credit(&clock, PG_CARE_TICK_SECONDS - 1);
    CHECK(ticks == 0u);
    CHECK(clock.care_residual_s == (uint32_t)PG_CARE_TICK_SECONDS - 1u);
    CHECK(clock.care_seconds_total == (uint64_t)PG_CARE_TICK_SECONDS - 1u);

    /* The residual never drifts: one more second completes the tick. */
    ticks = pg_time_credit(&clock, 1);
    CHECK(ticks == 1u);
    CHECK(clock.care_residual_s == 0u);

    /* Thirty days is 2880 ticks, which is the bound on catch-up work. */
    memset(&clock, 0, sizeof clock);
    ticks = pg_time_credit(&clock, PG_GAP_CREDIT_MAX_SECONDS);
    CHECK(ticks == 2880u);
    CHECK(clock.care_residual_s == 0u);

    /* No path decreases the total, including a nonsensical negative credit or
     * one past the cap. */
    before = clock.care_seconds_total;
    CHECK(pg_time_credit(&clock, -1) == 0u);
    CHECK(clock.care_seconds_total == before);
    CHECK(pg_time_credit(&clock, INT64_MIN) == 0u);
    CHECK(clock.care_seconds_total == before);
    CHECK(pg_time_credit(&clock, INT64_MAX) == 2880u);
    CHECK(clock.care_seconds_total == before + (uint64_t)PG_GAP_CREDIT_MAX_SECONDS);
    return true;
}

static bool test_next_tick_deadline(void)
{
    pg_time_anchor anchor = make_anchor(INT64_C(1700000000), 500, 1u, 0);
    pg_care_clock clock;

    memset(&clock, 0, sizeof clock);
    CHECK(pg_time_next_tick_wall_s(&anchor, &clock) ==
          INT64_C(1700000000) + PG_CARE_TICK_SECONDS);
    clock.care_residual_s = 300u;
    CHECK(pg_time_next_tick_wall_s(&anchor, &clock) ==
          INT64_C(1700000000) + PG_CARE_TICK_SECONDS - 300);

    anchor.last_wall_s = INT64_MAX;
    CHECK(pg_time_next_tick_wall_s(&anchor, &clock) == INT64_MAX);
    return true;
}

static bool test_null_arguments_are_survivable(void)
{
    pg_time_gap gap;
    pg_now now = make_now(1, 1, 1u, 0);

    memset(&gap, 0xffu, sizeof gap);
    pg_time_reconcile(NULL, now, &gap);
    CHECK(gap.credited_s == 0);
    pg_time_reconcile(NULL, now, NULL);
    pg_time_anchor_init(NULL);
    pg_time_anchor_set(NULL, now);
    CHECK(pg_time_credit(NULL, 100) == 0u);
    CHECK(pg_time_next_tick_wall_s(NULL, NULL) == INT64_MAX);
    return true;
}

int main(void)
{
    if (!test_saturating_arithmetic()) return 1;
    if (!test_tz_clamp()) return 1;
    if (!test_first_run()) return 1;
    if (!test_same_boot_is_authoritative()) return 1;
    if (!test_boot_change_falls_back_to_wall()) return 1;
    if (!test_cap_and_abandonment()) return 1;
    if (!test_timezone_shift()) return 1;
    if (!test_backwards_clock_does_not_refire()) return 1;
    if (!test_care_clock_is_monotone()) return 1;
    if (!test_next_tick_deadline()) return 1;
    if (!test_null_arguments_are_survivable()) return 1;
    (void)puts("time: PASS");
    return 0;
}
