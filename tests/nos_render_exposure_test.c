#include "../nos/render_history.c"
#include "render_exposure.h"
#include <assert.h>
#include <stdio.h>
void *hal_big_alloc(uint32_t n) { return malloc(n); }
static uint32_t cov[16][15];
static int base, active;
int ui_fblur_any_active(void) { return active; }
int ui_fblur_row_spans(int y, int spans[][2], int cap) {
    int n = 0, x = 0;
    if (y < base || y >= base + 16)
        return 0;
    while (x < GFX_W) {
        while (x < GFX_W && !((cov[y - base][x / 32] >> (x & 31)) & 1))
            x++;
        if (x == GFX_W)
            break;
        int a = x;
        while (x < GFX_W && ((cov[y - base][x / 32] >> (x & 31)) & 1))
            x++;
        if (n < cap) {
            spans[n][0] = a;
            spans[n++][1] = x;
        }
    }
    return n;
}
static void original(gfx_fb *f, int x0, int y0, int x1, int y1, int enabled) {
    if (!active) {
        nos_history_apply_disjoint(f, x0, y0, x1, y1, enabled);
        return;
    }
    for (int y = y0; y < y1; y++) {
        int spans[32][2], n = ui_fblur_row_spans(y, spans, 32), x = x0;
        for (int i = 0; i < n && x < x1; i++) {
            int a = spans[i][0], b = spans[i][1];
            if (b <= x || a >= x1)
                continue;
            if (a > x)
                nos_history_apply_disjoint(f, x, y, a < x1 ? a : x1, y + 1, enabled);
            if (a < x)
                a = x;
            if (b > x1)
                b = x1;
            nos_history_apply_disjoint(f, a, y, b, y + 1, 0);
            x = b;
        }
        if (x < x1)
            nos_history_apply_disjoint(f, x, y, x1, y + 1, enabled);
    }
}
static uint32_t rng = 17;
static uint32_t rnd(void) {
    rng = rng * 1664525u + 1013904223u;
    return rng;
}
static uint16_t input[GFX_W * 16 + 4], a[GFX_W * 16 + 4], b[GFX_W * 16 + 4], seed[GFX_W * GFX_H],
    history[GFX_W * GFX_H];
