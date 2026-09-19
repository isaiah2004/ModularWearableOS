/* pathtext.c — see pathtext.h. stb_truetype implementation lives here. */
#include "pathtext.h"
#include <math.h>
#include <stdlib.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "font_saira_bold_ttf.h"
#include "font_saira_condensed_ttf.h"

#define SS 3          /* source oversample factor */
#define SUB 2         /* dest supersample per axis (2x2) */

static stbtt_fontinfo s_fonts[PATHTEXT_FONT_COUNT];
static int s_ready[PATHTEXT_FONT_COUNT];
static int s_cap_height[PATHTEXT_FONT_COUNT];

static stbtt_fontinfo *pt_init(pathtext_font font)
{
    if ((unsigned)font >= PATHTEXT_FONT_COUNT) return NULL;
    if (!s_ready[font]) {
        const unsigned char *data = font == PATHTEXT_BOLD
            ? saira_bold_ttf : saira_condensed_ttf;
        int off = stbtt_GetFontOffsetForIndex(data, 0);
        s_ready[font] = off >= 0 && stbtt_InitFont(&s_fonts[font], data, off) ? 1 : -1;
        if (s_ready[font] == 1) {
            int x0, y0, x1, y1;
            stbtt_GetCodepointBox(&s_fonts[font], 'H', &x0, &y0, &x1, &y1);
            s_cap_height[font] = y1 - y0;
            if (s_cap_height[font] <= 0) s_ready[font] = -1;
        }
    }
    return s_ready[font] == 1 ? &s_fonts[font] : NULL;
}

static float pt_advance(stbtt_fontinfo *font, const char *c, float scale)
{
    int advance, bearing;
    stbtt_GetCodepointHMetrics(font, (unsigned char)*c, &advance, &bearing);
    if (c[1]) advance += stbtt_GetCodepointKernAdvance(font, (unsigned char)*c,
                                                     (unsigned char)c[1]);
    return advance * scale;
}

/* bilinear sample of an 8-bit coverage bitmap (0 outside) */
static float pt_bil(const unsigned char *b, int w, int h, float x, float y)
{
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float tx = x - x0, ty = y - y0;
    #define S(xx,yy) (((xx) < 0 || (xx) >= w || (yy) < 0 || (yy) >= h) ? 0.0f : (float)b[(yy) * w + (xx)])
    float top = S(x0, y0) * (1 - tx) + S(x0 + 1, y0) * tx;
    float bot = S(x0, y0 + 1) * (1 - tx) + S(x0 + 1, y0 + 1) * tx;
    #undef S
    return top * (1 - ty) + bot * ty;
}

/* Rasterize codepoint at `scale` and paint it with the affine frame
 * (right = (cph,sph), down = (-sph,cph)) anchored at baseline origin (Ox,Oy). */
