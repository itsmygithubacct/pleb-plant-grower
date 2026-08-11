/*
 * pg_audio — see pg_audio.h.
 */
#include "pg_audio.h"

#include "pg_care.h"
#include "pg_plant.h"
#include "pg_sim.h"
#include "pg_state.h"

#include <string.h>

static const char *const CUE_NAMES[PG_AUDIO_KIND_COUNT] = {
    "water", "mist", "feed", "snip", "pot_set", "rotate", "drain",
    "soil_press", "ui_move", "ui_accept", "ui_reject", "growth", "day_roll",
    "wilt", "rain", "spathe", "fold"
};

const char *pg_audio_cue_name(uint8_t kind)
{
    return kind < (uint8_t)PG_AUDIO_KIND_COUNT ? CUE_NAMES[kind] : NULL;
}

/* pg_audio_push lives in pg_actions.c, next to the verbs that emit the
 * sounds. Keeping the writer beside its callers and the drain beside the
 * music selection is the split that reads best; duplicating it here was the
 * obvious mistake and the linker caught it immediately. */

size_t pg_take_audio_events(pg_state *state, pg_audio_event *out,
                            size_t capacity)
{
    size_t written = 0u;

    if (state == NULL) {
        return 0u;
    }
    while (state->audio_count > 0u && written < capacity) {
        if (out != NULL) {
            out[written] = state->audio[state->audio_head];
        }
        state->audio_head =
            (uint8_t)(((unsigned)state->audio_head + 1u) %
                      (unsigned)PG_AUDIO_QUEUE_MAX);
        state->audio_count = (uint8_t)(state->audio_count - 1u);
        written += 1u;
    }
    /* Take-and-clear: anything that did not fit is discarded rather than kept
     * for next frame. A backlog of stale sounds is worse than silence. */
    state->audio_head = 0u;
    state->audio_count = 0u;
    return written;
}

uint32_t pg_music_scene(const pg_state *state)
{
    const pg_plant *plant;
    pg_care_env env;
    uint8_t health;

    if (state == NULL || state->plant_count == 0u) {
        return (uint32_t)PG_MUSIC_GROWING_DAY;
    }
    plant = &state->plants[0];
    env = pg_sim_env_now(state, 0u);
    health = pg_plant_health(plant);

    /* Stress wins over everything: if the plant is in trouble the music says
     * so whatever the season, because that is the one time the player needs
     * to be told something by the room rather than by a panel. */
    if (health >= (uint8_t)PG_HEALTH_DISTRESSED) {
        return (uint32_t)PG_MUSIC_STRESSED;
    }
    if (!env.is_daylight) {
        return (uint32_t)PG_MUSIC_NIGHT;
    }
    if (!env.is_growing_season) {
        return (uint32_t)PG_MUSIC_DORMANT;
    }
    return (uint32_t)PG_MUSIC_GROWING_DAY;
}
