/* Dedicated, background-owned Sports sessions. All sensor and storage work belongs
 * to workout.c; this UI only reads snapshots in tick/enter. Leaving a screen never
 * pauses or ends a session. Lists use the OS retained directional-blur path. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "workout.h"
#include "prefs.h"
#include "energy.h"
#include "wrist_detect.h"
#include "diag_log.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { VIEW_CHOOSE, VIEW_LIVE, VIEW_SUMMARY };
enum { HIT_NONE, HIT_LEFT, HIT_RIGHT, HIT_SINGLE, HIT_DETAILS, HIT_HISTORY, HIT_SETUP, HIT_SPORT_BASE = 16 };
#define LIST_TOP 116
#define GRID_TOP 150
#define GRID_STEP 94
#define GRID_W 146
#define GRID_H 82
#define RECENT_Y 410
#define DETAIL_ROWS (WORKOUT_LAPS_MAX + 36)
#define CHART_COLS 32           /* detail chart: up to 32 dot columns, bins grouped to fit */

static const nos_screen ready_screen, finish_screen, history_screen, detail_screen;
static const nos_screen catalog_screen, setup_screen, wrist_screen, mass_screen, height_screen, age_screen;
static const workout_profile *profiles[WORKOUT_SPORT_COUNT];
enum { BF_MASS, BF_HEIGHT, BF_AGE };
static int wrist_mode, wrist_started, mass_draft, body_field, weight_asked;
/* Stable disk IDs are independent of the owner's priority order. */
static const int favorites[6] = {
    WORKOUT_FOOTBALL, WORKOUT_CRICKET, WORKOUT_BADMINTON,
    WORKOUT_RUN, WORKOUT_HIKE, -1
};
static ui_list catalog_list, setup_list;
static struct {
    workout_snapshot session;
    workout_storage_status storage;
    uint32_t dismissed_id;
    int view, selected, initialized, metric;
    int pressed;
    nos_time_t press_t; nos_time_t release_at;
    char elapsed[20], distance[20], primary[20], heart[16], cadence[16], steps[16], avg_speed[16], summary_avg[20];
    char status[48], metric_label[24], lap_label[32], average[48], gps_coverage[32], energy[48];
} W;
typedef struct {
    ui_list list;
    workout_snapshot items[WORKOUT_HISTORY_MAX];
    char date[WORKOUT_HISTORY_MAX][24];
    char value[WORKOUT_HISTORY_MAX][48];
    char status[WORKOUT_HISTORY_MAX][12];
    uint32_t refreshed_ms;
    int count, metric;
} history_ui;
typedef struct {
    ui_list list;
    workout_snapshot item;
    struct { char label[40], value[64]; } rows[DETAIL_ROWS];
    uint8_t chart[DETAIL_ROWS];     /* 0 text row; else a dot-column chart (columns in value[], count in value[63]) */
    int n, metric;
    char title[32];
} detail_ui;
/* Allocate the bounded history/details state only when opened. Keeping it in
 * external RAM avoids consuming the board's internal heap for optional pages. */
static history_ui *history_state;
static detail_ui *detail_state;
#define H (*history_state)
#define D (*detail_state)

static const workout_profile *profile(int sport)
{
    return profiles[sport >= 0 && sport < WORKOUT_SPORT_COUNT ? sport : WORKOUT_GENERAL];
}
static const char *profile_name(int sport)
{
    const workout_profile *p = profile(sport);
    return p && p->name ? p->name : "Workout";
}
static int profile_icon(int sport)
{
    if (sport < 0) return DOT_ICON_ROUTE;
    const workout_profile *p = profile(sport);
    if (sport == WORKOUT_HIKE || sport == WORKOUT_TREK) return DOT_ICON_MOUNTAIN;
    if (p->uses_steps) return DOT_ICON_STEPS;
    return p->outdoor ? DOT_ICON_ROUTE : DOT_ICON_HEART;
}
static const char *hand_name(int hand)
{
    static const char *const names[] = {"Unknown", "Left", "Right", "Both"};
    return names[hand >= 0 && hand <= HAND_BOTH ? hand : 0];
}
static uint8_t *playing_pref(void)
{
    if (W.selected == WORKOUT_CRICKET) return &nos_prefs.sports_batting;
    if (W.selected == WORKOUT_VOLLEYBALL) return &nos_prefs.sports_volleyball;
    return &nos_prefs.sports_racket;
}
static int uses_playing_hand(int sport)
{
    return profile(sport)->family == WORKOUT_FAMILY_RACKET ||
           sport == WORKOUT_CRICKET || sport == WORKOUT_VOLLEYBALL;
}
static int body_year(void)
{
    hal_datetime d;
    hal_get_datetime(&d);
    return d.year >= 2024 && d.year <= 2100 ? d.year : ENERGY_YEAR_FALLBACK;
}
static void configure_session(void)
{
    workout_setup s = {
        .worn_wrist = nos_prefs.wrist_left ? HAND_LEFT : HAND_RIGHT,
        .dominant_hand = nos_prefs.sports_dominant,
        .playing_hand = uses_playing_hand(W.selected) ? *playing_pref() : HAND_UNKNOWN,
        .bowling_hand = W.selected == WORKOUT_CRICKET ? nos_prefs.sports_bowling : HAND_UNKNOWN,
        .auto_resume = nos_prefs.sports_auto_resume,
        .mass_kg = nos_prefs.sports_mass_kg,
        .placement_source = (uint8_t)prefs_wrist_source()
    };
    workout_setup_set(&s);
    /* The body profile is frozen with the session like the rest of the setup (energy.h). */
    workout_body b = { nos_prefs.body_height_cm, (uint8_t)energy_age_from_birth(nos_prefs.body_birth_year, body_year()),
                       nos_prefs.body_sex <= ENERGY_SEX_FEMALE ? nos_prefs.body_sex : 0, 0 };
    workout_body_set(&b);
    workout_placement_revision_set(prefs_wrist_revision());
}
static int outdoor(int sport)
{
    const workout_profile *p = profile(sport);
    return p && p->outdoor;
}
static int uses_steps(int sport)
{
    const workout_profile *p = profile(sport);
    return p && p->uses_steps;
}
static int show_speed(int sport)
{
    const workout_profile *p = profile(sport);
    return p && p->show_speed;
}
static void elapsed_text(char *b, size_t n, uint32_t ms)
{
    unsigned seconds = (unsigned)(ms / 1000);
    if (seconds >= 3600)
        snprintf(b, n, "%u:%02u:%02u", seconds / 3600, seconds / 60 % 60, seconds % 60);
    else snprintf(b, n, "%02u:%02u", seconds / 60, seconds % 60);
}
static void pace_text(char *b, size_t n, float seconds)
{
    if (!isfinite(seconds) || seconds <= 0 || seconds >= 6000) snprintf(b, n, UI_NONE);
    else {
        unsigned s = (unsigned)lroundf(seconds);
        snprintf(b, n, "%u:%02u", s / 60, s % 60);
    }
}
static double distance_unit(int metric) { return metric ? 1000.0 : 1609.344; }
static double speed_unit(int metric) { return metric ? 3.6 : 2.2369362921; }
static float pace_unit(int metric) { return metric ? 1.0f : 1.609344f; }
static const char *distance_label(int metric) { return metric ? "KM" : "MI"; }
static void distance_text(char *b, size_t n, float metres, int metric)
{
    double value = (double)metres / distance_unit(metric);
    snprintf(b, n, value < 999.5 ? "%.2f" : "%.1f", value);
}
/* The energy figure on the live/summary/history screens is ACTIVE kcal: above resting, so the
 * daily Activity total can add it without counting resting energy twice. Records from before
 * the energy basis was stored carry only the gross estimate. The hint says what the number
 * rests on and is never hidden: a missing weight reads SET WEIGHT, not a blank. */
