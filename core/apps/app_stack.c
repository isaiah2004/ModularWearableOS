/* app_stack.c — Flat60 app list (RIGHT edge overlay when wos_ui_style == 1).
 *
 * Built to the Flat60 budget (docs/FLAT60_DESIGN.md): each app is one bold chip
 * (surface pill, solid accent disc, white glyph, label) rendered ONCE into a
 * PSRAM sprite. Scrolling only clears to black and blits the visible chips, so
 * a frame costs ~33 ns per moving pixel whatever the scroll speed, and the
 * dirty area is bounded by the chip column.
 *
 * Depth without per-pixel effects: chips are 8-bit sprites (sprite8) blitted
 * through a colour table, so the focused chip's surface takes its app's accent
 * and chips dim with distance by swapping 256-entry tables, and chips shift
 * along the bezel (a precomputed offset per centre row) so the column follows
 * the glass. Both cost the same as a plain blit. */
#include "apps.h"
#include "../flat60.h"
#include "../font.h"
#include "../sprite.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHIP_W     320
#define CHIP_H     76
#define CHIP_R     (CHIP_H / 2)
#define PITCH      90             /* row spacing: a 14 px black gap between chips */
#define DISC_R     28
#define MAX_APPS   40
#define CURVE_K    0.55f          /* how far chips follow the bezel (1 = hug it) */
#define TINT_K     0.50f          /* focused surface = this much of the accent */
#define F60_TEXT3  GFX_RGB(146, 146, 170)  /* text two rows from the focus */
#define LUT_SLOTS  16

/* drag / inertia feel, as the classic list */
#define OVER_RESIST 60.0f
#define OVER_MAX    90.0f
#define MOVE_DMAX   (GFX_H * 0.5f)
#define DT_FLOOR    8u
#define VEL_MAX     4000.0f

static sprite8 chips[MAX_APPS];
static sprite8 title;           /* "Apps" heading, one row above the first chip */
static sprite  scratch;         /* RGB565 canvas each chip is drawn on, then quantized */
/* Depth and arc state lives in PSRAM: internal RAM has no room to spare (15 KB
 * more statics here starved the firmware's own buffers and tasks). */
static struct {
    float   keys[MAX_APPS][3][9], keys_inv[MAX_APPS][9];   /* see depth_keys() */
    uint8_t keys_ok[MAX_APPS];
    int16_t arc_x[CHIP_H + GFX_H + CHIP_H];                /* x shift by chip centre y + CHIP_H */
    struct { unsigned key; uint16_t lut[256]; } luts[LUT_SLOTS];
} *dm;
static int     lut_next;
static int    chips_ready;       /* sprites rendered for this many apps */
static float  scroll, vel;       /* px; row i is centred at GFX_CY + i*PITCH - scroll */
static int    dragging, last_y;
static uint32_t last_t;

static float max_scroll(void) { return (wos_app_count() - 1) * (float)PITCH; }

/* Draw app i's chip into its sprite (once). */
static void render_chip(int i)
{
    gfx_fb fb = sprite_fb(&scratch);
    float cy = CHIP_H * 0.5f;
    gfx_clear(&fb, F60_BLACK);
    gfx_fill_pill(&fb, CHIP_R, cy, CHIP_W - CHIP_R, CHIP_R - 1, F60_SURFACE);
    float dcx = CHIP_R;                                   /* disc sits in the left cap */
    gfx_fill_circle(&fb, dcx, cy, DISC_R, f60_accent(wos_app_accent(i)));
    int icon = wos_app_icon(i);
    if (icon != ICON_NONE) icon_glyph(&fb, icon, dcx, cy, DISC_R * 0.62f, F60_WHITE);
    else {
        char init[2] = { wos_app_name(i)[0], 0 };
        if (init[0] >= 'a' && init[0] <= 'z') init[0] -= 32;
        font_draw_centered(&fb, &font_medium, (int)dcx, (int)(cy + font_medium.ascent * 0.34f), init, F60_WHITE);
    }
    char label[24];                                       /* registry names vary in case */
    snprintf(label, sizeof label, "%s", wos_app_name(i));
    if (label[0] >= 'a' && label[0] <= 'z') label[0] -= 32;
    font_draw(&fb, &font_medium, (int)(dcx + DISC_R + 16), (int)(cy + font_medium.ascent * 0.34f),
              label, F60_WHITE);
    sprite8_from(&chips[i], &scratch);
}

