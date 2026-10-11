/* con_cell.c — Cellular (Quectel EG800AK strap module): status, SMS inbox, SMS reader.
 *
 * Status: four signal bars built from dot columns (the focal element) with the RSSI
 * in dBm beneath, the operator, technology and registration, and two round buttons
 * in the thumb zone: radio power (on = white) and messages (with an unread badge).
 * States: module absent, radio off, starting, no SIM, SIM locked, searching, live.
 * Inbox: a card per message (unread dot, sender, time, one-line preview).
 * Reader: sender, time, the wrapped text, then quick-reply pills sent through
 * hal_lte_sms_send with a "[ SENT ]" confirmation. */
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
#include <ctype.h>

static float q(float x, float n) { return floorf(m_clamp(x, 0, 1) * n + 0.5f) / n; }

/* ----------------------------------------------------------- shared data */
#define SMS_MAX 16
static hal_sms *s_sms;                 /* PSRAM, SMS_MAX entries (internal DRAM is full) */
static int s_nsms;
static int s_open = -1;                /* message shown by the reader */

static void sms_refresh(void)
{
    if (!s_sms) s_sms = (hal_sms *)hal_big_alloc(sizeof(hal_sms) * SMS_MAX);
    s_nsms = s_sms ? hal_lte_sms_list(s_sms, SMS_MAX) : 0;
    if (s_nsms < 0) s_nsms = 0;
}

/* "yy/mm/dd,hh:mm:ss" -> "09:58" today, else "10 OCT" */
static void sms_when(const hal_sms *m, char *out, int n)
{
    int yy = 0, mo = 0, dd = 0, hh = 0, mi = 0;
    out[0] = 0;
    if (sscanf(m->when, "%d/%d/%d,%d:%d", &yy, &mo, &dd, &hh, &mi) < 5) {
        snprintf(out, (size_t)n, "%s", m->when);
        return;
    }
    hal_datetime now;
    hal_get_datetime(&now);
    if (now.year % 100 == yy && now.month == mo && now.day == dd) ui_fmt_time(out, n, hh, mi);
    else snprintf(out, (size_t)n, "%d %s", dd, ui_month3(mo));
}

static int can_reply(const hal_sms *m)
{
    for (const char *p = m->from; *p; p++) if (isalpha((unsigned char)*p)) return 0;   /* sender IDs */
    return m->from[0] != 0;
}

/* One centred line of body text fitted inside the circle at its row. */
static void body_line(gfx_fb *fb, int y, const char *s, uint16_t c)
{
    int x0, x1;
    ui_row_span((float)(y - 8), 40, &x0, &x1);
    ui_text_fit(fb, UI_F_BODY, UI_CX, y, s, c, UI_CENTER, x1 - x0);
}

/* ============================================================ status ===== */
#define BAR_PITCH 12.0f
#define BAR_D     9.0f
#define BAR_BOT   206.0f
#define BTN_Y     360.0f
#define BTN_R     44.0f              /* 88 px: the OS button size */
#define BTN_DX    64.0f

enum { CB_NONE = 0, CB_POWER, CB_SMS };
enum { LS_ABSENT = 0, LS_OFF, LS_STARTING, LS_NOSIM, LS_LOCKED, LS_SEARCHING, LS_LIVE };

static struct {
    float t_enter, build, txt;
    hal_lte l;
    int   present;
    int   want_on;            /* the user asked for the radio */
    int   st;                 /* LS_* */
    int   pressed;
    int   sweep;              /* searching animation step */
    int   sweep_frame;
    int   unread;
    mtween fill;              /* power button white fill 0..1 */
    int   fill_on;
} L;

static int lte_state(void)
{
    if (!L.present) return LS_ABSENT;
    if (!L.want_on && !L.l.powered) return LS_OFF;
    if (!L.l.powered) return LS_STARTING;
    if (L.l.sim == 0) return LS_NOSIM;
    if (L.l.sim == 2) return LS_LOCKED;
    if (L.l.registered == 1 || L.l.registered == 2) return LS_LIVE;
    return LS_SEARCHING;
}

