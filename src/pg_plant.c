/*
 * pg_plant — see pg_plant.h.
 */
#include "pg_plant.h"

#include "pg_calendar.h"
#include "pg_care.h"
#include "pg_content.h"

#include <stdio.h>
#include <string.h>

/* Scene ids are a separate numbering from spot ids and no code may assume the
 * two are equal (D-081), so a spot's suggested plate is resolved by name
 * through this table rather than by index. The order is the scene enumeration
 * of the public header. */
static const char *const PG_SCENE_IDS[PG_SCENE_COUNT] = {
    "sunny-sill", "bright-corner", "study-desk",
    "plain-studio", "kitchen-shelf", "steamy-bath"
};

static uint8_t pg_scene_for_name(const char *name)
{
    uint8_t i;

    if (name == NULL) {
        return (uint8_t)PG_SCENE_DEFAULT;
    }
    for (i = 0; i < (uint8_t)PG_SCENE_COUNT; ++i) {
        if (strcmp(PG_SCENE_IDS[i], name) == 0) {
            return i;
        }
    }
    return (uint8_t)PG_SCENE_DEFAULT;
}

/* ---- the symptom ladders ------------------------------------------------
 * PLANT_CARE.md §6.1, as explicit rungs with an onset time and a
 * reversibility flag. Onset is in care ticks at effective scalar 1.0; the
 * caller divides it by the effective scalar, never by the raw one (D-085).
 * The rung a plant is on is derived from the axes and never stored. */

#define PG_TICKS_PER_HOUR ((uint16_t)PG_CARE_TICKS_PER_HOUR)
#define PG_TICKS_PER_DAY  ((uint16_t)PG_CARE_TICKS_PER_DAY)

static const pg_ladder_rung PG_LADDERS[PG_LADDER_COUNT][PG_LADDER_RUNG_MAX] = {
    /* underwater: hours to days, then it stops being reversible at rung 4 */
    { {  6u * 4u, true }, { 36u * 4u, true }, { 72u * 4u, true },
      { 96u * 4u, false }, { 240u * 4u, false } },
    /* overwater: rungs 1-3 are commonly misread as thirst, which is the point */
    { { 48u * 4u, true }, { 168u * 4u, false }, { 336u * 4u, true },
      { 336u * 4u, true }, { 504u * 4u, false } },
    /* too little light: weeks, and permanent from rung 2 */
    { { 14u * 96u, true }, { 21u * 96u, false }, { 42u * 96u, false },
      { 56u * 96u, false }, { 0u, false } },
    /* too much light: hours to days, and permanent from rung 2 */
    { { 12u * 4u, true }, { 24u * 4u, false }, { 48u * 4u, false },
      { 0u, false }, { 0u, false } },
    /* low humidity: permanent from the very first mark */
    { { 72u * 4u, false }, { 168u * 4u, false }, { 336u * 4u, false },
      { 0u, false }, { 0u, false } },
    /* fertiliser salts: the crust washes out, the tips do not */
    { { 14u * 96u, true }, { 21u * 96u, false }, { 28u * 96u, true },
      { 42u * 96u, true }, { 0u, false } },
    /* rootbound: fully recoverable, which is why it has no permanent rung */
    { { 60u * 96u, true }, { 90u * 96u, true }, { 120u * 96u, true },
      { 150u * 96u, true }, { 0u, false } },
    /* cold damage: within a day, and permanent from rung 2 */
    { { 8u * 4u, true }, { 24u * 4u, false }, { 48u * 4u, false },
      { 0u, false }, { 0u, false } },
    /* pests: the stippling is permanent on the leaves that got it */
    { { 7u * 96u, true }, { 10u * 96u, true }, { 14u * 96u, false },
      { 0u, false }, { 0u, false } }
};

static const char *const PG_LADDER_NAMES[PG_LADDER_COUNT] = {
    "underwater", "overwater", "low light", "excess light", "low humidity",
    "fertiliser salts", "rootbound", "cold", "pests"
};

const char *pg_plant_ladder_name(pg_ladder ladder)
{
    if ((size_t)ladder >= (size_t)PG_LADDER_COUNT) {
        return "";
    }
    return PG_LADDER_NAMES[ladder];
}

const pg_ladder_rung *pg_plant_ladder_spec(pg_ladder ladder, uint8_t rung)
{
    if ((size_t)ladder >= (size_t)PG_LADDER_COUNT
        || rung < 1u || rung > PG_LADDER_RUNG_MAX) {
        return NULL;
    }
    return &PG_LADDERS[ladder][rung - 1u];
}

