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
void time_ring(gfx_fb *fb, float cx, float cy, float r, int n, float d, float lit, uint16_t on, uint16_t off);

static float qz(float v, float n) { return floorf(v * n + 0.5f) / n; }
#define PRESS_HOLD 0.1f          /* a pressed control shows >= 0.1 s (3 frames) even on a quick tap */
static float ink_k(float k) { return qz(m_clamp((k - 0.3f) / 0.4f, 0, 1), 3); }   /* from the 2nd scale step */
/* Builds count whole frames (docs/MOTION30.html R3/R9): b runs 0..1 over BUILD_F frames
 * and bk() gives the frame back; list rows start one frame apart (at most 8 starts) and
 * build over 6 frames, their fill fading in over the first 3 in perceptual steps. */
#define BUILD_F 12
static int   bk(float b) { return (int)floorf(b * BUILD_F + 0.5f); }
static float row_p(float b, int r) { return m_clamp((float)m_stagger_k(bk(b), r, 1) / 6.0f, 0, 1); }

/* Standard offsets plus the city's daylight-saving rule. */
enum { DST_NONE = 0, DST_EU, DST_US, DST_AU, DST_NZ, DST_EG };
typedef struct { const char *name; int16_t off; uint8_t dst; } city;
static const city k_city[] = {
    { "Honolulu",    -600, DST_NONE }, { "Los Angeles", -480, DST_US }, { "New York", -300, DST_US },
    { "Sao Paulo",   -180, DST_NONE }, { "London",         0, DST_EU }, { "Paris",      60, DST_EU },
    { "Cairo",        120, DST_EG   }, { "Dubai",        240, DST_NONE }, { "New Delhi", 330, DST_NONE },
    { "Singapore",    480, DST_NONE }, { "Tokyo",        540, DST_NONE }, { "Sydney",    600, DST_AU },
    { "Auckland",     720, DST_NZ   },
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

/* ---- daylight saving, from the UTC instant ------------------------------------ */
static int64_t days_civil(int y, int m, int d)      /* days since 1970-01-01 (Hinnant) */
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static int year_of(int64_t z)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int m = mp + (mp < 10 ? 3 : -9);
    return (int)(yoe + era * 400) + (m <= 2);
}
static int wday(int64_t z) { return (int)(((z + 4) % 7 + 7) % 7); }   /* 0 = Sunday */
/* day of the n-th weekday `wd` of month m (n = -1: the last one) */
static int64_t nth_wd(int y, int m, int n, int wd)
{
    if (n > 0) {
        int64_t d = days_civil(y, m, 1);
        return d + (wd - wday(d) + 7) % 7 + 7 * (n - 1);
    }
    int64_t last = days_civil(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1) - 1;
    return last - (wday(last) - wd + 7) % 7;
}
/* local wall-clock time `sec` after midnight of `day`, in UTC seconds, at offset off_min */
static int64_t at_local(int64_t day, int sec, int off_min) { return day * 86400 + sec - (int64_t)off_min * 60; }
static int dst_active(const city *c, int64_t utc)
{
    int y = year_of(utc >= 0 ? utc / 86400 : (utc - 86399) / 86400), o = c->off;
    int64_t a, b;
    switch (c->dst) {
    case DST_EU:      /* last Sun Mar 01:00 UTC .. last Sun Oct 01:00 UTC */
        a = nth_wd(y, 3, -1, 0) * 86400 + 3600; b = nth_wd(y, 10, -1, 0) * 86400 + 3600;
        return utc >= a && utc < b;
    case DST_US:      /* 2nd Sun Mar 02:00 standard .. 1st Sun Nov 02:00 daylight */
        a = at_local(nth_wd(y, 3, 2, 0), 7200, o); b = at_local(nth_wd(y, 11, 1, 0), 7200, o + 60);
        return utc >= a && utc < b;
    case DST_EG:      /* last Fri Apr 00:00 .. last Thu Oct 24:00 */
        a = at_local(nth_wd(y, 4, -1, 5), 0, o); b = at_local(nth_wd(y, 10, -1, 4) + 1, 0, o + 60);
        return utc >= a && utc < b;
    case DST_AU:      /* southern: 1st Sun Oct 02:00 std .. 1st Sun Apr 03:00 daylight */
        a = at_local(nth_wd(y, 10, 1, 0), 7200, o); b = at_local(nth_wd(y, 4, 1, 0), 10800, o + 60);
        return utc >= a || utc < b;
    case DST_NZ:      /* last Sun Sep 02:00 std .. 1st Sun Apr 03:00 daylight */
        a = at_local(nth_wd(y, 9, -1, 0), 7200, o); b = at_local(nth_wd(y, 4, 1, 0), 10800, o + 60);
        return utc >= a || utc < b;
    default:
        return 0;
    }
}
/* UTC now: the trusted epoch, else the RTC's local time minus the configured zone */
static int64_t utc_now(const hal_datetime *dt)
{
    uint64_t ep = hal_epoch_seconds();
    if (ep) return (int64_t)ep;
    return at_local(days_civil(dt->year, dt->month, dt->day), dt->hour * 3600 + dt->minute * 60 + dt->second,
                    nos_prefs.tz_min);
}
static int city_off(int c, int64_t utc) { return k_city[c].off + (dst_active(&k_city[c], utc) ? 60 : 0); }