static void cell_enter(void)
{
    /* back from the inbox: no rebuild; opened by the iris: build from 40% of it */
    L.t_enter = nos_revealed() ? nos_t - 10.0f : nos_t + 0.4f * 0.42f;
    L.build = L.txt = 0;
    L.pressed = CB_NONE;
    L.want_on = nos_prefs.lte;
    if (L.want_on) hal_lte_enable(1);
    L.present = hal_lte_get(&L.l);
    L.st = lte_state();
    L.fill_on = L.want_on || L.l.powered;
    mtw_start(&L.fill, (float)L.fill_on, (float)L.fill_on, 0.01f, &EASE_MECH);
    sms_refresh();
}

static void cell_tick(float dt)
{
    L.build = q((nos_t - L.t_enter) / UI_ENTRY_S, 32);
    L.txt = nos_t >= L.t_enter ? 1.0f : 0.0f;           /* words arrive with the build, at full ink */
    L.present = hal_lte_get(&L.l);
    L.st = lte_state();
    L.unread = 0;
    for (int i = 0; i < s_nsms; i++) L.unread += s_sms[i].unread ? 1 : 0;
    if (L.l.unread_sms > L.unread) L.unread = L.l.unread_sms;
    int frames = (int)lroundf(dt * NOS_FPS);
    L.sweep_frame += frames < 1 ? 1 : frames;
    if (L.sweep_frame >= 6) { L.sweep_frame = 0; L.sweep = (L.sweep + 1) % 6; }
    int on = L.want_on || L.l.powered;
    if (on != L.fill_on) {               /* the modem can drop on its own */
        L.fill_on = on;
        mtw_start(&L.fill, mtw_value(&L.fill), (float)on, 0.18f, &EASE_MECH);
    }
    if (L.st == LS_STARTING || L.st == LS_SEARCHING) nos_keep_awake();
}

static void draw_bars(gfx_fb *fb, float build)
{
    const float bw = 2 * BAR_PITCH, gap = BAR_PITCH;
    float x0 = UI_CX - (4 * bw + 3 * gap) * 0.5f;
    for (int i = 0; i < 4; i++) {
        int rows = 2 * (i + 1);
        uint16_t on;
        int lit;
        switch (L.st) {
        case LS_LIVE:      lit = i < L.l.bars; on = C_WHITE; break;
        case LS_STARTING:
        case LS_SEARCHING: lit = i == L.sweep; on = C_SEC; break;   /* steps 4-5: a beat of rest */
        default:           lit = 0; on = C_WHITE; break;
        }
        uint16_t off = L.st == LS_LIVE ? C_LINE2 : C_LINE;
        /* bars build from the bottom up, one after another */
        float bb = q(build * 1.6f - i * 0.2f, 16);
        uint8_t v[2];
        v[0] = v[1] = (uint8_t)(255.0f * bb);
        dot_columns(fb, x0 + i * (bw + gap), BAR_BOT - rows * BAR_PITCH, 2, rows, BAR_PITCH, BAR_D, v,
                    lit ? on : off, 0);
    }
}

static void cell_button(gfx_fb *fb, float cx, int icon, int on, int pressed, int disabled, float r, float p)
{
    uint16_t fill = on ? (pressed ? C_SEC : C_WHITE) : disabled ? C_SURF : (pressed ? C_LINE2 : C_SURF2);
    uint16_t ink = on ? C_BG : disabled ? C_DIS : C_PRI;
    ui_circle(fb, cx, BTN_Y, r, fill);
    if (icon) dot_icon_anim(fb, icon, cx, BTN_Y, 3.4f, 2.7f, ink, p, DOT_REVEAL_CENTER);
}

