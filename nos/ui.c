/* ui.c — NOS design system. See ui.h. */
#include "ui.h"
#include "nos.h"
#include "prefs.h"
#include "gfx_cost.h"
#ifdef NOS_FONT_SHIM          /* until nos/fonts lands: borrow the old core fonts */
#define nf_hero    font_big
#define nf_display font_large
#define nf_title   font_medium
#define nf_body    font_small
#define nf_label   font_small
#define nf_small   font_small
#else
#include "fonts/nos_fonts.h"
#endif
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

const font_t *UI_F_HERO    = &nf_hero;
const font_t *UI_F_DISPLAY = &nf_display;
const font_t *UI_F_TITLE   = &nf_title;
const font_t *UI_F_BODY    = &nf_body;
const font_t *UI_F_LABEL   = &nf_label;
const font_t *UI_F_SMALL   = &nf_small;

#define LABEL_TRACK 2

/* ------------------------------------------------------------ geometry */
int ui_half_width(float y)
{
    float dy = y - UI_CY;
    float r2 = (float)UI_R * UI_R - dy * dy;
    return r2 > 0 ? (int)gfx_fast_sqrt(r2) : 0;
}
void ui_row_span(float y, float margin, int *x0, int *x1)
{
    int hw = ui_half_width(y) - (int)margin;
    if (hw < 0) hw = 0;
    *x0 = UI_CX - hw;
    *x1 = UI_CX + hw;
}

/* --------------------------------------------------------------- colour */
uint16_t ui_mix(uint16_t a, uint16_t b, float t)
{
    if (t <= 0) return a;
    if (t >= 1) return b;
    /* lerp in 8-bit channels, then pack: a grey stays a neutral RGB565 grey (lerping
     * the 5- and 6-bit fields separately tints dark greys green or magenta) */
    int ra = (a >> 11) * 255 / 31, ga = ((a >> 5) & 63) * 255 / 63, ba = (a & 31) * 255 / 31;
    int rb = (b >> 11) * 255 / 31, gb = ((b >> 5) & 63) * 255 / 63, bb = (b & 31) * 255 / 31;
    int r = ra + (int)lroundf((rb - ra) * t), g = ga + (int)lroundf((gb - ga) * t), bl = ba + (int)lroundf((bb - ba) * t);
    /* two neutral greys always mix to a neutral grey: one 5-bit level, green = 2x
     * (truncating each channel separately leaves g one step high: a green cast) */
    int na = (a >> 11) == (a & 31) && (((a >> 5) & 63) >> 1) == (a & 31);
    int nb = (b >> 11) == (b & 31) && (((b >> 5) & 63) >> 1) == (b & 31);
    if (na && nb) {
        int l5 = (r * 31 + 127) / 255;
        return (uint16_t)(l5 << 11 | (l5 * 2) << 5 | l5);
    }
    return GFX_RGB(r, g, bl);
}
uint16_t ui_dim(uint16_t c, float k) { return ui_mix(0, c, k); }

/* ----------------------------------------------------------------- text */
static const font_glyph *glyph(const font_t *f, unsigned code)
{
    int lo = 0, hi = f->nglyphs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].code == code) return &f->glyphs[mid];
        if (f->glyphs[mid].code < code) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

int ui_ascent(const font_t *f) { return f->ascent; }

int ui_text_width(const font_t *f, const char *s, int tracking)
{
    int w = 0, n = 0;
    for (const unsigned char *c = (const unsigned char *)s; *c; c++, n++) {
        const font_glyph *g = glyph(f, *c);
        w += g ? g->xadv : f->line_height / 3;
    }
    if (n > 1) w += tracking * (n - 1);
    return w;
}

static void blit_glyph(gfx_fb *fb, const font_t *f, const font_glyph *g, int x, int y,
                       uint16_t color, int cc0, int cc1)
{
    const uint8_t *bm = f->bitmap + g->bitmap_off;
    int gx = x + g->xoff + fb->ox;
    if (gx >= cc1 || gx + g->w <= cc0) return;
    int col0 = cc0 - gx < 0 ? 0 : cc0 - gx;
    int col1 = cc1 - gx > g->w ? g->w : cc1 - gx;
    int by0 = y + g->yoff + fb->oy - fb->y_base;
    int row0 = by0 < 0 ? -by0 : 0;
    int row1 = by0 + g->h > fb->h ? fb->h - by0 : g->h;
    unsigned n = 0;   /* pixels written (sim cost model only) */
    for (int row = row0; row < row1; row++) {
        uint16_t *dst = gfx_row(fb, by0 + row);
        const uint8_t *brow = &bm[row * g->w];
        for (int col = col0; col < col1; col++) {
            uint8_t a = brow[col];
            if (!a) continue;
            int bx = gx + col;
            dst[bx] = a >= 250 ? color : gfx_blend565(dst[bx], color, a);
            n++;
        }
    }
    GFX_COST_PX(f->line_height >= 96 ? GFX_COST_GLYPH : GFX_COST_TEXT, n);
}

