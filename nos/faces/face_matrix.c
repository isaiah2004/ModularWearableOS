/* face_matrix.c — MATRIX: the Phone (3) Glyph Matrix as a watch face.
 *
 * The whole face is one circular 25 x 25 dot grid (489 cells, pitch 17.6, dot 14) and
 * everything is drawn INTO it: HH over MM in 5x7 digits; the seconds as the outer
 * ring of cells filling clockwise through the minute, moving only on the second (60
 * head cells picked by nearest angle to i x 6 degrees; the 8 spare ring cells light
 * with their neighbours); a slow, dim sparkle in the background away from the digits;
 * the battery, when low or charging, as a 5-cell bar on row 22. Four brightness levels
 * (ghost, dim, mid, full) — no anti-aliasing, no other shapes. The unread
 * notification is the top centre cell turning into a small red dot.
 *
 * Cost: cell levels are computed in tick() into a PSRAM array; draw() records one
 * damage command per run of 5 cells (bbox + hash of their levels), so a changing cell
 * repaints an 88 x 18 px box. Full redraw (pager): ~11 ms on the cost model.
 * Motion: build-in = a ripple from the centre switches cells on, digits light behind it;
 * wake from AOD (6 frames) = the AOD digit dots grow into the grid while the grid fades
 * up, the seconds ring and its head rising from it over the last 3 (no blank frame); minute change = the ring drains backwards while the changed digit
 * cells fade out/in through the levels, each line scanning from its top (6 frames). */
#include "faces/face_kit.h"
#include "nos.h"
#include <string.h>

#define N      25
#define CEN    12
#define PITCH  17.6f
#define DOTD   14.0f
#define D_AOD  6.5f
#define R_CELL 12.4f
#define CHUNK  5
#define DRAIN_S 0.20f
#define BAT_ROW 22

#define GHOST 0x18C3
static const uint16_t k_lvl[5] = { GHOST /* #181818 */, 0x4A69 /* #4D4D4D */,
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
    uint8_t  lvl[N * N];       /* current level per cell (k_lvl index) */
    uint8_t  mask[N * N];      /* 1 inside the circle, 2 = outer ring cell */
    uint16_t ang[N * N];       /* ring cells: clock angle, 0.1 degree */
    int16_t  head[60];         /* ring cell lit as the head at second i */
    uint8_t  now[N * N], prev[N * N];   /* digit patterns: current / before the change */
    uint8_t  used[N * N];      /* scratch for the head assignment */
    int built;
} mx_state;

static mx_state *M;
static float s_change_t = -100.0f;
static int s_last_hm = -1;
static float s_wake_k = 1.0f;          /* published by tick for draw */
static uint8_t s_lo[N], s_hi[N];       /* first / one-past-last cell per row */
static fk_wake s_w;
static int s_bat_aod;                  /* critical battery: its bar shows in AOD too */

static float cell_x(int c) { return UI_CX + (float)(c - CEN) * PITCH; }
static float cell_y(int r) { return UI_CY + (float)(r - CEN) * PITCH; }

/* clock angle of (dx, dy) in degrees, 12 o'clock = 0, clockwise (atan approximation) */
static float clock_deg(float dx, float dy)
{
    float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    float mn = ax < ay ? ax : ay, mxv = ax > ay ? ax : ay;
    float t = mxv > 0 ? mn / mxv : 0, t2 = t * t;
    float q = t * (0.99997726f + t2 * (-0.33262347f + t2 * (0.19354346f + t2 * (-0.11643287f + t2 * (0.05265332f - t2 * 0.01172120f)))));
    if (ay > ax) q = 1.5707963f - q;              /* angle of (ax, ay) from the x axis */
    float th;                                      /* standard atan2(-dy, dx) */
    if (dx >= 0 && dy <= 0) th = q;
    else if (dx < 0 && dy <= 0) th = 3.1415927f - q;
    else if (dx < 0) th = 3.1415927f + q;
    else th = 6.2831853f - q;
    float a = 1.5707963f - th;
    if (a < 0) a += 6.2831853f;
    return a * 57.29578f;
}

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
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            if (!M->mask[r * N + c]) continue;
            int edge = r == 0 || c == 0 || r == N - 1 || c == N - 1 ||
                       !M->mask[(r - 1) * N + c] || !M->mask[(r + 1) * N + c] ||
                       !M->mask[r * N + c - 1] || !M->mask[r * N + c + 1];
            if (!edge) continue;
            M->mask[r * N + c] = 2;
            M->ang[r * N + c] = (uint16_t)(clock_deg((float)(c - CEN), (float)(r - CEN)) * 10.0f + 0.5f);
        }
    /* one head cell per second: the free ring cell nearest i x 6 degrees */
    uint8_t *used = M->used;
    for (int i = 0; i < 60; i++) {
        int best = -1, bd = 1 << 30;
        for (int k = 0; k < N * N; k++) {
            if (M->mask[k] != 2 || used[k]) continue;
            int d = (int)M->ang[k] - i * 60;
            if (d > 1800) d -= 3600;
            if (d < -1800) d += 3600;
            if (d < 0) d = -d;
            if (d < bd) { bd = d; best = k; }
        }
        M->head[i] = (int16_t)best;
        if (best >= 0) used[best] = 1;
    }
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
            char ch = rows[line][k];
            if (ch < '0' || ch > '9') continue;
            const uint8_t *g = k_dig[ch - '0'];
            int c0 = k ? 13 : 7, r0 = line ? 13 : 5;
            if (!line && rows[0][0] == ' ') c0 = 10;          /* 12 h: a lone hour digit centres */
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

