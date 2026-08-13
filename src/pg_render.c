/*
 * pg_render — one frame.
 *
 * ARCHITECTURE.md §3.2's layer stack, in order:
 *
 *   0  backdrop plate         ki_td_soft_rgba_backdrop, or the procedural stage
 *   1  static room dressing
 *   2  plant body             tinted; the health ramp modulates RGB
 *   3  decals at leaf anchors damage / new-growth / pest overlays
 *   4  pot                    IN FRONT of the stems (D-051)
 *   5  soil-line dressing     crust, gnats, saucer
 *   6  overlays               droplets, mist, feed sparkle, eyes
 *   7  foreground occluder    <scene>-front.png
 *   8  HUD                    kilix-ui            (Milestone 7)
 *   9  calendar / journal     kilix-ui            (Milestone 7)
 *
 * **The pot is drawn in front of the plant, and there is no soil layer between
 * them.** The soil surface is therefore never visible, which is exactly what
 * removes any need for a pot to have separate front and back pieces -- and
 * that is what makes 4 plants x 4 pots sixteen free combinations rather than
 * sixteen authored ones (D-051).
 *
 * Until Milestone 10 there are no atlases, so layer 2 is drawn procedurally.
 * That is deliberate and it is what lets the build stay playable while art is
 * outstanding: the four health channels of §3.3 are all exercised here --
 * silhouette by growth stage, the colour ramp, decals at anchors, and posture
 * -- so when authored art replaces the shapes, the wiring is already proven.
 *
 * Hard rules this file obeys, all checked by tests:
 *   - never allocates
 *   - never mutates the state it is handed
 *   - intersects and RESTORES the caller's clip
 *   - reads no clock: the local hour arrives through pg_sim_env_now
 */
#include "pleb_plant_grower.h"

#include "pg_care.h"
#include "pg_damage.h"
#include "pg_plant.h"
#include "pg_render.h"
#include "pg_scene.h"
#include "pg_sim.h"
#include "pg_state.h"
#include "pg_ui.h"

#include "kilix_top_down_soft.h"
#include "kilix_top_down_view.h"

#include <math.h>
#include <string.h>

/* ---- framebuffer snapping (§10.2) ---------------------------------------
 *
 * ki_td_view_fit under KI_TD_SCALE_PIXEL_ART floors the candidate scale, so
 * any framebuffer that is not an exact multiple of 480x270 letterboxes. We
 * snap instead: pick the largest integer k in 2..4 that fits, which gives
 * 960x540, 1440x810 or 1920x1080 exactly. Below 960x540 fall back to a
 * fractional fit rather than refusing to draw. */
bool pg_render_fit_view(ki_td_view *view, int width, int height)
{
    ki_td_fit_spec spec;
    int scale;

    if (view == NULL || width <= 0 || height <= 0) {
        return false;
    }
    for (scale = 4; scale >= 2; --scale) {
        if (PG_LOGICAL_WIDTH * scale <= width &&
            PG_LOGICAL_HEIGHT * scale <= height) {
            memset(view, 0, sizeof *view);
            view->logical_width = PG_LOGICAL_WIDTH;
            view->logical_height = PG_LOGICAL_HEIGHT;
            view->scale = (float)scale;
            /* Centred, and on an exact pixel so the fast paths stay fast. */
            view->origin_x = (width - PG_LOGICAL_WIDTH * scale) / 2;
            view->origin_y = (height - PG_LOGICAL_HEIGHT * scale) / 2;
            return true;
        }
    }
    if (!ki_td_fit_spec_init(&spec, PG_LOGICAL_WIDTH, PG_LOGICAL_HEIGHT,
                             width, height)) {
        return false;
    }
    spec.scale_policy = KI_TD_SCALE_PIXEL_ART;
    spec.integer_scale_threshold = 2.0f;
    spec.minimum_scale = 0.25f;
    return ki_td_view_fit(view, &spec);
}

/* ---- the health ramp (channel 2) ----------------------------------------
 * The fine channel. It interpolates BETWEEN the six authored rows and never
 * replaces one: sprite selection is still the coarse, silhouette-level
 * channel. */
static uint32_t health_tint(uint8_t health)
{
    static const uint32_t RAMP[PG_HEALTH_COUNT] = {
        UINT32_C(0x6fbf5a),   /* thriving  */
        UINT32_C(0x5fa84e),   /* healthy   */
        UINT32_C(0x8fa64a),   /* thirsty   */
        UINT32_C(0xa8933f),   /* distressed*/
        UINT32_C(0x8a6a33),   /* critical  */
        UINT32_C(0x6b5636)    /* dead      */
    };
    return RAMP[health < PG_HEALTH_COUNT ? health : PG_HEALTH_COUNT - 1u];
}

