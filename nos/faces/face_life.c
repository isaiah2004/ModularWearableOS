/* face_life.c — LIFE: life in dots.
 *
 * The hour as a 10 x 6 grid of dots: one row per ten minutes, one column per minute
 * (10:42 reads as four full rows and two dots), past minutes white, future ones
 * #2A2A2A, the present minute the red dot, breathing gently through its size. Around
 * the bezel, the day as 24 hour dots (past grey, this hour white, future dark). Date
 * above; below, a small grotesk time and (when low / charging) the battery.
 *
 * Motion: build-in = the past minutes light one by one in reading order (and the day
 * ring with them), then the red dot grows in. Minute change = the old red dot whitens
 * over 3 frames while the next one grows from 40 %, and the changed digit of the time
 * rolls. Top of the hour = the grid drains in reverse at 6 dots a frame, then the new
 * red dot grows. Wake from AOD = the small AOD dots grow into the grid (4 steps).
 * Cost: per frame only the red dot's box (8 size steps on a 2 s breath); everything
 * else changes once a minute. 12 h: "9:41" (no leading zero). */
#include "faces/face_kit.h"
#include "nos.h"
#include <stdio.h>
#include <string.h>

#define COLS   10
#define ROWS   6
#define PITCH  32.0f
#define DOTD   23.0f
#define GX0    (UI_CX - PITCH * (COLS - 1) * 0.5f)
#define GY0    (UI_CY - PITCH * (ROWS - 1) * 0.5f + 4.0f)
#define R_DAY  212.0f
#define C_FUT  C_SURF3
#define Y_DATE 106
#define Y_TIME 380
#define Y_STAT 408
#define DRAIN_S (10.0f / 30.0f)                      /* 60 dots at 6 a frame */

#define U_DY   40.0f                                 /* unread dot at y 64, in AOD too */

static struct { float change_t; int last_m; char tfrom[8], tto[8]; fk_wake w; fk_xfade date; } S = { .change_t = -100.0f, .last_m = -1 };

static void enter(void) { S.last_m = -1; }

static void tick(const face_ctx *c, float dt)
{
    fk_aod_update(c);
    fk_wake_tick(&S.w, c);
    fk_unread_tick(c);
    char b[8], db[16];
    int stale = fk_wake_stale(&S.w);
    fk_time(b, c->dt.hour, c->dt.minute);                /* 12 h: "9:41", no leading zero */
    if (c->dt.minute != S.last_m) {
        if (S.last_m >= 0 && !stale) { S.change_t = c->t; memcpy(S.tfrom, S.tto, sizeof S.tfrom); }
        else memcpy(S.tfrom, b, sizeof S.tfrom);
        S.last_m = c->dt.minute;
    }
    if (stale) { S.change_t = -100.0f; memcpy(S.tfrom, b, sizeof S.tfrom); }   /* off the glass: no replay */
    memcpy(S.tto, b, sizeof S.tto);
    if (strlen(S.tfrom) != strlen(S.tto)) memcpy(S.tfrom, S.tto, sizeof S.tfrom);
    fk_date(db, c);
    fk_xfade_tick(&S.date, db, c->t, c->aod || stale || fk_build(&S.w, c) < 1.0f || fk_wake_k(&S.w, c) < 1.0f);
}

typedef struct {
    float past; int now; float dx, dy; float k; int hour; float hpast;
    int prev; float whiten;                          /* the old now-dot turning white */
    int skip_now;
} life_ctx;

