/*
 * pg_state — the concrete pg_state layout and the internal enums that go with
 * it (ARCHITECTURE.md §8).
 *
 * The struct below is ARCHITECTURE.md §6.2's HDR section field for field,
 * followed by the PLNT array and the JRNL ring, so the encoder that arrives
 * with Milestone 5 has exactly one struct to walk and the catch-up engine has
 * exactly one struct to snapshot. Nothing here is live-only: every field is
 * either persisted or, where marked, deliberately transient host plumbing that
 * the save format does not carry (the audio queue, the sticky save_failed
 * flag).
 *
 * Two notes on the correspondence with §6.2, both deliberate:
 *
 *   - `care_seconds_total` and `care_residual_s` live in a pg_care_clock, and
 *     the five anchor fields live in a pg_time_anchor, because pg_time.c owns
 *     the policy that reads and writes them and it must be testable without a
 *     pg_state. The encoded field order is unchanged.
 *   - `weather_seed` is an HDR u64 alongside `rng_state`, not a replacement
 *     for it. Weather is required to be a pure function of (day ordinal, seed)
 *     so that a replayed catch-up produces the same July it produced the first
 *     time (pg_rng.h, GAME_DESIGN.md §8.2); `rng_state` is the *stream*, whose
 *     whole purpose is to advance. One number cannot be both.
 */
#ifndef PG_STATE_H
#define PG_STATE_H

#include "pleb_plant_grower.h"

#include "pg_plant.h"
#include "pg_rng.h"
#include "pg_scene.h"
#include "pg_time.h"
#include "pg_ui.h"

#include "kilix_assets.h"

/* pg_store holds its two generations and its settings record by value, so the
 * storage type must be complete here. Nothing else in the core includes it. */
#include "kilix_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Audio is take-and-clear and the host owns the mixer, so the queue is a
 * bounded ring the core writes and the frontend drains (ARCHITECTURE.md §7.1).
 * It is not persisted: a sound that was not heard before the lid closed is not
 * a fact about the plant. */
#define PG_AUDIO_QUEUE_MAX 32

/* What one journal entry means. The numbers are persisted inside the JRNL ring
 * and are therefore append-only for the same reason the content ids are
 * (D-044).
 *
 * `detail` is read against the kind: for an entry the fine tier produced it is
 * the local hour, 0..23, which is what lets the away report say "Wednesday
 * 14:00 — collapsed"; for a day summary it is the 0..5 health index. */
typedef enum pg_journal_kind {
    PG_JOURNAL_NONE = 0,
    PG_JOURNAL_THIRSTY = 1,
    PG_JOURNAL_COLLAPSED = 2,
    PG_JOURNAL_PERKED_UP = 3,
    PG_JOURNAL_SOIL_STAYED_WET = 4,
    PG_JOURNAL_ROOTS_SUFFERED = 5,
    PG_JOURNAL_SOAK_FINISHED = 6,
    PG_JOURNAL_FLUSH_FINISHED = 7,
    PG_JOURNAL_NEW_LEAF = 8,
    PG_JOURNAL_GREW = 9,
    PG_JOURNAL_SPATHE = 10,
    PG_JOURNAL_RAIN_JUG_FILLED = 11,
    PG_JOURNAL_DAY_SUMMARY = 12,
    PG_JOURNAL_WENT_DORMANT = 13,
    PG_JOURNAL_WOKE_UP = 14,
    PG_JOURNAL_AILING = 15,
    PG_JOURNAL_TERMINAL = 16,
    PG_JOURNAL_DIED = 17,
    PG_JOURNAL_LEFT_ALONE = 18,
    PG_JOURNAL_CLOCK_UNSETTLED = 19,
    PG_JOURNAL_WATERED = 20,
    PG_JOURNAL_FED = 21,
    PG_JOURNAL_REPOTTED = 22,
    PG_JOURNAL_MOVED = 23,
    PG_JOURNAL_KIND_COUNT = 24
} pg_journal_kind;

/* HDR, then PLNT, then JRNL. Held by value by the embedder; no allocator is
 * involved anywhere in the core (ARCHITECTURE.md §2.3). */
