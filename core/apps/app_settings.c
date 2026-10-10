/* app_settings.c — real settings. Brightness supports taps and live drags,
 * plus toggle rows. Brightness drives the
 * AMOLED brightness; the toggles hold real OS state other apps can read.
 *
 * A second page (s_page == PAGE_TIME) is a TIME / DATE / TIMEZONE setter: a
 * 2x3 grid of tappable fields (hour, minute, day, month, year, UTC offset) with
 * a big -/+ stepper pair and an APPLY button. On APPLY it composes a
 * hal_datetime (weekday derived, seconds zeroed), calls hal_set_datetime(), and
 * persists the timezone + a "last set" epoch to NVS via settings_save().
 *
 * On the 1.43-B, the HAL writes UTC to the PCF85063 RTC. Time survives reboot
 * while the RTC remains powered; the display timezone is retained in NVS. */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include "../settings.h"
#include "../wake_service.h"
#include <stdio.h>

/* shared settings state */
int  g_setting_24h = 1;
int  g_setting_tilt_wake = 1;
extern int wos_debug_hud;    /* the on-screen FPS/touch HUD (defined in wos.c) */
extern int wos_debug_rects;  /* outline every repainted rectangle (wos.c) */
extern int wos_fps_cap48;    /* experimental 48fps frame cap (wos.c) */
extern int wos_fast_display; /* 8-bit panel wire format: 0 off, 1 256 colours, 2 grey (wos.c) */
#define s_bright g_brightness   /* unified brightness (persisted) */

/* ---- main page geometry (shifted up from the original to make room for the
 * "Date & Time" entry button, which must sit in the centre column to clear the
 * round bezel — a left-aligned 4th row would fall off the bottom-left curve) */
#define SLIDER_X0 GFX_S(58)
#define SLIDER_X1 GFX_S(182)
#define SLIDER_Y  GFX_S(84)
#define ROW1_Y    GFX_S(120)
#define ROW2_Y    GFX_S(150)
#define ROW3_Y    GFX_S(180)
#define ROW_H     GFX_S(34)
#define TIMEBTN_Y GFX_S(214)
#define TIMEBTN_W GFX_S(140)
#define TIMEBTN_H GFX_S(30)

/* ---- setter page ---- */
enum { PAGE_MAIN = 0, PAGE_TIME, PAGE_WAKE, PAGE_DEV };
enum { F_HOUR = 0, F_MIN, F_DAY, F_MON, F_YEAR, F_TZ, F_COUNT };

static int          s_page = PAGE_MAIN;
static int          s_brightness_drag, s_brightness_changed, s_brightness_tap_done;
static int          s_field = F_HOUR;      /* selected grid field */
static hal_datetime s_edit;                /* working copy while editing */
static int          s_edit_tz;             /* working tz offset (minutes) */

/* field-grid layout: 2 columns x 3 rows, centred */
#define GX0   (GFX_CX - GFX_S(52))
#define GX1   (GFX_CX + GFX_S(52))
#define GRY0  GFX_S(72)
#define GROW  GFX_S(32)
#define GCW   GFX_S(94)
#define GCH   GFX_S(30)
/* stepper + apply controls */
#define STEP_Y  GFX_S(178)
#define STEP_DX GFX_S(72)
#define STEP_R  GFX_S(24)
#define APPLY_Y GFX_S(212)
#define APPLY_W GFX_S(120)
#define APPLY_H GFX_S(28)

static const char *MON3[12] = { "JAN","FEB","MAR","APR","MAY","JUN",
                                "JUL","AUG","SEP","OCT","NOV","DEC" };
static const char *FLABEL[F_COUNT] = { "HOUR","MIN","DAY","MONTH","YEAR","UTC" };

static void on_start(void)
{
    hal_set_backlight((uint8_t)s_bright);
    s_page = PAGE_MAIN;         /* always return to the main list on (re)entry */
    s_brightness_drag=s_brightness_changed=s_brightness_tap_done=0;
}

/* ---------------- date maths ---------------- */
static int days_in_month(int y, int m)
{
    static const int d[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return d[(m - 1) % 12];
}

/* day count since 1970-01-01 (Howard Hinnant's civil algorithm) */
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + doe - 719468L;
}

/* 0 = Sunday .. 6 = Saturday, via Zeller-equivalent from the day count */
static int weekday_of(int y, int m, int d)
{
    long days = days_from_civil(y, m, d);
    int w = (int)((days % 7 + 7 + 4) % 7);   /* 1970-01-01 was a Thursday (=4) */
    return w;
}

