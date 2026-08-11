/*
 * pg_audio — the core's side of sound, which is deliberately very little.
 *
 * The core never touches a mixer. It pushes pg_audio_event records into a
 * bounded ring and the frontend drains them; the standalone build maps them to
 * cues on the kit's mixer, and kilix-land maps the same events into its own
 * vocabulary. That asymmetry is the whole reason the API is take-and-clear:
 * one archive, two hosts, no #ifdef.
 *
 * Nothing here is persisted. A sound that was not heard before the lid closed
 * is not a fact about the plant.
 */
#ifndef PG_AUDIO_H
#define PG_AUDIO_H

#include "pleb_plant_grower.h"

#include <stdbool.h>
#include <stdint.h>

/* Music scenes, chosen by season and health. Returned by pg_music_scene. */
typedef enum pg_music_scene {
    PG_MUSIC_GROWING_DAY = 0,
    PG_MUSIC_NIGHT = 1,
    PG_MUSIC_DORMANT = 2,
    PG_MUSIC_STRESSED = 3,
    PG_MUSIC_SCENE_COUNT = 4
} pg_music_scene_id;

/* The cue name for an event kind, matching tools/gen_sfx.py's file names.
 * NULL for an unknown kind rather than a fallback: a silent unknown cue is a
 * missing sound, and a wrong one is a bug that sounds intentional. */
const char *pg_audio_cue_name(uint8_t kind);

#endif /* PG_AUDIO_H */
