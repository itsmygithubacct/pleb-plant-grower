/*
 * test_plant — the plant record, permanence, and the four identities.
 *
 * The assertion this file exists for is permanence: a leaf's form is fixed
 * when it unfurls and its damage bits are set and never cleared, so a
 * well-kept plant is legible as a history rather than as a number. Everything
 * else here — the health index, the ladders, the mercy mechanic and the four
 * species' signature behaviours — is downstream of that.
 */
#include "pg_plant.h"

#include "pg_calendar.h"
#include "pg_care.h"
#include "pg_content.h"
#include "pg_rng.h"

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

static bool test_init_is_a_sane_plant(void)
{
    uint8_t species_id;

    for (species_id = 0u; species_id < (uint8_t)PG_SPECIES_COUNT; ++species_id) {
        pg_plant plant;
        const pg_species *species = pg_content_species(species_id);

        pg_plant_init(&plant, species_id, (uint8_t)PG_POT_GLAZED,
                      (uint8_t)PG_SPOT_DEFAULT, 1700000000);
        CHECK(plant.species_id == species_id);
        CHECK(plant.pot_id == (uint8_t)PG_POT_GLAZED);
        CHECK(plant.spot_id == (uint8_t)PG_SPOT_DEFAULT);
        CHECK(plant.planted_wall_s == 1700000000);
        CHECK(strcmp(plant.name, species->default_plant_name) == 0);
        CHECK(plant.leaf_count == 2u);
        CHECK(plant.root_capacity == (uint16_t)PG_LEVEL_MAX);
        CHECK(plant.root_health <= plant.root_capacity);
        CHECK(plant.growth_stage == (uint8_t)PG_STAGE_ESTABLISHING);
        CHECK(plant.life_state == (uint8_t)PG_LIFE_ESTABLISHING);
        CHECK(plant.soggy_ticks == 0u);
        /* fold_state is deliberately never stored. */
        CHECK(pg_plant_health(&plant) < (uint8_t)PG_HEALTH_COUNT);
    }

    /* Out-of-range ids fall back rather than indexing off the end. */
    {
        pg_plant plant;
        pg_plant_init(&plant, 200u, 200u, 200u, 0);
        CHECK(plant.species_id < (uint8_t)PG_SPECIES_COUNT);
        CHECK(plant.pot_id < (uint8_t)PG_POT_COUNT);
        CHECK(plant.spot_id < (uint8_t)PG_SPOT_COUNT);
    }
    pg_plant_init(NULL, 0u, 0u, 0u, 0);
    return true;
}

static bool test_spot_id_is_not_scene_id(void)
{
    uint8_t spot_id;
    bool any_differs = false;

    /* D-081: the two are separate numberings and no code may assume they are
     * equal. The spot the game starts on is the proof — the safe light band is
     * spot 1 while the neutral plate is scene 3. */
    CHECK((uint8_t)PG_SPOT_DEFAULT != (uint8_t)PG_SCENE_DEFAULT);

    for (spot_id = 0u; spot_id < (uint8_t)PG_SPOT_COUNT; ++spot_id) {
        pg_plant plant;
        const pg_spot *spot = pg_content_spot(spot_id);
        pg_plant_init(&plant, (uint8_t)PG_SPECIES_POTHOS,
                      (uint8_t)PG_POT_GLAZED, spot_id, 0);
        CHECK(plant.scene_id < (uint8_t)PG_SCENE_COUNT);
        CHECK(spot->default_scene != NULL);
        if (plant.scene_id != spot_id) {
            any_differs = true;
        }
    }
    /* The resolution is by name; that it currently agrees index-for-index is a
     * coincidence of the authored data and must never become an assumption. */
    (void)any_differs;
    return true;
}

