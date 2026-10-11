/* face_matrix.c — MATRIX: the Phone (3) Glyph Matrix as a watch face.
 *
 * The whole face is one circular 25 x 25 dot grid (489 cells, pitch 17.6, dot 14) and
 * everything is drawn INTO it: HH over MM in 5x7 digits, the seconds as the outer
 * ring of cells filling clockwise through the minute (a brighter head cell), and a
 * slow sparkle of a few background cells breathing in and out. Cells have four
 * brightness levels (ghost, dim, mid, full) — no anti-aliasing, no other shapes.
 *
 * Cost: cell levels are computed in tick() into a 625-byte PSRAM array; draw()
 * records one damage command per run of 5 cells (bbox + hash of their levels), so a
 * changing cell repaints an 88 x 18 px box. Idle frames: the seconds head moves once
 * a second and ~3 sparkles step 4 levels each, all single-cell boxes.
 * Full redraw (pager): 489 mask stamps, ~11 ms on the cost model.
 * Build-in: a ripple runs out from the centre switching cells on, the digits light
 * behind it. Minute change: changed cells fade out/in through the levels, top row
 * first (a 6-frame scan). */
#include "faces/face_kit.h"
#include "nos.h"
#include <string.h>

#define N      25
#define CEN    12
#define PITCH  17.6f
#define DOTD   14.0f
#define R_CELL 12.4f
#define CHUNK  5

static const uint16_t k_lvl[5] = { 0x18C3 /* #1A1A1A ghost */, 0x4A69 /* #4D4D4D */,
                                   0x9CD3 /* #999 */, 0xFFFF, 0x0000 };
#define L_OFF 4   /* not drawn (build-in, before the ripple reaches it) */

/* 5x7 digits, bit 4 = leftmost column */
static const uint8_t k_dig[10][7] = {
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
    { 0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
};

typedef struct {
    uint8_t lvl[N * N];       /* current level per cell (k_lvl index) */
    uint8_t mask[N * N];      /* 1 inside the circle, 2 = outer ring cell */
    uint8_t ringpos[N * N];   /* ring cells: angular order 0..nring-1 */
    uint8_t now[N * N], prev[N * N];   /* digit patterns: current / before the change */
    int nring;
    int built;
} mx_state;

static mx_state *M;
static float s_change_t = -100.0f;
static int s_last_hm = -1;
static uint8_t s_lo[N], s_hi[N];       /* first / one-past-last cell per row */

static float cell_x(int c) { return UI_CX + (float)(c - CEN) * PITCH; }
static float cell_y(int r) { return UI_CY + (float)(r - CEN) * PITCH; }

static void build(void)
{
    if (M && M->built) return;
    if (!M) M = (mx_state *)hal_big_alloc(sizeof *M);
    if (!M) return;
    memset(M, 0, sizeof *M);
    for (int r = 0; r < N; r++) {
        s_lo[r] = N; s_hi[r] = 0;
        for (int c = 0; c < N; c++) {
            float dx = (float)(c - CEN), dy = (float)(r - CEN);
            if (dx * dx + dy * dy <= R_CELL * R_CELL) {
                M->mask[r * N + c] = 1;
                if (c < s_lo[r]) s_lo[r] = (uint8_t)c;
                s_hi[r] = (uint8_t)(c + 1);
            }
        }
    }
    /* outer ring: inside cells with an outside 4-neighbour */
    int idx[N * 4];
    float ang[N * 4];
    int n = 0;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            if (!M->mask[r * N + c]) continue;
            int edge = r == 0 || c == 0 || r == N - 1 || c == N - 1 ||
                       !M->mask[(r - 1) * N + c] || !M->mask[(r + 1) * N + c] ||
                       !M->mask[r * N + c - 1] || !M->mask[r * N + c + 1];
            if (!edge || n >= N * 4) continue;
            M->mask[r * N + c] = 2;
            float dx = (float)(c - CEN), dy = (float)(r - CEN);
            /* clockwise from 12 o'clock: atan2(dx, -dy) mapped to [0, 2pi) */
            float a = 0;
            {   /* cheap atan2 good to ~0.3 deg; only ordering matters */
                float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
                float mn = ax < ay ? ax : ay, mxv = ax > ay ? ax : ay;
                float t = mxv > 0 ? mn / mxv : 0, t2 = t * t;
                float q = t * (0.99997726f + t2 * (-0.33262347f + t2 * (0.19354346f + t2 * (-0.11643287f + t2 * (0.05265332f - t2 * 0.01172120f)))));
                if (ay > ax) q = 1.5707963f - q;          /* angle from the x axis */
                /* q = angle of (ax, ay) from +x; convert to clock angle */
                float th;                                  /* standard atan2(-dy, dx) */
                if (dx >= 0 && dy <= 0) th = q;
                else if (dx < 0 && dy <= 0) th = 3.1415927f - q;
                else if (dx < 0) th = 3.1415927f + q;
                else th = 6.2831853f - q;
                a = 1.5707963f - th;                       /* 12 o'clock = 0, clockwise */
                if (a < 0) a += 6.2831853f;
            }
            idx[n] = r * N + c; ang[n] = a; n++;
        }
    for (int i = 1; i < n; i++) {                          /* insertion sort by angle */
        int ki = idx[i]; float ka = ang[i]; int j = i - 1;
        while (j >= 0 && ang[j] > ka) { ang[j + 1] = ang[j]; idx[j + 1] = idx[j]; j--; }
        ang[j + 1] = ka; idx[j + 1] = ki;
    }
    for (int i = 0; i < n; i++) M->ringpos[idx[i]] = (uint8_t)i;
    M->nring = n;
    M->built = 1;
}

