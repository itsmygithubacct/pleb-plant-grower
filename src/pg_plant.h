/*
 * pg_plant — the plant record, growth, life state, health and the symptom
 * ladders.
 *
 * The record below is ARCHITECTURE.md §6.2's PLNT section field for field, so
 * the encoder has one struct to walk and the simulation has one struct to
 * advance. Every field is integer fixed point on the convention of D-083; no
 * float ever enters it, which is what makes a 30-day catch-up byte-identical
 * on every machine.
 *
 * Two structural commitments are worth naming because everything else follows
 * from them:
 *
 *   - **Every leaf is a record.** Attributes are frozen at birth from the
 *     conditions at the time, and damage bits are set and never cleared. That
 *     is the horticultural fact that fixing the light only makes the *new*
 *     growth correct, implemented literally, and it is why a well-kept plant
 *     is legible as a history rather than as a number.
 *   - **The fold state is not stored.** Nyctinasty is a pure function of local
 *     hour and stress and is always derived (ARCHITECTURE.md §6.2).
 */
#ifndef PG_PLANT_H
#define PG_PLANT_H

#include "pleb_plant_grower.h"

#include "pg_content.h"

#include <stdbool.h>
#include <stdint.h>

/* ---- life state ---------------------------------------------------------
 * Distinct from growth stage, which is size, and from health, which is the
 * coarse six-state readout the art indexes by. */
typedef enum pg_life_state {
    PG_LIFE_ESTABLISHING = 0,  /* newly potted or freshly rooted; shock is decaying */
    PG_LIFE_ACTIVE = 1,
    PG_LIFE_DORMANT = 2,       /* short days: reduced growth, not true dormancy */
    PG_LIFE_AILING = 3,        /* a ladder has passed its last reversible rung */
    PG_LIFE_TERMINAL = 4,      /* the mercy mechanic is offered here, before death */
    PG_LIFE_DEAD = 5,
    PG_LIFE_STATE_COUNT = 6
} pg_life_state;

/* The peace lily's whole achievement, and it is never promised. */
typedef enum pg_spathe_state {
    PG_SPATHE_NONE = 0,
    PG_SPATHE_BUDDING = 1,
    PG_SPATHE_OPEN = 2,
    PG_SPATHE_FADING = 3
} pg_spathe_state;

/* Timed actions are care-time deadlines, so a soak survives a crash, a closed
 * lid and a catch-up identically, and is force-completed to its worst outcome
 * on abandonment (D-089). */
typedef enum pg_pending_action {
    PG_PENDING_NONE = 0,
    PG_PENDING_BOTTOM_SOAK = 1,
    PG_PENDING_FLUSH = 2
} pg_pending_action;

/* A leaf's form is fixed when it unfurls — size, variegation and pattern
 * contrast are all set by the conditions at that moment and never change. */
#define PG_FORM_SIZE_SHIFT       0
#define PG_FORM_SIZE_MASK        0x03u
#define PG_FORM_VARIEGATION_SHIFT 2
#define PG_FORM_VARIEGATION_MASK 0x0Cu
#define PG_FORM_CONTRAST_SHIFT   4
#define PG_FORM_CONTRAST_MASK    0x30u
#define PG_FORM_ETIOLATED        0x40u
#define PG_FORM_UNFURLING        0x80u

typedef struct pg_leaf {
    uint16_t birth_care_day;
    uint8_t slot;          /* index into the species/stage anchor list (D-047) */
    uint8_t form_flags;
    uint8_t damage_mask;   /* pg_damage_bit; set, never cleared */
    uint8_t pose;
} pg_leaf;

/* Goldie is the exception to the anchor model: her vine is genuinely assembled
 * from tiled segments, so her nodes are positions rather than slots. */
typedef struct pg_vine_node {
    uint8_t internode;     /* length, set at birth from light_debt */
    uint8_t leaf_form;
    uint8_t damage_mask;
} pg_vine_node;

typedef struct pg_plant {
    int64_t planted_wall_s;
    char name[PG_NAME_BYTES];
    uint8_t species_id, pot_id, scene_id, spot_id;

    /* the nine level axes */
    uint16_t moisture_top, moisture_bottom, light_debt, nutrition, salt,
             purity_load, root_health, root_capacity, root_bound;

    uint16_t turgor;         /* first-order lag; the pose three failures share */
    uint32_t soggy_ticks;    /* care ticks at or above PG_SATURATION_L (D-082) */

    /* the remaining damage accumulators */
    uint16_t scorch_dose, crisp_dose, acclimation, shock;

    uint16_t dli_today;                  /* centi-mol/m2/day, rolls at local midnight */
    uint16_t dli_ring[PG_DLI_RING];
    uint8_t  dli_ring_head;

    uint16_t pest_gnats, pest_mites, pest_mealy;
    uint16_t pest_egg_timer;             /* why one treatment always fails */

    uint8_t  growth_stage;
    uint32_t growth_points;
    uint8_t  life_state;

    uint8_t  spathe_state, flower_count;

    uint8_t  leaf_count;
    pg_leaf  leaves[PG_LEAF_MAX];
    uint8_t  vine_count;
    pg_vine_node vine[PG_VINE_MAX];

    /* due dates live in CARE time, not wall time */
    uint64_t last_watered_care_s, last_fed_care_s, last_repotted_care_s,
             last_rotated_care_s, last_drained_care_s;
    uint8_t  pending_action;
    uint64_t pending_action_care_s;

    uint16_t streak_on_time, missed_events;
} pg_plant;