/* Power glyph in dots: an open ring (gap at the top) and a stem. */
static void power_glyph(gfx_fb *fb, float cx, float cy, uint16_t ink, float p)
{
    if (p <= 0) return;
    dot_ring(fb, cx, cy + 2, 13, 12, 3.6f, 2, 2 + 9 * p, ink, 0);
    for (int k = 0; k < 3; k++) if (p * 3 > k) dot_disc(fb, cx, cy - 14 + k * 5.0f, 3.6f, ink);
}

static void cell_draw(gfx_fb *fb)
{
    ui_title(fb, "CELLULAR", 0);
    uint16_t c1 = ui_dim(C_PRI, L.txt), c2 = ui_dim(C_SEC, L.txt), c3 = ui_dim(C_DIS, L.txt);
    if (L.st == LS_ABSENT) {
        ui_absent(fb, DOT_ICON_SIGNAL, "Not connected", "Add the LTE strap to text from anywhere, no phone needed",
                  "LTE MODULE", L.build);
        return;
    }
    if (nos_t < L.t_enter) return;
    draw_bars(fb, L.build);
    char b[40];
    switch (L.st) {
    case LS_LIVE:
        snprintf(b, sizeof b, "%d DBM", L.l.rssi_dbm);
        ui_label(fb, UI_CX, 238, L.l.rssi_dbm ? b : "-- DBM", c2, UI_CENTER);
        body_line(fb, 284, L.l.op[0] ? L.l.op : "Unknown network", c1);
        snprintf(b, sizeof b, "%s / %s", L.l.tech[0] ? L.l.tech : "LTE", L.l.registered == 2 ? "ROAMING" : "HOME");
        ui_label(fb, UI_CX, 312, b, c3, UI_CENTER);
        break;
    case LS_OFF:
        ui_state(fb, UI_CX, 238, "OFF", c2);
        body_line(fb, 290, "Tap RADIO to turn on", c1);
        break;
    case LS_STARTING:
        ui_state(fb, UI_CX, 238, "STARTING", c2);
        body_line(fb, 290, "Waking the modem", c1);
        break;
    case LS_NOSIM:
        ui_state(fb, UI_CX, 238, "NO SIM", c2);
        body_line(fb, 290, "Insert a nano SIM", c1);
        break;
    case LS_LOCKED:
        ui_state(fb, UI_CX, 238, "SIM LOCKED", c2);
        body_line(fb, 290, "Remove the PIN on a phone", c1);
        break;
    default:
        ui_state(fb, UI_CX, 238, "SEARCHING", c2);
        body_line(fb, 290, "Looking for a network", c1);
        break;
    }
    /* thumb zone: radio power, messages; full size, the glyphs build */
    float ib = q((nos_t - L.t_enter) / UI_ENTRY_S, 32);
    int on = L.want_on || L.l.powered;
    {   /* power: a white disc grows from the centre as the radio turns on */
        float f = q(mtw_value(&L.fill), 16);
        int pr = L.pressed == CB_POWER;
        cell_button(fb, UI_CX - BTN_DX, DOT_ICON_NONE, 0, pr, 0, BTN_R, ib);
        if (BTN_R * f > 1) ui_circle(fb, UI_CX - BTN_DX, BTN_Y, BTN_R * f, pr ? C_SEC : C_WHITE);
        power_glyph(fb, UI_CX - BTN_DX, BTN_Y, f > 0.6f ? C_BG : C_PRI, ib);
    }
    cell_button(fb, UI_CX + BTN_DX, DOT_ICON_MSG, 0, L.pressed == CB_SMS, 0, BTN_R, ib);
    if (L.unread > 0 && ib >= 1) {
        float bx = UI_CX + BTN_DX + 31, by = BTN_Y - 31;
        ui_circle(fb, bx, by, 14, C_WHITE);
        snprintf(b, sizeof b, "%d", L.unread > 9 ? 9 : L.unread);
        ui_text(fb, UI_F_LABEL, (int)bx, (int)(by + UI_F_LABEL->ascent * 0.5f - 1), b, C_BG, UI_CENTER, 0);
    }
    ui_label(fb, (int)(UI_CX - BTN_DX), 430, on ? "RADIO ON" : "RADIO", c2, UI_CENTER);
    ui_label(fb, (int)(UI_CX + BTN_DX), 430, "SMS", c2, UI_CENTER);
}