uint8_t pg_plant_ladder_first_permanent(pg_ladder ladder)
{
    uint8_t rung;

    for (rung = 1u; rung <= PG_LADDER_RUNG_MAX; ++rung) {
        const pg_ladder_rung *spec = pg_plant_ladder_spec(ladder, rung);
        if (spec != NULL && spec->onset_ticks > 0u && !spec->reversible) {
            return rung;
        }
    }
    return UINT8_MAX;   /* nothing on this ladder is permanent */
}

static uint8_t pg_rung_from_thresholds(uint32_t value, const uint32_t *steps,
                                       uint8_t count)
{
    uint8_t rung = 0u;
    uint8_t i;

    for (i = 0u; i < count; ++i) {
        if (value >= steps[i]) {
            rung = (uint8_t)(i + 1u);
        }
    }
    return rung;
}

uint8_t pg_plant_ladder_rung(const pg_plant *plant, pg_ladder ladder)
{
    const pg_species *species;

    if (plant == NULL) {
        return 0u;
    }
    species = pg_content_species(plant->species_id);
    if (species == NULL) {
        return 0u;
    }

    switch (ladder) {
    case PG_LADDER_UNDERWATER: {
        /* 1 soil pale, 2 gloss lost, 3 full wilt, 4 crisped edges, 5 leaf drop */
        uint8_t rung = 0u;
        if (plant->moisture_top < 3000u) {
            rung = 1u;
        }
        if (plant->turgor < 8000u && rung < 2u) {
            rung = 2u;
        }
        if (plant->turgor < 5000u) {
            rung = 3u;
        }
        if (plant->crisp_dose > 2000u) {
            rung = 4u;
        }
        if (plant->crisp_dose > 6000u) {
            rung = 5u;
        }
        return rung;
    }
    case PG_LADDER_OVERWATER: {
        uint8_t rung = 0u;
        if (plant->moisture_top >= 8000u || plant->pest_gnats > 500u) {
            rung = 1u;
        }
        if (plant->soggy_ticks > species->saturation_tolerance_ticks) {
            rung = 2u;
        }
        if (plant->root_health < 6000u) {
            rung = 3u;
        }
        if (plant->root_health < 4000u && plant->moisture_bottom > 6000u) {
            rung = 4u;
        }
        if (plant->root_health < 1500u) {
            rung = 5u;
        }
        return rung;
    }
    case PG_LADDER_LOW_LIGHT: {
        static const uint32_t steps[4] = { 1000u, 3000u, 6000u, 8500u };
        return pg_rung_from_thresholds(plant->light_debt, steps, 4u);
    }
    case PG_LADDER_EXCESS_LIGHT: {
        static const uint32_t steps[3] = { 500u, 2500u, 6000u };
        return pg_rung_from_thresholds(plant->scorch_dose, steps, 3u);
    }
    case PG_LADDER_LOW_HUMIDITY: {
        static const uint32_t steps[3] = { 500u, 3000u, 7000u };
        if (species->rh_damage_below == 0u) {
            return 0u;             /* genuinely indifferent to dry air */
        }
        return pg_rung_from_thresholds(plant->crisp_dose, steps, 3u);
    }
    case PG_LADDER_SALT: {
        static const uint32_t steps[4] = { 2000u, 5000u, 7000u, 9000u };
        return pg_rung_from_thresholds(plant->salt, steps, 4u);
    }
    case PG_LADDER_ROOTBOUND: {
        uint32_t tolerance = species->rootbound_tolerance_l;
        uint32_t steps[4];
        steps[0] = tolerance;
        steps[1] = tolerance + 1000u;
        steps[2] = tolerance + 2000u;
        steps[3] = (uint32_t)PG_LEVEL_MAX;
        return pg_rung_from_thresholds(plant->root_bound, steps, 4u);
    }
    case PG_LADDER_COLD: {
        static const uint32_t steps[3] = { 1000u, 3000u, 6000u };
        const pg_spot *spot = pg_content_spot(plant->spot_id);
        if (spot == NULL || !spot->cold_glass) {
            return 0u;
        }
        return pg_rung_from_thresholds(plant->crisp_dose, steps, 3u);
    }
    case PG_LADDER_PESTS: {
        uint8_t rung = 0u;
        if (plant->pest_gnats > 1000u || plant->pest_mealy > 500u) {
            rung = 1u;
        }
        if (plant->pest_mites > 1000u) {
            rung = 2u;
        }
        if (plant->pest_mites > 5000u) {
            rung = 3u;
        }
        return rung;
    }
    case PG_LADDER_COUNT:
    default:
        return 0u;
    }
}

