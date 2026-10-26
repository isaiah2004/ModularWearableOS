#include "../nos/kblur.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static int transitioning;
void *hal_big_alloc(uint32_t n) { return calloc(1, n); }
uint32_t hal_micros(void) { static uint32_t t; return t += 1000; }
int nos_transitioning(void) { return transitioning; }
static void source(gfx_fb *f, int i, int x0, int y, int x1, int h, void *ctx)
{
    (void)i; (void)x0; (void)y; (void)x1; (void)h; (void)ctx;
    for (int r = 0; r < f->h; r++) for (int x = 0; x < f->stride; x++)
        f->px[r * f->stride + x] = (uint16_t)((r + f->y_base + x) % 64 << 5);
}
int main(void)
{
    for (int h = 1; h <= 127; h += 7) {
        kblur *c = kblur_new(43, h, 24, 200000);
        assert(c);
        transitioning = 1;
        assert(kblur_bake(c, 7, 1, h, source, 0, 0, 1) && !c->job);
        transitioning = 0;
        kblur_bake(c, 7, 1, h, source, 0, 0, 1);
        assert(c->job && !kblur_ready(c, 7, 1, 1));
        kblur_bake(c, 7, 2, h, source, 0, 0, 1); /* changed partial source restarts */
        assert(c->job && c->job->sig == 2 && c->stage == 0 && c->strip == 1);
        for (int n = 0; n < 1000 && !kblur_ready(c, 7, 2, KB_NLEV); n++)
            kblur_bake(c, 7, 2, h, source, 0, 0, 1);
        assert(kblur_ready(c, 7, 2, KB_NLEV));
        kb_slot *s = find(c, 7);
        for (int lv = 0; lv < KB_NLEV; lv++) for (int y = 0; y < h; y += 2)
            for (int x = 0; x < c->W; x++) {
                int v[2];
                for (int pair = 0; pair < 2; pair++) {
                    int sum = 0, len = k_len[lv], begin = y + pair + c->C - len / 2;
                    for (int r = begin; r < begin + len; r++)
                        if (r >= 0 && r < h + 2 * c->C) sum += c->grey[r * c->W + x];
                    v[pair] = (sum + len / 2) / len;
                }
                int want = y + 1 < h ? (v[0] + v[1] + 1) / 2 : v[0];
                assert(s->blur[lv][(y / 2) * c->W + x] == want);
            }
        free(c);
    }
    assert(!kblur_new(466, 180, 24, 100)); /* scratch must fit before pointer layout */
    kblur *c = kblur_new(16, 16, 2, 14000);
    assert(c && c->nslots >= 2);
    for (int k = 0; k < c->nslots; k++) {
        c->s[k].used = c->s[k].ok = 1; c->s[k].key = (unsigned)k + 1;
        c->s[k].stamp = k ? (unsigned)k : 100;
    }
    c->tick = 100;
    kblur_bake(c, 999, 1, 16, source, 0, 0, 1);
    assert(c->job == &c->s[1]); /* actual oldest, not first occupied */
    free(c);
    puts("kblur: scalar slices, invalidation, transitions, minimum arena and LRU PASS");
    return 0;
}
