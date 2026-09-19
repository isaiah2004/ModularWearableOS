/* app_music.c — "Now Playing" UI mockup.
 *
 * IMPORTANT: this board (Waveshare ESP32-S3-Touch-AMOLED-1.43) has NO
 * speaker and NO audio DAC. Nothing here makes sound. This is a pure UI
 * mockup: the transport controls, progress and rotating "album art" all
 * animate against a *simulated* playback clock. See the HAL wish at the
 * bottom of this file (an audio-output contract) if this ever grows teeth.
 *
 * ---------------------------------------------------------------------------
 * PLAIN-ENGLISH ALGORITHM
 * ---------------------------------------------------------------------------
 * State (a tiny playlist "player"):
 *   - playlist[]   : 4 fixed tracks {title, artist, length_seconds, color}.
 *   - s_track      : which track is selected (0..3).
 *   - s_playing    : 1 while the simulated clock runs, 0 when paused.
 *   - s_pos        : seconds elapsed into the current track (float, smooth).
 *   - s_angle      : rotation of the album disc, in radians.
 *
 * Time / position advance (driven by WOS_EV_TICK; ev->x = ms since last frame):
 *   - If paused, do nothing (clock frozen, disc frozen).
 *   - If playing, add dt to the position and spin the disc a little.
 *   - When the position runs past the track length, carry the remainder over
 *     and AUTO-ADVANCE to the next track (wrapping 3 -> 0), so playback never
 *     "stops" at the end — it rolls into the next song like a real player.
 *
 * Progress:
 *   - fraction = s_pos / length; a background ring is drawn full circle and a
 *     foreground arc sweeps 0..fraction*2PI clockwise from 12 o'clock.
 *
 * Controls (hit-tested in on_event on WOS_EV_TAP against three circles):
 *   - PREV  : if we're >3s into the song, restart it (watchOS behaviour);
 *             otherwise jump to the previous track. Position resets to 0.
 *   - PLAY  : toggles s_playing (draws a play triangle or pause bars).
 *   - NEXT  : jump to the next track; position resets to 0.
 *   Switching tracks keeps the current play/pause state.
 *
 * Rendering (on_draw redraws everything, cheap):
 *   clear -> progress ring+arc -> rotating album disc (groove + orbiting
 *   accent dots + music glyph) -> title/artist -> elapsed/total -> buttons.
 * ---------------------------------------------------------------------------
 */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include "../hal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265f

/* ---- playlist state machine -------------------------------------------- */

typedef struct {
    const char *title;
    const char *artist;
    int         len;    /* track length, seconds */
    uint16_t    color;  /* album-art tint */
} track;

#define NTRACKS 4
static const track playlist[NTRACKS] = {
    { "Midnight City", "M83",              241, GFX_PURPLE },
    { "Redbone",       "Childish Gambino", 327, GFX_PINK   },
    { "Nightcall",     "Kavinsky",         258, GFX_ACCENT },
    { "Instant Crush", "Daft Punk",        337, GFX_ORANGE },
};

static int   s_track;         /* current track index */
static int   s_playing;       /* 1 = clock running */
static float s_pos;           /* seconds into current track */
static float s_angle;         /* album-disc rotation, radians */

/* One full disc revolution every ~6s while playing. */
#define ROT_SPEED (2.0f * PI / 6000.0f)   /* radians per millisecond */

/* Format seconds as "m:ss" (no zero pad on minutes, like a phone player). */
static void fmt_time(char *buf, int size, int secs)
{
    if (secs < 0) secs = 0;
    snprintf(buf, (size_t)size, "%d:%02d", secs / 60, secs % 60);
}

/* Advance the simulated player by dt_ms. Pure logic (no rendering / HAL),
 * so it is exercised directly by the self-test below. */
static void advance(int dt_ms)
{
    if (!s_playing || dt_ms <= 0)
        return;
    s_pos   += dt_ms / 1000.0f;
    s_angle  = fmodf(s_angle + dt_ms * ROT_SPEED, 2.0f * PI);
    /* carry over any overshoot and roll into the next track(s) */
    while (s_pos >= (float)playlist[s_track].len) {
        s_pos  -= (float)playlist[s_track].len;
        s_track = (s_track + 1) % NTRACKS;
    }
}

#ifndef SELFTEST
/* ======================================================================== */
/*  WOS-facing code (drawing + events)                                       */
/* ======================================================================== */

/* ---- layout, authored directly in 466px panel space ---------------------- */
#define R_OUT     (GFX_W * 0.5f)                    /* 233: panel radius */
#define COV_SIDE  250                               /* album square side (smaller -> more breathing room) */
#define COV_RAD   100.0f                            /* cover corner radius */
#define RING_TH   25.0f                             /* thick enough for time text to sit inside it */
#define RING_R    (R_OUT - 5.0f - RING_TH * 0.5f)   /* ~215: bar sits ~5px off the rim */
#define ARC_SWEEP (0.80f * 2.0f * PI)               /* bar spans 80% of the circumference */
#define PLAY_A    PI                                /* playhead PINNED at 6 o'clock; the ring rotates */
#define LBL_R     (RING_R - RING_TH * 0.5f - 12.0f) /* time labels: fully INSIDE the bar, off it */
#define TXT_R     164.0f                            /* artist/song banner, further in (room for a white pill) */

/* point on a circle: angle 0 = 12 o'clock, clockwise (matches gfx + circ_text). */
static void polar(float R, float a, float *x, float *y)
{
    *x = GFX_CX + R * gfx_fast_sin(a);
    *y = GFX_CY - R * gfx_fast_cos(a);
}
/* smallest absolute angular distance between a and b */
static float angdist(float a, float b)
{
    float d = a - b;
    while (d >  PI) d -= 2.0f * PI;
    while (d < -PI) d += 2.0f * PI;
    return d < 0 ? -d : d;
}
/* black or white, whichever reads better on an RGB565 background */
static uint16_t contrast_on(uint16_t c)
{
    int r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
    int y = (r * 299) / 31 + (g * 587) / 63 + (b * 114) / 31;   /* ~0..1000 luma */
    return (y > 530) ? GFX_BLACK : GFX_WHITE;
}

/* Copy a run of RGB565 pixels to logical row (lx,ly), respecting the band + x clip.
 * Straight array writes (no float) so it stays cheap even re-run per damage band. */
