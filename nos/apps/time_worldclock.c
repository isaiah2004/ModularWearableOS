/* time_worldclock.c — World clock: up to 4 favourite cities (local time, offset from
 * here, day/night dot), a city chooser over a fixed table of 12, and a big analog view
 * built from dots. Favourites persist in hal_store key "nos_wc". Team: time.
 *
 * Offsets are fixed standard-time offsets (no DST tables on the watch). */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* from time_timer.c (exact-position trig and dot ring) */
float time_sin(float x);
float time_cos(float x);
void time_ring(gfx_fb *fb, float cx, float cy, float r, int n, float d, float lit, uint16_t on, uint16_t off);

static float qz(float v, float n) { return floorf(v * n + 0.5f) / n; }

typedef struct { const char *name; int16_t off; } city;
static const city k_city[] = {
    { "Honolulu",    -600 }, { "Los Angeles", -480 }, { "New York",  -300 },
    { "Sao Paulo",   -180 }, { "London",         0 }, { "Paris",       60 },
    { "Cairo",        120 }, { "Dubai",        240 }, { "New Delhi",  330 },
    { "Singapore",    480 }, { "Tokyo",        540 }, { "Sydney",     600 },
};
#define NCITY ((int)(sizeof k_city / sizeof k_city[0]))
#define NFAV 4

static struct { uint8_t magic, n, idx[NFAV]; } F;
static int s_loaded;
#define WC_MAGIC 0x57

static void fav_load(void)
{
    if (s_loaded) return;
    s_loaded = 1;
    if (hal_store_load("nos_wc", &F, sizeof F) && F.magic == WC_MAGIC && F.n <= NFAV) {
        int ok = 1;
        for (int i = 0; i < F.n; i++) if (F.idx[i] >= NCITY) ok = 0;
        if (ok) return;
    }
    F.magic = WC_MAGIC; F.n = 4;
    F.idx[0] = 4; F.idx[1] = 2; F.idx[2] = 10; F.idx[3] = 11;   /* London, New York, Tokyo, Sydney */
}
static void fav_save(void) { hal_store_save("nos_wc", &F, sizeof F); }
static int fav_find(int c) { for (int i = 0; i < F.n; i++) if (F.idx[i] == c) return i; return -1; }

