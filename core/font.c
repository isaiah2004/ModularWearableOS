#include "font.h"

static const font_glyph *find_glyph(const font_t *f, unsigned code)
{
    int lo = 0, hi = f->nglyphs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].code == code) return &f->glyphs[mid];
        if (f->glyphs[mid].code < code) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

int font_draw(gfx_fb *fb, const font_t *f, int x, int y, const char *text, uint16_t color)
{
    if (fb->rec) {
        int w = font_text_width(f, text);
        unsigned hh = gfx_hash_u(gfx_hash_u(gfx_hash_u(gfx_hash_u(gfx_hash_u(0x100u,
                      color), (unsigned)x), (unsigned)y), (unsigned)f->line_height),
                      (unsigned)f->ascent);
        for (const unsigned char *c = (const unsigned char *)text; *c; c++)
            hh = gfx_hash_u(hh, *c);
        gfx_rec_cmd(fb, x, y - f->ascent - 1, x + w + 1,
                    y + (f->line_height - f->ascent) + 1, hh);
        return x + w;
    }
    /* 2D dirty-rect x-window: skip glyphs (and clip columns) outside it, so text
     * isn't rasterized when the dirty rect is elsewhere (e.g. a face's clock is
     * not redrawn while only the globe's rect repaints). */
    int cc0 = fb->cx0, cc1 = fb->cx1;
    if (cc1 <= cc0) { cc0 = 0; cc1 = GFX_W; }
    if (cc0 < 0) cc0 = 0;
    if (cc1 > GFX_W) cc1 = GFX_W;
    for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
        const font_glyph *g = find_glyph(f, *c);
        if (!g) { x += f->line_height / 3; continue; }
        const uint8_t *bm = f->bitmap + g->bitmap_off;
        int gx = x + g->xoff + fb->ox;
        if (gx >= cc1 || gx + g->w <= cc0) { x += g->xadv; continue; }  /* off-clip */
        int col0 = cc0 - gx < 0 ? 0 : cc0 - gx;
        int col1 = cc1 - gx > g->w ? g->w : cc1 - gx;
        int by0 = y + g->yoff + fb->oy - fb->y_base;   /* glyph top, buffer row */
        /* Only the rows overlapping this buffer/stripe; a glyph outside the
         * band iterates nothing (was the 8x-per-frame text tax before). */
        int row0 = by0 < 0 ? -by0 : 0;
        int row1 = by0 + g->h > fb->h ? fb->h - by0 : g->h;
        for (int row = row0; row < row1; row++) {
            uint16_t *dst = &fb->px[(by0 + row) * GFX_W];
            const uint8_t *brow = &bm[row * g->w];
            for (int col = col0; col < col1; col++) {
                uint8_t a = brow[col];
                if (!a) continue;
                unsigned bx = (unsigned)(gx + col);
                if (bx >= GFX_W) continue;
                dst[bx] = a >= 255 ? color : gfx_blend565(dst[bx], color, a);
            }
        }
        x += g->xadv;
    }
    return x;
}

int font_text_width(const font_t *f, const char *text)
{
    int w = 0;
    for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
        const font_glyph *g = find_glyph(f, *c);
        w += g ? g->xadv : f->line_height / 3;
    }
    return w;
}

void font_draw_centered(gfx_fb *fb, const font_t *f, int cx, int y, const char *text, uint16_t color)
{
    font_draw(fb, f, cx - font_text_width(f, text) / 2, y, text, color);
}
