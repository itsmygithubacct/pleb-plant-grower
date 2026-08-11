/*
 * pleb-plant-grower — the one public header.
 *
 * Everything kilix-land needs to embed the game, and everything the standalone
 * frontend needs to drive it, is declared here. Internal structure lives in
 * src/pg_*.h and is not installed.
 *
 * The surface below is ARCHITECTURE.md §2.3 in full. It is written out once,
 * at Milestone 2, so that no later milestone has to reopen this file: the
 * constants, the stable-id enumerations that are compiled into saved state,
 * the injected clock, the semantic input record, and the lifecycle,
 * simulation, graphics, render, audio and persistence entry points.
 *
 * Functions declared here that a later milestone implements are marked with
 * the milestone that owns them. A declaration without a definition is a
 * contract, not a stub: the archive is linked whole, so an unimplemented
 * entry point is a link error at the moment somebody calls it.
 */
#ifndef PLEB_PLANT_GROWER_H
#define PLEB_PLANT_GROWER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ki_td_view is held by value in pg_render_result and ki_td_soft_renderer is
 * the render target, so both types must be complete here. */
#include "kilix_top_down_soft.h"
#include "kilix_top_down_view.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- identity and the fixed dimensions of the world ---------- */

#define PG_VERSION "0.1.0"

/* Logical stage. Rooms are authored in this space and the framebuffer snaps to
 * an exact integer multiple of it (ARCHITECTURE.md §3.1). */
#define PG_LOGICAL_WIDTH  480
#define PG_LOGICAL_HEIGHT 270

#define PG_TICK_HZ 60

/* Biology quantises to a care tick; catch-up is bounded by the credit cap, and
 * the same cap is the abandonment rule (ARCHITECTURE.md §5.4). */
#define PG_CARE_TICK_SECONDS 900
#define PG_GAP_CREDIT_MAX_SECONDS (INT64_C(30) * 24 * 3600)

/* Solar noon is modelled at 13:00 local (D-080). Authored as
 * `solar_noon_local_minutes` in content/seasons.json; restated here because
 * every local-hour rule in the game hangs off it. */
#define PG_SOLAR_NOON_LOCAL_MINUTES 780

/* Fixed point, and there is only one convention (D-083). `_l` levels run
 * 0..10000, `_q12` multipliers have 4096 = x1.0, `_c` is centi-units of a
 * named physical quantity. No float ever enters the plant record. */
#define PG_LEVEL_MAX 10000
#define PG_Q12_ONE   4096

/* Exactly four of each at first release; six scenes and six spots. */
#define PG_SPECIES_COUNT 4
#define PG_POT_COUNT     4
#define PG_SCENE_COUNT   6
#define PG_SPOT_COUNT    6

/* Capacities that cross the embed boundary. The persisted counterparts are
 * ARCHITECTURE.md §6.2. */
#define PG_PLANT_MAX           2   /* the second shelf slot unlocks at 30 care-days (D-014) */
#define PG_NAME_BYTES         24   /* UTF-8, NUL-padded (D-018) */
#define PG_BOOT_ID_BYTES      16
#define PG_LEAF_MAX           12
#define PG_VINE_MAX           24
#define PG_DLI_RING           14
#define PG_JOURNAL_RING       48   /* the persisted event ring */
#define PG_JOURNAL_REPORT_MAX 32   /* display cap for one "while you were away" report */
#define PG_DAMAGE_MAX         16   /* damage rectangles carried out of one render */

/* No pointer this frame. Chosen outside the logical stage in both axes. */
#define PG_POINTER_NONE (-32768)

/* ---------- stable ids: these numbers are compiled into saved state ---------
 * Append at the end, never move an existing number (D-044, D-081). The
 * authority is content/id-ledger.json and the Milestone 0 gate that diffs it.
 */

typedef enum pg_species_id {
    PG_SPECIES_POTHOS = 0,
    PG_SPECIES_SNAKE = 1,
    PG_SPECIES_PEACE_LILY = 2,
    PG_SPECIES_CALATHEA = 3
} pg_species_id;