static uint32_t pot_body_colour(uint8_t pot_id)
{
    switch (pot_id) {
    case PG_POT_TERRACOTTA: return UINT32_C(0xb4693f);
    case PG_POT_GLAZED:     return UINT32_C(0x3f6f8f);
    case PG_POT_NURSERY:    return UINT32_C(0x4a4a4a);
    case PG_POT_CACHEPOT:   return UINT32_C(0x8a7d63);
    default:                return UINT32_C(0x8a7d63);
    }
}

/* ---- layer 2: the plant body -------------------------------------------- */

/* Posture (channel 4): turgor drives a droop offset. A limp plant literally
 * hangs, which is the readout three different failures share. */
static float droop_offset(const pg_plant *plant)
{
    float turgor = (float)plant->turgor / (float)PG_LEVEL_MAX;
    if (turgor > 1.0f) turgor = 1.0f;
    if (turgor < 0.0f) turgor = 0.0f;
    return (1.0f - turgor) * 7.0f;
}

static void draw_stem(ki_td_soft_renderer *renderer, const ki_td_view *view,
                      float base_x, float base_y, float height,
                      uint32_t rgb, float lean)
{
    int step;
    int steps = (int)height;

    if (steps < 1) steps = 1;
    for (step = 0; step < steps; ++step) {
        float t = (float)step / (float)steps;
        float x = base_x + lean * t * t;
        float y = base_y - (float)step;
        ki_td_soft_fill_rect(renderer, view, x, y, 2.0f, 1.0f, rgb, 1.0f);
    }
}

static void draw_leaf(ki_td_soft_renderer *renderer, const ki_td_view *view,
                      float cx, float cy, float rx, float ry, uint32_t rgb,
                      uint8_t damage_mask, uint16_t fold_l)
{
    /* Nyctinasty folds the leaf toward vertical; it is derived, never stored,
     * so it costs nothing to apply here (D-093's calathea and ART_BIBLE §4.2). */
    float fold = (float)fold_l / (float)PG_LEVEL_MAX;
    float wide = rx * (1.0f - 0.72f * fold);
    float tall = ry * (1.0f + 0.55f * fold);

    ki_td_soft_fill_ellipse(renderer, view, cx, cy, wide, tall, rgb, 1.0f);

    /* Layer 3, the decals. Damage bits are set and never cleared, so a leaf
     * carries its whole history: this is the only route by which permanence
     * (D-030) reaches the screen, and D-047 calls it the load-bearing half. */
    if (damage_mask & (uint8_t)PG_DAMAGE_TIP_CRISP) {
        ki_td_soft_fill_rect(renderer, view, cx + wide - 1.0f, cy - 1.0f,
                             2.0f, 2.0f, UINT32_C(0x8a5a2a), 0.95f);
    }
    if (damage_mask & (uint8_t)PG_DAMAGE_MARGIN_BROWN) {
        ki_td_soft_fill_rect(renderer, view, cx - wide, cy + tall - 1.0f,
                             wide * 2.0f, 1.0f, UINT32_C(0x6f4a22), 0.85f);
    }
    if (damage_mask & (uint8_t)PG_DAMAGE_YELLOW) {
        ki_td_soft_fill_ellipse(renderer, view, cx, cy, wide * 0.6f,
                                tall * 0.6f, UINT32_C(0xc9c14a), 0.5f);
    }
    if (damage_mask & (uint8_t)PG_DAMAGE_STIPPLE) {
        ki_td_soft_fill_rect(renderer, view, cx - 1.0f, cy - 1.0f, 1.0f, 1.0f,
                             UINT32_C(0xd8d0b0), 0.8f);
        ki_td_soft_fill_rect(renderer, view, cx + 2.0f, cy + 1.0f, 1.0f, 1.0f,
                             UINT32_C(0xd8d0b0), 0.8f);
    }
    if (damage_mask & (uint8_t)PG_DAMAGE_ROT_BLOTCH) {
        ki_td_soft_fill_ellipse(renderer, view, cx - wide * 0.3f, cy,
                                wide * 0.35f, tall * 0.5f,
                                UINT32_C(0x3d2b1a), 0.85f);
    }
    if (damage_mask & (uint8_t)PG_DAMAGE_BLEACH) {
        ki_td_soft_fill_ellipse(renderer, view, cx, cy - tall * 0.3f,
                                wide * 0.5f, tall * 0.35f,
                                UINT32_C(0xe8e6cf), 0.55f);
    }
    if (damage_mask & (uint8_t)PG_DAMAGE_TRIMMED) {
        /* A trim is tidier, and the scar stays. You cannot undo, only tend. */
        ki_td_soft_fill_rect(renderer, view, cx + wide - 2.0f, cy - tall,
                             2.0f, tall * 2.0f, UINT32_C(0x5c4a2c), 0.7f);
    }
}

