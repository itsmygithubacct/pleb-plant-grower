/*
 * pg_ui — see pg_ui.h. Screens, focus, and words instead of numbers.
 */
#include "pg_ui.h"

#include "pg_calendar.h"
#include "pg_care.h"
#include "pg_content.h"
#include "pg_plant.h"
#include "pg_scene.h"
#include "pg_sim.h"
#include "pg_state.h"

#include "kilix_top_down_soft.h"

#include <stdio.h>
#include <string.h>

/* The HUD column. panel_side mirrors it for scenes that put the plant on the
 * right, which is the whole reason that field exists. */
#define PG_UI_PANEL_W  (PG_LOGICAL_WIDTH - PG_PANEL_X - 6)
#define PG_UI_PANEL_Y  8

void pg_ui_init(pg_ui_state *ui)
{
    if (ui == NULL) return;
    memset(ui, 0, sizeof *ui);
    ui->screen = (uint8_t)PG_SCREEN_PLANT;
    ui->previous = (uint8_t)PG_SCREEN_PLANT;
    kilix_ui_focus_init(&ui->actions, (size_t)PG_VERB_COUNT, 8u);
    kilix_ui_focus_init(&ui->chooser, (size_t)PG_SPECIES_COUNT, 4u);
    kilix_ui_focus_init(&ui->journal, (size_t)PG_JOURNAL_REPORT_MAX, 10u);
    kilix_ui_focus_init(&ui->calendar, 42u, 42u);
}

void pg_ui_goto(pg_ui_state *ui, uint8_t screen)
{
    if (ui == NULL || screen >= (uint8_t)PG_SCREEN_COUNT) return;
    if (screen == ui->screen) return;
    ui->previous = ui->screen;
    ui->screen = screen;
}

void pg_ui_back(pg_ui_state *ui)
{
    if (ui == NULL) return;
    /* The two screens that are not dismissible: a damaged store and a dead
     * plant both need a decision, and Cancel is not one. */
    if (ui->screen == (uint8_t)PG_SCREEN_DAMAGED ||
        ui->screen == (uint8_t)PG_SCREEN_REPLANT) {
        return;
    }
    ui->screen = ui->previous;
    ui->previous = (uint8_t)PG_SCREEN_PLANT;
}

uint8_t pg_ui_entry_screen(const pg_state *state, uint8_t store_status)
{
    if (store_status == (uint8_t)PG_STORE_DAMAGED) {
        return (uint8_t)PG_SCREEN_DAMAGED;   /* never silently wipe */
    }
    if (state == NULL || state->plant_count == 0u) {
        return (uint8_t)PG_SCREEN_CHOOSER;
    }
    if (state->replant_offered) {
        return (uint8_t)PG_SCREEN_REPLANT;
    }
    return (uint8_t)PG_SCREEN_PLANT;
}

/* ---- readings, not numbers ---------------------------------------------- */

const char *pg_ui_moisture_words(uint16_t level_l)
{
    /* "dry at 2 cm" is the longest of these on purpose: it is the only one
     * that teaches the check rather than reporting a state, and "Soil dry at
     * 2 cm" is exactly the 16 characters a panel line holds. */
    if (level_l >= 9000u) return "soaked";
    if (level_l >= 7000u) return "wet";
    if (level_l >= 4500u) return "damp";
    if (level_l >= 2500u) return "drying";
    if (level_l >= 1200u) return "dry at 2 cm";
    return "bone dry";
}

const char *pg_ui_light_words(uint8_t light_band, uint16_t dli_c)
{
    if (dli_c == 0u) return "dark";
    switch (light_band) {
    case PG_LIGHT_LOW:    return "dim";
    case PG_LIGHT_MEDIUM: return "soft";
    /* "bright, no sun on the leaves" is the accurate phrase and it does not
     * fit a panel line; the close-up screen has room to say it in full. */
    case PG_LIGHT_BRIGHT_INDIRECT: return "bright";
    case PG_LIGHT_DIRECT: return "direct sun";
    default:              return "soft";
    }
}