int ui_text(gfx_fb *fb, const font_t *f, int x, int y, const char *s, uint16_t color,
            int align, int tracking)
{
    if (!s || !*s) return 0;
    int w = ui_text_width(f, s, tracking);
    if (align == UI_CENTER) x -= w / 2;
    else if (align == UI_RIGHT) x -= w;
    if (fb->rec) {
        unsigned h = gfx_hash_u(gfx_hash_u(gfx_hash_u(0x7e47u, (unsigned)(uintptr_t)f), color),
                                (unsigned)(x * 4099 + y * 7 + tracking));
        for (const unsigned char *c = (const unsigned char *)s; *c; c++) h = gfx_hash_u(h, *c);
        gfx_rec_cmd(fb, x - 2, y - f->ascent - 2, x + w + 2, y + (f->line_height - f->ascent) + 2, h);
        return w;
    }
    GFX_COST(f->line_height >= 96 ? GFX_COST_GLYPH : GFX_COST_TEXT, 0);   /* the call; glyphs add px */
    /* reject if the text's rows miss this stripe */
    int top = y - f->ascent - 2 + fb->oy - fb->y_base, bot = y + (f->line_height - f->ascent) + 2 + fb->oy - fb->y_base;
    if (bot < 0 || top >= fb->h) return w;
    int cc0 = fb->cx0, cc1 = fb->cx1;
    if (cc1 <= cc0) { cc0 = 0; cc1 = GFX_W; }
    if (cc0 < 0) cc0 = 0;
    if (cc1 > GFX_W) cc1 = GFX_W;
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        const font_glyph *g = glyph(f, *c);
        if (!g) { x += f->line_height / 3 + tracking; continue; }
        blit_glyph(fb, f, g, x, y, color, cc0, cc1);
        x += g->xadv + tracking;
    }
    return w;
}

int ui_label(gfx_fb *fb, int x, int y, const char *s, uint16_t color, int align)
{
    char buf[64];
    int i = 0;
    for (; s[i] && i < 63; i++) buf[i] = (char)toupper((unsigned char)s[i]);
    buf[i] = 0;
    return ui_text(fb, UI_F_LABEL, x, y, buf, color, align, LABEL_TRACK);
}

int ui_label_width(const char *s)
{
    char buf[64];
    int i = 0;
    for (; s[i] && i < 63; i++) buf[i] = (char)toupper((unsigned char)s[i]);
    buf[i] = 0;
    return ui_text_width(UI_F_LABEL, buf, LABEL_TRACK);
}

int ui_text_fit(gfx_fb *fb, const font_t *f, int x, int y, const char *s, uint16_t color,
                int align, int max_w)
{
    if (ui_text_width(f, s, 0) <= max_w) return ui_text(fb, f, x, y, s, color, align, 0);
    char buf[128];
    int n = (int)strlen(s);
    if (n > 120) n = 120;
    int ell = ui_text_width(f, "...", 0);
    while (n > 0) {
        memcpy(buf, s, (size_t)n); buf[n] = 0;
        if (ui_text_width(f, buf, 0) + ell <= max_w) break;
        n--;
    }
    while (n > 0 && buf[n - 1] == ' ') n--;
    memcpy(buf + n, "...", 4);
    return ui_text(fb, f, x, y, buf, color, align, 0);
}

int ui_text_wrap(gfx_fb *fb, const font_t *f, int x, int y, int max_w, int line_h,
                 int max_lines, const char *s, uint16_t color, int align)
{
    int lines = 0;
    const char *p = s;
    char buf[128];
    while (*p && lines < max_lines) {
        while (*p == ' ') p++;
        int best = 0, n = 0;
        while (p[n] && p[n] != '\n' && n < 120) {
            n++;
            if (p[n] == ' ' || p[n] == 0 || p[n] == '\n') {
                memcpy(buf, p, (size_t)n); buf[n] = 0;
                if (ui_text_width(f, buf, 0) <= max_w) best = n; else break;
            }
        }
        if (!best) {                       /* one long word: hard break */
            best = n;
            while (best > 1) { memcpy(buf, p, (size_t)best); buf[best] = 0; if (ui_text_width(f, buf, 0) <= max_w) break; best--; }
        }
        memcpy(buf, p, (size_t)best); buf[best] = 0;
        int last = lines == max_lines - 1 && p[best] && p[best + 1];
        if (last) ui_text_fit(fb, f, x, y + lines * line_h, p, color, align, max_w);
        else ui_text(fb, f, x, y + lines * line_h, buf, color, align, 0);
        lines++;
        p += best;
        if (*p == '\n') p++;
        if (last) break;
    }
    return lines;
}

