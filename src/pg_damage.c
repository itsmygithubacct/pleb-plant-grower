/*
 * pg_damage — see pg_damage.h for why the merge rule is deliberately coarse.
 */
#include "pg_damage.h"

#include <string.h>

static bool rect_is_empty(const pg_rect *rect)
{
    return rect->x1 <= rect->x0 || rect->y1 <= rect->y0;
}

static bool rects_touch(const pg_rect *a, const pg_rect *b)
{
    /* Touching counts as overlapping: two abutting rectangles are cheaper as
     * one, and the union is never wrong, only larger. */
    return a->x0 <= b->x1 && b->x0 <= a->x1 &&
           a->y0 <= b->y1 && b->y0 <= a->y1;
}

static void rect_union(pg_rect *into, const pg_rect *other)
{
    if (other->x0 < into->x0) into->x0 = other->x0;
    if (other->y0 < into->y0) into->y0 = other->y0;
    if (other->x1 > into->x1) into->x1 = other->x1;
    if (other->y1 > into->y1) into->y1 = other->y1;
}

void pg_damage_reset(pg_damage_set *set, int width, int height)
{
    if (set == NULL) {
        return;
    }
    memset(set, 0, sizeof *set);
    set->width = width > 0 ? width : 0;
    set->height = height > 0 ? height : 0;
}

void pg_damage_mark_full(pg_damage_set *set)
{
    if (set == NULL) {
        return;
    }
    set->full_frame = true;
    set->count = 0u;
}

void pg_damage_add(pg_damage_set *set, int x0, int y0, int x1, int y1)
{
    pg_rect rect;
    size_t index;

    if (set == NULL || set->full_frame) {
        return;                    /* already transmitting everything */
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > set->width) x1 = set->width;
    if (y1 > set->height) y1 = set->height;
    rect.x0 = x0; rect.y0 = y0; rect.x1 = x1; rect.y1 = y1;
    if (rect_is_empty(&rect)) {
        return;
    }

    for (index = 0u; index < set->count; ++index) {
        if (rects_touch(&set->rects[index], &rect)) {
            rect_union(&set->rects[index], &rect);
            return;
        }
    }
    if (set->count >= (size_t)PG_DAMAGE_MAX) {
        /* Out of budget. One full frame is always correct; a dropped rectangle
         * would be a silent partial update, which is the one outcome that
         * looks like a rendering bug forever. */
        pg_damage_mark_full(set);
        return;
    }
    set->rects[set->count++] = rect;
}

void pg_damage_add_logical(pg_damage_set *set, float scale, int origin_x,
                           int origin_y, float x, float y, float w, float h)
{
    float left, top, right, bottom;

    if (set == NULL || set->full_frame || !(scale > 0.0f) ||
        !(w > 0.0f) || !(h > 0.0f)) {
        return;
    }
    left = (float)origin_x + x * scale;
    top = (float)origin_y + y * scale;
    right = left + w * scale;
    bottom = top + h * scale;
    /* One pixel of slack on each side: at a fractional scale the rasterizer's
     * rounding can touch the neighbouring pixel, and a seam is far more
     * visible than a marginally larger rectangle is expensive. */
    pg_damage_add(set, (int)left - 1, (int)top - 1,
                  (int)right + 2, (int)bottom + 2);
}

void pg_damage_publish(const pg_damage_set *set, pg_render_result *result)
{
    size_t index;

    if (result == NULL) {
        return;
    }
    result->rect_count = 0u;
    if (set == NULL) {
        result->full_frame = true;
        return;
    }
    if (set->full_frame || set->count == 0u) {
        /* No rectangles at all also means "everything": a caller that tracked
         * nothing must not be told nothing changed. */
        result->full_frame = true;
        return;
    }
    result->full_frame = false;
    for (index = 0u; index < set->count && index < (size_t)PG_DAMAGE_MAX;
         ++index) {
        result->rects[index] = set->rects[index];
        result->rect_count += 1u;
    }
}