typedef enum pg_pot_id {
    PG_POT_TERRACOTTA = 0,
    PG_POT_GLAZED = 1,
    PG_POT_NURSERY = 2,
    PG_POT_CACHEPOT = 3
} pg_pot_id;

/* ART_BIBLE.md §4.5 order. Cosmetic: a scene never reaches the simulation
 * (D-041). */
typedef enum pg_scene_id {
    PG_SCENE_SUNNY_SILL = 0,
    PG_SCENE_BRIGHT_CORNER = 1,
    PG_SCENE_STUDY_DESK = 2,
    PG_SCENE_PLAIN_STUDIO = 3,
    PG_SCENE_KITCHEN_SHELF = 4,
    PG_SCENE_STEAMY_BATH = 5
} pg_scene_id;

/* GAME_DESIGN.md §8.4 order. The spot IS the simulation object. */
typedef enum pg_spot_id {
    PG_SPOT_SILL_SOUTH = 0,
    PG_SPOT_ONE_METRE = 1,
    PG_SPOT_TWO_METRE = 2,
    PG_SPOT_NORTH_SHELF = 3,
    PG_SPOT_RADIATOR_SHELF = 4,
    PG_SPOT_BATH_SHELF = 5
} pg_spot_id;

/* The two defaults deliberately differ and no code may assume
 * spot_id == scene_id (D-081). */
#define PG_SCENE_DEFAULT PG_SCENE_PLAIN_STUDIO
#define PG_SPOT_DEFAULT  PG_SPOT_ONE_METRE

/* Growth stage picks the atlas column, health state picks the row
 * (ARCHITECTURE.md §3.3). */
typedef enum pg_growth_stage {
    PG_STAGE_ESTABLISHING = 0,
    PG_STAGE_GROWING = 1,
    PG_STAGE_MATURE = 2,
    PG_STAGE_SPECIMEN = 3,
    PG_STAGE_COUNT = 4
} pg_growth_stage;

typedef enum pg_health_state {
    PG_HEALTH_THRIVING = 0,
    PG_HEALTH_HEALTHY = 1,
    PG_HEALTH_THIRSTY = 2,
    PG_HEALTH_DISTRESSED = 3,
    PG_HEALTH_CRITICAL = 4,
    PG_HEALTH_DEAD = 5,
    PG_HEALTH_COUNT = 6
} pg_health_state;

/* Persisted bit order for a leaf's damage_mask (GAME_DESIGN.md §4.3,
 * ART_BIBLE.md §4.4). Bits are set and never cleared, and the order is
 * append-only for the same reason the ids above are. */
typedef enum pg_damage_bit {
    PG_DAMAGE_TIP_CRISP = 1u << 0,
    PG_DAMAGE_MARGIN_BROWN = 1u << 1,
    PG_DAMAGE_BLEACH = 1u << 2,
    PG_DAMAGE_STIPPLE = 1u << 3,
    PG_DAMAGE_ROT_BLOTCH = 1u << 4,
    PG_DAMAGE_YELLOW = 1u << 5,
    PG_DAMAGE_TRIMMED = 1u << 6
} pg_damage_bit;

typedef enum pg_hemisphere {
    PG_HEMISPHERE_NORTHERN = 0,
    PG_HEMISPHERE_SOUTHERN = 1
} pg_hemisphere;

/* ---------- injected clock (the time oracle) ---------- */

/* Nothing in the core reads a clock. Only the frontend fills one of these
 * (ARCHITECTURE.md §5.1). */
typedef struct pg_now {
    int64_t wall_s;              /* CLOCK_REALTIME seconds, UTC */
    int64_t boot_s;              /* CLOCK_BOOTTIME seconds */
    uint8_t boot_id[PG_BOOT_ID_BYTES]; /* kernel boot id, or all zeroes */
    int32_t tz_offset_minutes;   /* host-derived local offset in minutes; a
                                  * SIMULATION input (D-079). Refilled on every
                                  * pg_now, clamped to [-720,+840]. */
} pg_now;