static void draw_plant_body(ki_td_soft_renderer *renderer,
                            const ki_td_view *view, const pg_state *state,
                            const pg_plant *plant, const pg_care_env *env,
                            uint32_t light)
{
    uint8_t health = pg_plant_health(plant);
    uint32_t base = sr_mix(health_tint(health), light, 0.18f);
    uint32_t dark = sr_scale_rgb(base, 0.72f);
    float droop = droop_offset(plant);
    float rim_y = (float)PG_POT_RIM_Y + droop;
    float cx = (float)PG_POT_CX;
    /* Silhouette (channel 1): growth stage picks the size the way it will pick
     * the atlas column once the art exists. */
    float scale = 0.55f + 0.22f * (float)plant->growth_stage;
    uint16_t fold = pg_plant_fold_l(plant, env->ordinal, env->local_minutes);
    size_t index;

    if (plant->species_id == (uint8_t)PG_SPECIES_SNAKE) {
        /* Upright blades, no droop to speak of: she is the easy one. */
        for (index = 0u; index < plant->leaf_count && index < PG_LEAF_MAX;
             ++index) {
            float offset = ((float)index - (float)plant->leaf_count * 0.5f);
            float lean = offset * 1.6f;
            float height = (34.0f + 5.0f * (float)(index % 3u)) * scale;
            draw_stem(renderer, view, cx + offset * 4.0f, rim_y, height,
                      (index & 1u) ? base : dark, lean);
            draw_leaf(renderer, view, cx + offset * 4.0f + lean,
                      rim_y - height, 2.4f, 5.0f * scale,
                      (index & 1u) ? base : dark,
                      plant->leaves[index].damage_mask, fold);
        }
        return;
    }

    if (plant->species_id == (uint8_t)PG_SPECIES_POTHOS &&
        plant->vine_count > 0u) {
        /* The vine is assembled from real segments, so its length is a real
         * growing quantity rather than a sprite swap. */
        float x = cx;
        float y = rim_y;
        for (index = 0u; index < plant->vine_count && index < PG_VINE_MAX;
             ++index) {
            float span = 3.0f + (float)plant->vine[index].internode * 0.06f;
            x += ((index & 1u) ? span : -span) * 0.6f;
            y += span * 0.55f;
            ki_td_soft_fill_rect(renderer, view, x, y, 2.0f, span, dark, 1.0f);
            draw_leaf(renderer, view, x + ((index & 1u) ? 4.0f : -4.0f), y,
                      4.5f * scale, 3.2f * scale, base,
                      plant->vine[index].damage_mask, fold);
        }
    }

    for (index = 0u; index < plant->leaf_count && index < PG_LEAF_MAX;
         ++index) {
        float angle = -1.15f + 0.42f * (float)index;
        float reach = (23.0f + 4.0f * (float)(index % 4u)) * scale;
        float lx = cx + sinf(angle) * reach;
        float ly = rim_y - cosf(angle) * reach * 0.72f;
        draw_stem(renderer, view, cx, rim_y, cosf(angle) * reach * 0.72f,
                  dark, sinf(angle) * reach * 0.5f);
        draw_leaf(renderer, view, lx, ly, 6.5f * scale, 4.2f * scale, base,
                  plant->leaves[index].damage_mask, fold);
    }

    /* The peace lily's whole achievement, and it is never promised. */
    if (plant->spathe_state == (uint8_t)PG_SPATHE_OPEN) {
        ki_td_soft_fill_ellipse(renderer, view, cx + 7.0f, rim_y - 26.0f,
                                4.0f, 7.0f, UINT32_C(0xf4f2e6), 1.0f);
        ki_td_soft_fill_rect(renderer, view, cx + 6.5f, rim_y - 30.0f, 1.0f,
                             6.0f, UINT32_C(0xd8d24a), 1.0f);
    }
    (void)state;
}


/* ---- the three overlay sheets ------------------------------------------- */

static bool overlay_cell(const pg_graphics *graphics, pg_overlay_atlas which,
                         unsigned column, unsigned row, ki_td_rgba8 *sprite)
{
    kilix_asset_region cell;

    if (graphics == NULL || !graphics->overlay_valid[which]) {
        return false;
    }
    cell = kilix_asset_atlas_cell(&graphics->overlay_grid[which],
                                  column, row);
    if (!kilix_asset_region_is_valid(&cell)) {
        return false;
    }
    *sprite = ki_td_rgba8_make(cell.pixels, (int)cell.width,
                               (int)cell.height);
    /* A region is a window into the atlas, not a copy: without its stride the
     * sprite walks the sheet a cell-width at a time and smears. */
    sprite->stride = cell.stride;
    return true;
}