static float active_kcal(const workout_snapshot *s) { return s->energy_basis ? s->energy_net_kcal : s->energy_kcal_est; }
static const char *energy_how(const workout_snapshot *s)
{
    return s->energy_basis & ENERGY_BASIS_HR ? "HEART RATE" : s->energy_basis & ENERGY_BASIS_MOVE ? "MOVEMENT" : "CATEGORY MET";
}
static void energy_line(char *b, size_t n, const workout_snapshot *s)
{
    snprintf(b, n, "ACTIVE %.0f KCAL  /  %s", (double)active_kcal(s), s->setup.mass_kg ? energy_how(s) : "SET WEIGHT");
}
static void number(gfx_fb *fb, int x, int y, const char *value, float pitch, uint16_t ink)
{
    dot_style st = { DOT_FONT_TALL, pitch, pitch * 0.8f, ink, 0, 1, 1 };
    dot_text_a(fb, &st, (float)x, (float)y, value, UI_CENTER);
}
static void micro(gfx_fb *fb, int x, int y, const char *value, uint16_t ink, int align)
{
    ui_text(fb, UI_F_SMALL, x, y, value, ink, align, 0);
}
static void clear_press(void)
{
    W.pressed = HIT_NONE;
    W.release_at = 0;
}
static void press_tick(void)
{
    if ((W.release_at > 0 && nos_t >= W.release_at) || nos_touch_travel() > 12) clear_press();
}
static int press_input(const nos_input *in, int hit)
{
    if(in->type==IN_CANCEL) {clear_press();return 1;}
    if (in->type == IN_DOWN) {
        W.pressed = hit; W.press_t = nos_t; W.release_at = 0;
        return hit != HIT_NONE;
    }
    if (in->type == IN_UP) {
        if (W.pressed != HIT_NONE) W.release_at = fmax(nos_t, W.press_t + 0.1f);
        return hit != HIT_NONE;
    }
    if (in->type == IN_DRAG_START) clear_press();
    return 0;
}
static int pair_hit(int x, int y)
{
    if (abs(y - UI_BTN_Y) > UI_BTN_H / 2) return HIT_NONE;
    if (abs(x - UI_BTN_PAIR_X0) <= UI_BTN_PAIR_W / 2) return HIT_LEFT;
    if (abs(x - UI_BTN_PAIR_X1) <= UI_BTN_PAIR_W / 2) return HIT_RIGHT;
    return HIT_NONE;
}
static void buttons(gfx_fb *fb, const char *left, const char *right, int right_kind)
{
    ui_button(fb, UI_BTN_PAIR_X0, UI_BTN_Y, UI_BTN_PAIR_W, UI_BTN_H,
              left, BTN_SECONDARY, W.pressed == HIT_LEFT);
    ui_button(fb, UI_BTN_PAIR_X1, UI_BTN_Y, UI_BTN_PAIR_W, UI_BTN_H,
              right, right_kind, W.pressed == HIT_RIGHT);
}
static void list_header(gfx_fb *fb, const char *title, const char *subtitle)
{
    ui_fblur_coverage_exclude(fb, 0, 0, GFX_W, LIST_TOP - 2);
    gfx_fill_rect(fb, 0, 0, GFX_W, LIST_TOP - 2, C_BG);
    ui_title(fb, title, 0);
    if (subtitle && *subtitle) micro(fb, UI_CX, 98, subtitle, C_DIS, UI_CENTER);
}
static void workout_list_tick(ui_list *list, float dt)
{
    /* Short bounded lists should return from their rubber band promptly. Keep
     * the standard pixel snap and release velocity, with a shorter response so
     * the final rounded text positions do not linger for three 1px frames. */
    if (list->sc.springing && !list->sc.dragging && list->sc.sp.response >= 0.38f) {
        float distance = fabsf(list->sc.sp.x - list->sc.sp.target);
        list->sc.sp.response = distance < 24.0f ? 0.22f : 0.30f;
        list->sc.sp.damping = 1.0f;
    }
    ui_list_tick(list, dt);
}
static void leaf_open(const nos_screen *screen, int x, int y)
{
    /* A local iris exposes one live layer and uses the engine's blurred edge.
     * It avoids the two full-screen PSRAM captures of a programmatic push. */
    /* Very low bezel controls would require a long, slow reveal. Keep those
     * reveals centered; profile icons retain their own visual origin. */
    int dx = x - UI_CX, dy = y - UI_CY;
    if (dx * dx + dy * dy > 170 * 170) { x = UI_CX; y = UI_CY; }
    nos_iris_x = x < 0 ? 0 : x >= GFX_W ? GFX_W - 1 : x;
    nos_iris_y = y < 0 ? 0 : y >= GFX_H ? GFX_H - 1 : y;
    nos_iris_seed = 0;
    nos_push(screen, TX_IRIS);
}
static int sport_x(int sport) { return sport & 1 ? UI_BTN_PAIR_X1 : UI_BTN_PAIR_X0; }
static int sport_y(int sport) { return GRID_TOP + sport / 2 * GRID_STEP; }
static int choose_hit(int x, int y)
{
    for (int i = 0; i < 6; i++)
        if (abs(x - sport_x(i)) <= GRID_W / 2 && abs(y - sport_y(i)) <= GRID_H / 2) return HIT_SPORT_BASE + i;
    if (abs(x - UI_CX) <= 119 && abs(y - RECENT_Y) <= 24) return HIT_HISTORY;
    return HIT_NONE;
}