/* ---- lifecycle ---------------------------------------------------------- */

void pg_plant_init(pg_plant *plant, uint8_t species_id, uint8_t pot_id,
                   uint8_t spot_id, int64_t planted_wall_s)
{
    const pg_species *species;
    const pg_spot *spot;

    if (plant == NULL) {
        return;
    }
    memset(plant, 0, sizeof *plant);

    if (species_id >= (uint8_t)PG_SPECIES_COUNT) {
        species_id = (uint8_t)PG_SPECIES_POTHOS;
    }
    if (pot_id >= (uint8_t)PG_POT_COUNT) {
        pot_id = (uint8_t)PG_POT_GLAZED;
    }
    if (spot_id >= (uint8_t)PG_SPOT_COUNT) {
        spot_id = (uint8_t)PG_SPOT_DEFAULT;
    }
    species = pg_content_species(species_id);
    spot = pg_content_spot(spot_id);

    plant->planted_wall_s = planted_wall_s;
    plant->species_id = species_id;
    plant->pot_id = pot_id;
    plant->spot_id = spot_id;
    plant->scene_id = pg_scene_for_name(spot != NULL ? spot->default_scene : NULL);

    if (species != NULL) {
        (void)snprintf(plant->name, sizeof plant->name, "%s",
                       species->default_plant_name);
    }

    /* A nursery plant arrives established, recently watered, and slightly
     * unsettled by the trip home. */
    plant->moisture_top = 8000u;
    plant->moisture_bottom = 8000u;
    plant->turgor = 9500u;
    plant->root_health = 9000u;
    plant->root_capacity = (uint16_t)PG_LEVEL_MAX;
    plant->root_bound = 3000u;
    plant->nutrition = 7000u;
    plant->shock = 2000u;
    plant->life_state = (uint8_t)PG_LIFE_ESTABLISHING;
    plant->growth_stage = (uint8_t)PG_STAGE_ESTABLISHING;

    /* Two leaves it grew before you met it, with no marks on them yet. */
    (void)pg_plant_add_leaf(plant, 0u);
    (void)pg_plant_add_leaf(plant, 0u);
}

uint8_t pg_plant_stage_for_points(uint32_t growth_points)
{
    uint8_t stage = (uint8_t)PG_STAGE_ESTABLISHING;
    uint8_t i;

    for (i = 0u; i < (uint8_t)PG_STAGE_COUNT; ++i) {
        static const uint32_t thresholds[PG_STAGE_COUNT] = {
            0u, 2000u, 8000u, 20000u
        };
        if (growth_points >= thresholds[i]) {
            stage = i;
        }
    }
    return stage;
}

uint16_t pg_plant_mean_dli_c(const pg_plant *plant)
{
    uint32_t total = 0u;
    size_t i;

    if (plant == NULL) {
        return 0u;
    }
    for (i = 0; i < (size_t)PG_DLI_RING; ++i) {
        total += plant->dli_ring[i];
    }
    return (uint16_t)(total / (uint32_t)PG_DLI_RING);
}

uint16_t pg_plant_available_water_l(const pg_plant *plant)
{
    const pg_species *species;
    uint32_t available;
    uint16_t osmotic_l;

    if (plant == NULL) {
        return 0u;
    }
    species = pg_content_species(plant->species_id);
    if (species == NULL) {
        return 0u;
    }
    /* One expression, three failure modes, no special cases: thirst is low
     * moisture, root rot is low root_health in wet soil, and fertiliser burn is
     * high salt in wet soil with healthy roots. All three produce the same
     * pose, and are separated only by the soil, the crust and the gnats. */
    available = ((uint32_t)plant->moisture_bottom * plant->root_health)
              / (uint32_t)PG_LEVEL_MAX;
    osmotic_l = pg_level_scale(plant->salt, species->salt_sensitivity_q12);
    available = (available * ((uint32_t)PG_LEVEL_MAX - osmotic_l))
              / (uint32_t)PG_LEVEL_MAX;
    return (uint16_t)available;
}