const char *pg_ui_humidity_words(uint8_t rh_pct)
{
    if (rh_pct >= 70u) return "muggy";
    if (rh_pct >= 55u) return "comfy";
    if (rh_pct >= 40u) return "dryish";
    if (rh_pct >= 30u) return "dry";
    return "very dry";
}

const char *pg_ui_health_words(uint8_t health)
{
    switch (health) {
    case PG_HEALTH_THRIVING:   return "thriving";
    case PG_HEALTH_HEALTHY:    return "well";
    case PG_HEALTH_THIRSTY:    return "thirsty";
    case PG_HEALTH_DISTRESSED: return "poorly";
    case PG_HEALTH_CRITICAL:   return "failing";
    case PG_HEALTH_DEAD:       return "gone";
    default:                   return "well";
    }
}

/* ---- input --------------------------------------------------------------- */

static kilix_ui_focus *active_focus(pg_ui_state *ui)
{
    switch ((pg_screen)ui->screen) {
    case PG_SCREEN_CHOOSER:  return &ui->chooser;
    case PG_SCREEN_JOURNAL:
    case PG_SCREEN_AWAY:     return &ui->journal;
    case PG_SCREEN_CALENDAR: return &ui->calendar;
    case PG_SCREEN_PLANT:    return &ui->actions;
    default:                 return NULL;
    }
}

bool pg_ui_input(pg_ui_state *ui, const pg_input *in, const pg_state *state)
{
    kilix_ui_focus *focus;

    if (ui == NULL || in == NULL) return false;

    /* The three toggles are always available and always consumed. */
    if (in->toggle_calendar) {
        pg_ui_goto(ui, ui->screen == (uint8_t)PG_SCREEN_CALENDAR
                       ? ui->previous : (uint8_t)PG_SCREEN_CALENDAR);
        return true;
    }
    if (in->toggle_journal) {
        pg_ui_goto(ui, ui->screen == (uint8_t)PG_SCREEN_JOURNAL
                       ? ui->previous : (uint8_t)PG_SCREEN_JOURNAL);
        return true;
    }
    if (in->help) {
        ui->help_open = !ui->help_open;
        return true;
    }
    if (in->cancel) {
        if (ui->help_open) { ui->help_open = false; return true; }
        if (ui->screen == (uint8_t)PG_SCREEN_CHOOSER &&
            ui->chooser_step != (uint8_t)PG_CHOOSER_SPECIES) {
            ui->chooser_step = (uint8_t)(ui->chooser_step - 1u);
            kilix_ui_focus_init(&ui->chooser,
                                ui->chooser_step ==
                                    (uint8_t)PG_CHOOSER_SPECIES
                                ? (size_t)PG_SPECIES_COUNT
                                : (size_t)PG_POT_COUNT, 4u);
            return true;
        }
        pg_ui_back(ui);
        return true;
    }

    focus = active_focus(ui);
    if (focus == NULL) return false;

    if (in->move_y < 0) {
        (void)kilix_ui_focus_apply(focus, KILIX_UI_ACTION_UP, NULL);
        return true;
    }
    if (in->move_y > 0) {
        (void)kilix_ui_focus_apply(focus, KILIX_UI_ACTION_DOWN, NULL);
        return true;
    }
    /* The calendar is a grid, so left/right is a day and up/down is a week. */
    if (ui->screen == (uint8_t)PG_SCREEN_CALENDAR && in->move_x != 0) {
        (void)kilix_ui_focus_apply(focus, in->move_x < 0
                                   ? KILIX_UI_ACTION_UP
                                   : KILIX_UI_ACTION_DOWN, NULL);
        return true;
    }

    if (in->confirm && ui->screen == (uint8_t)PG_SCREEN_CHOOSER) {
        if (ui->chooser_step == (uint8_t)PG_CHOOSER_SPECIES) {
            ui->chooser_species = (uint8_t)focus->selected;
            ui->chooser_step = (uint8_t)PG_CHOOSER_POT;
            kilix_ui_focus_init(&ui->chooser, (size_t)PG_POT_COUNT, 4u);
        } else if (ui->chooser_step == (uint8_t)PG_CHOOSER_POT) {
            ui->chooser_pot = (uint8_t)focus->selected;
            ui->chooser_step = (uint8_t)PG_CHOOSER_CONFIRM;
        }
        return true;
    }
    (void)state;
    return false;
}

