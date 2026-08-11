/*
 * pg_actions — the verbs.
 *
 * GAME_DESIGN.md §6 in one file: legality (including the four and only four
 * hard refusals, which live in pg_care.c and are consulted from here), the
 * effects, the streak accounting and the audio events. Nothing here reads a
 * clock or touches a mixer.
 *
 * The rule the whole surface is built on: an action that is wrong right now
 * stays **enabled with a stated reason** and then behaves truthfully. A
 * refusal is the one response that teaches nothing, so there are exactly four
 * of them and they are all under Feed. Everything else — a sip that wets only
 * the top, misting that does what misting really does, a two-sizes-up repot —
 * happens, and the notebook explains it afterwards (D-019).
 */
#ifndef PG_ACTIONS_H
#define PG_ACTIONS_H

#include "pleb_plant_grower.h"

#include "pg_care.h"
#include "pg_state.h"

#include <stdbool.h>
#include <stdint.h>

/* What the water came out of. Only the purity load differs, and only tap water
 * carries one worth modelling. */
typedef enum pg_water_source {
    PG_WATER_TAP = 0,
    PG_WATER_FILTERED = 1,
    PG_WATER_DISTILLED = 2,
    PG_WATER_RAIN = 3,
    PG_WATER_SOURCE_COUNT = 4
} pg_water_source;

typedef struct pg_action_result {
    pg_legality legality;
    const char *reason;      /* NULL when there is nothing to say */
    bool happened;           /* false only for the four Feed refusals */
    uint8_t audio_kind;      /* pg_audio_event_kind */
} pg_action_result;

/* Apply a verb. `param` is read against the verb: a pot id for the repots, a
 * spot id for a move, a pg_water_source for the water-source verb, and is
 * ignored otherwise. `out` may be NULL. Returns whether the action happened. */
bool pg_actions_apply(pg_state *state, uint8_t plant_index, pg_verb verb,
                      uint8_t param, pg_action_result *out);

/* Legality of a verb right now, against the environment at the anchor. The
 * action list draws its reasons from this and never greys anything out. */
pg_legality pg_actions_legality(const pg_state *state, uint8_t plant_index,
                                pg_verb verb, const char **reason);

/* The eleven input booleans are keyboard shortcuts for eleven verbs, and every
 * other verb is reached through the action list plus confirm (D-087). Returns
 * PG_VERB_COUNT when no shortcut is pressed. */
pg_verb pg_actions_verb_for_input(const pg_input *in);

/* Care time right now, which is what every due date and deadline is measured
 * in (ARCHITECTURE.md §6.2). */
uint64_t pg_actions_care_now(const pg_state *state);

#endif /* PG_ACTIONS_H */