/* ---- the symptom ladders ------------------------------------------------
 * PLANT_CARE.md §6.1. Each is an ordered set of rungs with an onset time and a
 * reversibility flag, and the fair-warning rule (D-022, D-085) is the promise
 * that no ladder reaches an irreversible rung without having shown a distinct,
 * reversible, visible one for at least 24 h of effective-scalar time first. */
typedef enum pg_ladder {
    PG_LADDER_UNDERWATER = 0,
    PG_LADDER_OVERWATER = 1,
    PG_LADDER_LOW_LIGHT = 2,
    PG_LADDER_EXCESS_LIGHT = 3,
    PG_LADDER_LOW_HUMIDITY = 4,
    PG_LADDER_SALT = 5,
    PG_LADDER_ROOTBOUND = 6,
    PG_LADDER_COLD = 7,
    PG_LADDER_PESTS = 8,
    PG_LADDER_COUNT = 9
} pg_ladder;

#define PG_LADDER_RUNG_MAX 5

typedef struct pg_ladder_rung {
    uint16_t onset_ticks;   /* at effective scalar 1.0; divided by it in winter */
    bool reversible;
} pg_ladder_rung;

/* Rung 0 means "nothing to see". The rung description is deliberately not
 * here: the game's own tooltips must never reinforce the wrong reading, so the
 * diagnosis cards list candidates plus distinguishing observations rather than
 * naming an answer (PLANT_CARE.md §6.2). */
uint8_t pg_plant_ladder_rung(const pg_plant *plant, pg_ladder ladder);
const pg_ladder_rung *pg_plant_ladder_spec(pg_ladder ladder, uint8_t rung);
const char *pg_plant_ladder_name(pg_ladder ladder);

/* The first irreversible rung of a ladder. Everything below it is a warning
 * the fair-warning rule guarantees the player saw. */
uint8_t pg_plant_ladder_first_permanent(pg_ladder ladder);

/* ---- lifecycle ---------------------------------------------------------- */

void pg_plant_init(pg_plant *plant, uint8_t species_id, uint8_t pot_id,
                   uint8_t spot_id, int64_t planted_wall_s);

/* The coarse 0..5 health index the art indexes rows by (D-048). Derived, never
 * stored: it is a summary of the axes and must never become a second source of
 * truth. */
uint8_t pg_plant_health(const pg_plant *plant);

/* Growth stage from accumulated growth points. */
uint8_t pg_plant_stage_for_points(uint32_t growth_points);

/* Water the roots can actually deliver — the one expression that carries
 * thirst, root rot and fertiliser burn with no special cases
 * (GAME_DESIGN.md §4.4). */
uint16_t pg_plant_available_water_l(const pg_plant *plant);

/* The 14-day mean DLI that sets leaf size and variegation at birth. */
uint16_t pg_plant_mean_dli_c(const pg_plant *plant);

/* Add a leaf whose form is frozen from the conditions right now. Returns false
 * when the anchor slots are full, which is not an error: a mature plant sheds
 * as it grows. */
bool pg_plant_add_leaf(pg_plant *plant, uint16_t birth_care_day);

/* Set a damage bit on every leaf that was out at the time. Damage bits are set
 * and never cleared; this is the only writer. */
void pg_plant_mark_damage(pg_plant *plant, uint8_t damage_bit);

/* ---- per-species flavour, because it is each plant's whole identity ------ */

/* Pothos: the vine accumulates with growth points and is the free progress
 * bar. Returns the vine length in nodes. */
uint8_t pg_plant_vine_length(const pg_plant *plant);
void pg_plant_vine_extend(pg_plant *plant, uint16_t light_debt_at_birth);

/* Snake plant: one new blade per good growing season, and a pot it can crack. */
bool pg_plant_snake_may_add_blade(const pg_plant *plant);
bool pg_plant_pot_is_cracking(const pg_plant *plant);

/* Peace lily: collapse is a posture, reversible in 2-24 h, and repeated severe
 * collapses scar permanently. */
bool pg_plant_is_collapsed(const pg_plant *plant);
bool pg_plant_spathe_possible(const pg_plant *plant);

/* Calathea: nyctinasty folds on the calendar clock and folds harder under
 * drought. Derived, never stored. 0 = open, PG_LEVEL_MAX = fully folded. */
uint16_t pg_plant_fold_l(const pg_plant *plant, uint16_t ordinal,
                         uint16_t local_minutes);

/* ---- the mercy mechanic -------------------------------------------------
 * A plant lost to rot very often still contains a viable piece, and it is
 * genuinely bad advice to present total loss without offering the cutting
 * (PLANT_CARE.md §6.3). Offered automatically at the terminal rung, before
 * death, and the odds are the tabled rot_survival — a real gamble, not a free
 * undo. */
bool pg_plant_mercy_offered(const pg_plant *plant);
uint16_t pg_plant_mercy_odds_l(const pg_plant *plant);

/* Headless diagnostic behind --rules-test: verb legality, the enumeration that
 * the hard refusals are exactly the four Feed cases, the season gating of the
 * feed refusal, and the myth blocklist against every string the compiled
 * content can put on screen. Returns 0 when every assertion held. */
int pg_plant_run_rules_test(void);

#endif /* PG_PLANT_H */
