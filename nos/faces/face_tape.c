/* face_tape.c — TAPE: the recorder.
 *
 * Two dot-grid tape reels in a cassette window, and a dot-matrix timecode HH:MM:SS.
 * The reels hold the day: the left (supply) pack shrinks and the right (take-up) pack
 * grows from midnight to midnight. Each hub is a ring of 12 dots with one gap that
 * steps 30 degrees (one dot pitch) every second in 4 frames (40/72/92/100 %: the first
 * frame turns 12 degrees, under half a pitch, so the ring never aliases), like a
 * stepper-driven deck. The packs are lit dot by dot
 * (about one dot per 8 minutes). The red dot by the date is the recording lamp, on the
 * shared blink envelope; its unlit lamp stays as a dark dot so the pair stays centred.
 * Wake from AOD: rims and hubs grow, the packs fade up, HH:MM scrolls left in whole
 * lattice cells (2, then 1 a frame, like an LED sign) as the seconds grow in, and the
 * date steps 2 px a frame to make room for the lamp. Midnight: an 11-frame rewind (the
 * take-up drains back into the supply dot by dot, the hubs spin backwards one dot pitch
 * a frame, so every frame the dots stand where dots stood) with the timecode morph;
 * the date cross-fades. Everything counts whole frames (docs/MOTION30.html).
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
#define REW_N  11                        /* frames: the midnight rewind (-30 deg each) */
#define PITCH_A 0.5235988f               /* one hub dot pitch, 30 degrees */

static struct { fk_morph tc; fk_wake w; fk_xfade date; int last_hm; nos_time_t rew_t0; int pj; float pa; } S = { .last_hm = -1, .rew_t0 = -100.0f };

static void enter(void) { S.tc.init = 0; }

static uint16_t gdimq(uint16_t c, float k) { return k >= 1.0f ? c : ui_dim(c, (float)(int)(k * 4.0f) / 4.0f); }

/* whole frames into the midnight rewind (-1 = not rewinding) */
static int rew_k(const face_ctx *c)
{
    if (c->aod) return -1;
    int k = fk_frames(c->t, S.rew_t0);
    return k >= 0 && k < REW_N ? k : -1;
}
/* hub rotation (radians): 30 degrees a second in 4 frames counted from the second's
 * first frame; the rewind turns back one pitch a frame from where the second began and
 * lands where this second's step ends (11 x -30 = +30 degrees, mod a turn) */
static float hub_rot(const face_ctx *c)
{
    if (c->aod) return 0.0f;
    float base = (float)(c->dt.second % 12) * PITCH_A;
    int kr = rew_k(c);
    if (kr >= 0) return base - (float)(kr + 1) * PITCH_A;
    float q = mf_at(&FK_TICK4, fk_sec_k(c) + 1);       /* frames since the second turned */
    return base + q * PITCH_A;
}
/* the wake (f = wake frame 0..6): HH:MM's scroll in whole cells, the date's 2 px steps */
static int wake_f(float k) { return (int)(k * 6.0f + 0.5f); }
static float tc_shift(float k, float cells)
{
    int f = wake_f(k);
    float n = f <= 0 ? 0.0f : (float)(f + 1);
    return n < cells ? n : cells;
}
static int date_shift(float k) { int v = 2 * wake_f(k); return v < 8 ? v : 8; }
/* the lamp snaps in over the build-in's last frames (50/83/100 %) */
static float lamp_g(float e) { return e >= 1.0f ? 1.0f : mf_at(&FK_SNAP3, fk_build_k(e) - 8); }

