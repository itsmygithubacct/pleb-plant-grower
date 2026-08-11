/*
 * Standalone frontend entry point. Not part of the archive kilix-land links:
 * the embed provides its own host, so main() and the terminal session are
 * deliberately excluded from libpleb-plant-grower.a.
 *
 * The dispatch is a table rather than a strcmp chain (IMPLEMENTATION_PLAN.md
 * §5.6). That is not tidiness: a chain accepts `--version --help` by silently
 * ignoring the second argument, and it has nowhere to state an arity, so
 * `--selftest abc def` ran seed 0 for 0 days and exited 0 — a typo'd gate that
 * looks exactly like a pass. Every row now declares how many arguments it
 * takes and every numeric argument is parsed strictly.
 */
#include "pleb_plant_grower.h"

#include "pg_advice.h"
#include "pg_calendar.h"
#include "pg_care.h"
#include "pg_plant.h"
#include "pg_render.h"
#include "pg_save.h"
#include "pg_sim.h"
#include "pg_state.h"
#include "pg_term.h"
#include "pg_time.h"
#include "pg_ui.h"

#include "kilix_game_audio.h"
#include "kilix_game_runtime.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    (void)puts("usage: pleb-plant-grower [--version] [--help]");
    (void)puts("                         [--calendar-test] [--time-test]");
    (void)puts("                         [--care-test] [--rules-test]");
    (void)puts("                         [--advice-test]");
    (void)puts("                         [--selftest [<seed> [<sim-days>]]]");
    (void)puts("                         [--save-test <dir>]");
    (void)puts("                         [--render-test <seed> <dir>]");
    (void)puts("                         [--headless [<frames>]]");
    (void)puts("");
    (void)puts("A realtime houseplant you actually have to look after.");
    (void)puts("Runs standalone, or embedded in kilix-land.");
    (void)puts("");
    (void)puts("  --calendar-test  date arithmetic, the season window and the");
    (void)puts("                   local-hour rules; output is independent of TZ");
    (void)puts("  --time-test      the gap matrix: every clock the policy survives");
    (void)puts("  --care-test      the care model: derived watering intervals, the");
    (void)puts("                   symptom ladders, the cachepot's soggy ticks");
    (void)puts("  --rules-test     verb legality, the four hard refusals and the");
    (void)puts("                   myth blocklist");
    (void)puts("  --selftest       the catch-up engine: the same gap simulated in");
    (void)puts("                   ticks, in one step and in ragged chunks must");
    (void)puts("                   agree, and no gap may cost more than 30 days");
    (void)puts("                   of work. Output is a pure function of its two");
    (void)puts("                   arguments, so two runs compare equal.");
    (void)puts("                   Defaults: seed 1337, 400 days");
    (void)puts("  --save-test      persistence: the record round trips, then every");
    (void)puts("                   single-bit flip, truncation and semantic case is");
    (void)puts("                   either refused or decodes to a record that");
    (void)puts("                   re-encodes to itself — and neither outcome ever");
    (void)puts("                   changes the plant it was handed. <dir> must be an");
    (void)puts("                   absolute path; it is written into and then emptied");
}

/* strtoull returns 0 on garbage, so a bare call cannot tell `0` from `abc`.
 * Reject an empty string, trailing text and a range error -- and a leading
 * sign, because strtoull negates rather than refusing it: `-1` would otherwise
 * be accepted as 18446744073709551615, which is a typo silently becoming a
 * legal seed. */
static bool parse_u64(const char *text, unsigned long long limit,
                      unsigned long long *out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || *text == '\0' || *text == '-' || *text == '+') {
        return false;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || value > limit) {
        return false;
    }
    *out = value;
    return true;
}

static int cmd_version(int extra, char **args)
{
    (void)extra; (void)args;
    (void)printf("pleb-plant-grower %s\n", pg_version());
    return 0;
}

static int cmd_help(int extra, char **args)
{
    (void)extra; (void)args;
    usage();
    return 0;
}

static int cmd_calendar(int extra, char **args)
{
    (void)extra; (void)args;
    return pg_calendar_run_test();
}

static int cmd_time(int extra, char **args)
{
    (void)extra; (void)args;
    return pg_time_run_test();
}

static int cmd_care(int extra, char **args)
{
    (void)extra; (void)args;
    return pg_care_run_test();
}

static int cmd_rules(int extra, char **args)
{
    (void)extra; (void)args;
    return pg_plant_run_rules_test();
}

static int cmd_advice(int extra, char **args)
{
    (void)extra; (void)args;
    return pg_advice_run_test();
}

/* Both arguments are optional and defaulted, so --selftest on its own is still
 * a meaningful gate. Neither may be garbage. */
