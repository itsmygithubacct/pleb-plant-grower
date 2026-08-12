/*
 * pg_graphics — backgrounds on disk.
 *
 * ARCHITECTURE.md §4.1. Three rules decide everything here, and all three come
 * from kilix-land-desktop's graphics.c, which learned them the hard way:
 *
 *   - **Wrong size is refused, never scaled.** A plate that is not exactly
 *     1920x1080 is rejected, so a bad cook is visible as a missing background
 *     rather than as a subtly blurry one nobody reports.
 *   - **Missing is not an error.** A plate that will not resolve takes the
 *     identical path as `--background none`: the procedural stage draws and the
 *     game runs. A houseplant must never fail to open because a picture moved.
 *   - **Disabled is a designed look.** background-off is §4.3's procedural
 *     stage, not a degradation and not `plain-studio`.
 *
 * This file and pg_store.c are the only two that touch the filesystem
 * (§2.2 rule 6). Nothing here reads a clock and nothing here allocates on a
 * render path -- loads happen at init and at an explicit scene swap.
 */
#include "pleb_plant_grower.h"

#include "pg_scene.h"
#include "pg_state.h"

#include "kilix_assets.h"

#include <stdio.h>
#include <string.h>

/* Exactly, or not at all. */
#define PG_PLATE_WIDTH  1920u
#define PG_PLATE_HEIGHT 1080u

/* The six scenes of ART_BIBLE.md §4.5, in stable order. Authored descriptors
 * live in assets/backgrounds/scenes.json for the art tools; the renderer keeps
 * its own copy because it may not read a file to draw a frame. */
typedef struct pg_scene_seed {
    const char *id;
    const char *name;
    uint8_t panel_side;
    uint8_t light_from;
    uint32_t ink;
    uint32_t ink_shadow;
    bool front_overlay;
} pg_scene_seed;

static const pg_scene_seed PG_SCENE_SEEDS[PG_SCENE_COUNT] = {
    {"sunny-sill",    "Sunny Sill",    PG_PANEL_RIGHT, PG_LIGHT_FROM_LEFT,
     UINT32_C(0xf6efe0), UINT32_C(0x141109), true},
    {"bright-corner", "Bright Corner", PG_PANEL_RIGHT, PG_LIGHT_FROM_LEFT,
     UINT32_C(0xf2e9d8), UINT32_C(0x12100e), false},
    {"study-desk",    "Study Desk",    PG_PANEL_LEFT,  PG_LIGHT_FROM_RIGHT,
     UINT32_C(0xece3d2), UINT32_C(0x100e0c), true},
    {"plain-studio",  "Plain Studio",  PG_PANEL_RIGHT, PG_LIGHT_FROM_FRONT,
     UINT32_C(0xf2e9d8), UINT32_C(0x12100e), false},
    {"kitchen-shelf", "Kitchen Shelf", PG_PANEL_RIGHT, PG_LIGHT_FROM_RIGHT,
     UINT32_C(0xf4ecdc), UINT32_C(0x11100d), false},
    {"steamy-bath",   "Steamy Bath",   PG_PANEL_LEFT,  PG_LIGHT_FROM_FRONT,
     UINT32_C(0xeef0ea), UINT32_C(0x0e1211), true}
};

