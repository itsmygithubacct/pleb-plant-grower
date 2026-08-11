/*
 * pg_term — see pg_term.h. Standalone only.
 */
#include "pg_term.h"

#include "kilix_game_runtime.h"
#include "kilix_top_down_view.h"
#include "kitty_framebuffer.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* The kernel boot id, read once. It is a stable string for the life of a
 * boot, which is exactly the property the gap policy needs: an id that
 * changed within a boot would make every launch look like a reboot, and one
 * that persisted across boots would let a rewound wall clock look trustworthy.
 *
 * A host that will not give us one leaves it all zeroes, and pg_time handles
 * that case explicitly (D-095): sixteen zero bytes compare EQUAL to
 * themselves, so a reboot on such a host looks like one continuous boot with
 * a rewound boot clock, and the policy falls back to the wall gap. */
static void read_boot_id(uint8_t out[PG_BOOT_ID_BYTES])
{
    static uint8_t cached[PG_BOOT_ID_BYTES];
    static bool loaded;
    FILE *file;

    if (!loaded) {
        loaded = true;
        memset(cached, 0, sizeof cached);
        file = fopen("/proc/sys/kernel/random/boot_id", "re");
        if (file != NULL) {
            char text[64];
            if (fgets(text, (int)sizeof text, file) != NULL) {
                size_t index;
                size_t written = 0u;
                /* Fold the hex digits into the sixteen bytes they encode;
                 * anything unparsable simply contributes nothing, which
                 * degrades to the all-zeroes case rather than to garbage. */
                for (index = 0u; text[index] != '\0' &&
                                 written < (size_t)PG_BOOT_ID_BYTES * 2u;
                     ++index) {
                    char c = text[index];
                    int value;
                    if (c >= '0' && c <= '9') value = c - '0';
                    else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
                    else continue;
                    if ((written & 1u) == 0u) {
                        cached[written / 2u] = (uint8_t)(value << 4);
                    } else {
                        cached[written / 2u] |= (uint8_t)value;
                    }
                    ++written;
                }
            }
            (void)fclose(file);
        }
    }
    memcpy(out, cached, (size_t)PG_BOOT_ID_BYTES);
}

pg_now pg_term_now(void)
{
    pg_now now;
    struct timespec wall;
    struct timespec boot;
    struct tm local;
    time_t seconds;

    memset(&now, 0, sizeof now);
    if (clock_gettime(CLOCK_REALTIME, &wall) == 0) {
        now.wall_s = (int64_t)wall.tv_sec;
    }
#if defined(CLOCK_BOOTTIME)
    if (clock_gettime(CLOCK_BOOTTIME, &boot) == 0) {
        now.boot_s = (int64_t)boot.tv_sec;
    }
#else
    /* CLOCK_MONOTONIC stops across suspend, so a laptop lid costs the plant
     * that time. It is the honest fallback: it can only under-credit. */
    if (clock_gettime(CLOCK_MONOTONIC, &boot) == 0) {
        now.boot_s = (int64_t)boot.tv_sec;
    }
#endif
    read_boot_id(now.boot_id);

    /* Every sample, not once. A session left running across a DST boundary
     * must see the new offset on the very next frame (D-079). */
    seconds = (time_t)now.wall_s;
    if (localtime_r(&seconds, &local) != NULL) {
        long offset = local.tm_gmtoff / 60;
        if (offset < PG_TZ_OFFSET_MIN || offset > PG_TZ_OFFSET_MAX) {
            offset = 0;
        }
        now.tz_offset_minutes = (int32_t)offset;
    }
    return now;
}

void pg_term_input_reset(pg_input *in)
{
    int pointer_x;
    int pointer_y;

    if (in == NULL) return;
    /* The pointer is a position, not an edge: it persists between frames the
     * way a mouse cursor does. Everything else is consumed. */
    pointer_x = in->pointer_x;
    pointer_y = in->pointer_y;
    memset(in, 0, sizeof *in);
    in->pointer_x = pointer_x;
    in->pointer_y = pointer_y;
}

