/*
 * pg_damage — which framebuffer pixels actually changed.
 *
 * The terminal transport is the expensive part of this game: every pixel we
 * claim changed is zlib'd and base64'd and pushed down a pty. So the renderer
 * reports rectangles and the host transmits only those.
 *
 * Rectangles are in FRAMEBUFFER pixels with x1/y1 EXCLUSIVE, which is the
 * presenter's convention. That is deliberately not ki_td_rect (x/y/w/h in
 * logical space) and deliberately not kittyfb_rect (which the core may not
 * name) -- pg_rect exists precisely so the core does not have to agree with
 * either of them.
 *
 * The merge rule is coarse on purpose: PG_DAMAGE_MAX rectangles is a small
 * budget, and a handful of slightly-too-large rectangles costs far less than
 * the bookkeeping to keep them minimal. When the budget is exhausted the
 * result collapses to one full-frame rectangle, which is always correct and
 * never a silent partial update.
 */
#ifndef PG_DAMAGE_H
#define PG_DAMAGE_H

#include "pleb_plant_grower.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct pg_damage_set {
    pg_rect rects[PG_DAMAGE_MAX];
    size_t count;
    bool full_frame;
    int width, height;             /* the framebuffer, for the full-frame rect */
} pg_damage_set;

void pg_damage_reset(pg_damage_set *set, int width, int height);

/* Add a rectangle in framebuffer pixels. Empty and out-of-bounds rectangles
 * are clipped away rather than rejected: a caller computing a rect from a
 * clipped sprite should not have to check twice. */
void pg_damage_add(pg_damage_set *set, int x0, int y0, int x1, int y1);

/* Add a rectangle given in LOGICAL coordinates, scaled and origin-shifted into
 * framebuffer pixels, then padded by one pixel on every side so a fractional
 * scale cannot leave a seam. */
void pg_damage_add_logical(pg_damage_set *set, float scale, int origin_x,
                           int origin_y, float x, float y, float w, float h);

/* Everything changed. Idempotent. */
void pg_damage_mark_full(pg_damage_set *set);

/* Publish into a pg_render_result. `result` may be NULL. */
void pg_damage_publish(const pg_damage_set *set, pg_render_result *result);

#endif /* PG_DAMAGE_H */