struct pg_state {
    /* ---- HDR ---- */
    uint64_t save_sequence;
    pg_care_clock clock;            /* care_seconds_total + care_residual_s   */
    pg_time_anchor anchor;          /* last_wall_s/last_boot_s/boot_id/tz     */
    int64_t first_run_wall_s;       /* the calendar origin of this save       */
    uint16_t clock_anomaly_count;
    pg_rng rng;                     /* rng_state: the persisted stream        */
    uint64_t weather_seed;          /* the stateless weather coordinate seed  */
    uint8_t hemisphere;             /* pg_hemisphere (D-075)                  */
    uint8_t time_scale;             /* 1 = realtime                           */
    uint8_t plant_count;            /* 1, or 2 after the 30-care-day unlock   */

    /* ---- PLNT ---- */
    pg_plant plants[PG_PLANT_MAX];

    /* ---- JRNL ---- */
    uint8_t journal_head;
    uint8_t journal_count;          /* entries actually written, <= the ring  */
    pg_journal_entry journal[PG_JOURNAL_RING];

    /* ---- not persisted ---- */
    uint8_t audio_head, audio_count;
    pg_audio_event audio[PG_AUDIO_QUEUE_MAX];
    bool save_failed;               /* sticky; the renderer shows it          */
    bool replant_offered;           /* a death opened the replant flow        */
    /* Which screen you are looking at. Live-only and deliberately not in the
     * save record: a screen is not a fact about the plant, and restoring
     * somebody into a modal they had already dismissed is worse than putting
     * them back in front of the plant (pg_ui.h). */
    pg_ui_state ui;
};

/* ---- settings: standalone only, and deliberately fail-soft --------------
 *
 * Persistence can never prevent play, so `pg_settings_load` writes defaults
 * first and returns success on every failure path (ARCHITECTURE.md §6.1). That
 * is exactly why `hemisphere` and `time_scale` are NOT here: they are
 * simulation inputs, and a silently lost hemisphere would rewrite a plant's
 * entire seasonal history with no error (D-078).
 *
 * The gains are on the level scale rather than in floats. The codec has no
 * `f32` — `kilixstate_write_f32` was proposed in ARCHITECTURE.md §11.5 and has
 * not landed — and this milestone does not add one, so a gain is stored the
 * way every other quantity in this game is stored (D-083) and converted to a
 * float once, at the mixer boundary, by the audio milestone. The round trip is
 * then exact, which a float never quite is. */
typedef struct pg_settings {
    uint16_t master_gain_l;      /* 0..PG_LEVEL_MAX */
    uint16_t music_gain_l;
    uint16_t sfx_gain_l;
    bool faces_enabled;          /* the procedural eyes (D-052) */
    bool backdrops_enabled;      /* disabled is a designed look, not a fault */
    uint8_t backdrop_scene_id;   /* the cycled scene, a view preference only:
                                  * the plant's own scene_id lives in PLNT */
} pg_settings;

/* ---- the durable store --------------------------------------------------
 *
 * `plant-a.state` / `plant-b.state` are a generation pair: a save always goes
 * to the generation that is not currently live, so a crash mid-write costs the
 * newer record and never the plant. `settings.state` is a third record and is
 * fail-soft on every path. */
typedef enum pg_store_status {
    PG_STORE_UNOPENED = 0,
    PG_STORE_READY = 1,          /* a record loaded or saved cleanly          */
    PG_STORE_FIRST_RUN = 2,      /* both generations absent: a new plant      */
    PG_STORE_RECOVERED = 3,      /* one generation was unreadable, one wasn't */
    PG_STORE_DAMAGED = 4,        /* both unreadable: never silently wipe      */
    PG_STORE_NEWER_VERSION = 5,  /* tended by a newer version: read-only      */
    PG_STORE_CONFLICT = 6,       /* another window is tending this plant      */
    PG_STORE_IO_ERROR = 7,       /* disk full, read-only home: keep playing   */
    PG_STORE_UNAVAILABLE = 8,    /* hostile path, or never opened             */
    PG_STORE_STATUS_COUNT = 9
} pg_store_status;

#define PG_STORE_GENERATION_NONE 0xffu

struct pg_store {
    kilixstate_store plant[2];
    kilixstate_store settings;
    bool plant_ready;
    bool settings_ready;
    uint8_t generation;          /* the generation last read or written      */
    uint8_t damaged_generation;  /* where the next write must go, if any     */
    uint64_t sequence;           /* the highest save_sequence we have seen   */
    uint8_t status;              /* pg_store_status                          */
    bool read_only;              /* a newer version exists: never write      */
    bool save_failed;            /* sticky; cleared only by a save that lands */
    /* The encode/decode scratch buffer. It lives here because the core never
     * allocates and a 4 KiB automatic in a frontend callback is a needless
     * stack risk (ARCHITECTURE.md §2.3). */
    uint8_t scratch[4096];
};

