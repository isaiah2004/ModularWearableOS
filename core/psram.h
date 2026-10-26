#ifndef NOS_PSRAM_H
#define NOS_PSRAM_H
#include <stdint.h>
#include <stdlib.h>
/* Payload accounting; allocator headers and SDK heaps are reported separately. */
typedef struct { const char *owner; uint32_t bytes, peak, blocks, failures; } nos_mem_stat;
void *nos_mem_alloc(uint32_t bytes, const char *owner);
void nos_mem_free(void *p);
void nos_mem_external(const char *owner, uint32_t bytes, int success);
unsigned nos_mem_stats(nos_mem_stat *out, unsigned index);
uint32_t nos_mem_used(void);
uint32_t nos_mem_available(void); /* maximum currently admissible payload, not a reservation */
uint32_t nos_mem_peak(void);
#define NOS_MEM_BUDGET (6656u * 1024u)
uint32_t nos_mem_failures(void);
#ifndef ESP_PLATFORM
void nos_mem_max_request(uint32_t n);
void nos_mem_fail_after(int n); /* simulator fault injection; -1 disables */
#endif
void nos_mem_dump(int failures_only);
void nos_mem_card_prepare(void);
/* Settled foreground interactive face only, before tick/draw: inactive list caches. */
void nos_mem_face_prepare(void);
void nos_mem_idle(void);
#ifdef NOS_PSRAM_TRACKING
#define hal_big_alloc(n) nos_mem_alloc((n), __FILE__)
#define hal_big_free(p) nos_mem_free(p)
#else
#define hal_big_free(p) free(p)
#endif
#endif
