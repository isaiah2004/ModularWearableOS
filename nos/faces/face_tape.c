/* face_tape.c — TAPE: the recorder.
 *
 * Two dot-grid tape reels in a cassette window, and a dot-matrix timecode HH:MM:SS.
 * The reels hold the day: the left (supply) pack shrinks and the right (take-up) pack
 * grows from midnight to midnight. Each hub is a ring of 12 dots with one gap that
 * steps 30 degrees every second with a short mechanical ease, like a stepper-driven
 * deck. The packs are lit dot by dot
 * (about one dot per 8 minutes). The red dot by the date is the recording lamp, on the
 * shared blink envelope; its unlit lamp stays as a dark dot so the pair stays centred.
 * Wake from AOD: rims and hubs grow, the packs fade up, HH:MM slides left as the
 * seconds grow in.
 *
 * Cost: still between ticks; each second the two hubs (~70 px boxes) move for 6
 * frames and the timecode's seconds morph for 4 frames. The packs change every ~8 min. Full redraw: ~300 small dots. */
#include "faces/face_kit.h"
#include "nos.h"
#include <stdio.h>
#include <string.h>

#define RX_L   (UI_CX - 98.0f)
#define RX_R   (UI_CX + 98.0f)
#define RY     204.0f
#define R_RIM  70.0f
#define R_HUB  15.0f
#define TC_P   6.0f                      /* timecode pitch */
#define TC_Y   322.0f
#define STEP_S 0.22f                     /* hub step duration */

static struct { fk_morph tc; fk_wake w; } S;

static void enter(void) { S.tc.init = 0; }

static uint16_t gdimq(uint16_t c, float k) { return k >= 1.0f ? c : ui_dim(c, (float)(int)(k * 4.0f) / 4.0f); }

static void tick(const face_ctx *c, float dt)
{
    fk_aod_update(c);
    fk_wake_tick(&S.w, c);
    fk_unread_tick(c);
    char b[12], hh[3], mm[3];
    fk_hh(hh, c->dt.hour); fk_mm(mm, c->dt.minute);
    snprintf(b, sizeof b, "%s:%s:%02d", hh, mm, c->dt.second);
    if (fk_build(&S.w, c) < 1.0f) S.tc.init = 0;
    fk_morph_tick(&S.tc, b, c->t);
}

/* reel geometry: pack rings of dots at fixed radii, lit dot by dot from the hub out */
static const float k_ring_r[] = { 28.0f, 38.0f, 48.0f, 58.0f };
#define NRINGS 4

typedef struct { float cx, cy, rot, dx, dy; int lit; float k, build; } reel_ctx;

static int ring_n(float r) { return (int)(6.2831853f * r / 11.0f + 0.5f); }
static int pack_dots(void)
{
    int n = 0;
    for (int k = 0; k < NRINGS; k++) n += ring_n(k_ring_r[k]);
    return n;
}

/* rim + pack rings as one dot list (chunked); pack index j runs hub -> outside */
static void pack_dot(int i, const void *vc, float *x, float *y, float *d, uint16_t *col)
{
    const reel_ctx *R = (const reel_ctx *)vc;
    float r = R_RIM;
    int n = ring_n(R_RIM), k = -1, j = 0;
    if (i >= n) {
        i -= n;
        for (k = 0; k < NRINGS; k++) {
            int m = ring_n(k_ring_r[k]);
            if (i < m) { n = m; r = k_ring_r[k]; break; }
            i -= m; j += m;
        }
        j += i;
    }
    float a = ((float)i + (k & 1 ? 0.5f : 0.0f)) * 6.2831853f / (float)n;
    *x = R->cx + R->dx + fk_sin(a) * r;
    *y = R->cy + R->dy - fk_cos(a) * r;
    float kk = R->k;
    if (kk <= 0.0f) { *d = k < 0 ? 3.0f : 0.0f; *col = C_LINE2; return; }   /* AOD: rims only */
    if (kk < 1.0f) {                                                       /* wake: pack fades up */
        if (k < 0) { *d = 3.0f + kk; *col = C_LINE2; return; }
        if (kk < 0.5f) { *d = 0; *col = 0; return; }
        int full = j < R->lit;
        *d = full ? 7.0f : 3.0f;
        *col = ui_mix(C_BG, full ? C_SEC : C_LINE2, (kk - 0.25f) / 0.75f);
        return;
    }
    float b = m_clamp(R->build * 1.6f - (r - 26.0f) / 55.0f, 0, 1);   /* builds outward */
    b = (float)(int)(b * 4.0f) / 4.0f;
    if (k < 0) { *d = 4.0f * b; *col = C_LINE2; return; }
    int full = j < R->lit;
    *d = (full ? 7.0f : 3.0f) * b;
    *col = full ? C_SEC : C_LINE2;
}

/* hub: a ring of 12 dots with one gap, so its stepping turn reads at a glance */
#define HUB_N 12
static void hub_dot(int i, const void *vc, float *x, float *y, float *d, uint16_t *col)
{
    const reel_ctx *R = (const reel_ctx *)vc;
    float a = (float)i * 0.5235988f + R->rot;
    *x = R->cx + R->dx + fk_sin(a) * R_HUB;
    *y = R->cy + R->dy - fk_cos(a) * R_HUB;
    if (i == 0) { *d = 0; *col = 0; return; }
    *d = (3.5f + 2.5f * R->k) * (R->k >= 1.0f ? R->build : 1.0f);
    *col = ui_mix(C_DIS, C_WHITE, R->k);
}

