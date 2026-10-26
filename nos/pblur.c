#include "pblur.h"
#include "mblur.h"
#include "nos.h"
#include "hal.h"
#include "gfx_cost.h"
#include "sprite.h"
#include "ui.h"
#include <string.h>

#define PB_N 7
#define PB_ROWS 16
#define PB_PIXELS (GFX_W * ((GFX_H + 1) / 2))
typedef struct {
    uint8_t *px;
    unsigned sig, generation;
    nos_time_t checked;
    int valid, failed, vertical;
} pb_slot;
static struct pb_work {
    pb_slot slot[PB_N];
    gfx_rec rec;
    uint16_t canvas[GFX_W * PB_ROWS];
    uint32_t line[GFX_W + 40];
    mb_h4_state hs;
    mb_v4_state vs;
    uint8_t *stage;
    uint16_t lut[256];
    int job, row;
    unsigned sig;
    nos_time_t started;
} *PB;
static uint32_t pb_frame;
static uint32_t pb_seen[PB_N];
int pblur_baking;
unsigned pblur_hits[2], pblur_misses[2], pblur_published, pblur_discarded, pblur_why[5];

/* Optional prewarm must leave room for live text/masks and the next UI owner.
 * Admission skips are not allocator failures and can retry when memory returns. */
static int pb_alloc_failed;
static uint32_t pb_retry_frame,pb_failed_used;
static void *pb_alloc(unsigned bytes)
{
    if(pb_alloc_failed) {
        int wait=(int32_t)(nos_frame-pb_retry_frame)<0;
#ifdef NOS_PSRAM_TRACKING
        wait=wait && nos_mem_used()>=pb_failed_used;
#endif
        if(wait) return 0;
        pb_alloc_failed=0;
    }
#ifdef NOS_PSRAM_TRACKING
    unsigned available=nos_mem_available();
    if(available<512u*1024u || bytes>available-512u*1024u) return 0;
#endif
    void *p=hal_big_alloc(bytes);
    if(!p) {
        pb_alloc_failed=1;pb_retry_frame=nos_frame+30;
#ifdef NOS_PSRAM_TRACKING
        pb_failed_used=nos_mem_used();
#else
        pb_failed_used=0;
#endif
    }
    return p;
}
static int init(void)
{
    if (!PB) {
        PB = pb_alloc(sizeof *PB);
        if (!PB) return 0;
        memset(PB, 0, sizeof *PB);
        PB->job = -1;
        /* 216 colour-cube entries plus 40 neutral greys. RGB332's identity LUT
         * tints neutral UI greys; these dedicated entries keep them neutral. */
        for (int r = 0; r < 6; r++) for (int g = 0; g < 6; g++) for (int b = 0; b < 6; b++)
            PB->lut[r * 36 + g * 6 + b] = (uint16_t)(((r * 31 + 2) / 5) << 11 |
                                                       ((g * 63 + 2) / 5) << 5 | ((b * 31 + 2) / 5));
        for (int i = 0; i < 40; i++) {
            int v = (i * 31 + 19) / 39;
            PB->lut[216 + i] = i == 39 ? 0xffffu : (uint16_t)(v << 11 | (2 * v) << 5 | v);
        }
    }
    if (!PB->stage) PB->stage = pb_alloc(PB_PIXELS);
    return PB->stage != 0;
}
void pblur_release(void)
{
    pb_alloc_failed=0;
    if (!PB) return;
    for (int i = 0; i < PB_N; i++) hal_big_free(PB->slot[i].px);
    hal_big_free(PB->stage);
    hal_big_free(PB); PB = 0;
}
int pblur_busy(int id) { return PB && PB->job == id; }
unsigned pblur_ready_mask(void)
{
    unsigned mask = 0;
    if (PB) for (int i = 0; i < PB_N; i++)
        if (PB->slot[i].valid && nos_t - PB->slot[i].checked <= 3.0f) mask |= 1u << i;
    return mask;
}
unsigned pblur_baked_mask(void)
{
    unsigned mask = 0;
    if (PB) for (int i = 0; i < PB_N; i++) if (PB->slot[i].generation) mask |= 1u << i;
    return mask;
}
unsigned pblur_bytes(void)
{
    if (!PB) return 0;
    unsigned n = sizeof *PB + (PB->stage ? PB_PIXELS : 0);
    for (int i = 0; i < PB_N; i++) if (PB->slot[i].px) n += PB_PIXELS;
    return n;
}
void pblur_invalidate(int id)
{
    if (!PB || id < 0 || id >= PB_N) return;
    PB->slot[id].valid = 0;
    if (PB->job == id) PB->job = -1;
}
static unsigned signature(pb_draw_fn draw)
{
    gfx_fb f = {0};
    f.h = GFX_H; f.cx1 = GFX_W; f.rec = &PB->rec;
    PB->rec.n = PB->rec.overflow = 0;
    pblur_baking = 1; draw(&f); pblur_baking = 0;
    /* An overflowing recorder cannot prove content identity. Never publish it. */
    if (PB->rec.overflow) return 0;
    unsigned h = 2166136261u;
    for (int i = 0; i < PB->rec.n; i++) {
        const gfx_cmd *c = &PB->rec.c[i];
        h = gfx_hash_u(h, c->hash);
        h = gfx_hash_u(h, (unsigned)c->x0); h = gfx_hash_u(h, (unsigned)c->y0);
        h = gfx_hash_u(h, (unsigned)c->x1); h = gfx_hash_u(h, (unsigned)c->y1);
    }
    return h ? h : 1;
}
void pblur_check(int id, pb_draw_fn draw)
{
    if (!PB || id < 0 || id >= PB_N || !draw) return;
    pb_slot *s = &PB->slot[id];
    if (!s->valid || nos_t - s->checked < 2.0f) return;
    unsigned sig = signature(draw);
    if (!sig || sig != s->sig) s->valid = 0;
    else s->checked = nos_t;
}
int pblur_tick(int id, int vertical, pb_draw_fn draw)
{
    if (!nos_mblur || nos_aod || nos_transitioning() || !draw || id < 0 || id >= PB_N) return 0;
    if (pb_frame == nos_frame) return pblur_busy(id);
    pb_frame = nos_frame;
    uint32_t cpu = nos_last.raster_us > nos_last.wait_us ? nos_last.raster_us - nos_last.wait_us : 0;
    int skip = cpu + nos_last.record_us >= 20000u;
#ifdef GFX_COST_MODEL
    /* A forced-full pixel reference changes modeled CPU even at an idle pose.
     * Replay only this adaptive admission, never mandatory guards or failures. */
    if (nos_quality_decision) skip = nos_quality_decision(nos_frame, 5, (unsigned)id, skip);
#endif
    if (skip) { pblur_why[3]++; return 1; }
    if (!init()) return 0;
    pb_slot *s = &PB->slot[id];
    if (PB->job >= 0 && PB->job != id) { PB->job = -1; pblur_discarded++; pblur_why[0]++; }
    uint32_t start = hal_micros();
#ifdef GFX_COST_MODEL
    (void)start; /* fixed slices in the sim; host CPU speed must not change pixels */
#endif
    if (PB->job < 0) {
        if (s->valid && nos_t - s->checked < 2.0f) return 0;
        unsigned sig = signature(draw);
        if (!sig) { s->valid = 0; return 0; }
        if (s->valid && s->sig == sig) { s->checked = nos_t; return 0; }
        s->valid = 0;
        if (!s->px) s->px = pb_alloc(PB_PIXELS);
        if (!s->px) return 0;
        s->vertical = vertical != 0;
        PB->job = id; PB->row = 0; PB->sig = sig; PB->started = nos_t;
        PB->hs.next = PB->vs.next = -1;
    }
    /* Never resume a very old partial source after a visit to another screen (8 s: on the
     * watch a bake slice costs > 2 ms and idle frames come ~7 a second; the signature check
     * at the end catches a page that changed underneath). */
    if (nos_t - PB->started > 8.0f) { PB->job = -1; pblur_discarded++; pblur_why[1]++; return 1; }
    for (int slice = 0; slice < 4; slice++) {
        int rows = GFX_H - PB->row;
        if (rows > PB_ROWS) rows = PB_ROWS;
        gfx_fb f = {0};
        f.px = PB->canvas; f.h = rows; f.y_base = PB->row; f.cx1 = GFX_W;
#ifdef GFX_COST_MODEL
        f.cost_psram = 1;
#endif
        memset(PB->canvas, 0, (size_t)rows * GFX_W * 2);
        pblur_baking = 1; draw(&f); pblur_baking = 0;
        if (vertical) mb_v4(&f, 0, GFX_W, 0, GFX_H, 3, &PB->vs);
        else mb_h4_rows(&f, 0, GFX_W, 4, PB->line, &PB->hs);
        for (int y = 0; y < rows; y += 2) for (int x = 0; x < GFX_W; x++) {
            uint16_t c = PB->canvas[y * GFX_W + x];
            int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
            int index = r == b && g >= 2 * r - 2 && g <= 2 * r + 2 ? 216 + (g * 39 + 31) / 63 :
                ((r * 5 + 15) / 31) * 36 + ((g * 5 + 31) / 63) * 6 + (b * 5 + 15) / 31;
            PB->stage[((PB->row + y) / 2) * GFX_W + x] = (uint8_t)index;
        }
        GFX_COST_PX(GFX_COST_DIM, (unsigned)(rows * GFX_W) * (vertical ? 50u : 35u) / 8u);
        GFX_COST_PX(GFX_COST_DIM, (unsigned)((rows + 1) / 2 * GFX_W) * 8u); /* palette conversion */
        PB->row += rows;
        pblur_why[4]++;
        if (PB->row == GFX_H) {
            unsigned sig = signature(draw);
            if (sig && sig == PB->sig) {
                uint8_t *old = s->px; s->px = PB->stage; PB->stage = old;
                s->sig = sig; s->valid = 1; s->checked = PB->started;
                s->generation++; pblur_published++;
            } else { pblur_discarded++; pblur_why[2]++; }
            PB->job = -1;
            return 0;
        }
#ifndef GFX_COST_MODEL
        if (hal_micros() - start >= 6000u) break;   /* at rest the watch ticks ~7 a second: 2 ms let a page take 4 s */
#endif
    }
    return 1;
}

int pblur_draw(gfx_fb *fb, int id, int x, int y, float step)
{
    if (!nos_mblur || step < 8 || id < 0 || id >= PB_N) return 0;
    pb_slot *s = PB ? &PB->slot[id] : 0;
    int axis = id == 6;
    int ready = s && s->valid && nos_t - s->checked <= 3.0f;
    /* One observation per page/frame, not per recorder/stripe. */
    if (pb_seen[id] != nos_frame) {
        pb_seen[id] = nos_frame;
        if (ready) pblur_hits[axis]++; else pblur_misses[axis]++;
    }
    if (!ready) return 0;
    sprite8 sp = { s->px, GFX_W, (GFX_H + 1) / 2 };
    sprite8_blit_y2(fb, &sp, x, y, GFX_H, PB->lut,
                   gfx_hash_u(gfx_hash_u(0x5042u + (unsigned)id, s->generation), s->sig));
    ui_fblur_coverage_claim(fb, x, y, x + GFX_W, y + GFX_H);
    return 1;
}
