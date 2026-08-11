/*
 * pg_advice — see pg_advice.h.
 *
 * Every string here is authored against the research, and the rule the whole
 * file follows is: **name the candidates, then give the check that separates
 * them.** A confident wrong diagnosis is worse than an honest ambiguous one,
 * because the player believes it and applies it to their next plant.
 */
#include "pg_advice.h"

#include "pg_content.h"
#include "pg_sim.h"
#include "pg_state.h"

#include <stddef.h>

/* ---- the guided first run ------------------------------------------------ */

static const pg_advice WALKTHROUGH[] = {
    {(uint8_t)PG_ADVICE_WELCOME, "A plant, and a month of your attention",
     "This one grows on the wall clock. Close the game for a week and a "
     "week of care, or neglect, has happened when you come back.",
     NULL, 0u},
    {(uint8_t)PG_ADVICE_CHOOSE_PLANT, "Pick something you will enjoy",
     "The four differ in how much they forgive. A snake plant will "
     "outlive most mistakes; a calathea notices everything.",
     "Read the label on any of them for the honest version.", 0u},
    {(uint8_t)PG_ADVICE_CHOOSE_POT, "The pot is half the watering",
     "Terracotta breathes and dries fast. A glazed pot holds water "
     "longer. A cachepot with no hole keeps every drop you give it, "
     "which is a kindness right up until it is not.",
     "Lift a pot to feel how much water is still in it.", 0u},
    {(uint8_t)PG_ADVICE_FIRST_WATER, "Water it properly, once",
     "Water until it runs from the hole. A splash on top wets the first "
     "centimetre and leaves the roots dry, and the plant will not tell "
     "you the difference for a fortnight.",
     NULL, 1u},
    {(uint8_t)PG_ADVICE_CHECK_SOIL, "\"Dry to 2 cm\" means press a finger in",
     "Not the surface -- the surface lies, especially in a glazed pot "
     "where the top can be dry over a wet bottom. Push in to the first "
     "knuckle. That is the single most useful thing you can do here.",
     "Check the soil, then lift the pot, and compare the two.", 0u}
};

/* ---- the diagnostic ladder ----------------------------------------------
 *
 * Brown tips are the case this whole design exists for. The research lists
 * four causes that look the same on the leaf, so the game lists four and tells
 * you how to tell them apart. Anything that named one cause would be
 * confidently wrong three times in four. */
static const pg_advice BROWN_TIP_CAUSES[] = {
    {(uint8_t)PG_ADVICE_BROWN_TIPS, "Dry air",
     "Common in winter with the heating on, and the usual first guess. "
     "It is right more often for a calathea than for anything else here.",
     "Is the air reading dry, and did the tips appear on new leaves as "
     "they opened?", 1u},
    {(uint8_t)PG_ADVICE_BROWN_TIPS, "Salt building up in the mix",
     "From feeding, or from tap water. It concentrates at the leaf edge, "
     "which is why the damage is a rim rather than a blotch.",
     "Look for a pale crust at the soil line or on the pot rim. If it is "
     "there, flush the pot rather than misting it.", 2u},
    {(uint8_t)PG_ADVICE_BROWN_TIPS, "It has been too dry, too often",
     "Not one missed watering -- a habit of them. The tip is the furthest "
     "point from the roots and dies first.",
     "Lift the pot. If it is light and the soil is dry through, this is "
     "the one.", 2u},
    {(uint8_t)PG_ADVICE_BROWN_TIPS, "Roots damaged by too much water",
     "The cruel one: damaged roots cannot drink, so the plant shows "
     "thirst while standing in wet soil. Watering more makes it worse.",
     "Check the soil at depth. Wet soil AND drooping leaves means stop "
     "watering, not start.", 3u}
};

/* The misdiagnosis loop the research calls the game's central tension. */
static const pg_advice YELLOW_CAUSES[] = {
    {(uint8_t)PG_ADVICE_YELLOW_LEAVES, "Too much water",
     "If the oldest, lowest leaves are yellowing evenly -- the whole "
     "leaf, not just the tips -- this is the likeliest cause by some "
     "margin, and the instinct to water more is what kills the plant.",
     "Is the soil still dark and wet days after you last watered? Are "
     "there gnats?", 3u},
    {(uint8_t)PG_ADVICE_YELLOW_LEAVES, "It is simply an old leaf",
     "Plants retire their lowest leaves. One yellow leaf on a plant that "
     "is otherwise growing is not a problem to solve.",
     "Is anything new growing at the top?", 0u},
    {(uint8_t)PG_ADVICE_YELLOW_LEAVES, "Not enough light",
     "A slow, general fade rather than a few distinct leaves, usually "
     "with long weak growth reaching toward the window.",
     "Have the new leaves been smaller and further apart than the old "
     "ones?", 1u}
};