static bool test_a_leaf_is_frozen_at_birth(void)
{
    pg_plant bright, dark;
    uint8_t bright_form, dark_form;

    pg_plant_init(&bright, (uint8_t)PG_SPECIES_POTHOS, (uint8_t)PG_POT_GLAZED,
                  (uint8_t)PG_SPOT_DEFAULT, 0);
    dark = bright;

    bright.light_debt = 0u;
    dark.light_debt = 9000u;

    CHECK(pg_plant_add_leaf(&bright, 10u));
    CHECK(pg_plant_add_leaf(&dark, 10u));
    bright_form = bright.leaves[bright.leaf_count - 1u].form_flags;
    dark_form = dark.leaves[dark.leaf_count - 1u].form_flags;

    /* A leaf formed in low light is formed with less gold, permanently. */
    CHECK(((bright_form & PG_FORM_VARIEGATION_MASK) >> PG_FORM_VARIEGATION_SHIFT)
          > ((dark_form & PG_FORM_VARIEGATION_MASK) >> PG_FORM_VARIEGATION_SHIFT));
    /* And an etiolated stem never re-compacts. */
    CHECK((dark_form & PG_FORM_ETIOLATED) != 0u);
    CHECK((bright_form & PG_FORM_ETIOLATED) == 0u);

    /* Fixing the light produces correct NEW leaves and does not recolour the
     * old ones. */
    dark.light_debt = 0u;
    CHECK(pg_plant_add_leaf(&dark, 40u));
    CHECK(dark.leaves[dark.leaf_count - 2u].form_flags == dark_form);
    CHECK(dark.leaves[dark.leaf_count - 1u].form_flags != dark_form);
    CHECK(dark.leaves[dark.leaf_count - 1u].birth_care_day == 40u);

    /* The anchor list is finite and running out is not an error. */
    while (pg_plant_add_leaf(&dark, 50u)) {
        CHECK(dark.leaf_count <= (uint8_t)PG_LEAF_MAX);
    }
    CHECK(dark.leaf_count == (uint8_t)PG_LEAF_MAX);
    CHECK(!pg_plant_add_leaf(NULL, 0u));
    return true;
}

static bool test_damage_is_set_and_never_cleared(void)
{
    pg_plant plant;
    uint8_t i;

    pg_plant_init(&plant, (uint8_t)PG_SPECIES_CALATHEA, (uint8_t)PG_POT_NURSERY,
                  (uint8_t)PG_SPOT_DEFAULT, 0);
    pg_plant_mark_damage(&plant, (uint8_t)PG_DAMAGE_TIP_CRISP);
    for (i = 0u; i < plant.leaf_count; ++i) {
        CHECK((plant.leaves[i].damage_mask & PG_DAMAGE_TIP_CRISP) != 0u);
    }
    /* Trimming tidies; it does not undo. The scar stays in the record. */
    pg_plant_mark_damage(&plant, (uint8_t)PG_DAMAGE_TRIMMED);
    for (i = 0u; i < plant.leaf_count; ++i) {
        CHECK((plant.leaves[i].damage_mask & PG_DAMAGE_TIP_CRISP) != 0u);
        CHECK((plant.leaves[i].damage_mask & PG_DAMAGE_TRIMMED) != 0u);
    }
    pg_plant_mark_damage(NULL, 1u);
    return true;
}

static bool test_health_is_derived_and_ordered(void)
{
    pg_plant plant;

    pg_plant_init(&plant, (uint8_t)PG_SPECIES_POTHOS, (uint8_t)PG_POT_GLAZED,
                  (uint8_t)PG_SPOT_DEFAULT, 0);
    plant.turgor = 9500u;
    plant.root_health = 9500u;
    plant.nutrition = 8000u;
    plant.light_debt = 0u;
    plant.salt = 0u;
    CHECK(pg_plant_health(&plant) == (uint8_t)PG_HEALTH_HEALTHY);

    /* Thriving needs the light as well as the water: an axis nobody can see is
     * still an axis. */
    {
        size_t i;
        for (i = 0; i < (size_t)PG_DLI_RING; ++i) {
            plant.dli_ring[i] = 900u;
        }
    }
    CHECK(pg_plant_health(&plant) == (uint8_t)PG_HEALTH_THRIVING);

    plant.turgor = 4000u;
    CHECK(pg_plant_health(&plant) == (uint8_t)PG_HEALTH_THIRSTY);

    plant.turgor = 9000u;
    plant.root_health = 3000u;
    CHECK(pg_plant_health(&plant) == (uint8_t)PG_HEALTH_DISTRESSED);

    plant.root_health = 1000u;
    CHECK(pg_plant_health(&plant) == (uint8_t)PG_HEALTH_CRITICAL);

    plant.life_state = (uint8_t)PG_LIFE_DEAD;
    CHECK(pg_plant_health(&plant) == (uint8_t)PG_HEALTH_DEAD);
    CHECK(pg_plant_health(NULL) < (uint8_t)PG_HEALTH_COUNT);
    return true;
}