static void blit_row(gfx_fb *fb, int lx, int ly, const uint16_t *src, int n)
{
    int y = ly + fb->oy - fb->y_base;
    if ((unsigned)y >= (unsigned)fb->h) return;
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    uint16_t *row = &fb->px[y * GFX_W];
    for (int i = 0; i < n; i++) {
        int x = lx + i + fb->ox;
        if ((unsigned)x >= GFX_W || x < c0 || x >= c1) continue;
        row[x] = src[i];
    }
}

/* Album cover: a rounded square. Blits the real album bitmap (streamed over Wi-Fi) when
 * present, else a FLAT placeholder (no gradient -> no RGB565 banding). Filled ONE SPAN
 * PER ROW with one sqrt/row for the corner curve — a per-pixel SDF here cost ~1s/frame
 * on the device. Recorded as ONE command; its hash folds in the cover generation so a
 * newly-arrived image repaints. */
static void draw_cover(gfx_fb *fb, uint16_t tint, unsigned ver)
{
    int cx = (int)GFX_CX, cy = (int)GFX_CY, half = COV_SIDE / 2, r = (int)COV_RAD;
    int apx = 0; unsigned gen = 0;
    const uint16_t *art = hal_media_cover(&apx, &gen);
    if (art && apx != COV_SIDE) art = NULL;             /* size must match for a 1:1 blit */
    unsigned h = ver ^ (art ? (0x9e3779b9u + gen * 2654435761u) : 0u);
    if (gfx_rec_cmd(fb, cx - half - 1, cy - half - 1, cx + half + 2, cy + half + 2, h))
        return;
    uint16_t flat = gfx_blend565(tint, GFX_BLACK, 96);
    for (int y = -half; y <= half; y++) {
        int ay = y < 0 ? -y : y, xw = half;
        if (ay > half - r) {                            /* inside a corner: curve the edge */
            int dy = ay - (half - r), v = r * r - dy * dy;
            xw = (half - r) + (v > 0 ? (int)gfx_fast_sqrt((float)v) : 0);
        }
        if (art) blit_row(fb, cx - xw, cy + y, &art[(y + half) * COV_SIDE + (half - xw)], 2 * xw + 1);
        else     gfx_fill_rect(fb, cx - xw, cy + y, 2 * xw + 1, 1, flat);
    }
}

/* small filled triangle: tip at (tx,ty), pointing along unit (nx,ny) */
static void arrow_tri(gfx_fb *fb, float tx, float ty, float nx, float ny,
                      float len, float wid, uint16_t col)
{
    float bx = tx - nx * len, by = ty - ny * len;   /* base centre */
    float px = -ny, py = nx;                          /* perpendicular */
    float x0 = bx + px * wid, y0 = by + py * wid;
    float x1 = bx - px * wid, y1 = by - py * wid;
    int miny = (int)fminf(ty, fminf(y0, y1)) - 1, maxy = (int)fmaxf(ty, fmaxf(y0, y1)) + 1;
    int minx = (int)fminf(tx, fminf(x0, x1)) - 1, maxx = (int)fmaxf(tx, fmaxf(x0, x1)) + 1;
    unsigned h = gfx_hash_f(gfx_hash_f(2166136261u, tx), ty);
    if (gfx_rec_cmd(fb, minx, miny, maxx + 1, maxy + 1, h)) return;   /* record as one unit */
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            float d0 = (x - x1) * (ty - y1) - (tx - x1) * (y - y1);
            float d1 = (x - tx) * (y0 - ty) - (x0 - tx) * (y - ty);
            float d2 = (x - x0) * (y1 - y0) - (x1 - x0) * (y - y0);
            int neg = (d0 < 0) || (d1 < 0) || (d2 < 0);
            int pos = (d0 > 0) || (d1 > 0) || (d2 > 0);
            if (!(neg && pos)) gfx_px_blend(fb, x, y, col, 255);
        }
}

/* ---- transport glyphs drawn around the cover so each tap zone is legible ---- */
static void tp_play(gfx_fb *fb, float x, float y, float s, uint16_t c)
{ arrow_tri(fb, x + s * 0.55f, y, 1, 0, s * 1.1f, s * 0.78f, c); }
static void tp_pause(gfx_fb *fb, float x, float y, float s, uint16_t c)
{
    gfx_fill_rect(fb, (int)(x - s * 0.5f), (int)(y - s * 0.65f), (int)(s * 0.32f), (int)(s * 1.3f), c);
    gfx_fill_rect(fb, (int)(x + s * 0.18f), (int)(y - s * 0.65f), (int)(s * 0.32f), (int)(s * 1.3f), c);
}
static void tp_prev(gfx_fb *fb, float x, float y, float s, uint16_t c)
{ arrow_tri(fb, x - s * 0.15f, y, -1, 0, s * 0.7f, s * 0.6f, c);
  arrow_tri(fb, x + s * 0.55f, y, -1, 0, s * 0.7f, s * 0.6f, c); }
static void tp_next(gfx_fb *fb, float x, float y, float s, uint16_t c)
{ arrow_tri(fb, x + s * 0.15f, y, 1, 0, s * 0.7f, s * 0.6f, c);
  arrow_tri(fb, x - s * 0.55f, y, 1, 0, s * 0.7f, s * 0.6f, c); }
static void tp_replay(gfx_fb *fb, float x, float y, float s, uint16_t c)
{
    /* circular arrow: a ~300-deg ring with a small arrowhead at its top opening */
    gfx_arc(fb, x, y, s * 0.85f, s * 0.24f, 0.7f, 2.0f * PI - 0.2f, c);
    float a = 0.7f, hx = x + s * 0.85f * gfx_fast_sin(a), hy = y - s * 0.85f * gfx_fast_cos(a);
    arrow_tri(fb, hx, hy - s * 0.05f, -gfx_fast_cos(a), -gfx_fast_sin(a), s * 0.5f, s * 0.42f, c);
}

/* ---- curved (rotated-glyph) text — local copy; the launcher's is static ---- */
static const font_glyph *find_glyph(const font_t *f, int code)
{
    for (int i = 0; i < f->nglyphs; i++)
        if (f->glyphs[i].code == code) return &f->glyphs[i];
    return 0;
}
/* baseline radius: push the baseline in from the visual-centre radius R by ~0.42*ascent so
 * capitals straddle R instead of floating. */