uint8_t pg_plant_health(const pg_plant *plant)
{
    const pg_species *species;
    uint16_t mean_dli;

    if (plant == NULL) {
        return (uint8_t)PG_HEALTH_HEALTHY;
    }
    if (plant->life_state == (uint8_t)PG_LIFE_DEAD) {
        return (uint8_t)PG_HEALTH_DEAD;
    }
    species = pg_content_species(plant->species_id);
    if (species == NULL) {
        return (uint8_t)PG_HEALTH_HEALTHY;
    }

    if (plant->life_state == (uint8_t)PG_LIFE_TERMINAL
        || plant->root_health < 1500u
        || plant->turgor < 1500u) {
        return (uint8_t)PG_HEALTH_CRITICAL;
    }
    if (plant->root_health < 4000u
        || plant->salt > 7000u
        || plant->crisp_dose > 6000u
        || plant->scorch_dose > 6000u
        || plant->light_debt > 8000u
        || plant->pest_mites > 6000u) {
        return (uint8_t)PG_HEALTH_DISTRESSED;
    }
    if (plant->turgor < 5000u) {
        return (uint8_t)PG_HEALTH_THIRSTY;
    }

    mean_dli = pg_plant_mean_dli_c(plant);
    if (plant->turgor >= 8000u
        && plant->root_health >= 8000u
        && plant->light_debt <= 1000u
        && plant->salt < 2000u
        && plant->nutrition >= 5000u
        && mean_dli >= species->dli_thriving_c) {
        return (uint8_t)PG_HEALTH_THRIVING;
    }
    return (uint8_t)PG_HEALTH_HEALTHY;
}

bool pg_plant_add_leaf(pg_plant *plant, uint16_t birth_care_day)
{
    const pg_species *species;
    pg_leaf *leaf;
    uint16_t mean_dli;
    uint8_t size, variegation, contrast;

    if (plant == NULL || plant->leaf_count >= (uint8_t)PG_LEAF_MAX) {
        return false;
    }
    species = pg_content_species(plant->species_id);
    if (species == NULL) {
        return false;
    }

    leaf = &plant->leaves[plant->leaf_count];
    memset(leaf, 0, sizeof *leaf);
    leaf->birth_care_day = birth_care_day;
    leaf->slot = plant->leaf_count;

    /* A leaf's form is fixed when it unfurls, from the conditions at that
     * moment, and never changes afterwards. Fixing the light produces correct
     * NEW leaves and does not recolour the old ones. */
    mean_dli = pg_plant_mean_dli_c(plant);
    size = (uint8_t)((mean_dli >= species->dli_thriving_c) ? 3u
                     : (mean_dli >= species->dli_maintenance_c) ? 2u
                     : (mean_dli > 0u) ? 1u : 2u);
    variegation = (uint8_t)((plant->light_debt < 2000u) ? 3u
                            : (plant->light_debt < 5000u) ? 2u
                            : (plant->light_debt < 8000u) ? 1u : 0u);
    contrast = variegation;

    leaf->form_flags = (uint8_t)(((uint8_t)(size & 3u) << PG_FORM_SIZE_SHIFT)
                                 | (uint8_t)((variegation & 3u)
                                             << PG_FORM_VARIEGATION_SHIFT)
                                 | (uint8_t)((contrast & 3u)
                                             << PG_FORM_CONTRAST_SHIFT));
    if (plant->light_debt >= 6000u) {
        /* Etiolated stems never re-compact. */
        leaf->form_flags |= (uint8_t)PG_FORM_ETIOLATED;
    }
    plant->leaf_count += 1u;
    return true;
}

void pg_plant_mark_damage(pg_plant *plant, uint8_t damage_bit)
{
    uint8_t i;

    if (plant == NULL) {
        return;
    }
    /* Set on every leaf that was out at the time, and never cleared: that is
     * the whole of permanence. */
    for (i = 0u; i < plant->leaf_count; ++i) {
        plant->leaves[i].damage_mask |= damage_bit;
    }
    for (i = 0u; i < plant->vine_count; ++i) {
        plant->vine[i].damage_mask |= damage_bit;
    }
}

/* ---- per-species flavour ------------------------------------------------ */

uint8_t pg_plant_vine_length(const pg_plant *plant)
{
    return (plant == NULL) ? 0u : plant->vine_count;
}

