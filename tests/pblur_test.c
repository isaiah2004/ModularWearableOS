/* Cache lifecycle and translated/clipped stripe blits, using the production cache
 * and kernels. No board, filesystem, display or serial access. */
#include "../nos/pblur.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

uint32_t nos_frame;
double nos_t;
int nos_mblur = 2, nos_aod;
nos_stats nos_last;
static unsigned coverage_claims;
void ui_fblur_coverage_claim(const gfx_fb *fb,int x0,int y0,int x1,int y1)
{
    if (!fb->rec) { assert(x1-x0==GFX_W && y1-y0==GFX_H); coverage_claims++; }
}
static int transition, allocations, fail_at, changed, overflow;
void *hal_big_alloc(uint32_t n) { return ++allocations == fail_at ? 0 : malloc(n); }
uint32_t hal_micros(void) { return 0; }
int nos_transitioning(void) { return transition; }
unsigned gfx_hash_u(unsigned h, unsigned v) { return (h ^ v) * 16777619u; }
int gfx_rec_cmd(gfx_fb *f, int x0, int y0, int x1, int y1, unsigned hash)
{
    if (!f->rec) return 0;
    if (f->rec->n >= GFX_MAX_CMDS) { f->rec->overflow = 1; return 1; }
    gfx_cmd *c = &f->rec->c[f->rec->n++];
    c->x0 = x0; c->y0 = y0; c->x1 = x1; c->y1 = y1; c->hash = hash;
    return 1;
}
static void source(gfx_fb *f)
{
    assert(pblur_baking);
    if (gfx_rec_cmd(f, 0, 0, GFX_W, GFX_H, 13u + (unsigned)changed)) {
        if (overflow) f->rec->overflow = 1;
        return;
    }
    for (int y = 0; y < f->h; y++) for (int x = 0; x < GFX_W; x++)
        gfx_row(f, y)[x] = (uint16_t)(((x / 8) & 31) << 11 | (((y + f->y_base) / 4) & 63) << 5 | (changed & 31));
}
static void advance(void) { nos_frame++; nos_t += 1.0f / 30; }
static void bake(int id, int axis)
{
    for (int i = 0; i < 30; i++) { advance(); if (!pblur_tick(id, axis, source)) break; }
}
int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) { /* Failure of work, staging or per-page allocation stays sharp. */
        fail_at = atoi(argv[1]);
        bake(0, 0);
        gfx_fb f = {0};
        assert(!pblur_draw(&f, 0, 0, 0, 20));
        int failed_allocations=allocations;
        bake(0,0);assert(allocations==failed_allocations); /* bounded retry cooldown */
        for(int i=0;i<30;i++) advance();
        bake(0, 0);
        assert(PB && PB->slot[0].valid); /* transient failure must not latch forever */
        pblur_release(); assert(!PB && !pblur_bytes());
        bake(6, 1); assert(PB && PB->slot[6].valid);
        puts("pblur allocation failure, recovery and release: PASS"); return 0;
    }
    transition = 1; advance(); pblur_tick(0, 0, source); assert(!PB);
    transition = 0; bake(0, 0); bake(6, 1);
    assert(PB->slot[0].valid && PB->slot[6].valid && pblur_published == 2);
    unsigned hits = pblur_hits[0];
    gfx_fb f = {0};
    assert(!pblur_draw(&f, 0, 0, 0, 0)); assert(pblur_hits[0] == hits);
    /* Arbitrary stripe heights, offsets, column clips, and ink dim. All pixels
     * outside the destination must retain their canary, including row boundaries. */
    uint16_t buf[GFX_W * 17 + 2];
    for (int axis = 0; axis < 2; axis++) for (int off = -37; off <= 37; off += 37)
        for (int height = 1; height <= 17; height++) {
            advance();
            for (int base = 0; base < GFX_H; base += height) {
                for (unsigned i = 0; i < sizeof buf / sizeof buf[0]; i++) buf[i] = 0x5a5a;
                f = (gfx_fb){0}; f.px = buf + 1; f.h = height; f.y_base = base;
                f.cx0 = 23; f.cx1 = 431; f.ox = off; f.oy = -off; f.dim = axis ? 12 : 0;
                /* Refresh the verification timestamp; this loop tests blitting. */
                PB->slot[axis ? 6 : 0].checked = nos_t;
                assert(pblur_draw(&f, axis ? 6 : 0, 0, 0, 24));
                for (int y = 0; y < height; y++) for (int x = 0; x < GFX_W; x++) {
                    int sx = x - off, sy = base + y + off;
                    uint16_t want = 0x5a5a;
                    if (x >= 23 && x < 431 && sx >= 0 && sx < GFX_W && sy >= 0 && sy < GFX_H)
                        want = gfx_ink(&f, PB->lut[PB->slot[axis ? 6 : 0].px[(sy / 2) * GFX_W + sx]]);
                    assert(buf[1 + y * GFX_W + x] == want);
                }
                assert(buf[0] == 0x5a5a && buf[1 + height * GFX_W] == 0x5a5a);
            }
        }
    /* Unchanged content revalidates without raster/allocation; changed content
     * invalidates. Incomplete and changed-generation bakes never publish. */
    nos_t += 4; advance(); pblur_check(0, source); assert(PB->slot[0].valid);
    changed++; nos_t += 2.1f; pblur_check(0, source); assert(!PB->slot[0].valid);
    unsigned pub = pblur_published;
    advance(); pblur_tick(0, 0, source); assert(pblur_busy(0));
    changed++; bake(0, 0); assert(pblur_published == pub && !PB->slot[0].valid);
    bake(0, 0); assert(PB->slot[0].valid);
    overflow = 1; nos_t += 2.1f; pblur_check(0, source); assert(!PB->slot[0].valid);
    puts("pblur: atomic publication, invalidation, rest, transitions, translated stripe blits PASS");
    return 0;
}