static void snapshot(void)
{
    W.session = *workout_get();
    workout_storage_get(&W.storage);
    W.metric = nos_prefs.units_metric != 0;
    const workout_snapshot *s = &W.session;
    elapsed_text(W.elapsed, sizeof W.elapsed, s->elapsed_ms);
    if (s->distance_valid) distance_text(W.distance, sizeof W.distance, s->distance_m, W.metric);
    else snprintf(W.distance, sizeof W.distance, UI_NONE);
    if (show_speed(s->sport)) {
        if (s->speed_valid) snprintf(W.primary, sizeof W.primary, "%.1f", (double)s->speed_mps * speed_unit(W.metric));
        else snprintf(W.primary, sizeof W.primary, UI_NONE);
        snprintf(W.metric_label, sizeof W.metric_label, W.metric ? "KM/H" : "MPH");
    } else {
        if (s->pace_valid) pace_text(W.primary, sizeof W.primary, s->pace_s_km * pace_unit(W.metric));
        else snprintf(W.primary, sizeof W.primary, UI_NONE);
        snprintf(W.metric_label, sizeof W.metric_label, W.metric ? "MIN/KM" : "MIN/MI");
    }
    if (s->hr_valid) snprintf(W.heart, sizeof W.heart, "%u", (unsigned)s->bpm);
    else snprintf(W.heart, sizeof W.heart, UI_NONE);
    if (uses_steps(s->sport) && s->cadence_valid)
        snprintf(W.cadence, sizeof W.cadence, "%.0f", (double)s->cadence_spm);
    else snprintf(W.cadence, sizeof W.cadence, UI_NONE);
    if (uses_steps(s->sport) && (s->steps || s->cadence_valid))
        snprintf(W.steps, sizeof W.steps, "%lu", (unsigned long)s->steps);
    else snprintf(W.steps, sizeof W.steps, UI_NONE);
    if (s->distance_valid && s->avg_speed_mps > 0)
        snprintf(W.avg_speed, sizeof W.avg_speed, "%.1f", (double)s->avg_speed_mps * speed_unit(W.metric));
    else snprintf(W.avg_speed, sizeof W.avg_speed, UI_NONE);
    if (show_speed(s->sport) && s->distance_valid && s->avg_speed_mps > 0)
        snprintf(W.summary_avg, sizeof W.summary_avg, "%.1f", (double)s->avg_speed_mps * speed_unit(W.metric));
    else if (!show_speed(s->sport) && s->distance_valid)
        pace_text(W.summary_avg, sizeof W.summary_avg, s->avg_pace_s_km * pace_unit(W.metric));
    else snprintf(W.summary_avg, sizeof W.summary_avg, UI_NONE);
    if (s->state == WORKOUT_PAUSED) snprintf(W.status, sizeof W.status, s->recovered ? "RECOVERED / PAUSED" : "PAUSED / TIME STOPPED");
    else if (!outdoor(s->sport)) snprintf(W.status, sizeof W.status, "RECORDING / INDOOR");
    else switch (s->gps_status) {
    case WORKOUT_GPS_GOOD: snprintf(W.status, sizeof W.status, "RECORDING / GPS READY"); break;
    case WORKOUT_GPS_OFF: snprintf(W.status, sizeof W.status, "RECORDING / NO GPS"); break;
    case WORKOUT_GPS_WAITING: snprintf(W.status, sizeof W.status, "RECORDING / WAITING FOR GPS"); break;
    case WORKOUT_GPS_STALE: snprintf(W.status, sizeof W.status, "RECORDING / GPS LOST"); break;
    case WORKOUT_GPS_POOR: snprintf(W.status, sizeof W.status, "RECORDING / WEAK GPS"); break;
    default: snprintf(W.status, sizeof W.status, "RECORDING / GPS FILTERED"); break;
    }
    energy_line(W.energy, sizeof W.energy, s);
    snprintf(W.lap_label, sizeof W.lap_label, "%u LAP%s  /  DETAILS", (unsigned)s->nlaps, s->nlaps == 1 ? "" : "S");
    if (s->hr_active_ms && s->avg_bpm)
        snprintf(W.average, sizeof W.average, "AVG HR %u BPM  /  %u LAP%s", (unsigned)s->avg_bpm, (unsigned)s->nlaps, s->nlaps == 1 ? "" : "S");
    else snprintf(W.average, sizeof W.average, "HR --  /  %u LAP%s", (unsigned)s->nlaps, s->nlaps == 1 ? "" : "S");
    unsigned coverage = s->elapsed_ms ? (unsigned)((uint64_t)s->gps_active_ms * 100u / s->elapsed_ms) : 0;
    if (coverage > 100) coverage = 100;
    snprintf(W.gps_coverage, sizeof W.gps_coverage, "GPS %u%% %s", coverage,
             coverage < 100 ? "/ PARTIAL" : "COVERAGE");
}
static void choose_draw(gfx_fb *fb)
{
    ui_title(fb, "SPORTS", 0);
    micro(fb, UI_CX, 98, W.storage.ready ? (W.storage.foreign ? "ORIGINAL SD NEEDED TO SAVE" : "CHOOSE YOUR SESSION") :
          W.storage.errors ? "SAVED HISTORY UNAVAILABLE" : "LOADING SAVED SESSIONS", C_DIS, UI_CENTER);
    for (int i = 0; i < 6; i++) {
        int x = sport_x(i), y = sport_y(i), pressed = W.pressed == HIT_SPORT_BASE + i;
        ui_rrect(fb, x - GRID_W / 2, y - GRID_H / 2, GRID_W, GRID_H, 26, pressed ? C_LINE2 : C_SURF2);
        dot_icon(fb, profile_icon(favorites[i]), x, y - 12, 2.5f, 2.0f, C_PRI);
        ui_label(fb, x, y + 25, favorites[i] < 0 ? "40 SPORTS" : profile_name(favorites[i]), C_PRI, UI_CENTER);
    }
    ui_button(fb, UI_CX, RECENT_Y, 238, 48, "RECENT WORKOUTS", BTN_GHOST, W.pressed == HIT_HISTORY);
}
static void choose_tap(int i)
{
    if (i == WORKOUT_SPORT_COUNT) {
        if (!history_state) {
            history_state = nos_mem_alloc(sizeof *history_state, "workout.ui");
            if (!history_state) { nos_feedback(FB_ERROR); nos_toast("HISTORY UNAVAILABLE"); return; }
            memset(history_state, 0, sizeof *history_state);
        }
        leaf_open(&history_screen, UI_CX, RECENT_Y);
        return;
    }
    if (i < 0 || i >= 6) return;
    if (favorites[i] < 0) { leaf_open(&catalog_screen, sport_x(i), sport_y(i)); return; }
    W.selected = favorites[i];
    leaf_open(&ready_screen, sport_x(i), sport_y(i) - 12);
}
static int view_for_session(void)
{
    if (W.session.state == WORKOUT_RUNNING || W.session.state == WORKOUT_PAUSED) return VIEW_LIVE;
    if (W.session.state == WORKOUT_FINISHED &&
        (W.session.save_state != WORKOUT_SAVE_SAVED || W.dismissed_id != W.session.id)) return VIEW_SUMMARY;
    return VIEW_CHOOSE;
}
static void main_enter(void)
{
    if (!W.initialized) {
        for (int i = 0; i < WORKOUT_SPORT_COUNT; i++) profiles[i] = workout_profile_get(i);
        W.initialized = 1;
    }
    snapshot();
    W.view = view_for_session();
    clear_press();
}
static void main_tick(float dt)
{
    snapshot();
    W.view = view_for_session();
    press_tick();
    (void)dt;
}
static void live_draw(gfx_fb *fb)
{
    const workout_snapshot *s = &W.session;
    int paused = s->state == WORKOUT_PAUSED;
    ui_title(fb, profile_name(s->sport), 0);
    micro(fb, UI_CX, 98, W.status, C_SEC, UI_CENTER);
    if (!paused) dot_disc(fb, 92, 69, 8, C_RED);
    /* The outdoor face carries four figures plus the energy row, so its clock is a size smaller. */
    int out = outdoor(s->sport);
    float pitch = out || strlen(W.elapsed) > 7 ? 6.0f : 7.0f;
    number(fb, UI_CX, 111, W.elapsed, pitch, paused ? C_SEC : C_WHITE);
    micro(fb, UI_CX, out ? 182 : 192, "ACTIVE TIME", C_DIS, UI_CENTER);
    if (out) {
        number(fb, 147, 190, W.distance, 4.1f, C_PRI);
        number(fb, 319, 190, W.primary, 4.1f, C_PRI);
        micro(fb, 147, 245, distance_label(W.metric), C_SEC, UI_CENTER);
        micro(fb, 319, 245, W.metric_label, C_SEC, UI_CENTER);
        number(fb, 147, 253, W.heart, 3.1f, C_PRI);
        number(fb, 319, 253, uses_steps(s->sport) ? W.cadence : W.avg_speed, 3.1f, C_PRI);
        micro(fb, 147, 299, "BPM", C_DIS, UI_CENTER);
        micro(fb, 319, 299, uses_steps(s->sport) ? "STEPS/MIN" : W.metric ? "AVG KM/H" : "AVG MPH", C_DIS, UI_CENTER);
        micro(fb, UI_CX, 320, W.energy, C_SEC, UI_CENTER);
    } else if (uses_steps(s->sport)) {
        number(fb, 147, 218, W.heart, 4.4f, C_PRI);
        number(fb, 319, 218, W.steps, 4.4f, C_PRI);
        micro(fb, 147, 275, "BPM", C_SEC, UI_CENTER);
        micro(fb, 319, 275, "STEPS", C_SEC, UI_CENTER);
        char b[40];
        snprintf(b, sizeof b, "%s STEPS/MIN  /  NO GPS", W.cadence);
        micro(fb, UI_CX, 298, b, C_DIS, UI_CENTER);
        micro(fb, UI_CX, 320, W.energy, C_SEC, UI_CENTER);
    } else {
        char bursts[20], placement[48];
        if (s->motion_valid_ms) snprintf(bursts, sizeof bursts, "%lu", (unsigned long)s->movement_bursts);
        else snprintf(bursts, sizeof bursts, UI_NONE);
        number(fb, 147, 220, W.heart, 4.4f, C_PRI);
        number(fb, 319, 220, bursts, 4.4f, C_PRI);
        micro(fb, 147, 278, "BPM", C_SEC, UI_CENTER);
        micro(fb, 319, 278, "BURSTS / EST.", C_SEC, UI_CENTER);
        snprintf(placement, sizeof placement, "%s WRIST / %s PLAY", hand_name(s->setup.worn_wrist), hand_name(s->setup.playing_hand));
        micro(fb, UI_CX, 298, uses_playing_hand(s->sport) ? placement : "WRIST MOVEMENT / NOT REPS", C_DIS, UI_CENTER);
        micro(fb, UI_CX, 320, W.energy, C_SEC, UI_CENTER);
    }
    buttons(fb, paused ? "FINISH" : "LAP", paused ? "RESUME" : "PAUSE", BTN_PRIMARY);
    micro(fb, UI_CX, 438, W.storage.foreign ? "ORIGINAL SD / DETAILS" :
          !W.storage.available && W.storage.errors ? "SD RETRY / DETAILS" : W.lap_label, C_DIS, UI_CENTER);
}
static void summary_draw(gfx_fb *fb)
{
    const workout_snapshot *s = &W.session;
    const char *status = s->save_state == WORKOUT_SAVE_SAVED ? "SAVED TO SD" :
                         W.storage.foreign ? "INSERT ORIGINAL SD / RETRY" :
                         s->save_state == WORKOUT_SAVE_FAILED ? "SAVE FAILED / RETRY" : "SAVING TO SD";
    ui_title(fb, profile_name(s->sport), 0);
    micro(fb, UI_CX, 100, status, s->save_state == WORKOUT_SAVE_FAILED ? C_RED : C_SEC, UI_CENTER);
    number(fb, UI_CX, 123, W.elapsed, strlen(W.elapsed) > 7 ? 5.6f : 6.4f, C_WHITE);
    micro(fb, UI_CX, 204, "ACTIVE TIME", C_DIS, UI_CENTER);
    if (outdoor(s->sport)) {
        number(fb, 147, 224, W.distance, 4.0f, C_PRI);
        number(fb, 319, 224, W.summary_avg, 4.0f, C_PRI);
        micro(fb, 147, 277, distance_label(W.metric), C_SEC, UI_CENTER);
        micro(fb, 319, 277, show_speed(s->sport) ? (W.metric ? "GPS AVG KM/H" : "GPS AVG MPH") :
              (W.metric ? "GPS AVG /KM" : "GPS AVG /MI"), C_SEC, UI_CENTER);
    } else {
        char wall[24];
        elapsed_text(wall, sizeof wall, s->total_ms);
        micro(fb, UI_CX, 244, "TOTAL DURATION", C_DIS, UI_CENTER);
        ui_text(fb, UI_F_BODY, UI_CX, 277, wall, C_PRI, UI_CENTER, 0);
    }
    micro(fb, UI_CX, 298, W.average, C_SEC, UI_CENTER);
    micro(fb, UI_CX, 320, W.energy, C_SEC, UI_CENTER);
    if (s->save_state == WORKOUT_SAVE_FAILED) buttons(fb, "RETRY", "DETAILS", BTN_PRIMARY);
    else buttons(fb, "DETAILS", s->save_state == WORKOUT_SAVE_SAVED ? "DONE" : "CLOSE", BTN_PRIMARY);
    if (s->save_state != WORKOUT_SAVE_SAVED)
        micro(fb, UI_CX, 438, "SESSION KEPT IN MEMORY", C_DIS, UI_CENTER);
    else if (outdoor(s->sport)) micro(fb, UI_CX, 438, W.gps_coverage, C_DIS, UI_CENTER);
}
static void main_draw(gfx_fb *fb)
{
    if (W.view == VIEW_LIVE) live_draw(fb);
    else if (W.view == VIEW_SUMMARY) summary_draw(fb);
    else choose_draw(fb);
}

/* Session command results confirm service transitions, never SD persistence. */
static int session_action(uint16_t action)
{
    const workout_snapshot *before = workout_get();
    uint32_t request = diag_action_request(DIAG_DOMAIN_SENSOR, action, (int32_t)before->id);
    diag_action_context(request, (int32_t)diag_log_name_id("SPORTS"), before->state);
    int accepted = action == DIAG_ACT_PAUSE ? workout_pause() : action == DIAG_ACT_RESUME ? workout_resume() :
                   action == DIAG_ACT_MARK ? workout_lap_add() : workout_finish();
    const workout_snapshot *after = workout_get();
    diag_action_result(request, accepted ? DIAG_RESULT_SUCCESS : DIAG_RESULT_REJECTED,
                       action == DIAG_ACT_MARK ? after->nlaps : after->state);
    return accepted;
}
static void session_retry(void)
{
    uint32_t request = diag_action_request(DIAG_DOMAIN_SENSOR, DIAG_ACT_SAVE, (int32_t)workout_get()->id);
    diag_action_context(request, (int32_t)diag_log_name_id("SPORTS"), workout_get()->save_state);
    workout_retry_save();
    diag_action_result(request, DIAG_RESULT_PENDING, workout_get()->save_state);
}
static void detail_open(const workout_snapshot *item, int x, int y);
static int main_input(const nos_input *in)
{
    if (W.view == VIEW_CHOOSE) {
        int hit = choose_hit(in->x, in->y);
        if (press_input(in, hit)) return 1;
        if (in->type == IN_TAP && hit != HIT_NONE) {
            nos_feedback(FB_TAP);
            choose_tap(hit == HIT_HISTORY ? WORKOUT_SPORT_COUNT : hit - HIT_SPORT_BASE);
            return 1;
        }
        return 0;
    }
    int hit = pair_hit(in->x, in->y);
    if (W.view == VIEW_LIVE && in->y >= 416 && in->y <= 455 && in->x >= 90 && in->x <= 376) hit = HIT_DETAILS;
    if (press_input(in, hit)) return 1;
    if (in->type != IN_TAP) return 0;
    if (W.view == VIEW_LIVE) {
        if (hit == HIT_DETAILS) detail_open(&W.session, UI_CX, 438);
        else if (W.session.state == WORKOUT_RUNNING) {
            if (hit == HIT_LEFT) {
                if (session_action(DIAG_ACT_MARK)) { nos_feedback(FB_TICK); nos_toast("LAP MARKED"); }
                else { nos_feedback(FB_LIMIT); nos_toast("LAP LIMIT REACHED"); }
            } else if (hit == HIT_RIGHT && session_action(DIAG_ACT_PAUSE)) nos_feedback(FB_TOGGLE_OFF);
        } else if (W.session.state == WORKOUT_PAUSED) {
            if (hit == HIT_LEFT) leaf_open(&finish_screen, UI_BTN_PAIR_X0, UI_BTN_Y);
            else if (hit == HIT_RIGHT && session_action(DIAG_ACT_RESUME)) nos_feedback(FB_TOGGLE_ON);
        }
    } else if (W.view == VIEW_SUMMARY) {
        if (W.session.save_state == WORKOUT_SAVE_FAILED) {
            if (hit == HIT_LEFT) { session_retry(); nos_toast(W.storage.foreign ? "RETRY ORIGINAL SD" : "RETRYING SAVE"); }
            else if (hit == HIT_RIGHT) detail_open(&W.session, UI_BTN_PAIR_X1, UI_BTN_Y);
        } else if (hit == HIT_LEFT) detail_open(&W.session, UI_BTN_PAIR_X0, UI_BTN_Y);
        else if (hit == HIT_RIGHT) {
            if (W.session.save_state == WORKOUT_SAVE_SAVED) { W.dismissed_id = W.session.id; W.view = VIEW_CHOOSE; clear_press(); }
            else nos_pop();
        }
    }
    return hit != HIT_NONE;
}

