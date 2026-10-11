/* sprite.c — see sprite.h */
#include "sprite.h"
#include "hal.h"
#include "gfx_cost.h"
#include <string.h>

int sprite_alloc(sprite *s, int w, int h)
{
    s->w = w; s->h = h;
    s->px = hal_big_alloc((uint32_t)w * (uint32_t)h * 2u);
    return s->px != 0;
}

gfx_fb sprite_fb(const sprite *s)
{
    gfx_fb fb;
    memset(&fb, 0, sizeof fb);
    fb.px = s->px;
    fb.h = s->h;
    fb.cx0 = 0; fb.cx1 = s->w;    /* clip every primitive to the sprite's width */
    fb.stride = s->w;
    return fb;
}

void sprite_blit(gfx_fb *fb, const sprite *s, int x, int y, unsigned key)
{
    if (fb->rec) {
        gfx_rec_cmd(fb, x, y, x + s->w, y + s->h,
                    gfx_hash_u(gfx_hash_u(gfx_hash_u(0x5917u, key), (unsigned)x), (unsigned)y));
        return;
    }
    if (!s->px) { GFX_COST(GFX_COST_BLIT, 0); return; }
    int c0 = fb->cx0, c1 = fb->cx1;           /* screen-column clip window */
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    int bx = x + fb->ox;                      /* buffer column of sprite column 0 */
    int k0 = bx < c0 ? c0 - bx : 0, k1 = bx + s->w > c1 ? c1 - bx : s->w;
    if (k1 <= k0) { GFX_COST(GFX_COST_BLIT, 0); return; }
    unsigned rows = 0;   /* cost model only */
    for (int r = 0; r < s->h; r++) {
        int by = y + r + fb->oy - fb->y_base;
        if ((unsigned)by >= (unsigned)fb->h) continue;
        memcpy(gfx_row(fb, by) + bx + k0, &s->px[r * s->w + k0], (size_t)(k1 - k0) * 2);
        rows++;
    }
    GFX_COST(GFX_COST_BLIT, rows * (unsigned)(k1 - k0));
}

int sprite8_alloc(sprite8 *s, int w, int h)
{
    s->w = w; s->h = h;
    s->px = hal_big_alloc((uint32_t)w * (uint32_t)h);
    return s->px != 0;
}

void rgb332_levels(int idx, int rgb[3])
{
    rgb[0] = ((idx >> 5) & 7) * 255 / 7;
    rgb[1] = ((idx >> 2) & 7) * 255 / 7;
    rgb[2] = (idx & 3) * 85;
}

void sprite8_lut_identity(uint16_t lut[256])
{
    for (int i = 0; i < 256; i++) {
        int c[3];
        rgb332_levels(i, c);
        lut[i] = GFX_RGB(c[0], c[1], c[2]);
    }
}

void sprite8_from(sprite8 *s, const sprite *src)
{
    for (int i = 0, n = s->w * s->h; i < n; i++) {
        uint16_t c = src->px[i];
        int r = (c >> 11) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
        s->px[i] = (uint8_t)(((r * 7 + 127) / 255) << 5 | ((g * 7 + 127) / 255) << 2 | (b * 3 + 127) / 255);
    }
}

void sprite8_blit(gfx_fb *fb, const sprite8 *s, int x, int y, const uint16_t *lut, unsigned key)
{
    if (fb->rec) {
        gfx_rec_cmd(fb, x, y, x + s->w, y + s->h,
                    gfx_hash_u(gfx_hash_u(gfx_hash_u(0x8a31u, key), (unsigned)x), (unsigned)y));
        return;
    }
    if (!s->px || !lut) { GFX_COST(GFX_COST_BLIT, 0); return; }
    int c0 = fb->cx0, c1 = fb->cx1;           /* screen-column clip window */
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    int bx = x + fb->ox;                      /* buffer column of sprite column 0 */
    int k0 = bx < c0 ? c0 - bx : 0, k1 = bx + s->w > c1 ? c1 - bx : s->w;
    if (k1 <= k0) { GFX_COST(GFX_COST_BLIT, 0); return; }
    int r0 = fb->y_base - y - fb->oy, r1 = r0 + fb->h;  /* sprite rows inside this buffer */
    if (r0 < 0) r0 = 0;
    if (r1 > s->h) r1 = s->h;
    /* sprite8 measured 34 ns/px, same class as the RGB565 blit */
    GFX_COST(GFX_COST_BLIT, r1 > r0 ? (unsigned)(r1 - r0) * (unsigned)(k1 - k0) : 0u);
    for (int r = r0; r < r1; r++) {
        uint16_t *d = gfx_row(fb, y + r + fb->oy - fb->y_base) + bx + k0;
        const uint8_t *p = &s->px[r * s->w + k0];
        int n = k1 - k0;
        for (; n >= 4; n -= 4, p += 4, d += 4) {
            uint16_t a = lut[p[0]], b = lut[p[1]], c = lut[p[2]], e = lut[p[3]];
            d[0] = a; d[1] = b; d[2] = c; d[3] = e;
        }
        while (n--) *d++ = lut[*p++];
    }
}
