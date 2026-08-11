/*
 * pg_render — the renderer's internal seam.
 *
 * `pg_render` itself is public (pleb_plant_grower.h). What is declared here is
 * what the render harness needs: the framebuffer snapping rule, so a test can
 * assert the scale it expects without duplicating the arithmetic, and the
 * headless diagnostic behind --render-test.
 */
#ifndef PG_RENDER_H
#define PG_RENDER_H

#include "pleb_plant_grower.h"

#include "kilix_top_down_view.h"

#include <stdbool.h>

/* Fit a view to a framebuffer under the snapping rule of
 * IMPLEMENTATION_PLAN.md §10.2: the largest integer scale in 2..4 that fits
 * exactly, so the frame is never letterboxed and the pixel-art blit always
 * takes its fast path; below 960x540, a fractional fit rather than nothing. */
bool pg_render_fit_view(ki_td_view *view, int width, int height);

/* Headless diagnostic behind `--render-test <seed> <dir>`. Writes one PPM per
 * fixture into a caller-supplied directory -- never the working tree -- and
 * asserts the properties of §10.5: the fixture count, a valid P6 header and
 * exact payload size, that no frame is blank, that states which should look
 * different do, that rendering mutates nothing, and both halves of the
 * time-zone gate. Returns 0 when every assertion held. */
int pg_render_run_test(uint64_t seed, const char *directory);

#endif /* PG_RENDER_H */