/* clamp the working day to the (possibly shorter) selected month */
static void clamp_day(void)
{
    int dim = days_in_month(s_edit.year, s_edit.month);
    if (s_edit.day > dim) s_edit.day = dim;
    if (s_edit.day < 1) s_edit.day = 1;
}

static void adjust_field(int delta)
{
    switch (s_field) {
    case F_HOUR: s_edit.hour = (s_edit.hour + delta + 24) % 24; break;
    case F_MIN:  s_edit.minute = (s_edit.minute + delta + 60) % 60; break;
    case F_DAY: {
        int dim = days_in_month(s_edit.year, s_edit.month);
        s_edit.day = (s_edit.day - 1 + delta + dim) % dim + 1;
        break;
    }
    case F_MON:
        s_edit.month = (s_edit.month - 1 + delta + 12) % 12 + 1;
        clamp_day();
        break;
    case F_YEAR:
        s_edit.year += delta;
        if (s_edit.year < 2020) s_edit.year = 2020;
        if (s_edit.year > 2099) s_edit.year = 2099;
        clamp_day();
        break;
    case F_TZ:
        s_edit_tz += delta * 15;              /* quarter-hour steps cover all zones */
        if (s_edit_tz < -720) s_edit_tz = -720;   /* UTC-12:00 */
        if (s_edit_tz > 840)  s_edit_tz = 840;     /* UTC+14:00 */
        break;
    default: break;
    }
}

static void open_setter(void)
{
    /* Snapshot the live clock once (static read — no per-frame latch needed;
     * we edit the copy, not the clock). Seconds start at 0 on a manual set. */
    hal_get_datetime(&s_edit);
    s_edit.second = 0;
    s_edit.millis = 0;
    if (s_edit.year < 2020 || s_edit.year > 2099) s_edit.year = 2026;
    if (s_edit.month < 1 || s_edit.month > 12) s_edit.month = 1;
    clamp_day();
    s_edit_tz = g_tz_offset_min;
    s_field = F_HOUR;
    s_page = PAGE_TIME;
}

static void apply_setter(void)
{
    s_edit.second = 0;
    s_edit.millis = 0;
    s_edit.weekday = weekday_of(s_edit.year, s_edit.month, s_edit.day);
    g_tz_offset_min = s_edit_tz;
    hal_set_datetime(&s_edit);               /* HAL converts local time to UTC and updates RTC */
    /* Record the local epoch we just set — best-effort staleness marker for a
     * future time sync. Stored as local seconds (no tz correction) on purpose:
     * without a real UTC reference there's nothing to correct against yet. */
    long days = days_from_civil(s_edit.year, s_edit.month, s_edit.day);
    g_time_last_set = (uint32_t)(days * 86400L
                      + s_edit.hour * 3600 + s_edit.minute * 60);
    settings_save();
    s_page = PAGE_MAIN;
}

/* ---------------- drawing helpers ---------------- */
static void fmt_tz(char *out, int n, int tz)
{
    char sign = tz < 0 ? '-' : '+';
    int a = tz < 0 ? -tz : tz;
    snprintf(out, n, "%c%d:%02d", sign, a / 60, a % 60);
}

static void field_value(int f, char *out, int n)
{
    switch (f) {
    case F_HOUR: snprintf(out, n, "%02d", s_edit.hour); break;
    case F_MIN:  snprintf(out, n, "%02d", s_edit.minute); break;
    case F_DAY:  snprintf(out, n, "%d", s_edit.day); break;
    case F_MON:  snprintf(out, n, "%s", MON3[(s_edit.month - 1) % 12]); break;
    case F_YEAR: snprintf(out, n, "%d", s_edit.year); break;
    case F_TZ:   fmt_tz(out, n, s_edit_tz); break;
    default:     out[0] = '\0'; break;
    }
}

/* rounded pill (stadium) from a rect + two end-caps */
static void pill(gfx_fb *fb, float cx, float cy, float w, float h, uint16_t col)
{
    float r = h / 2;
    gfx_fill_rect(fb, (int)(cx - w / 2 + r), (int)(cy - r), (int)(w - 2 * r), (int)h, col);
    gfx_fill_circle(fb, cx - w / 2 + r, cy, r, col);
    gfx_fill_circle(fb, cx + w / 2 - r, cy, r, col);
}