/* The peace lily's bloom, drawn from the sheet when it exists. The procedural
 * ellipse in draw_plant_body stays as the fallback, so a missing sheet costs
 * fidelity and not the feature. Row 0 is the fresh sequence, row 1 the spent
 * one; the column is the bloom's own stage, which is why this sheet is not
 * indexed like a species body. */
static bool draw_spathe(ki_td_soft_renderer *renderer,
                        const ki_td_view *view,
                        const pg_graphics *graphics, const pg_plant *plant)
{
    ki_td_rgba8 sprite;
    unsigned column;
    unsigned row;

    switch (plant->spathe_state) {
    case (uint8_t)PG_SPATHE_BUDDING: column = 0u; row = 0u; break;
    case (uint8_t)PG_SPATHE_OPEN:    column = 2u; row = 0u; break;
    case (uint8_t)PG_SPATHE_FADING:  column = 1u; row = 1u; break;
    default: return false;
    }
    if (!overlay_cell(graphics, PG_OVERLAY_SPATHE, column, row, &sprite)) {
        return false;
    }
    ki_td_soft_rgba_pixel_art(renderer, view,
                              (float)PG_POT_CX - (float)sprite.width * 0.5f
                                  + 9.0f,
                              (float)PG_POT_RIM_Y - (float)sprite.height
                                  - 12.0f,
                              &sprite, 1.0f);
    return true;
}

/* The pothos trailer. ART_BIBLE makes vine length "a free realtime progress
 * bar": segments tile downward from the rim, so how far the vine falls is the
 * plant's growth rather than a number on the HUD. Row 1 is the yellowing set,
 * chosen on health so a struggling pothos trails yellow. The last cell drawn
 * is a tip, which is what stops the vine mid-air. */
static void draw_vines(ki_td_soft_renderer *renderer, const ki_td_view *view,
                       const pg_graphics *graphics, const pg_plant *plant)
{
    const unsigned SEGMENTS = 6u;
    ki_td_rgba8 sprite;
    unsigned row;
    unsigned length;
    unsigned index;
    float x;
    float y;

    if (graphics == NULL || !graphics->overlay_valid[PG_OVERLAY_VINES]) {
        return;
    }
    row = pg_plant_health(plant) >= (uint8_t)PG_HEALTH_THIRSTY ? 1u : 0u;
    /* One segment per growth stage, so a seedling trails nothing and a
     * specimen trails the full run. */
    length = plant->growth_stage;
    if (length == 0u) {
        return;
    }
    if (length > SEGMENTS) {
        length = SEGMENTS;
    }
    /* Drape it over the pot's left shoulder and down the outside.
     *
     * It used to start at the plant's rim in the middle of the pot column,
     * which put every segment above the soil line behind the pot -- the pot
     * is drawn in front, deliberately -- so the vine appeared to sprout from
     * underneath the pot instead of hanging over its edge. The authored pot
     * spans x=70..246, so the trailer sits just inside its left edge and
     * starts below the rim lip. */
    x = (float)PG_POT_CX - 84.0f;
    y = (float)PG_SURFACE_Y - 84.0f;
    for (index = 0u; index < length; ++index) {
        if (!overlay_cell(graphics, PG_OVERLAY_VINES, index % SEGMENTS, row,
                          &sprite)) {
            return;
        }
        ki_td_soft_rgba_pixel_art(renderer, view, x, y, &sprite, 1.0f);
        y += (float)sprite.height;
    }
    /* Column 6 is the tip; it must end the vine rather than continue it. */
    if (overlay_cell(graphics, PG_OVERLAY_VINES, 6u, row, &sprite)) {
        ki_td_soft_rgba_pixel_art(renderer, view, x, y, &sprite, 1.0f);
    }
}

/* The calathea folds its leaves upright after dark -- nyctinasty, the reason
 * it is called a prayer plant. That is a time-of-day pose, not a health
 * state, which is why this sheet is one row indexed by growth stage and why
 * it replaces the body rather than layering over it. */
static bool draw_calathea_night(ki_td_soft_renderer *renderer,
                                const ki_td_view *view,
                                const pg_graphics *graphics,
                                const pg_plant *plant,
                                const pg_care_env *env)
{
    ki_td_rgba8 sprite;
    unsigned stage;

    if (env->is_daylight || plant->species_id != (uint8_t)PG_SPECIES_CALATHEA
        || plant->life_state == (uint8_t)PG_LIFE_DEAD) {
        return false;
    }
    stage = plant->growth_stage < (uint8_t)PG_STAGE_COUNT
          ? plant->growth_stage : (unsigned)PG_STAGE_COUNT - 1u;
    if (!overlay_cell(graphics, PG_OVERLAY_CALATHEA_NIGHT, stage, 0u,
                      &sprite)) {
        return false;
    }
    ki_td_soft_rgba_pixel_art(renderer, view,
                              (float)PG_POT_CX - (float)sprite.width * 0.5f,
                              (float)PG_POT_RIM_Y - (float)sprite.height
                                  + 8.0f,
                              &sprite, 1.0f);
    return true;
}