/* The clamp the decoder and every consumer apply to tz_offset_minutes. */
#define PG_TZ_OFFSET_MIN (-720)
#define PG_TZ_OFFSET_MAX (840)

/* ---------- semantic input; never raw key events ---------- */

typedef struct pg_input {
    int  move_x, move_y;         /* -1/0/+1 cursor */
    int  menu_delta;             /* wheel / page */
    int  pointer_x, pointer_y;   /* logical coords, or PG_POINTER_NONE */
    bool pointer_pressed;
    bool confirm, cancel, help;
    bool check_soil, lift_pot, water, feed, mist, rotate, wipe, trim, repot,
         drain, sleeve;
    /* drain  = empty the saucer / tip standing water out of the cachepot.
     * sleeve = lift the plant out of the cachepot, or put it back.
     * Every other verb in GAME_DESIGN.md §6 (a sip, bottom-soak, flush, water
     * source, feed strength, move, cutting, treat pests, look closely, read
     * label) is reached through the action list plus `confirm`, not through a
     * dedicated boolean, so the struct never grows one verb at a time (D-087). */
    bool toggle_calendar, toggle_journal, toggle_label;
} pg_input;

/* ---------- the concrete types the embedder holds ----------
 *
 * pg_state is a concrete struct, not an opaque handle: kilix-land embeds it by
 * value the way it holds land_game, and no allocator is involved
 * (ARCHITECTURE.md §2.3). Its layout, and the layouts of pg_graphics,
 * pg_settings and pg_store, are defined in src/pg_state.h (ARCHITECTURE.md §8)
 * — the file plan puts them there, so this header names the types and the
 * definitions arrive with the milestone that owns each of them. An embedder
 * that needs the size rather than the layout uses pg_state_size().
 */
typedef struct pg_state pg_state;
typedef struct pg_graphics pg_graphics;
typedef struct pg_settings pg_settings;
typedef struct pg_store pg_store;

/* One past-day mark in the persisted event ring, and the unit of a
 * "while you were away" report. */
typedef struct pg_journal_entry {
    uint32_t care_day;
    uint8_t kind;
    uint8_t detail;
    uint8_t plant_index;
} pg_journal_entry;

/* ---------- lifecycle ---------- */

void pg_init(pg_state *s, uint64_t seed);                          /* M3 */
bool pg_validate(const pg_state *s, char *error, size_t error_size); /* M3 */
size_t pg_state_size(void);        /* ABI cross-check for the embedder */ /* M3 */
const char *pg_version(void);

/* ---------- simulation ---------- */

/* Animation, UI, input. Advances no biology. Called once per 60 Hz step. */
void pg_update(pg_state *s, const pg_input *in, double step_seconds); /* M8 */

/* Biology. Pure function of (s, now). Idempotent: calling twice with the same
 * `now` credits nothing the second time. Transactional: on any internal
 * validation failure it restores the entry state and reports it. */
typedef struct pg_catchup_report {
    int64_t credited_seconds;
    uint32_t care_ticks, days_coarse;
    bool abandoned, clock_backwards, clock_jumped, rolled_back;
    uint16_t entry_count;
    pg_journal_entry entries[PG_JOURNAL_REPORT_MAX];  /* "while you were away" */
} pg_catchup_report;

bool pg_advance(pg_state *s, pg_now now, pg_catchup_report *report); /* M4 */

/* Next wall-clock second at which pg_advance would do visible work. The host
 * may use it to lengthen idle_sleep_ns; never required for correctness. */
int64_t pg_next_wall_deadline(const pg_state *s);                    /* M4 */

/* ---------- graphics ---------- */

bool  pg_graphics_init(pg_graphics *g, const char *asset_root,
                       char *error, size_t error_size);              /* M6 */