static void pt_glyph(gfx_fb *fb, stbtt_fontinfo *font, int cp, float scale, float Ox, float Oy,
                     float cph, float sph, uint16_t col)
{
    int w, h, xoff, yoff;
    unsigned char *bmp = stbtt_GetCodepointBitmap(font, scale * SS, scale * SS,
                                                  cp, &w, &h, &xoff, &yoff);
    if (!bmp) return;
    if (w == 0 || h == 0) { stbtt_FreeBitmap(bmp, NULL); return; }

    /* forward-map the four glyph corners (pen px) to screen space for a bbox */
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int cy = 0; cy <= 1; cy++)
        for (int cx = 0; cx <= 1; cx++) {
            float px = (xoff + (cx ? w : 0)) / (float)SS;
            float py = (yoff + (cy ? h : 0)) / (float)SS;
            float X = Ox + px * cph + py * (-sph);
            float Y = Oy + px * sph + py * cph;
            if (X < minx) minx = X;
            if (X > maxx) maxx = X;
            if (Y < miny) miny = Y;
            if (Y > maxy) maxy = Y;
        }
    int x0 = (int)floorf(minx) - 1, y0 = (int)floorf(miny) - 1;
    int x1 = (int)ceilf(maxx) + 1,  y1 = (int)ceilf(maxy) + 1;

    const float o = 0.5f / SUB, step = 1.0f / SUB;
    for (int oy = y0; oy <= y1; oy++) {
        if ((unsigned)oy >= (unsigned)GFX_H) continue;
        for (int ox = x0; ox <= x1; ox++) {
            if ((unsigned)ox >= (unsigned)GFX_W) continue;
            float acc = 0.0f;
            for (int sy = 0; sy < SUB; sy++)
                for (int sx = 0; sx < SUB; sx++) {
                    float dx = ox + o + sx * step - Ox;
                    float dy = oy + o + sy * step - Oy;
                    /* inverse of the affine frame (orthonormal): project onto axes */
                    float penx =  dx * cph + dy * sph;      /* pen px, rightward */
                    float peny = -dx * sph + dy * cph;      /* pen px, downward  */
                    float ssx = penx * SS - xoff - 0.5f;
                    float ssy = peny * SS - yoff - 0.5f;
                    acc += pt_bil(bmp, w, h, ssx, ssy);
                }
            acc /= (SUB * SUB);
            if (acc > 3.0f)
                gfx_px_blend(fb, ox, oy, col, (uint8_t)(acc > 255 ? 255 : acc));
        }
    }
    stbtt_FreeBitmap(bmp, NULL);
}

static void pt_arc(gfx_fb *fb, pathtext_font font, const char *s, float R, float a_mid,
                   float size_px, uint16_t col, float direction)
{
    if (fb->rec || R <= 0 || size_px <= 0) return;
    stbtt_fontinfo *info = pt_init(font);
    if (!info) return;
    float scale = size_px / s_cap_height[font];

    float total = 0.0f;
    for (const char *c = s; *c; c++) total += pt_advance(info, c, scale);
    float a = a_mid - direction * (total / R) * 0.5f, run = 0.0f;
    for (const char *c = s; *c; c++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(info, (unsigned char)*c, &adv, &lsb);
        float half = adv * scale * 0.5f;
        /* Tangent at the glyph centre: both digits have equal radial alignment. */
        float phi = a + direction * (run + half) / R;
        float cs = cosf(phi), sn = sinf(phi);
        float cph = direction * cs, sph = direction * sn;
        float Ox = GFX_CX + R * sn - half * cph;
        float Oy = GFX_CY - R * cs - half * sph;
        if (*c != ' ') pt_glyph(fb, info, (unsigned char)*c, scale, Ox, Oy, cph, sph, col);
        run += pt_advance(info, c, scale);
    }
}

void pathtext_arc(gfx_fb *fb, pathtext_font font, const char *s, float R, float a_mid,
                  float size_px, uint16_t col)
{
    pt_arc(fb, font, s, R, a_mid, size_px, col, 1.0f);
}

void pathtext_arc_inward(gfx_fb *fb, pathtext_font font, const char *s, float R,
                         float a_mid, float size_px, uint16_t col)
{
    pt_arc(fb, font, s, R, a_mid, size_px, col, -1.0f);
}

float pathtext_width(pathtext_font font, const char *s, float size_px)
{
    stbtt_fontinfo *info = pt_init(font);
    if (!info || size_px <= 0) return 0.0f;
    float scale = size_px / s_cap_height[font], w = 0.0f;
    for (const char *c = s; *c; c++) w += pt_advance(info, c, scale);
    return w;
}

int pathtext_draw(gfx_fb *fb, pathtext_font font, const char *s, int x, int y,
                  float size_px, uint16_t col, int center)
{
    if (fb->rec || size_px <= 0) return x;
    stbtt_fontinfo *info = pt_init(font);
    if (!info) return x;
    float scale = size_px / s_cap_height[font];
    float px = center ? x - pathtext_width(font, s, size_px) * 0.5f : (float)x;
    for (const char *c = s; *c; c++) {
        if (*c != ' ') pt_glyph(fb, info, (unsigned char)*c, scale, px, (float)y, 1.0f, 0.0f, col);
        px += pt_advance(info, c, scale);
    }
    return (int)px;
}
