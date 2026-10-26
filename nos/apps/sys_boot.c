/* sys_boot.c — the NOS boot animation (~1.4 s), then a cut to home.
 *
 * Black; then every dot of the wordmark bursts out of the centre, scatters onto a loose
 * ring, and is pulled into its cell of a 5x7 dot-matrix "NOS". The red period clicks in
 * last (the screen's one red element), the mark holds, then every dot shrinks away and
 * the screen cuts to home, where the face's own dot build-in is the reveal (no
 * crossfade / double exposure). Any touch or the button skips.
 *
 * Motion (docs/MOTION30.html): everything runs on whole frames. A dot starts on one of 4
 * frames and turns for its cell on one of 4 later frames (R9: whole-frame staggers, at
 * most 8 starts). Burst and gather both ride EASE_STEADY (no frame over 1.6x the
 * average, frame 1 already moving, a ~1 % last step: R3), and a dot that steps more than
 * 16 px in a frame leaves one ghost at 45 % ink where it was (R7: the eye does not track
 * 57 separate dots; ui_smear_disc). The period clicks in on the 3-frame snap table.
 *
 * Cost: positions are computed once per frame in tick (not per stripe in draw); ~50 dot
 * stamps (+ a ghost each on fast frames) per frame; the screen repaints in full while the
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
/* timeline in whole frames (30 fps) */
#define F_START 3          /* black before anything moves; dots start on frames 3-6 */
#define F_BURST 8          /* centre -> scatter ring (EASE_STEADY); the turn comes 6-9 in */
#define F_GATH  12         /* scatter ring -> cell (EASE_STEADY) */
#define F_RED   29         /* the period clicks in (every dot has landed by frame 28) */
#define F_OUT   37         /* the mark lets go: every dot shrinks away */
#define F_POP   43         /* cut to home; the face's own dot build-in is the reveal */
#define GHOST_STEP 16.0f   /* px a frame above which a dot leaves a ghost */

typedef struct {
    float tx, ty, sx, sy;  /* cell, scatter point */
    short f0, f1;          /* burst and gather start frames */
    float x, y, d;         /* this frame (tick) */
    int on;
    ui_trail tr;           /* where it was: the smear's ghost */
} particle;
static particle *s_p;      /* PSRAM: MAXDOT particles */
static int   s_n;
static float s_t;
static int   s_popped;
static float s_red_x, s_red_y;

static int boot_frame(void) { return (int)floorf(s_t * M_FPS + 0.5f); }

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
                p->f0 = (short)(F_START + (int)(m_hash01((unsigned)s_n, 3u) * 4.0f));   /* burst start */
                p->f1 = (short)(p->f0 + 6 + (int)(m_hash01((unsigned)s_n, 5u) * 4.0f)); /* gather start */
                p->on = 0;
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
    sys_home_build_in();                   /* the face builds in from black, its own reveal */
    nos_pop_with(TX_NONE);
}

/* Particle p at frame k: position (quarter pixels, which keep the dot masks' phases and
 * damage stable) and diameter. */
static void particle_step(particle *p, int k)
{
    if (k < p->f0) { p->on = 0; return; }
    float x, y, d;
    if (k < p->f1) {                       /* burst: out of the centre, decelerating evenly */
        float e = bezier_ease(&EASE_STEADY, m_clamp((float)(k - p->f0) / F_BURST, 0, 1));
        x = m_lerp(UI_CX, p->sx, e);
        y = m_lerp(UI_CY, p->sy, e);
        d = m_lerp(2.0f, DOTD * 0.55f, e);
    } else {
        /* gather: from where the burst was on its last frame into the cell (precise,
         * mechanical), already moving on its first frame; the last sub-pixel is snapped
         * (R5), so the final moving frame still moves >= 1 px */
        float eb = bezier_ease(&EASE_STEADY, m_clamp((float)(p->f1 - 1 - p->f0) / F_BURST, 0, 1));
        float bx = m_lerp(UI_CX, p->sx, eb), by = m_lerp(UI_CY, p->sy, eb);
        float e = bezier_ease(&EASE_STEADY, m_clamp((float)(k - p->f1 + 1) / F_GATH, 0, 1));
        float dist = sqrtf((p->tx - bx) * (p->tx - bx) + (p->ty - by) * (p->ty - by));
        if (dist * (1.0f - e) < 1.0f) e = 1.0f;
        x = m_lerp(bx, p->tx, e);
        y = m_lerp(by, p->ty, e);
        d = m_lerp(DOTD * 0.55f, DOTD, e);
    }
    p->x = floorf(x * 4.0f + 0.5f) * 0.25f;
    p->y = floorf(y * 4.0f + 0.5f) * 0.25f;
    p->d = d;
    if (!p->on) ui_trail_reset(&p->tr, p->x, p->y);   /* appearing is not a move */
    else ui_trail_push(&p->tr, p->x, p->y);
    p->on = 1;
}

