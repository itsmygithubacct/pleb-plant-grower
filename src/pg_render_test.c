/*
 * pg_render_test — the render fixture matrix.
 *
 * Deliberately NOT part of libpleb-plant-grower.a. kilix-land links that
 * archive into its own process and must not have to supply the kit's test
 * library to do it, so this file is compiled into the standalone binary only,
 * exactly like main.c and pg_term.c. That is also the narrow version of what
 * the M2-M5 review asked for when it noted every harness was being linked into
 * the shipped archive.
 */
#include "pleb_plant_grower.h"

#include "pg_care.h"
#include "pg_content.h"
#include "pg_plant.h"
#include "pg_render.h"
#include "pg_scene.h"
#include "pg_sim.h"
#include "pg_state.h"

#include "kilix_top_down_soft.h"
#include "kilix_top_down_view.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <zlib.h>

/* ==========================================================================
 * --render-test : the fixture matrix of IMPLEMENTATION_PLAN.md §10.5
 * ==========================================================================
 *
 * Every fixture supplies its own wall second AND its own tz offset, so the
 * local hour is pinned by the fixture and never by the machine (D-086). That
 * is what makes the two halves of the time-zone gate meaningful:
 *
 *   8a  the whole suite hashes identically under TZ=UTC and TZ=Asia/Tokyo,
 *       which asserts nothing here reads the process's time-zone state;
 *   8b  two fixtures at the SAME wall second with different offsets must
 *       DIFFER, which asserts the local hour actually reaches the screen.
 *
 * Without 8b, 8a would be satisfied by a renderer that ignored local time
 * entirely -- which is the bug the pair exists to catch.
 */

#include "pg_save.h"

#include "kilix_game_test.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#define PG_RENDER_TEST_WIDTH  PG_LOGICAL_WIDTH
#define PG_RENDER_TEST_HEIGHT PG_LOGICAL_HEIGHT

/* A fixed instant so nothing depends on when the suite runs: 2027-06-15
 * 09:00 UTC, mid growing season in the north. */
#define PG_RENDER_TEST_WALL INT64_C(1813244400)

/* The frozen suite hash.
 *
 * IMPLEMENTATION_PLAN.md §10.5: the goldens are frozen against the procedural
 * look here, and re-frozen exactly twice more -- once at Milestone 7 when the
 * UI lands, once at Milestone 10 when the art does. Any other change to this
 * number is a rendering regression and should be read as one.
 *
 * Set PG_RENDER_GOLDEN_REFREEZE=1 in the environment to print the new value
 * instead of failing. That is the only supported way to move it, and it is
 * deliberately not a flag: re-freezing must be a thing somebody decided to do,
 * not something a test run can do by accident. */
#define PG_RENDER_GOLDEN_SUITE_HASH UINT64_C(0x35efea6e0312688a)

static int pg_render_test_failures;

static void render_check(bool condition, const char *what)
{
    if (!condition) {
        (void)fprintf(stderr, "render-test: %s\n", what);
        pg_render_test_failures += 1;
    }
}

/* Force a plant into one of the six health rows through the axes health is
 * actually derived from -- health is never stored, so a fixture may not simply
 * assign it. */
static void fixture_health(pg_plant *plant, uint8_t health)
{
    plant->life_state = (uint8_t)PG_LIFE_ACTIVE;
    plant->turgor = 9000u;
    plant->root_health = 9000u;
    plant->root_capacity = 10000u;
    plant->light_debt = 0u;
    plant->salt = 0u;
    plant->nutrition = 9000u;
    plant->crisp_dose = 0u;
    plant->scorch_dose = 0u;
    plant->pest_mites = 0u;

    switch (health) {
    case PG_HEALTH_THRIVING: {
        size_t ring;
        const pg_species *species = pg_content_species(plant->species_id);
        uint16_t dli = (species != NULL)
                     ? (uint16_t)(species->dli_thriving_c + 50u) : 900u;
        for (ring = 0u; ring < (size_t)PG_DLI_RING; ++ring) {
            plant->dli_ring[ring] = dli;
        }
        break;
    }
    case PG_HEALTH_HEALTHY: {
        size_t ring;
        for (ring = 0u; ring < (size_t)PG_DLI_RING; ++ring) {
            plant->dli_ring[ring] = 10u;   /* below thriving, above nothing */
        }
        break;
    }
    case PG_HEALTH_THIRSTY:
        plant->turgor = 4000u;
        break;
    case PG_HEALTH_DISTRESSED:
        plant->root_health = 3000u;
        break;
    case PG_HEALTH_CRITICAL:
        plant->turgor = 1000u;
        break;
    case PG_HEALTH_DEAD:
    default:
        plant->life_state = (uint8_t)PG_LIFE_DEAD;
        break;
    }
}