static float glyph_base_R(const font_t *f, float R) { return R - (float)f->ascent * 0.42f; }

/* One glyph laid tangent to the arc at angle `phi`, its BASELINE on the arc (so letters share a
 * smooth baseline via yoff) and BILINEAR-sampled so the rotation stays smooth instead of jaggy. */
static void blit_glyph_rot(gfx_fb *fb, const font_t *f, int ch, float R, float phi, uint16_t col)
{
    const font_glyph *g = find_glyph(f, ch);
    if (!g || g->w == 0 || g->h == 0) return;
    const uint8_t *bm = f->bitmap + g->bitmap_off;
    int w = g->w, h = g->h;
    float Rb = glyph_base_R(f, R);
    float cph = gfx_fast_cos(phi), sph = gfx_fast_sin(phi);
    float Px = GFX_CX + Rb * sph, Py = GFX_CY - Rb * cph;   /* baseline point at glyph centre */
    float halfadv = g->xadv * 0.5f;
    float pcx = (g->xoff + w * 0.5f) - halfadv, pcy = g->yoff + h * 0.5f;   /* bitmap centre in pen space */
    float scx = Px + pcx * cph - pcy * sph, scy = Py + pcx * sph + pcy * cph;
    float ext = 0.5f * gfx_fast_sqrt((float)(w * w + h * h)) + 2.0f;
    for (int oy = (int)(scy - ext); oy <= (int)(scy + ext); oy++)
        for (int ox = (int)(scx - ext); ox <= (int)(scx + ext); ox++) {
            float dx = ox + 0.5f - Px, dy = oy + 0.5f - Py;
            float pxv =  dx * cph + dy * sph;      /* pen x from box centre  */
            float pyv = -dx * sph + dy * cph;      /* pen y from baseline     */
            float fbx = pxv + halfadv - g->xoff, fby = pyv - g->yoff;
            if (fbx < -0.5f || fbx > w - 0.5f || fby < -0.5f || fby > h - 0.5f) continue;
            int x0 = (int)floorf(fbx), y0 = (int)floorf(fby), x1 = x0 + 1, y1 = y0 + 1;
            float tx = fbx - x0, ty = fby - y0;
            #define SMP(xx,yy) (((xx) < 0 || (xx) >= w || (yy) < 0 || (yy) >= h) ? 0.0f : (float)bm[(yy) * w + (xx)])
            float a = (SMP(x0,y0) * (1 - tx) + SMP(x1,y0) * tx) * (1 - ty)
                    + (SMP(x0,y1) * (1 - tx) + SMP(x1,y1) * tx) * ty;
            #undef SMP
            if (a > 3.0f) gfx_px_blend(fb, ox, oy, col, (uint8_t)(a + 0.5f));
        }
}
/* screen bbox of one arc glyph (for per-glyph damage records) */
static void glyph_rot_bbox(const font_t *f, int ch, float R, float phi, int *x0, int *y0, int *x1, int *y1)
{
    const font_glyph *g = find_glyph(f, ch);
    int w = g ? g->w : 0, h = g ? g->h : 0;
    float Rb = glyph_base_R(f, R), cph = gfx_fast_cos(phi), sph = gfx_fast_sin(phi);
    float Px = GFX_CX + Rb * sph, Py = GFX_CY - Rb * cph;
    float halfadv = g ? g->xadv * 0.5f : 0.0f;
    float pcx = ((g ? g->xoff : 0) + w * 0.5f) - halfadv, pcy = (g ? g->yoff : 0) + h * 0.5f;
    float scx = Px + pcx * cph - pcy * sph, scy = Py + pcx * sph + pcy * cph;
    float ext = 0.5f * gfx_fast_sqrt((float)(w * w + h * h)) + 3.0f;
    *x0 = (int)(scx - ext); *y0 = (int)(scy - ext);
    *x1 = (int)(scx + ext); *y1 = (int)(scy + ext);
}
static void circ_draw(gfx_fb *fb, const font_t *f, const char *s, float R, float a_mid, uint16_t col)
{
    float total = 0.0f;
    for (int i = 0; s[i]; i++) { const font_glyph *g = find_glyph(f, (unsigned char)s[i]); total += g ? g->xadv : GFX_S(6); }
    float aw = total / R, a = a_mid - aw * 0.5f;
    for (int i = 0; s[i]; i++) {
        const font_glyph *g = find_glyph(f, (unsigned char)s[i]);
        float adv = (g ? g->xadv : GFX_S(6)) / R, phi = a + adv * 0.5f;
        blit_glyph_rot(fb, f, (unsigned char)s[i], R, phi, col);
        a += adv;
    }
}
/* STATIC curved text (artist/song): painted in the raster pass, persists between damages. */
static void circ_text(gfx_fb *fb, const font_t *f, const char *s, float R, float a_mid, uint16_t col)
{
    if (fb->rec) return;
    circ_draw(fb, f, s, R, a_mid, col);
}
static void circ_text_bbox(const font_t *f, const char *s, float R, float a_mid, int *bx0, int *by0, int *bx1, int *by1)
{
    float total = 0.0f;
    for (int i = 0; s[i]; i++) { const font_glyph *g = find_glyph(f, (unsigned char)s[i]); total += g ? g->xadv : GFX_S(6); }
    float aw = total / R, a = a_mid - aw * 0.5f;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; s[i]; i++) {
        const font_glyph *g = find_glyph(f, (unsigned char)s[i]);
        float adv = (g ? g->xadv : GFX_S(6)) / R, phi = a + adv * 0.5f;
        float x = GFX_CX + R * gfx_fast_sin(phi), y = GFX_CY - R * gfx_fast_cos(phi);
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
        a += adv;
    }
    int pad = (int)f->line_height;
    *bx0 = (int)minx - pad; *by0 = (int)miny - pad; *bx1 = (int)maxx + pad; *by1 = (int)maxy + pad;
}
/* MOVING curved text (the rotating time labels): records a bbox+hash so the damage
 * engine erases the old position and repaints the new one. */
