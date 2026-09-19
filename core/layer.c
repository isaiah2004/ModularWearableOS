#include "layer.h"
#include "hal.h"
#include <string.h>

/* One full-screen RGB565 buffer per slot, allocated lazily from PSRAM via
 * hal_big_alloc (see layer.h for RAM cost — too large for internal SRAM). */
static uint16_t *s_buf[LAYER_MAX];
static gfx_fb    s_fb[LAYER_MAX];
static int       s_tag[LAYER_MAX] = { -1, -1, -1, -1 };
static int       s_rr;   /* round-robin eviction cursor */
static unsigned  s_gen;  /* bumped on invalidate so recorded blits redraw */
int layer_dbg;           /* profiling: layer (re)render count */

static int slot_of(int tag)
{
    for (int i = 0; i < LAYER_MAX; i++)
        if (s_tag[i] == tag)
            return i;
    return -1;
}

gfx_fb *layer_need_render(int tag)
{
    if (slot_of(tag) >= 0)
        return NULL;                 /* already cached */

    /* Prefer a free slot so re-composing one layer never evicts another still
     * in use (e.g. an analog face's dial while its hands base is invalidated). */
    int s = -1;
    for (int i = 0; i < LAYER_MAX; i++)
        if (s_tag[i] < 0) { s = i; break; }
    if (s < 0) { s = s_rr; s_rr = (s_rr + 1) % LAYER_MAX; }  /* else recycle */
    if (!s_buf[s]) {
        s_buf[s] = (uint16_t *)hal_big_alloc((uint32_t)GFX_W * GFX_H * 2);
        if (!s_buf[s])
            return NULL;              /* no PSRAM: caller redraws uncached */
        /* zero fresh PSRAM so a stray blit can never surface uninitialised
         * memory (the magenta "pink noise" speckle) before it's fully drawn */
        memset(s_buf[s], 0, (size_t)GFX_W * GFX_H * 2);
    }
    extern int layer_dbg;
    layer_dbg++;               /* count (re)renders — should be ~0/frame once cached */
    s_tag[s] = tag;
    s_fb[s].px = s_buf[s];
    s_fb[s].ox = s_fb[s].oy = 0;      /* cache is authored in absolute coords */
    s_fb[s].y_base = 0;
    s_fb[s].h = GFX_H;                /* cache is always a full-size buffer */
    s_fb[s].cx0 = 0; s_fb[s].cx1 = GFX_W;   /* compose the whole width */
    s_fb[s].rec = 0;                  /* the cache render always rasterizes */
    return &s_fb[s];
}

void layer_blit(gfx_fb *dst, int tag)
{
    if (dst->rec) {
        gfx_rec_cmd(dst, 0, 0, GFX_W, GFX_H,
                    gfx_hash_u(gfx_hash_u(0x200u, (unsigned)tag), s_gen));
        return;
    }
    int s = slot_of(tag);
    if (s < 0)
        return;
    const uint16_t *src = s_buf[s];

    /* 2D dirty-rect x-window: only this column span is repainted (the big win —
     * this is the PSRAM read, so a thin dirty column reads far less). */
    int c0 = dst->cx0, c1 = dst->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;

    /* Copy per destination row so this works for a full frame, a page-slide
     * (dst->ox/oy shift) and a render stripe (dst->y_base = y0, dst->h = rows)
     * alike: buffer row dy holds cached (logical) row dy - oy + y_base. */
    for (int dy = 0; dy < dst->h; dy++) {
        int y = dy - dst->oy + dst->y_base;
        if ((unsigned)y >= GFX_H)
            continue;
        int sx = 0, w = GFX_W, tx = dst->ox;
        if (tx < 0) { sx = -tx; w += tx; tx = 0; }
        if (tx + w > GFX_W) w = GFX_W - tx;
        /* clip the copied span to the dirty-rect window [c0,c1) */
        int left = tx < c0 ? c0 : tx;
        int right = (tx + w) > c1 ? c1 : (tx + w);
        int ww = right - left;
        if (ww > 0)
            memcpy(&dst->px[dy * GFX_W + left], &src[y * GFX_W + sx + (left - tx)],
                   (size_t)ww * sizeof(uint16_t));
    }
}

gfx_fb *layer_fb(int tag)
{
    int s = slot_of(tag);
    return s >= 0 ? &s_fb[s] : NULL;   /* draw INTO a cached layer (accumulate) */
}

void layer_invalidate(int tag)
{
    s_gen++;   /* recorded blits differ next frame -> that region redraws */
    if (tag < 0) {
        for (int i = 0; i < LAYER_MAX; i++)
            s_tag[i] = -1;
    } else {
        int s = slot_of(tag);
        if (s >= 0)
            s_tag[s] = -1;
    }
}
