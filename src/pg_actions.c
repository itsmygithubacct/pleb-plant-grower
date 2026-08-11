/*
 * pg_actions — the verbs, their effects, the streak, and the audio events.
 *
 * Everything here is measured in CARE time, never wall time: a bottom-soak's
 * deadline, a feed's four-to-six week window and a watering's due date all sit
 * on the same monotone clock, which is why a soak survives a crash, a closed
 * lid and a catch-up identically (D-089, ARCHITECTURE.md §6.2).
 *
 * Two things this file deliberately does not do:
 *
 *   - it does not refuse anything except the four Feed cases. A sip really
 *     does wet only the top; a two-sizes-up repot really does leave wet mix
 *     with no roots in it; misting really does nothing much and adds a little
 *     fungal risk at night. All of them happen, and the notebook explains them
 *     afterwards. "Repotting into a much bigger pot" is itself on the myth
 *     blocklist, so refusing it would teach the player the opposite of the
 *     truth (D-019).
 *   - it does not scold. A watering that arrives late increments a counter
 *     that is never shown as a failure; the streak accrues and is phrased as a
 *     best run (GAME_DESIGN.md §10.4).
 */
#include "pg_actions.h"

#include "pg_calendar.h"
#include "pg_content.h"
#include "pg_plant.h"
#include "pg_rng.h"
#include "pg_sim.h"
#include "pg_state.h"

#include <string.h>

/* Feed strengths, on the Q12 multiplier scale of the label's full dose. */
#define PG_FEED_QUARTER_Q12 1024u
#define PG_FEED_HALF_Q12 2048u
#define PG_FEED_FULL_Q12 4096u

/* A bottom-soak is a real timed action: twenty to thirty minutes of care time,
 * and the pot stays in the water until somebody takes it out. */
#define PG_SOAK_CARE_SECONDS 1500u
#define PG_FLUSH_CARE_SECONDS 900u

/* Tap water carries dissolved solids; the species' own sensitivity decides
 * what that costs, and only a flush ever takes it back out. */
#define PG_TAP_PURITY_LOAD 400u
#define PG_FILTERED_PURITY_LOAD 120u

/* ---- audio -------------------------------------------------------------- */

void pg_audio_push(pg_state *state, uint8_t kind, float gain, float pitch)
{
    size_t slot;

    if (state == NULL || kind >= (uint8_t)PG_AUDIO_KIND_COUNT) {
        return;
    }
    if (state->audio_count >= (uint8_t)PG_AUDIO_QUEUE_MAX) {
        /* A queue nobody drained is a frontend that is not listening; drop the
         * oldest rather than the newest, so what is heard is what just
         * happened. */
        state->audio_head = (uint8_t)((state->audio_head + 1u)
                                      % (uint8_t)PG_AUDIO_QUEUE_MAX);
        state->audio_count = (uint8_t)(state->audio_count - 1u);
    }
    slot = ((size_t)state->audio_head + (size_t)state->audio_count)
         % (size_t)PG_AUDIO_QUEUE_MAX;
    state->audio[slot].kind = kind;
    state->audio[slot].gain = gain;
    state->audio[slot].pitch = pitch;
    state->audio_count = (uint8_t)(state->audio_count + 1u);
}

/* ---- helpers ------------------------------------------------------------ */

uint64_t pg_actions_care_now(const pg_state *state)
{
    return (state == NULL) ? 0u : state->clock.care_seconds_total;
}

static pg_plant *pg_actions_plant(pg_state *state, uint8_t plant_index)
{
    if (state == NULL || plant_index >= state->plant_count
        || plant_index >= (uint8_t)PG_PLANT_MAX) {
        return NULL;
    }
    return &state->plants[plant_index];
}

pg_legality pg_actions_legality(const pg_state *state, uint8_t plant_index,
                                pg_verb verb, const char **reason)
{
    pg_care_env env;

    if (state == NULL || plant_index >= state->plant_count) {
        return PG_LEGAL_ALLOWED;
    }
    env = pg_sim_env_now(state, plant_index);
    return pg_care_verb_legality(verb, &state->plants[plant_index], &env,
                                 pg_actions_care_now(state), reason);
}