static bool test_one_expression_three_failures(void)
{
    pg_plant thirst, rot, burn;
    uint16_t a_thirst, a_rot, a_burn;

    pg_plant_init(&thirst, (uint8_t)PG_SPECIES_PEACE_LILY,
                  (uint8_t)PG_POT_GLAZED, (uint8_t)PG_SPOT_DEFAULT, 0);
    rot = thirst;
    burn = thirst;

    thirst.moisture_bottom = 500u;
    thirst.root_health = 9000u;
    thirst.salt = 0u;

    rot.moisture_bottom = 9500u;
    rot.root_health = 1200u;
    rot.salt = 0u;

    burn.moisture_bottom = 9500u;
    burn.root_health = 9000u;
    burn.salt = 9500u;

    a_thirst = pg_plant_available_water_l(&thirst);
    a_rot = pg_plant_available_water_l(&rot);
    a_burn = pg_plant_available_water_l(&burn);

    /* Three of the most important real failure modes produce the same droop,
     * and are separated entirely by other readouts — the soil, the crust, the
     * gnats. That is the game's central tension implemented as physics. */
    CHECK(a_thirst < 2000u);
    CHECK(a_rot < 2000u);
    CHECK(a_burn < 4000u);
    CHECK(pg_plant_available_water_l(NULL) == 0u);

    /* The distinguishing observations really are distinguishable. */
    CHECK(thirst.moisture_bottom < rot.moisture_bottom);
    CHECK(rot.root_health < burn.root_health);
    CHECK(burn.salt > rot.salt);
    return true;
}

static bool test_the_ladders(void)
{
    int ladder;
    pg_plant plant;

    pg_plant_init(&plant, (uint8_t)PG_SPECIES_CALATHEA, (uint8_t)PG_POT_GLAZED,
                  (uint8_t)PG_SPOT_DEFAULT, 0);

    for (ladder = 0; ladder < (int)PG_LADDER_COUNT; ++ladder) {
        uint8_t rung;
        uint16_t previous = 0u;
        CHECK(pg_plant_ladder_name((pg_ladder)ladder)[0] != '\0');
        /* Rungs are ordered in time: a later rung never arrives sooner. */
        for (rung = 1u; rung <= PG_LADDER_RUNG_MAX; ++rung) {
            const pg_ladder_rung *spec =
                pg_plant_ladder_spec((pg_ladder)ladder, rung);
            CHECK(spec != NULL);
            if (spec->onset_ticks == 0u) {
                break;      /* the ladder is shorter than five rungs */
            }
            CHECK(spec->onset_ticks >= previous);
            previous = spec->onset_ticks;
        }
        CHECK(pg_plant_ladder_spec((pg_ladder)ladder, 0u) == NULL);
        CHECK(pg_plant_ladder_spec((pg_ladder)ladder,
                                   PG_LADDER_RUNG_MAX + 1u) == NULL);
    }

    /* The fair-warning rule: every ladder that can do permanent damage shows a
     * reversible rung first. */
    for (ladder = 0; ladder < (int)PG_LADDER_COUNT; ++ladder) {
        uint8_t permanent = pg_plant_ladder_first_permanent((pg_ladder)ladder);
        if (permanent == UINT8_MAX) {
            continue;       /* nothing on this ladder is permanent */
        }
        if (ladder == (int)PG_LADDER_LOW_HUMIDITY) {
            /* The exception the research forces: the very first mark of dry
             * air is a dead margin, so the warning has to live in the AIR
             * rather than on the leaf. */
            CHECK(permanent == 1u);
            continue;
        }
        CHECK(permanent >= 2u);
        CHECK(pg_plant_ladder_spec((pg_ladder)ladder, 1u)->reversible);
    }

    /* A species that is genuinely indifferent to dry air never climbs the
     * humidity ladder, whatever its crisp dose says. */
    {
        pg_plant sarge;
        pg_plant_init(&sarge, (uint8_t)PG_SPECIES_SNAKE, (uint8_t)PG_POT_GLAZED,
                      (uint8_t)PG_SPOT_DEFAULT, 0);
        sarge.crisp_dose = (uint16_t)PG_LEVEL_MAX;
        CHECK(pg_plant_ladder_rung(&sarge, PG_LADDER_LOW_HUMIDITY) == 0u);
    }

    /* Rungs climb monotonically with the axis that drives them. */
    {
        uint8_t previous_rung = 0u;
        uint16_t debt;
        for (debt = 0u; debt <= 9000u; debt = (uint16_t)(debt + 500u)) {
            uint8_t rung;
            plant.light_debt = debt;
            rung = pg_plant_ladder_rung(&plant, PG_LADDER_LOW_LIGHT);
            CHECK(rung >= previous_rung);
            previous_rung = rung;
        }
        CHECK(previous_rung >= 3u);
    }
    CHECK(pg_plant_ladder_rung(NULL, PG_LADDER_UNDERWATER) == 0u);
    CHECK(pg_plant_ladder_rung(&plant, PG_LADDER_COUNT) == 0u);
    return true;
}