static void toggle_row_font(gfx_fb *fb, float y, const char *label, int on,const font_t *font)
{
    font_draw(fb, font, (int)GFX_S(54),
              (int)(y + font->ascent * 0.34f), label, GFX_WHITE);
    float kw = GFX_S(42), kh = GFX_S(24);
    float kx = GFX_W - GFX_S(44) - kw, ky = y - kh / 2;
    uint16_t track = on ? GFX_GREEN : GFX_DGREY;
    gfx_fill_rect(fb, (int)kx, (int)ky, (int)kw, (int)kh, track);
    float knob = kh / 2;
    float cx = on ? kx + kw - knob : kx + knob;
    gfx_fill_circle(fb, cx, y, knob - GFX_S(2), GFX_WHITE);
}
static void toggle_row(gfx_fb *fb,float y,const char *label,int on)
{ toggle_row_font(fb,y,label,on,&font_medium); }

/* ---------------- main page ---------------- */
static void draw_main(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);
    font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)GFX_S(44),
                       "Settings", GFX_WHITE);

    /* brightness slider */
    char brightness[24];snprintf(brightness,sizeof brightness,"BRIGHTNESS %d%%",(s_bright*100+127)/255);
    font_draw(fb, &font_small, (int)SLIDER_X0,
              (int)(SLIDER_Y - GFX_S(18)), brightness, GFX_GREY);
    gfx_line(fb, SLIDER_X0, SLIDER_Y, SLIDER_X1, SLIDER_Y, GFX_S(4), GFX_DGREY);
    float t = s_bright / 255.0f;
    float hx = SLIDER_X0 + (SLIDER_X1 - SLIDER_X0) * t;
    gfx_line(fb, SLIDER_X0, SLIDER_Y, hx, SLIDER_Y, GFX_S(4), GFX_ACCENT);
    gfx_fill_circle(fb, hx, SLIDER_Y, GFX_S(11), GFX_WHITE);

    toggle_row(fb, ROW1_Y, "24-Hour", g_setting_24h);
    font_draw(fb,&font_small,(int)GFX_S(54),(int)(ROW2_Y+font_small.ascent*.34f),"Wake gestures",GFX_WHITE);
    font_draw(fb,&font_medium,(int)GFX_S(184),(int)(ROW2_Y+font_medium.ascent*.34f),">",GFX_ACCENT);
    font_draw(fb,&font_small,(int)GFX_S(54),(int)(ROW3_Y+font_small.ascent*.34f),"Developer",GFX_WHITE);
    font_draw(fb,&font_medium,(int)GFX_S(184),(int)(ROW3_Y+font_medium.ascent*.34f),">",GFX_ACCENT);

    /* "Date & Time" entry button (centred pill). Shows the live clock — a plain
     * static read in on_draw is fine (nothing animates here). */
    pill(fb, GFX_CX, TIMEBTN_Y, TIMEBTN_W, TIMEBTN_H, GFX_DGREY);
    hal_datetime now;
    hal_get_datetime(&now);
    char b[24];
    snprintf(b, sizeof b, "%02d:%02d  Date & Time", now.hour, now.minute);
    icon_glyph(fb, ICON_CLOCK, GFX_CX - TIMEBTN_W / 2 + GFX_S(18), TIMEBTN_Y,
               GFX_S(9), GFX_ACCENT);
    font_draw_centered(fb, &font_small, (int)(GFX_CX + GFX_S(8)),
                       (int)(TIMEBTN_Y + font_small.ascent * 0.34f), b, GFX_WHITE);
}

/* ---------------- setter page ---------------- */
static void field_center(int i, float *cx, float *cy)
{
    *cx = (i % 2 == 0) ? GX0 : GX1;
    *cy = GRY0 + (i / 2) * GROW;
}