static void circ_textr(gfx_fb *fb, const font_t *f, const char *s, float R, float a_mid, uint16_t col)
{
    int x0, y0, x1, y1;
    circ_text_bbox(f, s, R, a_mid, &x0, &y0, &x1, &y1);
    unsigned h = 2166136261u;
    for (int i = 0; s[i]; i++) h = gfx_hash_u(h, (unsigned)(unsigned char)s[i]);
    h = gfx_hash_f(h, a_mid);
    if (gfx_rec_cmd(fb, x0, y0, x1, y1, h)) return;
    circ_draw(fb, f, s, R, a_mid, col);
}
/* Angular span of a name at radius R (capped to ~100 chars). */
static float name_width(const font_t *f, const char *s, float R)
{
    float total = 0.0f; int n = 0;
    for (; s[n] && n < 100; n++) { const font_glyph *g = find_glyph(f, (unsigned char)s[n]); total += g ? g->xadv : GFX_S(6); }
    return total / R;
}
/* how many leading chars of `s` fit within `maxw` (all of them if the whole name fits). */
static int trunc_count(const font_t *f, const char *s, float R, float maxw)
{
    int full = 0; while (s[full] && full < 100) full++;
    if (name_width(f, s, R) <= maxw) return full;
    float acc = 0; int n = 0;
    for (int i = 0; i < full; i++) {
        const font_glyph *g = find_glyph(f, (unsigned char)s[i]);
        float a = (g ? g->xadv : GFX_S(6)) / R;
        if (acc + a > maxw) break;
        acc += a; n++;
    }
    return n < 1 ? 1 : n;
}
/* Artist/song on a curved palette banner. Shown TRUNCATED to `maxw`; on tap the banner snaps to
 * full width and `reveal` (a CHARACTER COUNT, animated in TICK) fills the letters in one at a time.
 * The banner is a single command (its width in the hash) and the GLYPHS piggyback on it in the
 * raster pass, so recording stays cheap and the count is constant — no structural redraw. The
 * char-by-char reveal is driven by manually dirtying ONLY the newly-lit characters (`wos_dirty`),
 * so each step repaints one glyph, not the whole banner (that per-frame re-raster was the lag). */
static int s_reveal_shown[2];       /* glyphs drawn last frame for [artist, song] */
static void draw_name(gfx_fb *fb, int idx, const font_t *f, const char *s0, float R, float a_mid,
                      float maxw, float reveal, uint16_t bg, uint16_t txt)
{
    char s[104]; int nchars = 0;
    for (; s0[nchars] && nchars < 100; nchars++) s[nchars] = s0[nchars];
    s[nchars] = 0;
    if (nchars == 0) return;
    float bth = (float)f->line_height + 6.0f;
    float adv[104], tw = 0.0f;
    for (int i = 0; i < nchars; i++) {
        const font_glyph *g = find_glyph(f, (unsigned char)s[i]);
        adv[i] = (g ? g->xadv : GFX_S(6)) / R;
        tw += adv[i];
    }
    int fits = (tw <= maxw);
    int trunc_n = trunc_count(f, s, R, maxw);              /* chars visible while collapsed */
    int expanded = (!fits) && (reveal > (float)trunc_n + 0.05f);

    float a0 = a_mid - (fits ? tw : maxw) * 0.5f;          /* left-anchored; grows to the right */
    int show_n; float a1;
    if (fits)           { show_n = nchars; a1 = a0 + tw; }
    else if (!expanded) { show_n = trunc_n; float acc = 0;
                          for (int i = 0; i < trunc_n; i++) acc += adv[i];
                          a1 = a0 + acc; }
    else                { a1 = a0 + tw;
                          show_n = (int)reveal;
                          if (show_n > nchars) show_n = nchars;
                          if (show_n < trunc_n) show_n = trunc_n; }

    /* per-char reveal: dirty just the characters that lit up (or went dark) since last frame, so
     * the banner command can stay static (no full re-raster) while the text fills in. */
    if (fb->rec && show_n != s_reveal_shown[idx]) {
        int lo = show_n < s_reveal_shown[idx] ? show_n : s_reveal_shown[idx];
        int hi = show_n > s_reveal_shown[idx] ? show_n : s_reveal_shown[idx];
        if (lo < 0) lo = 0;
        if (hi > nchars) hi = nchars;
        int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -(1 << 30), by1 = -(1 << 30);
        float a = a0;
        for (int i = 0; i < lo; i++) a += adv[i];
        for (int i = lo; i < hi; i++) {
            int gx0, gy0, gx1, gy1;
            glyph_rot_bbox(f, (unsigned char)s[i], R, a + adv[i] * 0.5f, &gx0, &gy0, &gx1, &gy1);
            if (gx0 < bx0) bx0 = gx0;
            if (gy0 < by0) by0 = gy0;
            if (gx1 > bx1) bx1 = gx1;
            if (gy1 > by1) by1 = gy1;
            a += adv[i];
        }
        if (bx1 > bx0) wos_dirty(bx0, by0, bx1, by1);
        s_reveal_shown[idx] = show_n;
    }

    gfx_arc(fb, GFX_CX, GFX_CY, R, bth, a0, a1, bg);
    { float x, y; polar(R, a0, &x, &y); gfx_fill_circle(fb, x, y, bth * 0.5f, bg);
      polar(R, a1, &x, &y); gfx_fill_circle(fb, x, y, bth * 0.5f, bg); }
    if (fb->rec) return;
    float a = a0;                                          /* glyphs piggyback on the banner */
    for (int i = 0; i < show_n; i++) {
        blit_glyph_rot(fb, f, (unsigned char)s[i], R, a + adv[i] * 0.5f, txt);
        a += adv[i];
    }
}

/* When a phone/PC bridge is streaming now-playing over BLE we show THAT and the
 * cover taps drive the host; otherwise we fall back to the local mockup. */
static int s_live;                 /* 1 = real BLE media this frame */
static char s_ltitle[64], s_lartist[64];
static uint32_t s_show_until;      /* transport controls visible while hal_millis() < this */
static int   s_seeking;            /* 1 while a bezel (rim) drag is scrubbing */
static float s_seek_frac;          /* fraction being scrubbed to */
static float s_seek_a;             /* last touch angle during the drag */
static int   s_screen;             /* 0 = now-playing, 1 = volume (swipe left <-> right) */
static float s_vol = 0.55f;        /* volume 0..1 */
static uint32_t s_vol_sent;        /* last time we pushed volume to the host (throttle) */
static float    s_reveal[2];       /* revealed CHARACTER count for [artist, song] */
static uint32_t s_reveal_until[2]; /* tapped name stays fully expanded while millis < this */
static int      s_name_len[2];     /* full length of [artist, song] this frame (for the reveal) */
static int      s_name_trunc[2];   /* truncated (resting) length of [artist, song] this frame */
#define SEEK_R  128.0f             /* touches beyond this radius = the rim/bezel */
#define NAME_W  1.9f               /* max angular width of a name before it scrolls (rad) */
#define NAME_HIT_R0 136.0f         /* name-banner tap hitbox: radial band ... */
#define NAME_HIT_R1 196.0f
#define NAME_HIT_A  0.90f          /* ... and angular half-width around each banner centre (rad) */