static void fixture_state(pg_state *state, uint64_t seed, uint8_t species,
                          uint8_t stage, uint8_t health, uint8_t pot,
                          int32_t tz_minutes, int64_t wall_s)
{
    pg_plant *plant;
    size_t leaf;

    pg_init(state, seed);
    state->plant_count = 1u;
    state->anchor.last_wall_s = wall_s;
    state->anchor.last_boot_s = 1000;
    state->anchor.tz_offset_minutes = tz_minutes;
    state->anchor.established = true;
    state->first_run_wall_s = wall_s - INT64_C(30) * 24 * 3600;

    plant = &state->plants[0];
    pg_plant_init(plant, species, pot, (uint8_t)PG_SPOT_DEFAULT, wall_s);
    plant->growth_stage = stage;
    plant->leaf_count = (uint8_t)(2u + stage * 2u);
    if (plant->leaf_count > (uint8_t)PG_LEAF_MAX) {
        plant->leaf_count = (uint8_t)PG_LEAF_MAX;
    }
    for (leaf = 0u; leaf < plant->leaf_count; ++leaf) {
        plant->leaves[leaf].birth_care_day = (uint16_t)(leaf * 3u);
        plant->leaves[leaf].slot = (uint8_t)leaf;
        plant->leaves[leaf].form_flags = 0u;
        /* Deterministic damage, so the decal channel is exercised and the
         * permanence record is visible in the goldens. */
        plant->leaves[leaf].damage_mask =
            (uint8_t)((health >= (uint8_t)PG_HEALTH_THIRSTY)
                      ? (1u << (leaf % 6u)) : 0u);
        plant->leaves[leaf].pose = 0u;
    }
    if (species == (uint8_t)PG_SPECIES_POTHOS) {
        size_t node;
        plant->vine_count = (uint8_t)(3u + stage * 3u);
        if (plant->vine_count > (uint8_t)PG_VINE_MAX) {
            plant->vine_count = (uint8_t)PG_VINE_MAX;
        }
        for (node = 0u; node < plant->vine_count; ++node) {
            plant->vine[node].internode = (uint8_t)(20u + node * 4u);
            plant->vine[node].leaf_form = 0u;
            plant->vine[node].damage_mask = 0u;
        }
    }
    if (species == (uint8_t)PG_SPECIES_PEACE_LILY &&
        health <= (uint8_t)PG_HEALTH_HEALTHY) {
        plant->spathe_state = (uint8_t)PG_SPATHE_OPEN;
        plant->flower_count = 1u;
    }
    plant->moisture_top = 5000u;
    plant->moisture_bottom = 5000u;
    fixture_health(plant, health);
}

/* Render one fixture, write it, and return its hash. Asserts the frame is not
 * blank and that rendering did not touch the state it was handed. */
