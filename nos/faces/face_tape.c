/* face_tape.c — TAPE: the recorder.
 *
 * Two dot-grid tape reels, a dotted tape run across the head, and a dot-matrix
 * timecode HH:MM:SS. The reels hold the day: the left (supply) pack shrinks and the
 * right (take-up) pack grows from midnight to midnight. Each hub is a ring of dots
 * with three gaps that steps 30 degrees every second with a short mechanical ease,
 * like a stepper-driven deck. The red dot by the date is the recording lamp.
 *
 * Cost: still between ticks; each second the two hubs (~70 px boxes) move for 6
 * frames and the timecode's seconds morph for 4 frames. The packs change a few times
 * an hour. Full redraw: ~300 small dots. */
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

static struct { fk_morph tc; } S;

static void enter(void) { S.tc.init = 0; }

static uint16_t gdimq(uint16_t c, float k) { return k >= 1.0f ? c : ui_dim(c, (float)(int)(k * 4.0f) / 4.0f); }

static void tick(const face_ctx *c, float dt)
{
    fk_aod_update(c);
    char b[12], hh[3], mm[3];
    fk_hh(hh, c->dt.hour); fk_mm(mm, c->dt.minute);
    snprintf(b, sizeof b, "%s:%s:%02d", hh, mm, c->dt.second);
    if (c->enter < 1.0f) S.tc.init = 0;
    fk_morph_tick(&S.tc, b, c->t);
}

/* reel geometry: rings of dots at fixed radii; the pack covers rings up to `pack` */
static const float k_ring_r[] = { 28.0f, 38.0f, 48.0f, 58.0f };
#define NRINGS 4

typedef struct { float cx, cy, pack, rot, dx, dy; int aod, hub_only; float build; } reel_ctx;

static int ring_n(float r) { return (int)(6.2831853f * r / 11.0f + 0.5f); }

/* pack rings + rim, as one dot list (chunked) */
static int pack_count(void)
{
    int n = ring_n(R_RIM);
    for (int k = 0; k < NRINGS; k++) n += ring_n(k_ring_r[k]);
    return n;
}

static void pack_dot(int i, const void *vc, float *x, float *y, float *d, uint16_t *col)
{
    const reel_ctx *R = (const reel_ctx *)vc;
    float r = R_RIM;
    int n = ring_n(R_RIM), k = -1;
    if (i >= n) {
        i -= n;
        for (k = 0; k < NRINGS; k++) {
            int m = ring_n(k_ring_r[k]);
            if (i < m) { n = m; r = k_ring_r[k]; break; }
            i -= m;
        }
    }
    float a = ((float)i + (k & 1 ? 0.5f : 0.0f)) * 6.2831853f / (float)n;
    *x = R->cx + R->dx + fk_sin(a) * r;
    *y = R->cy + R->dy - fk_cos(a) * r;
    if (R->aod) { *d = k < 0 ? 3.0f : 0.0f; *col = C_LINE2; return; }
    float b = m_clamp(R->build * 1.6f - (r - 26.0f) / 55.0f, 0, 1);   /* builds outward */
    b = (float)(int)(b * 4.0f) / 4.0f;
    if (k < 0) { *d = 4.0f * b; *col = C_LINE2; return; }
    int full = r <= R->pack + 0.5f;
    *d = (full ? 7.0f : 4.0f) * b;
    *col = full ? C_SEC : C_LINE;
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
    if (R->aod) { *d = 3.5f; *col = C_DIS; return; }
    *d = 6.0f * R->build;
    *col = C_WHITE;
}

static void reel(gfx_fb *fb, reel_ctx *R)
{
    fk_dots(fb, pack_count(), 12, pack_dot, R);
    fk_dots(fb, HUB_N, HUB_N, hub_dot, R);
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    float dx = c->aod ? (float)fk_aod_dx : 0, dy = c->aod ? (float)fk_aod_dy : 0;
    float e = c->aod ? 1.0f : c->enter;
    float day = ((float)c->dt.hour * 60.0f + (float)c->dt.minute) / 1440.0f;
    /* the packs: supply holds the rest of the day, take-up what has passed */
    float span = k_ring_r[NRINGS - 1] - k_ring_r[0];
    float pk_l = k_ring_r[0] + span * (1.0f - day), pk_r = k_ring_r[0] + span * day;
    /* hub rotation: 30 degrees per second, stepped with a short ease */
    float rot = 0;
    if (!c->aod) {
        float ph = (float)c->dt.millis * 0.001f;
        float k = ph < STEP_S ? fk_mech(ph / STEP_S) : 1.0f;
        k = (float)(int)(k * 16.0f) / 16.0f;
        rot = ((float)(c->dt.second % 12) + k) * 0.5235988f;  /* 30 deg a second, one turn per 12 s */
        if (e < 1.0f) rot = 0;
    }
    float build = e >= 1.0f ? 1.0f : fk_ease(e / 0.7f);
    reel_ctx L = { RX_L, RY, pk_l, rot, dx, dy, c->aod, 0, build };
    reel_ctx R = { RX_R, RY, pk_r, rot, dx, dy, c->aod, 0, build };
    reel(fb, &L);
    reel(fb, &R);
    /* the cassette window around the reels */
    if (!c->aod && build > 0.0f)
        ui_rrect_line(fb, 46, RY - 86, GFX_W - 92, 172, 40, 2, gdimq(C_LINE2, build));

    /* timecode */
    dot_style st = { DOT_FONT_TALL, TC_P, TC_P * 0.8f, C_WHITE, 0, 1, 1 };
    if (c->aod) {
        char b[8]; memcpy(b, S.tc.to, 5); b[5] = 0;
        st.color = C_SEC; st.dot = TC_P * 0.6f;
        dot_text_a(fb, &st, UI_CX + dx, TC_Y + dy, b, 1);
    } else if (e < 1.0f)
        dot_text_anim(fb, &st, UI_CX, TC_Y, S.tc.to, 1, m_clamp((e - 0.25f) / 0.6f, 0, 1), DOT_REVEAL_SCAN, 0);
    else
        dot_text_morph(fb, &st, UI_CX, TC_Y, S.tc.from, S.tc.to, 1, fk_morph_p(&S.tc, c->t, 0.15f));

    /* date with the recording lamp */
    char b[16];
    snprintf(b, sizeof b, "%s %d", ui_weekday3(c->dt.weekday), c->dt.day);
    int w = ui_text_width(UI_F_LABEL, b, 0);
    ui_label(fb, UI_CX + 10 + (int)dx, 96 + (int)dy, b, c->aod ? C_DIS : C_SEC, UI_CENTER);
    if (!c->aod) {
        int on = c->dt.millis < 500;
        float d = (on ? 9.0f : 0.0f) * fk_stagger(e, 0.8f, 0.2f);
        if (d > 0.5f) dot_disc(fb, UI_CX - (float)w * 0.5f - 4.0f, 90.0f, d, C_RED);
    }
}

const nos_face face_tape = { "TAPE", enter, tick, draw, 0 };