/* HH rows 5-11, MM rows 13-19; digit columns 7-11 and 13-17 */
static void pattern(uint8_t *out, int hour, int minute)
{
    memset(out, 0, N * N);
    char hh[3], mm[3];
    fk_hh(hh, hour);
    fk_mm(mm, minute);
    const char *rows[2] = { hh, mm };
    for (int line = 0; line < 2; line++)
        for (int k = 0; k < 2; k++) {
            const uint8_t *g = k_dig[rows[line][k] - '0'];
            int c0 = k ? 13 : 7, r0 = line ? 13 : 5;
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++)
                    if (g[r] & (0x10 >> c)) out[(r0 + r) * N + c0 + c] = 1;
        }
}

static void enter(void) { s_last_hm = -1; }

static unsigned hsh(unsigned a, unsigned b)
{
    unsigned h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

static void tick(const face_ctx *c, float dt)
{
    build();
    fk_aod_update(c);
    if (!M) return;
    int hm = c->dt.hour * 60 + c->dt.minute;
    if (s_last_hm < 0) { pattern(M->now, c->dt.hour, c->dt.minute); memcpy(M->prev, M->now, N * N); }
    else if (hm != s_last_hm) {
        memcpy(M->prev, M->now, N * N);
        pattern(M->now, c->dt.hour, c->dt.minute);
        s_change_t = c->t;
    }
    s_last_hm = hm;

    float e = c->enter;
    float since = c->t - s_change_t;
    float secf = (float)c->dt.second + (float)c->dt.millis * 0.001f;
    int head = (int)(secf / 60.0f * (float)M->nring);
    float wave = e < 1.0f ? fk_ease(e / 0.8f) * (R_CELL + 3.0f) : 1e9f;

    for (int r = 0; r < N; r++)
        for (int col = 0; col < N; col++) {
            int i = r * N + col;
            uint8_t mk = M->mask[i];
            if (!mk) continue;
            int L = 0;
            /* digits, with the minute-change scan */
            if (M->now[i] || M->prev[i]) {
                if (M->now[i] == M->prev[i]) L = 3;
                else {
                    int step = (int)((since - (float)(r - 5) * 0.012f) * 30.0f);   /* frames */
                    if (step < 0) step = 0;
                    if (step > 3) step = 3;
                    L = M->now[i] ? step : 3 - step;
                }
            }
            /* seconds ring */
            if (mk == 2 && !c->aod) {
                int p = M->ringpos[i];
                if (p < head) L = 1;
                else if (p == head) L = 3;
            }
            /* build-in ripple: cells appear as the wave passes, crest bright */
            if (e < 1.0f) {
                float dx = (float)(col - CEN), dy = (float)(r - CEN);
                float dist = gfx_fast_sqrt(dx * dx + dy * dy);
                if (dist > wave) L = L_OFF;
                else if (dist > wave - 1.5f) L = 2;
                else if (M->now[i]) {
                    float k = m_clamp((wave - dist - 1.5f) / 2.0f, 0, 1);
                    L = 1 + (int)(k * 2.0f + 0.5f);
                }
            }
            M->lvl[i] = (uint8_t)L;
        }
    /* sparkle: 4 slots, each lights one background cell for 1.6 s (dim-mid-dim) */
    if (e >= 1.0f && !c->aod) {
        for (int j = 0; j < 4; j++) {
            float tt = c->t + (float)j * 0.83f;
            int cyc = (int)(tt / 2.4f);
            float ph = tt / 2.4f - (float)cyc;               /* 0..1 within the cycle */
            if (ph > 0.66f) continue;                        /* rests a third of the time */
            unsigned h = hsh((unsigned)cyc * 7u + (unsigned)j, 0x5EEDu);
            int cell = (int)(h % (N * N));
            if (M->mask[cell] != 1 || M->now[cell] || M->prev[cell]) continue;
            float k = ph / 0.66f;                            /* 0..1 */
            int L = k < 0.2f || k > 0.8f ? 1 : 2;
            if (L > M->lvl[cell]) M->lvl[cell] = (uint8_t)L;
        }
    }
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    if (!M || !M->built) return;
    if (c->aod) {
        float dx = (float)fk_aod_dx, dy = (float)fk_aod_dy;
        for (int r = 5; r < 20; r++) {
            int any = 0;
            for (int col = 7; col < 18; col++) any |= M->now[r * N + col];
            if (!any) continue;
            /* one record per row of digits */
            if (fb->rec) {
                unsigned h = gfx_hash_u(0xA0Du, (unsigned)r);
                for (int col = 7; col < 18; col++) h = gfx_hash_u(h, M->now[r * N + col]);
                h = gfx_hash_f(gfx_hash_f(h, dx), dy);
                gfx_rec_cmd(fb, (int)(cell_x(7) + dx - 8), (int)(cell_y(r) + dy - 8),
                            (int)(cell_x(17) + dx + 9), (int)(cell_y(r) + dy + 9), h);
                continue;
            }
            for (int col = 7; col < 18; col++)
                if (M->now[r * N + col]) dot_disc(fb, cell_x(col) + dx, cell_y(r) + dy, 6.5f, C_SEC);
        }
        return;
    }
    fk_view v;
    if (!fb->rec) fk_view_of(fb, &v);
    const float e = DOTD * 0.5f + 1.0f;
    for (int r = 0; r < N; r++) {
        float y = cell_y(r);
        if (!fb->rec && (y + e <= v.y0 || y - e >= v.y1)) continue;
        for (int c0 = s_lo[r]; c0 < s_hi[r]; c0 += CHUNK) {
            int c1 = c0 + CHUNK < s_hi[r] ? c0 + CHUNK : s_hi[r];
            if (fb->rec) {
                unsigned h = gfx_hash_u(0x3A7Bu, (unsigned)(r * 64 + c0));
                for (int col = c0; col < c1; col++) h = gfx_hash_u(h, M->lvl[r * N + col]);
                gfx_rec_cmd(fb, (int)(cell_x(c0) - e), (int)(y - e), (int)(cell_x(c1 - 1) + e) + 1,
                            (int)(y + e) + 1, h);
                continue;
            }
            if (cell_x(c1 - 1) + e <= v.x0 || cell_x(c0) - e >= v.x1) continue;
            for (int col = c0; col < c1; col++) {
                int L = M->lvl[r * N + col];
                if (L == L_OFF) continue;
                dot_disc(fb, cell_x(col), y, DOTD, k_lvl[L]);
            }
        }
    }
}

static const struct nos_screen *tap(int x, int y) { return 0; }

const nos_face face_matrix = { "MATRIX", enter, tick, draw, tap };