/* ---- drawing ------------------------------------------------------------- */

static ki_td_rect panel_rect(const pg_graphics *graphics, int y, int height)
{
    ki_td_rect rect;
    bool left = false;

    if (graphics != NULL && graphics->scene < graphics->scene_count) {
        left = graphics->scenes[graphics->scene].panel_side ==
               (uint8_t)PG_PANEL_LEFT;
    }
    rect.x = left ? 6 : PG_PANEL_X;
    rect.y = y;
    rect.width = PG_UI_PANEL_W;
    rect.height = height;
    return rect;
}

static void draw_plant_screen(ki_td_soft_renderer *renderer,
                              const ki_td_view *view, const pg_state *state,
                              const pg_ui_state *ui,
                              const pg_graphics *graphics,
                              const kilix_ui_style *style)
{
    const pg_plant *plant = &state->plants[0];
    pg_care_env env = pg_sim_env_now(state, 0u);
    ki_td_rect rect = panel_rect(graphics, PG_UI_PANEL_Y, 118);
    uint16_t moisture = pg_care_moisture_whole_l(plant);
    int row = rect.y + 6;

    kilix_ui_draw_panel(renderer, view, rect, style, NULL);

    /* Readings, never numbers -- kilix_ui_draw_meter_text exists for this.
     *
     * Clipped to the panel: the meter draws its text without clipping it to
     * the bar, so a string longer than the line would run off the panel and
     * across the room. The words below are chosen to fit, and this is the
     * guard for the day one of them is not. */
    {
        sr_canvas *canvas = ki_td_soft_canvas(renderer);
        int saved[4];
        int x0, y0, x1, y1;

        saved[0] = canvas->clip_x0; saved[1] = canvas->clip_y0;
        saved[2] = canvas->clip_x1; saved[3] = canvas->clip_y1;
        x0 = ki_td_screen_x(view, (float)rect.x);
        y0 = ki_td_screen_y(view, (float)rect.y);
        x1 = ki_td_screen_x(view, (float)(rect.x + rect.width));
        y1 = ki_td_screen_y(view, (float)(rect.y + rect.height));
        if (x0 > canvas->clip_x0) canvas->clip_x0 = x0;
        if (y0 > canvas->clip_y0) canvas->clip_y0 = y0;
        if (x1 < canvas->clip_x1) canvas->clip_x1 = x1;
        if (y1 < canvas->clip_y1) canvas->clip_y1 = y1;
        if (canvas->clip_x1 < canvas->clip_x0)
            canvas->clip_x1 = canvas->clip_x0;
        if (canvas->clip_y1 < canvas->clip_y0)
            canvas->clip_y1 = canvas->clip_y0;
{
        ki_td_rect bar = {rect.x + 5, row, rect.width - 10, 12};
        kilix_ui_draw_meter_text(renderer, view, bar, style,
                                 (float)pg_plant_health(plant), 5.0f,
                                 "Health",
                                 pg_ui_health_words(pg_plant_health(plant)));
        bar.y = (row += 16);
        kilix_ui_draw_meter_text(renderer, view, bar, style,
                                 (float)moisture, (float)PG_LEVEL_MAX,
                                 "Soil", pg_ui_moisture_words(moisture));
        bar.y = (row += 16);
        kilix_ui_draw_meter_text(renderer, view, bar, style,
                                 (float)env.dli_day_c, 1800.0f, "Light",
                                 pg_ui_light_words(env.light_band,
                                                   env.dli_day_c));
        bar.y = (row += 16);
        kilix_ui_draw_meter_text(renderer, view, bar, style,
                                 (float)env.rh_pct, 100.0f, "Air",
                                 pg_ui_humidity_words(env.rh_pct));
}
        canvas->clip_x0 = saved[0]; canvas->clip_y0 = saved[1];
        canvas->clip_x1 = saved[2]; canvas->clip_y1 = saved[3];
    }
    (void)ui;
}