static void rgb_of(uint16_t c, float v[3])
{
    v[0] = (float)((c >> 11) * 255 / 31);
    v[1] = (float)(((c >> 5) & 63) * 255 / 63);
    v[2] = (float)((c & 31) * 255 / 31);
}

static int q_level(float v, int steps)        /* nearest of steps+1 levels over 0..255 */
{
    int q = (int)(v * steps / 255.0f + 0.5f);
    return q < 0 ? 0 : q > steps ? steps : q;
}

static void snap332(float v[3], float scale)  /* v * scale, then the nearest RGB332 colour */
{
    v[0] = q_level(v[0] * scale, 7) * 255 / 7;
    v[1] = q_level(v[1] * scale, 7) * 255 / 7;
    v[2] = q_level(v[2] * scale, 3) * 85.0f;
}

/* Chip i's depth keyframes, 0, 1 and 2 rows from the focus, as colour maps:
 * where surface, white and accent go (columns of key[k]). Each chip pixel is a
 * mix of those three and black, so the linear map M = key * inverse([S W A])
 * moves every anti-aliased edge along with them. All targets are RGB332-exact,
 * so chips at rest show exact palette colours. Returns 0 (and no map) when the
 * three colours are nearly dependent, e.g. a grey accent. */
static int depth_keys(int i, float key[3][9], float inv[9])
{
    float S[3], W[3] = { 255, 255, 255 }, A[3];
    rgb_of(F60_SURFACE, S);
    rgb_of(f60_accent(wos_app_accent(i)), A);
    float b[9] = { S[0], W[0], A[0], S[1], W[1], A[1], S[2], W[2], A[2] };
    float c0 = b[4] * b[8] - b[5] * b[7], c1 = b[5] * b[6] - b[3] * b[8], c2 = b[3] * b[7] - b[4] * b[6];
    float det = b[0] * c0 + b[1] * c1 + b[2] * c2;
    if (fabsf(det) < 2.0e5f) return 0;
    float m[9] = {
        c0 / det, (b[2] * b[7] - b[1] * b[8]) / det, (b[1] * b[5] - b[2] * b[4]) / det,
        c1 / det, (b[0] * b[8] - b[2] * b[6]) / det, (b[2] * b[3] - b[0] * b[5]) / det,
        c2 / det, (b[1] * b[6] - b[0] * b[7]) / det, (b[0] * b[4] - b[1] * b[3]) / det,
    };
    for (int k = 0; k < 9; k++) inv[k] = m[k];
    for (int k = 0; k < 3; k++) {
        float s[3] = { S[0], S[1], S[2] }, w[3], a[3] = { A[0], A[1], A[2] };
        rgb_of(k == 0 ? F60_WHITE : k == 1 ? F60_TEXT2 : F60_TEXT3, w);
        if (k == 0) { s[0] = A[0]; s[1] = A[1]; s[2] = A[2]; snap332(s, TINT_K); }   /* focus: accent surface */
        else snap332(a, k == 1 ? 0.75f : 0.5f);
        for (int r = 0; r < 3; r++) { key[k][r * 3] = s[r]; key[k][r * 3 + 1] = w[r]; key[k][r * 3 + 2] = a[r]; }
    }
    return 1;
}

/* Colour table for chip i (-1 = title) at tq/8 rows from the focus. Tables are
 * cached by key: building one is 256 small matrix products, and the raster pass
 * asks once per stripe (the record pass builds them first, once a frame). */
