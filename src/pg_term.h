/*
 * pg_term — the standalone frontend's terminal side.
 *
 * Never part of the archive kilix-land links: the embed brings its own host,
 * its own clock and its own input. Everything here is the part of the game
 * that only exists when it owns a terminal.
 *
 * Three responsibilities, and the reason each lives here rather than in the
 * core:
 *
 *   - **The clock.** Nothing in the core may call a clock function, which is
 *     what makes six months of growth a unit test and what `make embed-guard`
 *     enforces symbolically. So the frontend samples CLOCK_REALTIME,
 *     CLOCK_BOOTTIME and the kernel boot id, and fills a pg_now.
 *   - **The time zone.** `tz_offset_minutes` is a SIMULATION input (D-079) and
 *     is derived on **every** sample, never once at startup. Deriving it once
 *     is exactly what makes a long-running session wrong the moment DST flips
 *     — the plant's local hour would silently shift by an hour and its
 *     nyctinasty, daylight window and midnight rollover would all follow.
 *   - **The resolution floor.** kittyfb_options has min_width/min_height and
 *     we deliberately leave them unset. The library's defaults sit above our
 *     absolute floor, so setting them would make the game refuse to start on a
 *     small terminal instead of degrading to the fractional fit that
 *     pg_render already owns. The floor is policy, and policy belongs here.
 */
#ifndef PG_TERM_H
#define PG_TERM_H

#include "pleb_plant_grower.h"

#include "kitty_input.h"
#include "kitty_terminal_session.h"

#include <stdbool.h>

/* Sample the wall clock, the boot clock, the boot id and the local offset.
 * The only place in the standalone build that reads a clock. */
pg_now pg_term_now(void);

/* Translate one terminal event into the semantic input record. Accumulates
 * into `in` rather than replacing it, because several events arrive between
 * two steps and a frame's input is their union. `view` and the session origin
 * turn mouse pixels into logical coordinates; pass NULL to skip the pointer. */
void pg_term_translate(const kittyin_event *event, pg_input *in,
                       const ki_td_view *view, int origin_x, int origin_y);

/* Clear the per-frame edges, keeping nothing. Call after a step has consumed
 * the input: a held key must not water the plant sixty times a second. */
void pg_term_input_reset(pg_input *in);

/* Present a frame, using damage rectangles when the renderer reported them.
 * kittyts_present_damage falls back to a full frame by itself when patching
 * cannot help, so this never has to decide which is cheaper. */
bool pg_term_present(kittyts_session *session, const uint8_t *rgba,
                     int width, int height,
                     const pg_render_result *result);

#endif /* PG_TERM_H */