static const pg_advice TIPS[] = {
    {(uint8_t)PG_ADVICE_THIRSTY, "It wants water",
     "The soil is dry through and the leaves have lost their stiffness.",
     "Water thoroughly, until it runs from the hole.", 2u},
    {(uint8_t)PG_ADVICE_OVERWATERED, "The soil is staying wet",
     "It has not dried between waterings. Roots need air as much as "
     "water, and this is how they drown.",
     "Leave it. Check again in three days rather than watering.", 3u},
    {(uint8_t)PG_ADVICE_LOW_LIGHT, "It is darker here than it looks",
     "Eyes adjust; plants do not. Growth will get long and pale before "
     "anything looks wrong.",
     "Moving it a metre closer to the window is a bigger change than it "
     "sounds.", 1u},
    {(uint8_t)PG_ADVICE_SCORCH, "That is direct sun on the leaves",
     "Bleached patches rather than browning at the edges.",
     "Bright light near a window is not the same as sun landing on it.",
     2u},
    {(uint8_t)PG_ADVICE_SALT, "There is a crust at the rim",
     "Fertiliser and tap minerals that the plant did not use.",
     "Flush the pot with several volumes of water.", 2u},
    {(uint8_t)PG_ADVICE_ROOTBOUND, "It has filled the pot",
     "Roots circling the wall, water running straight through.",
     "One size up, in spring. Two sizes is worse than none.", 1u},
    {(uint8_t)PG_ADVICE_COLD, "It is cold against that glass",
     "A leaf resting on a winter window is several degrees colder than "
     "the room.",
     NULL, 2u},
    {(uint8_t)PG_ADVICE_PESTS, "Something is living on it",
     "One treatment will not do it -- the eggs hatch after you finish.",
     "Treat again in a week, and again the week after.", 2u},
    {(uint8_t)PG_ADVICE_DORMANCY, "It has slowed for the winter",
     "Shorter days, less growth. This is not a problem and it does not "
     "need fixing.",
     "Water less often now, and do not feed until spring.", 0u},
    {(uint8_t)PG_ADVICE_HEATING_SEASON, "The heating has come on",
     "Indoor air gets dry fast this time of year. A calathea feels that "
     "before you do.",
     NULL, 1u},
    {(uint8_t)PG_ADVICE_FEED_REFUSED, "Not right now",
     "Feeding into dry soil, a dormant plant, a fresh repot or a sick "
     "one does harm rather than nothing.",
     "Water first, then feed at half strength.", 1u}
};

static const char *const TOPIC_NAMES[PG_ADVICE_TOPIC_COUNT] = {
    "none", "welcome", "choose-plant", "choose-pot", "first-water",
    "check-soil", "thirsty", "overwatered", "brown-tips", "yellow-leaves",
    "low-light", "scorch", "salt", "rootbound", "cold", "pests", "dormancy",
    "heating-season", "feed-refused"
};

const char *pg_advice_topic_name(uint8_t topic)
{
    return topic < (uint8_t)PG_ADVICE_TOPIC_COUNT ? TOPIC_NAMES[topic]
                                                  : "none";
}

const pg_advice *pg_advice_walkthrough(size_t step)
{
    return step < sizeof WALKTHROUGH / sizeof WALKTHROUGH[0]
         ? &WALKTHROUGH[step] : NULL;
}

size_t pg_advice_walkthrough_length(void)
{
    return sizeof WALKTHROUGH / sizeof WALKTHROUGH[0];
}

size_t pg_advice_causes(uint8_t topic, const pg_advice **out, size_t capacity)
{
    const pg_advice *table;
    size_t count;
    size_t index;

    if (out == NULL || capacity == 0u) return 0u;
    if (topic == (uint8_t)PG_ADVICE_BROWN_TIPS) {
        table = BROWN_TIP_CAUSES;
        count = sizeof BROWN_TIP_CAUSES / sizeof BROWN_TIP_CAUSES[0];
    } else if (topic == (uint8_t)PG_ADVICE_YELLOW_LEAVES) {
        table = YELLOW_CAUSES;
        count = sizeof YELLOW_CAUSES / sizeof YELLOW_CAUSES[0];
    } else {
        return 0u;
    }
    if (count > capacity) count = capacity;
    for (index = 0u; index < count; ++index) out[index] = &table[index];
    return count;
}

static const pg_advice *find_tip(uint8_t topic)
{
    size_t index;
    for (index = 0u; index < sizeof TIPS / sizeof TIPS[0]; ++index) {
        if (TIPS[index].topic == topic) return &TIPS[index];
    }
    return NULL;
}