static const uint16_t *chip_lut(int i, int tq, unsigned key)
{
    for (int k = 0; k < LUT_SLOTS; k++) if (dm->luts[k].key == key) return dm->luts[k].lut;
    int slot = lut_next;
    lut_next = (lut_next + 1) % LUT_SLOTS;
    dm->luts[slot].key = key;
    float t = tq / 8.0f, m[9] = { 0 };
    if (i >= 0 && dm->keys_ok[i]) {
        int k = t < 1.0f ? 0 : 1;
        float u = t - k, T[9];
        for (int e = 0; e < 9; e++) T[e] = dm->keys[i][k][e] + u * (dm->keys[i][k + 1][e] - dm->keys[i][k][e]);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                m[r * 3 + c] = T[r * 3] * dm->keys_inv[i][c] + T[r * 3 + 1] * dm->keys_inv[i][3 + c] + T[r * 3 + 2] * dm->keys_inv[i][6 + c];
    } else {                    /* title, or no accent map: scale like the text does */
        float w1[3], w2[3], f = i < 0 ? (t < 1.0f ? 0.0f : t - 1.0f) : t * 0.5f;
        rgb_of(i < 0 ? F60_TEXT2 : F60_WHITE, w1);
        rgb_of(i < 0 ? F60_TEXT3 : F60_TEXT2, w2);
        for (int r = 0; r < 3; r++) m[r * 4] = (w1[r] + f * (w2[r] - w1[r])) / w1[r];
    }
    for (int x = 0; x < 256; x++) {
        int c[3];
        rgb332_levels(x, c);
        float o[3];
        for (int r = 0; r < 3; r++) o[r] = m[r * 3] * c[0] + m[r * 3 + 1] * c[1] + m[r * 3 + 2] * c[2];
        dm->luts[slot].lut[x] = GFX_RGB(q_level(o[0], 7) * 255 / 7, q_level(o[1], 7) * 255 / 7, q_level(o[2], 3) * 85);
    }
    return dm->luts[slot].lut;
}

static void prepare_chips(void)
{
    if (!dm && (dm = hal_big_alloc(sizeof *dm)) != NULL) memset(dm, 0, sizeof *dm);
    if (!dm || (!scratch.px && !sprite_alloc(&scratch, CHIP_W, CHIP_H))) return;
    if (!title.px && sprite8_alloc(&title, CHIP_W, CHIP_H)) {
        gfx_fb fb = sprite_fb(&scratch);
        gfx_clear(&fb, F60_BLACK);
        font_draw_centered(&fb, &font_large, CHIP_W / 2, (int)(CHIP_H * 0.5f + font_large.ascent * 0.34f),
                           "Apps", F60_TEXT2);
        sprite8_from(&title, &scratch);
        for (int y = 0; y < (int)(sizeof dm->arc_x / sizeof dm->arc_x[0]); y++) {
            float d = fabsf((float)(y - CHIP_H) - GFX_CY), r = GFX_W * 0.5f;
            dm->arc_x[y] = (int16_t)(CURVE_K * (d < r ? r - sqrtf(r * r - d * d) : r) + 0.5f);
        }
    }
    int n = wos_app_count();
    if (n > MAX_APPS) n = MAX_APPS;
    for (int i = chips_ready; i < n; i++) {
        if (!chips[i].px && !sprite8_alloc(&chips[i], CHIP_W, CHIP_H)) break;
        render_chip(i);
        dm->keys_ok[i] = (uint8_t)depth_keys(i, dm->keys[i], dm->keys_inv[i]);
        chips_ready = i + 1;
    }
}

static void on_start(void)
{
    scroll = 0; vel = 0; dragging = 0;
    prepare_chips();
}