/* Volume screen: a circular knob in the same style as the track ring — a thick 80% arc,
 * accent-filled to the level, with a handle; grab the rim and spin to grow/shrink it. */
static void draw_volume(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);
    float a0 = PLAY_A + (2.0f * PI - ARC_SWEEP) * 0.5f;   /* start at lower-left, gap at bottom */
    float head = a0 + s_vol * ARC_SWEEP;
    uint16_t grey = gfx_blend565(GFX_BLACK, GFX_WHITE, 34);
    const int NSEG = 40; const float dth = ARC_SWEEP / NSEG;
    for (int k = 0; k < NSEG; k++) {
        float amid = a0 + ((float)k + 0.5f) * dth;
        uint16_t col = ((amid - a0) <= s_vol * ARC_SWEEP) ? GFX_ACCENT : grey;
        float x0, y0, x1, y1;
        polar(RING_R, a0 + (float)k * dth, &x0, &y0);
        polar(RING_R, a0 + (float)(k + 1) * dth, &x1, &y1);
        gfx_line(fb, x0, y0, x1, y1, RING_TH, col);
    }
    { float x, y; polar(RING_R, a0, &x, &y); gfx_fill_circle(fb, x, y, RING_TH * 0.5f, GFX_ACCENT);
      polar(RING_R, head, &x, &y); gfx_fill_circle(fb, x, y, RING_TH * 0.6f, GFX_WHITE); }  /* handle */
    /* centre readout: big level (font_big is digits-only, so the "%" is drawn small beside it) */
    char b[8]; snprintf(b, sizeof b, "%d", (int)(s_vol * 100.0f + 0.5f));
    int cy = (int)(GFX_CY - font_big.line_height * 0.5f);
    font_draw_centered(fb, &font_big, (int)GFX_CX - GFX_S(8), cy, b, GFX_WHITE);
    font_draw(fb, &font_medium, (int)GFX_CX + GFX_S(24), cy + GFX_S(6), "%", GFX_ACCENT);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(GFX_CY + GFX_S(42)), "VOLUME", GFX_GREY);
}