static int cell_btn_at(int x, int y)
{
    float dy = y - BTN_Y, dx = x - (UI_CX - BTN_DX);
    if (dx * dx + dy * dy <= 50 * 50) return CB_POWER;
    dx = x - (UI_CX + BTN_DX);
    if (dx * dx + dy * dy <= 50 * 50) return CB_SMS;
    return CB_NONE;
}

static const nos_screen scr_cell_sms;

static int cell_input(const nos_input *in)
{
    if (L.st == LS_ABSENT) return 0;
    switch (in->type) {
    case IN_DOWN:
        L.pressed = cell_btn_at(in->x, in->y);
        return 1;
    case IN_TAP: {
        int b = cell_btn_at(in->x, in->y);
        L.pressed = CB_NONE;
        if (b == CB_POWER) {
            int on = !(L.want_on || L.l.powered);
            L.want_on = on;
            hal_lte_enable(on);
            nos_prefs.lte = (uint8_t)on;
            prefs_save();
            nos_feedback(on ? FB_TOGGLE_ON : FB_TOGGLE_OFF);
        } else if (b == CB_SMS) {
            nos_feedback(FB_TAP);
            nos_push(&scr_cell_sms, TX_PUSH);
        }
        return 1;
    }
    case IN_UP:
    case IN_DRAG_START:
        L.pressed = CB_NONE;
        return 0;
    default:
        return 0;
    }
}

const nos_screen scr_cell = { "CELLULAR", cell_enter, 0, cell_tick, cell_draw, cell_input, 0 };

/* ============================================================= inbox ===== */
#define ROW_H    UI_ROW_H
#define CARD_X0  53               /* fixed card: 360 px, text at card + 40 */
#define CARD_W   360

static struct {
    ui_list list;
    float t_enter;
    float txt;
    int   inited;
} I;

static const nos_screen scr_cell_read;

static void inbox_row(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed)
{
    if (i >= s_nsms) return;
    const hal_sms *m = &s_sms[i];
    /* rows stagger in 34 ms apart (6 rows at most) */
    float st = q((nos_t - I.t_enter - 0.034f * (i < 6 ? i : 6)) / 0.15f, 4);
    float k = fade * st;
    if (k <= 0) return;
    /* fixed-width cards: the glass edge crops a low card, the text never jumps sideways */
    ui_rrect(fb, (float)CARD_X0, (float)(y + 4), (float)CARD_W, (float)(h - 8), 40,
             ui_dim(pressed ? C_LINE2 : C_SURF2, k));
    int tx = CARD_X0 + 40, tr = UI_VALUE_X;
    if (m->unread) dot_disc(fb, (float)(CARD_X0 + 21), (float)(y + 30), 9, ui_dim(C_WHITE, k));
    /* line 1: sender, time at the right; line 2: the whole preview */
    char when[24];
    sms_when(m, when, sizeof when);
    int ww = ui_label(fb, tr, y + 37, when, ui_dim(C_DIS, k), UI_RIGHT);
    ui_text_fit(fb, UI_F_BODY, tx, y + 38, m->from, ui_dim(m->unread ? C_WHITE : C_PRI, k), UI_LEFT, tr - ww - 12 - tx);
    ui_text_fit(fb, UI_F_BODY, tx, y + 70, m->text, ui_dim(C_SEC, k), UI_LEFT, tr - tx);
}

static void inbox_tap(int i)
{
    if (i < 0 || i >= s_nsms) return;
    s_open = i;
    nos_push(&scr_cell_read, TX_PUSH);
}