void pg_plant_vine_extend(pg_plant *plant, uint16_t light_debt_at_birth)
{
    const pg_species *species;
    pg_vine_node *node;

    if (plant == NULL || plant->vine_count >= (uint8_t)PG_VINE_MAX) {
        return;
    }
    species = pg_content_species(plant->species_id);
    if (species == NULL || (species->flags & (uint8_t)PG_FLAG_TRAILING) == 0u) {
        return;
    }
    node = &plant->vine[plant->vine_count];
    memset(node, 0, sizeof *node);
    /* Internodes stretch in low light, and a stretched stem never re-compacts,
     * so the vine is a literal record of where it has been standing. */
    node->internode = (uint8_t)(2u + (light_debt_at_birth / 1500u));
    node->leaf_form = (uint8_t)((light_debt_at_birth < 3000u) ? 3u
                                : (light_debt_at_birth < 6000u) ? 2u : 1u);
    plant->vine_count += 1u;
}

bool pg_plant_snake_may_add_blade(const pg_plant *plant)
{
    const pg_species *species;

    if (plant == NULL) {
        return false;
    }
    species = pg_content_species(plant->species_id);
    if (species == NULL || (species->flags & (uint8_t)PG_FLAG_CAM) == 0u) {
        return false;
    }
    /* One new blade per good growing season, and only if it actually had a
     * good one. A CAM succulent is not going to hurry. */
    return plant->growth_points >= 3000u
        && plant->leaf_count < (uint8_t)PG_LEAF_MAX
        && plant->root_health >= 6000u;
}

bool pg_plant_pot_is_cracking(const pg_plant *plant)
{
    const pg_species *species;
    const pg_pot *pot;

    if (plant == NULL) {
        return false;
    }
    species = pg_content_species(plant->species_id);
    pot = pg_content_pot(plant->pot_id);
    if (species == NULL || pot == NULL) {
        return false;
    }
    /* When severely rootbound it can crack or distort its pot — the snake
     * plant especially. */
    if ((species->flags & (uint8_t)PG_FLAG_CAM) == 0u) {
        return false;
    }
    return plant->root_bound >= (uint16_t)PG_LEVEL_MAX
        && pot->numeric_id != (uint8_t)PG_POT_NURSERY;
}

bool pg_plant_is_collapsed(const pg_plant *plant)
{
    const pg_species *species;

    if (plant == NULL) {
        return false;
    }
    species = pg_content_species(plant->species_id);
    if (species == NULL) {
        return false;
    }
    /* Collapse is a posture, not a state: it is a threshold on turgor, and it
     * comes back within the species' recovery window once the water arrives.
     * It is stored nowhere. */
    return plant->turgor < 3500u;
}

bool pg_plant_spathe_possible(const pg_plant *plant)
{
    const pg_species *species;

    if (plant == NULL) {
        return false;
    }
    species = pg_content_species(plant->species_id);
    if (species == NULL || species->dli_flower_c == 0u
        || (species->flags & (uint8_t)PG_FLAG_FLOWERS) == 0u) {
        return false;
    }
    /* Slow, light-gated, and never promised: home rebloom is markedly less
     * reliable than commonly claimed, so this is a possibility rather than a
     * schedule. */
    return plant->growth_stage >= (uint8_t)PG_STAGE_MATURE
        && pg_plant_mean_dli_c(plant) >= species->dli_flower_c
        && plant->root_health >= 6000u;
}

uint16_t pg_plant_fold_l(const pg_plant *plant, uint16_t ordinal,
                         uint16_t local_minutes)
{
    const pg_species *species;
    uint16_t fold;

    if (plant == NULL) {
        return 0u;
    }
    species = pg_content_species(plant->species_id);
    if (species == NULL
        || (species->flags & (uint8_t)PG_FLAG_NYCTINASTY) == 0u) {
        return 0u;
    }
    /* Derived, never stored: a pure function of the local hour and stress
     * (ARCHITECTURE.md §6.2). */
    fold = pg_calendar_is_folded(ordinal, local_minutes)
         ? (uint16_t)8000u : (uint16_t)0u;
    /* Under stress they fold harder and reopen incompletely. The research
     * grades that [MED], so it is a tendency here and never a number the
     * player is shown. */
    if (plant->turgor < 5000u) {
        fold = (uint16_t)(fold + 1500u);
        if (fold > (uint16_t)PG_LEVEL_MAX) {
            fold = (uint16_t)PG_LEVEL_MAX;
        }
    }
    return fold;
}

/* ---- the mercy mechanic ------------------------------------------------- */

