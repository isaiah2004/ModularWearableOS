/* sys_boot.c — the NOS boot animation (~1.6 s), then a dissolve into home.
 *
 * Black; then every dot of the wordmark bursts out of the centre, scatters onto a loose
 * ring, and is pulled into its cell of a 5x7 dot-matrix "NOS". The red period clicks in
 * last (the screen's one red element), the mark holds, and the boot screen dissolves
 * away to reveal the face, which builds in underneath. Any touch or the button skips.
 *
 * Cost: ~50 dot stamps (mask blits) per frame; the screen repaints in full while the
 * dots fly (the damage boxes cover it) — a few ms on the device model. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include <math.h>

void sys_home_build_in(void);              /* sys_home.c: restart the face build-in */

/* 5x7 glyphs of the wordmark, '#' = lit (authored here so every dot is a particle) */
static const char *const k_glyph[3] = {
    "#...#" "##..#" "#.#.#" "#.#.#" "#..##" "#...#" "#...#",   /* N */
    ".###." "#...#" "#...#" "#...#" "#...#" "#...#" ".###.",   /* O */
    ".####" "#...." "#...." ".###." "....#" "....#" "####.",   /* S */
};

#define PITCH   16.0f
#define DOTD    (PITCH * 0.8f)
#define MAXDOT  64
#define T_START 0.10f      /* black before anything moves */
#define T_BURST 0.32f      /* centre -> scatter ring */
#define T_GATH  0.42f      /* scatter ring -> cell */
#define T_RED   0.98f      /* the period clicks in */
#define T_POP   1.30f      /* start the 0.3 s dissolve: ~1.6 s total */

typedef struct { float tx, ty, sx, sy, t0, t1; } particle;
static particle *s_p;      /* PSRAM: MAXDOT particles */
static int   s_n;
static float s_t;
static int   s_popped;
static float s_red_x, s_red_y;

static void layout(void)
{
    if (!s_p) s_p = (particle *)hal_big_alloc(sizeof(particle) * MAXDOT);
    if (!s_p) { s_n = 0; return; }
    /* 3 glyphs x 5 cells + 2 gap cells, plus a gap cell and the period */
    const int cols = 3 * 5 + 2 + 2;
    float w = (cols - 1) * PITCH, h = 6 * PITCH;
    float x0 = UI_CX - w * 0.5f, y0 = UI_CY - h * 0.5f;
    s_n = 0;
    for (int g = 0; g < 3; g++)
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++) {
                if (k_glyph[g][r * 5 + c] != '#' || s_n >= MAXDOT) continue;
                particle *p = &s_p[s_n];
                p->tx = x0 + (g * 6 + c) * PITCH;
                p->ty = y0 + r * PITCH;
                /* scatter point: out along the dot's own bearing, onto a loose ring */
                float dx = p->tx - UI_CX, dy = p->ty - UI_CY;
                float a = atan2f(dy, dx) + (m_hash01((unsigned)s_n, 7u) - 0.5f) * 0.9f;
                float rr = 112.0f + m_hash01((unsigned)s_n, 11u) * 64.0f;
                p->sx = UI_CX + cosf(a) * rr;
                p->sy = UI_CY + sinf(a) * rr;
                p->t0 = T_START + m_hash01((unsigned)s_n, 3u) * 0.10f;          /* burst start */
                p->t1 = p->t0 + 0.22f + m_hash01((unsigned)s_n, 5u) * 0.12f; /* gather start */
                s_n++;
            }
    s_red_x = x0 + 18 * PITCH;
    s_red_y = y0 + 6 * PITCH;
}

static void boot_enter(void)
{
    s_t = 0;
    s_popped = 0;
    layout();
    hal_sfx(SFX_BOOT);
}

static void finish(void)
{
    if (s_popped) return;
    s_popped = 1;
    sys_home_build_in();                   /* the face builds in under the dissolve */
    nos_pop_with(TX_DISSOLVE);
}

static void boot_tick(float dt)
{
    s_t += dt;
    if (!s_popped && s_t >= T_RED && s_t - dt < T_RED) nos_feedback(FB_TICK);
    if (s_t >= T_POP) finish();
}

/* Position and diameter of particle i at time t. */
static int particle_at(const particle *p, float t, float *x, float *y, float *d)
{
    if (t < p->t0) return 0;
    if (t < p->t1) {                       /* burst: fast out of the centre, decelerating */
        float k = bezier_ease(&EASE_DECEL, m_clamp((t - p->t0) / T_BURST, 0, 1));
        *x = m_lerp(UI_CX, p->sx, k);
        *y = m_lerp(UI_CY, p->sy, k);
        *d = m_lerp(2.0f, DOTD * 0.55f, k);
        return 1;
    }
    /* gather: from wherever the burst had got to, into the cell (precise, mechanical) */
    float kb = bezier_ease(&EASE_DECEL, m_clamp((p->t1 - p->t0) / T_BURST, 0, 1));
    float bx = m_lerp(UI_CX, p->sx, kb), by = m_lerp(UI_CY, p->sy, kb);
    float k = bezier_ease(&EASE_STANDARD, m_clamp((t - p->t1) / T_GATH, 0, 1));
    *x = m_lerp(bx, p->tx, k);
    *y = m_lerp(by, p->ty, k);
    *d = m_lerp(DOTD * 0.55f, DOTD, k);
    return 1;
}

static void boot_draw(gfx_fb *fb)
{
    float t = s_t;
    for (int i = 0; i < s_n; i++) {
        float x, y, d;
        if (!particle_at(&s_p[i], t, &x, &y, &d)) continue;
        /* quarter-pixel positions keep the dot masks' phases (and damage) stable */
        x = floorf(x * 4.0f + 0.5f) * 0.25f;
        y = floorf(y * 4.0f + 0.5f) * 0.25f;
        dot_disc(fb, x, y, d, C_WHITE);
    }
    if (t >= T_RED) {                      /* the period: grows to size in 3 frames */
        float k = m_clamp((t - T_RED) / 0.10f, 0, 1);
        k = floorf(k * 3.0f + 0.5f) / 3.0f;
        if (k > 0) dot_disc(fb, s_red_x, s_red_y, DOTD * k, C_RED);
    }
}

static int boot_input(const nos_input *in)
{
    if (in->type == IN_DOWN || in->type == IN_BTN || in->type == IN_BTN_LONG) { finish(); return 1; }
    return 1;                              /* nothing else leaks to the screen below */
}

const nos_screen scr_boot = { "BOOT", boot_enter, 0, boot_tick, boot_draw, boot_input, SCR_NOBACK };