/* One line per kind, in the plant's own register: what happened, when, and
 * nothing else. No adjectives, no blame, and never "your plant missed you" —
 * guilt copy is banned by design (GAME_DESIGN.md §10.4). The away report and
 * the post-mortem are both built from these. */
const char *pg_journal_kind_line(uint8_t kind);

/* Whether this kind's `detail` byte is a local hour (the fine tier's "Wednesday
 * 14:00") or a quantity that belongs to the kind (a health index, a leaf
 * count, a growth stage). A reader that does not ask will print a health index
 * as a time of day. */
bool pg_journal_detail_is_hour(uint8_t kind);

/* ---- graphics ------------------------------------------------------------
 *
 * The one place in the game that touches the filesystem outside pg_store
 * (ARCHITECTURE.md §2.2 rule 6). Held by value like everything else: the
 * plate's pixels are owned by kilix-assets, and nothing here is persisted.
 *
 * `scene` is an index into `scenes`, or PG_SCENE_NONE for background-off --
 * which is the procedural stage of §4.3, NOT plain-studio. Those are two
 * different looks and both ship. */
#define PG_SCENE_NONE 0xffu

/* Sheets that overlay a plant body rather than replace it. Kept separate from
 * the species atlases because they are indexed by their own meaning, not by
 * (growth stage, health): the spathe by bloom stage, the vine by segment, the
 * night pose by growth stage alone. */
typedef enum pg_overlay_atlas {
    PG_OVERLAY_SPATHE = 0,
    PG_OVERLAY_VINES = 1,
    PG_OVERLAY_CALATHEA_NIGHT = 2,
    PG_OVERLAY_COUNT = 3
} pg_overlay_atlas;

struct pg_graphics {
    pg_scene_desc scenes[PG_SCENE_COUNT];
    uint8_t scene_count;
    uint8_t scene;                  /* index, or PG_SCENE_NONE = off */
    uint8_t loaded_count;           /* plates that resolved AND passed the gate */
    bool full_frame_pending;        /* a scene swap must repaint everything */
    kilix_asset_limits limits;
    kilix_asset_locator locator;
    char asset_root[512];
    kilix_asset_image plate;        /* the current scene's backdrop */
    kilix_asset_image front;        /* its optional RGBA occluder */
    bool plate_valid, front_valid;

    /* One atlas per species: 4 growth columns x 6 health rows of 160 px
     * cells. Growth stage picks the column, health picks the row
     * (ARCHITECTURE.md §3.3), which is the coarse silhouette channel the
     * colour ramp then interpolates between.
     *
     * Absent atlases are not an error: the renderer draws the procedural
     * plant instead, which is what keeps the build playable while art is
     * outstanding and is why art latency was never on the critical path. */
    kilix_asset_image plant_atlas[PG_SPECIES_COUNT];
    kilix_asset_atlas plant_grid[PG_SPECIES_COUNT];
    bool plant_atlas_valid[PG_SPECIES_COUNT];
    uint8_t plant_atlas_count;

    /* Three sheets that are not a species body: the peace lily's spathe, the
     * pothos trailer tiled downward, and the calathea's folded night pose.
     * Same rule as the bodies -- a missing one falls back to the procedural
     * draw rather than failing, so the game still opens with no art. */
    kilix_asset_image overlay_atlas[PG_OVERLAY_COUNT];
    kilix_asset_atlas overlay_grid[PG_OVERLAY_COUNT];
    bool overlay_valid[PG_OVERLAY_COUNT];
};

/* The event ring. Writing is append-only and wraps; reading hands back the
 * most recent entries oldest-first, which is the order the away report and the
 * post-mortem both want. */
void pg_journal_push(pg_state *state, uint32_t care_day, uint8_t kind,
                     uint8_t detail, uint8_t plant_index);
size_t pg_journal_recent(const pg_state *state, pg_journal_entry *out,
                         size_t capacity);

/* Take-and-clear audio. The core never touches a mixer. */
void pg_audio_push(pg_state *state, uint8_t kind, float gain, float pitch);

#endif /* PG_STATE_H */