/* Layer 2, the authored path. Returns false when this species has no atlas,
 * which is how the caller knows to draw the procedural plant instead. */
static bool draw_plant_atlas(ki_td_soft_renderer *renderer,
                             const ki_td_view *view,
                             const pg_graphics *graphics,
                             const pg_plant *plant, uint32_t light)
{
    kilix_asset_region cell;
    ki_td_rgba8 sprite;
    uint8_t health;
    uint8_t stage;
    float droop;
    float x;
    float y;

    if (graphics == NULL || plant->species_id >= (uint8_t)PG_SPECIES_COUNT ||
        !graphics->plant_atlas_valid[plant->species_id]) {
        return false;
    }
    stage = plant->growth_stage < (uint8_t)PG_STAGE_COUNT
          ? plant->growth_stage : (uint8_t)(PG_STAGE_COUNT - 1);
    health = pg_plant_health(plant);
    if (health >= (uint8_t)PG_HEALTH_COUNT) {
        health = (uint8_t)PG_HEALTH_COUNT - 1u;
    }
    cell = kilix_asset_atlas_cell(&graphics->plant_grid[plant->species_id],
                                  stage, health);
    if (!kilix_asset_region_is_valid(&cell)) {
        return false;
    }
    sprite = ki_td_rgba8_make(cell.pixels, (int)cell.width, (int)cell.height);
    sprite.stride = cell.stride;      /* a region is a window, not a copy */

    /* Posture, channel 4: the same droop the procedural plant uses, so the
     * two paths agree about what a limp plant looks like. */
    droop = droop_offset(plant);
    x = (float)PG_POT_CX - (float)cell.width * 0.5f;
    y = (float)PG_POT_RIM_Y + droop - (float)cell.height + 8.0f;

    /* Tinted rather than plain: channel 2 interpolates between the authored
     * rows and is what makes the transition between two health states a fade
     * rather than a jump. */
    ki_td_soft_rgba_tinted(renderer, view, x, y, &sprite, (int)cell.width,
                           (int)cell.height,
                           sr_mix(light, health_tint(health), 0.25f), 1.0f);
    return true;
}

/* Layer 3 for the authored path: the decals still composite at the leaf
 * anchors, because permanence (D-030) reaches the screen only through them and
 * an atlas cell cannot carry a per-leaf damage history. */
static void draw_plant_decals(ki_td_soft_renderer *renderer,
                              const ki_td_view *view, const pg_plant *plant,
                              const pg_care_env *env)
{
    size_t index;
    uint16_t fold = pg_plant_fold_l(plant, env->ordinal, env->local_minutes);
    float droop = droop_offset(plant);

    for (index = 0u; index < plant->leaf_count && index < PG_LEAF_MAX;
         ++index) {
        uint8_t mask = plant->leaves[index].damage_mask;
        float angle;
        float reach;

        if (mask == 0u) continue;
        angle = -1.15f + 0.42f * (float)index;
        reach = 18.0f + 3.0f * (float)(index % 4u);
        draw_leaf(renderer, view, (float)PG_POT_CX + sinf(angle) * reach,
                  (float)PG_POT_RIM_Y + droop - cosf(angle) * reach * 0.72f,
                  0.5f, 0.5f, UINT32_C(0x000000), mask, fold);
    }
}

/* ---- layer 4: the pot, in front ------------------------------------------ */
/* Rows: 0 clean, 1 salt crust, 2 rootbound, 3 failing. Structural wear
 * outranks the cosmetic crust -- a cracked pot that also has salt on it reads
 * as cracked, not as salty. */
#define PG_POT_SALT_VISIBLE 5000u

static unsigned pot_wear_row(const pg_plant *plant)
{
    unsigned capacity = plant->root_capacity != 0u
                      ? (unsigned)plant->root_capacity : 1u;
    unsigned bound = (unsigned)plant->root_bound * 100u / capacity;

    if (bound >= 100u) return 3u;
    if (bound >= 80u) return 2u;
    if ((unsigned)plant->salt >= PG_POT_SALT_VISIBLE) return 1u;
    return 0u;
}

/* The authored pot. The sheet is four materials across by four wear states
 * down, and it had been sitting in the tree unloaded while the pot was drawn
 * as a tapered stack of rectangles. Returns false when the sheet is absent so
 * the procedural pot still covers a build with no art. */