static void copy_bounded(char *destination, size_t capacity,
                         const char *source)
{
    size_t length;

    if (destination == NULL || capacity == 0u) {
        return;
    }
    destination[0] = '\0';
    if (source == NULL) {
        return;
    }
    length = strlen(source);
    if (length >= capacity) {
        length = capacity - 1u;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

/* `<id>/<id>.png`, and `<id>/<id>-front.png` for the occluder. */
static bool scene_relative_path(char *out, size_t capacity, const char *id,
                                bool front)
{
    int written = snprintf(out, capacity, "backgrounds/%s/%s%s.png", id, id,
                           front ? "-front" : "");
    return written > 0 && (size_t)written < capacity;
}

static bool load_plate(pg_graphics *graphics, const char *id, bool front,
                       kilix_asset_image *image)
{
    char relative[256];
    char resolved[512];

    kilix_asset_image_clear(image);
    if (!scene_relative_path(relative, sizeof relative, id, front) ||
        !kilix_asset_path_is_safe(relative)) {
        return false;
    }
    if (kilix_asset_resolve(&graphics->locator, relative, resolved,
                            sizeof resolved) != KILIX_ASSET_OK) {
        return false;              /* missing is not an error */
    }
    if (kilix_asset_image_load_png(image, resolved, &graphics->limits)
        != KILIX_ASSET_OK) {
        return false;
    }
    /* Refused, never scaled. */
    if (image->width != PG_PLATE_WIDTH || image->height != PG_PLATE_HEIGHT) {
        kilix_asset_image_clear(image);
        return false;
    }
    return true;
}

static void release_plates(pg_graphics *graphics)
{
    kilix_asset_image_clear(&graphics->plate);
    kilix_asset_image_clear(&graphics->front);
    graphics->plate_valid = false;
    graphics->front_valid = false;
}

static uint8_t scene_index_for(const pg_graphics *graphics, const char *id)
{
    uint8_t index;

    if (id == NULL) {
        return PG_SCENE_NONE;
    }
    for (index = 0u; index < graphics->scene_count; ++index) {
        if (strcmp(graphics->scenes[index].id, id) == 0) {
            return index;
        }
    }
    return PG_SCENE_NONE;
}

/* The atlas file names are the species ids of content/plants.json, which is
 * also what ART_BIBLE names them. One lookup table rather than a switch, so
 * adding a fifth species is a content change and not a code change. */
static const char *const PG_ATLAS_FILES[PG_SPECIES_COUNT] = {
    "pothos", "snake-plant", "peace-lily", "calathea"
};

/* file, columns, rows, cell width, cell height. Exact size or nothing, the
 * same contract the species atlases hold: a mis-sized sheet slices at the
 * wrong offsets, and half a leaf is worse than no leaf. */
static const struct pg_overlay_spec {
    const char *file;
    unsigned columns;
    unsigned rows;
    unsigned cell_w;
    unsigned cell_h;
} PG_OVERLAY_SPECS[PG_OVERLAY_COUNT] = {
    { "spathe",         4u, 2u,  64u,  64u },
    { "vines",          8u, 2u,  48u,  48u },
    { "calathea-night", 4u, 1u, 160u, 160u }
};

static void load_overlay_atlases(pg_graphics *graphics)
{
    size_t index;

    for (index = 0u; index < (size_t)PG_OVERLAY_COUNT; ++index) {
        const struct pg_overlay_spec *spec = &PG_OVERLAY_SPECS[index];
        char relative[128];
        char resolved[512];
        kilix_asset_image *image = &graphics->overlay_atlas[index];
        int written = snprintf(relative, sizeof relative,
                               "graphics/atlases/%s.png", spec->file);

        graphics->overlay_valid[index] = false;
        kilix_asset_image_clear(image);
        if (written <= 0 || (size_t)written >= sizeof relative ||
            !kilix_asset_path_is_safe(relative)) {
            continue;
        }
        if (kilix_asset_resolve(&graphics->locator, relative, resolved,
                                sizeof resolved) != KILIX_ASSET_OK) {
            continue;
        }
        if (kilix_asset_image_load_png(image, resolved, &graphics->limits)
            != KILIX_ASSET_OK) {
            continue;
        }
        if (image->width != spec->columns * spec->cell_w ||
            image->height != spec->rows * spec->cell_h ||
            !kilix_asset_atlas_init_grid(&graphics->overlay_grid[index],
                                         image, spec->columns, spec->rows)) {
            kilix_asset_image_clear(image);
            continue;
        }
        graphics->overlay_valid[index] = true;
    }
}

#define PG_ATLAS_COLUMNS 4u
#define PG_ATLAS_ROWS 6u
#define PG_ATLAS_CELL 160u

/* Load whatever plant atlases exist. A missing one leaves that species on the
 * procedural path and is not reported as a failure: the game must open with
 * no art at all, which is the whole reason the procedural plant exists. */
static void load_plant_atlases(pg_graphics *graphics)
{
    size_t index;

    graphics->plant_atlas_count = 0u;
    for (index = 0u; index < (size_t)PG_SPECIES_COUNT; ++index) {
        char relative[128];
        char resolved[512];
        kilix_asset_image *image = &graphics->plant_atlas[index];
        int written = snprintf(relative, sizeof relative,
                               "graphics/atlases/%s.png",
                               PG_ATLAS_FILES[index]);

        graphics->plant_atlas_valid[index] = false;
        kilix_asset_image_clear(image);
        if (written <= 0 || (size_t)written >= sizeof relative ||
            !kilix_asset_path_is_safe(relative)) {
            continue;
        }
        if (kilix_asset_resolve(&graphics->locator, relative, resolved,
                                sizeof resolved) != KILIX_ASSET_OK) {
            continue;
        }
        if (kilix_asset_image_load_png(image, resolved, &graphics->limits)
            != KILIX_ASSET_OK) {
            continue;
        }
        /* Exact size or nothing, like the backdrop plates: a mis-sized atlas
         * would slice cells at the wrong offsets and produce sprites made of
         * two half-plants, which is far worse than no sprite. */
        if (image->width != PG_ATLAS_COLUMNS * PG_ATLAS_CELL ||
            image->height != PG_ATLAS_ROWS * PG_ATLAS_CELL ||
            !kilix_asset_atlas_init_grid(&graphics->plant_grid[index], image,
                                         PG_ATLAS_COLUMNS, PG_ATLAS_ROWS)) {
            kilix_asset_image_clear(image);
            continue;
        }
        graphics->plant_atlas_valid[index] = true;
        graphics->plant_atlas_count += 1u;
    }
}

bool pg_graphics_init(pg_graphics *graphics, const char *asset_root,
                      char *error, size_t error_size)
{
    size_t index;

    if (error != NULL && error_size > 0u) {
        error[0] = '\0';
    }
    if (graphics == NULL) {
        if (error != NULL && error_size > 0u) {
            copy_bounded(error, error_size, "no graphics record");
        }
        return false;
    }
    memset(graphics, 0, sizeof *graphics);
    kilix_asset_limits_init(&graphics->limits);
    kilix_asset_locator_init(&graphics->locator);
    copy_bounded(graphics->asset_root, sizeof graphics->asset_root,
                 asset_root);
    if (graphics->asset_root[0] != '\0') {
        graphics->locator.source_root = graphics->asset_root;
    }
    graphics->locator.environment_variable = "PLEB_PLANT_ASSET_ROOT";

    graphics->scene_count = (uint8_t)PG_SCENE_COUNT;
    for (index = 0u; index < (size_t)PG_SCENE_COUNT; ++index) {
        pg_scene_desc *scene = &graphics->scenes[index];
        const pg_scene_seed *seed = &PG_SCENE_SEEDS[index];

        copy_bounded(scene->id, sizeof scene->id, seed->id);
        copy_bounded(scene->name, sizeof scene->name, seed->name);
        scene->nudge_x = 0;
        scene->nudge_y = 0;
        scene->panel_side = seed->panel_side;
        scene->light_from = seed->light_from;
        scene->ink = seed->ink;
        scene->ink_shadow = seed->ink_shadow;
        scene->front_overlay = seed->front_overlay;
        scene->plate_loaded = false;
    }

    /* Probe every scene once so --list-backgrounds and the HUD can say which
     * are actually present, then leave the backdrop off. Probing is a resolve,
     * not a decode: six 1920x1080 decodes at startup would be wasteful and
     * only one can be shown at a time. */
    graphics->loaded_count = 0u;
    for (index = 0u; index < (size_t)graphics->scene_count; ++index) {
        char relative[256];
        char resolved[512];

        if (scene_relative_path(relative, sizeof relative,
                                graphics->scenes[index].id, false) &&
            kilix_asset_path_is_safe(relative) &&
            kilix_asset_resolve(&graphics->locator, relative, resolved,
                                sizeof resolved) == KILIX_ASSET_OK) {
            graphics->scenes[index].plate_loaded = true;
            graphics->loaded_count += 1u;
        }
    }

    load_plant_atlases(graphics);
    load_overlay_atlases(graphics);

    graphics->scene = PG_SCENE_NONE;
    graphics->full_frame_pending = true;
    return true;
}

void pg_graphics_shutdown(pg_graphics *graphics)
{
    if (graphics == NULL) {
        return;
    }
    {
        size_t index;
        for (index = 0u; index < (size_t)PG_SPECIES_COUNT; ++index) {
            kilix_asset_image_clear(&graphics->plant_atlas[index]);
            graphics->plant_atlas_valid[index] = false;
        }
        graphics->plant_atlas_count = 0u;
    }
    release_plates(graphics);
    graphics->scene = PG_SCENE_NONE;
    graphics->scene_count = 0u;
    graphics->loaded_count = 0u;
}

size_t pg_graphics_loaded_count(const pg_graphics *graphics)
{
    return (graphics == NULL) ? 0u : (size_t)graphics->loaded_count;
}

size_t pg_graphics_scene_count(const pg_graphics *graphics)
{
    return (graphics == NULL) ? 0u : (size_t)graphics->scene_count;
}

const char *pg_graphics_scene_id(const pg_graphics *graphics, size_t index)
{
    if (graphics == NULL || index >= (size_t)graphics->scene_count) {
        return NULL;
    }
    return graphics->scenes[index].id;
}

const char *pg_graphics_scene(const pg_graphics *graphics)
{
    if (graphics == NULL || graphics->scene >= graphics->scene_count) {
        return NULL;               /* background-off */
    }
    return graphics->scenes[graphics->scene].id;
}

bool pg_graphics_set_scene(pg_graphics *graphics, const char *scene_id)
{
    uint8_t index;

    if (graphics == NULL) {
        return false;
    }
    if (scene_id == NULL) {        /* deliberate: the procedural stage */
        release_plates(graphics);
        graphics->scene = PG_SCENE_NONE;
        graphics->full_frame_pending = true;
        return true;
    }
    index = scene_index_for(graphics, scene_id);
    if (index == PG_SCENE_NONE) {
        return false;              /* an id we do not have is a caller error */
    }

    release_plates(graphics);
    graphics->plate_valid = load_plate(graphics, graphics->scenes[index].id,
                                       false, &graphics->plate);
    if (graphics->plate_valid && graphics->scenes[index].front_overlay) {
        graphics->front_valid = load_plate(graphics,
                                           graphics->scenes[index].id, true,
                                           &graphics->front);
    }
    graphics->scenes[index].plate_loaded = graphics->plate_valid;

    /* A scene whose plate will not load is not a failure: it selects, and the
     * procedural stage draws underneath. Saying otherwise would make a moved
     * file into a crash. */
    graphics->scene = index;
    graphics->full_frame_pending = true;
    return true;
}