/* ------------------------------------------------------------- surfaces */
static int ifl(float v) { int i = (int)v; return i - (v < (float)i); }
static int icl(float v) { int i = (int)v; return i + (v > (float)i); }

/* signed distance to a rounded box (negative inside) */
static float rbox_sdf(float px, float py, float cx, float cy, float hx, float hy, float r)
{
    float qx = fabsf(px - cx) - (hx - r), qy = fabsf(py - cy) - (hy - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float out = (ox > 0 || oy > 0) ? gfx_fast_sqrt(ox * ox + oy * oy) : 0;
    float in = qx > qy ? qx : qy;
    if (in > 0) in = 0;
    return out + in - r;
}
static float cov_of(float sdf) { float c = 0.5f - sdf; return c <= 0 ? 0 : c >= 1 ? 1 : c; }

/* left boundary of a rounded box at row-coordinate yy (inside [y, y+h]) */
static float rbox_left(float x, float y, float h, float r, float yy)
{
    float d = 0;
    if (yy < y + r) d = (y + r) - yy;
    else if (yy > y + h - r) d = yy - (y + h - r);
    else return x;
    if (d >= r) return x + r;
    return x + r - gfx_fast_sqrt(r * r - d * d);
}

static void clip_cols(const gfx_fb *fb, int *c0, int *c1)
{
    int a = fb->cx0, b = fb->cx1;
    if (b <= a) { a = 0; b = GFX_W; }
    if (a < 0) a = 0;
    if (b > GFX_W) b = GFX_W;
    *c0 = a; *c1 = b;
}

static inline int put(uint16_t *row, int bx, uint16_t color, float cov)
{
    if (cov <= 0.004f) return 0;
    if (cov >= 0.996f) row[bx] = color;
    else row[bx] = gfx_blend565(row[bx], color, (uint8_t)(cov * 255.0f));
    return 1;   /* pixel written (sim cost model) */
}

static void fill_run(uint16_t *p, int n, uint16_t color)
{
    for (int i = 0; i < n; i++) p[i] = color;
}

void ui_rrect(gfx_fb *fb, float x, float y, float w, float h, float r, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    if (r < 0) r = 0;
    int bx0 = ifl(x), by0 = ifl(y), bx1 = icl(x + w), by1 = icl(y + h);
    if (fb->rec) {
        unsigned hh = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_u(0x44c1u, color), x), y), w), h), r);
        gfx_rec_cmd(fb, bx0, by0, bx1, by1, hh);
        return;
    }
    int lo = fb->y_base - fb->oy, hi = lo + fb->h;      /* logical rows held */
    if (by0 < lo) by0 = lo;
    if (by1 > hi) by1 = hi;
    if (by1 <= by0) { GFX_COST(GFX_COST_BLEND, 0); return; }
    int c0, c1;
    clip_cols(fb, &c0, &c1);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hx = w * 0.5f, hy = h * 0.5f;
    unsigned nb = 0, nf = 0;   /* AA / solid pixels written (sim cost model only) */
    for (int py = by0; py < by1; py++) {
        uint16_t *row = gfx_row(fb, py + fb->oy - fb->y_base);
        float yt = (float)py, yb = (float)py + 1;
        if (yt < y) yt = y;
        if (yb > y + h) yb = y + h;
        float vcov = yb - yt;
        if (vcov <= 0) continue;
        /* boundary at the top and bottom of this pixel row: the edge band spans both */
        float l0 = rbox_left(x, y, h, r, yt), l1 = rbox_left(x, y, h, r, yb);
        float lmin = l0 < l1 ? l0 : l1, lmax = l0 > l1 ? l0 : l1;
        int el0 = ifl(lmin), el1 = icl(lmax);           /* left AA band [el0, el1) */
        float rmin = x + w - (lmax - x), rmax = x + w - (lmin - x);
        int er0 = ifl(rmin), er1 = icl(rmax);           /* right AA band [er0, er1) */
        if (el1 > er0) { el1 = er0 = (el1 + er0) / 2; } /* tiny shapes: bands meet */
        int full_row = vcov >= 0.999f;
        /* left band */
        for (int px = el0; px < el1; px++) {
            int bx = px + fb->ox;
            if (bx < c0 || bx >= c1) continue;
            float cov = cov_of(rbox_sdf(px + 0.5f, py + 0.5f, cx, cy, hx, hy, r));
            nb += put(row, bx, color, full_row ? cov : cov * vcov);
        }
        /* solid middle */
        int m0 = el1 + fb->ox, m1 = er0 + fb->ox;
        if (m0 < c0) m0 = c0;
        if (m1 > c1) m1 = c1;
        if (m1 > m0) {
            if (full_row) { fill_run(row + m0, m1 - m0, color); nf += m1 - m0; }
            else for (int bx = m0; bx < m1; bx++) nb += put(row, bx, color, vcov);
        }
        /* right band */
        for (int px = er0; px < er1; px++) {
            int bx = px + fb->ox;
            if (bx < c0 || bx >= c1) continue;
            float cov = cov_of(rbox_sdf(px + 0.5f, py + 0.5f, cx, cy, hx, hy, r));
            nb += put(row, bx, color, full_row ? cov : cov * vcov);
        }
    }
    GFX_COST(GFX_COST_BLEND, nb);
    GFX_COST_PX(GFX_COST_FILL, nf);
}

