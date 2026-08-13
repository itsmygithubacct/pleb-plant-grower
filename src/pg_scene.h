/*
 * pg_scene — backgrounds, and the procedural stage.
 *
 * ARCHITECTURE.md §4.1 and §4.3. Two things live here that are easy to
 * conflate, and conflating them is how the plan once lost a scene:
 *
 *   - **`plain-studio`** is a real, generated 1920x1080 plate and the default
 *     scene. It is what you see if you change nothing.
 *   - **background-off** (`scene == NULL`, `PLEB_PLANT_BACKGROUND=none`) is the
 *     *procedural* stage, drawn in code: a vertical sky band, a floor band, a
 *     soft shelf line and a warm pool of light at the pot. It is a designed
 *     look, not a degradation — the owner asked for the background to be
 *     disable-able, and this is that feature.
 *
 * Both exist and both ship.
 *
 * A scene is **cosmetic only** and never a simulation input (D-041). `light_from`
 * tints the render; it does not reach pg_care. The spot is the simulation
 * object, and `default` / `default_spot` are deliberately not paired by index
 * (D-081).
 */
#ifndef PG_SCENE_H
#define PG_SCENE_H

#include "pleb_plant_grower.h"

#include "kilix_top_down_soft.h"
#include "kilix_top_down_view.h"

#include <stdbool.h>
#include <stdint.h>

/* The stage geometry of content/layout.json (D-042). Restated here as
 * compile-time constants because every layer hangs off them; the JSON is what
 * the art tools read, and tests/test_content.py holds the two in agreement. */
#define PG_POT_CX      158
#define PG_SURFACE_Y   232
#define PG_POT_RIM_Y   144
#define PG_PANEL_X     330
#define PG_HORIZON_Y   176

/* A plate may be aligned but no plate may move the composite: the pot, the
 * HUD and the calendar sit where layout.json puts them, and a scene only
 * shifts the plant to meet the surface it drew.
 *
 * The limit was 16, on the assumption that every plate would be authored with
 * its surface at PG_SURFACE_Y. None of the six were -- their surfaces land
 * between logical 173 and 219 against a constant of 232 -- so the pot stood
 * sunk into the cabinetry, by a different amount in every scene. The plates
 * cover 1920x1080 exactly, with no crop headroom to realign them, so the
 * choice was this number or regenerating all six to a fixed surface line.
 * 64 covers the measured range; the invariant it protects is unchanged. */
#define PG_ANCHOR_NUDGE_LIMIT 64

#define PG_SCENE_ID_BYTES 24
#define PG_SCENE_NAME_BYTES 32

typedef enum pg_light_from {
    PG_LIGHT_FROM_FRONT = 0,
    PG_LIGHT_FROM_LEFT = 1,
    PG_LIGHT_FROM_RIGHT = 2
} pg_light_from;

typedef enum pg_panel_side {
    PG_PANEL_RIGHT = 0,
    PG_PANEL_LEFT = 1
} pg_panel_side;

typedef struct pg_scene_desc {
    char id[PG_SCENE_ID_BYTES];
    char name[PG_SCENE_NAME_BYTES];
    int16_t nudge_x, nudge_y;      /* clamped to +-PG_ANCHOR_NUDGE_LIMIT */
    uint8_t panel_side;            /* pg_panel_side */
    uint8_t light_from;            /* pg_light_from */
    uint32_t ink, ink_shadow;      /* 0xRRGGBB */
    bool front_overlay;
    bool plate_loaded;             /* a plate resolved and passed the size gate */
} pg_scene_desc;

/* Draw the procedural stage: the background-off look. Never fails, needs no
 * asset, and is the only backdrop that always exists. */
void pg_scene_draw_procedural(ki_td_soft_renderer *renderer,
                              const ki_td_view *view);

/* The tint a scene's light direction applies to the plant body, as a Q12-style
 * float multiplier triple packed into 0xRRGGBB scaling. Cosmetic only. */
uint32_t pg_scene_light_tint(uint8_t light_from);

/* Clamp an authored nudge to the limit above. */
int pg_scene_clamp_nudge(int nudge);

#endif /* PG_SCENE_H */