static int bat_cell(int i) { int r = i / N, c = i % N; return r == BAT_ROW && c >= 10 && c < 15; }
static int bat_lit(int batt) { int n = (batt + 19) / 20; return n < 1 ? 1 : n > 5 ? 5 : n; }

static int in_digit_zone(int r, int c) { return r >= 4 && r <= 20 && c >= 6 && c <= 18; }

static void tick(const face_ctx *c, float dt)
{
    build();
    fk_aod_update(c);
    fk_wake_tick(&s_w, c);
    fk_unread_tick(c);
    s_wake_k = fk_wake_k(&s_w, c);
    { face_ctx ca = *c; ca.aod = 1; s_bat_aod = fk_status_shown(&ca); }
    if (!M) return;
    int hm = c->dt.hour * 60 + c->dt.minute;
    if (s_last_hm < 0) { pattern(M->now, c->dt.hour, c->dt.minute); memcpy(M->prev, M->now, N * N); }
    else if (hm != s_last_hm) {
        memcpy(M->prev, M->now, N * N);
        pattern(M->now, c->dt.hour, c->dt.minute);
        s_change_t = c->t;
    }
    s_last_hm = hm;
    if (fk_wake_stale(&s_w)) {                       /* off the glass: no replayed change */
        memcpy(M->prev, M->now, N * N);
        s_change_t = -100.0f;
    }

    float e = fk_build(&s_w, c);
    int calm = c->aod;                               /* no ring / sparkle / status (the wake fades them up) */
    float since = c->t - s_change_t;
    int sec = c->dt.second;
    int past10 = sec * 60;                            /* ring lit below this angle (0.1 deg) */
    int head = M->head[sec];
    if (since < DRAIN_S && sec == 0) {               /* the full ring drains backwards */
        past10 = (int)(3600.0f * (1.0f - fk_mech(since / DRAIN_S)));
        head = -1;
    }
    float wave = e < 1.0f ? fk_ease(e / 0.8f) * (R_CELL + 3.0f) : 1e9f;

    for (int r = 0; r < N; r++)
        for (int col = 0; col < N; col++) {
            int i = r * N + col;
            uint8_t mk = M->mask[i];
            if (!mk) continue;
            int L = 0;
            /* digits, with the minute-change scan (each line from its own top row) */
            if (M->now[i] || M->prev[i]) {
                if (M->now[i] == M->prev[i]) L = 3;
                else {
                    int r0 = r < 12 ? 5 : 13;
                    int step = (int)((since - (float)(r - r0) * 0.012f) * 30.0f);
                    if (step < 0) step = 0;
                    if (step > 3) step = 3;
                    L = M->now[i] ? step : 3 - step;
                }
            }
            if (mk == 2 && !calm) {
                if (i == head) L = 3;
                else if ((int)M->ang[i] < past10) L = 1;
            }
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
    if (e >= 1.0f && !calm) {
        /* sparkle: 3 slots, each breathes one background cell to the dim level */
        for (int j = 0; j < 3 && s_wake_k >= 1.0f; j++) {
            float tt = c->t + (float)j * 0.83f;
            int cyc = (int)(tt / 2.4f);
            float ph = tt / 2.4f - (float)cyc;
            if (ph > 0.5f) continue;
            unsigned h = hsh((unsigned)cyc * 7u + (unsigned)j, 0x5EEDu);
            int cell = (int)(h % (N * N));
            int r = cell / N, col = cell % N;
            if (M->mask[cell] != 1 || in_digit_zone(r, col) || r >= BAT_ROW - 1) continue;
            if (M->lvl[cell] < 1) M->lvl[cell] = 1;
        }
        /* battery: a 5-cell bar on row 22 when low or charging */
        if (fk_status_shown(c)) {
            int lit = bat_lit(c->battery);
            for (int k = 0; k < 5; k++) {
                int L = k < lit ? 3 : 1;
                if (c->charging && k == lit - 1 && c->battery < 100) L = fk_blink(c->dt.millis) > 0.5f ? 3 : 2;
                M->lvl[BAT_ROW * N + 10 + k] = (uint8_t)L;
            }
        }
    }
}

/* per-cell look: level colour normally; during the wake morph the digit cells grow from
 * the AOD dot and the grid fades up from black */
static int cell_look(int i, float k, float *d, uint16_t *col)
{
    int L = M->lvl[i];
    if (L == L_OFF) return 0;
    if (k >= 1.0f) { *d = DOTD; *col = k_lvl[L]; return 1; }
    if (M->now[i]) { *d = D_AOD + (DOTD - D_AOD) * k; *col = ui_mix(C_SEC, C_WHITE, k); return 1; }
    if (s_bat_aod && bat_cell(i)) {                   /* the AOD battery bar grows like the digits */
        *d = D_AOD + (DOTD - D_AOD) * k;
        *col = ui_mix(L >= 3 ? C_SEC : C_LINE2, k_lvl[L], k);
        return 1;
    }
    if (k < 0.5f) return 0;
    /* the grid fades up from black; ring, head and battery cells rise from it over
     * the wake's last 3 frames (never all at once) */
    uint16_t base = ui_mix(C_BG, GHOST, (k - 0.25f) / 0.75f);
    *d = DOTD;
    *col = L > 0 ? ui_mix(base, k_lvl[L], (k - 0.5f) / 0.5f) : base;
    return 1;
}

/* AOD: the critical battery bar as small dots on its row (1 lit cell of 5) */
static void aod_battery(gfx_fb *fb, const face_ctx *c, float dx, float dy)
{
    int lit = bat_lit(c->battery);
    float y = cell_y(BAT_ROW) + dy;
    if (fb->rec) {
        unsigned h = gfx_hash_f(gfx_hash_f(gfx_hash_u(0xBA7Au, (unsigned)lit), dx), dy);
        gfx_rec_cmd(fb, (int)(cell_x(10) + dx - 8), (int)(y - 8), (int)(cell_x(14) + dx + 9), (int)(y + 9), h);
        return;
    }
    for (int k = 0; k < 5; k++) dot_disc(fb, cell_x(10 + k) + dx, y, D_AOD, k < lit ? C_SEC : C_LINE2);
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    if (!M || !M->built) return;
    float dx, dy;
    fk_wake_off(&s_w, c, &dx, &dy);
    if (c->aod) {
        for (int r = 5; r < 20; r++) {
            int any = 0;
            for (int col = 7; col < 18; col++) any |= M->now[r * N + col];
            if (!any) continue;
            if (fb->rec) {
                unsigned h = gfx_hash_u(0xA0Du, (unsigned)r);
                for (int col = 7; col < 18; col++) h = gfx_hash_u(h, M->now[r * N + col]);
                h = gfx_hash_f(gfx_hash_f(h, dx), dy);
                gfx_rec_cmd(fb, (int)(cell_x(7) + dx - 8), (int)(cell_y(r) + dy - 8),
                            (int)(cell_x(17) + dx + 9), (int)(cell_y(r) + dy + 9), h);
                continue;
            }
            for (int col = 7; col < 18; col++)
                if (M->now[r * N + col]) dot_disc(fb, cell_x(col) + dx, cell_y(r) + dy, D_AOD, C_SEC);
        }
        if (s_bat_aod) aod_battery(fb, c, dx, dy);
        fk_unread(fb, c, C_SEC, C_BG, dx, dy - 2.2f);
        return;
    }
    float k = s_wake_k;
    fk_view v;
    if (!fb->rec) fk_view_of(fb, &v);
    const float e = DOTD * 0.5f + 1.0f;
    for (int r = 0; r < N; r++) {
        float y = cell_y(r) + dy;
        if (!fb->rec && (y + e <= v.y0 || y - e >= v.y1)) continue;
        for (int c0 = s_lo[r]; c0 < s_hi[r]; c0 += CHUNK) {
            int c1 = c0 + CHUNK < s_hi[r] ? c0 + CHUNK : s_hi[r];
            if (fb->rec) {
                unsigned h = gfx_hash_u(0x3A7Bu, (unsigned)(r * 64 + c0));
                h = gfx_hash_f(gfx_hash_f(gfx_hash_f(h, k), dx), dy);
                for (int col = c0; col < c1; col++) h = gfx_hash_u(h, M->lvl[r * N + col] | (M->now[r * N + col] << 4));
                gfx_rec_cmd(fb, (int)(cell_x(c0) + dx - e), (int)(y - e), (int)(cell_x(c1 - 1) + dx + e) + 1,
                            (int)(y + e) + 1, h);
                continue;
            }
            if (cell_x(c1 - 1) + dx + e <= v.x0 || cell_x(c0) + dx - e >= v.x1) continue;
            for (int col = c0; col < c1; col++) {
                float d; uint16_t cc;
                if (cell_look(r * N + col, k, &d, &cc)) dot_disc(fb, cell_x(col) + dx, y, d, cc);
            }
        }
    }
    /* the unread dot replaces the top centre cell (row 0 sits at y = 21.8) */
    fk_unread(fb, c, ui_mix(C_SEC, C_RED, k), C_BG, dx, dy - 2.2f);
}

static const struct nos_screen *tap(int x, int y) { return 0; }

const nos_face face_matrix = { "MATRIX", enter, tick, draw, tap };
