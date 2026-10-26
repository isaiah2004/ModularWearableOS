/* Permanent candidate: include production implementation to test private packed kernel. */
#include "../nos/render_history.c"
#include <assert.h>
#include <stdio.h>
void *hal_big_alloc(uint32_t n) { return malloc(n); }
static uint32_t seed = 14717;
static uint32_t rnd(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}
static uint16_t scalar(uint16_t c, uint16_t p) {
    unsigned r = (3 * (c >> 11) + (p >> 11) + 2) / 4,
             g = (3 * ((c >> 5) & 63) + ((p >> 5) & 63) + 2) / 4,
             b = (3 * (c & 31) + (p & 31) + 2) / 4;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
static uint32_t reference(uint32_t c, uint32_t p) {
    return scalar(c, p) | ((uint32_t)scalar(c >> 16, p >> 16) << 16);
}
int main(void) {
    unsigned cases = 0;
    for (int lane = 0; lane < 2; lane++)
        for (int ch = 0; ch < 3; ch++) {
            int shift = ch == 0 ? 0 : ch == 1 ? 5 : 11, max = ch == 1 ? 64 : 32;
            for (int c = 0; c < max; c++)
                for (int p = 0; p < max; p++) {
                    uint32_t cw = (uint32_t)c << (shift + 16 * lane),
                             pw = (uint32_t)p << (shift + 16 * lane);
                    assert(expose_pair(cw, pw) == reference(cw, pw));
                    cases++;
                }
        }
    for (unsigned i = 0; i < 10000000; i++) {
        uint32_t c = rnd(), p = rnd();
        assert(expose_pair(c, p) == reference(c, p));
        cases++;
    }
    _Alignas(4) uint16_t a[70], b[70], pa[70], pb[70];
    for (int trial = 0; trial < 20000; trial++) {
        int count = 1 + rnd() % 65, offset = trial & 1, enabled = (trial >> 1) & 1;
        for (int i = 0; i < 70; i++) {
            a[i] = b[i] = (uint16_t)rnd();
            pa[i] = pb[i] = (trial % 3) ? (uint16_t)rnd() : a[i];
        }
        memset(&s_stats, 0, sizeof s_stats);
        nos_history_packed = 0;
        ready_span(a + offset, pa + offset, count, enabled);
        nos_history_stats old = s_stats;
        memset(&s_stats, 0, sizeof s_stats);
        nos_history_packed = 1;
        ready_span(b + offset, pb + offset, count, enabled);
        assert(!memcmp(a, b, sizeof a));
        assert(!memcmp(pa, pb, sizeof pa));
        assert(!memcmp(&old, &s_stats, sizeof old));
    }
    printf("PASS packed %u channel/mixed words and20000 aligned/unaligned enabled/disabled spans, "
           "exact stats\n",
           cases);
}