static bool draw_pot_atlas(ki_td_soft_renderer *renderer,
                           const ki_td_view *view,
                           const pg_graphics *graphics,
                           const pg_plant *plant)
{
    ki_td_rgba8 sprite;
    unsigned column = plant->pot_id < 4u ? (unsigned)plant->pot_id : 0u;

    if (!overlay_cell(graphics, PG_OVERLAY_POTS, column,
                      pot_wear_row(plant), &sprite)) {
        return false;
    }
    /* Bottom-anchored on the surface line, not centred on the pot box: the
     * pot stands on the ground the scene drew. */
    ki_td_soft_rgba_pixel_art(renderer, view,
                              (float)PG_POT_CX - (float)sprite.width * 0.5f,
                              (float)PG_SURFACE_Y - (float)sprite.height,
                              &sprite, 1.0f);
    return true;
}

static void draw_pot(ki_td_soft_renderer *renderer, const ki_td_view *view,
                     const pg_plant *plant)
{
    uint32_t body = pot_body_colour(plant->pot_id);
    uint32_t shade = sr_scale_rgb(body, 0.68f);
    float top = (float)PG_POT_RIM_Y;
    float bottom = (float)PG_SURFACE_Y;
    float half_top = 34.0f;
    float half_bottom = 25.0f;
    float y;

    /* A tapered body drawn as rows: no trapezoid primitive needed and the
     * silhouette stays crisp at every integer scale. */
    for (y = top; y < bottom; y += 1.0f) {
        float t = (y - top) / (bottom - top);
        float half = half_top + (half_bottom - half_top) * t;
        uint32_t rgb = sr_mix(body, shade, t * 0.65f);
        ki_td_soft_fill_rect(renderer, view, (float)PG_POT_CX - half, y,
                             half * 2.0f, 1.0f, rgb, 1.0f);
    }
    /* The rim, and the highlight that tells terracotta from glaze. */
    ki_td_soft_fill_rect(renderer, view, (float)PG_POT_CX - half_top - 1.0f,
                         top - 3.0f, (half_top + 1.0f) * 2.0f, 4.0f,
                         sr_scale_rgb(body, 1.12f), 1.0f);
    if (plant->pot_id == (uint8_t)PG_POT_GLAZED) {
        ki_td_soft_fill_ellipse(renderer, view, (float)PG_POT_CX - 12.0f,
                                top + 14.0f, 3.0f, 9.0f,
                                UINT32_C(0xffffff), 0.22f);
    }
    if (plant->pot_id == (uint8_t)PG_POT_NURSERY) {
        int rib;
        for (rib = -2; rib <= 2; ++rib) {
            ki_td_soft_fill_rect(renderer, view,
                                 (float)PG_POT_CX + (float)rib * 9.0f, top,
                                 1.0f, bottom - top,
                                 sr_scale_rgb(body, 0.85f), 0.7f);
        }
    }
}

/* ---- layer 5: soil-line dressing ----------------------------------------- */
static void draw_soil_line(ki_td_soft_renderer *renderer,
                           const ki_td_view *view, const pg_plant *plant)
{
    const pg_pot *pot = pg_content_pot(plant->pot_id);
    float rim = (float)PG_POT_RIM_Y;

    /* The visible band of soil at the rim -- never a top-down disc, because
     * the pot is in front and the surface is not visible (D-051). */
    ki_td_soft_fill_rect(renderer, view, (float)PG_POT_CX - 24.0f, rim - 2.0f,
                         48.0f, 2.0f, UINT32_C(0x3b2f24), 1.0f);

    /* Moisture reads as a sheen on that band, which is the honest cue: you
     * are looking at wet soil, not at a number. */
    {
        uint16_t whole = pg_care_moisture_whole_l(plant);
        float wet = (float)whole / (float)PG_LEVEL_MAX;
        if (wet > 0.05f) {
            ki_td_soft_fill_rect(renderer, view, (float)PG_POT_CX - 24.0f,
                                 rim - 2.0f, 48.0f, 1.0f,
                                 UINT32_C(0x6a8fa8), wet * 0.55f);
        }
    }
    /* Salt crust, on the pots that show it. */
    if (pot != NULL && pot->shows_salt_crust && plant->salt > 3000u) {
        float amount = (float)plant->salt / (float)PG_LEVEL_MAX;
        ki_td_soft_fill_rect(renderer, view, (float)PG_POT_CX - 22.0f,
                             rim - 3.0f, 44.0f, 1.0f, UINT32_C(0xe8e2d0),
                             amount * 0.8f);
    }
    /* Fungus gnats: a real consequence of soil that never dries. */
    if (plant->pest_gnats > 1500u) {
        int gnat;
        int count = (int)(plant->pest_gnats / 2500u) + 1;
        if (count > 5) count = 5;
        for (gnat = 0; gnat < count; ++gnat) {
            float gx = (float)PG_POT_CX - 18.0f + (float)(gnat * 9);
            float gy = rim - 8.0f - (float)((gnat * 5) % 7);
            ki_td_soft_fill_rect(renderer, view, gx, gy, 1.0f, 1.0f,
                                 UINT32_C(0x201a14), 0.9f);
        }
    }
}