static void tick(const face_ctx *c, float dt)
{
    fk_aod_update(c);
    fk_wake_tick(&S.w, c);
    fk_unread_tick(c);
    char b[12], hm[8], db[16];
    fk_time(hm, c->dt.hour, c->dt.minute);               /* 12 h: "9:41:13", centred as is */
    snprintf(b, sizeof b, "%s:%02d", hm, c->dt.second % 60);
    float e = fk_build(&S.w, c);
    int stale = fk_wake_stale(&S.w);
    int calm = c->aod || stale || e < 1.0f || fk_wake_k(&S.w, c) < 1.0f;
    if (e < 1.0f) S.tc.init = 0;
    fk_morph_tick(&S.tc, b, c->t);
    if (stale) fk_morph_settle(&S.tc);
    /* midnight: the take-up reel rewinds into the supply, starting with the timecode morph */
    int now_hm = c->dt.hour * 60 + c->dt.minute;
    if (S.last_hm == 1439 && now_hm == 0 && !calm) S.rew_t0 = c->t;
    if (calm) S.rew_t0 = -100.0f;
    S.last_hm = now_hm;
    fk_date(db, c);
    fk_xfade_tick(&S.date, db, c->t, calm);
#ifdef GFX_COST_MODEL
    fk_probe_common("tape", &S.w, c);
    fk_probe_xfade("tape", "date", &S.date, c->t);
    {
        float k = fk_wake_k(&S.w, c), dx, dy;                /* k = 0 in AOD */
        fk_wake_off(&S.w, c, &dx, &dy);
        dot_style st = { DOT_FONT_TALL, TC_P, TC_P * 0.8f, C_WHITE, 0, 1, 1 };
        int nh = (int)strlen(S.tc.to) - 3;
        if (nh < 0) nh = 0;
        char hm[8]; memcpy(hm, S.tc.to, (size_t)nh); hm[nh] = 0;
        int w5 = dot_text_width(&st, hm), w8 = dot_text_width(&st, S.tc.to);
        float x_aod = UI_CX - (float)w5 * 0.5f, x_awk = UI_CX - (float)w8 * 0.5f;
        float cells = (x_aod - x_awk) / TC_P;
        fk_probe("tape", "tc.x", MC_MICRO, k < 1.0f ? x_aod - TC_P * tc_shift(k, cells) + dx : x_awk);
        int w = ui_text_width(UI_F_LABEL, S.date.to, 2);
        int lx = UI_CX - (w + 16) / 2;
        fk_probe("tape", "date.x", MC_MICRO, (float)(lx + 8 + date_shift(k) + (int)dx));
        if (!c->aod) {
            /* the hub's rotation unwrapped against the last frame (as arc px at the hub radius) */
            int fresh = fk_probe_jump(c, &S.pj);
            float a = hub_rot(c);
            while (a - S.pa > 3.1415927f) a -= 6.2831853f;
            while (a - S.pa < -3.1415927f) a += 6.2831853f;
            S.pa = a;
            fk_probe_at("tape", rew_k(c) >= 0 ? "hub.rew" : "hub", rew_k(c) >= 0 ? MC_DECORATIVE : MC_MICRO, a * R_HUB, fresh);
            float kl = m_clamp((k - 0.5f) / 0.5f, 0, 1);
            float g = lamp_g(e) * kl;
            fk_probe("tape", "lamp", MC_MICRO, 9.0f * g);
            fk_probe("tape", "lamp.blink", MC_DECORATIVE, 9.0f * g * fk_blink(c));
        }
    }
#endif
}

/* reel geometry: pack rings of dots at fixed radii, lit dot by dot from the hub out */
static const float k_ring_r[] = { 28.0f, 38.0f, 48.0f, 58.0f };
#define NRINGS 4

