#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "render_history.h"
#define N (GFX_W * GFX_H)
static int allocation_fails = 1;
static uint8_t *allocation;
static uint32_t allocation_size;
void *hal_big_alloc(uint32_t bytes)
{
    if (allocation_fails) return NULL;
    assert(!allocation); allocation_size = bytes;
    allocation = malloc(bytes + 64); assert(allocation);
    memset(allocation, 0xa7, bytes + 64);
    return allocation + 32;
}
static uint16_t sharp[N], previous[N], expected[N], output[N];
static uint16_t reference(uint16_t c, uint16_t p)
{
    unsigned r = (3 * (c >> 11) + (p >> 11) + 2) / 4;
    unsigned g = (3 * ((c >> 5) & 63) + ((p >> 5) & 63) + 2) / 4;
    unsigned b = (3 * (c & 31) + (p & 31) + 2) / 4;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static uint32_t rng = 41;
static uint16_t random_pixel(void) { rng = rng * 1664525u + 1013904223u; return (uint16_t)(rng >> 7); }
static gfx_fb full(uint16_t *pixels)
{
    gfx_fb f = {0}; f.px = pixels; f.h = GFX_H; f.cx1 = GFX_W; return f;
}
static void random_scene(int frame)
{
    for (int i = 0; i < N; i++) {
        sharp[i] = frame ? random_pixel() : 0xffff;
        expected[i] = frame ? reference(sharp[i], previous[i]) : sharp[i];
    }
}
static void equivalence(int mode)
{
    nos_history_reset(); rng = 41;
    for (int frame = 0; frame < 8; frame++) {
        random_scene(frame); memcpy(output, sharp, sizeof output);
        nos_history_begin((uint32_t)frame);
        if (mode == 0) { gfx_fb f = full(output); assert(nos_history_apply(&f, 0, 0, GFX_W, GFX_H, 1)); }
        else if (mode == 1) {
            for (int y = 0; y < GFX_H; y += 16) {
                gfx_fb f = full(output + y * GFX_W); f.y_base = y; f.h = GFX_H - y < 16 ? GFX_H - y : 16;
                f.ox = -31; f.oy = 91; /* already rasterized screen pixels */
                assert(nos_history_apply(&f, -20, y - 9, 700, y + 20, 1));
            }
        } else {
            for (int x = 0; x < GFX_W; x += 29) {
                int w = GFX_W - x < 29 ? GFX_W - x : 29;
                uint16_t band[GFX_H * 29 + 2];
                band[0] = band[GFX_H * w + 1] = 0xbeef;
                for (int y = 0; y < GFX_H; y++) memcpy(band + 1 + y * w, sharp + y * GFX_W + x, w * 2);
                gfx_fb f = full(band + 1); f.stride = w; f.x_base = x;
                assert(nos_history_apply(&f, -33, -200, 900, 900, 1));
                for (int y = 0; y < GFX_H; y++) memcpy(output + y * GFX_W + x, band + 1 + y * w, w * 2);
                assert(band[0] == 0xbeef && band[GFX_H * w + 1] == 0xbeef);
            }
        }
        assert(!memcmp(output, expected, sizeof output));
        const nos_history_stats *s = nos_history_get_stats();
        assert(s->pixels == N && s->duplicates == 0);
        assert(frame ? s->seeded == 0 : s->seeded == N);
        assert(s->seeded + s->unchanged + s->changed == N);
        memcpy(previous, sharp, sizeof previous);
    }
}
static void dirty_tail(void)
{
    nos_history_reset(); memset(output, 0, sizeof output); gfx_fb f = full(output);
    nos_history_begin(50); nos_history_apply(&f, 0, 0, GFX_W, GFX_H, 1);
    nos_history_begin(51); output[13] = 0xffff; nos_history_apply(&f, 13, 0, 14, 1, 1);
    assert(output[13] == reference(0xffff, 0));
    nos_history_begin(52); output[13] = 0; output[14] = 0xf800;
    nos_history_apply(&f, 13, 0, 15, 1, 1);
    assert(output[13] == reference(0, 0xffff));
    assert(output[14] == reference(0xf800, 0));
    nos_history_begin(53); output[13] = 0; output[14] = 0xf800;
    nos_history_apply(&f, 13, 0, 15, 1, 1);
    assert(output[13] == 0 && output[14] == 0xf800); /* nonrecursive, tail erased */
    assert(nos_history_get_stats()->unchanged == 2);
    /* Same frame and overlapping window must not update sharp twice. */
    uint16_t saved = output[14]; nos_history_begin(53);
    nos_history_apply(&f, 14, 0, 15, 1, 1);
    assert(output[14] == saved && nos_history_get_stats()->duplicates == 1);
    nos_history_begin(54); output[14] = 0x07e0;
    nos_history_apply(&f, 14, 0, 15, 1, 0); assert(output[14] == 0x07e0);
    nos_history_begin(55); output[14] = 0x001f;
    nos_history_apply(&f, 14, 0, 15, 1, 1);
    assert(output[14] == reference(0x001f, 0x07e0));
}
static void rounded_channels(void)
{
    nos_history_reset(); gfx_fb f = full(output);
    for (int pass = 0; pass < 6; pass++) {
        for (int i = 0; i < N; i++) output[i] = (uint16_t)(i + pass * 10937u);
        nos_history_begin((uint32_t)(100 + pass));
        memcpy(sharp, output, sizeof output);
        nos_history_apply(&f, 0, 0, GFX_W, GFX_H, 1);
        for (int i = 0; i < N; i++) assert(output[i] == (pass ? reference(sharp[i], previous[i]) : sharp[i]));
        memcpy(previous, sharp, sizeof previous);
    }
    nos_history_begin(200); memcpy(output, previous, sizeof output);
    nos_history_apply(&f, 0, 0, GFX_W, GFX_H, 1);
    const nos_history_stats *s = nos_history_get_stats();
    assert(s->unchanged == N && s->changed == 0 && s->blended == 0);
    assert(s->equal_words == GFX_H * (GFX_W / 2));
    assert(s->rows_cleared == GFX_H);
    nos_history_begin(201); assert(nos_history_get_stats()->rows_cleared == 0);
    nos_history_begin(202); assert(nos_history_get_stats()->rows_cleared == 0);
    nos_history_apply(&f, 5, 7, 9, 8, 1);
    assert(nos_history_get_stats()->rows_cleared == 1);
    nos_history_apply(&f, 20, 7, 21, 8, 1);
    assert(nos_history_get_stats()->rows_cleared == 1);
    nos_history_begin(203); nos_history_apply(&f, 5, 7, 9, 8, 1);
    assert(nos_history_get_stats()->rows_cleared == 1);
}
static void signed_clips(void)
{
    uint16_t band[34]; for (int i = 0; i < 34; i++) band[i] = 0xdead;
    gfx_fb f = {0}; f.px = band + 1; f.stride = 8; f.x_base = -3;
    f.h = 4; f.y_base = -1; f.cx0 = -20; f.cx1 = 3;
    nos_history_reset(); nos_history_begin(300);
    nos_history_apply(&f, -100, -100, 900, 900, 1);
    assert(nos_history_get_stats()->pixels == 9);
    nos_history_begin(301);
    for (int i = 1; i < 33; i++) band[i] = 0xf800;
    nos_history_apply(&f, -100, -100, 900, 900, 1);
    for (int row = 0; row < 4; row++) for (int col = 0; col < 8; col++) {
        int inside = row >= 1 && col >= 3 && col < 6;
        assert(band[1 + row * 8 + col] == (inside ? reference(0xf800, 0xdead) : 0xf800));
    }
    assert(band[0] == 0xdead && band[33] == 0xdead);
    nos_history_begin(302);
    f.x_base = INT32_MAX; f.y_base = INT32_MIN;
    nos_history_apply(&f, INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX, 1);
    assert(nos_history_get_stats()->pixels == 0);
    for (unsigned i = 0; i < 32; i++) {
        assert(allocation[i] == 0xa7);
        assert(allocation[allocation_size + 32 + i] == 0xa7);
    }
}
int main(void)
{
    gfx_fb f = full(output);
    assert(!nos_history_init()); assert(nos_history_storage_bytes() == 0); assert(!nos_history_apply(&f, 0, 0, 1, 1, 1));
    allocation_fails = 0; assert(nos_history_init()); assert(nos_history_init());
    assert(nos_history_storage_bytes() == allocation_size);
    equivalence(0); equivalence(1); equivalence(2);
    dirty_tail(); rounded_channels(); signed_clips();
    printf("history: pixels, rounded channels, stripes, bands, dirty tails, overlap, bounds and stats match (%u bytes)\n", allocation_size);
    free(allocation); return 0;
}