static void inbox_enter(void)
{
    sms_refresh();
    if (!I.inited) { ui_list_init(&I.list, s_nsms, ROW_H, 104, inbox_row, inbox_tap); I.inited = 1; }
    else ui_list_set_n(&I.list, s_nsms);
    I.list.margin = 30;
    /* revealed by back (reader popped): keep the cards, no re-stagger */
    if (!nos_revealed()) I.t_enter = nos_t;
}

static void inbox_tick(float dt)
{
    ui_list_tick(&I.list, dt);
    I.txt = 1.0f;
}

static void inbox_draw(gfx_fb *fb)
{
    if (!s_nsms) {
        ui_title(fb, "MESSAGES", 0);
        ui_absent(fb, DOT_ICON_MSG, "No messages", "Texts to your SIM land here", NULL,
                  q((nos_t - I.t_enter) / UI_ENTRY_S, 32));
        return;
    }
    ui_list_draw(fb, &I.list);
    ui_title(fb, "MESSAGES", ui_list_offset(&I.list));
}

static int inbox_input(const nos_input *in) { return s_nsms ? ui_list_input(&I.list, in) : 0; }

static const nos_screen scr_cell_sms = { "MESSAGES", inbox_enter, 0, inbox_tick, inbox_draw, inbox_input, 0 };

/* ============================================================ reader ===== */
#define REPLY_H   UI_ROW_H
#define NREPLY    5
#define TEXT_W    320
#define LINE_H    34
static const char *const k_replies[NREPLY] = { "OK", "On my way", "Call you later", "Yes", "No" };

static struct {
    ui_list list;
    float t_enter, txt;
    int   lines;               /* wrapped body lines */
    int   header_h;
    int   reply;               /* can reply */
    int   result;              /* 0 none, 1 sent, -1 failed */
    float t_result;
    int   sent_i;
    int   popped;
    int   inited;
} R;

static int count_lines(const char *s, int max_w)
{
    /* the same greedy wrap ui_text_wrap does, counted without drawing */
    int lines = 0;
    const char *p = s;
    char buf[128];
    while (*p && lines < 8) {
        while (*p == ' ') p++;
        if (!*p) break;
        int best = 0, n = 0;
        while (p[n] && p[n] != '\n' && n < 120) {
            n++;
            if (p[n] == ' ' || p[n] == 0 || p[n] == '\n') {
                memcpy(buf, p, (size_t)n); buf[n] = 0;
                if (ui_text_width(UI_F_BODY, buf, 0) <= max_w) best = n; else break;
            }
        }
        if (!best) best = n ? n : 1;
        lines++;
        p += best;
        if (*p == '\n') p++;
    }
    return lines ? lines : 1;
}

static void reply_row(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed)
{
    float st = q((nos_t - R.t_enter - 0.1f - 0.034f * i) / 0.15f, 4);
    float k = fade * st;
    if (k <= 0) return;
    float w = 240, ph = 60;                         /* chip: 60 tall in a 90 px hit row */
    ui_rrect(fb, UI_CX - w / 2, (float)y + (h - ph) / 2, w, ph, ph / 2, ui_dim(pressed ? C_LINE2 : C_SURF2, k));
    ui_text(fb, UI_F_BODY, UI_CX, y + h / 2 + 8, k_replies[i], ui_dim(C_PRI, k), UI_CENTER, 0);
}

static void reply_tap(int i)
{
    if (R.result || s_open < 0 || i < 0 || i >= NREPLY) return;
    int ok = hal_lte_sms_send(s_sms[s_open].from, k_replies[i]);
    R.result = ok ? 1 : -1;
    R.t_result = nos_t;
    R.sent_i = i;
    nos_feedback(ok ? FB_SUCCESS : FB_ERROR);
}