/* ---- time maths ------------------------------------------------------------ */
typedef struct { int h, m, s, dayrel, rel_min, wd; } ctime_t;
static int local_offset_min(const hal_datetime *dt)
{
    uint64_t ep = hal_epoch_seconds();
    if (!ep) return nos_prefs.tz_min;
    int64_t local = days_civil(dt->year, dt->month, dt->day) * 86400 +
                    dt->hour * 3600 + dt->minute * 60 + dt->second;
    int64_t seconds = local - (int64_t)ep;
    int64_t d = (seconds >= 0 ? seconds + 30 : seconds - 30) / 60;
    return d >= -720 && d <= 840 ? (int)d : nos_prefs.tz_min;
}
static void city_time(int c, ctime_t *o)
{
    hal_datetime dt;
    hal_get_datetime(&dt);
    int lo = local_offset_min(&dt);
    int co = city_off(c, utc_now(&dt));
    int mins = dt.hour * 60 + dt.minute - lo + co;
    int rel = 0;
    while (mins < 0) { mins += 1440; rel--; }
    while (mins >= 1440) { mins -= 1440; rel++; }
    o->h = mins / 60; o->m = mins % 60; o->s = dt.second;
    o->dayrel = rel;
    o->wd = ((dt.weekday + rel) % 7 + 7) % 7;
    o->rel_min = co - lo;
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
static struct {
    nos_time_t t0; float build; ctime_t t[NFAV];
    int empty_pressed; nos_time_t empty_t0, empty_unpress;
    int tap_i; nos_time_t tap_t;      /* the tapped row stays pressed PRESS_HOLD past the tap */
} WL;
extern const nos_screen scr_wc_pick, scr_wc_detail;
static int s_detail_city;

#define WL_ROW 90                /* the list-row pitch (§8) */
#define WL_TOP 108

static void wl_row(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed)
{
    float p = row_p(WL.build, i);
    if (p <= 0) return;
    float f = fade * m_ink(p * 2);
    float cy = y + h * 0.5f;
    pressed = pressed || (i == WL.tap_i && nos_t - WL.tap_t < PRESS_HOLD);
    if (i >= F.n) {                                       /* edit row: a pill, pressed C_SEC + black ink */
        float w = UI_BTN_W;
        ui_rrect(fb, UI_CX - w / 2, y + 4, w, h - 8, (h - 8) / 2.0f, ui_dim(pressed ? C_SEC : C_SURF2, f));
        ui_label(fb, UI_CX, (int)cy + 6, "EDIT CITIES", pressed ? C_BG : ui_dim(C_PRI, f), UI_CENTER);
        return;
    }
    /* fixed-width card (the notification card: 344 wide, r 40, SURF2); content on fixed
     * anchors, so rows near the bezel are clipped by the glass, never squeezed */
    const ctime_t *t = &WL.t[i];
    uint16_t card = pressed ? C_LINE2 : C_SURF2;          /* a pressed card: C_LINE2 */
    ui_rrect(fb, UI_CX - 172.0f, y + 4, 344.0f, h - 8.0f, 40, ui_dim(card, f));
    char nm[24], rel[32], tm[12];
    upper(nm, sizeof nm, k_city[F.idx[i]].name);
    fmt_rel(rel, sizeof rel, t);
    ui_fmt_time(tm, sizeof tm, t->h, t->m);
    int day = is_day(t);
    float ax = UI_CX - 150.0f;
    dot_disc(fb, ax, cy - 12, 9, ui_dim(day ? C_WHITE : C_DIS, f));
    if (!day) dot_disc(fb, ax, cy - 12, 5, ui_dim(card, f));  /* hollow = night */
    ui_label(fb, (int)ax + 16, (int)cy - 5, nm, ui_dim(C_PRI, f), UI_LEFT);
    ui_label(fb, (int)ax + 16, (int)cy + 23, rel, ui_dim(C_SEC, f), UI_LEFT);
    dot_style st = { DOT_FONT_TALL, UI_LIST_PITCH, UI_LIST_PITCH * 0.8f, ui_dim(day ? C_WHITE : C_PRI, f), 0, 1, 1 };
    float tw = (float)dot_text_width(&st, tm);
    dot_text_anim(fb, &st, UI_VALUE_X - tw, cy - 20, tm, 0, p, DOT_REVEAL_RANDOM, (unsigned)i);
}
static void wl_tap(int i)
{
    WL.tap_i = i; WL.tap_t = nos_t;
    if (i >= F.n) { nos_push(&scr_wc_pick, TX_PUSH); return; }
    s_detail_city = F.idx[i];
    nos_push(&scr_wc_detail, TX_PUSH);
}
static void wl_enter(void)
{
    fav_load();
    float keep = s_list.n ? ui_list_offset(&s_list) : 0;
    ui_list_init(&s_list, F.n + 1, WL_ROW, WL_TOP, wl_row, wl_tap);
    s_list.margin = 28;
    msc_jump(&s_list.sc, m_clamp(keep, s_list.sc.min, s_list.sc.max));
    s_list.bar_t = -10;
    WL.empty_pressed = 0; WL.empty_unpress = 0; WL.tap_i = -1;
    if (!nos_revealed()) { WL.t0 = nos_enter_t(); WL.build = 0; }   /* pop reveal: no rebuild */
    for (int i = 0; i < F.n; i++) city_time(F.idx[i], &WL.t[i]);
}
static void wl_tick(float dt)
{
    if (s_list.n != F.n + 1) ui_list_set_n(&s_list, F.n + 1);
    for (int i = 0; i < F.n; i++) city_time(F.idx[i], &WL.t[i]);
    ui_list_tick(&s_list, dt);
    WL.build = m_clamp((nos_t - WL.t0) / M_FRAMES(BUILD_F), 0, 1);
    if (WL.empty_unpress > 0 && nos_t >= WL.empty_unpress) { WL.empty_pressed = 0; WL.empty_unpress = 0; }
    nos_motion_probe("wc.row0.ink", MC_FADE, m_ink(row_p(WL.build, 0) * 2) * 100.0f);
    nos_motion_probe("wc.row4.ink", MC_FADE, m_ink(row_p(WL.build, 4) * 2) * 100.0f);
}
#define WL_EMPTY_BTN ((float)UI_BTN_Y)
static void wl_draw(gfx_fb *fb)
{
    if (F.n == 0) {
        ui_title(fb, "WORLD CLOCK", 0);
        ui_absent(fb, DOT_ICON_GLOBE, "No cities", NULL, NULL, WL.build);
        float k = mf_at(&MF_MICRO6, bk(WL.build) - 3);   /* frames 4-9 */
        if (k <= 0) return;
        /* primary button builds by scale steps at full colour (no grey fade-up) */
        float sc = 0.72f + 0.28f * k, w = UI_BTN_W * sc, h = UI_BTN_H * sc, ik = ink_k(k);
        uint16_t fill = WL.empty_pressed ? C_SEC : C_WHITE;
        ui_rrect(fb, UI_CX - w / 2, WL_EMPTY_BTN - h / 2, w, h, h / 2, fill);
        if (ik > 0) ui_label(fb, UI_CX, (int)WL_EMPTY_BTN + 6, "ADD CITIES", ui_mix(fill, C_BG, ik), UI_CENTER);
        return;
    }
    ui_title(fb, "WORLD CLOCK", ui_list_offset(&s_list));
    ui_list_draw(fb, &s_list);
}
static int wl_input(const nos_input *in)
{
    if (in->type == IN_CANCEL) { WL.empty_pressed = 0; WL.empty_unpress = 0; WL.tap_i = -1; ui_list_input(&s_list, in); return 1; }

    if (F.n == 0) {
        int hit = fabsf((float)(in->x - UI_CX)) < UI_BTN_W / 2 + 4 && fabsf(in->y - WL_EMPTY_BTN) < 48;
        if (in->type == IN_DOWN) { WL.empty_pressed = hit; WL.empty_t0 = nos_t; WL.empty_unpress = 0; return hit; }
        if (in->type == IN_DRAG_START) { WL.empty_pressed = 0; WL.empty_unpress = 0; return 0; }
        if (in->type == IN_UP) { if (WL.empty_pressed) WL.empty_unpress = fmax(nos_t, WL.empty_t0 + PRESS_HOLD); return 0; }
        if (in->type == IN_TAP && hit) { nos_feedback(FB_TAP); nos_push(&scr_wc_pick, TX_PUSH); return 1; }
        return 0;
    }
    return ui_list_input(&s_list, in);
}
const nos_screen scr_worldclock = { "WORLD CLOCK", wl_enter, 0, wl_tick, wl_draw, wl_input, 0 };

/* ============================================================== chooser */
static ui_list s_pick;
static nos_time_t s_pick_t0; static float s_pick_build;
static int   s_pk_tap_i = -1;
static nos_time_t s_pk_tap_t;
#define PK_ROW 90                /* the list-row pitch (§8) */
#define PK_TOP 100

static void pk_row(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed)
{
    float p = row_p(s_pick_build, i);
    if (p <= 0) return;
    float f = fade * m_ink(p * 2);
    float cy = y + h * 0.5f;
    int sel = fav_find(i) >= 0;
    int cx0, cx1;                                         /* see wl_row: fixed anchors */
    ui_row_span(cy, 26, &cx0, &cx1);
    if (cx0 > UI_CX - 170) cx0 = UI_CX - 170;
    if (cx1 < UI_CX + 170) cx1 = UI_CX + 170;
    pressed = pressed || (i == s_pk_tap_i && nos_t - s_pk_tap_t < PRESS_HOLD);
    if (pressed) ui_rrect(fb, (float)cx0, y + 4, (float)(cx1 - cx0), h - 8.0f, 40, ui_dim(C_LINE2, f));   /* the pressed-card look */
    char nm[24], off[16];
    upper(nm, sizeof nm, k_city[i].name);
    hal_datetime dt;
    hal_get_datetime(&dt);
    int o = city_off(i, utc_now(&dt)), a = o < 0 ? -o : o;   /* UTC offset in force now */
    if (a % 60) snprintf(off, sizeof off, "%c%d:%02d", o < 0 ? '-' : '+', a / 60, a % 60);
    else snprintf(off, sizeof off, "%c%d", o < 0 ? '-' : '+', a / 60);
    ui_label(fb, UI_CX - 146, (int)cy + 6, nm, ui_dim(sel ? C_WHITE : C_SEC, f), UI_LEFT);
    float cx = UI_CX + 130.0f;
    ui_label(fb, (int)cx - 32, (int)cy + 6, off, ui_dim(C_DIS, f), UI_RIGHT);
    if (sel) {
        ui_circle(fb, cx, cy, 17, ui_dim(C_WHITE, f));
        dot_icon(fb, DOT_ICON_CHECK, cx, cy, 2.0f, 1.6f, C_BG);
    } else ui_circle_line(fb, cx, cy, 17, 2, ui_dim(pressed ? C_SEC : C_LINE2, f));   /* visible on the pressed fill */
}
static void pk_tap(int i)
{
    s_pk_tap_i = i; s_pk_tap_t = nos_t;
    int k = fav_find(i);
    if (k >= 0) {
        for (int j = k; j < F.n - 1; j++) F.idx[j] = F.idx[j + 1];
        F.n--;
        nos_feedback(FB_TOGGLE_OFF);
    } else if (F.n >= NFAV) {
        nos_feedback(FB_ERROR);
        nos_toast("List full");
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
    s_pick_t0 = nos_enter_t(); s_pick_build = 0; s_pk_tap_i = -1;
}
static void pk_tick(float dt)
{
    ui_list_tick(&s_pick, dt);
    s_pick_build = m_clamp((nos_t - s_pick_t0) / M_FRAMES(BUILD_F), 0, 1);
    nos_motion_probe("wcp.row0.ink", MC_FADE, m_ink(row_p(s_pick_build, 0) * 2) * 100.0f);
}
static void pk_draw(gfx_fb *fb)
{
    char t[24];
    snprintf(t, sizeof t, "CITIES  %d/%d", F.n, NFAV);
    ui_title(fb, t, ui_list_offset(&s_pick));
    ui_list_draw(fb, &s_pick);
}
static int pk_input(const nos_input *in)
{
    if (in->type == IN_CANCEL) s_pk_tap_i = -1;
    return ui_list_input(&s_pick, in);
}
const nos_screen scr_wc_pick = { "CITIES", pk_enter, 0, pk_tick, pk_draw, pk_input, 0 };

/* ============================================================== analog view */
#define AR 196.0f
/* Detail build, whole frames: the 60-dot dial and its 12 hour marks fill as an even
 * sweep (EASE_STEADY, 9 frames), the city and offset fade in (frames 3-8), the hands
 * grow out dot by dot at a constant rate (frames 3-10), the seconds dot last (10-13). */
static struct { nos_time_t t0; float build; ctime_t t; char name[24], rel[32], tm[12]; float lit, lab_k, hand_p, sec_k; } D;

static void dt_enter(void)
{
    D.t0 = nos_enter_t(); D.build = 0;
    city_time(s_detail_city, &D.t);
    upper(D.name, sizeof D.name, k_city[s_detail_city].name);
    fmt_rel(D.rel, sizeof D.rel, &D.t);
}
static void dt_tick(float dt)
{
    city_time(s_detail_city, &D.t);
    fmt_rel(D.rel, sizeof D.rel, &D.t);
    ui_fmt_time(D.tm, sizeof D.tm, D.t.h, D.t.m);
    D.build = m_clamp((nos_t - D.t0) / M_FRAMES(14), 0, 1);
    {
        int k = m_frame_k(D.t0);
        D.lit = 60.0f * (k <= 0 ? 0.0f : k >= 9 ? 1.0f : bezier_ease(&EASE_STEADY, (float)k / 9.0f));
        D.lab_k = m_ink((float)(k - 2) / 6.0f);
        D.hand_p = m_clamp((float)(k - 2) / 8.0f, 0, 1);
        D.sec_k = qz(m_clamp((float)(k - 9) / 4.0f, 0, 1), 4);
    }
    /* motion audit (sim only) */
    nos_motion_probe("wcd.ring.arc", MC_TRANSITION, floorf(D.lit * 32.0f + 0.5f) / 32.0f * 6.2831853f * AR / 60.0f);
    nos_motion_probe("wcd.label.ink", MC_FADE, D.lab_k * 100.0f);
    nos_motion_probe("wcd.minute.len", MC_COMPONENT, m_clamp(D.hand_p * 11.0f, 0, 9.0f) * 14.0f);
    nos_motion_probe("wcd.sec.d", MC_MICRO, 10 * D.sec_k);
}
static void hand(gfx_fb *fb, float a, float r0, float r1, float step, float d, uint16_t c, float p)
{
    float s = gfx_fast_sin(a), co = gfx_fast_cos(a);
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
    float lit = D.lit;
    time_ring(fb, UI_CX, UI_CY, AR, 60, 4, lit, C_LINE2, 0);
    time_ring(fb, UI_CX, UI_CY, AR, 12, 8, lit / 5.0f, C_SEC, 0);
    int day = is_day(&D.t);
    float k = D.lab_k;
    if (k > 0) {
        /* title: the city with the list's day/night dot (filled = day, hollow = night) */
        int tw = ui_text_width(UI_F_LABEL, D.name, 2);
        float dx = UI_CX - tw / 2.0f - 8;
        dot_disc(fb, dx, 64, 9, ui_dim(day ? C_WHITE : C_DIS, k));
        if (!day) dot_disc(fb, dx, 64, 5, C_BG);
        ui_label(fb, UI_CX + 8, UI_TITLE_Y, D.name, ui_dim(C_SEC, k), UI_CENTER);
        /* bottom: only the offset from here (the hands tell the time) */
        ui_label(fb, UI_CX, 392, D.rel, ui_dim(C_SEC, k), UI_CENTER);
    }
    float ph = D.hand_p;
    float ha = 6.2831853f * ((D.t.h % 12) + D.t.m / 60.0f) / 12.0f;
    float ma = 6.2831853f * D.t.m / 60.0f;
    hand(fb, ma, 26, 140, 14, 8, C_PRI, ph);
    hand(fb, ha, 26, 98, 18, 11, C_WHITE, ph);
    if (ph > 0) dot_disc(fb, UI_CX, UI_CY, 14, C_WHITE);
    float sk = D.sec_k;                                     /* the seconds dot grows in last */
    if (sk > 0) {
        float sa = 6.2831853f * D.t.s / 60.0f;
        dot_disc(fb, UI_CX + AR * gfx_fast_sin(sa), UI_CY - AR * gfx_fast_cos(sa), 10 * sk, C_RED);
    }
}
const nos_screen scr_wc_detail = { "CITY", dt_enter, 0, dt_tick, dt_draw, 0, 0 };