static uint64_t render_fixture(ki_td_soft_renderer *renderer,
                               const pg_state *state,
                               const pg_graphics *graphics,
                               const char *directory, const char *name,
                               kilix_test_golden_suite *suite)
{
    static pg_state before;
    static uint8_t rgba[(size_t)PG_RENDER_TEST_WIDTH *
                        (size_t)PG_RENDER_TEST_HEIGHT * 4u];
    char path[1024];
    pg_render_result result;
    uint64_t hash;
    size_t distinct = 0u;
    size_t index;
    uint32_t seen[64];
    size_t seen_count = 0u;
    int written;

    before = *state;
    ki_td_soft_clear(renderer, UINT32_C(0x000000));
    if (!pg_render(renderer, state, graphics, &result)) {
        render_check(false, "pg_render refused a fixture");
        return 0u;
    }
    render_check(memcmp(&before, state, sizeof before) == 0,
                 "rendering mutated the state it was handed");
    render_check(sr_pack_rgba(ki_td_soft_canvas_const(renderer), rgba,
                              sizeof rgba),
                 "the canvas would not pack");

    /* Not blank: at least 16 distinct colours. A frame that is one flat colour
     * is the failure mode a hash alone will happily certify forever. */
    for (index = 0u; index < sizeof rgba && seen_count < 64u; index += 4u) {
        uint32_t rgb = ((uint32_t)rgba[index] << 16) |
                       ((uint32_t)rgba[index + 1u] << 8) |
                       (uint32_t)rgba[index + 2u];
        size_t probe;
        bool found = false;
        for (probe = 0u; probe < seen_count; ++probe) {
            if (seen[probe] == rgb) { found = true; break; }
        }
        if (!found) { seen[seen_count++] = rgb; distinct += 1u; }
    }
    render_check(distinct >= 16u, "a fixture rendered fewer than 16 colours");

    written = snprintf(path, sizeof path, "%s/%s.ppm", directory, name);
    render_check(written > 0 && (size_t)written < sizeof path,
                 "a fixture path did not fit");
    if (written > 0 && (size_t)written < sizeof path) {
        render_check(kilix_test_write_ppm_rgba(path, rgba,
                                               (size_t)PG_RENDER_TEST_WIDTH,
                                               (size_t)PG_RENDER_TEST_HEIGHT,
                                               (size_t)PG_RENDER_TEST_WIDTH
                                               * 4u),
                     "a fixture PPM would not write");
    }
    hash = kilix_test_hash64(rgba, sizeof rgba);
    if (suite != NULL) {
        (void)kilix_test_golden_add(suite, rgba, sizeof rgba, hash);
    }
    return hash;
}

/* Two small helpers so the fixture atlas can be written without pulling a PNG
 * encoder into the game. zlib is already linked for kilix-assets. */
static bool pg_test_make_dirs(const char *path)
{
    char buffer[1024];
    size_t index;
    size_t length = strlen(path);

    if (length == 0u || length >= sizeof buffer) return false;
    memcpy(buffer, path, length + 1u);
    for (index = 1u; index < length; ++index) {
        if (buffer[index] != '/') continue;
        buffer[index] = '\0';
        if (mkdir(buffer, 0700) != 0 && errno != EEXIST) return false;
        buffer[index] = '/';
    }
    return mkdir(buffer, 0700) == 0 || errno == EEXIST;
}

static void pg_test_put_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static bool pg_test_write_chunk(FILE *file, const char *kind,
                                const uint8_t *payload, size_t size)
{
    uint8_t header[4];
    uint8_t trailer[4];
    uLong crc;

    pg_test_put_be32(header, (uint32_t)size);
    if (fwrite(header, 1u, 4u, file) != 4u) return false;
    if (fwrite(kind, 1u, 4u, file) != 4u) return false;
    if (size != 0u && fwrite(payload, 1u, size, file) != size) return false;
    crc = crc32(0L, (const Bytef *)kind, 4);
    if (size != 0u) crc = crc32(crc, (const Bytef *)payload, (uInt)size);
    pg_test_put_be32(trailer, (uint32_t)crc);
    return fwrite(trailer, 1u, 4u, file) == 4u;
}