static void draw_music(gfx_fb *fb)
{
    uint32_t s_now = hal_millis();
    const track *t = &playlist[s_track];
    int lp = s_playing;
    s_live = hal_media_get(s_ltitle, sizeof s_ltitle, s_lartist, sizeof s_lartist, &lp);
    if (s_live) s_playing = lp;    /* mirror the host's play/pause */

    const char *title  = s_live ? s_ltitle  : t->title;
    const char *artist = s_live ? s_lartist : t->artist;
    uint16_t    tint   = s_live ? GFX_PURPLE : t->color;
    int pos_ms = (int)(s_pos * 1000.0f), dur_ms = t->len * 1000;   /* mock, ms */
    if (s_live) {                                                 /* live: real ms position */
        int lp, ld;
        if (hal_media_time(&lp, &ld)) { pos_ms = lp; dur_ms = ld; }
        else dur_ms = 0;                                          /* connected but no position */
    }
    float frac = (dur_ms > 0) ? (float)pos_ms / (float)dur_ms : 0.0f;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    if (s_seeking && dur_ms > 0) { frac = s_seek_frac; pos_ms = (int)(frac * (float)dur_ms); }  /* scrub */
    int pos = pos_ms / 1000, tlen = dur_ms / 1000;                /* seconds for the text labels */

    gfx_clear(fb, GFX_BLACK);

    /* ROTATING dial: the playhead (arrow + current time) is PINNED at the bottom and
     * the whole bar rotates under it, so grey "remaining" turns green as it passes.
     * The rotation is quantised to ~1px steps, so identical frames record identically
     * and the damage engine repaints only when it truly moves (a few times/sec, not 60). */
    uint16_t ring = GFX_GREEN, artbg = GFX_WHITE, songbg = GFX_WHITE, arttxt = GFX_BLACK, songtxt = GFX_BLACK;
    hal_media_colors(&ring, &artbg, &songbg, &arttxt, &songtxt);   /* palette from the art */
    uint16_t grey = gfx_blend565(GFX_BLACK, GFX_WHITE, 34);   /* very dark remaining track */
    float raw = frac * ARC_SWEEP;
    float qs  = 1.0f / RING_R;                        /* ~1px at the bar radius */
    float rot = qs * (float)((int)(raw / qs + 0.5f)); /* quantised elapsed angle */
    float rlen = ARC_SWEEP - rot;
    float a_zero = PLAY_A - rot;                      /* green tail (grows) */
    float a_len  = PLAY_A + rlen;                     /* grey head (shrinks) */

    /* The bar is a ring of FIXED angular segments, each a stable command: a segment only
     * repaints when a moving end sweeps across its edge (green<->gap or grey<->gap). A
     * sub-segment TIP at each end keeps motion smooth, and the command COUNT is constant
     * every frame -> nothing triggers a structural full-screen redraw. Only the two ends
     * (plus their caps and the rotating labels) are ever dirty. */
    const int   NSEG = 32;
    const float dth  = 2.0f * PI / NSEG;
    int gfull = (int)ceilf(rlen / dth) - 1; if (gfull < 0) gfull = 0;  /* full grey segments */
    int efull = (rot > qs) ? (int)ceilf(rot / dth) - 1 : 0; if (efull < 0) efull = 0;
    /* Each segment is a CHORD (capsule via gfx_line), visually identical to a 7.5-deg arc
     * but with a clean stable bbox and rounded caps that overlap the neighbours. */
    for (int k = 0; k < NSEG; k++) {
        uint16_t col = GFX_BLACK;                     /* gap (drawn to erase + keep count) */
        if (k < gfull) col = grey;                    /* remaining, ahead of the playhead */
        else if (k >= NSEG - efull) col = ring;       /* elapsed (art palette), behind it */
        float x0, y0, x1, y1;
        polar(RING_R, PLAY_A + (float)k * dth, &x0, &y0);
        polar(RING_R, PLAY_A + (float)(k + 1) * dth, &x1, &y1);
        gfx_line(fb, x0, y0, x1, y1, RING_TH, col);
    }
    { float x0, y0, x1, y1;                                     /* grey tip */
      polar(RING_R, PLAY_A + (float)gfull * dth, &x0, &y0);
      polar(RING_R, a_len, &x1, &y1);
      gfx_line(fb, x0, y0, x1, y1, RING_TH, grey); }
    if (rot > qs) {                                            /* green tip */
        float x0, y0, x1, y1;
        polar(RING_R, PLAY_A - (float)efull * dth, &x0, &y0);
        polar(RING_R, a_zero, &x1, &y1);
        gfx_line(fb, x0, y0, x1, y1, RING_TH, ring);
    }

    /* album cover (rounded square). ver hash -> redraws only on track change. */
    unsigned ver = (unsigned)tint * 131u + (unsigned)(unsigned char)title[0] * 7u
                 + (unsigned)s_live;
    draw_cover(fb, tint, ver);

    /* transport controls appear only after a tap (auto-hide): prev left, play centre,
     * next right, REPLAY under play. */
    if (s_now < s_show_until) {
        uint16_t ic = GFX_WHITE;
        float d = 82.0f, ts = 21.0f;
        tp_prev(fb, GFX_CX - d, GFX_CY, ts, ic);
        tp_next(fb, GFX_CX + d, GFX_CY, ts, ic);
        if (s_playing) tp_pause(fb, GFX_CX, GFX_CY - 8.0f, ts, ic);
        else           tp_play(fb, GFX_CX, GFX_CY - 8.0f, ts, ic);
        tp_replay(fb, GFX_CX, GFX_CY + 52.0f, ts * 0.85f, ic);   /* under the play button */
    }

    /* artist/song: text on an art-palette banner, truncated; tap fills the rest in char by char */
    { int an = 0; while (artist[an] && an < 100) an++;   s_name_len[0] = an;
      int tn = 0; while (title[tn]  && tn < 100) tn++;   s_name_len[1] = tn;
      s_name_trunc[0] = trunc_count(&font_small, artist, TXT_R, NAME_W);
      s_name_trunc[1] = trunc_count(&font_small, title,  TXT_R, NAME_W); }
    draw_name(fb, 0, &font_small, artist, TXT_R, -0.82f, NAME_W, s_reveal[0], artbg, arttxt);
    draw_name(fb, 1, &font_small, title,  TXT_R,  2.32f, NAME_W, s_reveal[1], songbg, songtxt);

    /* 0:00 and the track length ride ON the bar itself (inside the arc), white for contrast */
    char buf[16];
    fmt_time(buf, sizeof buf, 0);
    circ_textr(fb, &font_small, buf, RING_R, a_zero, GFX_WHITE);
    if (tlen > 0) {
        fmt_time(buf, sizeof buf, tlen);
        circ_textr(fb, &font_small, buf, RING_R, a_len, GFX_WHITE);
    }
    /* current-time BADGE pinned at the bottom of the arc (the playhead): a small pill on the bar,
     * a touch taller than it so it pops out. Coloured from the art palette (ring) with auto-
     * contrast digits. */
    if (tlen > 0) {
        float hx, hy; polar(RING_R, PLAY_A, &hx, &hy);
        fmt_time(buf, sizeof buf, pos);
        int tw = font_text_width(&font_small, buf), r = (int)(RING_TH * 0.5f) + 5;
        int bx = (int)hx, by = (int)hy;
        uint16_t bgc = ring, fgc = contrast_on(ring);
        gfx_fill_rect(fb, bx - tw / 2, by - r, tw, 2 * r, bgc);
        gfx_fill_circle(fb, bx - tw / 2, by, r, bgc);
        gfx_fill_circle(fb, bx + tw / 2, by, r, bgc);
        font_draw_centered(fb, &font_small, bx, by - font_small.line_height / 2, buf, fgc);
    }
}

/* ---- scan-line crossfade between the two screens. Both are rendered ONCE into PSRAM;
 * a soft ~TR_FADE-wide front sweeps left->right blending B over A. Only the moving front
 * (~72px x 466 ~ 34k px) is redrawn per frame; input is ignored until it lands. ---- */
#define TR_FADE 44
#define TR_STEP 30.0f
static int      s_trans;           /* 1 while sweeping */
static float    s_trans_x;         /* wipe-front x */
static int      s_trans_to;        /* destination screen */
static int      s_trans_dir;       /* +1 sweep left->right, -1 right->left */
static uint16_t *s_bufA, *s_bufB;  /* from / to, full-frame RGB565 in PSRAM */