static int cmd_selftest(int extra, char **args)
{
    unsigned long long seed = 1337ull;
    unsigned long long days = 400ull;

    if (extra >= 1 && !parse_u64(args[0], UINT64_MAX, &seed)) {
        (void)fprintf(stderr, "--selftest: seed '%s' is not a number\n",
                      args[0]);
        return 2;
    }
    if (extra >= 2 && !parse_u64(args[1], UINT32_MAX, &days)) {
        (void)fprintf(stderr,
                      "--selftest: sim-days '%s' is not a number in 0..%u\n",
                      args[1], (unsigned)UINT32_MAX);
        return 2;
    }
    return pg_sim_run_selftest((uint64_t)seed, (uint32_t)days);
}

static int cmd_save_test(int extra, char **args)
{
    (void)extra;
    return pg_save_run_test(args[0]);
}

/* <seed> <dir>: both required, because a render fixture set that silently
 * defaulted its directory would write into the working tree. */
static int cmd_render_test(int extra, char **args)
{
    unsigned long long seed = 0ull;

    (void)extra;
    if (!parse_u64(args[0], UINT64_MAX, &seed)) {
        (void)fprintf(stderr, "--render-test: seed '%s' is not a number\n",
                      args[0]);
        return 2;
    }
    return pg_render_run_test((uint64_t)seed, args[1]);
}

/* ---- the standalone game ------------------------------------------------
 *
 * kilix_game_host_run owns signals, the crash-time tty restore, headless,
 * max_frames, idle_sleep_ns and the SIGTSTP suspend/resume path. Hand-rolling
 * the loop would re-implement five things the kit already gets right, and the
 * clock reset on resume costs nothing because the wall reconciliation below
 * runs every frame anyway.
 */
typedef struct pg_app {
    pg_state state;
    pg_graphics graphics;
    pg_store store;
    pg_settings settings;
    pg_input input;
    ki_td_soft_renderer renderer;
    pg_render_result result;
    int64_t last_advance_wall_s;
    uint64_t tick;
    bool store_open;
    bool graphics_open;
} pg_app;

static bool app_start(kilix_game_host *host, void *user)
{
    pg_app *app = (pg_app *)user;
    char asset_root[1024];
    char error[256];
    pg_catchup_report report;
    bool recovered = false;

    (void)host;
    pg_init(&app->state, 20260811u);

    if (!kilix_game_data_root_from_executable(
            "PLEB_PLANT_ASSETS", "assets",
            "../share/pleb-plant-grower/assets", asset_root,
            sizeof asset_root)) {
        asset_root[0] = '\0';
    }
    app->graphics_open = pg_graphics_init(&app->graphics, asset_root, error,
                                          sizeof error);
    app->store_open = pg_store_open(&app->store, NULL);
    if (app->store_open) {
        (void)pg_settings_load(&app->settings, &app->store);
        (void)pg_store_load(&app->state, &app->store, &recovered);
    }
    /* Reconcile at launch exactly as at any other moment: a cold start, a
     * laptop lid and a midnight rollover all take this same path, so there is
     * one thing to get right rather than three. */
    {
        pg_now now = pg_term_now();
        (void)pg_advance(&app->state, now, &report);
        app->last_advance_wall_s = now.wall_s;
        if (report.entry_count > 0u || report.abandoned) {
            pg_ui_goto(&app->state.ui, (uint8_t)PG_SCREEN_AWAY);
        } else {
            pg_ui_goto(&app->state.ui,
                       pg_ui_entry_screen(&app->state,
                                          pg_store_last_status(&app->store)));
        }
    }
    app->input.pointer_x = PG_POINTER_NONE;
    app->input.pointer_y = PG_POINTER_NONE;
    return true;
}

static void app_event(kilix_game_host *host, void *user,
                      const kittyin_event *event)
{
    pg_app *app = (pg_app *)user;
    kittyts_session *terminal = kilix_game_host_terminal(host);

    pg_term_translate(event, &app->input, &app->result.view,
                      terminal ? kittyts_origin_x(terminal) : 0,
                      terminal ? kittyts_origin_y(terminal) : 0);
}

static bool app_step(kilix_game_host *host, void *user, double step_seconds)
{
    pg_app *app = (pg_app *)user;

    (void)host;
    pg_update(&app->state, &app->input, step_seconds);
    pg_term_input_reset(&app->input);

    /* Biology is driven by the wall clock, never by frames. Sampling
     * CLOCK_REALTIME is a vDSO read and effectively free, and advancing only
     * once a care tick has elapsed keeps the work bounded. */
    {
        pg_now now = pg_term_now();
        if (now.wall_s - app->last_advance_wall_s >= PG_CARE_TICK_SECONDS ||
            now.wall_s < app->last_advance_wall_s) {
            pg_catchup_report report;
            (void)pg_advance(&app->state, now, &report);
            app->last_advance_wall_s = now.wall_s;
        }
    }
    app->tick += 1u;
    return true;
}