/* ---- layer 1: static room dressing --------------------------------------- */
static void draw_room_dressing(ki_td_soft_renderer *renderer,
                               const ki_td_view *view)
{
    /* A shelf edge under the pot so the plant stands on something even with
     * the backdrop off. */
    ki_td_soft_fill_rect(renderer, view, (float)PG_POT_CX - 62.0f,
                         (float)PG_SURFACE_Y - 1.0f, 124.0f, 1.0f,
                         UINT32_C(0x7a6952), 0.55f);
}

/* ---- layer 6: overlays --------------------------------------------------- */
/* `view` is the real stage view and `plant_view` the scene-shifted one. The
 * night wash covers the whole screen and must use the former: drawn through
 * the shifted view it left an unwashed strip at the bottom exactly as wide as
 * the scene's nudge. Everything else here hangs off the pot and uses the
 * latter. */
static void draw_overlays(ki_td_soft_renderer *renderer,
                          const ki_td_view *view,
                          const ki_td_view *plant_view,
                          const pg_plant *plant,
                          const pg_care_env *env)
{
    /* Night is a look, not a filter on the simulation: a cool wash after dusk
     * so the calathea's fold reads as evening rather than as damage. */
    if (!env->is_daylight) {
        ki_td_soft_fill_rect(renderer, view, 0.0f, 0.0f,
                             (float)PG_LOGICAL_WIDTH,
                             (float)PG_LOGICAL_HEIGHT,
                             UINT32_C(0x101a2c), 0.22f);
    }
    if (plant->life_state == (uint8_t)PG_LIFE_DEAD) {
        return;
    }
    /* Just-watered droplets. */
    if (pg_care_moisture_whole_l(plant) > 8200u) {
        int drop;
        for (drop = 0; drop < 3; ++drop) {
            float dx = (float)PG_POT_CX - 10.0f + (float)(drop * 10);
            ki_td_soft_fill_rect(renderer, plant_view, dx,
                                 (float)PG_POT_RIM_Y - 6.0f - (float)drop,
                                 1.0f, 2.0f, UINT32_C(0x9fd0e8), 0.75f);
        }
    }
}

/* ---- the frame ----------------------------------------------------------- */