static void ready_enter(void)
{
    clear_press();
    configure_session();
    workout_prepare(W.selected);
    snapshot();
}
static void ready_leave(void) { workout_unprepare(); }
static void ready_tick(float dt)
{
    (void)dt;
    workout_prepare(W.selected);
    snapshot();
    press_tick();
}
static void ready_draw(gfx_fb *fb)
{
    int is_outdoor = outdoor(W.selected), gps_ready = W.session.gps_status == WORKOUT_GPS_GOOD;
    int storage_error = !W.storage.ready && W.storage.errors;
    const char *headline = "Ready when you are", *status, *note;
    uint16_t status_ink = C_SEC;
    if (storage_error) {
        headline = W.storage.corrupt ? "History unreadable" : "SD card unavailable";
        status = W.storage.corrupt ? "RESTORE A VALID SD BACKUP" : "INSERT CARD THEN RETRY";
        note = W.storage.corrupt ? "CARD FILES HAVE BEEN PRESERVED" : "SAVED HISTORY MUST LOAD FIRST";
        status_ink = C_RED;
    } else if (W.storage.foreign) {
        status = "ORIGINAL SD NEEDED TO SAVE";
        note = "SESSION WILL WAIT TO SAVE";
        status_ink = C_RED;
    } else if (is_outdoor) {
        status = gps_ready ? "GPS READY" : W.session.gps_status == WORKOUT_GPS_OFF ?
                 "GPS NOT AVAILABLE" : "WAITING FOR A GOOD GPS FIX";
        note = gps_ready ? "DISTANCE USES ACCEPTED GPS FIXES" : "START NOW; DISTANCE NEEDS GPS";
    } else {
        status = "INDOOR / GPS OFF";
        note = W.selected == WORKOUT_TREADMILL ? "STEP CADENCE WHEN AVAILABLE" : "ACTIVE TIME + VALID HEART RATE";
    }
    ui_title(fb, profile_name(W.selected), 0);
    dot_icon(fb, profile_icon(W.selected), UI_CX, 146, 6.2f, 4.9f, C_PRI);
    ui_text(fb, UI_F_TITLE, UI_CX, 229, headline, C_PRI, UI_CENTER, 0);
    micro(fb, UI_CX, 262, status, status_ink, UI_CENTER);
    char placement[64];
    if (uses_playing_hand(W.selected))
        snprintf(placement, sizeof placement, "%s WRIST / %s PLAY", nos_prefs.wrist_left ? "LEFT" : "RIGHT", hand_name(*playing_pref()));
    else snprintf(placement, sizeof placement, "%s WRIST", nos_prefs.wrist_left ? "LEFT" : "RIGHT");
    micro(fb, UI_CX, 286, storage_error ? note : placement, C_DIS, UI_CENTER);
    ui_button(fb, UI_CX, 307, 306, 30, "WRIST + HAND SETUP", BTN_GHOST, W.pressed == HIT_SETUP);
    const char *label = storage_error ? "RETRY" : !W.storage.ready ? "LOADING" : is_outdoor && !gps_ready ? "START ANYWAY" : "START";
    ui_button(fb, UI_CX, UI_BTN_Y, 238, UI_BTN_H, label,
              W.storage.ready || storage_error ? BTN_PRIMARY : BTN_SECONDARY, W.pressed == HIT_SINGLE);
    micro(fb, UI_CX, 438, !nos_prefs.sports_mass_kg ? "70 KG ASSUMED" :
          nos_prefs.sports_auto_resume ? "CRASH AUTO-RESUME ON" : "RECOVER PAUSED AFTER CRASH", C_DIS, UI_CENTER);
}
static void body_open(int field, int x, int y);
static int ready_input(const nos_input *in)
{
    int hit = abs(in->y - UI_BTN_Y) <= UI_BTN_H / 2 && abs(in->x - UI_CX) <= 119 ? HIT_SINGLE : HIT_NONE;
    if (abs(in->y - 307) <= 20 && abs(in->x - UI_CX) <= 153) hit = HIT_SETUP;
    if (press_input(in, hit)) return 1;
    if (in->type != IN_TAP) return 0;
    if (hit == HIT_SETUP) { leaf_open(&setup_screen, UI_CX, 327); return 1; }
    if (hit != HIT_SINGLE) return 0;
    uint32_t request = diag_action_request(DIAG_DOMAIN_SENSOR, DIAG_ACT_START, W.selected);
    diag_action_context(request, (int32_t)diag_log_name_id("SPORTS"), (int32_t)W.session.id);
    if (!W.storage.ready) {
        diag_action_result(request, DIAG_RESULT_REJECTED, W.storage.errors ? -1 : 0);
        if (W.storage.errors) { session_retry(); nos_toast("RETRYING SD CARD"); }
        else nos_toast("LOADING SAVED SESSIONS");
        return 1;
    }
    /* Calories need a weight: ask once before the first Start (a skip keeps the flagged 70 kg
     * default; the ENERGY row keeps saying SET WEIGHT until it is entered). */
    if (!nos_prefs.sports_mass_kg && !weight_asked) {
        weight_asked = 1;
        diag_action_result(request, DIAG_RESULT_REJECTED, -2);
        nos_toast("SET WEIGHT");
        body_open(BF_MASS, UI_CX, UI_BTN_Y);
        return 1;
    }
    configure_session();
    int accepted = workout_start(W.selected);
    diag_action_result(request, accepted ? DIAG_RESULT_SUCCESS : DIAG_RESULT_REJECTED, (int32_t)workout_get()->id);
    if (accepted) { nos_feedback(FB_TOGGLE_ON); nos_pop(); }
    else { nos_feedback(FB_ERROR); nos_toast("FINISH SAVING CURRENT SESSION"); }
    return 1;
}

static void catalog_row(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed)
{
    (void)x0; (void)x1;
    if (i < 0 || i >= WORKOUT_SPORT_COUNT) return;
    if (pressed) ui_rrect(fb, 68, y + 3, 330, h - 6, 24, ui_dim(C_SURF2, fade));
    dot_icon(fb, profile_icon(i), 95, y + 38, 2.4f, 1.9f, ui_dim(C_PRI, fade));
    ui_text_fit(fb, UI_F_BODY, 128, y + 31, profile_name(i), ui_dim(C_PRI, fade), UI_LEFT, 246);
    micro(fb, 128, y + 55, profile(i)->subtitle, ui_dim(C_DIS, fade), UI_LEFT);
    gfx_fill_rect(fb, 82, y + h - 1, 302, 1, ui_dim(C_LINE, fade));
}
static void catalog_tap(int i)
{
    if (i < 0 || i >= WORKOUT_SPORT_COUNT) return;
    W.selected = i;
    leaf_open(&ready_screen, UI_CX, UI_CY);
}
static void catalog_enter(void)
{
    if (!nos_revealed()) ui_list_init(&catalog_list, WORKOUT_SPORT_COUNT, 78, LIST_TOP, catalog_row, catalog_tap);
}
static void catalog_tick(float dt)
{
    workout_list_tick(&catalog_list, dt);
    const workout_snapshot *s = workout_get();
    if ((s->state == WORKOUT_RUNNING || s->state == WORKOUT_PAUSED) && !nos_transitioning()) nos_pop();
}
static void catalog_draw(gfx_fb *fb)
{
    ui_list_draw(fb, &catalog_list);
    list_header(fb, "ALL SPORTS", "40 PROFILES / SWIPE TO SCROLL");
}
static int catalog_input(const nos_input *in)
{
    if (in->type == IN_TAP && in->y < LIST_TOP) return 1;
    return ui_list_input(&catalog_list, in);
}

enum { SET_WRIST, SET_DOMINANT, SET_PLAYING, SET_BOWLING, SET_RESUME,
       SET_MASS, SET_DETECT, SET_LEARN_LEFT, SET_LEARN_RIGHT,
       SET_HEIGHT, SET_AGE, SET_SEX, SET_COUNT };   /* body profile appended: diagnostic ids stay fixed */