static uint32_t valid[HISTORY_BITS/4],seen[HISTORY_BITS/4],uniform[GFX_H],generations[GFX_H];
static uint16_t vlo[GFX_H],vhi[GFX_H];
int main(void) {
    assert(nos_history_init());
    _Static_assert(sizeof(((nos_exposure_job *)0)->directional) == 960,
                   "two stripe slots must retain their fixed coverage budget");
    gfx_fb invalid = {0};
    nos_exposure_job invalid_job;
    assert(!nos_exposure_prepare(&invalid_job, &invalid, 0, 0, 466, 16, 1, 0));
    invalid.px = a;
    invalid.h = 17;
    assert(!nos_exposure_prepare(&invalid_job, &invalid, 0, 0, 466, 17, 1, 0));
    invalid.h = 16;
    assert(nos_exposure_prepare(&invalid_job, &invalid, 3, 0, 3, 16, 1, 0));
    nos_exposure_apply(&invalid_job);
    for (unsigned k = 0; k < sizeof seed / sizeof seed[0]; k++)
        seed[k] = (uint16_t)rnd();
    for (int trial = 0; trial < 6000; trial++) {
        base = rnd() % (GFX_H - 16);
        active = trial % 4 != 0;
        int enabled = (trial >> 2) & 1, aod = trial % 11 == 0;
        int stride = trial % 3 ? GFX_W : 93, xbase = stride == 93 ? 31 : 0, offset = trial & 1;
        for (unsigned k = 0; k < sizeof seed / sizeof seed[0]; k++)
            seed[k] = trial % 5 ? (uint16_t)rnd() : (trial & 1 ? 0xffff : 0);
        for (unsigned k = 0; k < sizeof input / sizeof input[0]; k++)
            input[k] = trial % 5 ? (uint16_t)rnd() : (trial & 1 ? 0xffff : 0);
        memcpy(a, input, sizeof a);
        memcpy(b, input, sizeof b);
        memset(cov, 0, sizeof cov);
        for (int y = 0; y < 16; y++)
            for (int i = 0; i < 12; i++) {
                int x = rnd() % GFX_W, n = rnd() % 75;
                for (int q = x; q < x + n && q < GFX_W; q++)
                    cov[y][q / 32] |= 1u << (q & 31);
            }
        if (trial % 13 == 0)
            memset(cov, 255, sizeof cov);
        if (trial % 17 == 0)
            memset(cov, 0, sizeof cov);
        if (trial % 19 == 0) {
            memset(cov, 0, sizeof cov);
            for (int y = 0; y < 16; y++) {
                cov[y][0] = 1u << 31;
                cov[y][1] = 1u;
                cov[y][14] = 1u << 17; /* last physical column465 */
            }
        }
        if(trial%3==0){
            int group=1<<((trial/3)%5);
            for(int y=1;y<16;y++)if(y%group)memcpy(cov[y],cov[y-y%group],sizeof cov[y]);
        }
        gfx_fb f = {0};
        f.px = a + offset;
        f.h = 16;
        f.y_base = base;
        f.stride = stride;
        f.x_base = xbase;
        f.cx0 = xbase + (trial % 7);
        f.cx1 = xbase + stride - (trial % 9);
        int x0 = xbase - 5, x1 = xbase + stride + 5, y0 = base - (trial % 3),
            y1 = base + 1 + rnd() % 16;
        nos_history_stats expected;
        nos_exposure_job job;
        f.px = b + offset;
        assert(nos_exposure_prepare(&job, &f, x0, y0, x1, y1, enabled, aod));
        for (int pass = 0; pass < 2; pass++) {
            nos_history_reset();
            gfx_fb full = {0};
            full.px = seed;
            full.h = GFX_H;
            full.cx1 = GFX_W;
            nos_history_begin(1);
            /* Exercise both genuinely cold history and warm histories with holes
             * excluded by the independently clipped framebuffer. */
            if (trial % 7)
                nos_history_apply_disjoint(&full, 0, 0, GFX_W, GFX_H, 0);
            nos_history_begin(2);
            nos_history_uniform = trial & 1;
            if (!pass) {
                f.px = a + offset;
                original(&f, x0, y0, x1, y1, enabled && !aod);
                memcpy(history, s_sharp, sizeof history);
                expected = *nos_history_get_stats();
                memcpy(valid,s_valid,sizeof valid);memcpy(seen,s_seen,sizeof seen);
                memcpy(uniform,s_uniform,sizeof uniform);memcpy(generations,s_row_generation,sizeof generations);
                memcpy(vlo,s_valid_lo,sizeof vlo);memcpy(vhi,s_valid_hi,sizeof vhi);
            } else {
                memset(cov, 0, sizeof cov);
                active = 0;
                nos_exposure_apply(&job);
                assert(!memcmp(a, b, sizeof a));
                assert(!memcmp(history, s_sharp, sizeof history));
                assert(!memcmp(valid,s_valid,sizeof valid));assert(!memcmp(seen,s_seen,sizeof seen));
                assert(!memcmp(uniform,s_uniform,sizeof uniform));assert(!memcmp(generations,s_row_generation,sizeof generations));
                assert(!memcmp(vlo,s_valid_lo,sizeof vlo));assert(!memcmp(vhi,s_valid_hi,sizeof vhi));
                if (memcmp(&expected, nos_history_get_stats(), sizeof expected)) {
                    printf(
                        "stats mismatch trial%d pixels%u/%u blended%u/%u equal%u/%u uniform%u/%u\n",
                        trial, expected.pixels, s_stats.pixels, expected.blended, s_stats.blended,
                        expected.equal_words, s_stats.equal_words, expected.uniform_pixels,
                        s_stats.uniform_pixels);
                    return 2;
                }
            }
        }
    }
    printf("PASS6000 delayed immutable exposure jobs; sizeof=%zu mask=%zu\n",
           sizeof(nos_exposure_job), sizeof(((nos_exposure_job *)0)->directional));
}