static bool pg_test_write_png_rgba(const char *path, const uint8_t *rgba,
                                   int width, int height)
{
    static uint8_t raw[(size_t)640 * 960 * 4 + 960];
    static uint8_t packed[(size_t)640 * 960 * 4 + 4096];
    uint8_t header[13];
    uLongf packed_size = (uLongf)sizeof packed;
    size_t stride = (size_t)width * 4u;
    size_t cursor = 0u;
    FILE *file;
    int row;

    for (row = 0; row < height; ++row) {
        raw[cursor++] = 0u;                    /* filter: none */
        memcpy(raw + cursor, rgba + (size_t)row * stride, stride);
        cursor += stride;
    }
    if (compress2(packed, &packed_size, raw, (uLong)cursor, 6) != Z_OK) {
        return false;
    }
    file = fopen(path, "wbe");
    if (file == NULL) return false;
    if (fwrite("\x89PNG\r\n\x1a\n", 1u, 8u, file) != 8u) goto fail;
    pg_test_put_be32(header, (uint32_t)width);
    pg_test_put_be32(header + 4, (uint32_t)height);
    header[8] = 8u;    /* depth */
    header[9] = 6u;    /* RGBA  */
    header[10] = 0u; header[11] = 0u; header[12] = 0u;
    if (!pg_test_write_chunk(file, "IHDR", header, sizeof header)) goto fail;
    if (!pg_test_write_chunk(file, "IDAT", packed, (size_t)packed_size))
        goto fail;
    if (!pg_test_write_chunk(file, "IEND", NULL, 0u)) goto fail;
    return fclose(file) == 0;
fail:
    (void)fclose(file);
    return false;
}

/* A synthetic plant atlas, written into the caller's directory so the authored
 * render path is covered by a fixture rather than only by the procedural one.
 *
 * This exists because `make test-render` once passed with a real atlas present
 * and proved nothing: the suite ran with no asset root, so it never took the
 * authored branch at all. A fixture that depends on generated art would be
 * worse -- it would go red whenever the art was mid-regeneration -- so the
 * atlas is built here, deterministically, from four flat colours per health
 * row. Flat cells make a wrong cell obvious: if column and row were ever
 * transposed, the fixture's colours would swap and the golden would move.
 */
static bool write_test_atlas(const char *directory, const char *species)
{
    enum { WIDTH = 160 * 4, HEIGHT = 160 * 6 };
    static uint8_t rgba[(size_t)WIDTH * HEIGHT * 4];
    char path[1024];
    int written;
    int column;
    int row;

    for (row = 0; row < 6; ++row) {
        for (column = 0; column < 4; ++column) {
            /* One flat colour per (stage, health), inset by 24 px so the cell
             * has a transparent border and a wrong offset shows as a seam. */
            uint8_t red = (uint8_t)(40 + row * 34);
            uint8_t green = (uint8_t)(200 - row * 28);
            uint8_t blue = (uint8_t)(60 + column * 40);
            int y;
            for (y = 24; y < 160 - 24; ++y) {
                int x;
                for (x = 24; x < 160 - 24; ++x) {
                    size_t offset = ((size_t)(row * 160 + y) * WIDTH
                                     + (size_t)(column * 160 + x)) * 4u;
                    rgba[offset] = red;
                    rgba[offset + 1] = green;
                    rgba[offset + 2] = blue;
                    rgba[offset + 3] = 255u;
                }
            }
        }
    }
    written = snprintf(path, sizeof path, "%s/graphics/atlases", directory);
    if (written <= 0 || (size_t)written >= sizeof path) return false;
    if (!pg_test_make_dirs(path)) return false;
    written = snprintf(path, sizeof path, "%s/graphics/atlases/%s.png",
                       directory, species);
    if (written <= 0 || (size_t)written >= sizeof path) return false;
    return pg_test_write_png_rgba(path, rgba, WIDTH, HEIGHT);
}

