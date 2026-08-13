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
#include "pg_audio.h"
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
    (void)puts("                         [--shot <dir>]");
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

/* ---- audio ---------------------------------------------------------------
 *
 * One cue per pg_audio_event_kind, loaded from the generated bank. require_mixer
 * is false on purpose: a box with no sound sink still plays the game, because
 * pcm-mixer degrades a dead sink to a silent no-op by design. A houseplant must
 * not fail to open because pulseaudio is not running.
 */
#define PG_AUDIO_CUE_COUNT ((uint32_t)PG_AUDIO_KIND_COUNT)

static kilix_game_audio_cue_spec PG_CUE_SPECS[PG_AUDIO_KIND_COUNT];
static char PG_CUE_PATHS[PG_AUDIO_KIND_COUNT][48];

static size_t build_cue_specs(void)
{
    size_t count = 0u;
    uint32_t kind;

    for (kind = 0u; kind < PG_AUDIO_CUE_COUNT; ++kind) {
        const char *name = pg_audio_cue_name((uint8_t)kind);
        if (name == NULL) continue;
        (void)snprintf(PG_CUE_PATHS[count], sizeof PG_CUE_PATHS[count],
                       "sfx/%s.wav", name);
        PG_CUE_SPECS[count].cue = kind;
        PG_CUE_SPECS[count].variant = 0u;
        PG_CUE_SPECS[count].relative_path = PG_CUE_PATHS[count];
        PG_CUE_SPECS[count].gain = 1.0f;
        PG_CUE_SPECS[count].pitch = 1.0f;
        /* Not required: a missing cue is silence, not a refusal to start. */
        PG_CUE_SPECS[count].required = false;
        count += 1u;
    }
    return count;
}

static bool audio_start(kilix_game_audio *audio, const char *asset_root,
                        bool offline, char *error, size_t error_size)
{
    kilix_game_audio_options options;

    kilix_game_audio_options_init(&options);
    options.cue_count = PG_AUDIO_CUE_COUNT;
    options.cues = PG_CUE_SPECS;
    options.cue_spec_count = build_cue_specs();
    options.data.environment_variable = "PLEB_PLANT_ASSETS";
    options.data.local_root = asset_root;
    options.data.installed_root = "../share/pleb-plant-grower/assets";
    options.start_mixer = !offline;
    options.require_mixer = false;
    options.mixer.offline = offline;
    return kilix_game_audio_init(audio, &options, error, error_size);
}

/* --sound-test: the bank loads, every cue resolves, and the whole thing works
 * with no sink at all. Run under `PATH= ` it must still pass, which is the
 * point -- this is the check that audio can never be the reason the game will
 * not start. */