void ui_rrect_line(gfx_fb *fb, float x, float y, float w, float h, float r, float t, uint16_t color)
{
    if (w <= 0 || h <= 0 || t <= 0) return;
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    int bx0 = ifl(x), by0 = ifl(y), bx1 = icl(x + w), by1 = icl(y + h);
    if (fb->rec) {
        unsigned hh = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_u(0x44c2u, color), x), y), w), h), r), t);
        gfx_rec_cmd(fb, bx0, by0, bx1, by1, hh);
        return;
    }
    int lo = fb->y_base - fb->oy, hi = lo + fb->h;
    if (by0 < lo) by0 = lo;
    if (by1 > hi) by1 = hi;
    if (by1 <= by0) { GFX_COST(GFX_COST_BLEND, 0); return; }
    int c0, c1;
    clip_cols(fb, &c0, &c1);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hx = w * 0.5f, hy = h * 0.5f;
    float ri = r - t > 0 ? r - t : 0;
    unsigned nb = 0;   /* pixels written (sim cost model only) */
    for (int py = by0; py < by1; py++) {
        uint16_t *row = gfx_row(fb, py + fb->oy - fb->y_base);
        float pyc = py + 0.5f;
        int band_row = pyc < y + t + 1.5f || pyc > y + h - t - 1.5f;
        float yy = pyc < y ? y : pyc > y + h ? y + h : pyc;
        float lo_x = rbox_left(x, y, h, r, yy);
        int a0 = ifl(lo_x) - 1, a1;
        if (band_row) a1 = icl(x + w) + 1;
        else {
            float li = rbox_left(x + t, y + t, h - 2 * t, ri, yy);
            a1 = icl(li) + 2;
        }
        for (int pass = 0; pass < 2; pass++) {
            int s0, s1;
            if (pass == 0) { s0 = a0; s1 = band_row ? a1 : a1; }
            else {
                if (band_row) break;
                float rr = x + w - (lo_x - x);
                s0 = icl(x + w - ((rbox_left(x + t, y + t, h - 2 * t, ri, yy)) - x)) - 2;
                s1 = icl(rr) + 1;
            }
            for (int px = s0; px < s1; px++) {
                int bx = px + fb->ox;
                if (bx < c0 || bx >= c1) continue;
                float d = rbox_sdf(px + 0.5f, pyc, cx, cy, hx, hy, r);
                float cov = cov_of(d) - cov_of(d + t);
                nb += put(row, bx, color, cov);
            }
        }
    }
    GFX_COST(GFX_COST_BLEND, nb);
}

void ui_circle(gfx_fb *fb, float cx, float cy, float r, uint16_t color)
{
    gfx_fill_circle(fb, cx, cy, r, color);
}
void ui_circle_line(gfx_fb *fb, float cx, float cy, float r, float t, uint16_t color)
{
    gfx_ring(fb, cx, cy, r - t * 0.5f, t, color);
}

/* -------------------------------------------------------------- chrome */
void ui_title(gfx_fb *fb, const char *s, float scroll)
{
    float k = 1.0f - m_clamp(scroll / 60.0f, 0, 1);
    if (k <= 0.02f) return;
    int y = 70 - (int)lroundf((1 - k) * 18.0f);
    ui_label(fb, UI_CX, y, s, ui_dim(C_SEC, k), UI_CENTER);
}

void ui_clock_small(gfx_fb *fb, int y)
{
    hal_datetime dt;
    hal_get_datetime(&dt);
    char b[12];
    ui_fmt_time(b, sizeof b, dt.hour, dt.minute);
    ui_text(fb, UI_F_SMALL, UI_CX, y, b, C_SEC, UI_CENTER, 1);
}

