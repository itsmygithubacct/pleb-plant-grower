/*
 * pg_time — gap reconciliation policy, and the only place tamper policy lives.
 *
 * ARCHITECTURE.md §5.3 and §5.7, IMPLEMENTATION_PLAN.md §6.2. Saturating
 * subtraction, the boot-id cross-check, the 30-day cap, anomaly
 * classification. Nothing here reads a clock: every input arrives in a pg_now
 * the frontend filled.
 */
#ifndef PG_TIME_H
#define PG_TIME_H

#include "pleb_plant_grower.h"

#include <stdbool.h>
#include <stdint.h>

/* Wall/boot divergence below this is ordinary NTP correction and is not
 * reported (ARCHITECTURE.md §5.7 rule 4). */
#define PG_TIME_NTP_SLACK_SECONDS INT64_C(300)

/* A local-offset change of this size or more is a flight or an equinox: it
 * suppresses the "the clock jumped" message and is never an anomaly (D-079). */
#define PG_TIME_TZ_SHIFT_MINUTES 60

/* The persisted anchors, exactly the HDR fields the reconciliation reads
 * (ARCHITECTURE.md §6.2). Kept as its own struct so pg_time can be tested
 * without a pg_state and so the save code has one thing to copy. */
typedef struct pg_time_anchor {
    int64_t last_wall_s;
    int64_t last_boot_s;
    uint8_t boot_id[PG_BOOT_ID_BYTES];
    int32_t tz_offset_minutes;
    bool established;   /* false on a first run: there is no gap to credit */
} pg_time_anchor;

/* Everything the classification concluded. `credited_s` is the only number the
 * simulation may spend; the rest is diagnosis and in-character reporting. */
typedef struct pg_time_gap {
    int64_t wall_gap_s;        /* saturating, may be negative */
    int64_t boot_gap_s;        /* saturating, may be negative across a reboot */
    int64_t trusted_s;         /* uncapped, never negative */
    int64_t credited_s;        /* clamped to [0, PG_GAP_CREDIT_MAX_SECONDS] */
    int32_t tz_delta_minutes;  /* new offset minus the latched one */
    bool first_run;
    bool same_boot;
    bool abandoned;            /* trusted_s exceeded the cap: resolve, do not simulate */
    bool clock_backwards;
    bool clock_jumped;
    bool tz_shifted;
    bool counts_as_anomaly;    /* whether clock_anomaly_count should increment */
} pg_time_gap;

/* The biology clock. care_seconds_total is monotone; no path decreases it
 * (ARCHITECTURE.md §5.7 rule 1). */
typedef struct pg_care_clock {
    uint64_t care_seconds_total;
    uint32_t care_residual_s;
} pg_care_clock;

/* Saturating integer helpers. Mandatory rather than convenient: a legal
 * INT64_MIN -> INT64_MAX wall transition overflows plain signed subtraction,
 * which is exactly the defect UBSan found in the kit's own loop. */
int64_t pg_time_saturating_sub(int64_t minuend, int64_t subtrahend);
int64_t pg_time_saturating_add(int64_t left, int64_t right);
int64_t pg_time_clamp_i64(int64_t value, int64_t low, int64_t high);

/* Clamp to [PG_TZ_OFFSET_MIN, PG_TZ_OFFSET_MAX]; anything outside is 0
 * (D-079). */
int32_t pg_time_clamp_tz_minutes(int32_t minutes);

/* An un-established anchor: the first-run state. */
void pg_time_anchor_init(pg_time_anchor *anchor);

/* Re-anchor to `now`. Called once per advance, after crediting, and
 * unconditionally — that is what stops a backwards clock re-firing on every
 * launch (ARCHITECTURE.md §5.7 rule 5). */
void pg_time_anchor_set(pg_time_anchor *anchor, pg_now now);

/* Classify the gap between the anchor and `now`. Pure: touches nothing but
 * `gap`, which is fully overwritten. */
void pg_time_reconcile(const pg_time_anchor *anchor, pg_now now,
                       pg_time_gap *gap);

/* Spend credited seconds on the care clock and return the whole care ticks
 * that became available. `credited_s` outside [0, PG_GAP_CREDIT_MAX_SECONDS]
 * is clamped, so this can never rewind care_seconds_total. */
uint32_t pg_time_credit(pg_care_clock *clock, int64_t credited_s);

/* The wall second at which the next care tick completes, given the anchor and
 * the residual. Saturates rather than wrapping. */
int64_t pg_time_next_tick_wall_s(const pg_time_anchor *anchor,
                                 const pg_care_clock *clock);

/* Headless diagnostic behind --time-test: prints a deterministic gap matrix
 * and returns 0 when every case classified as documented. */
int pg_time_run_test(void);

#endif /* PG_TIME_H */