const pg_advice *pg_advice_current(const pg_plant *plant,
                                   const pg_care_env *env,
                                   uint64_t now_care_s)
{
    uint16_t moisture;

    if (plant == NULL || env == NULL) return NULL;
    if (plant->life_state == (uint8_t)PG_LIFE_DEAD) return NULL;

    moisture = pg_care_moisture_whole_l(plant);

    /* Ordered by what would go wrong soonest, not by severity of symptom:
     * the plant that is drowning needs to be left alone today, and the one
     * that is thirsty needs water today. */
    if (pg_plant_ladder_rung(plant, PG_LADDER_OVERWATER) > 0u &&
        moisture > 7000u) {
        return find_tip((uint8_t)PG_ADVICE_OVERWATERED);
    }
    if (pg_plant_ladder_rung(plant, PG_LADDER_UNDERWATER) > 0u) {
        return find_tip((uint8_t)PG_ADVICE_THIRSTY);
    }
    if (pg_plant_ladder_rung(plant, PG_LADDER_PESTS) > 0u) {
        return find_tip((uint8_t)PG_ADVICE_PESTS);
    }
    if (pg_plant_ladder_rung(plant, PG_LADDER_SALT) > 0u) {
        return find_tip((uint8_t)PG_ADVICE_SALT);
    }
    if (pg_plant_ladder_rung(plant, PG_LADDER_EXCESS_LIGHT) > 0u) {
        return find_tip((uint8_t)PG_ADVICE_SCORCH);
    }
    if (pg_plant_ladder_rung(plant, PG_LADDER_LOW_LIGHT) > 0u) {
        return find_tip((uint8_t)PG_ADVICE_LOW_LIGHT);
    }
    if (pg_plant_ladder_rung(plant, PG_LADDER_COLD) > 0u) {
        return find_tip((uint8_t)PG_ADVICE_COLD);
    }
    if (pg_plant_ladder_rung(plant, PG_LADDER_ROOTBOUND) > 0u) {
        return find_tip((uint8_t)PG_ADVICE_ROOTBOUND);
    }
    /* Seasonal notes, which are context rather than problems. */
    if (!env->is_growing_season &&
        plant->life_state == (uint8_t)PG_LIFE_DORMANT) {
        return find_tip((uint8_t)PG_ADVICE_DORMANCY);
    }
    if (!env->is_growing_season && env->rh_pct < 40u) {
        return find_tip((uint8_t)PG_ADVICE_HEATING_SEASON);
    }
    if (pg_care_feed_refusal(plant, env, now_care_s) !=
        PG_FEED_REFUSAL_NONE) {
        return find_tip((uint8_t)PG_ADVICE_FEED_REFUSED);
    }
    /* A plant that is fine gets silence. Nagging is how a helper becomes
     * wallpaper. */
    return NULL;
}

/* ---- --advice-test -------------------------------------------------------
 *
 * The assertions that matter are about honesty, not coverage: an ambiguous
 * symptom must offer more than one cause, and every cause of an ambiguous
 * symptom must carry the check that separates it from its neighbours. A single
 * confident answer would pass a coverage test and fail the player.
 */
#include <stdio.h>
#include <string.h>

static int advice_failures;

static void advice_check(bool condition, const char *what)
{
    if (!condition) {
        (void)fprintf(stderr, "advice-test: %s\n", what);
        advice_failures += 1;
    }
}

