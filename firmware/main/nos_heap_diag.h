#ifndef NOS_HEAP_DIAG_H
#define NOS_HEAP_DIAG_H
#include <stdint.h>

typedef struct {
    uint32_t total, dropped, sequence, size, caps;
    const char *function_name;
    int busy;
    uint32_t uptime_ms,cycles,caller_pc,task,core,flags,boot,build;
    uint32_t internal_free,internal_largest,internal_min;
    uint32_t dma_free,dma_largest,dma_min,metrics_ms;
} nos_heap_failure;
enum { NOS_HEAP_CALLER_KNOWN=1, NOS_HEAP_ISR=2, NOS_HEAP_METRICS_CACHED=4 };

void nos_heap_diag_init(void);
/* Nonblocking snapshot. busy=1 means record fields are unavailable. */
nos_heap_failure nos_heap_diag_snapshot(void);
void nos_heap_diag_report(void);
void nos_heap_diag_tick(void); /* ordinary task only: queries/persistence deferred */
/* Slot0 is the immutable first failure; slots1..7 retain the recent suffix.
 * Numeric RTC records survive warm reset; function pointers are never retained. */
int nos_heap_diag_history(unsigned index,nos_heap_failure *out);

enum { NOS_CRASH_DISABLED, NOS_CRASH_EMPTY, NOS_CRASH_VALID, NOS_CRASH_INVALID, NOS_CRASH_IO_ERROR };
typedef struct {
    uint32_t state,bytes,address,pc,sp,cause,vaddr,task,depth,backtrace[16],format_version;
    int32_t error;
    uint8_t backtrace_corrupt;
    char task_name[16],elf_sha256[65];
} nos_crash_info;
void nos_crash_refresh(void); /* ordinary host task, read-only flash and supported SDK parser */
void nos_crash_get(nos_crash_info *out);
void nos_crash_report(void);
int nos_crash_read(uint32_t offset,void *buffer,uint32_t bytes); /* <=1024, count or -1 */
int nos_crash_clear(void); /* explicit operator action only; never called by init/tick/export */
#endif
