/* sprite.c — see sprite.h */
#include "sprite.h"
#include "hal.h"
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
    if (!s->px) return;
    int c0 = fb->cx0, c1 = fb->cx1;           /* screen-column clip window */
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    int bx = x + fb->ox;                      /* buffer column of sprite column 0 */
    int k0 = bx < c0 ? c0 - bx : 0, k1 = bx + s->w > c1 ? c1 - bx : s->w;
    if (k1 <= k0) return;
    for (int r = 0; r < s->h; r++) {
        int by = y + r + fb->oy - fb->y_base;
        if ((unsigned)by >= (unsigned)fb->h) continue;
        memcpy(gfx_row(fb, by) + bx + k0, &s->px[r * s->w + k0], (size_t)(k1 - k0) * 2);
    }
}
