/* Independent original shader oracle plus real-thread publication/lifetime cases. */
#include "../nos/faces/orrery_shade_q16.c"
#include <assert.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#define YIELD() SwitchToThread()
#else
#include <pthread.h>
#include <sched.h>
#define YIELD() sched_yield()
#endif
static unsigned available = UINT32_MAX;
static atomic_uint allocations, frees;
static atomic_int pause_allocator, allocator_entered, allow_allocator;
static int fail_allocation;
static float coefficients[512];
static uint8_t original_lighting[512];
uint32_t nos_mem_available(void) { return available; }
void *nos_mem_alloc(uint32_t bytes, const char *owner) {
    assert(bytes == 4608 && !strcmp(owner, "card.shade_global"));
    atomic_fetch_add(&allocations, 1);
    if (fail_allocation)
        return NULL;
    if (atomic_load(&pause_allocator)) {
        atomic_store(&allocator_entered, 1);
        while (!atomic_load(&allow_allocator))
            YIELD();
    }
    return malloc(bytes);
}
void nos_mem_free(void *pointer) {
    assert(orrery_shade_q16_state() != ORRERY_SHADE_Q16_READY);
    atomic_fetch_add(&frees, 1);
    free(pointer);
}
static void original_coefficients(void) {
    for (unsigned i = 0; i < 256; i++) {
        uint8_t planet = (uint8_t)(255 * powf(i / 255.f, .85f) + .5f);
        uint8_t sun = (uint8_t)(255 * powf(i / 255.f, .6f) + .5f);
        original_lighting[i] = planet;
        original_lighting[256 + i] = sun;
        coefficients[i] = .05f + .95f * planet / 255.f;
        coefficients[256 + i] = .55f + .45f * sun / 255.f;
    }
}
static uint16_t original_shade(uint16_t color, float k) {
    unsigned r = color >> 11, g = (color >> 5) & 63, b = color & 31;
    if (r == b && abs((int)g / 2 - (int)r) <= 1) {
        int v = (int)(r * k + .5f);
        return (uint16_t)(v << 11 | (v * 2 + (v >= 16)) << 5 | v);
    }
    int red = (int)(r * k), green = (int)(g * k), blue = (int)(b * k);
    return (uint16_t)(red << 11 | green << 5 | blue);
}
static uint16_t q16_shade(uint16_t color, uint32_t multiplier) {
    unsigned r = color >> 11, g = (color >> 5) & 63, b = color & 31;
    if (r == b && abs((int)(g / 2) - (int)r) <= 1) {
        unsigned v = (r * multiplier + 32768u) >> 16;
        return (uint16_t)(v << 11 | (v * 2 + (v >= 16)) << 5 | v);
    }
    return (uint16_t)(((r * multiplier) >> 16) << 11 | ((g * multiplier) >> 16) << 5 |
                      ((b * multiplier) >> 16));
}
static atomic_uint first_result, second_result;
#ifdef _WIN32
static DWORD WINAPI builder(void *which)
#else
static void *builder(void *which)
#endif
{
    unsigned result = orrery_shade_q16_prebake();
    atomic_store(which ? &second_result : &first_result, result);
    return 0;
}
static void concurrent_build(void) {
    atomic_store(&pause_allocator, 1);
#ifdef _WIN32
    HANDLE first = CreateThread(NULL, 0, builder, NULL, 0, NULL);
    assert(first);
#else
    pthread_t first;
    assert(!pthread_create(&first, NULL, builder, NULL));
#endif
    while (!atomic_load(&allocator_entered))
        YIELD();
    assert(orrery_shade_q16_state() == ORRERY_SHADE_Q16_BUILDING);
    assert(!orrery_shade_q16_ready() && !orrery_shade_q16_get(coefficients));
    assert(!orrery_shade_lighting_get());
    assert(orrery_shade_q16_prebake() == ~0u);
#ifdef _WIN32
    HANDLE second = CreateThread(NULL, 0, builder, (void *)1, 0, NULL);
    assert(second);
    assert(WaitForSingleObject(second, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(second);
#else
    pthread_t second;
    assert(!pthread_create(&second, NULL, builder, (void *)1));
    assert(!pthread_join(second, NULL));
#endif
    assert(atomic_load(&second_result) == ~0u && atomic_load(&allocations) == 1);
    atomic_store(&allow_allocator, 1);
#ifdef _WIN32
    assert(WaitForSingleObject(first, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(first);
#else
    assert(!pthread_join(first, NULL));
#endif
    assert(!atomic_load(&first_result) && orrery_shade_q16_ready());
    assert(atomic_load(&allocations) == 1 && !atomic_load(&frees));
}
int main(int argc, char **argv) {
    assert(argc == 2);
    original_coefficients();
    assert(orrery_shade_q16_state() == ORRERY_SHADE_Q16_EMPTY);
    assert(!orrery_shade_q16_get(coefficients) && !orrery_shade_q16_get(NULL));
    assert(!orrery_shade_lighting_get());
    if (!strcmp(argv[1], "admission"))
        available = 4608 + 16384 + 7;
    if (!strcmp(argv[1], "allocfail"))
        fail_allocation = 1;
    if (!strcmp(argv[1], "boundary"))
        available = 4608 + 16384 + 8;
    if (!strcmp(argv[1], "concurrent"))
        concurrent_build();
    else {
        unsigned result = orrery_shade_q16_prebake();
        if (!strcmp(argv[1], "admission") || fail_allocation) {
            assert(result == ~0u && orrery_shade_q16_state() == ORRERY_SHADE_Q16_REJECTED);
            assert(orrery_shade_q16_rejection() == (fail_allocation ? 2 : 1));
            unsigned calls = atomic_load(&allocations);
            fail_allocation = 0;
            available = UINT32_MAX;
            assert(orrery_shade_q16_prebake() == ~0u && !orrery_shade_q16_get(coefficients));
            assert(!orrery_shade_lighting_get());
            assert(atomic_load(&allocations) == calls && !atomic_load(&frees));
            puts("PASS terminal admission/allocation rejection without retry flood");
            return 0;
        }
        assert(!result && orrery_shade_q16_ready());
    }
    const uint32_t *multipliers = orrery_shade_q16_get(coefficients);
    assert(multipliers);
    const uint8_t *lighting = orrery_shade_lighting_get();
    assert(lighting && !memcmp(lighting, original_lighting, sizeof original_lighting));
    assert(!orrery_shade_q16_rejection());
    uint32_t bits;
    memcpy(&bits, &coefficients[123], 4);
    bits ^= 1;
    memcpy(&coefficients[123], &bits, 4);
    assert(!orrery_shade_q16_get(coefficients));
    original_coefficients();
    assert(orrery_shade_q16_get(coefficients) == multipliers);
    uint32_t unused;
    assert(!derive_multiplier(NAN, &unused));
    assert(!derive_multiplier(-1, &unused));
    assert(!derive_multiplier(2, &unused));
    assert(!multiplier_matches(coefficients[0], 65536));
    for (unsigned i = 0; i < 20; i++) {
        assert(!orrery_shade_q16_prebake());
        orrery_shade_q16_prewarm();
    }
    assert(atomic_load(&allocations) == 1 && !atomic_load(&frees));
    assert(orrery_shade_lighting_get() == lighting);
    assert(!memcmp(lighting, original_lighting, sizeof original_lighting));
    if (!strcmp(argv[1], "exhaustive")) {
        for (unsigned i = 0; i < 512; i++)
            for (unsigned color = 0; color < 65536; color++) {
                uint16_t a = original_shade((uint16_t)color, coefficients[i]);
                uint16_t b = q16_shade((uint16_t)color, multipliers[i]);
                if (a != b) {
                    printf("MISMATCH coefficient%u color%u original%u q16%u\n", i, color, a, b);
                    return 1;
                }
            }
        puts("PASS33554432 independent original RGB565/neargray comparisons");
    }
    puts("PASS immutable Q16 table, exact coefficient rejection, one allocation, permanent "
         "lifetime");
}