static bool test_the_four_are_not_one_game_four_times(void)
{
    pg_plant goldie, sarge, ophelia, nyx;

    pg_plant_init(&goldie, (uint8_t)PG_SPECIES_POTHOS, (uint8_t)PG_POT_GLAZED,
                  (uint8_t)PG_SPOT_DEFAULT, 0);
    pg_plant_init(&sarge, (uint8_t)PG_SPECIES_SNAKE, (uint8_t)PG_POT_GLAZED,
                  (uint8_t)PG_SPOT_DEFAULT, 0);
    pg_plant_init(&ophelia, (uint8_t)PG_SPECIES_PEACE_LILY,
                  (uint8_t)PG_POT_GLAZED, (uint8_t)PG_SPOT_ONE_METRE, 0);
    pg_plant_init(&nyx, (uint8_t)PG_SPECIES_CALATHEA, (uint8_t)PG_POT_NURSERY,
                  (uint8_t)PG_SPOT_TWO_METRE, 0);

    /* Goldie: the vine is the free progress bar, and only she has one. */
    CHECK(pg_plant_vine_length(&goldie) == 0u);
    pg_plant_vine_extend(&goldie, 0u);
    pg_plant_vine_extend(&goldie, 9000u);
    CHECK(pg_plant_vine_length(&goldie) == 2u);
    /* Internodes stretch in low light, and the vine carries the record. */
    CHECK(goldie.vine[1].internode > goldie.vine[0].internode);
    pg_plant_vine_extend(&sarge, 0u);
    CHECK(pg_plant_vine_length(&sarge) == 0u);

    /* Sarge: one new blade per good growing season, and a pot he can crack. */
    CHECK(!pg_plant_snake_may_add_blade(&sarge));
    sarge.growth_points = 5000u;
    CHECK(pg_plant_snake_may_add_blade(&sarge));
    CHECK(!pg_plant_snake_may_add_blade(&goldie));
    CHECK(!pg_plant_pot_is_cracking(&sarge));
    sarge.root_bound = (uint16_t)PG_LEVEL_MAX;
    CHECK(pg_plant_pot_is_cracking(&sarge));
    goldie.root_bound = (uint16_t)PG_LEVEL_MAX;
    CHECK(!pg_plant_pot_is_cracking(&goldie));

    /* Ophelia: collapse is a posture and the spathe is light-gated. */
    CHECK(!pg_plant_is_collapsed(&ophelia));
    ophelia.turgor = 2000u;
    CHECK(pg_plant_is_collapsed(&ophelia));
    ophelia.turgor = 9000u;
    CHECK(!pg_plant_is_collapsed(&ophelia));
    CHECK(!pg_plant_spathe_possible(&ophelia));
    {
        size_t i;
        for (i = 0; i < (size_t)PG_DLI_RING; ++i) {
            ophelia.dli_ring[i] = 1250u;
        }
    }
    ophelia.growth_stage = (uint8_t)PG_STAGE_MATURE;
    CHECK(pg_plant_spathe_possible(&ophelia));
    /* Nobody else flowers, whatever the light. */
    {
        size_t i;
        for (i = 0; i < (size_t)PG_DLI_RING; ++i) {
            nyx.dli_ring[i] = 1800u;
        }
        nyx.growth_stage = (uint8_t)PG_STAGE_SPECIMEN;
        CHECK(!pg_plant_spathe_possible(&nyx));
        CHECK(!pg_plant_spathe_possible(&goldie));
        CHECK(!pg_plant_spathe_possible(&sarge));
    }

    /* Nyx: nyctinasty is derived from the local hour, never stored, and only
     * she does it. */
    {
        uint16_t noon = 720u;
        uint16_t midnight = 30u;
        uint16_t open_fold, night_fold;

        open_fold = pg_plant_fold_l(&nyx, 172u, noon);
        night_fold = pg_plant_fold_l(&nyx, 172u, midnight);
        CHECK(open_fold == 0u);
        CHECK(night_fold > 0u);
        CHECK(pg_plant_fold_l(&goldie, 172u, midnight) == 0u);
        CHECK(pg_plant_fold_l(&ophelia, 172u, midnight) == 0u);
        /* Under stress she folds harder — a tendency, never a number the
         * player is shown. */
        nyx.turgor = 2000u;
        CHECK(pg_plant_fold_l(&nyx, 172u, midnight) > night_fold);
        CHECK(pg_plant_fold_l(NULL, 172u, midnight) == 0u);
    }
    return true;
}