pg_verb pg_actions_verb_for_input(const pg_input *in)
{
    if (in == NULL) {
        return PG_VERB_COUNT;
    }
    if (in->check_soil) return PG_VERB_CHECK_SOIL;
    if (in->lift_pot)   return PG_VERB_LIFT_POT;
    if (in->water)      return PG_VERB_WATER_THOROUGHLY;
    if (in->feed)       return PG_VERB_FEED_HALF;
    if (in->mist)       return PG_VERB_MIST;
    if (in->rotate)     return PG_VERB_ROTATE;
    if (in->wipe)       return PG_VERB_WIPE;
    if (in->trim)       return PG_VERB_TRIM;
    if (in->repot)      return PG_VERB_REPOT;
    if (in->drain)      return PG_VERB_EMPTY_SAUCER;
    if (in->sleeve)     return PG_VERB_SLEEVE_LIFT;
    return PG_VERB_COUNT;
}

/* The reward loop, and the only place it is written. A watering that lands
 * inside the window the conditions actually asked for is a streak; one that
 * arrives after the plant has already lost turgor is a missed event. Neither
 * is ever shown as a score (D-021, GAME_DESIGN.md §10.4). */
static void pg_actions_streak(pg_plant *plant, const pg_care_env *env)
{
    const pg_species *species = pg_content_species(plant->species_id);
    uint16_t whole;

    if (species == NULL) {
        return;
    }
    whole = pg_care_moisture_whole_l(plant);
    if (plant->turgor < 5000u) {
        if (plant->missed_events < UINT16_MAX) {
            plant->missed_events = (uint16_t)(plant->missed_events + 1u);
        }
        return;
    }
    if (whole <= species->thirst_threshold_l && plant->streak_on_time < UINT16_MAX) {
        plant->streak_on_time = (uint16_t)(plant->streak_on_time + 1u);
    }
    (void)env;
}

static void pg_actions_add_purity(pg_plant *plant, uint16_t load)
{
    uint32_t total = (uint32_t)plant->purity_load + (uint32_t)load;

    plant->purity_load = (uint16_t)((total > (uint32_t)PG_LEVEL_MAX)
                                    ? (uint32_t)PG_LEVEL_MAX : total);
}

static uint16_t pg_actions_source_load(uint8_t source)
{
    switch ((pg_water_source)source) {
    case PG_WATER_TAP:
        return PG_TAP_PURITY_LOAD;
    case PG_WATER_FILTERED:
        return PG_FILTERED_PURITY_LOAD;
    case PG_WATER_DISTILLED:
    case PG_WATER_RAIN:
    case PG_WATER_SOURCE_COUNT:
    default:
        return 0u;
    }
}

/* ---- the verbs ---------------------------------------------------------- */

