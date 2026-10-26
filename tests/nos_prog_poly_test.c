/* Polygon row spans versus the independent scalar renderer, including counters. */
#include "gfx_cost.h"
static unsigned long cost_px[GFX_COST_NCLASS], cost_calls[GFX_COST_NCLASS];
void gfx_cost_target(int psram) { (void)psram; }
void gfx_cost_add(int cls, unsigned px, int calls) {
    cost_px[cls] += px;
    cost_calls[cls] += (unsigned)calls;
}
void gfx_cost_psram_edge(unsigned px) { (void)px; }
/* Reuse the ring oracle's allocation fixtures, scalar reference, and RGB buffers.
 * Running it also verifies that the polygon change preserves every ring style. */
#define main run_ring_geometry_oracle
#include "nos_prog_geometry_test.c"
#undef main

int main(void)
{
    assert(!run_ring_geometry_oracle());
    const int ns[] = { 3, 4, 6, 12, 16 };
    unsigned cases = 0;
    for (int shape = 0; shape < 10; shape++) for (int z = 0; z < 3000; z++) {
        gfx_fb f = {0};
        f.h = z % 3 == 0 ? 1 : z % 3 == 1 ? 16 : 31;
        f.y_base = (int)(random_word() % 500) - 17;
        f.ox = (int)(random_word() % 65) - 32;
        f.oy = (int)(random_word() % 65) - 32;
        f.dim = z % 4 == 0 ? 13 : 0;
        f.cx0 = (int)(random_word() % 300);
        f.cx1 = f.cx0 + 1 + (int)(random_word() % 166);
        if (z % 3 == 0) { f.x_base = f.cx0; f.stride = f.cx1 - f.cx0; }
        float xy[32];
        float cx = f.cx0 + (float)(f.cx1 - f.cx0) * 0.5f - f.ox;
        float cy = f.y_base - f.oy + f.h * 0.5f;
        cx += (int)(random_word() % 257) - 128 + (int)(random_word() % 1024) / 1024.0f;
        cy += (int)(random_word() % 65) - 32 + (int)(random_word() % 1024) / 1024.0f;
        int n = ns[z % 5];
        float rx = 1 + (random_word() % 250), ry = 0.125f + (random_word() % 200);
        float rot = (random_word() % 4096) * 6.2831853f / 4096.0f;
        for (int i = 0; i < n; i++) {
            float u = 6.2831853f * i / n, x = rx * cosf(u), y = ry * sinf(u);
            if (shape == 1) { x = (i % 2 ? 1 : -1) * 0.00001f; y = ry * i / n; }
            if (shape == 2) { x = 0; y = 0; }
            if (shape == 3) { x = i * 5.0f; y = i * 5.0f; }
            if (shape == 4) { x = (i % 2 ? 1 : -1) * 0.5f; y = ry * i / n; }
            if (shape == 5) { x = (i % 2 ? 1 : -1) * 0.49999997f; y = ry * i / n; }
            if (shape == 6 && i % 2) {
                x = xy[2 * (i - 1)] - cx;
                y = xy[2 * (i - 1) + 1] - cy;
                rot = 0;
            }
            if (shape == 7) { x *= 0.001f; y *= 0.001f; }
            if (shape == 8) { x *= 0.01f; y *= 0.01f; }
            if (shape == 9) { x *= 1 + (i % 3) * 0.25f; y *= 1 + (i % 4) * 0.125f; }
            xy[2 * i] = cx + x * cosf(rot) - y * sinf(rot);
            xy[2 * i + 1] = cy + x * sinf(rot) + y * cosf(rot);
        }
        for (unsigned i = 0; i < sizeof a / sizeof *a; i++) a[i] = b[i] = (uint16_t)random_word();
        uint16_t ink = (uint16_t)random_word();
        memset(cost_px, 0, sizeof cost_px); memset(cost_calls, 0, sizeof cost_calls);
        f.px = a;
        prog_poly_reference(&f, xy, n, ink);
        unsigned long px[GFX_COST_NCLASS], calls[GFX_COST_NCLASS];
        memcpy(px, cost_px, sizeof px); memcpy(calls, cost_calls, sizeof calls);
        memset(cost_px, 0, sizeof cost_px); memset(cost_calls, 0, sizeof cost_calls);
        f.px = b;
        prog_poly(&f, xy, n, ink);
        if (memcmp(a, b, sizeof a) || memcmp(px, cost_px, sizeof px) ||
            memcmp(calls, cost_calls, sizeof calls)) {
            fprintf(stderr, "POLY MISMATCH shape%d case%d n%d\n", shape, z, n);
            abort();
        }
        cases++;
    }
    printf("PASS %u polygon property cases: exact pixels/counters, clips/packed/dim/transforms, degenerate/thin/nonconvex\n", cases);
    return 0;
}