static void render_screen_to(uint16_t *buf, int screen)
{
    gfx_fb fb;
    fb.px = buf; fb.ox = fb.oy = 0; fb.y_base = 0; fb.h = GFX_H;
    fb.cx0 = 0; fb.cx1 = GFX_W; fb.rec = 0;
    if (screen == 1) draw_volume(&fb); else draw_music(&fb);
}
static void start_trans(int to, int dir)
{
    if (!s_bufA) s_bufA = hal_big_alloc((uint32_t)GFX_W * GFX_H * 2);
    if (!s_bufB) s_bufB = hal_big_alloc((uint32_t)GFX_W * GFX_H * 2);
    if (!s_bufA || !s_bufB) { s_screen = to; return; }   /* no PSRAM -> instant switch */
    render_screen_to(s_bufA, s_screen);
    render_screen_to(s_bufB, to);
    s_trans_to = to; s_trans_dir = dir;
    s_trans_x = (dir > 0) ? 0.0f : (float)GFX_W;
    s_trans = 1;
}
static void draw_transition(gfx_fb *fb)
{
    int x = (int)s_trans_x;
    /* Redraw the FULL width every step: each frame is a complete A/B composite, so the whole
     * screen crossfades on device without relying on prior partial-band DMAs persisting. */
    if (gfx_rec_cmd(fb, 0, 0, GFX_W, GFX_H, gfx_hash_u(0x5ca7u, (unsigned)x)))
        return;
    /* Only the ~TR_FADE-wide soft front is BLENDED; the solid A/B fields on either side are a
     * straight copy. Per-pixel float division across the whole screen was ~150 ms/frame on device;
     * this is a couple of memcpys + a thin blended band. dir>0: left=B, right=A; dir<0: left=A,right=B. */
    int f0, f1;
    if (s_trans_dir > 0) { f0 = x;            f1 = x + TR_FADE; }
    else                 { f0 = x - TR_FADE;  f1 = x;           }
    if (f0 < 0) f0 = 0;
    if (f1 > GFX_W) f1 = GFX_W;
    if (f0 > GFX_W) f0 = GFX_W;
    if (f1 < 0) f1 = 0;
    uint8_t wt[TR_FADE + 1];
    for (int c = f0; c < f1; c++) {
        float al = (s_trans_dir > 0) ? (float)(x + TR_FADE - c) / (float)TR_FADE
                                     : (float)(c - x + TR_FADE) / (float)TR_FADE;
        if (al < 0) al = 0;
        if (al > 1) al = 1;
        wt[c - f0] = (uint8_t)(al * 255.0f);          /* weight of B over A */
    }
    uint16_t *lbuf = (s_trans_dir > 0) ? s_bufB : s_bufA;   /* solid field left of the front  */
    uint16_t *rbuf = (s_trans_dir > 0) ? s_bufA : s_bufB;   /* solid field right of the front */
    for (int y = 0; y < fb->h; y++) {
        int ly = y + fb->y_base;
        uint16_t *arow = &s_bufA[ly * GFX_W], *brow = &s_bufB[ly * GFX_W], *drow = &fb->px[y * GFX_W];
        if (f0 > 0)        memcpy(drow,      &lbuf[ly * GFX_W],      (size_t)f0 * 2);
        for (int c = f0; c < f1; c++) drow[c] = gfx_blend565(arow[c], brow[c], wt[c - f0]);
        if (f1 < GFX_W)    memcpy(drow + f1, &rbuf[ly * GFX_W + f1], (size_t)(GFX_W - f1) * 2);
    }
}
static void on_draw(gfx_fb *fb)
{
    if (s_trans) { draw_transition(fb); return; }
    if (s_screen == 1) draw_volume(fb);
    else               draw_music(fb);
}

/* Transport lives on the cover itself: tap CENTER = play/pause, LEFT = previous,
 * RIGHT = next, TOP = replay current. When live these command the host over BLE;
 * otherwise they drive the local mockup. */
static void do_prev(void)
{
    if (s_live) { hal_media_cmd(2); return; }
    if (s_pos > 3.0f) s_pos = 0.0f;                       /* restart current */
    else { s_track = (s_track + NTRACKS - 1) % NTRACKS; s_pos = 0.0f; }
}
static void do_next(void)
{
    if (s_live) { hal_media_cmd(1); return; }
    s_track = (s_track + 1) % NTRACKS; s_pos = 0.0f;
}
static void do_replay(void)
{
    if (s_live) hal_media_cmd(3);                         /* seek-to-0 on the host */
    else s_pos = 0.0f;
}
static void do_playpause(void)
{
    s_playing = !s_playing;                               /* optimistic feedback */
    if (s_live) hal_media_cmd(0);
}
static float cur_frac(void)
{
    int p, d;
    if (hal_media_time(&p, &d) && d > 0) return (float)p / (float)d;   /* ms ratio */
    const track *t = &playlist[s_track];
    return t->len > 0 ? s_pos / (float)t->len : 0.0f;
}
static void do_seek(float frac)
{
    int p, d;
    if (hal_media_time(&p, &d) && d > 0) hal_media_seek((int)(frac * (float)d / 1000.0f));  /* ms->s */
    else s_pos = frac * (float)playlist[s_track].len;                                        /* mock */
}

