/* face_life.c — LIFE: life in dots.
 *
 * The hour as a 10 x 6 grid of dots: one row per ten minutes, one column per minute
 * (so 10:42 reads as four full rows and two dots), past minutes white, future ones
 * #2A2A2A, the present minute the red dot, breathing gently through its size. Around
 * the bezel, the day as 24 hour dots (past grey, this hour white, future dark). Date
 * above, a small grotesk time below.
 *
 * Motion: build-in = the past minutes light one by one in reading order (and the day
 * ring with them), then the red dot grows in. Minute change = the new red dot grows
 * from the future colour; at the top of the hour the grid drains in reverse first.
 * Cost: per frame only the red dot's box (its size is quantised to 8 steps on a 2 s
 * breath, so it changes ~every 4th frame); everything else changes once a minute. */
#include "faces/face_kit.h"
#include "nos.h"
#include <stdio.h>

#define COLS   10
#define ROWS   6
#define PITCH  32.0f
#define DOTD   23.0f
#define GX0    (UI_CX - PITCH * (COLS - 1) * 0.5f)
#define GY0    (UI_CY - PITCH * (ROWS - 1) * 0.5f + 4.0f)
#define R_DAY  212.0f
#define C_FUT  C_SURF3

static struct { float change_t; int last_m; } S = { -100.0f, -1 };

static void enter(void) { S.last_m = -1; }

static void tick(const face_ctx *c, float dt)
{
    fk_aod_update(c);
    if (c->dt.minute != S.last_m) {
        if (S.last_m >= 0) S.change_t = c->t;
        S.last_m = c->dt.minute;
    }
}

typedef struct { float past; int now; float dx, dy; int aod; int hour; float hpast; } life_ctx;

static void grid_dot(int i, const void *vc, float *x, float *y, float *d, uint16_t *col)
{
    const life_ctx *L = (const life_ctx *)vc;
    *x = GX0 + (float)(i % COLS) * PITCH + L->dx;
    *y = GY0 + (float)(i / COLS) * PITCH + L->dy;
    if (L->aod) {
        *d = i < L->now ? 7.0f : i > L->now ? 3.0f : 0.0f;
        *col = i < L->now ? C_DIS : C_LINE2;
        return;
    }
    if (i == L->now) { *d = 0; *col = 0; return; }        /* the red dot draws itself */
    float f = m_clamp(L->past - (float)i, 0, 1);
    if (f >= 1.0f) { *d = DOTD; *col = C_PRI; }
    else if (f <= 0.0f) { *d = DOTD; *col = C_FUT; }
    else { *d = DOTD; *col = ui_mix(C_FUT, C_PRI, (float)(int)(f * 4.0f) / 4.0f); }
}

static void day_dot(int i, const void *vc, float *x, float *y, float *d, uint16_t *col)
{
    const life_ctx *L = (const life_ctx *)vc;
    float a = (float)i * (6.2831853f / 24.0f);
    *x = UI_CX + fk_sin(a) * R_DAY;
    *y = UI_CY - fk_cos(a) * R_DAY;
    float f = m_clamp(L->hpast - (float)i, 0, 1);
    *d = i % 6 == 0 ? 9.0f : 6.0f;
    if (i == L->hour && f > 0.0f) { *col = C_WHITE; *d = 11.0f; }
    else if (f >= 1.0f) *col = C_SEC;
    else *col = C_LINE2;
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    char b[24];
    int m = c->dt.minute;
    if (c->aod) {
        float dx = (float)fk_aod_dx, dy = (float)fk_aod_dy;
        life_ctx L = { 0, m, dx, dy, 1, c->dt.hour, 0 };
        fk_dots(fb, COLS * ROWS, COLS, grid_dot, &L);
        float nx = GX0 + (float)(m % COLS) * PITCH + dx, ny = GY0 + (float)(m / COLS) * PITCH + dy;
        dot_disc(fb, nx, ny, 14.0f, C_SEC);
        dot_disc(fb, nx, ny, 10.0f, C_BG);
        ui_fmt_time(b, sizeof b, c->dt.hour, m);
        ui_text(fb, UI_F_TITLE, UI_CX + (int)dx, 380 + (int)dy, b, C_SEC, UI_CENTER, 0);
        return;
    }
    float e = c->enter;
    float past = (float)m, hpast = (float)c->dt.hour + 1.0f;
    float since = c->t - S.change_t;
    float grow = 1.0f;                                  /* the red dot's build */
    if (e < 1.0f) {
        float k = m_clamp(e / 0.75f, 0, 1);
        past = (float)m * k;
        hpast = ((float)c->dt.hour + 1.0f) * k;
        grow = fk_stagger(e, 0.7f, 0.3f);
    } else if (m == 0 && since < 0.6f) {                /* new hour: drain, then start */
        past = 60.0f * (1.0f - fk_mech(since / 0.45f));
        grow = fk_stagger(since, 0.4f, 0.2f);
    } else if (since < 0.3f) {
        grow = fk_mech(since / 0.25f);
    }
    life_ctx L = { past, m, 0, 0, 0, c->dt.hour, hpast };
    fk_dots(fb, 24, 4, day_dot, &L);
    fk_dots(fb, COLS * ROWS, 5, grid_dot, &L);

    snprintf(b, sizeof b, "%s %d", ui_weekday3(c->dt.weekday), c->dt.day);
    ui_label(fb, UI_CX, 106, b, C_SEC, UI_CENTER);
    ui_fmt_time(b, sizeof b, c->dt.hour, m);
    ui_text(fb, UI_F_TITLE, UI_CX, 380, b, C_PRI, UI_CENTER, 0);

    /* the present minute: red, breathing on a 2 s cycle, 8 size steps */
    float ph = ((float)(c->dt.second % 2) * 1000.0f + (float)c->dt.millis) / 2000.0f;
    float br = 0.5f - 0.5f * fk_cos(ph * 6.2831853f);   /* 0..1..0 */
    br = (float)(int)(br * 7.0f + 0.5f) / 7.0f;
    float d = DOTD * (0.74f + 0.26f * br) * grow;
    float nx = GX0 + (float)(m % COLS) * PITCH, ny = GY0 + (float)(m / COLS) * PITCH;
    if (grow < 1.0f) dot_disc(fb, nx, ny, DOTD, C_FUT);
    if (d > 0.5f) dot_disc(fb, nx, ny, d, C_RED);
}

const nos_face face_life = { "LIFE", enter, tick, draw, 0 };
