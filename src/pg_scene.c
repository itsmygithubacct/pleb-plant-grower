/*
 * pg_scene — the procedural stage and the scene descriptors.
 *
 * See pg_scene.h for why background-off is a designed look rather than a
 * fallback. Everything below draws in LOGICAL space through the view, so the
 * same code produces the same picture at every integer scale.
 */
#include "pg_scene.h"

#include "kilix_top_down_soft.h"

#include <math.h>

/* The palette of content/layout.json's procedural_stage block. Restated as
 * constants for the same reason the geometry is: the renderer must not read a
 * file, and the JSON is the art tools' copy. */
#define PG_STAGE_SKY_TOP     UINT32_C(0x2b3a4a)
#define PG_STAGE_SKY_BOTTOM  UINT32_C(0x4a5a63)
#define PG_STAGE_FLOOR       UINT32_C(0x3a3128)
#define PG_STAGE_FLOOR_SHADE UINT32_C(0x2a231c)
#define PG_STAGE_SHELF       UINT32_C(0x6b5a44)
#define PG_STAGE_LIGHT_POOL  UINT32_C(0x8a7a55)

int pg_scene_clamp_nudge(int nudge)
{
    if (nudge < -PG_ANCHOR_NUDGE_LIMIT) return -PG_ANCHOR_NUDGE_LIMIT;
    if (nudge > PG_ANCHOR_NUDGE_LIMIT) return PG_ANCHOR_NUDGE_LIMIT;
    return nudge;
}

uint32_t pg_scene_light_tint(uint8_t light_from)
{
    /* Cosmetic only (D-041). A warm key from one side, a neutral front. */
    switch ((pg_light_from)light_from) {
    case PG_LIGHT_FROM_LEFT:  return UINT32_C(0xfff4e2);
    case PG_LIGHT_FROM_RIGHT: return UINT32_C(0xf4f0ff);
    case PG_LIGHT_FROM_FRONT:
    default:                  return UINT32_C(0xffffff);
    }
}

void pg_scene_draw_procedural(ki_td_soft_renderer *renderer,
                              const ki_td_view *view)
{
    int row;

    if (renderer == NULL || view == NULL) {
        return;
    }

    /* The wall: a vertical gradient, one logical row at a time. Banding is
     * the point -- this is a pixel-art stage, not a photograph. */
    for (row = 0; row < PG_HORIZON_Y; ++row) {
        float t = (float)row / (float)PG_HORIZON_Y;
        uint32_t rgb = sr_mix(PG_STAGE_SKY_TOP, PG_STAGE_SKY_BOTTOM, t);
        ki_td_soft_fill_rect(renderer, view, 0.0f, (float)row,
                             (float)PG_LOGICAL_WIDTH, 1.0f, rgb, 1.0f);
    }

    /* The floor, and a darker band under the shelf so the surface reads as a
     * surface rather than as a colour change. */
    ki_td_soft_fill_rect(renderer, view, 0.0f, (float)PG_HORIZON_Y,
                         (float)PG_LOGICAL_WIDTH,
                         (float)(PG_LOGICAL_HEIGHT - PG_HORIZON_Y),
                         PG_STAGE_FLOOR, 1.0f);
    ki_td_soft_fill_rect(renderer, view, 0.0f, (float)(PG_SURFACE_Y + 2),
                         (float)PG_LOGICAL_WIDTH,
                         (float)(PG_LOGICAL_HEIGHT - PG_SURFACE_Y - 2),
                         PG_STAGE_FLOOR_SHADE, 1.0f);

    /* A warm pool of light on the surface, centred on the pot. Drawn as
     * concentric ellipses of falling alpha: no allocation, no blur kernel. */
    {
        int ring;
        for (ring = 6; ring >= 1; --ring) {
            float rx = 26.0f + (float)ring * 11.0f;
            float ry = 4.0f + (float)ring * 1.6f;
            float alpha = 0.055f;
            ki_td_soft_fill_ellipse(renderer, view, (float)PG_POT_CX,
                                    (float)PG_SURFACE_Y, rx, ry,
                                    PG_STAGE_LIGHT_POOL, alpha);
        }
    }

    /* The shelf line itself, and its lip. */
    ki_td_soft_fill_rect(renderer, view, 0.0f, (float)PG_SURFACE_Y,
                         (float)PG_LOGICAL_WIDTH, 1.0f, PG_STAGE_SHELF, 0.9f);
    ki_td_soft_fill_rect(renderer, view, 0.0f, (float)(PG_SURFACE_Y + 1),
                         (float)PG_LOGICAL_WIDTH, 1.0f,
                         sr_scale_rgb(PG_STAGE_SHELF, 0.55f), 0.8f);
}