static int on_event(const wos_event *ev)
{
    uint32_t now = hal_millis();
    if (ev->type == WOS_EV_TICK) {
        if (s_trans) {                                             /* sweep the crossfade */
            s_trans_x += (float)s_trans_dir * TR_STEP;
            if ((s_trans_dir > 0 && s_trans_x >= (float)GFX_W) ||
                (s_trans_dir < 0 && s_trans_x <= 0.0f)) { s_screen = s_trans_to; s_trans = 0; }
            return 1;
        }
        int lp;
        if (hal_media_get(NULL, 0, NULL, 0, &lp)) s_playing = lp;   /* live: host owns state */
        else advance(ev->x);                                       /* mock: real-time fake clock */
        for (int i = 0; i < 2; i++) {                              /* fill/unfill the name char by char */
            float tgt = (now < s_reveal_until[i]) ? (float)s_name_len[i]     /* full when tapped */
                                                  : (float)s_name_trunc[i];  /* rest at truncated */
            float sp  = (float)ev->x * 0.025f;                     /* ~25 chars / second */
            if (s_reveal[i] < tgt)      { s_reveal[i] += sp; if (s_reveal[i] > tgt) s_reveal[i] = tgt; }
            else if (s_reveal[i] > tgt) { s_reveal[i] -= sp; if (s_reveal[i] < tgt) s_reveal[i] = tgt; }
        }
        return 1;
    }
    if (s_trans) return 1;                                          /* ignore input mid-transition */

    float dx = (float)ev->x - GFX_CX, dy = (float)ev->y - GFX_CY;
    float r = gfx_fast_sqrt(dx * dx + dy * dy);

    /* --- VOLUME screen: reached by swiping the now-playing screen LEFT --- */
    if (s_screen == 1) {
        if (ev->type == WOS_EV_SWIPE_RIGHT) { start_trans(0, +1); return 1; }   /* back, sweep L->R */
        if (ev->type == WOS_EV_TOUCH_DOWN) { if (r > SEEK_R) { s_seeking = 1; s_seek_a = atan2f(dx, -dy); } return 1; }
        if (ev->type == WOS_EV_TOUCH_MOVE) {
            if (s_seeking) {
                float a = atan2f(dx, -dy), da = a - s_seek_a;
                if (da >  PI) da -= 2.0f * PI;
                if (da < -PI) da += 2.0f * PI;
                s_seek_a = a;
                s_vol += da / ARC_SWEEP;                 /* grab + spin to grow/shrink */
                if (s_vol < 0) s_vol = 0;
                if (s_vol > 1) s_vol = 1;
                uint32_t now = hal_millis();             /* stream to the host, throttled */
                if (now - s_vol_sent > 120) { hal_media_vol((int)(s_vol * 100.0f + 0.5f)); s_vol_sent = now; }
            }
            return 1;
        }
        if (ev->type == WOS_EV_TOUCH_UP) {
            if (s_seeking) hal_media_vol((int)(s_vol * 100.0f + 0.5f));   /* land the final level */
            s_seeking = 0; return 1;
        }
        return 0;
    }
    if (ev->type == WOS_EV_SWIPE_LEFT) { start_trans(1, -1); return 1; }   /* -> volume, sweep R->L */

    /* bezel seek: grab the rim and spin it round to scrub the track. BUT a press that lands on
     * either name banner is a name tap, not a seek — don't start scrubbing there. */
    if (ev->type == WOS_EV_TOUCH_DOWN) {
        float ta = atan2f(dx, -dy);
        if (r > NAME_HIT_R0 && r < NAME_HIT_R1 &&
            (angdist(ta, -0.82f) < NAME_HIT_A || angdist(ta, 2.32f) < NAME_HIT_A))
            return 1;                                    /* name banner — handled on TAP, no seek */
        if (r > SEEK_R) { s_seeking = 1; s_seek_a = ta; s_seek_frac = cur_frac(); }
        return 1;
    }
    if (ev->type == WOS_EV_TOUCH_MOVE) {
        if (s_seeking) {
            float a = atan2f(dx, -dy), da = a - s_seek_a;
            if (da >  PI) da -= 2.0f * PI;
            if (da < -PI) da += 2.0f * PI;
            s_seek_a = a;
            s_seek_frac -= da / ARC_SWEEP;               /* rim maps to the 80% arc (spin dir = progress dir) */
            if (s_seek_frac < 0) s_seek_frac = 0;
            if (s_seek_frac > 1) s_seek_frac = 1;
        }
        return 1;
    }
    if (ev->type == WOS_EV_TOUCH_UP) {
        if (s_seeking) { s_seeking = 0; do_seek(s_seek_frac); }
        return 1;
    }
    if (ev->type != WOS_EV_TAP)
        return 0;

    /* tap a name banner -> fill it in to reveal the full text (then it shrinks back) */
    if (r > NAME_HIT_R0 && r < NAME_HIT_R1) {
        float ta = atan2f(dx, -dy);
        if (angdist(ta, -0.82f) < NAME_HIT_A) { s_reveal_until[0] = now + 5000; return 1; }
        if (angdist(ta,  2.32f) < NAME_HIT_A) { s_reveal_until[1] = now + 5000; return 1; }
    }
    if (r > COV_SIDE * 0.5f) return 0;                   /* rim taps belong to the seek zone */
    int visible = now < s_show_until;
    s_show_until = now + 3500;                           /* any cover tap (re)reveals controls */
    if (!visible) return 1;                              /* first tap only reveals */

    if (dy > 22.0f && fabsf(dx) < 42.0f) do_replay();            /* replay, under play */
    else if (dx * dx + dy * dy <= 48.0f * 48.0f) do_playpause(); /* play/pause centre */
    else if (dx < 0) do_prev();                                 /* left  */
    else do_next();                                             /* right */
    return 1;
}

static void on_start(void)
{
    /* Mock demo: start partway into the first track so the progress ring reads as
     * "playing" immediately (real position comes from the phone when live). */
    s_track = 0;
    s_pos = 60.0f;
    s_angle = 0.0f;
    s_playing = 1;
}

const wos_app app_music = { "Music", on_start, on_event, on_draw,
                            GFX_PURPLE, ICON_MUSIC };

#else  /* SELFTEST */
/* ======================================================================== */
/*  Self-test for the mm:ss formatting + position / auto-advance logic.      */
/*  Build & run:  gcc -DSELFTEST -std=c99 app_music.c -o t && ./t            */
/* ======================================================================== */
#include <string.h>

static int fails;

static void expect_time(int secs, const char *want)
{
    char got[16];
    fmt_time(got, sizeof got, secs);
    if (strcmp(got, want) != 0) {
        printf("FAIL fmt_time(%d): got \"%s\" want \"%s\"\n", secs, got, want);
        fails++;
    } else {
        printf("ok   fmt_time(%d) = %s\n", secs, got);
    }
}

int main(void)
{
    /* mm:ss formatting */
    expect_time(0,   "0:00");
    expect_time(5,   "0:05");
    expect_time(65,  "1:05");
    expect_time(241, "4:01");   /* Midnight City */
    expect_time(327, "5:27");   /* Redbone       */
    expect_time(-3,  "0:00");   /* clamp negative */

    /* auto-advance across a track boundary */
    s_track = 0; s_pos = 240.0f; s_playing = 1; s_angle = 0.0f;
    advance(2000);   /* 240 + 2 = 242 >= 241 -> carry 1s into track 1 */
    if (s_track == 1 && fabsf(s_pos - 1.0f) < 0.001f) {
        printf("ok   auto-advance: track=%d pos=%.3f\n", s_track, s_pos);
    } else {
        printf("FAIL auto-advance: track=%d pos=%.3f (want track=1 pos=1.0)\n",
               s_track, s_pos);
        fails++;
    }

    /* paused clock does not move */
    s_playing = 0; s_pos = 10.0f; s_track = 2;
    advance(5000);
    if (s_track == 2 && fabsf(s_pos - 10.0f) < 0.001f) {
        printf("ok   paused clock frozen: pos=%.3f\n", s_pos);
    } else {
        printf("FAIL paused clock moved: pos=%.3f\n", s_pos);
        fails++;
    }

    /* progress fraction stays in [0,1] and matches pos/len */
    s_playing = 1; s_track = 0; s_pos = 0.0f;
    advance(120500);   /* ~120.5s into a 241s track -> ~0.5 */
    {
        float frac = s_pos / (float)playlist[s_track].len;
        if (s_track == 0 && frac > 0.49f && frac < 0.51f) {
            printf("ok   progress fraction = %.3f\n", frac);
        } else {
            printf("FAIL progress fraction = %.3f (want ~0.5)\n", frac);
            fails++;
        }
    }

    printf(fails ? "\n%d TEST(S) FAILED\n" : "\nALL TESTS PASSED\n", fails);
    return fails ? 1 : 0;
}
#endif /* SELFTEST */