static void grid_dot(int i, const void *vc, float *x, float *y, float *d, uint16_t *col)
{
    const life_ctx *L = (const life_ctx *)vc;
    *x = GX0 + (float)(i % COLS) * PITCH + L->dx;
    *y = GY0 + (float)(i / COLS) * PITCH + L->dy;
    float k = L->k;
    if (k < 1.0f) {                                   /* AOD (k = 0) and the wake morph */
        if (i == L->now) { *d = 0; *col = 0; return; }
        int past = i < L->now;
        float d0 = past ? 7.0f : 3.0f;
        *d = d0 + (DOTD - d0) * k;
        *col = past ? ui_mix(C_DIS, C_PRI, k) : ui_mix(C_LINE2, C_FUT, k);
        return;
    }
    if (i == L->now && L->skip_now) { *d = 0; *col = 0; return; }   /* the red dot draws itself */
    *d = DOTD;
    if (i == L->prev && L->whiten < 1.0f) { *col = ui_mix(C_RED, C_PRI, L->whiten); return; }
    float f = m_clamp(L->past - (float)i, 0, 1);
    if (f >= 1.0f) *col = C_PRI;
    else if (f <= 0.0f) *col = C_FUT;
    else *col = ui_mix(C_FUT, C_PRI, (float)(int)(f * 4.0f) / 4.0f);
}

static void day_dot(int i, const void *vc, float *x, float *y, float *d, uint16_t *col)
{
    const life_ctx *L = (const life_ctx *)vc;
    float a = (float)i * (6.2831853f / 24.0f);
    *x = UI_CX + fk_sin(a) * R_DAY + L->dx;
    *y = UI_CY - fk_cos(a) * R_DAY + L->dy;
    if (L->k < 0.5f) { *d = 0; *col = 0; return; }
    float f = m_clamp(L->hpast - (float)i, 0, 1);
    *d = i % 6 == 0 ? 9.0f : 6.0f;
    if (i == L->hour && f > 0.0f) { *col = C_WHITE; *d = 11.0f; }
    else if (f >= 1.0f) *col = C_SEC;
    else *col = C_LINE2;
    if (L->k < 1.0f) *col = ui_mix(C_BG, *col, (L->k - 0.25f) / 0.75f);
}

/* the small time in tabular cells (every digit centred in the width of "0", so the
 * layout never shifts); changed digits roll */
