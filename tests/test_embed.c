/*
 * The embed contract, tested the way kilix-land will actually use it.
 *
 * This links **only the archive** — no main.c, no pg_term.c, no terminal
 * session, no clock. That restriction is the whole point: if the archive had
 * quietly grown a dependency on the frontend, every other test would still
 * pass and only a consumer would find out, at link time, in their repository.
 *
 * What a host promises us: a zeroed pg_state, a pg_now it filled itself, and a
 * renderer it owns. What we promise a host: no allocation on the render path,
 * no clock, no I/O outside pg_graphics_* and pg_store_*, no exit, and a
 * pg_state whose size it can check before it embeds one by value.
 */
#include "pleb_plant_grower.h"

/* A host embedding pg_state by value needs its layout, which is what
 * mk/pleb-plant-grower.mk's PG_CPPFLAGS exposes. An embedder that only wants
 * the size uses pg_state_size() and needs nothing from src/. */
#include "pg_state.h"

#include "kilix_top_down_soft.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) do {                                               \
    if (!(condition)) {                                                     \
        (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                      #condition);                                          \
        failures++;                                                         \
    }                                                                       \
} while (0)

/* A host holds pg_state BY VALUE, the way kilix-land holds land_game. If this
 * number moves without the header moving with it, a host compiled against the
 * old header and linked against the new archive corrupts its own stack — which
 * is exactly why pg_state_size() exists rather than sizeof in a header the
 * host might have a stale copy of. */
static void test_abi(void)
{
    CHECK(pg_state_size() == sizeof(pg_state));
    CHECK(pg_state_size() > 0u);
}

static pg_now host_now(int64_t wall_s, int64_t boot_s, int32_t tz_minutes)
{
    pg_now now;
    memset(&now, 0, sizeof now);
    now.wall_s = wall_s;
    now.boot_s = boot_s;
    now.tz_offset_minutes = tz_minutes;
    /* A host that cannot supply a boot id leaves it zeroed; the gap policy
     * handles that explicitly (D-095). */
    return now;
}

static void test_lifecycle(void)
{
    static pg_state state;
    char error[128];
    pg_catchup_report report;

    pg_init(&state, 4242u);
    CHECK(pg_validate(&state, error, sizeof error));
    CHECK(state.plant_count >= 1u);

    /* A first advance establishes the anchor and credits nothing. */
    CHECK(pg_advance(&state, host_now(INT64_C(1813244400), 1000, 0),
                     &report));
    CHECK(report.credited_seconds == 0);

    /* A day later, at the same boot, credits a day. */
    CHECK(pg_advance(&state,
                     host_now(INT64_C(1813244400) + 86400, 1000 + 86400, 0),
                     &report));
    CHECK(report.credited_seconds == 86400);
    CHECK(report.care_ticks == 86400 / PG_CARE_TICK_SECONDS);
    CHECK(pg_validate(&state, error, sizeof error));

    /* Idempotent: the same instant twice credits nothing the second time.
     * A host that calls pg_advance from both its update and its render must
     * not double-age the plant. */
    CHECK(pg_advance(&state,
                     host_now(INT64_C(1813244400) + 86400, 1000 + 86400, 0),
                     &report));
    CHECK(report.credited_seconds == 0);
}

/* The host owns the renderer and the canvas. pg_render must fit its own view
 * into whatever it is given, mutate nothing, and put the clip back. */
static void test_render_against_a_host_renderer(void)
{
    static pg_state state;
    static pg_state before;
    ki_td_soft_renderer renderer = {0};
    pg_render_result result;
    sr_canvas *canvas;
    int saved[4];

    pg_init(&state, 7u);
    (void)pg_advance(&state, host_now(INT64_C(1813244400), 1000, 540), NULL);

    /* Deliberately not a multiple of 480x270: a host's canvas is whatever its
     * terminal is, and the snap has to cope. */
    CHECK(ki_td_soft_renderer_init(&renderer, 1000, 620));
    canvas = ki_td_soft_canvas(&renderer);

    saved[0] = canvas->clip_x0; saved[1] = canvas->clip_y0;
    saved[2] = canvas->clip_x1; saved[3] = canvas->clip_y1;
    canvas->clip_x0 = 12; canvas->clip_y0 = 9;
    canvas->clip_x1 = 700; canvas->clip_y1 = 500;

    before = state;
    CHECK(pg_render(&renderer, &state, NULL, &result));
    CHECK(memcmp(&before, &state, sizeof before) == 0);

    CHECK(canvas->clip_x0 == 12 && canvas->clip_y0 == 9);
    CHECK(canvas->clip_x1 == 700 && canvas->clip_y1 == 500);
    /* An integer scale, never letterboxed. */
    CHECK(result.view.scale == 2.0f);

    /* graphics may be NULL: an embedder that wants no backdrop of ours gets
     * the procedural stage rather than a crash. */
    canvas->clip_x0 = saved[0]; canvas->clip_y0 = saved[1];
    canvas->clip_x1 = saved[2]; canvas->clip_y1 = saved[3];
    CHECK(pg_render(&renderer, &state, NULL, NULL));

    ki_td_soft_renderer_destroy(&renderer);
}

/* Audio is take-and-clear so a host can route our events into its own
 * vocabulary. kilix-land has fourteen semantic events and we have seventeen
 * kinds; the mapping is the host's business, and this asserts only that the
 * queue behaves. */
static void test_audio_handoff(void)
{
    static pg_state state;
    pg_audio_event events[PG_AUDIO_QUEUE_MAX];
    size_t taken;

    pg_init(&state, 11u);
    taken = pg_take_audio_events(&state, events, PG_AUDIO_QUEUE_MAX);
    CHECK(taken == 0u);
    CHECK(pg_music_scene(&state) < 4u);
    /* A host that passes no buffer still drains, which is what an embedder
     * with audio disabled will do. */
    CHECK(pg_take_audio_events(&state, NULL, 0u) == 0u);
}

int main(void)
{
    test_abi();
    test_lifecycle();
    test_render_against_a_host_renderer();
    test_audio_handoff();

    if (failures != 0) {
        (void)fprintf(stderr, "FAIL: %d embed checks\n", failures);
        return 1;
    }
    (void)printf("PASS embed: archive-only link, ABI %zu bytes, catch-up "
                 "idempotent, render pure\n", pg_state_size());
    return 0;
}