/* ---- the chooser -------------------------------------------------------
 *
 * Four plants, then four pots, then a confirmation that says why this pot
 * suits this plant. Two lists rather than a sixteen-cell grid, because the
 * second choice depends on the first: a cachepot is a different decision for
 * a snake plant than for a calathea, and the list can say so. */
static void draw_chooser_screen(ki_td_soft_renderer *renderer,
                                const ki_td_view *view,
                                const pg_ui_state *ui,
                                const kilix_ui_style *style)
{
    const char *names[PG_SPECIES_COUNT > PG_POT_COUNT
                      ? PG_SPECIES_COUNT : PG_POT_COUNT];
    ki_td_rect rect = {110, 60, 260, 150};
    size_t count;
    size_t index;
    const char *title;

    if (ui->chooser_step == (uint8_t)PG_CHOOSER_SPECIES) {
        count = (size_t)PG_SPECIES_COUNT;
        title = "Which plant would you like?";
        for (index = 0u; index < count; ++index) {
            const pg_species *species = pg_content_species((uint8_t)index);
            names[index] = species ? species->common_name : "";
        }
    } else {
        count = (size_t)PG_POT_COUNT;
        title = "And a pot for it?";
        for (index = 0u; index < count; ++index) {
            const pg_pot *pot = pg_content_pot((uint8_t)index);
            names[index] = pot ? pot->display_name : "";
        }
    }
    kilix_ui_draw_panel(renderer, view, rect, style, NULL);
    kilix_ui_draw_list(renderer, view, rect, style, NULL, &ui->chooser,
                       names, NULL, count);
    {
        ki_td_rect caption = {rect.x, rect.y - 14, rect.width, 12};
        kilix_ui_draw_meter_text(renderer, view, caption, style, 0.0f, 1.0f,
                                 title, NULL);
    }
}

/* ---- journal and the away report ---------------------------------------
 *
 * The same list twice, because they are the same thing: what happened while
 * you were not looking. The away report is the journal filtered to the last
 * absence, and calling it a second screen would have meant a second layout to
 * keep in agreement with the first. */
static void draw_journal_screen(ki_td_soft_renderer *renderer,
                                const ki_td_view *view,
                                const pg_state *state,
                                const pg_ui_state *ui,
                                const kilix_ui_style *style)
{
    static const char *const KIND_TEXT[PG_JOURNAL_KIND_COUNT] = {
        "", "It got thirsty", "It collapsed", "It perked up",
        "The soil stayed wet", "The roots suffered", "The soak finished",
        "The flush finished", "A new leaf", "It grew", "A spathe opened",
        "The rain jug filled", "", "It went dormant", "It woke up",
        "It started ailing", "It is in real trouble", "It died",
        "Left alone", "The clock was unsettled", "Watered", "Fed",
        "Repotted", "Moved"
    };
    pg_journal_entry entries[PG_JOURNAL_REPORT_MAX];
    const char *lines[PG_JOURNAL_REPORT_MAX];
    char text[PG_JOURNAL_REPORT_MAX][64];
    ki_td_rect rect = {40, 24, PG_LOGICAL_WIDTH - 80, PG_LOGICAL_HEIGHT - 56};
    size_t count;
    size_t index;

    count = pg_journal_recent(state, entries, PG_JOURNAL_REPORT_MAX);
    for (index = 0u; index < count; ++index) {
        const char *what = entries[index].kind < PG_JOURNAL_KIND_COUNT
                         ? KIND_TEXT[entries[index].kind] : "";
        /* The fine tier stores the local hour in `detail`, which is what lets
         * a line say "Wednesday 14:00" rather than "at some point". */
        (void)snprintf(text[index], sizeof text[index], "day %u  %02u:00  %s",
                       (unsigned)entries[index].care_day,
                       (unsigned)entries[index].detail, what);
        lines[index] = text[index];
    }
    kilix_ui_draw_panel(renderer, view, rect, style, NULL);
    if (count == 0u) {
        ki_td_rect empty = {rect.x + 8, rect.y + 8, rect.width - 16, 12};
        kilix_ui_draw_meter_text(renderer, view, empty, style, 0.0f, 1.0f,
                                 "Nothing has happened yet.", NULL);
        return;
    }
    kilix_ui_draw_list(renderer, view, rect, style, NULL, &ui->journal,
                       lines, NULL, count);
}