void ui_scrollbar(gfx_fb *fb, float offset, float content, float view, float alpha)
{
    if (alpha <= 0.02f || content <= view + 1) return;
    /* an arc on the right bezel between -38 and +38 degrees around 3 o'clock */
    const float span = 1.33f, a_mid = 1.5708f;
    float frac = view / content;
    if (frac < 0.18f) frac = 0.18f;
    float pos = m_clamp(offset / (content - view), -0.15f, 1.15f);
    float len = span * frac;
    float a0 = a_mid - span * 0.5f + (span - len) * pos;
    float a1 = a0 + len;
    float lo = a_mid - span * 0.5f, hi = a_mid + span * 0.5f;
    if (a0 < lo) a0 = lo;
    if (a1 > hi) a1 = hi;
    if (a1 - a0 < 0.06f) a1 = a0 + 0.06f;
    gfx_arc(fb, UI_CX, UI_CY, UI_R - 9, 5, lo, hi, ui_dim(C_LINE2, alpha));
    gfx_arc(fb, UI_CX, UI_CY, UI_R - 9, 5, a0, a1, ui_dim(C_PRI, alpha));
}

void ui_page_dots_a(gfx_fb *fb, int cx, int y, int n, float pos, float alpha)
{
    if (alpha <= 0.02f) return;
    const float pitch = 14, d = 7;
    float x0 = cx - (n - 1) * pitch * 0.5f;
    for (int i = 0; i < n; i++) {
        float near = 1.0f - m_clamp(fabsf(pos - i), 0, 1);
        dot_disc(fb, x0 + i * pitch, (float)y, d + near * 2.0f, ui_dim(ui_mix(C_LINE2, C_WHITE, near), alpha));
    }
}
void ui_page_dots(gfx_fb *fb, int cx, int y, int n, float pos) { ui_page_dots_a(fb, cx, y, n, pos, 1.0f); }

/* ------------------------------------------------------------ controls */
void ui_toggle(gfx_fb *fb, float cx, float cy, float on)
{
    const float w = 76, h = 44, r = 16;
    on = m_clamp(on, 0, 1);
    uint16_t track = ui_mix(C_LINE2, C_WHITE, on);
    uint16_t thumb = ui_mix(C_DIS, C_BG, on);
    gfx_fill_pill(fb, cx - w / 2 + h / 2, cy, cx + w / 2 - h / 2, h / 2, track);
    float tx = m_lerp(cx - w / 2 + h / 2, cx + w / 2 - h / 2, on);
    gfx_fill_circle(fb, tx, cy, r, thumb);
}

void ui_button(gfx_fb *fb, float cx, float cy, float w, float h, const char *label, int kind, int pressed)
{
    uint16_t fill, ink;
    switch (kind) {
    case BTN_PRIMARY:   fill = pressed ? C_SEC : C_WHITE; ink = C_BG; break;
    case BTN_RED:       fill = pressed ? C_RED_D : C_RED; ink = C_WHITE; break;
    case BTN_DANGER:    fill = pressed ? C_SURF2 : C_BG; ink = C_RED; break;
    case BTN_GHOST:     fill = pressed ? C_SURF2 : C_BG; ink = C_PRI; break;
    default:            fill = pressed ? C_SEC : C_SURF2; ink = pressed ? C_BG : C_PRI; break;   /* C_SURF3 is invisible on the panel */
    }
    if (kind == BTN_DANGER || kind == BTN_GHOST) {
        if (pressed) ui_rrect(fb, cx - w / 2, cy - h / 2, w, h, h / 2, fill);
        ui_rrect_line(fb, cx - w / 2, cy - h / 2, w, h, h / 2, 2, kind == BTN_DANGER ? C_RED : C_LINE2);
    } else
        ui_rrect(fb, cx - w / 2, cy - h / 2, w, h, h / 2, fill);
    int asc = UI_F_LABEL->ascent;
    ui_label(fb, (int)cx, (int)(cy + asc * 0.5f - 2), label, ink, UI_CENTER);
}

void ui_icon_button(gfx_fb *fb, float cx, float cy, float r, int icon, int on, int pressed)
{
    uint16_t fill = on ? (pressed ? C_SEC : C_WHITE) : (pressed ? C_SEC : C_SURF2);
    uint16_t ink = on || pressed ? C_BG : C_PRI;
    gfx_fill_circle(fb, cx, cy, r, fill);
    float pitch = r * 0.16f;
    if (pitch < 2.5f) pitch = 2.5f;
    dot_icon(fb, icon, cx, cy, pitch, pitch * 0.8f, ink);
}

