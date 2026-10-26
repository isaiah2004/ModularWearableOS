#include "render_layers.h"
#include "render_bounds.h"
#include "gfx_cost.h"
#include "ui.h"
#include <string.h>

#define SLOTS 3
#define ROWS 16
#define BANDS ((GFX_H + ROWS - 1) / ROWS)
typedef struct {
    const nos_screen *screen;
    uint16_t *pixels;
    uint32_t frame, touched;
    int prepared, observed, bypass;
    unsigned hash[BANDS], next[BANDS], previous[BANDS];
    uint8_t valid[BANDS];
} layer;
static layer layers[SLOTS];
static gfx_rec *record;
static uint32_t frame_id;
static nos_layer_stats stats;
static int allocation_blocked;
int nos_layer_cache_enabled = 1;

void nos_layers_init(void)
{
    /* Allocate only when an eligible screen is drawn. Specialized face, list
     * and planet-card caches must not compete with unused full-screen layers. */
}
void nos_layers_release(void)
{
    for (int i = 0; i < SLOTS; i++) {
        hal_big_free(layers[i].pixels);
    }
    hal_big_free(record); record = NULL;
    memset(layers, 0, sizeof layers);
    stats.bytes = 0;
    allocation_blocked = 0;
}
void nos_layers_idle(void)
{
    const nos_screen *s = nos_top();
    if (stats.bytes && !nos_transitioning() && s && (s->flags & (SCR_NATIVE_CACHE | SCR_LIVE_RENDER)))
        nos_layers_release();
}
static int allocate_layer(layer *l)
{
    if (allocation_blocked) return 0;
    uint32_t bytes = GFX_W * GFX_H * sizeof(uint16_t);
#ifdef NOS_PSRAM_TRACKING
    /* Admission before allocation leaves room for other UI owners. A skipped
     * optional cache is not an allocator failure and uses the live renderer. */
    uint32_t available = nos_mem_available();
    if (available < 512u * 1024u || bytes + (record ? 0u : sizeof(gfx_rec)) > available - 512u * 1024u)
        return 0;
#endif
    if (!record) {
        record = hal_big_alloc(sizeof *record);
        if (!record) { stats.allocation_failures++; allocation_blocked = 1; return 0; }
        stats.bytes += sizeof *record;
    }
    l->pixels = hal_big_alloc(bytes);
    if (!l->pixels) { stats.allocation_failures++; allocation_blocked = 1; return 0; }
    stats.bytes += bytes;
    return 1;
}
void nos_layers_invalidate(void)
{
    for (int i = 0; i < SLOTS; i++) {
        memset(layers[i].valid, 0, sizeof layers[i].valid);
        layers[i].prepared = layers[i].observed = 0;
    }
}
void nos_layers_begin(uint32_t frame)
{
    frame_id = frame;
    stats.bands_rendered = stats.bands_reused = stats.blit_pixels = 0;
    stats.overflow_frames = stats.record_us = stats.raster_us = 0;
}
nos_layer_stats nos_layers_stats(void) { return stats; }

static layer *find_layer(const nos_screen *s)
{
    layer *victim = NULL, *empty = NULL;
    for (int i = 0; i < SLOTS; i++) {
        layer *l = &layers[i];
        if (!l->pixels) { if (!empty) empty = l; continue; }
        if (l->screen == s) { l->touched = frame_id; return l; }
        /* Never evict a screen already referenced in this frame. */
        if ((!l->screen || l->touched != frame_id) && (!victim || !l->screen || l->touched < victim->touched)) victim = l;
    }
    /* Reuse an old screen's allocation before growing the resident set. More
     * slots are needed only when multiple screens are drawn in one frame. */
    if (!victim && empty && allocate_layer(empty)) victim = empty;
    if (!victim) return NULL;
    victim->screen = s; victim->frame = 0; victim->touched = frame_id;
    victim->prepared = victim->observed = victim->bypass = 0;
    memset(victim->valid, 0, sizeof victim->valid);
    return victim;
}