int pg_advice_run_test(void)
{
    const pg_advice *causes[8];
    size_t count;
    size_t index;
    size_t topic;

    advice_failures = 0;
    (void)printf("advice %s\n", pg_version());

    /* The walkthrough runs from welcome to the soil check, in order, and every
     * step says something. */
    advice_check(pg_advice_walkthrough_length() >= 5u,
                 "the guided first run is shorter than the five steps the "
                 "plan names");
    for (index = 0u; index < pg_advice_walkthrough_length(); ++index) {
        const pg_advice *step = pg_advice_walkthrough(index);
        advice_check(step != NULL, "a walkthrough step is missing");
        if (step == NULL) continue;
        advice_check(step->title != NULL && step->title[0] != '\0',
                     "a walkthrough step has no title");
        advice_check(step->body != NULL && strlen(step->body) > 40u,
                     "a walkthrough step says almost nothing");
    }
    advice_check(pg_advice_walkthrough(pg_advice_walkthrough_length())
                 == NULL, "the walkthrough runs past its last step");

    /* The two ambiguous symptoms. This is the assertion the file exists for. */
    {
        static const uint8_t AMBIGUOUS[] = {
            (uint8_t)PG_ADVICE_BROWN_TIPS, (uint8_t)PG_ADVICE_YELLOW_LEAVES
        };
        for (topic = 0u; topic < sizeof AMBIGUOUS / sizeof AMBIGUOUS[0];
             ++topic) {
            count = pg_advice_causes(AMBIGUOUS[topic], causes, 8u);
            advice_check(count >= 2u,
                         "an ambiguous symptom offers a single confident "
                         "cause -- that is the myth-making shape this file "
                         "exists to avoid");
            for (index = 0u; index < count; ++index) {
                advice_check(causes[index]->check != NULL &&
                             causes[index]->check[0] != '\0',
                             "a candidate cause offers no way to tell it "
                             "apart from the others");
                advice_check(causes[index]->title != NULL,
                             "a candidate cause has no title");
            }
            (void)printf("  %-14s causes=%zu\n",
                         pg_advice_topic_name(AMBIGUOUS[topic]), count);
        }
        /* Brown tips specifically: the research names four. */
        count = pg_advice_causes((uint8_t)PG_ADVICE_BROWN_TIPS, causes, 8u);
        advice_check(count >= 4u,
                     "brown tips have at least four causes in the research "
                     "and the game must not pretend otherwise");
    }

    /* An unambiguous topic has no cause list, so a caller cannot render an
     * empty "possible causes" panel. */
    advice_check(pg_advice_causes((uint8_t)PG_ADVICE_THIRSTY, causes, 8u)
                 == 0u, "an unambiguous topic returned a cause list");
    advice_check(pg_advice_causes((uint8_t)PG_ADVICE_BROWN_TIPS, NULL, 8u)
                 == 0u, "a NULL destination was written to");

    /* A healthy plant is told nothing. Silence is a feature: a helper that
     * always has something to say becomes wallpaper. */
    {
        static pg_state state;
        pg_care_env env;
        const pg_advice *advice;

        pg_init(&state, 99u);
        /* A fortnight of decent light. Without this the DLI ring is all
         * zeroes, the low-light ladder fires, and the plant is correctly told
         * it is in the dark -- which is the code being right and the fixture
         * being wrong. */
        {
            const pg_species *species =
                pg_content_species(state.plants[0].species_id);
            size_t ring;
            for (ring = 0u; ring < (size_t)PG_DLI_RING; ++ring) {
                state.plants[0].dli_ring[ring] =
                    species ? (uint16_t)(species->dli_thriving_c + 50u) : 900u;
            }
        }
        state.plants[0].turgor = 9000u;
        state.plants[0].root_health = 9000u;
        state.plants[0].root_capacity = 10000u;
        state.plants[0].moisture_top = 5000u;
        state.plants[0].moisture_bottom = 5000u;
        state.plants[0].nutrition = 8000u;
        /* Midsummer, so the seasonal notes are not in play. pg_init leaves
         * the anchor at the epoch, which is January -- and a healthy plant in
         * a dry January genuinely SHOULD be told the heating has come on, so
         * asserting silence there would have been asserting a bug. */
        state.anchor.last_wall_s = INT64_C(1813244400);   /* 2027-06-15 */
        state.anchor.established = true;
        env = pg_sim_env_now(&state, 0u);
        advice = pg_advice_current(&state.plants[0], &env, 0u);
        if (advice != NULL) {
            (void)fprintf(stderr, "  (it said: %s / %s)\n",
                          pg_advice_topic_name(advice->topic),
                          advice->title ? advice->title : "?");
        }
        advice_check(advice == NULL,
                     "a healthy plant in summer was given advice it did not "
                     "need");

        /* And the other half: the same healthy plant in a dry January IS told
         * about the heating, because that is context worth having rather than
         * a fault to fix. */
        state.anchor.last_wall_s = INT64_C(1799000000);   /* 2027-01-01ish */
        env = pg_sim_env_now(&state, 0u);
        advice = pg_advice_current(&state.plants[0], &env, 0u);
        advice_check(advice != NULL &&
                     advice->topic == (uint8_t)PG_ADVICE_HEATING_SEASON,
                     "a healthy plant in a dry winter was told nothing about "
                     "the heating");
        state.anchor.last_wall_s = INT64_C(1813244400);

        /* A dead plant is not lectured either. */
        state.plants[0].life_state = (uint8_t)PG_LIFE_DEAD;
        advice = pg_advice_current(&state.plants[0], &env, 0u);
        advice_check(advice == NULL, "a dead plant was given care advice");

        advice_check(pg_advice_current(NULL, &env, 0u) == NULL,
                     "a NULL plant produced advice");
        advice_check(pg_advice_current(&state.plants[0], NULL, 0u) == NULL,
                     "a NULL environment produced advice");
    }

    /* Every topic names itself, so a UI cannot show a blank heading. */
    for (topic = 0u; topic < (size_t)PG_ADVICE_TOPIC_COUNT; ++topic) {
        const char *name = pg_advice_topic_name((uint8_t)topic);
        advice_check(name != NULL && name[0] != '\0',
                     "a topic has no name");
    }

    if (advice_failures != 0) {
        (void)fprintf(stderr, "advice-test: FAILED after %d failures\n",
                      advice_failures);
        return 1;
    }
    (void)puts("advice: PASS");
    return 0;
}