void ui_segments(gfx_fb *fb, float x, float y, float w, float h, int n, float value,
                 uint16_t on, uint16_t off)
{
    if (n <= 0) return;
    const float gap = 3;
    float sw = (w - gap * (n - 1)) / n;
    for (int i = 0; i < n; i++) {
        float sx = x + i * (sw + gap);
        /* discrete progress: a segment is on or off, never a sliver */
        float f = value - i >= 0.5f ? 1.0f : 0.0f;
        int x0 = (int)lroundf(sx), x1 = (int)lroundf(sx + sw);
        if (f >= 1) gfx_fill_rect(fb, x0, (int)y, x1 - x0, (int)h, on);
        else if (f <= 0) { if (off) gfx_fill_rect(fb, x0, (int)y, x1 - x0, (int)h, off); }
        else {
            int xm = x0 + (int)lroundf((x1 - x0) * f);
            gfx_fill_rect(fb, x0, (int)y, xm - x0, (int)h, on);
            if (off) gfx_fill_rect(fb, xm, (int)y, x1 - xm, (int)h, off);
        }
    }
}

void ui_tick_ring(gfx_fb *fb, float cx, float cy, float r0, float r1, int n, float w,
                  float lit, uint16_t on, uint16_t off)
{
    float litn = lit * n;
    for (int i = 0; i < n; i++) {
        float a = 6.2831853f * i / n;
        float s = gfx_fast_sin(a), c = gfx_fast_cos(a);
        uint16_t col;
        if (litn >= n && i < litn - n) col = C_RED;        /* overflow lap */
        else if (i < litn) col = on;
        else col = off;
        if (!col) continue;
        gfx_line(fb, cx + s * r0, cy - c * r0, cx + s * r1, cy - c * r1, w, col);
    }
}

void ui_slider(gfx_fb *fb, float x, float y, float w, float h, float value, int icon)
{
    value = m_clamp(value, 0, 1);
    float r = h / 2;
    gfx_fill_pill(fb, x + r, y + r, x + w - r, r, C_SURF2);
    float fw = h + (w - h) * value;
    gfx_fill_pill(fb, x + r, y + r, x + fw - r, r, C_WHITE);
    if (icon) dot_icon(fb, icon, x + r, y + r, h * 0.065f + 1.0f, (h * 0.065f + 1.0f) * 0.8f, C_BG);
}

void ui_value(gfx_fb *fb, float cx, float y, const char *value, const char *unit,
              float pitch, uint16_t color)
{
    dot_style st = { DOT_FONT_TALL, pitch, pitch * 0.8f, color, 0, 1, 1 };
    dot_text_a(fb, &st, cx, y, value, 1);
    if (unit && *unit) ui_label(fb, (int)cx, (int)(y + dot_text_height(&st) + 26), unit, C_SEC, UI_CENTER);
}

void ui_absent(gfx_fb *fb, int icon, const char *head, const char *body, const char *footer, float p)
{
    p = m_clamp(p, 0, 1);
    if (icon) dot_icon_anim(fb, icon, UI_CX, 170, 4.2f, 3.4f, C_DIS, m_clamp(p / 0.7f, 0, 1), DOT_REVEAL_CENTER);
    float k = floorf(m_clamp((p - 0.3f) / 0.7f, 0, 1) * 8.0f + 0.5f) / 8.0f;   /* text follows the icon */
    if (k <= 0) return;
    if (head && *head) ui_text(fb, UI_F_TITLE, UI_CX, 270, head, ui_dim(C_PRI, k), UI_CENTER, 0);
    if (body && *body) ui_text_wrap(fb, UI_F_BODY, UI_CX, 306, 330, 32, 2, body, ui_dim(C_SEC, k), UI_CENTER);
    if (footer && *footer) ui_label(fb, UI_CX, 398, footer, ui_dim(C_DIS, k), UI_CENTER);
}

int ui_label_unit(gfx_fb *fb, int x, int y, const char *value, const char *unit, uint16_t color, int align)
{
    char v[32];
    int i = 0;
    for (; value && value[i] && i < 31; i++) v[i] = (char)toupper((unsigned char)value[i]);
    v[i] = 0;
    int wv = ui_text_width(UI_F_LABEL, v, LABEL_TRACK);
    int gap = unit && *unit ? 8 : 0;
    int wu = unit && *unit ? ui_text_width(UI_F_LABEL, unit, LABEL_TRACK) : 0;
    int w = wv + gap + wu;
    int x0 = align == UI_CENTER ? x - w / 2 : align == UI_RIGHT ? x - w : x;
    ui_text(fb, UI_F_LABEL, x0, y, v, color, UI_LEFT, LABEL_TRACK);
    if (wu) ui_text(fb, UI_F_LABEL, x0 + wv + gap, y, unit, color, UI_LEFT, LABEL_TRACK);
    return w;
}

