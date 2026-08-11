/*
 * pg_ui — the screens, and which one you are looking at.
 *
 * Seven screens and the transitions between them. The state lives in pg_state
 * as one small record so the frontend and the embed drive identical UI, and so
 * a screen can be restored after a crash without a second source of truth.
 *
 * Everything drawn here goes through kilix-ui. In particular there is **no bar
 * drawer in this game**: `kilix_ui_draw_meter_text` is the readout for health,
 * moisture, light, humidity and feed, which is exactly why that entry point was
 * contributed rather than a local one written. A game that draws its own
 * rectangle where a shared widget exists is how two consumers end up with two
 * slightly different meters.
 *
 * Numbers the player never sees: the meters take a caller-supplied *reading*
 * ("thirsty", "damp", "soaked"), never "62/100". The simulation is integer
 * fixed point everywhere, and exposing that is a design failure, not a feature.
 */
#ifndef PG_UI_H
#define PG_UI_H

#include "pleb_plant_grower.h"

#include "kilix_ui.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum pg_screen {
    PG_SCREEN_CHOOSER = 0,     /* 4 plants x 4 pots, the first run       */
    PG_SCREEN_PLANT = 1,       /* the plant itself: the home screen      */
    PG_SCREEN_CALENDAR = 2,
    PG_SCREEN_JOURNAL = 3,
    PG_SCREEN_AWAY = 4,        /* "while you were away"                  */
    PG_SCREEN_DAMAGED = 5,     /* both save generations unreadable       */
    PG_SCREEN_REPLANT = 6,     /* a death opened the replant flow        */
    PG_SCREEN_COUNT = 7
} pg_screen;

/* The chooser is two lists, not one: species then pot. Which one has focus is
 * part of the screen, so cancel means "back to the species list" before it
 * means "leave the chooser". */
typedef enum pg_chooser_step {
    PG_CHOOSER_SPECIES = 0,
    PG_CHOOSER_POT = 1,
    PG_CHOOSER_CONFIRM = 2
} pg_chooser_step;

/* Not persisted. A screen is where you were looking, not a fact about the
 * plant, and restoring somebody into a modal they had already dismissed is
 * worse than putting them back on the plant. The two exceptions are derived
 * on load rather than stored: a damaged store opens PG_SCREEN_DAMAGED, and a
 * dead plant opens PG_SCREEN_REPLANT. */
typedef struct pg_ui_state {
    uint8_t screen;
    uint8_t previous;          /* what Cancel returns to */
    uint8_t chooser_step;
    uint8_t chooser_species;
    uint8_t chooser_pot;
    kilix_ui_focus actions;    /* the verb list on the plant screen */
    kilix_ui_focus chooser;
    kilix_ui_focus journal;
    kilix_ui_focus calendar;
    uint32_t calendar_month_offset;  /* 0 = the month containing today */
    bool help_open;
} pg_ui_state;

void pg_ui_init(pg_ui_state *ui);

/* Move to a screen, remembering where Cancel goes. */
void pg_ui_goto(pg_ui_state *ui, uint8_t screen);
void pg_ui_back(pg_ui_state *ui);

/* Apply one frame of semantic input. Returns true when the input was consumed
 * by the UI and must not also be read as a verb. */
bool pg_ui_input(pg_ui_state *ui, const pg_input *in, const pg_state *state);

/* The screen the state implies on load: damaged store and dead plant both
 * open their own screen, and a first run opens the chooser. */
uint8_t pg_ui_entry_screen(const pg_state *state, uint8_t store_status);

/* A moisture/light/humidity reading as words. Never a number, never a
 * percentage: the player is looking at soil, not at a gauge. */
const char *pg_ui_moisture_words(uint16_t level_l);
const char *pg_ui_light_words(uint8_t light_band, uint16_t dli_c);
const char *pg_ui_humidity_words(uint8_t rh_pct);
const char *pg_ui_health_words(uint8_t health);

/* Draw the current screen. Layers 8 and 9 of ARCHITECTURE.md §3.2. */
void pg_ui_draw(ki_td_soft_renderer *renderer, const ki_td_view *view,
                const pg_state *state, const pg_ui_state *ui,
                const pg_graphics *graphics);

#endif /* PG_UI_H */