int pg_render_run_test(uint64_t seed, const char *directory)
{
    static ki_td_soft_renderer renderer;
    static pg_state state;
    static pg_graphics graphics;
    kilix_test_golden_suite suite;
    uint64_t health_hash[PG_HEALTH_COUNT];
    uint64_t species_hash[PG_SPECIES_COUNT];
    uint64_t pot_hash[PG_POT_COUNT];
    uint64_t background_hash[PG_SCENE_COUNT + 1u];
    size_t fixtures = 0u;
    uint8_t species, stage, health, pot;
    size_t index;
    char name[128];

    pg_render_test_failures = 0;
    if (directory == NULL || directory[0] != '/') {
        (void)fputs("render-test: <dir> must be an absolute path\n", stderr);
        return 2;
    }
    if (!ki_td_soft_renderer_init(&renderer, PG_RENDER_TEST_WIDTH,
                                  PG_RENDER_TEST_HEIGHT)) {
        (void)fputs("render-test: the renderer would not initialise\n",
                    stderr);
        return 1;
    }
    kilix_test_golden_suite_init(&suite);
    /* Graphics with no asset root: every plate is absent, which is the
     * background-off path and is exactly what this milestone freezes. */
    (void)pg_graphics_init(&graphics, NULL, NULL, 0u);

    (void)printf("render-test %s\n", pg_version());

    /* 4 species x 4 growth stages x 6 health states. */
    for (species = 0u; species < (uint8_t)PG_SPECIES_COUNT; ++species) {
        for (stage = 0u; stage < (uint8_t)PG_STAGE_COUNT; ++stage) {
            for (health = 0u; health < (uint8_t)PG_HEALTH_COUNT; ++health) {
                uint64_t hash;
                fixture_state(&state, seed, species, stage, health,
                              (uint8_t)PG_POT_TERRACOTTA, 0,
                              PG_RENDER_TEST_WALL);
                (void)snprintf(name, sizeof name, "plant-%u-%u-%u",
                               (unsigned)species, (unsigned)stage,
                               (unsigned)health);
                hash = render_fixture(&renderer, &state, &graphics, directory,
                                      name, &suite);
                fixtures += 1u;
                if (species == 0u && stage == 2u) health_hash[health] = hash;
                if (stage == 2u && health == (uint8_t)PG_HEALTH_HEALTHY) {
                    species_hash[species] = hash;
                }
            }
        }
    }

    /* The four pots, same plant. */
    for (pot = 0u; pot < (uint8_t)PG_POT_COUNT; ++pot) {
        fixture_state(&state, seed, (uint8_t)PG_SPECIES_POTHOS, 2u,
                      (uint8_t)PG_HEALTH_HEALTHY, pot, 0,
                      PG_RENDER_TEST_WALL);
        (void)snprintf(name, sizeof name, "pot-%u", (unsigned)pot);
        pot_hash[pot] = render_fixture(&renderer, &state, &graphics,
                                       directory, name, &suite);
        fixtures += 1u;
    }

    /* Six scenes plus background-off. With no asset root every scene selects
     * but no plate loads, so all seven render the procedural stage: this
     * milestone freezes that look, and Milestone 10 re-freezes it once art
     * exists. The fixtures exist now so the selection path is covered. */
    fixture_state(&state, seed, (uint8_t)PG_SPECIES_CALATHEA, 2u,
                  (uint8_t)PG_HEALTH_HEALTHY, (uint8_t)PG_POT_GLAZED, 0,
                  PG_RENDER_TEST_WALL);
    for (index = 0u; index <= (size_t)PG_SCENE_COUNT; ++index) {
        const char *scene_id = (index == (size_t)PG_SCENE_COUNT)
                             ? NULL
                             : pg_graphics_scene_id(&graphics, index);
        (void)pg_graphics_set_scene(&graphics, scene_id);
        (void)snprintf(name, sizeof name, "background-%s",
                       scene_id != NULL ? scene_id : "off");
        background_hash[index] = render_fixture(&renderer, &state, &graphics,
                                                directory, name, &suite);
        fixtures += 1u;
    }
    (void)pg_graphics_set_scene(&graphics, NULL);

    /* The screens. The plan asks for calendar and chooser fixtures by name;
     * the journal and the two modal screens come along because they are the
     * same layout code and a screen nobody renders is a screen nobody
     * notices breaking. */
    {
        static const uint8_t SCREENS[] = {
            (uint8_t)PG_SCREEN_CALENDAR, (uint8_t)PG_SCREEN_CHOOSER,
            (uint8_t)PG_SCREEN_JOURNAL,  (uint8_t)PG_SCREEN_AWAY,
            (uint8_t)PG_SCREEN_DAMAGED,  (uint8_t)PG_SCREEN_REPLANT
        };
        static const char *const SCREEN_NAMES[] = {
            "calendar", "chooser", "journal", "away", "damaged", "replant"
        };
        uint64_t screen_hash[sizeof SCREENS / sizeof SCREENS[0]];
        size_t screen;

        for (screen = 0u; screen < sizeof SCREENS / sizeof SCREENS[0];
             ++screen) {
            fixture_state(&state, seed, (uint8_t)PG_SPECIES_PEACE_LILY, 2u,
                          (uint8_t)PG_HEALTH_HEALTHY,
                          (uint8_t)PG_POT_TERRACOTTA, 0,
                          PG_RENDER_TEST_WALL);
            /* A few journal entries so the calendar has marks and the journal
             * has rows: an empty screen renders, and proves nothing. */
            pg_journal_push(&state, 3u, (uint8_t)PG_JOURNAL_WATERED, 9u, 0u);
            pg_journal_push(&state, 5u, (uint8_t)PG_JOURNAL_FED, 11u, 0u);
            pg_journal_push(&state, 9u, (uint8_t)PG_JOURNAL_NEW_LEAF, 14u, 0u);
            pg_journal_push(&state, 12u, (uint8_t)PG_JOURNAL_THIRSTY, 8u, 0u);
            state.ui.screen = SCREENS[screen];
            if (SCREENS[screen] == (uint8_t)PG_SCREEN_CHOOSER) {
                state.ui.chooser_step = (uint8_t)PG_CHOOSER_SPECIES;
            }
            (void)snprintf(name, sizeof name, "screen-%s",
                           SCREEN_NAMES[screen]);
            screen_hash[screen] = render_fixture(&renderer, &state, &graphics,
                                                 directory, name, &suite);
            fixtures += 1u;
        }
        /* The journal and the away report are deliberately the SAME screen:
         * the away report is the journal filtered to the last absence, and
         * giving it its own layout would mean two layouts to keep in
         * agreement. Assert that on purpose. Every other pair must differ. */
        render_check(screen_hash[2] == screen_hash[3],
                     "journal and away are one layout and must match");
        for (screen = 1u; screen < sizeof SCREENS / sizeof SCREENS[0];
             ++screen) {
            if (screen == 3u) continue;    /* away, matched above */
            render_check(screen_hash[screen] != screen_hash[screen - 1u],
                         "two distinct screens rendered identically");
        }
    }

    /* The authored path. Until this fixture existed the suite ran with no
     * asset root and never took it, so an atlas could be loaded, sliced and
     * drawn entirely wrong while every render fixture stayed green. */
    {
        static pg_graphics art;
        uint64_t authored;
        uint64_t procedural;

        if (write_test_atlas(directory, "pothos")) {
            fixture_state(&state, seed, (uint8_t)PG_SPECIES_POTHOS, 2u,
                          (uint8_t)PG_HEALTH_HEALTHY,
                          (uint8_t)PG_POT_TERRACOTTA, 0,
                          PG_RENDER_TEST_WALL);
            procedural = render_fixture(&renderer, &state, &graphics,
                                        directory, "atlas-absent", &suite);
            fixtures += 1u;

            (void)pg_graphics_init(&art, directory, NULL, 0u);
            render_check(art.plant_atlas_valid[PG_SPECIES_POTHOS],
                         "the fixture atlas did not load");
            authored = render_fixture(&renderer, &state, &art, directory,
                                      "atlas-present", &suite);
            fixtures += 1u;
            render_check(authored != procedural,
                         "an atlas was loaded and changed nothing -- the "
                         "authored path is not being taken");

            /* Column is growth stage, row is health. The checks below prove
             * both axes are READ -- that changing either picks a different
             * cell -- which is what catches an axis being ignored entirely.
             *
             * They do NOT catch a transpose: swapping the arguments still
             * yields different cells for different inputs, so all three still
             * hold. The frozen golden is what catches that, and it does --
             * verified by swapping the arguments and watching the suite hash
             * move. Saying so here because the first version of this comment
             * claimed these assertions caught it, and they do not. */
            {
                uint64_t other_stage;
                uint64_t other_health;
                fixture_state(&state, seed, (uint8_t)PG_SPECIES_POTHOS, 0u,
                              (uint8_t)PG_HEALTH_HEALTHY,
                              (uint8_t)PG_POT_TERRACOTTA, 0,
                              PG_RENDER_TEST_WALL);
                other_stage = render_fixture(&renderer, &state, &art,
                                             directory, "atlas-stage0",
                                             &suite);
                fixtures += 1u;
                fixture_state(&state, seed, (uint8_t)PG_SPECIES_POTHOS, 2u,
                              (uint8_t)PG_HEALTH_CRITICAL,
                              (uint8_t)PG_POT_TERRACOTTA, 0,
                              PG_RENDER_TEST_WALL);
                other_health = render_fixture(&renderer, &state, &art,
                                              directory, "atlas-health4",
                                              &suite);
                fixtures += 1u;
                render_check(other_stage != authored,
                             "two growth stages picked the same atlas cell");
                render_check(other_health != authored,
                             "two health states picked the same atlas cell");
                render_check(other_stage != other_health,
                             "growth stage and health are indexing the same "
                             "axis -- column and row are transposed");
            }
            pg_graphics_shutdown(&art);
        } else {
            render_check(false, "the fixture atlas could not be written");
        }
    }

    /* Gate 8b: the same instant, two offsets, a calathea either side of dusk.
     * These MUST differ or the renderer is ignoring local time. */
    {
        uint64_t utc_hash, tokyo_hash;
        fixture_state(&state, seed, (uint8_t)PG_SPECIES_CALATHEA, 2u,
                      (uint8_t)PG_HEALTH_HEALTHY,
                      (uint8_t)PG_POT_TERRACOTTA, 0, PG_RENDER_TEST_WALL);
        utc_hash = render_fixture(&renderer, &state, &graphics, directory,
                                  "localhour-utc", &suite);
        fixtures += 1u;
        fixture_state(&state, seed, (uint8_t)PG_SPECIES_CALATHEA, 2u,
                      (uint8_t)PG_HEALTH_HEALTHY,
                      (uint8_t)PG_POT_TERRACOTTA, 540, PG_RENDER_TEST_WALL);
        tokyo_hash = render_fixture(&renderer, &state, &graphics, directory,
                                    "localhour-plus540", &suite);
        fixtures += 1u;
        render_check(utc_hash != tokyo_hash,
                     "gate 8b: local hour does not reach the screen -- the "
                     "same instant at two offsets rendered identically");
    }

    /* Distinctness. Each of these is a way for the renderer to look right and
     * be wrong. */
    for (index = 1u; index < (size_t)PG_HEALTH_COUNT; ++index) {
        render_check(health_hash[index] != health_hash[index - 1u],
                     "two adjacent health states rendered identically");
    }
    for (index = 1u; index < (size_t)PG_SPECIES_COUNT; ++index) {
        render_check(species_hash[index] != species_hash[index - 1u],
                     "two species rendered identically");
    }
    for (index = 1u; index < (size_t)PG_POT_COUNT; ++index) {
        render_check(pot_hash[index] != pot_hash[index - 1u],
                     "two pots rendered identically");
    }
    /* No plate exists before Milestone 10, so the BACKDROP is the procedural
     * stage in all seven fixtures -- "missing is not an error". What still
     * differs is the plant, because light_from is applied whether or not a
     * plate loaded: a scene says which way its light comes from, and the plant
     * is lit to match.
     *
     * So the assertion is not "all seven are identical" (they are not, and a
     * first draft of this test wrongly said so) but the exact contract: two
     * scenes agree if and only if they light the plant from the same side.
     * That covers both halves at once -- the absent plate changed nothing, and
     * the scene selection reached the screen. background-off is index
     * PG_SCENE_COUNT and lights from the front. */
    for (index = 0u; index <= (size_t)PG_SCENE_COUNT; ++index) {
        size_t other;
        uint8_t light_a = (index == (size_t)PG_SCENE_COUNT)
                        ? (uint8_t)PG_LIGHT_FROM_FRONT
                        : graphics.scenes[index].light_from;
        uint8_t panel_a = (index == (size_t)PG_SCENE_COUNT)
                        ? (uint8_t)PG_PANEL_RIGHT
                        : graphics.scenes[index].panel_side;
        for (other = index + 1u; other <= (size_t)PG_SCENE_COUNT; ++other) {
            uint8_t light_b = (other == (size_t)PG_SCENE_COUNT)
                            ? (uint8_t)PG_LIGHT_FROM_FRONT
                            : graphics.scenes[other].light_from;
            uint8_t panel_b = (other == (size_t)PG_SCENE_COUNT)
                            ? (uint8_t)PG_PANEL_RIGHT
                            : graphics.scenes[other].panel_side;
            /* Since the HUD landed a scene decides TWO visible things with no
             * plate loaded: which side lights the plant, and which side the
             * panel sits on. Both must match for two scenes to agree. */
            if (light_a == light_b && panel_a == panel_b) {
                render_check(background_hash[index] == background_hash[other],
                             "two scenes with the same light and panel side, "
                             "with no plate loaded, rendered differently");
            } else {
                render_check(background_hash[index] != background_hash[other],
                             "two scenes differing in light or panel side "
                             "rendered identically -- the scene is not "
                             "reaching the screen");
            }
        }
    }

    render_check(fixtures == (size_t)(PG_SPECIES_COUNT * PG_STAGE_COUNT *
                                      PG_HEALTH_COUNT) +
                             (size_t)PG_POT_COUNT +
                             (size_t)PG_SCENE_COUNT + 1u + 6u + 4u + 2u,
                 "the fixture count is not what §10.5 specifies");

    /* The snapping rule, asserted where the arithmetic lives. */
    {
        ki_td_view view;
        render_check(pg_render_fit_view(&view, 1920, 1080) &&
                     view.scale == 4.0f, "1920x1080 must snap to scale 4");
        render_check(pg_render_fit_view(&view, 1440, 810) &&
                     view.scale == 3.0f, "1440x810 must snap to scale 3");
        render_check(pg_render_fit_view(&view, 960, 540) &&
                     view.scale == 2.0f, "960x540 must snap to scale 2");
        /* The whole point of snapping: an odd framebuffer still gets an exact
         * integer scale rather than a letterboxed fractional one. */
        render_check(pg_render_fit_view(&view, 1000, 600) &&
                     view.scale == 2.0f,
                     "an odd framebuffer must still snap to an integer scale");
        render_check(!pg_render_fit_view(&view, 0, 0),
                     "a zero framebuffer must be refused");
    }

    (void)printf("  fixtures           %zu\n", fixtures);
    (void)printf("  suite hash         %016" PRIx64 "\n", suite.suite_hash);
    {
        const char *refreeze = getenv("PG_RENDER_GOLDEN_REFREEZE");
        if (refreeze != NULL && refreeze[0] == '1') {
            (void)printf("  REFREEZE: set PG_RENDER_GOLDEN_SUITE_HASH to "
                         "UINT64_C(0x%016" PRIx64 ")\n", suite.suite_hash);
        } else if (PG_RENDER_GOLDEN_SUITE_HASH != UINT64_C(0)) {
            render_check(kilix_test_golden_finish(
                             &suite, PG_RENDER_GOLDEN_SUITE_HASH),
                         "the frozen render goldens moved -- if that was "
                         "intended, re-freeze with "
                         "PG_RENDER_GOLDEN_REFREEZE=1");
        }
    }

    pg_graphics_shutdown(&graphics);
    ki_td_soft_renderer_destroy(&renderer);

    if (pg_render_test_failures != 0) {
        (void)fprintf(stderr, "render-test: FAILED after %d failures\n",
                      pg_render_test_failures);
        return 1;
    }
    (void)puts("render-test: PASS");
    return 0;
}