void ui_state(gfx_fb *fb, int cx, int y, const char *s, uint16_t color)
{
    char b[48];
    snprintf(b, sizeof b, "[ %s ]", s);
    ui_label(fb, cx, y, b, color, UI_CENTER);
}

/* ---------------------------------------------------------------- list */
void ui_list_init(ui_list *l, int n, int row_h, int top, ui_row_fn draw_row, void (*on_tap)(int))
{
    memset(l, 0, sizeof *l);
    l->n = n; l->row_h = row_h; l->top = top; l->bottom_pad = 90;
    l->draw_row = draw_row; l->on_tap = on_tap;
    l->pressed = -1; l->margin = 26;
    float max = (float)(top + n * row_h + l->bottom_pad - GFX_H);
    msc_init(&l->sc, 0, max > 0 ? max : 0, 0);
}
void ui_list_set_n(ui_list *l, int n)
{
    l->n = n;
    float max = (float)(l->top + n * l->row_h + l->bottom_pad - GFX_H);
    msc_bounds(&l->sc, 0, max > 0 ? max : 0);
}
float ui_list_offset(const ui_list *l) { return l->sc.x; }
int ui_list_at_top(const ui_list *l) { return l->sc.x <= 0.5f; }

static int row_at(const ui_list *l, int y)
{
    float fy = y + l->sc.x - l->top;
    if (fy < 0) return -1;
    int i = (int)(fy / l->row_h);
    return i < l->n ? i : -1;
}

int ui_list_input(ui_list *l, const nos_input *in)
{
    static int s_was_moving;
    switch (in->type) {
    case IN_DOWN:
        s_was_moving = l->sc.moving && fabsf(l->sc.v) > 60.0f;
        msc_grab(&l->sc);
        l->dragging = 0;
        l->pressed = s_was_moving ? -1 : row_at(l, in->y);
        l->press_t = nos_t;
        return 1;
    case IN_DRAG_START:
        if (in->axis != AXIS_Y) { l->pressed = -1; msc_release(&l->sc, 0); return 0; }
        l->pressed = -1;
        l->dragging = 1;
        msc_drag(&l->sc, in->dy);
        l->bar_t = nos_t;
        return 1;
    case IN_DRAG:
        if (!l->dragging) return 0;
        msc_drag(&l->sc, in->dy);
        l->bar_t = nos_t;
        return 1;
    case IN_DRAG_END:
        if (!l->dragging) return 0;
        l->dragging = 0;
        msc_release(&l->sc, -in->vy);
        l->bar_t = nos_t;
        return 1;
    case IN_TAP: {
        int was = s_was_moving;
        msc_release(&l->sc, 0);
        int i = row_at(l, in->y);
        l->pressed = -1;
        if (!was && i >= 0 && l->on_tap) { nos_feedback(FB_TAP); l->on_tap(i); }
        return 1;
    }
    case IN_UP:
        if (l->sc.dragging) msc_release(&l->sc, 0);   /* a fling was already released by DRAG_END */
        l->pressed = -1;
        return 1;
    default:
        return 0;
    }
}

void ui_list_tick(ui_list *l, float dt)
{
    /* the bar shows only while the content actually moves (a resting tap must not flash it) */
    if (msc_step(&l->sc, dt) && (l->dragging || l->sc.moving)) l->bar_t = nos_t;
    float idle = nos_t - l->bar_t;
    float target = idle < 0.6f ? 1.0f : 0.0f;
    /* linear fade (in 0.1 s, out 0.3 s) then quantise: an exponential fade rounded to
     * 1/16 steps can stall short of zero */
    float step = dt * (target > l->bar_alpha ? 10.0f : 3.3f);
    if (target > l->bar_alpha) l->bar_alpha = l->bar_alpha + step > target ? target : l->bar_alpha + step;
    else l->bar_alpha = l->bar_alpha - step < target ? target : l->bar_alpha - step;
    l->bar_alpha = floorf(l->bar_alpha * 16.0f + 0.5f) / 16.0f;
}