static int cmd_sound_test(int extra, char **args)
{
    static kilix_game_audio audio;
    char asset_root[1024];
    size_t specs;
    uint32_t kind;
    char error[256];
    int failures = 0;

    (void)extra; (void)args;
    if (!kilix_game_data_root_from_executable(
            "PLEB_PLANT_ASSETS", "assets",
            "../share/pleb-plant-grower/assets", asset_root,
            sizeof asset_root)) {
        asset_root[0] = '\0';
    }
    specs = build_cue_specs();
    (void)printf("sound %s\n", pg_version());
    (void)printf("  cues declared      %zu\n", specs);
    if (specs != (size_t)PG_AUDIO_KIND_COUNT) {
        (void)fprintf(stderr, "sound-test: a cue kind has no name\n");
        failures += 1;
    }
    for (kind = 0u; kind < PG_AUDIO_CUE_COUNT; ++kind) {
        if (pg_audio_cue_name((uint8_t)kind) == NULL) {
            (void)fprintf(stderr, "sound-test: kind %u has no cue name\n",
                          (unsigned)kind);
            failures += 1;
        }
    }
    /* Offline: deterministic, silent, and it exercises the same load path. */
    memset(&audio, 0, sizeof audio);
    if (!audio_start(&audio, asset_root, true, error, sizeof error)) {
        (void)fprintf(stderr, "sound-test: audio would not initialise even "
                              "offline (%s) -- audio must never block the "
                              "game\n", error);
        failures += 1;
    } else {
        (void)printf("  cues loaded        %zu\n", audio.loaded_cues);
        (void)printf("  ready              %s\n",
                     kilix_game_audio_is_ready(&audio) ? "yes" : "no");
        for (kind = 0u; kind < PG_AUDIO_CUE_COUNT; ++kind) {
            (void)kilix_game_audio_play(&audio, kind,
                                        KILIX_GAME_AUDIO_BUS_SFX, 1.0f, 1.0f);
        }
        kilix_game_audio_update(&audio, 1.0f / 60.0f);
        kilix_game_audio_shutdown(&audio);
    }
    /* The music scene selection is pure and must always answer. */
    {
        static pg_state state;
        pg_init(&state, 7u);
        if (pg_music_scene(&state) >= (uint32_t)PG_MUSIC_SCENE_COUNT) {
            (void)fprintf(stderr, "sound-test: music scene out of range\n");
            failures += 1;
        }
        if (pg_music_scene(NULL) >= (uint32_t)PG_MUSIC_SCENE_COUNT) {
            (void)fprintf(stderr, "sound-test: NULL state has no scene\n");
            failures += 1;
        }
    }
    /* Take-and-clear: the queue empties and stays empty. */
    {
        static pg_state state;
        pg_audio_event events[8];
        size_t taken;
        pg_init(&state, 8u);
        pg_audio_push(&state, (uint8_t)PG_AUDIO_WATER, 1.0f, 1.0f);
        pg_audio_push(&state, (uint8_t)PG_AUDIO_GROWTH, 1.0f, 1.0f);
        taken = pg_take_audio_events(&state, events, 8u);
        if (taken != 2u) {
            (void)fprintf(stderr, "sound-test: took %zu events, expected 2\n",
                          taken);
            failures += 1;
        }
        if (pg_take_audio_events(&state, events, 8u) != 0u) {
            (void)fprintf(stderr, "sound-test: the queue was not cleared\n");
            failures += 1;
        }
    }
    if (failures != 0) {
        (void)fprintf(stderr, "sound-test: FAILED after %d failures\n",
                      failures);
        return 1;
    }
    (void)puts("sound: PASS");
    return 0;
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
    kilix_game_audio audio;
    pg_render_result result;
    uint32_t music_scene;
    bool audio_open;
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
    /* Audio never blocks the game: require_mixer is false and a failure here
     * is silence, not an exit. */
    app->audio_open = audio_start(&app->audio, asset_root, false, error,
                                  sizeof error);
    app->music_scene = (uint32_t)PG_MUSIC_SCENE_COUNT;
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
    /* Drain the core's events into the mixer. The core never touches a mixer;
     * this is the whole of the boundary. */
    if (app->audio_open) {
        pg_audio_event events[PG_AUDIO_QUEUE_MAX];
        size_t count = pg_take_audio_events(&app->state, events,
                                            (size_t)PG_AUDIO_QUEUE_MAX);
        size_t index;
        uint32_t scene;
        for (index = 0u; index < count; ++index) {
            (void)kilix_game_audio_play(&app->audio, events[index].kind,
                                        KILIX_GAME_AUDIO_BUS_SFX,
                                        events[index].gain,
                                        events[index].pitch);
        }
        scene = pg_music_scene(&app->state);
        if (scene != app->music_scene) {
            app->music_scene = scene;
        }
        kilix_game_audio_update(&app->audio, (float)step_seconds);
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
    if (app->audio_open) {
        kilix_game_audio_shutdown(&app->audio);
    }
    if (app->graphics_open) {
        pg_graphics_shutdown(&app->graphics);
    }
    ki_td_soft_renderer_destroy(&app->renderer);
}


/* RGBA framebuffer to binary PPM. Local rather than borrowed from the test
 * kit: --shot ships in the game binary, and a release path should not depend
 * on a test-only header. */
static bool shot_write_ppm(const char *path, const uint8_t *rgba,
                           int width, int height)
{
    FILE *file;
    int y;

    if (path == NULL || rgba == NULL || width <= 0 || height <= 0) {
        return false;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    (void)fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (y = 0; y < height; ++y) {
        int x;
        for (x = 0; x < width; ++x) {
            const uint8_t *px = rgba + ((size_t)y * (size_t)width
                                        + (size_t)x) * 4u;
            (void)fwrite(px, 1u, 3u, file);
        }
    }
    return fclose(file) == 0;
}

/* --shot: render one frame per scene through the REAL asset root and write it
 * out. This exists because --render-test cannot answer the question. That
 * suite is hermetic on purpose -- it builds synthetic atlases so its 119
 * fixtures hash the same on any machine -- with the consequence that nothing
 * in the build ever drew the art the game actually ships. The suite hash was
 * identical before and after six backdrop plates entered the tree, which is
 * exactly the blind spot this closes. Determinism stays with --render-test;
 * this is the eyes-on path. */
static int cmd_shot(int extra, char **args)
{
    static pg_app app;
    char asset_root[1024];
    char error[256];
    size_t scenes;
    size_t index;
    int written = 0;

    (void)extra;
    if (args[0][0] != '/') {
        (void)fprintf(stderr, "--shot: <dir> must be an absolute path\n");
        return 2;
    }
    memset(&app, 0, sizeof app);
    pg_init(&app.state, 20260811u);
    if (!kilix_game_data_root_from_executable(
            "PLEB_PLANT_ASSETS", "assets",
            "../share/pleb-plant-grower/assets", asset_root,
            sizeof asset_root)) {
        asset_root[0] = '\0';
    }
    if (!pg_graphics_init(&app.graphics, asset_root, error, sizeof error)) {
        (void)fprintf(stderr, "--shot: graphics: %s\n", error);
        return 1;
    }
    if (!ki_td_soft_renderer_init(&app.renderer, PG_LOGICAL_WIDTH * 2,
                                  PG_LOGICAL_HEIGHT * 2)) {
        (void)fprintf(stderr, "--shot: renderer\n");
        pg_graphics_shutdown(&app.graphics);
        return 1;
    }
    /* Say which overlay sheets loaded. They are indexed by their own
     * meaning rather than by (stage, health), so a silent miss would look
     * like a plant that simply has no bloom yet. */
    {
        static const char *const names[PG_OVERLAY_COUNT] = {
            "spathe", "vines", "calathea-night", "pots"
        };
        size_t which;
        for (which = 0u; which < (size_t)PG_OVERLAY_COUNT; ++which) {
            (void)printf("overlay %-15s %s\n", names[which],
                         app.graphics.overlay_valid[which] ? "loaded"
                                                           : "ABSENT");
        }
    }
    /* Force the three overlay conditions and shoot each one.
     *
     * A fresh save is a young pothos in daylight, so the spathe, the vine and
     * the night pose never fire and none of them had been seen drawing. That
     * mattered: the helper they share shipped without carrying the region's
     * stride, which drew every one of them as a smeared band, and the bug was
     * only caught because the pot happened to use the same helper and the pot
     * was on screen. "The atlas loaded" is not evidence that it draws. */
    {
        static const struct {
            const char *name;
            uint8_t species;
            uint8_t stage;
            uint8_t spathe;
            bool night;
        } STATES[] = {
            { "state-vines",    (uint8_t)PG_SPECIES_POTHOS,     4u, 0u, false },
            { "state-spathe",   (uint8_t)PG_SPECIES_PEACE_LILY, 3u,
              (uint8_t)PG_SPATHE_OPEN, false },
            { "state-night",    (uint8_t)PG_SPECIES_CALATHEA,   3u, 0u, true  }
        };
        size_t which;

        (void)pg_graphics_set_scene(&app.graphics, "plain-studio");
        for (which = 0u; which < sizeof STATES / sizeof STATES[0]; ++which) {
            pg_plant *plant = &app.state.plants[0];
            char path[1200];
            const uint8_t *rgba;

            plant->species_id = STATES[which].species;
            plant->growth_stage = STATES[which].stage;
            plant->spathe_state = STATES[which].spathe;
            /* Walk the clock forward an hour at a time until the sim itself
             * says it is night, rather than computing an offset and trusting
             * it -- the first attempt subtracted its way to UTC noon and shot
             * the day pose, which looked plausible enough to nearly pass. */
            if (STATES[which].night) {
                int tries;
                for (tries = 0; tries < 24; ++tries) {
                    if (!pg_sim_env_now(&app.state, 0u).is_daylight) break;
                    app.state.anchor.last_wall_s += 3600;
                }
                if (pg_sim_env_now(&app.state, 0u).is_daylight) {
                    (void)fprintf(stderr,
                                  "--shot: could not reach night\n");
                    continue;
                }
            }
            if (!pg_render(&app.renderer, &app.state, &app.graphics,
                           &app.result)) {
                continue;
            }
            rgba = ki_td_soft_pack_rgba(&app.renderer);
            if (rgba == NULL) continue;
            if (snprintf(path, sizeof path, "%s/%s.ppm", args[0],
                         STATES[which].name) >= (int)sizeof path) continue;
            if (shot_write_ppm(path, rgba, ki_td_soft_width(&app.renderer),
                               ki_td_soft_height(&app.renderer))) {
                written += 1;
                (void)printf("shot %s\n", STATES[which].name);
            }
        }
        pg_init(&app.state, 20260811u);   /* leave the state as we found it */
    }

    scenes = pg_graphics_scene_count(&app.graphics);
    for (index = 0u; index <= scenes; ++index) {
        /* index == scenes is the scene-off case, which the owner's brief
         * called for explicitly: the backdrop must be disable-able. */
        const char *id = index < scenes
                       ? pg_graphics_scene_id(&app.graphics, index) : NULL;
        char path[1200];
        const uint8_t *rgba;

        if (!pg_graphics_set_scene(&app.graphics, id) && id != NULL) {
            (void)fprintf(stderr, "--shot: scene %s did not load\n", id);
            continue;
        }
        if (!pg_render(&app.renderer, &app.state, &app.graphics,
                       &app.result)) {
            (void)fprintf(stderr, "--shot: render failed for %s\n",
                          id != NULL ? id : "scene-off");
            continue;
        }
        rgba = ki_td_soft_pack_rgba(&app.renderer);
        if (rgba == NULL) {
            continue;
        }
        if (snprintf(path, sizeof path, "%s/%s.ppm", args[0],
                     id != NULL ? id : "scene-off") >= (int)sizeof path) {
            continue;
        }
        if (shot_write_ppm(path, rgba, ki_td_soft_width(&app.renderer),
                           ki_td_soft_height(&app.renderer))) {
            written += 1;
            (void)printf("shot %s\n", id != NULL ? id : "scene-off");
        }
    }
    ki_td_soft_renderer_destroy(&app.renderer);
    pg_graphics_shutdown(&app.graphics);
    (void)printf("--shot: %d frames from %s\n", written,
                 asset_root[0] != '\0' ? asset_root : "(no asset root)");
    return written > 0 ? 0 : 1;
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
    { "--shot",          1, 1, " <dir>",                  cmd_shot },
    { "--sound-test",    0, 0, "",                        cmd_sound_test },
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