/* motion audit (sim only): the path length of a few particles, burst and gather apart.
 * Compiled only into the sim (its state would cost the watch's DRAM for nothing). */
#ifdef GFX_COST_MODEL
static float s_pr_x[3], s_pr_y[3], s_pr_len[3];
static int s_pr_ph[3];
static void boot_probe(int k)
{
    static const char *const k_b[3] = { "boot.burst.0", "boot.burst.20", "boot.burst.40" };
    static const char *const k_g[3] = { "boot.gather.0", "boot.gather.20", "boot.gather.40" };
    for (int j = 0; j < 3; j++) {
        int i = j * 20;
        if (i >= s_n || !s_p[i].on) {
            s_pr_len[j] = 0; s_pr_x[j] = UI_CX; s_pr_y[j] = UI_CY; s_pr_ph[j] = 0;
            continue;
        }
        float x = s_p[i].x, y = s_p[i].y, dx = x - s_pr_x[j], dy = y - s_pr_y[j];
        int ph = k < s_p[i].f1 ? 1 : 2;
        const char *nm = ph == 1 ? k_b[j] : k_g[j];
        if (ph != s_pr_ph[j]) nos_motion_probe(nm, MC_TRANSITION, s_pr_len[j]);   /* origin of the phase */
        s_pr_ph[j] = ph;
        s_pr_len[j] += sqrtf(dx * dx + dy * dy);
        s_pr_x[j] = x; s_pr_y[j] = y;
        nos_motion_probe(nm, MC_TRANSITION, s_pr_len[j]);
    }
}
#else
#define boot_probe(k) ((void)(k))
#endif

static void boot_tick(float dt)
{
    s_t += dt;
    int k = boot_frame();
    if (!s_popped && k >= F_RED && (int)floorf((s_t - dt) * M_FPS + 0.5f) < F_RED) nos_feedback(FB_TICK);
    if (k >= F_POP) { finish(); return; }
    for (int i = 0; i < s_n; i++) particle_step(&s_p[i], k);
    boot_probe(k);
}

static void boot_draw(gfx_fb *fb)
{
    int k = boot_frame();
    /* exit: dots shrink to nothing (accelerating), so the glass is black before the face */
    float out = 1.0f - bezier_ease(&EASE_ACCEL, m_clamp((float)(k - F_OUT) / (F_POP - F_OUT), 0, 1));
    out = floorf(out * 8.0f + 0.5f) / 8.0f;
    if (out <= 0) return;
    for (int i = 0; i < s_n; i++) {
        const particle *p = &s_p[i];
        if (!p->on) continue;
        /* a fast frame: one ghost where the dot was (the smear first, then the dot) */
        if (out >= 1.0f) ui_smear_disc(fb, &p->tr, p->d * 0.5f, C_WHITE, GHOST_STEP, 1);
        dot_disc(fb, p->x, p->y, p->d * out, C_WHITE);
    }
    if (k >= F_RED) {                      /* the period: clicks in on the snap table */
        float s = mf_at(&MF_SNAP3, k - F_RED + 1);
        dot_disc(fb, s_red_x, s_red_y, DOTD * s * out, C_RED);
    }
}

static int boot_input(const nos_input *in)
{
    if (in->type == IN_DOWN || in->type == IN_BTN || in->type == IN_BTN_LONG) { finish(); return 1; }
    return 1;                              /* nothing else leaks to the screen below */
}

const nos_screen scr_boot = { "BOOT", boot_enter, 0, boot_tick, boot_draw, boot_input, SCR_NOBACK };