static void draw_setter(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(34),
                       "SET DATE & TIME", GFX_GREY);

    for (int i = 0; i < F_COUNT; i++) {
        float cx, cy;
        field_center(i, &cx, &cy);
        int sel = (i == s_field);
        if (sel)
            pill(fb, cx, cy + GFX_S(4), GCW, GCH, GFX_RGB(30, 46, 56));
        font_draw_centered(fb, &font_small, (int)cx, (int)(cy - GFX_S(4)),
                           FLABEL[i], sel ? GFX_ACCENT : GFX_GREY);
        char v[16];
        field_value(i, v, sizeof v);
        font_draw_centered(fb, &font_medium, (int)cx,
                           (int)(cy + GFX_S(14)), v, sel ? GFX_WHITE : GFX_GREY);
    }

    /* -/+ steppers flanking the selected field's name */
    gfx_fill_circle(fb, GFX_CX - STEP_DX, STEP_Y, STEP_R, GFX_DGREY);
    gfx_fill_circle(fb, GFX_CX + STEP_DX, STEP_Y, STEP_R, GFX_DGREY);
    icon_glyph(fb, ICON_MINUS, GFX_CX - STEP_DX, STEP_Y, GFX_S(11), GFX_WHITE);
    icon_glyph(fb, ICON_PLUS,  GFX_CX + STEP_DX, STEP_Y, GFX_S(11), GFX_WHITE);
    font_draw_centered(fb, &font_small, (int)GFX_CX,
                       (int)(STEP_Y + font_small.ascent * 0.34f),
                       FLABEL[s_field], GFX_ACCENT);

    /* APPLY */
    pill(fb, GFX_CX, APPLY_Y, APPLY_W, APPLY_H, GFX_GREEN);
    font_draw_centered(fb, &font_small, (int)GFX_CX,
                       (int)(APPLY_Y + font_small.ascent * 0.34f), "APPLY", GFX_BLACK);

    /* Back (‹) — kept inward from the top-left so it stays on the round glass */
    font_draw(fb, &font_medium, (int)GFX_S(34), (int)GFX_S(50), "<", GFX_GREY);
}

static void draw_wake(gfx_fb *fb)
{
    wake_info info;wake_service_get(&info);
    gfx_clear(fb,GFX_BLACK);
    font_draw_centered(fb,&font_medium,GFX_CX,GFX_S(34),"Motion wake",GFX_WHITE);
    font_draw(fb,&font_medium,GFX_S(34),GFX_S(50),"<",GFX_GREY);
    font_draw_centered(fb,&font_small,GFX_CX,GFX_S(57),"WATCH WRIST",GFX_GREY);
    for(int hand=0;hand<2;hand++) {
        float x=GFX_S(hand?154:86);int selected=info.wrist==hand;
        pill(fb,x,GFX_S(76),GFX_S(60),GFX_S(26),selected?GFX_ACCENT:GFX_DGREY);
        font_draw_centered(fb,&font_small,x,GFX_S(80),hand?"RIGHT":"LEFT",selected?GFX_BLACK:GFX_WHITE);
    }
    toggle_row_font(fb,GFX_S(112),"Pickup",info.pickup,&font_small);
    toggle_row_font(fb,GFX_S(146),"Wrist turn",info.turn,&font_small);
    pill(fb,GFX_CX,GFX_S(181),GFX_S(140),GFX_S(28),info.learning?GFX_DGREY:GFX_GREEN);
    font_draw_centered(fb,&font_small,GFX_CX,GFX_S(185),info.learning?"HOLD STILL...":"LEARN VIEW",info.learning?GFX_WHITE:GFX_BLACK);
    const char *status=info.learning?"Hold your reading pose":
        info.learn_result==-2?"Could not save. Retry":info.learn_result==-1?"Moved too much. Retry":
        info.learned?"Your viewing angle saved":"Using default view angle";
    font_draw_centered(fb,&font_small,GFX_CX,GFX_S(206),status,GFX_GREY);
    font_draw_centered(fb,&font_small,GFX_CX,GFX_S(223),info.learning?"Hold still 1 second":"Lift or turn to wake",GFX_GREY);
}
/* Developer page: rendering diagnostics */
#define DEV_ROW1_Y GFX_S(78)
#define DEV_ROW2_Y GFX_S(108)
#define DEV_ROW3_Y GFX_S(138)
#define DEV_ROW4_Y GFX_S(168)
#define DEV_HIT    GFX_S(15)       /* rows sit 30 apart: half-pitch tap zones */
static void draw_dev(gfx_fb *fb)
{
    gfx_clear(fb,GFX_BLACK);
    font_draw_centered(fb,&font_medium,GFX_CX,GFX_S(34),"Developer",GFX_WHITE);
    font_draw(fb,&font_medium,GFX_S(34),GFX_S(50),"<",GFX_GREY);
    toggle_row_font(fb,DEV_ROW1_Y,"FPS HUD",wos_debug_hud,&font_small);
    toggle_row_font(fb,DEV_ROW2_Y,"Redraw boxes",wos_debug_rects,&font_small);
    toggle_row_font(fb,DEV_ROW3_Y,"48 FPS cap",wos_fps_cap48,&font_small);
    /* Fast display cycles Off -> 256 colours -> grey on tap */
    static const char *const fast_names[3] = { "Off", "256", "Grey" };
    font_draw(fb,&font_small,(int)GFX_S(54),(int)(DEV_ROW4_Y+font_small.ascent*0.34f),"Fast display",GFX_WHITE);
    const char *fv = fast_names[wos_fast_display % 3];
    float pw = GFX_S(42), ph = GFX_S(24), px = GFX_W - GFX_S(44) - pw / 2;
    pill(fb,px,DEV_ROW4_Y,pw,ph,wos_fast_display ? GFX_GREEN : GFX_DGREY);
    font_draw_centered(fb,&font_small,(int)px,(int)(DEV_ROW4_Y+font_small.ascent*0.34f),fv,
                       wos_fast_display ? GFX_BLACK : GFX_WHITE);
    font_draw_centered(fb,&font_small,GFX_CX,GFX_S(196),"Boxes: green stripes, red atomic",GFX_GREY);
}
static void on_draw(gfx_fb *fb)
{
    if(s_page==PAGE_DEV)draw_dev(fb);
    else if(s_page==PAGE_WAKE)draw_wake(fb);
    else if (s_page == PAGE_TIME)
        draw_setter(fb);
    else
        draw_main(fb);
}