/* ---- the two screens that need a decision ------------------------------- */
static void draw_modal_screen(ki_td_soft_renderer *renderer,
                              const ki_td_view *view, const pg_state *state,
                              const pg_ui_state *ui,
                              const kilix_ui_style *style)
{
    ki_td_rect rect = {70, 80, 340, 110};
    const char *lines[3];
    const char *speaker;

    if (ui->screen == (uint8_t)PG_SCREEN_DAMAGED) {
        speaker = "The notebook";
        lines[0] = "Both copies of this plant's record are unreadable.";
        lines[1] = "Nothing has been erased. Choose to start again";
        lines[2] = "only when you are ready.";
    } else {
        speaker = "The notebook";
        lines[0] = "It did not make it. That happens, and it is";
        lines[1] = "usually the soil rather than you.";
        lines[2] = "The pot and the calendar are still here.";
    }
    kilix_ui_draw_dialogue(renderer, view, rect, style, NULL, NULL, speaker,
                           lines, 3u, "Enter");
    (void)state;
}

/* ---- the calendar screen ------------------------------------------------
 *
 * The month grid on kilix_ui_draw_calendar. The compact 7x14 face is used for
 * the day numbers for a VERTICAL reason, not a horizontal one: it has the same
 * 8 px advance as the default face and a 14 px cell instead of 16, so six week
 * rows fit in 84 logical rows where the fixed face needs 96. That difference
 * is what lets a title, seven weekday labels, the grid and a prompt strip
 * share 270 rows.
 *
 * All the date arithmetic is here, because the widget deliberately does none. */