void ui_list_draw(gfx_fb *fb, ui_list *l)
{
    float off = floorf(l->sc.x + 0.5f);
    for (int i = 0; i < l->n; i++) {
        int y = l->top + i * l->row_h - (int)off;
        if (y + l->row_h < 0) continue;
        if (y > GFX_H) break;
        float yc = y + l->row_h * 0.5f;
        float d = fabsf(yc - UI_CY);
        /* rows dim from full at 140 px from centre to 30% at the bezel */
        float fade = 1.0f - 0.7f * m_smoothstep(140.0f, 230.0f, d);
        fade = floorf(fade * 16.0f + 0.5f) / 16.0f;   /* quantised: stable damage */
        float edge_y = yc < UI_CY ? (float)y : (float)(y + l->row_h);
        int x0, x1;
        ui_row_span(edge_y, (float)l->margin, &x0, &x1);
        if (x1 - x0 < 60) continue;
        l->draw_row(fb, i, x0, y, x1, l->row_h, fade, i == l->pressed);
    }
    float content = (float)(l->top + l->n * l->row_h + l->bottom_pad);
    ui_scrollbar(fb, l->sc.x, content, GFX_H, l->bar_alpha);
}

/* ----------------------------------------------------- engine overlays */
void ui_draw_banner(gfx_fb *fb, const char *title, const char *body, int icon, float y)
{
    /* top 52 / width 300 keeps the rounded corners inside the glass; two body lines */
    const float w = 300, h = 136, top = UI_BANNER_TOP + y;
    float x = UI_CX - w / 2;
    ui_rrect(fb, x, top, w, h, 40, C_SURF2);
    float iy = top + 34;
    dot_icon(fb, icon ? icon : DOT_ICON_BELL, x + 42, iy, 2.6f, 2.1f, C_WHITE);   /* never a red */
    char t[40];
    int n = 0;
    for (; title[n] && n < 39; n++) t[n] = (char)toupper((unsigned char)title[n]);
    t[n] = 0;
    /* fit the caps title to the card with an ellipsis */
    const int maxw = (int)w - 64 - 30;
    while (n > 1 && ui_text_width(UI_F_LABEL, t, LABEL_TRACK) > maxw) { t[--n] = 0; if (n > 3) memcpy(t + n - 3, "...", 4); }
    ui_text(fb, UI_F_LABEL, (int)(x + 64), (int)(iy + 6), t, C_SEC, UI_LEFT, LABEL_TRACK);
    ui_text_wrap(fb, UI_F_BODY, (int)(x + 30), (int)(top + 82), (int)w - 60, 30, 2, body, C_PRI, UI_LEFT);
}

void ui_draw_toast(gfx_fb *fb, const char *text, float t)
{
    float k = t < 0.12f ? t / 0.12f : t > 0.82f ? (1.0f - t) / 0.18f : 1.0f;
    k = m_clamp(k, 0, 1);
    k = floorf(k * 8.0f + 0.5f) / 8.0f;
    if (k <= 0) return;
    /* states read as bracketed caps text: "[ SAVED ]" */
    char b[52];
    int i = 0, o = 0;
    while (text[i] == '[' || text[i] == ' ') i++;
    b[o++] = '['; b[o++] = ' ';
    for (; text[i] && o < 46; i++) b[o++] = (char)toupper((unsigned char)text[i]);
    while (o > 2 && (b[o - 1] == ']' || b[o - 1] == ' ')) o--;
    b[o++] = ' '; b[o++] = ']'; b[o] = 0;
    int tw = ui_text_width(UI_F_LABEL, b, LABEL_TRACK);
    float w = tw + 40.0f, h = 42;
    if (w > 290) w = 290;                     /* the chord at the top of the glass */
    float y = 30 - (1 - k) * 8;               /* the top: never over the thumb-zone actions */
    ui_rrect(fb, UI_CX - w / 2, y, w, h, h / 2, ui_dim(C_WHITE, k));
    ui_text(fb, UI_F_LABEL, UI_CX, (int)(y + h / 2 + UI_F_LABEL->ascent * 0.5f - 2), b, C_BG, UI_CENTER, LABEL_TRACK);
}

/* --------------------------------------------------------- formatting */
void ui_fmt_time(char *out, int n, int h, int m)
{
    if (!nos_prefs.hour24) { h %= 12; if (h == 0) h = 12; }
    snprintf(out, (size_t)n, "%02d:%02d", h, m);
}
void ui_fmt_dur(char *out, int n, uint32_t ms, int tenths)
{
    uint32_t s = ms / 1000, h = s / 3600, m = (s / 60) % 60;
    s %= 60;
    if (h) snprintf(out, (size_t)n, "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
    else if (tenths) snprintf(out, (size_t)n, "%02u:%02u.%u", (unsigned)m, (unsigned)s, (unsigned)((ms / 100) % 10));
    else snprintf(out, (size_t)n, "%02u:%02u", (unsigned)m, (unsigned)s);
}
const char *ui_weekday3(int wd)
{
    static const char *d[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    return d[(wd % 7 + 7) % 7];
}
const char *ui_month3(int m)
{
    static const char *n[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
    return n[((m - 1) % 12 + 12) % 12];
}
