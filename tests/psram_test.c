#define NOS_PSRAM_TRACKING
#include "psram.h" /* exercise either header order with the tracking macro enabled */
#include "hal.h"
#undef hal_big_alloc
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
void *hal_big_alloc(uint32_t n) { return malloc(n); }
/* Cache lifecycle boundaries are linked explicitly on Windows too. */
void ui_fblur_mem(void) {} void sys_notifs_blur_mem(void) {} void set_settings_cache_mem(void) {}
static int idle_released, card_trimmed, notifications_released, settings_released;
void ui_fblur_idle(void) { idle_released = 1; }
void ui_fblur_release(void) {} void sys_notifs_blur_release(void) { notifications_released++; } void pblur_release(void) {}
void orrery_cache_card_prepare(void) {} void sys_launcher_card_prepare(void) { card_trimmed++; } void set_settings_cache_release(void) { settings_released++; }
void orrery_cache_idle(void) { assert(idle_released); }
void orrery_card_idle(void) { assert(idle_released); }
void set_settings_cache_idle(void) { assert(idle_released); }
void nos_layers_release(void) {} void nos_layers_idle(void) {}
int main(void)
{
    assert(nos_mem_available() == NOS_MEM_BUDGET - 8);
    nos_mem_max_request(1024); assert(nos_mem_available() == 1024);
    nos_mem_max_request(0);
    void *a = nos_mem_alloc(100, "dir/alpha");
    void *b = nos_mem_alloc(200, "dir\\beta");
    assert(a && b && (uintptr_t)a % 8 == 0 && nos_mem_used() == 300);
    nos_mem_stat s;
    assert(nos_mem_stats(&s, 0) == 2 && s.bytes == 100 && s.blocks == 1);
    nos_mem_fail_after(0);
    assert(!nos_mem_alloc(10, "alpha") && nos_mem_failures() == 1);
    assert(!nos_mem_alloc(UINT32_MAX, "alpha"));
    assert(!nos_mem_alloc(NOS_MEM_BUDGET, "alpha"));
    nos_mem_free(a); nos_mem_free(b); nos_mem_free(NULL);
    assert(nos_mem_used() == 0 && nos_mem_peak() == 300);
    a = nos_mem_alloc(NOS_MEM_BUDGET - 8, "alpha");
    assert(a && !nos_mem_alloc(1, "beta"));
    assert(nos_mem_available() == 0);
    nos_mem_free(a);
    assert(!nos_mem_used() && nos_mem_failures() == 4);
    nos_mem_face_prepare(); assert(notifications_released == 1 && settings_released == 1);
    nos_mem_idle(); assert(idle_released);
    nos_mem_card_prepare(); assert(card_trimmed == 1);
    puts("psram: ownership, alignment, overflow, budget, failure and release PASS");
}