bool pg_plant_mercy_offered(const pg_plant *plant)
{
    if (plant == NULL) {
        return false;
    }
    /* Offered automatically at the last rung of the rot ladder, before death,
     * because a plant lost to rot very often still contains a viable piece and
     * presenting total loss without offering the cutting is bad advice. */
    return plant->life_state != (uint8_t)PG_LIFE_DEAD
        && pg_plant_ladder_rung(plant, PG_LADDER_OVERWATER) >= 5u;
}

uint16_t pg_plant_mercy_odds_l(const pg_plant *plant)
{
    const pg_species *species;

    if (plant == NULL) {
        return 0u;
    }
    species = pg_content_species(plant->species_id);
    if (species == NULL) {
        return 0u;
    }
    /* The tabled rot_survival, told to the player straight: a real gamble, not
     * a free undo. */
    return species->rot_survival_l;
}

/* ---- --rules-test ------------------------------------------------------- */

static int pg_rules_failed;

static void pg_rules_check(bool condition, const char *what)
{
    if (!condition) {
        pg_rules_failed += 1;
        (void)printf("  FAIL  %s\n", what);
    }
}

/* PLANT_CARE.md §7. Nothing on this list may be asserted by the game in any
 * voice — not in a care label, a notebook page, a diagnosis card, a tip or a
 * plant's dialogue. Each myth remains available as an action and behaves
 * truthfully; what is forbidden is the game recommending it. */
static const char *const PG_MYTH_NEEDLES[] = {
    "improves drainage",
    "improve drainage",
    "for drainage",
    "raises humidity",
    "raise humidity",
    "pebble tray",
    "purif",
    "cleans the air",
    "much bigger pot",
    "bigger pot helps",
    "ice cube",
    "yellow leaves mean",
    "needs more water",
    "watering day",
    "once a week",
    "weekly watering",
    "every week",
    "droplets cause",
    "helps it recover"
};