static void read_enter(void)
{
    if (s_open >= 0 && s_open < s_nsms && s_sms[s_open].unread) {
        hal_lte_sms_mark_read(s_open);
        s_sms[s_open].unread = 0;
    }
    R.t_enter = nos_t;
    R.result = 0;
    R.popped = 0;
    const hal_sms *m = (s_open >= 0 && s_open < s_nsms) ? &s_sms[s_open] : 0;
    R.lines = m ? count_lines(m->text, TEXT_W) : 1;
    if (R.lines > 7) R.lines = 7;
    R.reply = m && can_reply(m) && (hal_ext_present() & EXT_LTE);
    R.header_h = 136 + R.lines * LINE_H + (R.reply ? 52 : 40);
    int n = R.reply ? NREPLY : 0;
    if (!R.inited) { ui_list_init(&R.list, n, REPLY_H, R.header_h, reply_row, reply_tap); R.inited = 1; }
    else {
        R.list.top = R.header_h;
        ui_list_set_n(&R.list, n);
        msc_jump(&R.list.sc, 0);
    }
    R.list.bottom_pad = 70;
    ui_list_set_n(&R.list, n);
}

static void read_tick(float dt)
{
    ui_list_tick(&R.list, dt);
    R.txt = 1.0f;
    if (R.result == 1 && !R.popped && nos_t - R.t_result > 1.3f && !nos_transitioning()) { R.popped = 1; nos_pop(); }
    if (R.result == -1 && nos_t - R.t_result > 1.6f) R.result = 0;
}

static void read_draw(gfx_fb *fb)
{
    const hal_sms *m = (s_open >= 0 && s_open < s_nsms) ? &s_sms[s_open] : 0;
    if (!m) { ui_state(fb, UI_CX, UI_CY, "NO MESSAGE", C_SEC); return; }
    if (R.result) {
        float p = q((nos_t - R.t_result) / 0.3f, 32);
        dot_icon_anim(fb, R.result > 0 ? DOT_ICON_CHECK : DOT_ICON_CROSS, UI_CX, 196, 8.0f, 6.4f,
                      C_WHITE, p, DOT_REVEAL_CENTER);
        ui_state(fb, UI_CX, 300, R.result > 0 ? "SENT" : "NOT SENT", C_PRI);
        char b[64];
        if (R.result > 0) snprintf(b, sizeof b, "\"%s\"", k_replies[R.sent_i]);
        else snprintf(b, sizeof b, "Check the signal and try again");
        body_line(fb, 336, b, C_SEC);
        return;
    }
    int off = (int)floorf(ui_list_offset(&R.list) + 0.5f);
    uint16_t c1 = ui_dim(C_PRI, R.txt), c2 = ui_dim(C_SEC, R.txt), c3 = ui_dim(C_DIS, R.txt);
    char when[24];
    sms_when(m, when, sizeof when);
    int y = 108 - off;
    if (y > -40) {
        int x0, x1;
        ui_label(fb, UI_CX, 70 - off, when, c3, UI_CENTER);
        ui_row_span((float)(y - 24), 40, &x0, &x1);
        ui_text_fit(fb, UI_F_TITLE, UI_CX, y, m->from, ui_dim(C_WHITE, R.txt), UI_CENTER, x1 - x0);
    }
    ui_text_wrap(fb, UI_F_BODY, UI_CX, 160 - off, TEXT_W, LINE_H, 7, m->text, c1, UI_CENTER);
    int ly = R.header_h - (R.reply ? 22 : 10) - off;
    ui_label(fb, UI_CX, ly, R.reply ? "QUICK REPLY" : "REPLIES NOT SUPPORTED", R.reply ? c2 : c3, UI_CENTER);
    ui_list_draw(fb, &R.list);
}

static int read_input(const nos_input *in)
{
    if (R.result) {
        if (in->type == IN_TAP && R.result > 0 && !R.popped) { R.popped = 1; nos_pop(); }
        return in->type == IN_TAP || in->type == IN_DOWN || in->type == IN_UP;
    }
    return ui_list_input(&R.list, in);
}

static const nos_screen scr_cell_read = { "MESSAGE", read_enter, 0, read_tick, read_draw, read_input, 0 };