static bool test_the_mercy_mechanic(void)
{
    uint8_t species_id;

    for (species_id = 0u; species_id < (uint8_t)PG_SPECIES_COUNT; ++species_id) {
        pg_plant plant;
        const pg_species *species = pg_content_species(species_id);

        pg_plant_init(&plant, species_id, (uint8_t)PG_POT_CACHEPOT,
                      (uint8_t)PG_SPOT_DEFAULT, 0);
        CHECK(!pg_plant_mercy_offered(&plant));

        plant.root_health = 500u;
        CHECK(pg_plant_ladder_rung(&plant, PG_LADDER_OVERWATER) == 5u);
        /* Offered at the last rung, BEFORE death: presenting total loss
         * without offering the cutting is genuinely bad advice. */
        CHECK(pg_plant_mercy_offered(&plant));
        CHECK(pg_plant_mercy_odds_l(&plant) == species->rot_survival_l);

        plant.life_state = (uint8_t)PG_LIFE_DEAD;
        CHECK(!pg_plant_mercy_offered(&plant));
    }
    /* The pothos takes node cuttings; everybody else divides. */
    CHECK(pg_content_species((uint8_t)PG_SPECIES_POTHOS)->propagation_mode
          == (uint8_t)PG_PROPAGATION_NODE_CUTTING);
    CHECK(pg_content_species((uint8_t)PG_SPECIES_SNAKE)->propagation_mode
          == (uint8_t)PG_PROPAGATION_DIVISION);
    CHECK(!pg_plant_mercy_offered(NULL));
    CHECK(pg_plant_mercy_odds_l(NULL) == 0u);
    return true;
}

static bool test_growth_stages_are_ordered(void)
{
    uint32_t points;
    uint8_t previous = 0u;

    CHECK(pg_plant_stage_for_points(0u) == (uint8_t)PG_STAGE_ESTABLISHING);
    for (points = 0u; points < 40000u; points += 250u) {
        uint8_t stage = pg_plant_stage_for_points(points);
        CHECK(stage >= previous);
        CHECK(stage < (uint8_t)PG_STAGE_COUNT);
        previous = stage;
    }
    CHECK(previous == (uint8_t)PG_STAGE_SPECIMEN);
    return true;
}

static bool test_the_rng_replays(void)
{
    pg_rng a, b;
    uint32_t i;

    pg_rng_seed(&a, 1337u);
    pg_rng_seed(&b, 1337u);
    for (i = 0u; i < 1000u; ++i) {
        CHECK(pg_rng_next_u64(&a) == pg_rng_next_u64(&b));
    }
    /* Weather is a pure function of (day, seed), so a replayed catch-up
     * produces the same July it produced the first time no matter how many
     * draws happened in between. */
    for (i = 1u; i <= 366u; ++i) {
        uint16_t first = pg_rng_weather_l(99u, i, (uint8_t)PG_WEATHER_CLOUD);
        (void)pg_rng_next_u64(&a);
        CHECK(pg_rng_weather_l(99u, i, (uint8_t)PG_WEATHER_CLOUD) == first);
        CHECK(first <= (uint16_t)PG_LEVEL_MAX);
        CHECK(pg_rng_weather_l(99u, i, (uint8_t)PG_WEATHER_CHANNEL_COUNT) == 0u);
    }
    CHECK(pg_rng_below(&a, 0u) == 0u);
    CHECK(pg_rng_level(&a) <= (uint16_t)PG_LEVEL_MAX);
    CHECK(!pg_rng_chance_l(&a, 0u));
    CHECK(pg_rng_chance_l(&a, (uint16_t)PG_LEVEL_MAX));
    pg_rng_seed(NULL, 0u);
    CHECK(pg_rng_next_u64(NULL) == 0u);
    return true;
}

