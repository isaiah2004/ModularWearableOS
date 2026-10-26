#include "orrery_shade_q16.h"
#include "psram.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

typedef struct {
    float coefficients[512];
    uint32_t multipliers[512];
    uint8_t lighting[512];
} shade_cache;
_Static_assert(sizeof(shade_cache) == 4608, "fixed shade cache payload");
static atomic_int cache_state = ATOMIC_VAR_INIT(ORRERY_SHADE_Q16_EMPTY);
static shade_cache *published;
static unsigned bake_result = ~0u;
static int rejection_reason;

/* Original night-free shader branches. Green covers every possible truncating
 * channel value (1..63); gray covers all rounded five-bit values (1..31).
 * Checking both proves every RGB565 value, including green variants selected
 * by the original near-gray condition. No float reciprocal approximation. */
static unsigned original_green(unsigned value, float coefficient) {
    return (unsigned)(int)(value * coefficient + 0.0f);
}
static unsigned original_gray(unsigned value, float coefficient) {
    return (unsigned)(int)(value * coefficient + .5f);
}
static int multiplier_matches(float coefficient, uint32_t multiplier) {
    for (unsigned value = 1; value < 64; value++)
        if (original_green(value, coefficient) != ((value * multiplier) >> 16))
            return 0;
    for (unsigned value = 1; value < 32; value++)
        if (original_gray(value, coefficient) != ((value * multiplier + 32768u) >> 16))
            return 0;
    return 1;
}
static int derive_multiplier(float coefficient, uint32_t *out) {
    if (!(coefficient >= 0.0f && coefficient <= 1.0f))
        return 0;
    uint32_t candidate = (uint32_t)(coefficient * 65536.0f);
    if (multiplier_matches(coefficient, candidate)) {
        *out = candidate;
        return 1;
    }
    if (candidate < 65536u && multiplier_matches(coefficient, candidate + 1u)) {
        *out = candidate + 1u;
        return 1;
    }
    return 0;
}
unsigned orrery_shade_q16_prebake(void) {
    int expected = ORRERY_SHADE_Q16_EMPTY;
    if (!atomic_compare_exchange_strong_explicit(&cache_state, &expected, ORRERY_SHADE_Q16_BUILDING,
                                                 memory_order_acq_rel, memory_order_acquire))
        return expected == ORRERY_SHADE_Q16_READY      ? 0u
               : expected == ORRERY_SHADE_Q16_REJECTED ? bake_result
                                                       : ~0u;
    shade_cache *cache = NULL;
    unsigned mismatches = 0;
    /* Allocator admission reports payload capacity. Retain16KiB plus one
     * allocator header; failure is terminal rather than retrying each frame. */
    if (nos_mem_available() < sizeof(shade_cache) + 16384u + 8u) {
        rejection_reason = 1;
        goto rejected;
    }
    cache = nos_mem_alloc(sizeof *cache, "card.shade_global");
    if (!cache) {
        rejection_reason = 2;
        goto rejected;
    }
    for (unsigned i = 0; i < 256; i++) {
        uint8_t planet = (uint8_t)(255 * powf(i / 255.f, .85f) + .5f);
        uint8_t sun = (uint8_t)(255 * powf(i / 255.f, .6f) + .5f);
        cache->lighting[i] = planet;
        cache->lighting[256 + i] = sun;
        cache->coefficients[i] = .05f + .95f * planet / 255.f;
        cache->coefficients[256 + i] = .55f + .45f * sun / 255.f;
    }
    for (unsigned i = 0; i < 512; i++)
        mismatches += !derive_multiplier(cache->coefficients[i], &cache->multipliers[i]);
    if (mismatches) {
        rejection_reason = 3;
        bake_result = mismatches;
        goto rejected;
    }
    /* Complete validation precedes publication. Get/state acquire this release;
     * nobody ever mutates or frees a successfully published cache. */
    published = cache;
    atomic_store_explicit(&cache_state, ORRERY_SHADE_Q16_READY, memory_order_release);
    return 0;
rejected:
    if (cache)
        nos_mem_free(cache);
    atomic_store_explicit(&cache_state, ORRERY_SHADE_Q16_REJECTED, memory_order_release);
    return bake_result;
}
void orrery_shade_q16_prewarm(void) { (void)orrery_shade_q16_prebake(); }
int orrery_shade_q16_ready(void) { return orrery_shade_q16_state() == ORRERY_SHADE_Q16_READY; }
int orrery_shade_q16_rejection(void) {
    return orrery_shade_q16_state() == ORRERY_SHADE_Q16_REJECTED ? rejection_reason : 0;
}
const uint32_t *orrery_shade_q16_get(const float coefficients[512]) {
    if (!coefficients ||
        atomic_load_explicit(&cache_state, memory_order_acquire) != ORRERY_SHADE_Q16_READY)
        return NULL;
    const shade_cache *cache = published;
    return memcmp(cache->coefficients, coefficients, sizeof cache->coefficients)
               ? NULL
               : cache->multipliers;
}
int orrery_shade_q16_state(void) {
    return atomic_load_explicit(&cache_state, memory_order_acquire);
}
const uint8_t *orrery_shade_lighting_get(void) {
    if (atomic_load_explicit(&cache_state, memory_order_acquire) != ORRERY_SHADE_Q16_READY)
        return NULL;
    return published->lighting;
}