static void reel(gfx_fb *fb, reel_ctx *R)
{
    fk_dots(fb, ring_n(R_RIM) + pack_dots(), 12, pack_dot, R);
    fk_dots(fb, HUB_N, HUB_N, hub_dot, R);
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    float dx, dy;
    fk_wake_off(&S.w, c, &dx, &dy);
    float k = fk_wake_k(&S.w, c);
    float e = c->aod ? 1.0f : fk_build(&S.w, c);
    float day = ((float)c->dt.hour * 60.0f + (float)c->dt.minute) / 1440.0f;
    /* the packs: supply holds the rest of the day, take-up what has passed; one dot of
     * ~180 per reel is ~8 minutes */
    int np = pack_dots();
    int lit_l = (int)((1.0f - day) * (float)np + 0.5f), lit_r = np - lit_l;
    /* hub rotation: 30 degrees per second, stepped with a short ease (also during the
     * build-in, so the hubs never jump when it ends) */
    float rot = 0;
    if (!c->aod) {
        float ph = (float)c->dt.millis * 0.001f;
        float q = ph < STEP_S ? fk_mech(ph / STEP_S) : 1.0f;
        q = q > 0.93f ? 1.0f : (float)(int)(q * 32.0f) / 32.0f;
        rot = ((float)(c->dt.second % 12) + q) * 0.5235988f;  /* one turn per 12 s */
    }
    float build = e >= 1.0f ? 1.0f : fk_ease(e / 0.7f);
    reel_ctx L = { RX_L, RY, rot, dx, dy, lit_l, k, build };
    reel_ctx R = { RX_R, RY, rot, dx, dy, lit_r, k, build };
    reel(fb, &L);
    reel(fb, &R);
    /* the cassette window around the reels */
    float wa = k < 1.0f ? (k >= 0.5f ? k : 0.0f) : build;
    if (wa > 0.0f) ui_rrect_line(fb, 46 + dx, RY - 86 + dy, GFX_W - 92, 172, 40, 2, gdimq(C_LINE2, wa));

    /* timecode HH:MM:SS; AOD shows HH:MM centred, the wake slides it left as the
     * seconds grow in */
    dot_style st = { DOT_FONT_TALL, TC_P, TC_P * 0.8f, C_WHITE, 0, 1, 1 };
    if (c->aod || k < 1.0f) {
        char hm[8]; memcpy(hm, S.tc.to, 5); hm[5] = 0;
        int w5 = dot_text_width(&st, hm), w8 = dot_text_width(&st, S.tc.to);
        float x_aod = UI_CX - (float)w5 * 0.5f, x_awk = UI_CX - (float)w8 * 0.5f;
        st.color = ui_mix(C_SEC, C_WHITE, k); st.dot = TC_P * (0.6f + 0.2f * k);
        float xh = x_aod + (x_awk - x_aod) * k + dx;
        dot_text(fb, &st, xh, TC_Y + dy, hm);
        if (k > 0.0f) {                                  /* ":SS" rides along, growing */
            st.dot = TC_P * 0.8f * k;
            dot_text(fb, &st, xh + (float)w5 + TC_P, TC_Y + dy, S.tc.to + 5);
        }
    } else if (e < 1.0f)
        dot_text_anim(fb, &st, UI_CX, TC_Y, S.tc.to, 1, m_clamp((e - 0.25f) / 0.6f, 0, 1), DOT_REVEAL_SCAN, 0);
    else
        dot_text_morph(fb, &st, UI_CX, TC_Y, S.tc.from, S.tc.to, 1, fk_morph_p(&S.tc, c->t, 0.15f));

    /* date with the recording lamp: the lamp is always there (off = #1A1A1A), so the
     * pair stays centred; it lights with the shared blink envelope */
    char b[16];
    snprintf(b, sizeof b, "%s %d", ui_weekday3(c->dt.weekday), c->dt.day);
    int w = ui_text_width(UI_F_LABEL, b, 2);
    int lx = UI_CX - (w + 16) / 2;                       /* lamp 9 px + 7 px gap, then text */
    ui_label(fb, lx + 16 + (int)dx, 96 + (int)dy, b, c->aod ? C_DIS : ui_mix(C_DIS, C_SEC, k), UI_LEFT);
    if (!c->aod && k >= 1.0f) {
        float g = fk_stagger(e, 0.8f, 0.2f);
        dot_disc(fb, (float)lx + 4.5f, 90.0f, 9.0f * g, C_SURF2);
        float d = 9.0f * fk_blink(c->dt.millis) * g;
        if (d > 0.5f) dot_disc(fb, (float)lx + 4.5f, 90.0f, d, C_RED);
        fk_status(fb, c, UI_CX, 406, C_SEC, UI_CENTER);
    }
    fk_unread(fb, c, c->aod ? C_SEC : C_PRI, C_BG, dx, dy);
}

const nos_face face_tape = { "TAPE", enter, tick, draw, 0 };