static void draw_calendar_screen(ki_td_soft_renderer *renderer,
                                 const ki_td_view *view,
                                 const pg_state *state,
                                 const pg_ui_state *ui,
                                 const kilix_ui_style *style)
{
    static const char *const WEEKDAYS[7] = {"M", "T", "W", "T", "F", "S", "S"};
    static const char *const MONTHS[13] = {
        "", "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY",
        "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"
    };
    kilix_ui_calendar_day days[42];
    kilix_ui_calendar calendar;
    ki_td_rect rect = {24, 12, PG_LOGICAL_WIDTH - 48, PG_LOGICAL_HEIGHT - 40};
    char title[48];
    pg_date today;
    int64_t midnight;
    size_t index;
    int lead;
    int month_days;
    int day;

    if (!pg_calendar_local(state->anchor.last_wall_s,
                           state->anchor.tz_offset_minutes, &today)) {
        return;
    }
    /* Weekday of the first of the month: walk back from today, which needs no
     * Zeller and cannot disagree with pg_calendar. */
    lead = ((int)today.weekday - ((int)today.day - 1) % 7 + 14) % 7;
    midnight = pg_calendar_local_midnight_s(state->anchor.last_wall_s,
                                            state->anchor.tz_offset_minutes);
    {
        static const uint8_t LENGTHS[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31,
                                            30, 31, 30, 31};
        bool leap = (today.year % 4 == 0 && today.year % 100 != 0) ||
                    today.year % 400 == 0;
        month_days = (int)LENGTHS[today.month];
        if (today.month == 2u && leap) month_days = 29;
    }

    memset(days, 0, sizeof days);
    memset(&calendar, 0, sizeof calendar);
    for (index = 0u; index < 42u; ++index) {
        day = (int)index - lead + 1;
        days[index].in_month = day >= 1 && day <= month_days;
        days[index].enabled = days[index].in_month;
        days[index].marks = 0u;
    }
    /* Journal marks: one bit per event class, so a month reads as a history
     * at a glance. The ring is care-day indexed, so a mark lands on the day
     * it happened rather than on the day it was read. */
    for (index = 0u; index < (size_t)state->journal_count &&
                     index < (size_t)PG_JOURNAL_RING; ++index) {
        const pg_journal_entry *entry = &state->journal[index];
        int cell;
        uint8_t bit;
        switch ((pg_journal_kind)entry->kind) {
        case PG_JOURNAL_WATERED:   bit = 0x01u; break;
        case PG_JOURNAL_FED:       bit = 0x02u; break;
        case PG_JOURNAL_REPOTTED:  bit = 0x04u; break;
        case PG_JOURNAL_NEW_LEAF:
        case PG_JOURNAL_GREW:      bit = 0x08u; break;
        default:                   bit = 0x10u; break;
        }
        cell = lead + (int)(entry->care_day % (uint32_t)month_days);
        if (cell >= 0 && cell < 42) days[cell].marks |= bit;
    }

    (void)snprintf(title, sizeof title, "%s %d",
                   MONTHS[today.month <= 12u ? today.month : 0u],
                   (int)today.year);
    calendar.title = title;
    calendar.weekday_labels = WEEKDAYS;
    calendar.days = days;
    calendar.day_count = 42u;
    calendar.today = (size_t)(lead + (int)today.day - 1);
    calendar.mark_colors[0] = UINT32_C(0x6ab7ff);   /* watered   */
    calendar.mark_colors[1] = UINT32_C(0xffc65a);   /* fed       */
    calendar.mark_colors[2] = UINT32_C(0xc98a5a);   /* repotted  */
    calendar.mark_colors[3] = UINT32_C(0x7fd67f);   /* grew      */
    calendar.mark_colors[4] = UINT32_C(0x9a9a9a);   /* everything else */
    calendar.font = SR_FONT_COMPACT_7X14;

    kilix_ui_draw_calendar(renderer, view, rect, style, NULL, &ui->calendar,
                           &calendar);
    (void)midnight;
}

void pg_ui_draw(ki_td_soft_renderer *renderer, const ki_td_view *view,
                const pg_state *state, const pg_ui_state *ui,
                const pg_graphics *graphics)
{
    kilix_ui_style style;

    if (renderer == NULL || view == NULL || state == NULL || ui == NULL) {
        return;
    }
    kilix_ui_style_init(&style);
    /* The HUD never depends on the art being legible: every text surface is a
     * panel with its own alpha, and nothing is drawn straight onto a plate. */

    switch ((pg_screen)ui->screen) {
    case PG_SCREEN_PLANT:
        if (state->plant_count > 0u) {
            draw_plant_screen(renderer, view, state, ui, graphics, &style);
        }
        break;
    case PG_SCREEN_CALENDAR:
        draw_calendar_screen(renderer, view, state, ui, &style);
        break;
    case PG_SCREEN_CHOOSER:
        draw_chooser_screen(renderer, view, ui, &style);
        break;
    case PG_SCREEN_JOURNAL:
    case PG_SCREEN_AWAY:
        draw_journal_screen(renderer, view, state, ui, &style);
        break;
    case PG_SCREEN_DAMAGED:
    case PG_SCREEN_REPLANT:
        draw_modal_screen(renderer, view, state, ui, &style);
        break;
    default:
        break;
    }
}