static wrist_detect_status wrist_status;
static const char *wrist_reason(int reason)
{
    switch (reason) {
    case WRIST_REASON_MISSING_REFS: return "Learn both wrists first";
    case WRIST_REASON_AMBIGUOUS_REFS: return "References overlap; learn again";
    case WRIST_REASON_GAP: return "Sensor gap; please retry";
    case WRIST_REASON_SENSOR: return "Invalid sensor sample";
    case WRIST_REASON_POSE: return "Start with the screen facing up";
    case WRIST_REASON_NOT_QUIET: return "Hold still at the start and end";
    case WRIST_REASON_ARC: return "Turn your palm fully upward";
    case WRIST_REASON_MIXED_AXIS: return "Rotate only your forearm";
    case WRIST_REASON_NO_MATCH: return "No reliable reference match";
    default: return "Wrist could not be determined";
    }
}
static void setup_text(int i, const char **label, char *value, size_t n)
{
    switch (i) {
    case SET_WRIST: *label = "WATCH WRIST"; snprintf(value, n, "%s / tap to change", nos_prefs.wrist_left ? "Left" : "Right"); break;
    case SET_DOMINANT: *label = "DOMINANT HAND"; snprintf(value, n, "%s", hand_name(nos_prefs.sports_dominant)); break;
    case SET_PLAYING:
        *label = W.selected == WORKOUT_CRICKET ? "BATTING HAND" : "PLAYING HAND";
        snprintf(value, n, "%s", uses_playing_hand(W.selected) ? hand_name(*playing_pref()) : "Not used for this sport"); break;
    case SET_BOWLING: *label = "BOWLING HAND"; snprintf(value, n, "%s", W.selected == WORKOUT_CRICKET ? hand_name(nos_prefs.sports_bowling) : "Cricket only"); break;
    case SET_RESUME: *label = "RESUME AFTER CRASH"; snprintf(value, n, "%s", nos_prefs.sports_auto_resume ? "Automatic / excludes downtime" : "Recover paused"); break;
    case SET_MASS:
        *label = "BODY MASS / ENERGY ESTIMATE";
        if (nos_prefs.sports_mass_kg) snprintf(value, n, "%u kg / tap to edit", nos_prefs.sports_mass_kg);
        else snprintf(value, n, "Not set / 70 kg used");
        break;
    case SET_HEIGHT:
        *label = "HEIGHT / STRIDE + RESTING";
        if (nos_prefs.body_height_cm) snprintf(value, n, "%u cm / tap to edit", nos_prefs.body_height_cm);
        else snprintf(value, n, "Not set / 170 cm used");
        break;
    case SET_AGE:
        *label = "AGE / RESTING ENERGY";
        if (nos_prefs.body_birth_year) snprintf(value, n, "%d years / tap to edit", energy_age_from_birth(nos_prefs.body_birth_year, body_year()));
        else snprintf(value, n, "Not set / 30 used");
        break;
    case SET_SEX:
        *label = "SEX / ENERGY FORMULA";
        snprintf(value, n, "%s", nos_prefs.body_sex == ENERGY_SEX_MALE ? "Male / tap to change" :
                 nos_prefs.body_sex == ENERGY_SEX_FEMALE ? "Female / tap to change" : "Not set / average");
        break;
    case SET_DETECT: *label = "GUIDED WRIST DETECTION"; snprintf(value, n, "Experimental / confirm result"); break;
    case SET_LEARN_LEFT: *label = "LEARN LEFT WRIST"; snprintf(value, n, "%s", nos_prefs.wrist_refs[0] ? "Reference saved / tap to repeat" : "Wear on LEFT; learn gesture"); break;
    default: *label = "LEARN RIGHT WRIST"; snprintf(value, n, "%s", nos_prefs.wrist_refs[2] ? "Reference saved / tap to repeat" : "Wear on RIGHT; learn gesture"); break;
    }
}
static void setup_row(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed)
{
    (void)x0; (void)x1;
    const char *label; char value[64]; setup_text(i, &label, value, sizeof value);
    if (pressed) ui_rrect(fb, 68, y + 3, 330, h - 6, 24, ui_dim(C_SURF2, fade));
    micro(fb, 76, y + 25, label, ui_dim(C_DIS, fade), UI_LEFT);
    ui_text_fit(fb, UI_F_BODY, 76, y + 56, value, ui_dim(C_PRI, fade), UI_LEFT, 316);
    gfx_fill_rect(fb, 76, y + h - 1, 314, 1, ui_dim(C_LINE, fade));
}
static int setup_value(int i)
{
    return i == SET_WRIST ? nos_prefs.wrist_left : i == SET_DOMINANT ? nos_prefs.sports_dominant :
           i == SET_MASS ? nos_prefs.sports_mass_kg : i == SET_HEIGHT ? nos_prefs.body_height_cm :
           i == SET_AGE ? energy_age_from_birth(nos_prefs.body_birth_year, body_year()) : i == SET_SEX ? nos_prefs.body_sex :
           i == SET_PLAYING ? *playing_pref() : i == SET_BOWLING ? nos_prefs.sports_bowling : nos_prefs.sports_auto_resume;
}
static void setup_tap(int i)
{
    if (i < 0 || i >= SET_COUNT) return;
    int old = setup_value(i);
    switch (i) {
    case SET_WRIST: prefs_set_wrist(!nos_prefs.wrist_left,PREFS_WRIST_SOURCE_MANUAL); break;
    case SET_DOMINANT: nos_prefs.sports_dominant = (nos_prefs.sports_dominant + 1) % 3; break;
    case SET_PLAYING:
        if (!uses_playing_hand(W.selected)) return;
        *playing_pref() = (*playing_pref() + 1) % 4; break;
    case SET_BOWLING:
        if (W.selected != WORKOUT_CRICKET) return;
        nos_prefs.sports_bowling = (nos_prefs.sports_bowling + 1) % 3; break;
    case SET_RESUME: nos_prefs.sports_auto_resume = !nos_prefs.sports_auto_resume; break;
    case SET_MASS: body_open(BF_MASS, UI_CX, UI_CY); return;
    case SET_HEIGHT: body_open(BF_HEIGHT, UI_CX, UI_CY); return;
    case SET_AGE: body_open(BF_AGE, UI_CX, UI_CY); return;
    case SET_SEX: nos_prefs.body_sex = (uint8_t)((nos_prefs.body_sex + 1) % 3); break;
    default:
        wrist_mode = i == SET_DETECT ? WRIST_DETECT : i == SET_LEARN_LEFT ? WRIST_LEARN_LEFT : WRIST_LEARN_RIGHT;
        leaf_open(&wrist_screen, UI_CX, UI_CY); return;
    }
    prefs_save(); configure_session(); ui_fblur_invalidate(&setup_list, i, i);
    uint32_t request = diag_action_request(DIAG_DOMAIN_CONTROL, DIAG_ACT_SET, 0x7000 + i);
    diag_action_context(request, (int32_t)diag_log_name_id("sports.setup"), old);
    if (i == SET_WRIST) diag_action_context(request, PREFS_WRIST_SOURCE_MANUAL, (int32_t)prefs_wrist_revision());
    diag_action_result(request, DIAG_RESULT_SUCCESS, setup_value(i));
    nos_feedback(FB_TICK);
}
static void setup_enter(void)
{
    if (!nos_revealed()) ui_list_init(&setup_list, SET_COUNT, 78, LIST_TOP, setup_row, setup_tap);
    else ui_fblur_invalidate(&setup_list, 0, SET_COUNT - 1);
}
static void setup_tick(float dt) { workout_list_tick(&setup_list, dt); }
static void setup_draw(gfx_fb *fb)
{
    ui_list_draw(fb, &setup_list);
    list_header(fb, "SESSION SETUP", "WRIST AND PLAYING HAND ARE SEPARATE");
}
static int setup_input(const nos_input *in)
{
    if (in->type == IN_TAP && in->y < LIST_TOP) return 1;
    return ui_list_input(&setup_list, in);
}
/* Weight, height and age share one editor: -/+ steps, long press = 10, SAVE, or CLEAR to unset. */
static const struct { int lo, hi, def; const char *title, *ask, *unit, *what; } BF[] = {
    { 20, 250, 70, "BODY MASS", "ENTER YOUR OWN WEIGHT", "KG", "KG / CALORIE ESTIMATES" },
    { 100, 230, 170, "BODY HEIGHT", "ENTER YOUR HEIGHT", "CM", "CM / STRIDE + RESTING" },
    { 5, 120, 30, "BODY AGE", "ENTER YOUR AGE", "YR", "YEARS / RESTING ENERGY" },
};
static void body_open(int field, int x, int y)
{
    body_field = field;
    int v = field == BF_MASS ? nos_prefs.sports_mass_kg : field == BF_HEIGHT ? nos_prefs.body_height_cm :
            energy_age_from_birth(nos_prefs.body_birth_year, body_year());
    mass_draft = v ? v : BF[field].def;
    leaf_open(field == BF_MASS ? &mass_screen : field == BF_HEIGHT ? &height_screen : &age_screen, x, y);
}
static void mass_enter(void) { clear_press(); }
static void mass_tick(float dt) { (void)dt; press_tick(); }
static void mass_draw(gfx_fb *fb)
{
    char b[12]; snprintf(b, sizeof b, "%d", mass_draft);
    ui_title(fb, BF[body_field].title, 0);
    micro(fb, UI_CX, 110, BF[body_field].ask, C_DIS, UI_CENTER);
    number(fb, UI_CX, 148, b, 7.0f, C_PRI);
    micro(fb, UI_CX, 230, BF[body_field].what, C_DIS, UI_CENTER);
    const char *noun = body_field == BF_MASS ? "WEIGHT" : body_field == BF_HEIGHT ? "HEIGHT" : "AGE";
    char minus[16], plus[16], save[24], clear[24];
    snprintf(minus, sizeof minus, "- 1 %s", BF[body_field].unit);
    snprintf(plus, sizeof plus, "+ 1 %s", BF[body_field].unit);
    snprintf(save, sizeof save, "SAVE %s", noun);
    snprintf(clear, sizeof clear, "CLEAR %s", noun);
    ui_button(fb, UI_BTN_PAIR_X0, 292, 112, 48, minus, BTN_SECONDARY, W.pressed == HIT_LEFT);
    ui_button(fb, UI_BTN_PAIR_X1, 292, 112, 48, plus, BTN_SECONDARY, W.pressed == HIT_RIGHT);
    ui_button(fb, UI_CX, UI_BTN_Y, 238, UI_BTN_H, save, BTN_PRIMARY, W.pressed == HIT_SINGLE);
    micro(fb, UI_CX, 438, clear, C_DIS, UI_CENTER);
}
static int mass_input(const nos_input *in)
{
    int hit = abs(in->y - 292) <= 24 ? (in->x < UI_CX ? HIT_LEFT : HIT_RIGHT) :
        abs(in->y - UI_BTN_Y) <= UI_BTN_H / 2 ? HIT_SINGLE : in->y > 420 ? HIT_DETAILS : HIT_NONE;
    if (press_input(in, hit)) return 1;
    if (in->type != IN_TAP && in->type != IN_LONG) return 0;
    int step = in->type == IN_LONG ? 10 : 1;
    if (hit == HIT_LEFT) mass_draft -= step;
    if (hit == HIT_RIGHT) mass_draft += step;
    if (mass_draft < BF[body_field].lo) mass_draft = BF[body_field].lo;
    if (mass_draft > BF[body_field].hi) mass_draft = BF[body_field].hi;
    if (hit == HIT_SINGLE || hit == HIT_DETAILS) {
        int v = hit == HIT_SINGLE ? mass_draft : 0;
        if (body_field == BF_MASS) nos_prefs.sports_mass_kg = (uint8_t)v;
        else if (body_field == BF_HEIGHT) nos_prefs.body_height_cm = (uint8_t)v;
        else nos_prefs.body_birth_year = (uint16_t)(v ? body_year() - v : 0);
        prefs_save(); configure_session(); nos_pop();
    }
    return hit != HIT_NONE;
}
static void wrist_enter(void)
{
    clear_press(); wrist_started = 0;
    memset(&wrist_status, 0, sizeof wrist_status);
}
static void wrist_leave(void) { wrist_detect_cancel(); }
static void wrist_tick(float dt)
{
    (void)dt; press_tick();
    if (wrist_started) wrist_detect_get(&wrist_status);
    if (wrist_status.state == WRIST_CAPTURING) nos_keep_awake();
}
static void wrist_draw(gfx_fb *fb)
{
    int valid = wrist_status.state == WRIST_VALID;
    ui_title(fb, wrist_mode == WRIST_LEARN_LEFT ? "LEARN LEFT WRIST" :
                 wrist_mode == WRIST_LEARN_RIGHT ? "LEARN RIGHT WRIST" : "DETECT WRIST", 0);
    micro(fb, UI_CX, 105, "GUIDED / EXPERIMENTAL", C_DIS, UI_CENTER);
    if (wrist_status.state == WRIST_CAPTURING) {
        char time[12]; snprintf(time, sizeof time, "%.1f", wrist_status.remaining_ms / 1000.0);
        number(fb, UI_CX, 151, time, 7.0f, C_PRI);
        ui_text_fit(fb, UI_F_BODY, UI_CX, 268, wrist_status.remaining_ms > 3400 ? "Hold still" :
                    wrist_status.remaining_ms > 600 ? "Rotate palm upward once" : "Hold still again",
                    C_PRI, UI_CENTER, 330);
    } else if (valid) {
        ui_text(fb, UI_F_TITLE, UI_CX, 190, wrist_mode == WRIST_DETECT ?
                (wrist_status.candidate == WRIST_CANDIDATE_LEFT ? "Left wrist suggested" : "Right wrist suggested") :
                "Gesture captured", C_PRI, UI_CENTER, 0);
        micro(fb, UI_CX, 241, "CONFIRM BEFORE USING THIS RESULT", C_SEC, UI_CENTER);
        micro(fb, UI_CX, 274, "KEEP THE SAME STRAP ORIENTATION", C_DIS, UI_CENTER);
    } else {
        ui_text_fit(fb, UI_F_BODY, UI_CX, 160, wrist_started ? wrist_reason(wrist_status.reason) :
                    wrist_mode == WRIST_LEARN_LEFT ? "Wear the watch on your LEFT wrist" :
                    wrist_mode == WRIST_LEARN_RIGHT ? "Wear the watch on your RIGHT wrist" :
                    "Use your learned strap orientation", C_PRI, UI_CENTER, 330);
        micro(fb, UI_CX, 211, "SCREEN UP / 12 O'CLOCK TOWARD ELBOW", C_SEC, UI_CENTER);
        micro(fb, UI_CX, 244, "HOLD STILL, THEN TURN YOUR PALM UP", C_DIS, UI_CENTER);
        micro(fb, UI_CX, 277, "STOP AND HOLD UNTIL THE TIMER ENDS", C_DIS, UI_CENTER);
    }
    if (wrist_status.state != WRIST_CAPTURING)
        ui_button(fb, UI_CX, UI_BTN_Y, 238, UI_BTN_H, valid ?
                  (wrist_mode == WRIST_DETECT ? "CONFIRM WRIST" : "SAVE REFERENCE") : wrist_started ? "TRY AGAIN" : "BEGIN",
                  BTN_PRIMARY, W.pressed == HIT_SINGLE);
    micro(fb, UI_CX, 438, "BACK CANCELS / MANUAL WRIST IS AVAILABLE", C_DIS, UI_CENTER);
}
static int wrist_input(const nos_input *in)
{
    int hit = abs(in->y - UI_BTN_Y) <= UI_BTN_H / 2 && abs(in->x - UI_CX) <= 119 ? HIT_SINGLE : HIT_NONE;
    if (press_input(in, hit)) return 1;
    if (in->type != IN_TAP || hit != HIT_SINGLE || wrist_status.state == WRIST_CAPTURING) return 0;
    if (wrist_status.state == WRIST_VALID) {
        if (wrist_mode == WRIST_DETECT) {
            prefs_set_wrist(wrist_status.candidate == WRIST_CANDIDATE_LEFT,PREFS_WRIST_SOURCE_GUIDED);
        } else {
            int index = wrist_mode == WRIST_LEARN_LEFT ? 0 : 2;
            nos_prefs.wrist_refs[index] = wrist_status.signature;
            nos_prefs.wrist_refs[index + 1] = wrist_status.arc;
        }
        prefs_save(); configure_session();
        uint32_t request = diag_action_request(DIAG_DOMAIN_CONTROL, DIAG_ACT_SET, 0x7000 + SET_DETECT);
        diag_action_context(request, (int32_t)diag_log_name_id("sports.wrist"), wrist_mode);
        diag_action_context(request, PREFS_WRIST_SOURCE_GUIDED, (int32_t)prefs_wrist_revision());
        diag_action_result(request, DIAG_RESULT_SUCCESS, wrist_status.candidate);
        nos_pop();
    } else {
        wrist_started = 1;
        uint32_t request = diag_action_request(DIAG_DOMAIN_SENSOR, DIAG_ACT_START, 0x7000 + SET_DETECT);
        diag_action_context(request, (int32_t)diag_log_name_id("sports.wrist"), wrist_mode);
        int accepted = wrist_detect_begin(wrist_mode, nos_prefs.wrist_refs);
        diag_action_result(request, accepted ? DIAG_RESULT_PENDING : DIAG_RESULT_REJECTED, accepted);
        wrist_detect_get(&wrist_status);
    }
    return 1;
}