void  pg_graphics_shutdown(pg_graphics *g);                          /* M6 */
size_t pg_graphics_loaded_count(const pg_graphics *g);               /* M6 */
size_t pg_graphics_scene_count(const pg_graphics *g);                /* M6 */
const char *pg_graphics_scene_id(const pg_graphics *g, size_t index);/* M6 */
bool  pg_graphics_set_scene(pg_graphics *g, const char *scene_id);   /* NULL = off */
const char *pg_graphics_scene(const pg_graphics *g);                 /* NULL = off */

/* ---------- rendering ---------- */

/* A damage rectangle in FRAMEBUFFER pixels. x1/y1 are exclusive, matching the
 * presenter's convention; deliberately not ki_td_rect, which is x/y/w/h in
 * logical space, and deliberately not kittyfb_rect, which the core may not
 * name. */
typedef struct pg_rect { int x0, y0, x1, y1; } pg_rect;

typedef struct pg_render_result {
    ki_td_view view;                       /* the view it fitted */
    bool full_frame;                       /* transmit everything */
    size_t rect_count;
    pg_rect rects[PG_DAMAGE_MAX];          /* FRAMEBUFFER pixels, x1/y1 exclusive */
} pg_render_result;

/* Fits its own view to the renderer's full framebuffer and owns the canvas.
 * result may be NULL. Never allocates, never mutates s. */
bool pg_render(ki_td_soft_renderer *r, const pg_state *s,
               const pg_graphics *g, pg_render_result *result);      /* M6 */

/* Reserved for a future kilix-land prop/panel mode. Not implemented in 0.1 —
 * see ARCHITECTURE.md §2.6. Declared as a comment so the contract is fixed
 * before a consumer exists, without offering a symbol nobody defines. */
/* bool pg_render_in(ki_td_soft_renderer *r, const ki_td_view *view,
                     ki_td_rect logical_rect, const pg_state *s,
                     const pg_graphics *g); */

/* ---------- audio: take-and-clear, host owns the mixer ---------- */

typedef enum pg_audio_event_kind {
    PG_AUDIO_WATER = 0, PG_AUDIO_MIST, PG_AUDIO_FEED, PG_AUDIO_SNIP,
    PG_AUDIO_POT_SET, PG_AUDIO_ROTATE, PG_AUDIO_DRAIN, PG_AUDIO_SOIL_PRESS,
    PG_AUDIO_UI_MOVE, PG_AUDIO_UI_ACCEPT, PG_AUDIO_UI_REJECT, PG_AUDIO_GROWTH,
    PG_AUDIO_DAY_ROLL, PG_AUDIO_WILT, PG_AUDIO_RAIN, PG_AUDIO_SPATHE,
    PG_AUDIO_FOLD, PG_AUDIO_KIND_COUNT
} pg_audio_event_kind;

typedef struct pg_audio_event { uint8_t kind; float gain, pitch; } pg_audio_event;

size_t pg_take_audio_events(pg_state *s, pg_audio_event *out, size_t capacity); /* M9 */

/* Music scene the host should be playing (season + health derived). */
uint32_t pg_music_scene(const pg_state *s);                          /* M9 */

/* ---------- persistence: encode/decode are PURE, I/O is separate ---------- */

bool pg_save_encode(const pg_state *s, uint8_t *bytes, size_t capacity,
                    size_t *written);                                /* M5 */
bool pg_save_decode(pg_state *s, const uint8_t *bytes, size_t size); /* M5 */
bool pg_store_save(const pg_state *s, pg_store *store);              /* M5 */
bool pg_store_load(pg_state *s, pg_store *store, bool *recovered);   /* M5 */

/* ---------- settings (standalone only; the host owns them when embedded) --- */

bool pg_settings_load(pg_settings *out, pg_store *store);  /* never fails */ /* M5 */
bool pg_settings_save(const pg_settings *in, pg_store *store);       /* M5 */

#ifdef __cplusplus
}
#endif

#endif /* PLEB_PLANT_GROWER_H */