static void prepare(layer *l)
{
    if (l->prepared && l->frame == frame_id) return;
    l->frame = frame_id;
    l->prepared = 1;
    l->bypass = 0;
    uint32_t start = hal_micros();
    record->n = record->overflow = 0;
    gfx_fb r = {0}; r.h = GFX_H; r.cx1 = GFX_W; r.rec = record;
    uint32_t blur_epoch = nos_mblur_record_epoch();
    l->screen->draw(&r);
    /* Streaming directional filters own continuation state and directional
     * coverage in the live output. Retaining their raster in independent bands
     * would suppress coverage and replay a stale filter tail. */
    l->bypass = nos_mblur_record_epoch() != blur_epoch;
    for (int b = 0; b < BANDS; b++) l->next[b] = 0x917cab13u;
    if (record->overflow) {
        /* No out-of-bounds command reads or false cache hits after overflow. */
        memset(l->valid, 0, sizeof l->valid);
        l->observed = 0;
        stats.overflow_frames++;
    } else {
        for (int i = 0; i < record->n; i++) {
            const gfx_cmd *c = &record->c[i];
            if (c->x1 <= c->x0 || c->y1 <= c->y0) continue;
            int b0 = c->y0 / ROWS, b1 = (c->y1 - 1) / ROWS;
            if (b0 < 0) b0 = 0;
            if (b1 >= BANDS) b1 = BANDS - 1;
            unsigned h = gfx_hash_u(c->hash, (unsigned)c->x0);
            h = gfx_hash_u(h, (unsigned)c->y0);
            h = gfx_hash_u(h, (unsigned)c->x1);
            h = gfx_hash_u(h, (unsigned)c->y1);
            for (int b = b0; b <= b1; b++) l->next[b] = gfx_hash_u(l->next[b], h);
        }
        int changed_rows = 0;
        for (int b = 0; b < BANDS; b++) {
            if (l->observed && l->previous[b] != l->next[b]) {
                int rows = GFX_H - b * ROWS;
                changed_rows += rows < ROWS ? rows : ROWS;
            }
            if (l->hash[b] != l->next[b]) l->valid[b] = 0;
            l->previous[b] = l->next[b];
        }
        /* Rebuilding most retained bands adds PSRAM writes to a render which
         * is already live work. Compare consecutive records, not cache validity:
         * scrolling must stay live even after a bypass invalidated the cache,
         * and while directional blur is still waiting for a live cost sample.
         * The first observation still bootstraps static/sparse screen caching. */
        if (l->observed && changed_rows * 2 >= GFX_H) l->bypass = 1;
        l->observed = 1;
    }
    /* A later settled frame must rebuild sharp pixels instead of replaying the
     * last cached position. Keep this frame's decision for every output stripe. */
    if (l->bypass) memset(l->valid, 0, sizeof l->valid);
    stats.record_us += hal_micros() - start;
}

/* A cold band can be composed in the output's internal RAM before a single
 * sequential cache write. This avoids PSRAM read/modify/write for every AA
 * primitive without another raster allocation. Only a complete, untranslated
 * band which owns all visible output columns may borrow that output. */
static int output_band(const gfx_fb *fb, int y, int h)
{
    if (fb->ox || fb->oy || fb->x_base ||
        (fb->stride && fb->stride != GFX_W) ||
        y < fb->y_base || y + h > fb->y_base + fb->h) return 0;
    int x0 = 0, x1 = GFX_W;
    if (fb->glass_only) nos_glass_span(y, y + h, &x0, &x1);
    return fb->cx1 <= fb->cx0 || (fb->cx0 <= x0 && fb->cx1 >= x1);
}