/* ---------------- events ---------------- */
static int hit_circle(const wos_event *ev, float cx, float cy, float r)
{
    float dx = ev->x - cx, dy = ev->y - cy;
    return dx * dx + dy * dy <= r * r;
}

static int brightness_hit(int x,int y)
{
    return x>=SLIDER_X0-GFX_S(14) && x<=SLIDER_X1+GFX_S(14) &&
           y>=SLIDER_Y-GFX_S(16) && y<=SLIDER_Y+GFX_S(16);
}
static int brightness_apply(int x)
{
    float t=(x-SLIDER_X0)/(SLIDER_X1-SLIDER_X0);
    if(t<0)t=0;
    if(t>1)t=1;
    int value=(int)(t*255+0.5f);
    if(value<5)value=5;
    if(value==s_bright)return 0;
    s_bright=value;hal_set_backlight((uint8_t)value);
    return 1;
}
static int on_event_main(const wos_event *ev)
{
    if(ev->type==WOS_EV_TOUCH_DOWN) {
        s_brightness_tap_done=0;
        s_brightness_drag=brightness_hit(ev->x,ev->y);
        s_brightness_changed=s_brightness_drag ? brightness_apply(ev->x) : 0;
        return s_brightness_drag;
    }
    if(ev->type==WOS_EV_TOUCH_MOVE && s_brightness_drag) {
        s_brightness_changed|=brightness_apply(ev->x);
        return 1;
    }
    if(ev->type==WOS_EV_TOUCH_UP && s_brightness_drag) {
        s_brightness_changed|=brightness_apply(ev->x);
        if(s_brightness_changed)settings_save(); /* Once on release, not per sample. */
        s_brightness_drag=0;s_brightness_tap_done=1;
        return 1;
    }
    if (ev->type != WOS_EV_TAP)
        return 0;
    /* The OS emits TAP after raw UP. Avoid saving the same gesture twice,
     * or toggling a row if a short slider drag ended beyond the hit area. */
    if(s_brightness_tap_done) {s_brightness_tap_done=0;return 1;}
    if (brightness_hit(ev->x,ev->y)) {
        if(brightness_apply(ev->x))settings_save();
        return 1;
    }
    if (ev->y > ROW1_Y - ROW_H / 2 && ev->y < ROW1_Y + ROW_H / 2) {
        g_setting_24h = !g_setting_24h; settings_save(); return 1;
    }
    if (ev->y > ROW2_Y - ROW_H / 2 && ev->y < ROW2_Y + ROW_H / 2) {
        s_page=PAGE_WAKE;return 1;
    }
    if (ev->y > ROW3_Y - ROW_H / 2 && ev->y < ROW3_Y + ROW_H / 2) {
        s_page=PAGE_DEV;return 1;
    }
    /* Date & Time entry button */
    if (ev->x > GFX_CX - TIMEBTN_W / 2 && ev->x < GFX_CX + TIMEBTN_W / 2 &&
        ev->y > TIMEBTN_Y - TIMEBTN_H / 2 && ev->y < TIMEBTN_Y + TIMEBTN_H / 2) {
        open_setter();
        return 1;
    }
    return 0;
}

