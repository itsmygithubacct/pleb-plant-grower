/*
 * pg_advice — the instruction surface.
 *
 * "Offers instructions to the player", built on kilix-story: flags, counters,
 * conditions and validated dialogue graphs, with zero allocation, zero I/O and
 * no game rules of its own. The rules stay in pg_care; this decides what is
 * worth *saying*.
 *
 * Three layers, in increasing order of how much the player already knows:
 *
 *   1. **The guided first run.** A validated dialogue graph: pick a plant,
 *      pick a pot, here is why this pot suits this plant, water it once, and
 *      here is what "dry to 2 cm" actually means.
 *   2. **Situational tips.** Conditions over flags and counters mapped from
 *      the care axes and the calendar. "It is November, the heating is on and
 *      the air just got dry -- your calathea feels that before you do." Every
 *      tip cites the mechanic it comes from.
 *   3. **The diagnostic ladder.** When a symptom appears, name the *likely*
 *      cause with the research's honest ambiguity and offer the discriminating
 *      check.
 *
 * The third layer is the one worth getting right, and it is why this file
 * exists rather than a table of strings. Brown leaf tips have at least four
 * causes -- dry air, salt build-up, underwatering and root damage from
 * overwatering -- and they look the same. Software that says "your air is too
 * dry" is confidently wrong three times in four, and it teaches the player a
 * myth that will cost them their next plant. So the advice names the
 * candidates and tells you how to tell them apart: lift the pot, check the
 * soil at depth, look for crust at the rim. That is the honest shape of the
 * knowledge, and it is also better play.
 *
 * The myth blocklist is enforced over content/strings/en-US.json by
 * tests/test_content.py, not by review, because a myth is exactly the sort of
 * plausible sentence that survives review.
 */
#ifndef PG_ADVICE_H
#define PG_ADVICE_H

#include "pleb_plant_grower.h"

#include "pg_care.h"
#include "pg_plant.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What a piece of advice is about, so the UI can place it and the tests can
 * assert coverage. Append-only: these are not persisted, but the tips that
 * cite them are authored against the numbers. */
typedef enum pg_advice_topic {
    PG_ADVICE_NONE = 0,
    PG_ADVICE_WELCOME,
    PG_ADVICE_CHOOSE_PLANT,
    PG_ADVICE_CHOOSE_POT,
    PG_ADVICE_FIRST_WATER,
    PG_ADVICE_CHECK_SOIL,
    PG_ADVICE_THIRSTY,
    PG_ADVICE_OVERWATERED,
    PG_ADVICE_BROWN_TIPS,
    PG_ADVICE_YELLOW_LEAVES,
    PG_ADVICE_LOW_LIGHT,
    PG_ADVICE_SCORCH,
    PG_ADVICE_SALT,
    PG_ADVICE_ROOTBOUND,
    PG_ADVICE_COLD,
    PG_ADVICE_PESTS,
    PG_ADVICE_DORMANCY,
    PG_ADVICE_HEATING_SEASON,
    PG_ADVICE_FEED_REFUSED,
    PG_ADVICE_TOPIC_COUNT
} pg_advice_topic;

/* One piece of advice. `body` states what is likely; `check` is the
 * discriminating observation that separates it from its neighbours, and is
 * NULL only where there genuinely is no ambiguity. */
typedef struct pg_advice {
    uint8_t topic;               /* pg_advice_topic */
    const char *title;
    const char *body;
    const char *check;           /* "lift the pot", "look at the rim" */
    uint8_t urgency;             /* 0 = a note, 3 = act today */
} pg_advice;

/* The most useful thing to say about this plant right now, or NULL when there
 * is nothing worth saying. Never nags: a plant that is fine gets silence. */
const pg_advice *pg_advice_current(const pg_plant *plant,
                                   const pg_care_env *env,
                                   uint64_t now_care_s);

/* Every candidate cause of a symptom, in descending likelihood, written so the
 * player can tell them apart. Returns how many were written. */
size_t pg_advice_causes(uint8_t topic, const pg_advice **out,
                        size_t capacity);

/* The guided first run, step by step. Returns NULL past the last step. */
const pg_advice *pg_advice_walkthrough(size_t step);
size_t pg_advice_walkthrough_length(void);

const char *pg_advice_topic_name(uint8_t topic);

/* Headless diagnostic behind --advice-test: every topic reachable, every
 * ambiguous symptom offering more than one cause and a discriminating check,
 * and no authored string matching the myth blocklist. Returns 0 when every
 * assertion held. */
int pg_advice_run_test(void);

#endif /* PG_ADVICE_H */