static int ensure_band(layer *l, int band, gfx_fb *output)
{
    if (l->valid[band]) { stats.bands_reused++; return 0; }
    uint32_t start = hal_micros();
    int y = band * ROWS, h = GFX_H - y;
    if (h > ROWS) h = ROWS;
    int borrowed = output_band(output, y, h);
    uint16_t *cached = l->pixels + y * GFX_W;
    gfx_fb f = {0};
    f.px = borrowed ? output->px + (y - output->y_base) * GFX_W : cached;
    f.y_base = y; f.h = h; f.cx1 = GFX_W;
#ifdef GFX_COST_MODEL
    f.cost_psram = borrowed ? output->cost_psram : 1;
    gfx_cost_target(f.cost_psram);
#endif
    memset(f.px, 0, (size_t)h * GFX_W * sizeof(uint16_t));
    GFX_COST_PX(GFX_COST_FILL, h * GFX_W);
    int track = ui_fblur_coverage_enable(0);
    l->screen->draw(&f);
    ui_fblur_coverage_enable(track);
    if (borrowed) {
        memcpy(cached, f.px, (size_t)h * GFX_W * sizeof(uint16_t));
#ifdef GFX_COST_MODEL
        gfx_cost_target(1);
#endif
        GFX_COST_PX(GFX_COST_BLIT, h * GFX_W);
    }
    l->hash[band] = l->next[band]; l->valid[band] = 1;
    stats.bands_rendered++;
    stats.raster_us += hal_micros() - start;
    return borrowed;
}

int nos_layers_draw(gfx_fb *fb, const nos_screen *screen)
{
    if (!nos_layer_cache_enabled || !screen || !screen->draw || fb->rec ||
        (screen->flags & SCR_NATIVE_CACHE) || ui_fblur_any_active()) return 0;
    layer *l = find_layer(screen);
    if (!l) return 0;
    prepare(l);
    if (l->bypass) return 0;
    int x0 = fb->cx0, x1 = fb->cx1;
    if (x1 <= x0) { x0 = 0; x1 = GFX_W; }
    if (x0 < 0) x0 = 0;
    if (x1 > GFX_W) x1 = GFX_W;
    if (fb->stride > 0) {
        if (x0 < fb->x_base) x0 = fb->x_base;
        if (x1 > fb->x_base + fb->stride) x1 = fb->x_base + fb->stride;
    }
    int sy0 = fb->y_base - fb->oy, sy1 = sy0 + fb->h;
    int sx0 = x0 - fb->ox, sx1 = x1 - fb->ox;
    if (sx0 < 0) sx0 = 0;
    if (sx1 > GFX_W) sx1 = GFX_W;
    if (sy0 < 0) sy0 = 0;
    if (sy1 > GFX_H) sy1 = GFX_H;
    if (sx1 <= sx0 || sy1 <= sy0) return 1;
    uint32_t fresh = 0;
    for (int b = sy0 / ROWS; b <= (sy1 - 1) / ROWS; b++)
        if (ensure_band(l, b, fb)) fresh |= 1u << b;
    int w = sx1 - sx0;
    unsigned n = 0;
    for (int y = sy0; y < sy1; y++) {
        if (!fb->dim && (fresh & (1u << (y / ROWS)))) continue;
        const uint16_t *src = l->pixels + y * GFX_W + sx0;
        uint16_t *dst = gfx_row(fb, y + fb->oy - fb->y_base) + sx0 + fb->ox;
        if (!fb->dim) memcpy(dst, src, (size_t)w * sizeof(uint16_t));
        else for (int x = 0; x < w; x++) {
            uint16_t c = src[x];
            /* Most of a watch screen is black. It stays black at every dim
             * level without the RGB565 neutral-color and channel arithmetic. */
            dst[x] = c ? gfx_ink(fb, c) : 0;
        }
        n += (unsigned)w;
    }
    stats.blit_pixels += n;
#ifdef GFX_COST_MODEL
    gfx_cost_target(fb->cost_psram);
#endif
    GFX_COST(GFX_COST_BLIT, n);
    return 1;
}