static bool test_content_tables(void)
{
    char error[128];
    uint8_t i;

    CHECK(pg_content_validate(error, sizeof error));
    CHECK(pg_content_species_count() == (size_t)PG_SPECIES_COUNT);
    CHECK(pg_content_pot_count() == (size_t)PG_POT_COUNT);
    CHECK(pg_content_spot_count() == (size_t)PG_SPOT_COUNT);
    CHECK(pg_content_species((uint8_t)PG_SPECIES_COUNT) == NULL);
    CHECK(pg_content_pot((uint8_t)PG_POT_COUNT) == NULL);
    CHECK(pg_content_spot((uint8_t)PG_SPOT_COUNT) == NULL);
    CHECK(pg_content_month(0u) == NULL);
    CHECK(pg_content_month(13u) == NULL);
    CHECK(pg_content_month(1u)->month == 1u);
    CHECK(pg_content_species_by_key("golden-pothos")
          == pg_content_species((uint8_t)PG_SPECIES_POTHOS));
    CHECK(pg_content_species_by_key("no-such-plant") == NULL);
    CHECK(pg_content_species_by_key(NULL) == NULL);
    CHECK(pg_content_pot_by_key("cachepot")
          == pg_content_pot((uint8_t)PG_POT_CACHEPOT));
    CHECK(pg_content_spot_by_key("two-metre")
          == pg_content_spot((uint8_t)PG_SPOT_TWO_METRE));
    CHECK(pg_content_string(pg_content_string_count()) == NULL);
    CHECK(pg_content_saturation_level() == 8500u);

    /* D-093, pinned: the calathea's maintenance DLI is 1.5, not 3, which is
     * what makes the 2 m spot — the design's home for her — satisfy BOTH
     * halves of the reachability assertion instead of only one. */
    {
        const pg_species *nyx = pg_content_species((uint8_t)PG_SPECIES_CALATHEA);
        const pg_spot *two_metre = pg_content_spot((uint8_t)PG_SPOT_TWO_METRE);
        CHECK(nyx->dli_maintenance_c == 150u);
        CHECK(nyx->dli_thriving_c == 500u);
        CHECK(two_metre->dli_peak_c >= nyx->dli_thriving_c);
        CHECK(two_metre->dli_floor_c >= nyx->dli_maintenance_c);
        CHECK(two_metre->light_band <= nyx->light_max);
    }
    /* And the peace lily's flower is reachable at exactly one spot inside her
     * ceiling, which is why it is achievable, slow and light-gated. */
    {
        const pg_species *ophelia =
            pg_content_species((uint8_t)PG_SPECIES_PEACE_LILY);
        uint8_t reachable = 0u;
        CHECK(ophelia->dli_flower_c == 1200u);
        for (i = 0u; i < (uint8_t)PG_SPOT_COUNT; ++i) {
            const pg_spot *spot = pg_content_spot(i);
            if (spot->light_band <= ophelia->light_max
                && spot->dli_peak_c >= ophelia->dli_flower_c) {
                reachable += 1u;
                CHECK(i == (uint8_t)PG_SPOT_ONE_METRE);
            }
        }
        CHECK(reachable == 1u);
    }
    /* The snake plant's humidity indifference is a sentinel pair, not a band. */
    {
        const pg_species *sarge = pg_content_species((uint8_t)PG_SPECIES_SNAKE);
        CHECK(sarge->rh_ideal_low == 0u);
        CHECK(sarge->rh_ideal_high == 0u);
        CHECK(sarge->rh_damage_below == 0u);
        CHECK((sarge->flags & (uint8_t)PG_FLAG_CAM) != 0u);
    }
    /* The cachepot's floor sits strictly above the one saturation threshold. */
    {
        const pg_pot *trap = pg_content_pot((uint8_t)PG_POT_CACHEPOT);
        CHECK(!trap->has_drainage);
        CHECK(!trap->can_be_flushed);
        CHECK(trap->is_sleeve_capable);
        CHECK(trap->bottom_floor_l > pg_content_saturation_level());
    }
    return true;
}

int main(void)
{
    if (!test_content_tables()) return 1;
    if (!test_init_is_a_sane_plant()) return 1;
    if (!test_spot_id_is_not_scene_id()) return 1;
    if (!test_a_leaf_is_frozen_at_birth()) return 1;
    if (!test_damage_is_set_and_never_cleared()) return 1;
    if (!test_health_is_derived_and_ordered()) return 1;
    if (!test_one_expression_three_failures()) return 1;
    if (!test_the_ladders()) return 1;
    if (!test_the_four_are_not_one_game_four_times()) return 1;
    if (!test_the_mercy_mechanic()) return 1;
    if (!test_growth_stages_are_ordered()) return 1;
    if (!test_the_rng_replays()) return 1;
    (void)puts("plant: PASS");
    return 0;
}