static int on_event_setter(const wos_event *ev)
{
    switch (ev->type) {
    case WOS_EV_TAP:
        /* Back */
        if (ev->x < GFX_S(58) && ev->y < GFX_S(70)) {
            s_page = PAGE_MAIN;
            return 1;
        }
        /* steppers */
        if (hit_circle(ev, GFX_CX - STEP_DX, STEP_Y, STEP_R + GFX_S(6))) {
            adjust_field(-1);
            return 1;
        }
        if (hit_circle(ev, GFX_CX + STEP_DX, STEP_Y, STEP_R + GFX_S(6))) {
            adjust_field(+1);
            return 1;
        }
        /* APPLY */
        if (ev->x > GFX_CX - APPLY_W / 2 && ev->x < GFX_CX + APPLY_W / 2 &&
            ev->y > APPLY_Y - APPLY_H / 2 && ev->y < APPLY_Y + APPLY_H / 2) {
            apply_setter();
            return 1;
        }
        /* field cells -> select */
        for (int i = 0; i < F_COUNT; i++) {
            float cx, cy;
            field_center(i, &cx, &cy);
            if (ev->x > cx - GCW / 2 && ev->x < cx + GCW / 2 &&
                ev->y > cy - GCH / 2 && ev->y < cy + GCH / 2 + GFX_S(8)) {
                s_field = i;
                return 1;
            }
        }
        return 1;   /* stay on the page; swallow stray taps */
    case WOS_EV_SWIPE_UP:
        adjust_field(+1);
        return 1;
    case WOS_EV_SWIPE_DOWN:
        adjust_field(-1);
        return 1;
    case WOS_EV_SWIPE_RIGHT:   /* swipe back to the main list */
        s_page = PAGE_MAIN;
        return 1;
    default:
        return 1;              /* keep the setter modal while it's open */
    }
}

static int on_event(const wos_event *ev)
{
    if(s_page==PAGE_DEV) {
        if(ev->type==WOS_EV_SWIPE_RIGHT){s_page=PAGE_MAIN;return 1;}
        if(ev->type==WOS_EV_TAP) {
            if(ev->x<GFX_S(58) && ev->y<GFX_S(65))s_page=PAGE_MAIN;
            else if(ev->y>DEV_ROW1_Y-DEV_HIT && ev->y<DEV_ROW1_Y+DEV_HIT){wos_debug_hud=!wos_debug_hud;settings_save();}
            else if(ev->y>DEV_ROW2_Y-DEV_HIT && ev->y<DEV_ROW2_Y+DEV_HIT){wos_debug_rects=!wos_debug_rects;settings_save();}
            else if(ev->y>DEV_ROW3_Y-DEV_HIT && ev->y<DEV_ROW3_Y+DEV_HIT){wos_fps_cap48=!wos_fps_cap48;settings_save();}
            else if(ev->y>DEV_ROW4_Y-DEV_HIT && ev->y<DEV_ROW4_Y+DEV_HIT){wos_fast_display=(wos_fast_display+1)%3;settings_save();}
            return 1;
        }
        return 0;
    }
    if(s_page==PAGE_WAKE) {
        if(ev->type==WOS_EV_SWIPE_RIGHT){s_page=PAGE_MAIN;return 1;}
        if(ev->type==WOS_EV_TAP) {
            wake_info info;wake_service_get(&info);
            if(ev->x<GFX_S(58) && ev->y<GFX_S(65))s_page=PAGE_MAIN;
            else if(ev->y>GFX_S(62) && ev->y<GFX_S(90))wake_service_set("wrist",ev->x>=GFX_CX);
            else if(ev->y>GFX_S(96) && ev->y<GFX_S(128))wake_service_set("pickup",!info.pickup);
            else if(ev->y>GFX_S(130) && ev->y<GFX_S(162))wake_service_set("turn",!info.turn);
            else if(ev->y>GFX_S(166) && ev->y<GFX_S(196) && !info.learning)wake_service_learn();
            return 1;
        }
        return 0;
    }
    if (s_page == PAGE_TIME)
        return on_event_setter(ev);
    return on_event_main(ev);
}

const wos_app app_settings = { "Settings", on_start, on_event, on_draw, GFX_GREY, ICON_SETTINGS };