static bool app_render(kilix_game_host *host, void *user, double alpha)
{
    pg_app *app = (pg_app *)user;
    kittyts_session *terminal = kilix_game_host_terminal(host);
    int width = 0;
    int height = 0;
    const uint8_t *rgba;

    (void)alpha;
    if (terminal == NULL) return true;
    width = kittyts_width(terminal);
    height = kittyts_height(terminal);
    if (width <= 0 || height <= 0) return true;
    if (ki_td_soft_width(&app->renderer) != width ||
        ki_td_soft_height(&app->renderer) != height) {
        if (!ki_td_soft_renderer_resize(&app->renderer, width, height) &&
            !ki_td_soft_renderer_init(&app->renderer, width, height)) {
            return true;
        }
    }
    /* Nothing here animates yet, so present at ~15 fps rather than 60: the
     * terminal transport is the expensive part of this game and a still frame
     * costs the same to send as a moving one. */
    if ((app->tick & 3u) != 0u) return true;
    if (!pg_render(&app->renderer, &app->state, &app->graphics,
                   &app->result)) {
        return true;
    }
    rgba = ki_td_soft_pack_rgba(&app->renderer);
    if (rgba == NULL) return true;
    (void)pg_term_present(terminal, rgba, width, height, &app->result);
    return true;
}

static void app_stop(kilix_game_host *host, void *user)
{
    pg_app *app = (pg_app *)user;

    (void)host;
    if (app->store_open) {
        (void)pg_store_save(&app->state, &app->store);
        (void)pg_settings_save(&app->settings, &app->store);
        pg_store_close(&app->store);
    }
    if (app->graphics_open) {
        pg_graphics_shutdown(&app->graphics);
    }
    ki_td_soft_renderer_destroy(&app->renderer);
}

static int run_game(bool headless, uint64_t max_frames)
{
    static pg_app app;
    static kilix_game_host host;
    kilix_game_host_options options;
    kilix_game_host_callbacks callbacks;

    memset(&app, 0, sizeof app);
    kilix_game_host_options_init(&options);
    options.terminal.mouse_tracking = KITTYIN_MOUSE_TRACKING_MOTION;
    options.terminal.framebuffer.max_width = 1920;   /* 480 x 4 */
    options.terminal.framebuffer.max_height = 1080;  /* 270 x 4 */
    /* min_width/min_height are deliberately left at the library defaults --
     * see pg_term.h. A hard minimum refuses to start where we would rather
     * degrade. */
    options.clock.step_ns = KILIX_GAME_NANOSECONDS_PER_SECOND / 60;
    options.idle_sleep_ns = 2 * 1000 * 1000;
    options.headless = headless;
    options.max_frames = max_frames;

    memset(&callbacks, 0, sizeof callbacks);
    callbacks.start = app_start;
    callbacks.event = app_event;
    callbacks.step = app_step;
    callbacks.render = app_render;
    callbacks.stop = app_stop;

    return kilix_game_host_run(&host, &options, &callbacks, &app);
}

static int cmd_headless(int extra, char **args)
{
    unsigned long long frames = 120ull;

    if (extra >= 1 && !parse_u64(args[0], UINT32_MAX, &frames)) {
        (void)fprintf(stderr, "--headless: frames '%s' is not a number\n",
                      args[0]);
        return 2;
    }
    return run_game(true, (uint64_t)frames);
}

typedef struct pg_command {
    const char *name;
    int min_extra;
    int max_extra;
    const char *argspec;                  /* for the arity diagnostic */
    int (*run)(int extra, char **args);
} pg_command;

static const pg_command COMMANDS[] = {
    { "--version",       0, 0, "",                        cmd_version },
    { "--help",          0, 0, "",                        cmd_help },
    { "--calendar-test", 0, 0, "",                        cmd_calendar },
    { "--time-test",     0, 0, "",                        cmd_time },
    { "--care-test",     0, 0, "",                        cmd_care },
    { "--rules-test",    0, 0, "",                        cmd_rules },
    { "--advice-test",   0, 0, "",                        cmd_advice },
    { "--selftest",      0, 2, " [<seed> [<sim-days>]]",  cmd_selftest },
    { "--save-test",     1, 1, " <dir>",                  cmd_save_test },
    { "--render-test",   2, 2, " <seed> <dir>",           cmd_render_test },
    { "--headless",      0, 1, " [<frames>]",             cmd_headless },
};

int main(int argc, char **argv)
{
    size_t index;

    if (argc <= 1) {
        return run_game(false, 0u);
    }
    for (index = 0; index < sizeof COMMANDS / sizeof COMMANDS[0]; ++index) {
        const pg_command *command = &COMMANDS[index];
        int extra;

        if (strcmp(argv[1], command->name) != 0) {
            continue;
        }
        extra = argc - 2;
        if (extra < command->min_extra || extra > command->max_extra) {
            (void)fprintf(stderr, "usage: pleb-plant-grower %s%s\n",
                          command->name, command->argspec);
            return 2;
        }
        return command->run(extra, &argv[2]);
    }
    (void)fprintf(stderr, "unknown option: %s\n", argv[1]);
    return 2;
}
