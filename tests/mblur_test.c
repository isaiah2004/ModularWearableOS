#include "mblur.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t rng = 17;
static uint16_t pixel(void) { rng = rng * 1664525u + 1013904223u; return (uint16_t)(rng >> 16); }
static int channel(uint16_t p, int c) { return c == 0 ? p & 31 : c == 1 ? (p >> 5) & 63 : p >> 11; }
static uint16_t rgb(const int *v) { return (uint16_t)(v[0] | v[1] << 5 | v[2] << 11); }

/* Scalar channels, explicit convolution: independent of packed running sums. */
static void horizontal(void)
{
    uint16_t src[GFX_W + 2], dst[GFX_W + 2];
    uint32_t scratch[GFX_W + 40];
    int low[(GFX_W + 3) / 4 + 8][3], blur[(GFX_W + 3) / 4 + 1][3];
    for (int width = 1; width <= GFX_W; width++) for (int k = 2; k <= 5; k++) {
        for (int i = 0; i < width + 2; i++) src[i] = pixel();
        memcpy(dst, src, sizeof src);
        mb_h4(dst + 1, width, k, scratch);
        assert(dst[0] == src[0] && dst[width + 1] == src[width + 1]);
        int n = (width + 3) / 4, len = 1 << (k - 2);
        memset(low, 0, sizeof low); memset(blur, 0, sizeof blur);
        for (int i = 0; i < n; i++) for (int c = 0; c < 3; c++) {
            low[i][c] = channel(src[1 + 4 * i], c);
        }
        for (int i = 0; i <= n; i++) for (int c = 0; c < 3; c++) {
            for (int j = 0; j < len; j++) {
                int x = i - len / 2 + j;
                if (x >= 0 && x < n) blur[i][c] += low[x][c];
            }
            blur[i][c] /= len;
        }
        for (int x = 0; x < width; x++) {
            int v[3], phase = x & 3, i = x / 4;
            for (int c = 0; c < 3; c++) v[c] = ((4 - phase) * blur[i][c] + phase * blur[i + 1][c]) / 4;
            assert(dst[x + 1] == rgb(v));
        }
    }
}

#define HEIGHT 39
static uint16_t src[GFX_W * HEIGHT], ref[GFX_W * HEIGHT], dst[GFX_W * HEIGHT];
static void horizontal_stripes(void)
{
    uint32_t scratch[GFX_W + 40];
    for (int i = 0; i < GFX_W * HEIGHT; i++) src[i] = pixel();
    for (int k = 2; k <= 5; k++) {
        memcpy(ref, src, sizeof ref);
        gfx_fb f = {0}; f.px = ref; f.h = HEIGHT;
        mb_h4_state state = {0}; state.next = -1;
        mb_h4_rows(&f, 3, 462, k, scratch, &state);
        for (int stripe = 1; stripe <= 17; stripe++) {
            memcpy(dst, src, sizeof dst);
            memset(&state, 0, sizeof state); state.next = -1;
            for (int y = 0; y < HEIGHT; y += stripe) {
                f.px = dst + y * GFX_W; f.y_base = y;
                f.h = HEIGHT - y < stripe ? HEIGHT - y : stripe;
                mb_h4_rows(&f, 3, 462, k, scratch, &state);
            }
            assert(!memcmp(ref, dst, sizeof ref));
        }
        for (int y = 0; y < HEIGHT; y++) for (int x = 0; x < GFX_W; x++)
            if (x < 3 || x >= 462) assert(ref[y * GFX_W + x] == src[y * GFX_W + x]);
    }
}
static void vertical(void)
{
    for (int i = 0; i < GFX_W * HEIGHT; i++) src[i] = pixel();
    for (int k = 1; k <= 4; k++) for (int y0 = 0; y0 < 5; y0++) {
        memcpy(ref, src, sizeof ref);
        gfx_fb f = {0}; f.px = ref; f.h = HEIGHT; f.oy = 2;
        mb_v4_state state = {0}; state.next = -1;
        mb_v4(&f, 3, 462, y0, 34, k, &state);
        for (int stripe = 1; stripe <= 17; stripe++) {
            memcpy(dst, src, sizeof dst);
            memset(&state, 0, sizeof state); state.next = -1;
            for (int y = 0; y < HEIGHT; y += stripe) {
                f.px = dst + y * GFX_W; f.y_base = y;
                f.h = HEIGHT - y < stripe ? HEIGHT - y : stripe;
                mb_v4(&f, 3, 462, y0, 34, k, &state);
            }
            assert(!memcmp(ref, dst, sizeof ref));
        }
        for (int y = 0; y < HEIGHT; y++) for (int x = 0; x < GFX_W; x++)
            if (x < 3 || x >= 462 || y - 2 < y0 || y - 2 >= 34) assert(ref[y * GFX_W + x] == src[y * GFX_W + x]);
    }
    /* Pure primaries never leak into another packed channel. */
    for (int c = 0; c < 3; c++) {
        uint16_t mask = c == 0 ? 31 : c == 1 ? 2016 : 63488;
        for (int i = 0; i < GFX_W * HEIGHT; i++) dst[i] = i % 7 ? mask : 0;
        gfx_fb f = {0}; f.px = dst; f.h = HEIGHT;
        mb_v4_state state = {0}; state.next = -1;
        mb_v4(&f, 0, GFX_W, 0, HEIGHT, 4, &state);
        for (int i = 0; i < GFX_W * HEIGHT; i++) assert(!(dst[i] & ~mask));
    }
}
int main(void) { horizontal(); horizontal_stripes(); vertical(); puts("mblur: scalar RGB reference, bounds, channels, H/V stripe continuity PASS"); return 0; }