bool pg_actions_apply(pg_state *state, uint8_t plant_index, pg_verb verb,
                      uint8_t param, pg_action_result *out)
{
    pg_plant *plant = pg_actions_plant(state, plant_index);
    const pg_pot *pot;
    pg_care_env env;
    pg_legality legality;
    const char *reason = NULL;
    uint64_t care_now;
    uint8_t audio = (uint8_t)PG_AUDIO_UI_ACCEPT;

    if (out != NULL) {
        memset(out, 0, sizeof *out);
        out->legality = PG_LEGAL_ALLOWED;
        out->audio_kind = (uint8_t)PG_AUDIO_UI_REJECT;
    }
    if (plant == NULL || (size_t)verb >= (size_t)PG_VERB_COUNT) {
        return false;
    }

    env = pg_sim_env_now(state, plant_index);
    care_now = pg_actions_care_now(state);
    legality = pg_care_verb_legality(verb, plant, &env, care_now, &reason);
    if (out != NULL) {
        out->legality = legality;
        out->reason = reason;
    }
    if (legality == PG_LEGAL_REFUSED) {
        /* The four Feed cases, and nothing else in the game. The reason is
         * spoken and always says what to do instead. */
        pg_audio_push(state, (uint8_t)PG_AUDIO_UI_REJECT, 1.0f, 1.0f);
        return false;
    }

    pot = pg_content_pot(plant->pot_id);

    switch (verb) {
    /* ---- observation: free, unlimited, never harmful ---- */
    case PG_VERB_CHECK_SOIL:
    case PG_VERB_LIFT_POT:
    case PG_VERB_LOOK_CLOSELY:
    case PG_VERB_READ_LABEL:
        audio = (verb == PG_VERB_CHECK_SOIL) ? (uint8_t)PG_AUDIO_SOIL_PRESS
                                             : (uint8_t)PG_AUDIO_UI_MOVE;
        break;

    /* ---- water ---- */
    case PG_VERB_WATER_THOROUGHLY:
        pg_actions_streak(plant, &env);
        pg_care_water_thoroughly(plant, care_now);
        pg_actions_add_purity(plant, PG_TAP_PURITY_LOAD);
        audio = (uint8_t)PG_AUDIO_WATER;
        break;
    case PG_VERB_WATER_SOURCE:
        pg_actions_streak(plant, &env);
        pg_care_water_thoroughly(plant, care_now);
        pg_actions_add_purity(plant, pg_actions_source_load(param));
        audio = (uint8_t)PG_AUDIO_WATER;
        break;
    case PG_VERB_WATER_SIP:
        /* It visibly succeeds and the plant does not improve. */
        pg_care_water_sip(plant, care_now);
        pg_actions_add_purity(plant, PG_TAP_PURITY_LOAD / 3u);
        audio = (uint8_t)PG_AUDIO_WATER;
        break;
    case PG_VERB_BOTTOM_SOAK:
        /* A real timed action. The deadline tells you it is done; it does not
         * lift the pot out of the water for you, and leaving it in is exactly
         * how a soak becomes a bog. */
        plant->pending_action = (uint8_t)PG_PENDING_BOTTOM_SOAK;
        plant->pending_action_care_s = care_now + PG_SOAK_CARE_SECONDS;
        audio = (uint8_t)PG_AUDIO_WATER;
        break;
    case PG_VERB_EMPTY_SAUCER:
        /* Also how you end a soak: you take the pot out of the water. */
        plant->pending_action = (uint8_t)PG_PENDING_NONE;
        plant->pending_action_care_s = 0u;
        plant->last_drained_care_s = care_now;
        if (pot != NULL) {
            if (pot->has_drainage) {
                plant->moisture_bottom = (plant->moisture_bottom > 500u)
                                       ? (uint16_t)(plant->moisture_bottom - 500u)
                                       : 0u;
            } else {
                /* Tipping a cachepot out works, briefly. The perched water
                 * table is a property of the pot, so it fills back in — which
                 * is the lesson, and why the sleeve is the real answer. */
                plant->moisture_bottom = pot->bottom_floor_l;
            }
        }
        audio = (uint8_t)PG_AUDIO_DRAIN;
        break;
    case PG_VERB_SLEEVE_LIFT:
    case PG_VERB_SLEEVE_RETURN:
        if (pot != NULL && pot->is_sleeve_capable) {
            /* Out of the sleeve and into the liner it was always sitting in. */
            plant->pot_id = (uint8_t)PG_POT_NURSERY;
        } else if (pot != NULL && pot->is_sleeve_liner) {
            plant->pot_id = (uint8_t)PG_POT_CACHEPOT;
        }
        audio = (uint8_t)PG_AUDIO_POT_SET;
        break;

    /* ---- feed ---- */
    case PG_VERB_FEED_QUARTER:
        pg_care_feed(plant, PG_FEED_QUARTER_Q12, care_now);
        audio = (uint8_t)PG_AUDIO_FEED;
        break;
    case PG_VERB_FEED_HALF:
        pg_care_feed(plant, PG_FEED_HALF_Q12, care_now);
        audio = (uint8_t)PG_AUDIO_FEED;
        break;
    case PG_VERB_FEED_FULL:
        /* Available, warned, and wrong. The surplus stays behind as salt. */
        pg_care_feed(plant, PG_FEED_FULL_Q12, care_now);
        audio = (uint8_t)PG_AUDIO_FEED;
        break;
    case PG_VERB_FLUSH:
        pg_care_flush(plant, care_now);
        plant->pending_action = (uint8_t)PG_PENDING_FLUSH;
        plant->pending_action_care_s = care_now + PG_FLUSH_CARE_SECONDS;
        audio = (uint8_t)PG_AUDIO_WATER;
        break;

    /* ---- maintenance ---- */
    case PG_VERB_ROTATE:
        plant->last_rotated_care_s = care_now;
        audio = (uint8_t)PG_AUDIO_ROTATE;
        break;
    case PG_VERB_WIPE:
        pg_care_wipe_leaves(plant);
        audio = (uint8_t)PG_AUDIO_UI_ACCEPT;
        break;
    case PG_VERB_TRIM: {
        /* The scar stays in the record and in the sprite; the plant just looks
         * tidier. You cannot undo, you can only tend. */
        uint8_t index;
        for (index = 0u; index < plant->leaf_count; ++index) {
            if (plant->leaves[index].damage_mask != 0u) {
                plant->leaves[index].damage_mask |= (uint8_t)PG_DAMAGE_TRIMMED;
            }
        }
        for (index = 0u; index < plant->vine_count; ++index) {
            if (plant->vine[index].damage_mask != 0u) {
                plant->vine[index].damage_mask |= (uint8_t)PG_DAMAGE_TRIMMED;
            }
        }
        audio = (uint8_t)PG_AUDIO_SNIP;
        break;
    }
    case PG_VERB_MOVE: {
        const pg_spot *spot = pg_content_spot(param);
        if (spot != NULL) {
            plant->spot_id = param;
        }
        audio = (uint8_t)PG_AUDIO_POT_SET;
        break;
    }
    case PG_VERB_REPOT:
        pg_care_repot(plant, param, false, false, care_now);
        audio = (uint8_t)PG_AUDIO_POT_SET;
        break;
    case PG_VERB_REPOT_TWO_STEP:
        pg_care_repot(plant, param, true, false, care_now);
        audio = (uint8_t)PG_AUDIO_POT_SET;
        break;
    case PG_VERB_REPOT_GRAVEL:
        pg_care_repot(plant, param, false, true, care_now);
        audio = (uint8_t)PG_AUDIO_POT_SET;
        break;
    case PG_VERB_CUTTING:
        /* The mercy mechanic: a real gamble at the tabled odds, not a free
         * undo, and it is offered before death rather than after it (D-031).
         * This is one of the two places the persisted RNG stream is consumed,
         * because whether the cutting took is part of the history. */
        if (pg_plant_mercy_offered(plant)) {
            uint16_t odds = pg_plant_mercy_odds_l(plant);
            char kept_name[PG_NAME_BYTES];
            uint8_t kept_pot = plant->pot_id;
            uint8_t kept_spot = plant->spot_id;
            int64_t kept_planted = plant->planted_wall_s;

            memcpy(kept_name, plant->name, sizeof kept_name);
            if (pg_rng_chance_l(&state->rng, odds)) {
                pg_plant_init(plant, plant->species_id, kept_pot, kept_spot,
                              kept_planted);
                memcpy(plant->name, kept_name, sizeof plant->name);
                plant->name[PG_NAME_BYTES - 1] = '\0';
            } else {
                plant->life_state = (uint8_t)PG_LIFE_DEAD;
                plant->turgor = 0u;
                state->replant_offered = true;
            }
        }
        audio = (uint8_t)PG_AUDIO_SNIP;
        break;
    case PG_VERB_TREAT_PESTS:
        /* One treatment never finishes it: the eggs are still to hatch, and
         * the timer that says so is the second consumer of the stream. */
        plant->pest_mites = (uint16_t)((uint32_t)plant->pest_mites * 4u / 10u);
        plant->pest_mealy = (uint16_t)((uint32_t)plant->pest_mealy * 4u / 10u);
        plant->pest_gnats = (uint16_t)((uint32_t)plant->pest_gnats * 6u / 10u);
        plant->pest_egg_timer = (uint16_t)(384u + pg_rng_below(&state->rng, 288u));
        audio = (uint8_t)PG_AUDIO_UI_ACCEPT;
        break;
    case PG_VERB_MIST:
        /* Exactly what it really does: the air is damp for twenty minutes, and
         * at night it adds a small fungal-spot risk. Nothing in the game ever
         * recommends it. */
        if (!env.is_daylight) {
            uint32_t risk = (uint32_t)plant->pest_mealy + 150u;
            plant->pest_mealy = (uint16_t)((risk > (uint32_t)PG_LEVEL_MAX)
                                           ? (uint32_t)PG_LEVEL_MAX : risk);
        }
        audio = (uint8_t)PG_AUDIO_MIST;
        break;

    case PG_VERB_COUNT:
    default:
        return false;
    }

    pg_audio_push(state, audio, 1.0f, 1.0f);
    if (out != NULL) {
        out->happened = true;
        out->audio_kind = audio;
    }
    return true;
}
