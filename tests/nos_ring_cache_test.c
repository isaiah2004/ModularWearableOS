/* Pixel/hash equivalence against the original uncached ring renderer.
 * gcc -O2 -std=c11 -Icore -Inos tests/nos_ring_cache_test.c
 *     nos/dot_glyphs.c core/gfx.c -lm -o <temporary-output>
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nos/dot.c"
static int fail_alloc;
void *hal_big_alloc(uint32_t bytes) { return fail_alloc ? NULL : malloc(bytes); }
static void reference_ring(gfx_fb *fb, float cx, float cy, float r, int n, float d,
              float lit0, float lit1, uint16_t on, uint16_t off)
{
    if (n <= 0 || d <= 0.0f) return;
    if (n > 1024) n = 1024;
    lit0 = (float)ifloor(lit0 * 64.0f + 0.5f) / 64.0f;
    lit1 = (float)ifloor(lit1 * 64.0f + 0.5f) / 64.0f;
    if (lit1 < lit0) lit1 = lit0;
    float span = lit1 - lit0;
    if (span > (float)n) { lit0 = 0.0f; lit1 = (float)n; }
    else if (lit0 < 0.0f || lit0 >= (float)n) {          /* normalise start into [0, n) */
        float sh = (float)ifloor(lit0 / (float)n) * (float)n;
        lit0 -= sh; lit1 -= sh;
    }
    float step = 6.2831853f / (float)n, e = d * 0.5f + 1.0f;
    if (fb->rec) {
        int K = n < RING_CHUNKS ? n : RING_CHUNKS;
        for (int ch = 0; ch < K; ch++) {
            int i0 = ch * n / K, i1 = (ch + 1) * n / K;
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            unsigned h = gfx_hash_u(gfx_hash_u(gfx_hash_u(0x6A11u, on), off), (unsigned)(n * 64 + ch));
            h = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(h, cx), cy), r), d);
            for (int i = i0; i < i1; i++) {
                float a = step * (float)i;
                float px = cx + r * gfx_fast_sin(a), py = cy - r * gfx_fast_cos(a);
                if (px < x0) x0 = px;
                if (px > x1) x1 = px;
                if (py < y0) y0 = py;
                if (py > y1) y1 = py;
                float f = lit_frac((float)i, lit0, lit1);
                float f2 = lit_frac((float)(i + n), lit0, lit1);
                h = gfx_hash_f(h, f > f2 ? f : f2);
            }
            gfx_rec_cmd(fb, ifloor(x0 - e), ifloor(y0 - e), iceil(x1 + e), iceil(y1 + e), h);
        }
        return;
    }
    dclip k;
    clip_init(&k, fb);
    float re = r + e;
    if (!clip_hit(&k, cx - re, cy - re, cx + re, cy + re)) return;
    for (int i = 0; i < n; i++) {
        float a = step * (float)i;
        float py = cy - r * gfx_fast_cos(a);
        if (py + e <= (float)k.ylo || py - e >= (float)k.yhi) continue;
        float px = cx + r * gfx_fast_sin(a);
        float f = lit_frac((float)i, lit0, lit1), f2 = lit_frac((float)(i + n), lit0, lit1);
        if (f2 > f) f = f2;
        if (f <= 0.0f && !off) continue;
        lit_dot(&k, px, py, d, f, on, off);
    }
}

static uint16_t actual[GFX_W * GFX_H], expected[GFX_W * GFX_H];
static void compare(int n, int mode)
{
    gfx_rec ar = {0}, er = {0};
    gfx_fb a = {0}, e = {0};
    a.px = actual; e.px = expected;
    a.h = e.h = mode == 1 ? 16 : GFX_H;
    a.y_base = e.y_base = mode == 1 ? 113 : 0;
    a.cx0 = e.cx0 = mode == 2 ? 91 : 0;
    a.cx1 = e.cx1 = mode == 2 ? 317 : GFX_W;
    a.ox = e.ox = mode == 2 ? -23 : 0;
    a.oy = e.oy = mode == 2 ? 17 : 0;
    a.dim = e.dim = mode == 2 ? 9 : 0;
    if (mode == 3) { a.rec = &ar; e.rec = &er; }
    for (int i = 0; i < GFX_W * GFX_H; i++) actual[i] = expected[i] = (uint16_t)(i * 71u);
    dot_ring(&a, 233.25f, 221.5f, 134.375f, n, 5.75f, -2.125f, n * 0.73f, 0xf800, 0x2945);
    reference_ring(&e, 233.25f, 221.5f, 134.375f, n, 5.75f, -2.125f, n * 0.73f, 0xf800, 0x2945);
    assert(!memcmp(actual, expected, sizeof actual));
    assert(!memcmp(&ar, &er, sizeof ar));
}
int main(void)
{
    const int counts[] = {1, 12, 60, 120, 89, 1024, 60, 120, 12};
    for (unsigned i = 0; i < sizeof counts / sizeof counts[0]; i++)
        for (int mode = 0; mode < 4; mode++) compare(counts[i], mode);
    /* Exact unit values across every supported size, exercising eviction. */
    for (int n = 1; n <= 1024; n++) {
        const float *xy = ring_units(n);
        assert(xy);
        float step = 6.2831853f / (float)n;
        for (int i = 0; i < n; i++) {
            float a = step * (float)i;
            assert(xy[2 * i] == gfx_fast_sin(a));
            assert(xy[2 * i + 1] == gfx_fast_cos(a));
        }
    }
    /* A cache allocation failure falls back to the same direct geometry. */
    ring_circle saved = s_ring_circle[0];
    memset(s_ring_circle, 0, sizeof s_ring_circle);
    fail_alloc = 1;
    assert(!ring_units(61));
    for (int mode = 0; mode < 4; mode++) compare(61, mode);
    fail_alloc = 0;
    s_ring_circle[0] = saved;
    puts("ring cache: unit geometry, eviction, allocation fallback, pixels and damage hashes match");
    return 0;
}