static void on_draw(gfx_fb *fb)
{
    if (fb->rec) { wos_stream_hint(); wos_flat_hint(); }   /* blits of palette chips */
    gfx_clear(fb, F60_BLACK);
    if (!dm) return;
    int n = chips_ready, focus = (int)(scroll / PITCH + 0.5f);
    /* rows within +-3 of the focus cover the whole glass */
    for (int i = focus - 3; i <= focus + 3; i++) {
        if (i < -1 || i >= n) continue;
        float cy = GFX_CY + i * (float)PITCH - scroll;
        int y = (int)floorf(cy - CHIP_H * 0.5f + 0.5f);
        if (y + CHIP_H <= 0 || y >= GFX_H) continue;
        /* depth snaps at half rows: blends between keyframes would leave the
         * RGB332 levels and shift hue (blue has only four) */
        float d = fabsf(cy - GFX_CY) / PITCH;
        int tq = d < 0.5f ? 0 : d < 1.5f ? 8 : 16;
        unsigned key = (unsigned)(i + 2) | (unsigned)tq << 8;
        sprite8_blit(fb, i < 0 ? &title : &chips[i], (GFX_W - CHIP_W) / 2 + dm->arc_x[y + CHIP_H / 2 + CHIP_H], y,
                     chip_lut(i, tq, key), key);
    }
}

static int row_at(int y)
{
    int i = (int)floorf((y - GFX_CY + scroll) / PITCH + 0.5f);
    float cy = GFX_CY + i * (float)PITCH - scroll;
    if (i < 0 || i >= wos_app_count() || fabsf(y - cy) > CHIP_H * 0.5f) return -1;
    return i;
}

static int on_event(const wos_event *ev)
{
    switch (ev->type) {
    case WOS_EV_TOUCH_DOWN:
        last_y = ev->y; dragging = 1; vel = 0; last_t = hal_millis();
        return 1;
    case WOS_EV_TOUCH_MOVE:
        if (dragging) {
            uint32_t now = hal_millis(), dt = now - last_t;
            if (dt < DT_FLOOR) dt = DT_FLOOR;
            float d = (float)(last_y - ev->y);
            if (d > MOVE_DMAX) d = MOVE_DMAX;
            else if (d < -MOVE_DMAX) d = -MOVE_DMAX;
            float m = max_scroll(), over = scroll < 0 ? -scroll : scroll > m ? scroll - m : 0;
            if (over > 0) d /= 1.0f + over / OVER_RESIST;     /* rubber band */
            scroll += d;
            if (scroll < -OVER_MAX) scroll = -OVER_MAX;
            else if (scroll > m + OVER_MAX) scroll = m + OVER_MAX;
            vel = 0.7f * (d * 1000.0f / dt) + 0.3f * vel;
            if (vel > VEL_MAX) vel = VEL_MAX;
            else if (vel < -VEL_MAX) vel = -VEL_MAX;
            last_y = ev->y; last_t = now;
        }
        return 1;
    case WOS_EV_TOUCH_UP:
        dragging = 0;                         /* momentum coasts, then snaps (TICK) */
        return 1;
    case WOS_EV_TAP: {
        int i = row_at(ev->y);
        if (i >= 0) { wos_open_app(i); return 1; }
        return 0;
    }
    case WOS_EV_TICK:
        if (!dragging) {
            float dt = ev->x / 1000.0f, m = max_scroll();
            scroll += vel * dt;
            vel *= 0.88f;
            if (scroll < 0)      { scroll += -scroll * 0.30f; vel = 0; }
            else if (scroll > m) { scroll += (m - scroll) * 0.30f; vel = 0; }
            else if (fabsf(vel) < 40.0f) {    /* settle the nearest chip onto the centre */
                float target = floorf(scroll / PITCH + 0.5f) * PITCH;
                scroll += (target - scroll) * 0.22f;
                if (fabsf(target - scroll) < 0.5f) scroll = target;
                vel = 0;
            }
        }
        return 1;
    default:
        return 0;
    }
}

const wos_app app_stack = { "stack", on_start, on_event, on_draw, F60_BLUE, ICON_NONE };