static void finish_enter(void) { clear_press(); snapshot(); }
static void finish_tick(float dt) { (void)dt; snapshot(); press_tick(); }
static void finish_draw(gfx_fb *fb)
{
    ui_title(fb, "FINISH SESSION?", 0);
    dot_icon(fb, DOT_ICON_CHECK, UI_CX, 151, 6.0f, 4.8f, C_PRI);
    ui_text(fb, UI_F_TITLE, UI_CX, 237, "Finish and save", C_PRI, UI_CENTER, 0);
    micro(fb, UI_CX, 273, W.elapsed, C_SEC, UI_CENTER);
    micro(fb, UI_CX, 302, "YOUR SESSION IS PAUSED", C_DIS, UI_CENTER);
    buttons(fb, "GO BACK", "FINISH", BTN_PRIMARY);
}
static int finish_input(const nos_input *in)
{
    int hit = pair_hit(in->x, in->y);
    if (press_input(in, hit)) return 1;
    if (in->type != IN_TAP) return 0;
    if (hit == HIT_LEFT) nos_pop();
    else if (hit == HIT_RIGHT) {
        if (session_action(DIAG_ACT_STOP)) { nos_feedback(FB_SUCCESS); nos_pop(); }
        else { nos_feedback(FB_ERROR); nos_toast("SESSION COULD NOT FINISH"); }
    }
    return hit != HIT_NONE;
}