/* ---- time maths ------------------------------------------------------------ */
typedef struct { int h, m, s, dayrel, rel_min, wd; } ctime_t;
static int local_offset_min(const hal_datetime *dt)
{
    uint64_t ep = hal_epoch_seconds();
    if (!ep) return nos_prefs.tz_min;
    int local = dt->hour * 3600 + dt->minute * 60 + dt->second;
    int utc = (int)(ep % 86400u);
    int d = (local - utc) / 60;
    d = (int)floorf(d / 15.0f + 0.5f) * 15;
    while (d > 840) d -= 1440;
    while (d < -720) d += 1440;
    return d;
}
static void city_time(int c, ctime_t *o)
{
    hal_datetime dt;
    hal_get_datetime(&dt);
    int lo = local_offset_min(&dt);
    int mins = dt.hour * 60 + dt.minute - lo + k_city[c].off;
    int rel = 0;
    while (mins < 0) { mins += 1440; rel--; }
    while (mins >= 1440) { mins -= 1440; rel++; }
    o->h = mins / 60; o->m = mins % 60; o->s = dt.second;
    o->dayrel = rel;
    o->wd = ((dt.weekday + rel) % 7 + 7) % 7;
    o->rel_min = k_city[c].off - lo;
}
static void fmt_rel(char *b, int n, const ctime_t *t)
{
    int r = t->rel_min, a = r < 0 ? -r : r;
    if (!r) { snprintf(b, (size_t)n, "SAME TIME"); return; }
    /* the city's weekday only when it is not today here */
    snprintf(b, (size_t)n, "%c%d:%02d%s%s", r < 0 ? '-' : '+', a / 60, a % 60,
             t->dayrel ? "  " : "", t->dayrel ? ui_weekday3(t->wd) : "");
}
static int is_day(const ctime_t *t) { return t->h >= 6 && t->h < 18; }
static void upper(char *d, int n, const char *s)
{
    int i = 0;
    for (; s[i] && i < n - 1; i++) d[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
    d[i] = 0;
}

/* ================================================================ list */
static ui_list s_list;
static struct { float t0, build; ctime_t t[NFAV]; int empty_pressed; } WL;
extern const nos_screen scr_wc_pick, scr_wc_detail;
static int s_detail_city;

#define WL_ROW 96
#define WL_TOP 108

static void wl_row(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed)
{
    float p = m_stagger(WL.build, i < 6 ? i : 6, 7, 0.5f);
    if (p <= 0) return;
    float f = fade * qz(m_clamp(p * 2, 0, 1), 8);
    float cy = y + h * 0.5f;
    if (i >= F.n) {                                       /* edit row */
        float w = 200;
        ui_rrect(fb, UI_CX - w / 2, y + 4, w, h - 8, (h - 8) / 2.0f, ui_dim(pressed ? C_SURF3 : C_SURF2, f));
        ui_label(fb, UI_CX, (int)cy + 6, "EDIT CITIES", ui_dim(C_PRI, f), UI_CENTER);
        return;
    }
    /* the card follows the circle at its centre line; content sits on fixed anchors so
     * rows near the bezel are clipped by the glass, never squeezed into each other */
    int cx0, cx1;
    ui_row_span(cy, 28, &cx0, &cx1);
    if (cx0 > UI_CX - 158) cx0 = UI_CX - 158;
    if (cx1 < UI_CX + 158) cx1 = UI_CX + 158;
    const ctime_t *t = &WL.t[i];
    ui_rrect(fb, (float)cx0, y + 4, (float)(cx1 - cx0), h - 8.0f, 36, ui_dim(pressed ? C_SURF2 : C_SURF, f));
    char nm[24], rel[32], tm[12];
    upper(nm, sizeof nm, k_city[F.idx[i]].name);
    fmt_rel(rel, sizeof rel, t);
    ui_fmt_time(tm, sizeof tm, t->h, t->m);
    int day = is_day(t);
    float ax = UI_CX - 150.0f;
    dot_disc(fb, ax, cy - 12, 9, ui_dim(day ? C_WHITE : C_DIS, f));
    if (!day) dot_disc(fb, ax, cy - 12, 5, pressed ? C_SURF2 : C_SURF);  /* hollow = night */
    ui_label(fb, (int)ax + 16, (int)cy - 5, nm, ui_dim(C_PRI, f), UI_LEFT);
    ui_label(fb, (int)ax + 16, (int)cy + 23, rel, ui_dim(C_SEC, f), UI_LEFT);
    dot_style st = { DOT_FONT_TALL, 4.0f, 3.2f, ui_dim(day ? C_WHITE : C_PRI, f), 0, 1, 1 };
    float tw = (float)dot_text_width(&st, tm);
    dot_text_anim(fb, &st, UI_CX + 152 - tw, cy - 18, tm, 0, p, DOT_REVEAL_RANDOM, (unsigned)i);
}
static void wl_tap(int i)
{
    if (i >= F.n) { nos_push(&scr_wc_pick, TX_PUSH); return; }
    s_detail_city = F.idx[i];
    nos_push(&scr_wc_detail, TX_PUSH);
}
static float s_wl_seen = -10;
static void wl_enter(void)
{
    fav_load();
    float keep = s_list.n ? ui_list_offset(&s_list) : 0;
    ui_list_init(&s_list, F.n + 1, WL_ROW, WL_TOP, wl_row, wl_tap);
    s_list.margin = 28;
    msc_jump(&s_list.sc, m_clamp(keep, s_list.sc.min, s_list.sc.max));
    s_list.bar_t = -10;
    WL.empty_pressed = 0;
    if (nos_t - s_wl_seen >= 0.2f) { WL.t0 = nos_t; WL.build = 0; }   /* pop reveal: no rebuild */
    for (int i = 0; i < F.n; i++) city_time(F.idx[i], &WL.t[i]);
}
static void wl_tick(float dt)
{
    s_wl_seen = nos_t;
    if (s_list.n != F.n + 1) ui_list_set_n(&s_list, F.n + 1);
    for (int i = 0; i < F.n; i++) city_time(F.idx[i], &WL.t[i]);
    ui_list_tick(&s_list, dt);
    WL.build = m_clamp((nos_t - WL.t0) / 0.4f, 0, 1);
}
#define WL_EMPTY_BTN 334.0f
static void wl_draw(gfx_fb *fb)
{
    if (F.n == 0) {
        ui_title(fb, "WORLD CLOCK", 0);
        dot_icon_anim(fb, DOT_ICON_GLOBE, UI_CX, 176, 5.0f, 4.0f, C_SEC, WL.build, DOT_REVEAL_CENTER);
        float k = qz(m_clamp(WL.build * 2 - 0.5f, 0, 1), 8);
        if (k <= 0) return;
        ui_text(fb, UI_F_BODY, UI_CX, 266, "No cities", ui_dim(C_PRI, k), UI_CENTER, 0);
        ui_rrect(fb, UI_CX - 100, WL_EMPTY_BTN - 44, 200, 88, 44, ui_dim(WL.empty_pressed ? C_SEC : C_WHITE, k));
        ui_label(fb, UI_CX, (int)WL_EMPTY_BTN + 6, "ADD CITIES", C_BG, UI_CENTER);
        return;
    }
    ui_title(fb, "WORLD CLOCK", ui_list_offset(&s_list));
    ui_list_draw(fb, &s_list);
}
static int wl_input(const nos_input *in)
{
    if (F.n == 0) {
        int hit = fabsf((float)(in->x - UI_CX)) < 104 && fabsf(in->y - WL_EMPTY_BTN) < 48;
        if (in->type == IN_DOWN) { WL.empty_pressed = hit; return hit; }
        if (in->type == IN_UP || in->type == IN_DRAG_START) { WL.empty_pressed = 0; return 0; }
        if (in->type == IN_TAP && hit) { WL.empty_pressed = 0; nos_feedback(FB_TAP); nos_push(&scr_wc_pick, TX_PUSH); return 1; }
        return 0;
    }
    return ui_list_input(&s_list, in);
}
const nos_screen scr_worldclock = { "WORLD CLOCK", wl_enter, 0, wl_tick, wl_draw, wl_input, 0 };

/* ============================================================== chooser */
static ui_list s_pick;
static float s_pick_t0, s_pick_build;
#define PK_ROW 80
#define PK_TOP 100

static void pk_row(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed)
{
    float p = m_stagger(s_pick_build, i < 6 ? i : 6, 7, 0.5f);
    if (p <= 0) return;
    float f = fade * qz(m_clamp(p * 2, 0, 1), 8);
    float cy = y + h * 0.5f;
    int sel = fav_find(i) >= 0;
    int cx0, cx1;                                         /* see wl_row: fixed anchors */
    ui_row_span(cy, 26, &cx0, &cx1);
    if (cx0 > UI_CX - 170) cx0 = UI_CX - 170;
    if (cx1 < UI_CX + 170) cx1 = UI_CX + 170;
    if (pressed) ui_rrect(fb, (float)cx0, y + 4, (float)(cx1 - cx0), h - 8.0f, 32, ui_dim(C_SURF2, f));
    char nm[24], off[16];
    upper(nm, sizeof nm, k_city[i].name);
    int o = k_city[i].off, a = o < 0 ? -o : o;
    if (a % 60) snprintf(off, sizeof off, "UTC%c%d:%02d", o < 0 ? '-' : '+', a / 60, a % 60);
    else if (o) snprintf(off, sizeof off, "UTC%c%d", o < 0 ? '-' : '+', a / 60);
    else snprintf(off, sizeof off, "UTC");
    ui_label(fb, UI_CX - 146, (int)cy + 6, nm, ui_dim(sel ? C_WHITE : C_SEC, f), UI_LEFT);
    float cx = UI_CX + 130.0f;
    ui_label(fb, (int)cx - 32, (int)cy + 6, off, ui_dim(C_DIS, f), UI_RIGHT);
    if (sel) {
        ui_circle(fb, cx, cy, 17, ui_dim(C_WHITE, f));
        dot_icon(fb, DOT_ICON_CHECK, cx, cy, 2.0f, 1.6f, C_BG);
    } else ui_circle_line(fb, cx, cy, 17, 2, ui_dim(C_LINE2, f));
}
static void pk_tap(int i)
{
    int k = fav_find(i);
    if (k >= 0) {
        for (int j = k; j < F.n - 1; j++) F.idx[j] = F.idx[j + 1];
        F.n--;
        nos_feedback(FB_TOGGLE_OFF);
    } else if (F.n >= NFAV) {
        nos_feedback(FB_ERROR);
        nos_toast("Up to 4 cities");
        return;
    } else {
        F.idx[F.n++] = (uint8_t)i;
        nos_feedback(FB_TOGGLE_ON);
    }
    fav_save();
}
static void pk_enter(void)
{
    fav_load();
    ui_list_init(&s_pick, NCITY, PK_ROW, PK_TOP, pk_row, pk_tap);
    s_pick.margin = 26;
    s_pick.bar_t = -10;
    s_pick_t0 = nos_t; s_pick_build = 0;
}
static void pk_tick(float dt)
{
    ui_list_tick(&s_pick, dt);
    s_pick_build = m_clamp((nos_t - s_pick_t0) / 0.4f, 0, 1);
}
static void pk_draw(gfx_fb *fb)
{
    char t[24];
    snprintf(t, sizeof t, "CITIES  %d/%d", F.n, NFAV);
    ui_title(fb, t, ui_list_offset(&s_pick));
    ui_list_draw(fb, &s_pick);
}
static int pk_input(const nos_input *in) { return ui_list_input(&s_pick, in); }
const nos_screen scr_wc_pick = { "CITIES", pk_enter, 0, pk_tick, pk_draw, pk_input, 0 };

/* ============================================================== analog view */
#define AR 196.0f
static struct { float t0, build; ctime_t t; char name[24], rel[32], tm[12]; } D;

static void dt_enter(void)
{
    D.t0 = nos_t; D.build = 0;
    city_time(s_detail_city, &D.t);
    upper(D.name, sizeof D.name, k_city[s_detail_city].name);
    fmt_rel(D.rel, sizeof D.rel, &D.t);
}
static void dt_tick(float dt)
{
    city_time(s_detail_city, &D.t);
    fmt_rel(D.rel, sizeof D.rel, &D.t);
    ui_fmt_time(D.tm, sizeof D.tm, D.t.h, D.t.m);
    D.build = m_clamp((nos_t - D.t0) / 0.45f, 0, 1);
}
static void hand(gfx_fb *fb, float a, float r0, float r1, float step, float d, uint16_t c, float p)
{
    float s = time_sin(a), co = time_cos(a);
    int n = (int)((r1 - r0) / step + 0.5f) + 1;
    for (int i = 0; i < n; i++) {
        float g = m_clamp(p * (n + 2) - i, 0, 1);       /* dots appear outward */
        if (g <= 0) break;
        float r = r0 + i * step;
        dot_disc(fb, UI_CX + s * r, UI_CY - co * r, qz(d * g, 4), c);
    }
}
static void dt_draw(gfx_fb *fb)
{
    float b = D.build;
    float lit = 60.0f * m_clamp(b * 1.4f, 0, 1);
    time_ring(fb, UI_CX, UI_CY, AR, 60, 4, lit, C_LINE2, 0);
    time_ring(fb, UI_CX, UI_CY, AR, 12, 8, lit / 5.0f, C_SEC, 0);
    int day = is_day(&D.t);
    float k = qz(m_clamp(b * 2 - 0.3f, 0, 1), 8);
    if (k > 0) {
        int tw = ui_text_width(UI_F_LABEL, D.name, 2);
        dot_icon(fb, day ? DOT_ICON_SUN : DOT_ICON_MOON, UI_CX - tw / 2.0f - 20, 64, 2.0f, 1.6f, ui_dim(C_SEC, k));
        ui_label(fb, UI_CX + 11, 70, D.name, ui_dim(C_PRI, k), UI_CENTER);
        /* bottom: digital time (primary) + offset from here (secondary) as one line */
        int w1 = ui_text_width(UI_F_LABEL, D.tm, 2), w2 = ui_text_width(UI_F_LABEL, D.rel, 2);
        int x = UI_CX - (w1 + 14 + w2) / 2;
        ui_label(fb, x, 398, D.tm, ui_dim(C_PRI, k), UI_LEFT);
        ui_label(fb, x + w1 + 14, 398, D.rel, ui_dim(C_SEC, k), UI_LEFT);
    }
    float ph = m_clamp(b * 1.6f - 0.3f, 0, 1);
    float ha = 6.2831853f * ((D.t.h % 12) + D.t.m / 60.0f) / 12.0f;
    float ma = 6.2831853f * D.t.m / 60.0f;
    hand(fb, ma, 26, 140, 14, 8, C_PRI, ph);
    hand(fb, ha, 26, 98, 18, 11, C_WHITE, ph);
    if (ph > 0) dot_disc(fb, UI_CX, UI_CY, 14, C_WHITE);
    if (b >= 1) {
        float sa = 6.2831853f * D.t.s / 60.0f;
        dot_disc(fb, UI_CX + AR * time_sin(sa), UI_CY - AR * time_cos(sa), 10, C_RED);
    }
}
const nos_screen scr_wc_detail = { "CITY", dt_enter, 0, dt_tick, dt_draw, 0, 0 };