static void draw_time(gfx_fb *fb, const face_ctx *c, uint16_t col, float dx, float dy, float p)
{
    int dw = ui_text_width(UI_F_TITLE, "0", 0), kw = ui_text_width(UI_F_TITLE, ":", 0) + 4;
    int n = (int)strlen(S.tto), w = 0;
    for (int i = 0; i < n; i++) w += S.tto[i] == ':' ? kw : dw;
    int x = UI_CX - w / 2 + (int)dx, y = Y_TIME + (int)dy;
    int cap = UI_F_TITLE->ascent * 7 / 10 + 2;
    for (int i = 0; i < n; i++) {
        int cell = S.tto[i] == ':' ? kw : dw;
        char a[2] = { S.tfrom[i], 0 }, b[2] = { S.tto[i], 0 };
        int na = (cell - ui_text_width(UI_F_TITLE, a, 0)) / 2, nb = (cell - ui_text_width(UI_F_TITLE, b, 0)) / 2;
        if (p >= 1.0f || S.tfrom[i] == S.tto[i]) ui_text(fb, UI_F_TITLE, x + nb, y, b, col, UI_LEFT, 0);
        else fk_roll2(fb, UI_F_TITLE, 0, x, y, cell, cap, S.tfrom[i], S.tto[i], na, nb, p, col);
        x += cell;
    }
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    int m = c->dt.minute;
    float dx, dy;
    fk_wake_off(&S.w, c, &dx, &dy);
    float k = fk_wake_k(&S.w, c);
    if (c->aod || k < 1.0f) {
        life_ctx L = { (float)m, m, dx, dy, k, c->dt.hour, (float)c->dt.hour + 1.0f, -1, 1.0f, 0 };
        fk_dots(fb, 24, 4, day_dot, &L);
        fk_dots(fb, COLS * ROWS, COLS, grid_dot, &L);
        float nx = GX0 + (float)(m % COLS) * PITCH + dx, ny = GY0 + (float)(m / COLS) * PITCH + dy;
        if (k >= 0.75f) dot_disc(fb, nx, ny, DOTD * k, C_RED);
        else {
            dot_disc(fb, nx, ny, 14.0f + 9.0f * k, ui_mix(C_SEC, C_RED, k));
            dot_disc(fb, nx, ny, 10.0f * (1.0f - k), C_BG);
        }
        if (k > 0.0f) ui_label(fb, UI_CX + (int)dx, Y_DATE + (int)dy, S.date.to, ui_dim(C_SEC, k), UI_CENTER);
        draw_time(fb, c, ui_mix(C_SEC, C_PRI, k), dx, dy, 1.0f);
        /* battery: a critical one shows in AOD too and brightens; otherwise it fades up */
        face_ctx ca = *c; ca.aod = 1;
        uint16_t sc = c->aod ? C_DIS : fk_status_shown(&ca) ? ui_mix(C_DIS, C_SEC, k) : ui_dim(C_SEC, k);
        fk_status(fb, c, UI_CX + (int)dx, Y_STAT + (int)dy, sc, UI_CENTER);
        fk_unread(fb, c, ui_mix(C_SEC, C_PRI, k), C_BG, dx, dy + U_DY);
        return;
    }
    float e = fk_build(&S.w, c);
    float past = (float)m, hpast = (float)c->dt.hour + 1.0f;
    float since = c->t - S.change_t;
    float grow = 1.0f, whiten = 1.0f;
    int draining = 0;
    if (e < 1.0f) {
        float q = fk_ease(m_clamp(e / 0.75f, 0, 1));
        past = (float)m * q;
        hpast = ((float)c->dt.hour + 1.0f) * q;
        grow = fk_stagger(e, 0.7f, 0.3f);
    } else if (m == 0 && since < DRAIN_S + 0.2f) {      /* new hour: drain, then start */
        float fr = (float)(int)(since * 30.0f + 0.5f);
        past = 60.0f - 6.0f * fr;
        if (past > 0.0f) { draining = 1; grow = 0.0f; }
        else { past = 0.0f; grow = 0.4f + 0.6f * fk_ease((since - DRAIN_S) / 0.2f); }
    } else if (since < 0.2f) {
        grow = 0.4f + 0.6f * fk_ease(since / 0.2f);
        whiten = (float)(int)(since * 30.0f + 1.0f) / 3.0f;
    }
    life_ctx L = { past, m, 0, 0, 1.0f, c->dt.hour, hpast, m > 0 ? m - 1 : -1, whiten, !draining };
    fk_dots(fb, 24, 4, day_dot, &L);
    fk_dots(fb, COLS * ROWS, 5, grid_dot, &L);

    fk_xfade_label(fb, &S.date, c->t, UI_CX, Y_DATE, C_SEC, UI_CENTER);
    draw_time(fb, c, C_PRI, 0, 0, e < 1.0f ? 1.0f : m_clamp(since / 0.3f, 0, 1));
    fk_status(fb, c, UI_CX, Y_STAT, C_SEC, UI_CENTER);

    /* the present minute: red, breathing on a 2 s cycle: a symmetric triangle through a
     * smoothstep, 8 size steps, so the rise and the fall hold each step alike */
    if (!draining) {
        float ph = ((float)(c->dt.second % 2) * 1000.0f + (float)c->dt.millis) / 2000.0f;
        float tri = ph < 0.5f ? ph * 2.0f : 2.0f - ph * 2.0f;
        float br = tri * tri * (3.0f - 2.0f * tri);
        br = (float)(int)(br * 7.0f + 0.5f) / 7.0f;
        float d = DOTD * (0.78f + 0.22f * br) * grow;
        float nx = GX0 + (float)(m % COLS) * PITCH, ny = GY0 + (float)(m / COLS) * PITCH;
        if (grow < 1.0f) dot_disc(fb, nx, ny, DOTD, C_FUT);
        if (d > 0.5f) dot_disc(fb, nx, ny, d, C_RED);
    }
    fk_unread(fb, c, C_PRI, C_BG, 0, U_DY);          /* y 64: well clear of the day ring's top dot */
}

const nos_face face_life = { "LIFE", enter, tick, draw, 0 };