typedef struct { float cx, cy, rot, dx, dy; int lit; float k, build, ghost; } reel_ctx;

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
        *col = ui_mix(C_BG, full ? C_SEC : C_LINE2, m_ink((kk - 0.25f) / 0.75f));   /* perceptual */
        return;
    }
    float b = m_clamp(R->build * 1.6f - (r - 26.0f) / 55.0f, 0, 1);   /* builds outward */
    b = (float)(int)(b * 4.0f) / 4.0f;
    if (k < 0) { *d = 4.0f * b; *col = C_LINE2; return; }
    int full = j < R->lit;
    if (!full && R->ghost <= 0.0f) { *d = 0; *col = 0; return; }   /* lite: empty pack skipped */
    *d = (full ? 7.0f : 3.0f) * b;
    *col = full ? C_SEC : R->ghost >= 1.0f ? C_LINE2 : ui_mix(C_BG, C_LINE2, R->ghost);
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
    /* the midnight rewind: the full take-up drains outer dot first while the supply
     * winds back on from the hub, both hubs spinning one turn backwards */
    int kr = rew_k(c);
    if (kr >= 0) {                                       /* an even transfer, frame by frame */
        int moved = (int)((float)(kr + 1) / (float)REW_N * (float)np + 0.5f);
        lit_l = moved; lit_r = np - moved;
    }
    /* hub rotation (also during the build-in, so the hubs never jump when it ends) */
    float rot = hub_rot(c);
    /* the build-in: the reels grow outward over 8 frames (quarter dot sizes) */
    float build = e >= 1.0f ? 1.0f : m_clamp((float)(fk_build_k(e) + 1) / 8.0f, 0, 1);
    float g = fk_ghost(c);
    reel_ctx L = { RX_L, RY, rot, dx, dy, lit_l, k, build, g };
    reel_ctx R = { RX_R, RY, rot, dx, dy, lit_r, k, build, g };
    reel(fb, &L);
    reel(fb, &R);
    /* the cassette window around the reels */
    float wa = k < 1.0f ? (k >= 0.5f ? k : 0.0f) : build;
    if (wa > 0.0f) ui_rrect_line(fb, 46 + dx, RY - 86 + dy, GFX_W - 92, 172, 40, 2, gdimq(C_LINE2, wa));

    /* timecode HH:MM:SS; AOD shows HH:MM centred, the wake slides it left as the
     * seconds grow in */
    dot_style st = { DOT_FONT_TALL, TC_P, TC_P * 0.8f, C_WHITE, 0, 1, 1 };
    if (c->aod || k < 1.0f) {
        int nh = (int)strlen(S.tc.to) - 3;                 /* HH:MM (H:MM in 12 h) before ":SS" */
        if (nh < 0) nh = 0;
        char hm[8]; memcpy(hm, S.tc.to, (size_t)nh); hm[nh] = 0;
        int w5 = dot_text_width(&st, hm), w8 = dot_text_width(&st, S.tc.to);
        float x_aod = UI_CX - (float)w5 * 0.5f, x_awk = UI_CX - (float)w8 * 0.5f;
        st.color = ui_mix(C_SEC, C_WHITE, k); st.dot = TC_P * (0.6f + 0.2f * k);
        float xh = x_aod - TC_P * tc_shift(k, (x_aod - x_awk) / TC_P) + dx;   /* whole cells */
        dot_text(fb, &st, xh, TC_Y + dy, hm);
        if (k > 0.0f) {                                  /* ":SS" rides along, growing */
            st.dot = TC_P * 0.8f * k;
            dot_text(fb, &st, xh + (float)w5 + TC_P, TC_Y + dy, S.tc.to + nh);
        }
    } else if (e < 1.0f)
        dot_text_anim(fb, &st, UI_CX, TC_Y, S.tc.to, 1, m_clamp((float)(fk_build_k(e) - 2) / 7.0f, 0, 1), DOT_REVEAL_SCAN, 0);
    else if (strlen(S.tc.from) != strlen(S.tc.to))   /* 12 h "9:59:59" -> "10:00:00": a clean cut, */
        dot_text_a(fb, &st, UI_CX, TC_Y, S.tc.to, 1);  /* not dot_text_morph's half-slot glide */
    else
        dot_text_morph(fb, &st, UI_CX, TC_Y, S.tc.from, S.tc.to, 1, fk_morph_pk(&S.tc, c->t, 4));

    /* date with the recording lamp: the lamp is always there (off = #1A1A1A), so the
     * pair stays centred; it lights with the shared blink envelope */
    int w = ui_text_width(UI_F_LABEL, S.date.to, 2);
    int lx = UI_CX - (w + 16) / 2;                       /* lamp 9 px + 7 px gap, then text */
    /* AOD has no lamp: the date centres alone, and slides 8 px right as the wake brings
     * the lamp up (it grows over the wake's second half) */
    float kw = c->aod ? 0.0f : k;
    fk_xfade_label(fb, &S.date, c->t, lx + 8 + date_shift(kw) + (int)dx, 96 + (int)dy,
                   c->aod ? C_DIS : ui_mix(C_DIS, C_SEC, k), UI_LEFT);
    float kl = m_clamp((kw - 0.5f) / 0.5f, 0, 1);
    if (kl > 0.0f) {
        float g = lamp_g(e) * kl;
        float x = (float)lx + 4.5f + dx, y = 90.0f + dy;
        dot_disc(fb, x, y, 9.0f * g, C_SURF2);
        float d = 9.0f * fk_blink(c) * g;
        if (d > 0.5f) dot_disc(fb, x, y, d, C_RED);     /* the REC light: stays red */
    }
    /* battery: a critical one shows in AOD too and brightens; otherwise it fades up */
    face_ctx ca = *c; ca.aod = 1;
    uint16_t sc = c->aod ? C_DIS : k >= 1.0f ? C_SEC : fk_status_shown(&ca) ? ui_mix(C_DIS, C_SEC, k) : ui_dim(C_SEC, m_ink(k));
    fk_status(fb, c, UI_CX + (int)dx, 406 + (int)dy, sc, UI_CENTER);
    fk_unread(fb, c, ui_mix(C_SEC, C_PRI, k), C_BG, dx, dy);
}

const nos_face face_tape = { "TAPE", enter, tick, draw, 0 };
