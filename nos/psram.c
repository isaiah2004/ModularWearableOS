#include "hal.h"
#include "psram.h"
#include "sprite.h"
#include <stdio.h>
#include <string.h>
#undef hal_big_alloc
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
static portMUX_TYPE mem_lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&mem_lock)
#define UNLOCK() portEXIT_CRITICAL(&mem_lock)
#else
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif
#define OWNERS 64
static nos_mem_stat stats[OWNERS];
static unsigned count;
static uint32_t used, peak, charged;
#ifndef ESP_PLATFORM
static int fail_after = -1;
static uint32_t max_request = UINT32_MAX;
void nos_mem_max_request(uint32_t n) { max_request = n ? n : UINT32_MAX; }
void nos_mem_fail_after(int n) { fail_after = n; }
#endif
/* Keep the returned buffer aligned for doubles on both targets. */
typedef union { struct { uint32_t bytes, owner; } v; double align; } mem_header;
void *nos_mem_alloc(uint32_t bytes, const char *path)
{
    const char *owner = path;
    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') owner = p + 1;
    LOCK();
    unsigned i;
    for (i = 0; i < count; i++) if (!strcmp(stats[i].owner, owner)) break;
    if (i == count && count < OWNERS) { stats[count++].owner = owner; }
    if (i == OWNERS) { UNLOCK(); return NULL; }
    uint32_t charge = bytes + (uint32_t)sizeof(mem_header);
    int deny = charge < bytes || charged > NOS_MEM_BUDGET || charge > NOS_MEM_BUDGET - charged;
#ifndef ESP_PLATFORM
    if (bytes > max_request) deny = 1;
    if (fail_after == 0) { deny = 1; fail_after = -1; }
    else if (fail_after > 0) fail_after--;
#endif
    if (deny) { stats[i].failures++; UNLOCK(); return NULL; }
    charged += charge; /* reserve before dropping the lock for the underlying heap */
    UNLOCK();
#ifdef ESP_PLATFORM
    /* SDK/network heaps also use SPIRAM. Never spend the final MiB on a UI cache. */
    size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    mem_header *h = available >= 1024u * 1024u && charge <= available - 1024u * 1024u ? hal_big_alloc(charge) : NULL;
#else
    mem_header *h = bytes <= UINT32_MAX - sizeof *h ? hal_big_alloc(bytes + sizeof *h) : NULL;
#endif
    LOCK();
    if (!h) { charged -= charge; stats[i].failures++; UNLOCK(); return NULL; }
    h->v.bytes = bytes; h->v.owner = i;
    stats[i].bytes += bytes; stats[i].blocks++; used += bytes;
    if (stats[i].bytes > stats[i].peak) stats[i].peak = stats[i].bytes;
    if (used > peak) peak = used;
    UNLOCK();
    return h + 1;
}
void nos_mem_free(void *p)
{
    if (!p) return;
    mem_header *h = (mem_header *)p - 1;
    LOCK();
    stats[h->v.owner].bytes -= h->v.bytes; stats[h->v.owner].blocks--; used -= h->v.bytes;
    charged -= h->v.bytes + sizeof *h;
    UNLOCK();
    free(h);
}
/* SDK-owned task stacks: persistent, no header, never passed to nos_mem_free. */
void nos_mem_external(const char *owner, uint32_t bytes, int success)
{
    LOCK();
    unsigned i;
    for (i = 0; i < count; i++) if (!strcmp(stats[i].owner, owner)) break;
    if (i == count && count < OWNERS) stats[count++].owner = owner;
    if (i < OWNERS) {
        if (!success) stats[i].failures++;
        else {
            stats[i].bytes += bytes; stats[i].blocks++; used += bytes; charged += bytes;
            if (stats[i].bytes > stats[i].peak) stats[i].peak = stats[i].bytes;
            if (used > peak) peak = used;
        }
    }
    UNLOCK();
}
unsigned nos_mem_stats(nos_mem_stat *out, unsigned i)
{
    LOCK(); unsigned n = count; if (i < n && out) *out = stats[i]; UNLOCK(); return n;
}
uint32_t nos_mem_used(void) { LOCK(); uint32_t n = used; UNLOCK(); return n; }
uint32_t nos_mem_available(void)
{
    LOCK();
    uint32_t room=charged<NOS_MEM_BUDGET?NOS_MEM_BUDGET-charged:0;
    room=room>sizeof(mem_header)?room-(uint32_t)sizeof(mem_header):0;
#ifndef ESP_PLATFORM
    if(room>max_request) room=max_request;
#endif
    UNLOCK();
#ifdef ESP_PLATFORM
    size_t free_bytes=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest=heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t physical=free_bytes>1024u*1024u+sizeof(mem_header)?free_bytes-1024u*1024u-sizeof(mem_header):0;
    size_t contiguous=largest>sizeof(mem_header)?largest-sizeof(mem_header):0;
    if(physical<room) room=(uint32_t)physical;
    if(contiguous<room) room=(uint32_t)contiguous;
#endif
    return room;
}
uint32_t nos_mem_peak(void) { LOCK(); uint32_t n = peak; UNLOCK(); return n; }
uint32_t nos_mem_failures(void)
{
    LOCK(); uint32_t n = 0;
    for (unsigned i = 0; i < count; i++) n += stats[i].failures;
    UNLOCK(); return n;
}
void nos_mem_dump(int failures_only)
{
    nos_mem_stat s;
    printf("MEM tracked=%u peak=%u budget=%u failures=%u\n", (unsigned)nos_mem_used(), (unsigned)nos_mem_peak(),
           NOS_MEM_BUDGET, (unsigned)nos_mem_failures());
    for (unsigned i = 0; i < nos_mem_stats(&s, i); i++) {
        if (failures_only) { printf("MEM_FAIL %s=%u\n", s.owner, (unsigned)s.failures); continue; }
        printf("MEM %-24s bytes=%u peak=%u blocks=%u fail=%u\n", s.owner,
               (unsigned)s.bytes, (unsigned)s.peak, (unsigned)s.blocks, (unsigned)s.failures);
    }
    if (!failures_only) {
        extern void ui_fblur_mem(void), sys_notifs_blur_mem(void), set_settings_cache_mem(void);
        ui_fblur_mem(); sys_notifs_blur_mem(); set_settings_cache_mem();
    }
}
/* UI-thread safe points only: no allocator callback ever evicts live render data. */
void nos_mem_card_prepare(void)
{
    extern void ui_fblur_release(void), sys_notifs_blur_release(void), pblur_release(void);
    extern void orrery_cache_card_prepare(void);
    extern void sys_launcher_card_prepare(void);
    extern void set_settings_cache_release(void);
    extern void nos_layers_release(void);
    nos_layers_release();
    set_settings_cache_release();
    ui_fblur_release(); sys_notifs_blur_release(); pblur_release();
    orrery_cache_card_prepare();
    sys_launcher_card_prepare();
}
void nos_mem_face_prepare(void)
{
    extern void sys_notifs_blur_release(void), set_settings_cache_release(void);
    set_settings_cache_release();
    sys_notifs_blur_release();
}
void nos_mem_idle(void)
{
    extern void orrery_cache_idle(void), orrery_card_idle(void);
    extern void nos_layers_idle(void);
    nos_layers_idle();
    extern void ui_fblur_idle(void);
    ui_fblur_idle(); /* drop generic pixels before native-cache home prewarm */
    extern void set_settings_cache_idle(void);
    set_settings_cache_idle();
    orrery_card_idle(); orrery_cache_idle();
}
#ifdef NOS_PSRAM_TRACKING
int nos_sprite_alloc(sprite *s, int w, int h, const char *owner)
{
    s->w = w; s->h = h; s->px = nos_mem_alloc((uint32_t)w * h * 2u, owner); return s->px != NULL;
}
int nos_sprite8_alloc(sprite8 *s, int w, int h, const char *owner)
{
    s->w = w; s->h = h; s->px = nos_mem_alloc((uint32_t)w * h, owner); return s->px != NULL;
}
#endif