static void date_text(char *b, size_t n, uint32_t epoch)
{
    /* The saved UTC timestamp stays explicit; no dependence on today's timezone. */
    if (!epoch) { snprintf(b, n, "TIME NOT SET"); return; }
    /* Civil date from Unix day count (Gregorian era arithmetic). */
    unsigned day_seconds = epoch % 86400u;
    int z = (int)(epoch / 86400u) + 719468;
    int era = z / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int year = (int)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned day = doy - (153 * mp + 2) / 5 + 1;
    int month = (int)mp + (mp < 10 ? 3 : -9);
    year += month <= 2;
    snprintf(b, n, "%04d-%02d-%02u %02u:%02uZ", year, month, day, day_seconds / 3600, day_seconds / 60 % 60);
}
static void history_row(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed)
{
    (void)x0; (void)x1;
    if (i < 0 || i >= H.count) return;
    if (pressed) ui_rrect(fb, 68, y + 3, 330, h - 6, 24, ui_dim(C_SURF2, fade));
    ui_text_fit(fb, UI_F_BODY, 82, y + 31, profile_name(H.items[i].sport), ui_dim(C_PRI, fade), UI_LEFT, 200);
    micro(fb, 384, y + 30, H.status[i], ui_dim(H.items[i].save_state == WORKOUT_SAVE_FAILED ? C_RED : C_DIS, fade), UI_RIGHT);
    micro(fb, 82, y + 55, H.value[i], ui_dim(C_SEC, fade), UI_LEFT);
    micro(fb, 82, y + 76, H.date[i], ui_dim(C_DIS, fade), UI_LEFT);
    gfx_fill_rect(fb, 82, y + h - 1, 302, 1, ui_dim(C_LINE, fade));
}
static void history_tap(int i)
{
    if (i >= 0 && i < H.count) {
        int y = LIST_TOP + i * 94 + 47 - (int)floorf(ui_list_offset(&H.list) + 0.5f);
        detail_open(&H.items[i], UI_CX, y);
    }
}
static void history_refresh(void)
{
    int metric = nos_prefs.units_metric != 0, units_changed = metric != H.metric;
    int found = 0;
    int count = workout_history_count();
    if (count > WORKOUT_HISTORY_MAX) count = WORKOUT_HISTORY_MAX;
    for (int i = 0; i < count; i++) {
        workout_snapshot next;
        if (!workout_history_get(i, &next)) continue;
        if (units_changed || found >= H.count || memcmp(&next, &H.items[found], sizeof next))
            ui_fblur_invalidate(&H.list, found, found);
        H.items[found] = next;
        workout_snapshot *s = &H.items[found];
        char duration[20];
        elapsed_text(duration, sizeof duration, s->elapsed_ms);
        date_text(H.date[found], sizeof H.date[found], s->started_epoch);
        snprintf(H.status[found], sizeof H.status[found], s->save_state == WORKOUT_SAVE_SAVED ? "SAVED" :
                 s->save_state == WORKOUT_SAVE_FAILED ? "UNSAVED" : "SAVING");
        /* ~ marks a figure estimated for a default 70 kg because no weight was entered. */
        char kcal[16];
        snprintf(kcal, sizeof kcal, "%s%.0f KCAL", s->setup.mass_kg ? "" : "~", (double)active_kcal(s));
        if (outdoor(s->sport) && s->distance_valid)
            snprintf(H.value[found], sizeof H.value[found], "%s  /  %.2f %s  /  %s", duration,
                     (double)s->distance_m / distance_unit(metric), distance_label(metric), kcal);
        else snprintf(H.value[found], sizeof H.value[found], "%s  /  %s  /  %u LAP%s", duration, kcal, (unsigned)s->nlaps, s->nlaps == 1 ? "" : "S");
        found++;
    }
    H.count = found;
    H.metric = metric;
    ui_list_set_n(&H.list, H.count);
    H.refreshed_ms = hal_millis();
}
static void history_enter(void)
{
    if (!nos_revealed()) ui_list_init(&H.list, 0, 94, LIST_TOP, history_row, history_tap);
    history_refresh();
}
static void history_tick(float dt)
{
    if (!H.list.dragging && H.list.pressed < 0 &&
        (H.metric != (nos_prefs.units_metric != 0) || hal_millis() - H.refreshed_ms >= 1000)) history_refresh();
    workout_list_tick(&H.list, dt);
}
static void history_draw(gfx_fb *fb)
{
    if (H.count) {
        ui_list_draw(fb, &H.list);
        list_header(fb, "RECENT WORKOUTS", "LAST 12 SESSIONS / SAVE STATUS");
    } else {
        workout_storage_status st;workout_storage_get(&st);
        ui_title(fb, "RECENT WORKOUTS", 0);
        const char *title="No sessions yet",*hint="Finish a Sports session to see it here";
        if(st.foreign) {title="Original card required";hint="Return the session card, then tap to retry";}
        else if(st.worker_failed) {title="Storage worker unavailable";hint="Freeing resources / tap to retry";}
        else if(st.uncertain) {title="History read failed";hint="Banks preserved / tap to retry";}
        else if(st.corrupt) {title="History bank damaged";hint="Card preserved / tap to retry";}
        else if(!st.ready && !st.errors) {title="Loading workout history";hint="Reading session checkpoints";}
        else if(!st.ready || !st.available) {title="Workout history unavailable";hint="Check the card / tap to retry";}
        ui_absent(fb,DOT_ICON_CALENDAR,title,hint,0,1);
    }
}
static int history_input(const nos_input *in)
{
    if(!H.count && in->type==IN_TAP && in->y>=LIST_TOP) {
        session_retry();nos_feedback(FB_TAP);return 1;
    }
    if (in->type == IN_TAP && in->y < LIST_TOP) return 1;
    return H.count ? ui_list_input(&H.list, in) : 0;
}

static void add_detail(const char *label, const char *value)
{
    if (D.n >= DETAIL_ROWS) return;
    snprintf(D.rows[D.n].label, sizeof D.rows[D.n].label, "%s", label);
    snprintf(D.rows[D.n].value, sizeof D.rows[D.n].value, "%s", value);
    D.chart[D.n] = 0;
    D.n++;
}
/* Per-session series as a dot-column chart (movement % or heart rate), the NOS house style of the
 * Activity hourly chart. Bins are grouped so at most CHART_COLS columns show; an unknown bin
 * (no valid motion / no heart rate) stays empty rather than reading as zero. Returns 0 when the
 * session has nothing to draw. Column heights are 0-255; the label says what a column spans. */