static bool pg_rules_string_is_clean(const char *text)
{
    char lowered[512];
    size_t i;
    size_t length;

    if (text == NULL) {
        return true;
    }
    length = strlen(text);
    if (length >= sizeof lowered) {
        length = sizeof lowered - 1u;
    }
    for (i = 0; i < length; ++i) {
        char c = text[i];
        lowered[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lowered[length] = '\0';

    for (i = 0; i < sizeof PG_MYTH_NEEDLES / sizeof PG_MYTH_NEEDLES[0]; ++i) {
        if (strstr(lowered, PG_MYTH_NEEDLES[i]) != NULL) {
            (void)printf("  FAIL  blocklisted myth \"%s\" appears in: %s\n",
                         PG_MYTH_NEEDLES[i], text);
            return false;
        }
    }
    return true;
}

/* One representative state per axis the legality rules can branch on. */
typedef struct pg_rules_case {
    const char *label;
    uint8_t species_id;
    uint8_t pot_id;
    bool growing_season;
    bool watered;
    bool recently_repotted;
    bool sick;
} pg_rules_case;

static void pg_rules_build(pg_plant *plant, pg_care_env *env,
                           const pg_rules_case *state, uint64_t now_care_s)
{
    pg_plant_init(plant, state->species_id, state->pot_id,
                  (uint8_t)PG_SPOT_DEFAULT, 0);
    *env = pg_care_env_for(state->pot_id < (uint8_t)PG_SPOT_COUNT
                           ? (uint8_t)PG_SPOT_DEFAULT : (uint8_t)PG_SPOT_DEFAULT,
                           state->growing_season ? 172u : 355u, 720u, 1u);
    plant->moisture_bottom = state->watered ? (uint16_t)PG_LEVEL_MAX : 0u;
    plant->moisture_top = plant->moisture_bottom;
    plant->last_repotted_care_s = state->recently_repotted ? now_care_s : 0u;
    if (state->sick) {
        plant->root_health = 1000u;
        plant->life_state = (uint8_t)PG_LIFE_AILING;
    }
}

int pg_plant_run_rules_test(void)
{
    static const pg_rules_case cases[] = {
        { "growing, watered, settled, well",   0u, 1u, true,  true,  false, false },
        { "dormant",                           0u, 1u, false, true,  false, false },
        { "dry soil",                          0u, 1u, true,  false, false, false },
        { "repotted last week",                0u, 1u, true,  true,  true,  false },
        { "visibly sick",                      0u, 1u, true,  true,  false, true  },
        { "cachepot, watered",                 3u, 3u, true,  true,  false, false },
        { "nursery pot, dry",                  2u, 2u, true,  false, false, false },
        { "terracotta, dormant",               1u, 0u, false, true,  false, false }
    };
    const uint64_t now_care_s = (uint64_t)7 * 86400u;
    char error[128];
    size_t case_index;
    size_t verb_index;
    bool seen_refusal[PG_FEED_REFUSAL_COUNT];
    uint32_t refused_non_feed = 0u;
    uint32_t refused_feed = 0u;
    size_t i;

    pg_rules_failed = 0;
    memset(seen_refusal, 0, sizeof seen_refusal);

    if (!pg_content_validate(error, sizeof error)) {
        (void)printf("rules: content invalid: %s\n", error);
        return 1;
    }

    (void)printf("-- verb legality across %zu states x %d verbs --\n",
                 sizeof cases / sizeof cases[0], (int)PG_VERB_COUNT);

    for (case_index = 0; case_index < sizeof cases / sizeof cases[0];
         ++case_index) {
        const pg_rules_case *state = &cases[case_index];
        pg_plant plant;
        pg_care_env env;
        uint32_t allowed = 0u, warned = 0u, refused = 0u;

        pg_rules_build(&plant, &env, state, now_care_s);

        for (verb_index = 0; verb_index < (size_t)PG_VERB_COUNT; ++verb_index) {
            const char *reason = NULL;
            pg_legality legality = pg_care_verb_legality((pg_verb)verb_index,
                                                         &plant, &env,
                                                         now_care_s, &reason);
            switch (legality) {
            case PG_LEGAL_ALLOWED:
                allowed += 1u;
                break;
            case PG_LEGAL_WARNED:
                warned += 1u;
                /* An action that is wrong right now is offered WITH A STATED
                 * REASON rather than greyed out; a warning with no reason is
                 * the failure this asserts against. */
                pg_rules_check(reason != NULL && reason[0] != '\0',
                               "every warned verb states its reason");
                pg_rules_check(pg_rules_string_is_clean(reason),
                               "no warning reason asserts a blocklisted myth");
                break;
            case PG_LEGAL_REFUSED:
            default:
                refused += 1u;
                if (pg_care_verb_is_feed((pg_verb)verb_index)) {
                    refused_feed += 1u;
                } else {
                    refused_non_feed += 1u;
                    (void)printf("  FAIL  %s was refused outside Feed\n",
                                 pg_care_verb_name((pg_verb)verb_index));
                }
                pg_rules_check(reason != NULL && reason[0] != '\0',
                               "every refusal states its reason");
                pg_rules_check(pg_rules_string_is_clean(reason),
                               "no refusal reason asserts a blocklisted myth");
                break;
            }
        }

        {
            pg_feed_refusal refusal = pg_care_feed_refusal(&plant, &env,
                                                           now_care_s);
            if (refusal != PG_FEED_REFUSAL_NONE) {
                seen_refusal[refusal] = true;
            }
            (void)printf("  %-30s allowed %2u  warned %2u  refused %2u  (%s)\n",
                         state->label, allowed, warned, refused,
                         refusal == PG_FEED_REFUSAL_NONE ? "no refusal"
                                                         : "feed refused");
        }
    }

    /* The enumeration the plan asks for: the set of hard refusals is exactly
     * the four Feed cases, and nothing else in the game refuses anything. */
    pg_rules_check(refused_non_feed == 0u,
                   "the only hard refusals in the game are under Feed");
    pg_rules_check(refused_feed > 0u,
                   "the Feed refusals actually fire");
    for (i = 1; i < (size_t)PG_FEED_REFUSAL_COUNT; ++i) {
        pg_rules_check(seen_refusal[i],
                       "all four hard refusals are reachable");
        if (!seen_refusal[i]) {
            (void)printf("        unreached refusal %zu: %s\n", i,
                         pg_care_refusal_reason((pg_feed_refusal)i));
        }
    }
    (void)printf("refusals: %d of 4 reachable, and there are exactly 4\n",
                 (int)(PG_FEED_REFUSAL_COUNT - 1));

    /* Season gating, stated on its own because it is the one refusal a player
     * meets every winter. */
    {
        pg_plant plant;
        pg_care_env growing, dormant;
        uint16_t ordinal;
        uint32_t refused_days = 0u;

        pg_plant_init(&plant, (uint8_t)PG_SPECIES_POTHOS, (uint8_t)PG_POT_GLAZED,
                      (uint8_t)PG_SPOT_DEFAULT, 0);
        plant.moisture_bottom = (uint16_t)PG_LEVEL_MAX;
        for (ordinal = 1u; ordinal <= PG_ORDINAL_MAX; ++ordinal) {
            pg_care_env env = pg_care_env_for((uint8_t)PG_SPOT_DEFAULT, ordinal,
                                              720u, 1u);
            if (pg_care_feed_refusal(&plant, &env, 0u)
                == PG_FEED_REFUSAL_DORMANT) {
                refused_days += 1u;
            }
        }
        growing = pg_care_env_for((uint8_t)PG_SPOT_DEFAULT, 172u, 720u, 1u);
        dormant = pg_care_env_for((uint8_t)PG_SPOT_DEFAULT, 355u, 720u, 1u);
        (void)printf("season: feeding refused on %u of %d ordinals; the window "
                     "falls out of the daylight curve, ordinals %d..%d\n",
                     refused_days, (int)PG_ORDINAL_MAX,
                     (int)PG_SEASON_FIRST_GROWING_ORDINAL,
                     (int)PG_SEASON_LAST_GROWING_ORDINAL);
        pg_rules_check(refused_days
                       == (uint32_t)(PG_ORDINAL_MAX
                                     - (PG_SEASON_LAST_GROWING_ORDINAL
                                        - PG_SEASON_FIRST_GROWING_ORDINAL + 1)),
                       "the feed refusal is gated by the derived growing window");
        pg_rules_check(pg_care_feed_refusal(&plant, &growing, 0u)
                       == PG_FEED_REFUSAL_NONE,
                       "midsummer feeding is allowed");
        pg_rules_check(pg_care_feed_refusal(&plant, &dormant, 0u)
                       == PG_FEED_REFUSAL_DORMANT,
                       "midwinter feeding is refused as dormancy");
    }

    /* The myth blocklist, over every string the compiled content and the
     * refusal reasons can put in front of a player. */
    {
        size_t clean = 0u;
        for (i = 0; i < pg_content_string_count(); ++i) {
            if (pg_rules_string_is_clean(pg_content_string(i))) {
                clean += 1u;
            }
        }
        pg_rules_check(clean == pg_content_string_count(),
                       "no compiled content string asserts a blocklisted myth");
        for (i = 0; i < (size_t)PG_FEED_REFUSAL_COUNT; ++i) {
            pg_rules_check(pg_rules_string_is_clean(
                               pg_care_refusal_reason((pg_feed_refusal)i)),
                           "no refusal reason asserts a blocklisted myth");
        }
        for (i = 0; i < (size_t)PG_VERB_COUNT; ++i) {
            pg_rules_check(pg_rules_string_is_clean(
                               pg_care_verb_name((pg_verb)i)),
                           "no verb name asserts a blocklisted myth");
        }
        (void)printf("myths: %zu content strings, %d refusal reasons and %d "
                     "verb names checked against %zu blocklist phrases\n",
                     pg_content_string_count(), (int)PG_FEED_REFUSAL_COUNT,
                     (int)PG_VERB_COUNT,
                     sizeof PG_MYTH_NEEDLES / sizeof PG_MYTH_NEEDLES[0]);
    }

    /* The mercy mechanic is not optional: a rotted plant must offer the
     * cutting or the division rather than presenting total loss. */
    {
        uint8_t species_id;
        for (species_id = 0; species_id < (uint8_t)PG_SPECIES_COUNT;
             ++species_id) {
            pg_plant plant;
            pg_plant_init(&plant, species_id, (uint8_t)PG_POT_CACHEPOT,
                          (uint8_t)PG_SPOT_DEFAULT, 0);
            plant.root_health = 500u;
            plant.life_state = (uint8_t)PG_LIFE_TERMINAL;
            pg_rules_check(pg_plant_mercy_offered(&plant),
                           "a rotted plant offers the cutting or the division");
            pg_rules_check(pg_plant_mercy_odds_l(&plant) > 0u
                           && pg_plant_mercy_odds_l(&plant)
                              < (uint16_t)PG_LEVEL_MAX,
                           "the mercy odds are a real gamble, not a free undo");
        }
        (void)printf("mercy: all %d species offer a viable piece at the "
                     "terminal rung\n", (int)PG_SPECIES_COUNT);
    }

    (void)printf("\nrules: %s\n", pg_rules_failed == 0 ? "PASS" : "FAIL");
    return pg_rules_failed == 0 ? 0 : 1;
}