void pg_term_translate(const kittyin_event *event, pg_input *in,
                       const ki_td_view *view, int origin_x, int origin_y)
{
    if (event == NULL || in == NULL) return;

    if (event->kind == KITTYIN_EVENT_MOUSE) {
        if (view != NULL && event->data.mouse.pixel_coordinates) {
            float logical_x = 0.0f;
            float logical_y = 0.0f;
            if (ki_td_screen_to_logical(
                    view, (float)(event->data.mouse.x - origin_x),
                    (float)(event->data.mouse.y - origin_y),
                    &logical_x, &logical_y)) {
                in->pointer_x = (int)logical_x;
                in->pointer_y = (int)logical_y;
            }
        }
        if (event->data.mouse.button != 0u) {
            in->pointer_pressed = true;
        }
        if (event->data.mouse.wheel_y != 0) {
            in->menu_delta += event->data.mouse.wheel_y;
        }
        return;
    }
    if (event->kind != KITTYIN_EVENT_KEY) return;

    /* The eleven verb shortcuts of D-087. Every other verb is reached through
     * the action list plus confirm, so this struct never grows one boolean
     * per verb. */
    if (kilix_game_event_letter(event, 'c')) in->check_soil = true;
    if (kilix_game_event_letter(event, 'l')) in->lift_pot = true;
    if (kilix_game_event_letter(event, 'w')) in->water = true;
    if (kilix_game_event_letter(event, 'f')) in->feed = true;
    if (kilix_game_event_letter(event, 'm')) in->mist = true;
    if (kilix_game_event_letter(event, 'r')) in->rotate = true;
    if (kilix_game_event_letter(event, 'p')) in->wipe = true;
    if (kilix_game_event_letter(event, 't')) in->trim = true;
    if (kilix_game_event_letter(event, 'o')) in->repot = true;
    if (kilix_game_event_letter(event, 'd')) in->drain = true;
    if (kilix_game_event_letter(event, 's')) in->sleeve = true;

    if (kilix_game_event_letter(event, 'k')) in->toggle_calendar = true;
    if (kilix_game_event_letter(event, 'j')) in->toggle_journal = true;
    if (kilix_game_event_letter(event, 'b')) in->toggle_label = true;
    if (kilix_game_event_letter(event, 'h')) in->help = true;

    switch (event->data.key.key) {
    case KITTYKB_KEY_UP:     in->move_y = -1; break;
    case KITTYKB_KEY_DOWN:   in->move_y = 1;  break;
    case KITTYKB_KEY_LEFT:   in->move_x = -1; break;
    case KITTYKB_KEY_RIGHT:  in->move_x = 1;  break;
    case KITTYKB_KEY_ENTER:  in->confirm = true; break;
    case KITTYKB_KEY_ESCAPE: in->cancel = true;  break;
    default: break;
    }
}

bool pg_term_present(kittyts_session *session, const uint8_t *rgba,
                     int width, int height, const pg_render_result *result)
{
    kittyfb_rect rects[PG_DAMAGE_MAX];
    size_t index;

    if (session == NULL || rgba == NULL) return false;
    if (result == NULL || result->full_frame || result->rect_count == 0u) {
        return kittyts_present(session, rgba, width, height);
    }
    /* pg_rect and kittyfb_rect agree on x1/y1-exclusive framebuffer pixels but
     * are deliberately different types: the core may not name a kittyfb type
     * (ARCHITECTURE.md §2.2), so the conversion happens here, in the only file
     * allowed to know both. */
    for (index = 0u; index < result->rect_count &&
                     index < (size_t)PG_DAMAGE_MAX; ++index) {
        rects[index].x0 = result->rects[index].x0;
        rects[index].y0 = result->rects[index].y0;
        rects[index].x1 = result->rects[index].x1;
        rects[index].y1 = result->rects[index].y1;
    }
    return kittyts_present_damage(session, rgba, width, height, rects,
                                  result->rect_count);
}