static int add_chart(const workout_snapshot *s, int heart)
{
    int n = s->series_n, g = (n + CHART_COLS - 1) / CHART_COLS, cols = (n + g - 1) / g, any = 0;
    if (n < 2 || D.n >= DETAIL_ROWS) return 0;
    uint8_t *v = (uint8_t *)D.rows[D.n].value;
    memset(D.rows[D.n].value, 0, sizeof D.rows[D.n].value);
    for (int c = 0; c < cols; c++) {
        int sum = 0, known = 0;
        for (int i = c * g; i < (c + 1) * g && i < n; i++) {
            int x = heart ? s->series_bpm[i] : s->series_move[i];
            if (heart ? x > 0 : x != 255) { sum += x; known++; }
        }
        if (!known) continue;
        int a = sum / known;
        a = heart ? (a - 40) * 255 / 140 : a * 255 / 100;          /* 40-180 bpm / 0-100 % */
        v[c] = (uint8_t)(a < 12 ? 12 : a > 255 ? 255 : a);
        any = 1;
    }
    if (!any) return 0;
    unsigned span = (unsigned)g * s->series_bin_s / 60u;              /* minutes per column */
    char label[40];
    snprintf(label, sizeof label, "%s / %u MIN PER COLUMN", heart ? "HEART RATE" : "WRIST MOVEMENT", span ? span : 1);
    snprintf(D.rows[D.n].label, sizeof D.rows[D.n].label, "%s", label);
    D.rows[D.n].value[63] = (char)cols;
    D.chart[D.n] = 1;
    D.n++;
    return 1;
}
static void chart_row(gfx_fb *fb, int i, int y, float fade)
{
    const uint8_t *v = (const uint8_t *)D.rows[i].value;
    int cols = (uint8_t)D.rows[i].value[63];
    if (cols < 1 || cols > CHART_COLS) return;
    dot_columns(fb, 80.0f, (float)(y + 32), cols, 4, 9.4f, 6.6f, v, ui_dim(C_SEC, fade), ui_dim(C_GHOST, fade));
}
static void detail_row(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed)
{
    (void)x0; (void)x1; (void)pressed;
    if (i < 0 || i >= D.n) return;
    micro(fb, 76, y + 25, D.rows[i].label, ui_dim(C_DIS, fade), UI_LEFT);
    if (D.chart[i]) chart_row(fb, i, y, fade);
    else ui_text_fit(fb, UI_F_BODY, 76, y + 56, D.rows[i].value, ui_dim(C_PRI, fade), UI_LEFT, 316);
    gfx_fill_rect(fb, 76, y + h - 1, 314, 1, ui_dim(C_LINE, fade));
}
static void detail_open(const workout_snapshot *item, int x, int y)
{
    if (!detail_state) {
        detail_state = nos_mem_alloc(sizeof *detail_state, "workout.ui");
        if (!detail_state) { nos_feedback(FB_ERROR); nos_toast("DETAILS UNAVAILABLE"); return; }
        memset(detail_state, 0, sizeof *detail_state);
    }
    D.item = *item;
    leaf_open(&detail_screen, x, y);
}
static void detail_enter(void)
{
    const workout_snapshot *s = &D.item;
    char b[64], label[32];
    D.n = 0;
    D.metric = nos_prefs.units_metric != 0;
    snprintf(D.title, sizeof D.title, "%s", profile_name(s->sport));
    add_detail("SESSION STATUS", s->state == WORKOUT_RUNNING ? "Recording in background" :
               s->state == WORKOUT_PAUSED ? "Paused" :
               s->save_state == WORKOUT_SAVE_SAVED ? "Saved to SD" :
               s->save_state == WORKOUT_SAVE_FAILED ? "Unsaved / retry in Sports" : "Saving / retained in memory");
    elapsed_text(b, sizeof b, s->elapsed_ms); add_detail("ACTIVE TIME / EXCLUDES PAUSES", b);
    elapsed_text(b, sizeof b, s->total_ms); add_detail("TOTAL DURATION", b);
    if (outdoor(s->sport)) {
        if (s->distance_valid) snprintf(b, sizeof b, "%.2f %s", (double)s->distance_m / distance_unit(D.metric), D.metric ? "km" : "mi");
        else snprintf(b, sizeof b, "Unavailable");
        add_detail("ACCEPTED GPS DISTANCE", b);
        if (s->distance_valid && show_speed(s->sport) && s->avg_speed_mps > 0)
            snprintf(b, sizeof b, "%.1f %s", (double)s->avg_speed_mps * speed_unit(D.metric), D.metric ? "km/h" : "mph");
        else if (s->distance_valid && !show_speed(s->sport) && s->avg_pace_s_km > 0) {
            char pace[20]; pace_text(pace, sizeof pace, s->avg_pace_s_km * pace_unit(D.metric));
            snprintf(b, sizeof b, "%s %s", pace, D.metric ? "min/km" : "min/mi");
        } else snprintf(b, sizeof b, "Unavailable");
        add_detail(show_speed(s->sport) ? "AVG SPEED / VALID GPS TIME" : "AVG PACE / VALID GPS TIME", b);
        snprintf(b, sizeof b, "%.0f%% of active time", s->elapsed_ms ? (double)s->gps_active_ms * 100.0 / s->elapsed_ms : 0.0);
        add_detail("GPS COVERAGE", b);
    }
    if (s->hr_active_ms && s->avg_bpm) snprintf(b, sizeof b, "%u bpm / peak %u", (unsigned)s->avg_bpm, (unsigned)s->peak_bpm);
    else snprintf(b, sizeof b, "Unavailable");
    add_detail("AVERAGE HEART RATE", b);
    snprintf(b, sizeof b, "%.0f%% of active time", s->elapsed_ms ? (double)s->hr_active_ms * 100.0 / s->elapsed_ms : 0.0);
    add_detail("VALID HEART RATE COVERAGE", b);
    snprintf(b, sizeof b, "%s wrist / %s dominant", hand_name(s->setup.worn_wrist), hand_name(s->setup.dominant_hand));
    add_detail("PLACEMENT / FROZEN AT START", b);
    snprintf(b,sizeof b,"%s / revision %lu",s->setup.placement_source==PLACEMENT_CALIBRATED?"Guided, user confirmed":
             s->setup.placement_source==PLACEMENT_USER?"Manual":"Unknown",(unsigned long)s->placement_revision);
    add_detail("WRIST SELECTION SOURCE",b);
    if (uses_playing_hand(s->sport)) {
        add_detail(s->sport == WORKOUT_CRICKET ? "BATTING HAND" : "PLAYING HAND", hand_name(s->setup.playing_hand));
        if (s->sport == WORKOUT_CRICKET) add_detail("BOWLING HAND", hand_name(s->setup.bowling_hand));
        if (s->setup.worn_wrist && s->setup.playing_hand && s->setup.playing_hand != HAND_BOTH)
            add_detail("SENSOR LOCATION", s->setup.worn_wrist == s->setup.playing_hand ? "Playing wrist" : "Opposite wrist / no shot count");
    }
    if (s->motion_valid_ms) {
        snprintf(b, sizeof b, "%lu / movement episodes", (unsigned long)s->movement_bursts);
        add_detail("WRIST BURSTS / ESTIMATE", b);
        elapsed_text(b, sizeof b, s->movement_ms); add_detail("WRIST MOVEMENT TIME / ESTIMATE", b);
    } else add_detail("WRIST MOVEMENT", "Unavailable / no valid samples");
    snprintf(b, sizeof b, "%.0f%% of recording time", s->elapsed_ms ? (double)s->motion_valid_ms * 100.0 / s->elapsed_ms : 0.0);
    add_detail("VALID MOTION COVERAGE", b);
    unsigned hr_reject=0,gps_reject=0,motion_reject=0;
    for(unsigned i=1;i<WORKOUT_REJECT_COUNT;i++) {
        if(i<WORKOUT_REJECT_GPS_MISSING)hr_reject+=s->quality.rejected[i];
        else if(i<WORKOUT_REJECT_MOTION_INVALID)gps_reject+=s->quality.rejected[i];
        else motion_reject+=s->quality.rejected[i];
    }
    snprintf(b,sizeof b,"HR %u / GPS %u / motion %u",hr_reject,gps_reject,motion_reject);
    add_detail("QUALITY GATE REJECTIONS",b);
    /* Energy is always listed. ACTIVE is above resting and is what joins the daily Activity total;
     * TOTAL includes the resting share during the session. Never a measurement. */
    snprintf(b, sizeof b, "%.0f kcal / above resting", (double)active_kcal(s));
    add_detail("ACTIVE ENERGY / ESTIMATE", b);
    if (s->energy_basis) {
        snprintf(b, sizeof b, "%.0f kcal / incl. resting", (double)s->energy_kcal_est);
        add_detail("TOTAL ENERGY / ESTIMATE", b);
    }
    if (s->energy_basis & ENERGY_BASIS_HR)
        snprintf(b, sizeof b, "Heart rate%s", s->energy_basis & (ENERGY_BASIS_MOVE | ENERGY_BASIS_MET) ? " + MET when unavailable" : "");
    else if (s->energy_basis & ENERGY_BASIS_MOVE) snprintf(b, sizeof b, "%.1f MET scaled by wrist movement", s->met_tenths / 10.0);
    else snprintf(b, sizeof b, "%.1f MET category estimate", s->met_tenths / 10.0);
    add_detail("ENERGY METHOD / NOT MEASURED", b);
    if (!s->setup.mass_kg) snprintf(b, sizeof b, "SET WEIGHT / assumed %d kg", ENERGY_DEF_MASS_KG);
    else snprintf(b, sizeof b, "%u kg entered%s", s->setup.mass_kg, s->energy_basis & ENERGY_BASIS_ASSUMED_BODY ? " / default build" : "");
    add_detail("ENERGY BODY PROFILE", b);
    add_detail("ENERGY LIMIT", "Idle time may overcount");
    int charts = add_chart(s, 0);
    charts += add_chart(s, 1);
    if (!charts)
        add_detail("MOVEMENT / HEART RATE CHART", s->series_n ? "No valid samples" : "Not recorded for this session");
    snprintf(b, sizeof b, "%s / %lu recoveries", s->setup.auto_resume ? "Automatic resume" : "Resume paused", (unsigned long)s->recovery_count);
    add_detail("CRASH RECOVERY / DOWNTIME EXCLUDED", b);
    if (uses_steps(s->sport)) {
        if (s->steps || s->cadence_valid) snprintf(b, sizeof b, "%lu", (unsigned long)s->steps);
        else snprintf(b, sizeof b, "Unavailable");
        add_detail("SESSION STEPS", b);
    }
    date_text(b, sizeof b, s->started_epoch); add_detail("STARTED / UTC", b);
    for (int i = 0; i < s->nlaps && i < WORKOUT_LAPS_MAX; i++) {
        uint32_t ms = s->laps[i].elapsed_ms - (i ? s->laps[i - 1].elapsed_ms : 0);
        float distance = s->laps[i].distance_m - (i ? s->laps[i - 1].distance_m : 0);
        char duration[20]; elapsed_text(duration, sizeof duration, ms);
        snprintf(label, sizeof label, "LAP %02d", i + 1);
        if (outdoor(s->sport) && s->distance_valid && distance > 0)
            snprintf(b, sizeof b, "%s / %.2f %s", duration, (double)distance / distance_unit(D.metric), D.metric ? "km" : "mi");
        else snprintf(b, sizeof b, "%s", duration);
        add_detail(label, b);
    }
    if (!s->nlaps) add_detail("LAPS", "No manual laps marked");
    ui_list_init(&D.list, D.n, 78, LIST_TOP, detail_row, 0);
}
static void detail_tick(float dt)
{
    if (D.metric != (nos_prefs.units_metric != 0)) detail_enter();
    workout_list_tick(&D.list, dt);
}
static void detail_draw(gfx_fb *fb)
{
    ui_list_draw(fb, &D.list);
    list_header(fb, D.title, D.item.state == WORKOUT_RUNNING ?
                "AT OPEN / SESSION STILL RUNNING" : "SESSION DETAILS / SWIPE TO SCROLL");
}
static int detail_input(const nos_input *in)
{
    if (in->type == IN_TAP && in->y < LIST_TOP) return 1;
    return ui_list_input(&D.list, in);
}

static const nos_screen ready_screen = { "SPORT READY", ready_enter, ready_leave, ready_tick, ready_draw, ready_input, SCR_LIVE_RENDER };
static const nos_screen finish_screen = { "FINISH SESSION", finish_enter, 0, finish_tick, finish_draw, finish_input, SCR_LIVE_RENDER };
static const nos_screen history_screen = { "WORKOUT HISTORY", history_enter, 0, history_tick, history_draw, history_input, SCR_LIVE_RENDER };
static const nos_screen detail_screen = { "SESSION DETAILS", detail_enter, 0, detail_tick, detail_draw, detail_input, SCR_LIVE_RENDER };
static const nos_screen catalog_screen = { "ALL SPORTS", catalog_enter, 0, catalog_tick, catalog_draw, catalog_input, SCR_LIVE_RENDER };
static const nos_screen setup_screen = { "SESSION SETUP", setup_enter, 0, setup_tick, setup_draw, setup_input, SCR_LIVE_RENDER };
static const nos_screen mass_screen = { "BODY MASS", mass_enter, 0, mass_tick, mass_draw, mass_input, SCR_LIVE_RENDER };
static const nos_screen height_screen = { "BODY HEIGHT", mass_enter, 0, mass_tick, mass_draw, mass_input, SCR_LIVE_RENDER };
static const nos_screen age_screen = { "BODY AGE", mass_enter, 0, mass_tick, mass_draw, mass_input, SCR_LIVE_RENDER };
static const nos_screen wrist_screen = { "DETECT WRIST", wrist_enter, wrist_leave, wrist_tick, wrist_draw, wrist_input, SCR_LIVE_RENDER };
const nos_screen scr_workout = { "SPORTS", main_enter, 0, main_tick, main_draw, main_input, SCR_LIVE_RENDER };