bool pg_render(ki_td_soft_renderer *renderer, const pg_state *state,
               const pg_graphics *graphics, pg_render_result *result)
{
    ki_td_view view;
    pg_damage_set damage;
    sr_canvas *canvas;
    int clip_x0, clip_y0, clip_x1, clip_y1;
    uint32_t light = UINT32_C(0xffffff);
    const pg_scene_desc *scene = NULL;
    size_t plant_index;

    if (renderer == NULL || state == NULL) {
        return false;
    }
    if (!pg_render_fit_view(&view, ki_td_soft_width(renderer),
                  ki_td_soft_height(renderer))) {
        return false;
    }
    canvas = ki_td_soft_canvas(renderer);
    if (canvas == NULL) {
        return false;
    }
    /* Intersect and RESTORE: the caller's clip is the caller's. */
    clip_x0 = canvas->clip_x0; clip_y0 = canvas->clip_y0;
    clip_x1 = canvas->clip_x1; clip_y1 = canvas->clip_y1;

    if (graphics != NULL && graphics->scene < graphics->scene_count) {
        scene = &graphics->scenes[graphics->scene];
        light = pg_scene_light_tint(scene->light_from);
    }

    pg_damage_reset(&damage, ki_td_soft_width(renderer),
                    ki_td_soft_height(renderer));
    /* Milestone 6 repaints whole frames. Partial damage arrives with the UI,
     * which is what makes it worth the bookkeeping. */
    pg_damage_mark_full(&damage);

    /* Layer 0. A plate if one is loaded, the procedural stage otherwise --
     * and the procedural stage is a designed look, not a fallback. */
    if (graphics != NULL && graphics->plate_valid &&
        kilix_asset_image_is_valid(&graphics->plate)) {
        ki_td_rgba8 plate = ki_td_rgba8_make(graphics->plate.pixels,
                                             (int)graphics->plate.width,
                                             (int)graphics->plate.height);
        ki_td_soft_rgba_backdrop(renderer, &view, &plate, 1.0f);
    } else {
        pg_scene_draw_procedural(renderer, &view);
    }

    /* Layer 1. */
    draw_room_dressing(renderer, &view);

    /* The plant composite meets the surface the scene drew.
     *
     * Every plate puts its counter or shelf at a different height, and the
     * stage constants are one fixed set, so without this the pot stands sunk
     * into the cabinetry -- by 13 logical pixels on the studio and 59 on the
     * bright corner. The shift is applied to a COPY of the view, and only
     * around the plant layers: the HUD, the calendar and the backdrop keep
     * the real view, because a scene aligns the plant to itself and is not
     * allowed to move the composite around it.
     *
     * view offsets are screen pixels, so the logical nudge scales. */
    {
        ki_td_view plant_view = view;
        /* Only when a plate actually loaded. The nudge aligns the plant to a
         * surface the SCENE drew, so with no plate there is no surface to
         * meet and the procedural room's own PG_SURFACE_Y is already right.
         * Applying it regardless also broke a real invariant the render
         * suite checks: two scenes with the same light and panel side and no
         * plate must be indistinguishable, and they stopped being so. */
        bool aligned = scene != NULL && scene->plate_loaded;
        int nudge_y = aligned ? pg_scene_clamp_nudge(scene->nudge_y) : 0;
        int nudge_x = aligned ? pg_scene_clamp_nudge(scene->nudge_x) : 0;

        plant_view.offset_x += (int)((float)nudge_x * view.scale);
        plant_view.offset_y += (int)((float)nudge_y * view.scale);

    for (plant_index = 0u;
         plant_index < state->plant_count && plant_index < PG_PLANT_MAX;
         ++plant_index) {
        const pg_plant *plant = &state->plants[plant_index];
        pg_care_env env = pg_sim_env_now(state, (uint8_t)plant_index);

        /* Layer 2. Authored art when the species has an atlas, the procedural
         * plant when it does not -- the same "missing is not an error" rule
         * the backdrops follow, and the reason art latency was never on the
         * critical path.
         *
         * Channel 1 of ARCHITECTURE.md §3.3: growth stage picks the COLUMN,
         * health picks the ROW. The colour ramp (channel 2) still tints,
         * because it interpolates between the six authored rows rather than
         * replacing them, and the decals and posture channels still apply on
         * top -- an atlas cell is the silhouette, not the whole readout. */
        /* The night pose replaces the body outright -- a folded calathea is
         * a different silhouette, not a tint over the open one. */
        if (!draw_calathea_night(renderer, &plant_view, graphics, plant, &env)) {
            if (!draw_plant_atlas(renderer, &plant_view, graphics, plant, light)) {
                draw_plant_body(renderer, &plant_view, state, plant, &env, light);
            } else {
                draw_plant_decals(renderer, &plant_view, plant, &env);
            }
        }
        if (plant->species_id == (uint8_t)PG_SPECIES_PEACE_LILY) {
            (void)draw_spathe(renderer, &plant_view, graphics, plant);
        }
        /* Layer 4 -- in front of the stems, deliberately. */
        if (!draw_pot_atlas(renderer, &plant_view, graphics, plant)) {
            draw_pot(renderer, &plant_view, plant);
        }
        /* In front of the pot: a trailer hangs down the OUTSIDE of the pot it
         * grows in, so it occludes the pot rather than the other way round. */
        if (plant->species_id == (uint8_t)PG_SPECIES_POTHOS) {
            draw_vines(renderer, &plant_view, graphics, plant);
        }
        /* Layer 5. */
        draw_soil_line(renderer, &plant_view, plant);
        /* Layer 6. */
        draw_overlays(renderer, &view, &plant_view, plant, &env);
    }
    }

    /* Layer 7: the occluder, over everything the room owns. */
    if (graphics != NULL && graphics->front_valid &&
        kilix_asset_image_is_valid(&graphics->front)) {
        ki_td_rgba8 front = ki_td_rgba8_make(graphics->front.pixels,
                                             (int)graphics->front.width,
                                             (int)graphics->front.height);
        ki_td_soft_rgba_backdrop(renderer, &view, &front, 1.0f);
    }

    /* Layers 8 and 9: the HUD and the calendar/journal, all through
     * kilix-ui. Drawn over the occluder because the interface belongs to the
     * player, not to the room. */
    pg_ui_draw(renderer, &view, state, &state->ui, graphics);

    canvas->clip_x0 = clip_x0; canvas->clip_y0 = clip_y0;
    canvas->clip_x1 = clip_x1; canvas->clip_y1 = clip_y1;

    if (result != NULL) {
        result->view = view;
        pg_damage_publish(&damage, result);
    }
    return true;
}
