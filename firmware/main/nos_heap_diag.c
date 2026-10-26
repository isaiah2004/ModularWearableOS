#include "nos_heap_diag.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "../../nos/diag_log.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "xt_utils.h"
#include "xtensa/xtruntime.h"
#include "esp_cpu.h"
#include "esp_cpu_utils.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_core_dump.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#ifndef RTC_NOINIT_ATTR
#define RTC_NOINIT_ATTR
#endif
#endif

/* Internal DRAM and IRAM: allocator failures can occur on either core, in an
 * ISR, or with the flash cache unavailable. Never dereference function_name in
 * the hook: IDF supplies static __func__ strings, printed later by info.
 * This observes final failed API requests, not successful fallback attempts. */
static DRAM_ATTR uint32_t s_lock, s_total[2], s_dropped[2];
static DRAM_ATTR uint32_t s_capture_sequence;
static DRAM_ATTR nos_heap_failure s_last;
typedef struct {uint32_t task,caller_pc,depth;} allocation_context;
#define CONTEXT_CAP 16u
static DRAM_ATTR allocation_context s_context[CONTEXT_CAP];
static DRAM_ATTR nos_heap_failure s_metrics;
#define HEAP_RETAIN_MAGIC 0x48463231u
#define HEAP_RETAIN_CAP 8u
typedef struct {uint32_t magic,boot,build,check;} heap_retained_header;
static RTC_NOINIT_ATTR heap_retained_header s_heap_header;
static RTC_NOINIT_ATTR nos_heap_failure s_history[HEAP_RETAIN_CAP];
static RTC_NOINIT_ATTR uint32_t s_history_check[HEAP_RETAIN_CAP];
static DRAM_ATTR uint32_t s_boot,s_build;
static uint32_t s_last_tick,s_reported;
static DRAM_ATTR int s_ticked;
static int s_poll_attempted,s_crash_checked;
static nos_crash_info s_crash;
#ifndef ESP_PLATFORM
static uint32_t s_test_clock_ms;
#endif
static DRAM_ATTR int s_registration = -1;
/* S3 internal aligned word loads/stores are atomic. Use native CAS directly:
 * the configured generic stdatomic PSRAM workaround may otherwise take locks.
 * The callback masks local allocator-capable interrupts only for its bounded
 * instructions. Its per-core counters have one writer; another core only reads.
 * High-level (4+) interrupt handlers must not call the SDK heap API. */
#ifdef ESP_PLATFORM
static inline uint32_t load32(uint32_t *p) {
    uint32_t v;
    __asm__ volatile("memw; l32i %0, %1, 0" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static inline void store32(uint32_t *p,uint32_t v) {
    __asm__ volatile("s32i %0, %1, 0; memw" :: "r"(v),"r"(p) : "memory");
}
static inline int try_lock(void) {
    __asm__ volatile("memw" ::: "memory");
    int ok=xt_utils_compare_and_set(&s_lock,0,1);
    __asm__ volatile("memw" ::: "memory");
    return ok;
}
static inline void increment(uint32_t *p) {store32(p,load32(p)+1);}
#else
static inline uint32_t load32(uint32_t *p) {return __atomic_load_n(p,__ATOMIC_ACQUIRE);}
static inline void store32(uint32_t *p,uint32_t v) {__atomic_store_n(p,v,__ATOMIC_RELEASE);}
static inline int try_lock(void) {uint32_t zero=0;return __atomic_compare_exchange_n(&s_lock,&zero,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);}
static inline void increment(uint32_t *p) {__atomic_add_fetch(p,1,__ATOMIC_RELAXED);}
#endif

static inline uint32_t hook_ms(void)
{
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time()/1000);
#else
    return s_test_clock_ms;
#endif
}
static inline uint32_t current_task_key(unsigned core,int *isr)
{
#ifdef ESP_PLATFORM
    *isr=xPortInIsrContext()!=0;
    return *isr?UINT32_MAX-core:(uint32_t)(uintptr_t)xTaskGetCurrentTaskHandle();
#else
    (void)core;*isr=0;return 1;
#endif
}
static uint32_t IRAM_ATTR failure_check(const nos_heap_failure *f)
{
    /* Numeric fields only: neither dereference nor retain a flash-string pointer. */
    return 0x7c51f0a3u^f->sequence^f->size^f->caps^f->uptime_ms^f->cycles^
        f->caller_pc^f->task^f->core^f->flags^f->boot^f->build^
        f->internal_free^f->internal_largest^f->internal_min^
        f->dma_free^f->dma_largest^f->dma_min^f->metrics_ms;
}
static void IRAM_ATTR history_store(unsigned index,const nos_heap_failure *value)
{
    uint32_t sequence=value->sequence;s_history[index].sequence=0;
    const size_t at=offsetof(nos_heap_failure,sequence);
    memcpy(&s_history[index],value,at);
    memcpy((unsigned char *)&s_history[index]+at+4,(const unsigned char *)value+at+4,sizeof *value-at-4);
    s_history_check[index]=failure_check(value);__sync_synchronize();
    s_history[index].sequence=sequence;
}

static void IRAM_ATTR allocation_failed(size_t size, uint32_t caps, const char *function_name)
{
#ifdef ESP_PLATFORM
    unsigned saved=XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL);
    unsigned core=xt_utils_get_core_id();
#else
    unsigned core=0;
#endif
    increment(&s_total[core]);
    if (!try_lock()) {
        increment(&s_dropped[core]);
#ifdef ESP_PLATFORM
        XTOS_RESTORE_INTLEVEL(saved);
#endif
        return;
    }
    uint32_t sequence=++s_capture_sequence;if(!sequence)sequence=++s_capture_sequence;
    int isr=0;uint32_t task=current_task_key(core,&isr);
    nos_heap_failure record={.sequence=sequence,.size=(uint32_t)size,.caps=caps,
        .function_name=function_name,.uptime_ms=hook_ms(),.task=task,.core=core,
        .boot=s_boot,.build=s_build,.flags=isr?NOS_HEAP_ISR:0};
#ifdef ESP_PLATFORM
    record.cycles=esp_cpu_get_cycle_count();
#endif
    for(unsigned i=0;i<CONTEXT_CAP;i++)if(s_context[i].depth && s_context[i].task==task) {
        record.caller_pc=s_context[i].caller_pc;record.flags|=NOS_HEAP_CALLER_KNOWN;break;
    }
    record.internal_free=s_metrics.internal_free;record.internal_largest=s_metrics.internal_largest;
    record.internal_min=s_metrics.internal_min;record.dma_free=s_metrics.dma_free;
    record.dma_largest=s_metrics.dma_largest;record.dma_min=s_metrics.dma_min;
    record.metrics_ms=s_metrics.metrics_ms;
    if(s_ticked)record.flags|=NOS_HEAP_METRICS_CACHED;
    s_last=record;record.function_name=NULL;
    if(!s_history[0].sequence || s_history_check[0]!=failure_check(&s_history[0]))history_store(0,&record);
    history_store(1u+(sequence-1u)%(HEAP_RETAIN_CAP-1u),&record);
    store32(&s_lock,0);
#ifdef ESP_PLATFORM
    XTOS_RESTORE_INTLEVEL(saved);
#endif
}

void nos_heap_diag_init(void)
{
    int cold=0;
#ifdef ESP_PLATFORM
    cold=esp_reset_reason()==ESP_RST_POWERON;
    const esp_app_desc_t *app=esp_app_get_description();
    memcpy(&s_build,app->app_elf_sha256,sizeof s_build);
#endif
    uint32_t check=HEAP_RETAIN_MAGIC^s_heap_header.boot^s_heap_header.build^0x137ad923u;
    if(cold || s_heap_header.magic!=HEAP_RETAIN_MAGIC || s_heap_header.check!=check) {
        memset(s_history,0,sizeof s_history);memset(s_history_check,0,sizeof s_history_check);s_boot=1;
    } else {s_boot=s_heap_header.boot+1u;if(!s_boot)s_boot=1;}
    s_heap_header.magic=0;s_heap_header.boot=s_boot;s_heap_header.build=s_build;
    s_heap_header.check=HEAP_RETAIN_MAGIC^s_boot^s_build^0x137ad923u;
    __sync_synchronize();s_heap_header.magic=HEAP_RETAIN_MAGIC;
    /* Register once, before the first application allocation. No heap use. */
    s_registration = heap_caps_register_failed_alloc_callback(allocation_failed);
}

nos_heap_failure nos_heap_diag_snapshot(void)
{
    nos_heap_failure result = {0};
    if (try_lock()) {
        result=s_last;
        store32(&s_lock,0);
    } else result.busy = 1;
    result.total = load32(&s_total[0])+load32(&s_total[1]);
    result.dropped = load32(&s_dropped[0])+load32(&s_dropped[1]);
    return result;
}
int nos_heap_diag_history(unsigned index,nos_heap_failure *out)
{
    if(index>=HEAP_RETAIN_CAP || !out || !try_lock())return 0;
    nos_heap_failure f=s_history[index];uint32_t check=s_history_check[index];store32(&s_lock,0);
    if(!f.sequence || check!=failure_check(&f))return 0;
    f.function_name=NULL;*out=f;return 1;
}

#if defined(ESP_PLATFORM) || defined(NOS_HEAP_DIAG_TEST)
typedef struct {int index;uint32_t previous,task;} heap_scope;
static heap_scope IRAM_ATTR scope_begin(uint32_t pc)
{
    heap_scope scope={.index=-1};
#ifdef ESP_PLATFORM
    unsigned saved=XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL),core=xt_utils_get_core_id();
#else
    unsigned core=0;
#endif
    int isr=0;scope.task=current_task_key(core,&isr);
    if(try_lock()) {
        int empty=-1;
        for(unsigned i=0;i<CONTEXT_CAP;i++) {
            if(!s_context[i].depth) {if(empty<0)empty=(int)i;continue;}
            if(s_context[i].task==scope.task) {scope.index=(int)i;break;}
        }
        if(scope.index<0)scope.index=empty;
        if(scope.index>=0) {
            allocation_context *c=&s_context[scope.index];scope.previous=c->caller_pc;
            c->task=scope.task;c->caller_pc=pc;c->depth++;
        }
        store32(&s_lock,0);
    }
#ifdef ESP_PLATFORM
    XTOS_RESTORE_INTLEVEL(saved);
#endif
    return scope;
}
static void IRAM_ATTR scope_end(heap_scope scope)
{
    if(scope.index<0)return;
#ifdef ESP_PLATFORM
    unsigned saved=XTOS_SET_INTLEVEL(XCHAL_EXCM_LEVEL);
#endif
    if(try_lock()) {
        allocation_context *c=&s_context[scope.index];
        if(c->task==scope.task && c->depth) {
            c->depth--;c->caller_pc=c->depth?scope.previous:0;
        }
        store32(&s_lock,0);
    } else {
        /* Never leave a stale requesting PC attached to later unwrapped calls.
         * Only this task can end this still-active slot; aligned depth store is
         * enough to make concurrent readers conservatively report unknown. */
        allocation_context *c=&s_context[scope.index];
        if(c->task==scope.task)store32(&c->depth,0);
    }
#ifdef ESP_PLATFORM
    XTOS_RESTORE_INTLEVEL(saved);
#endif
}
#ifdef ESP_PLATFORM
#define REQUEST_PC() ((uint32_t)esp_cpu_get_call_addr(esp_cpu_process_stack_pc((uint32_t)(uintptr_t)__builtin_return_address(0))))
#else
#define REQUEST_PC() 0x40001234u
#endif
void *__real_heap_caps_malloc(size_t size,uint32_t caps);
void *__real_heap_caps_calloc(size_t count,size_t size,uint32_t caps);
void *__real_heap_caps_realloc(void *ptr,size_t size,uint32_t caps);
void *__real_heap_caps_aligned_alloc(size_t alignment,size_t size,uint32_t caps);
void *IRAM_ATTR __wrap_heap_caps_malloc(size_t size,uint32_t caps)
{
    if(s_registration!=0)return __real_heap_caps_malloc(size,caps);
    heap_scope scope=scope_begin(REQUEST_PC());void *p=__real_heap_caps_malloc(size,caps);scope_end(scope);return p;
}
void *IRAM_ATTR __wrap_heap_caps_calloc(size_t count,size_t size,uint32_t caps)
{
    if(s_registration!=0)return __real_heap_caps_calloc(count,size,caps);
    heap_scope scope=scope_begin(REQUEST_PC());void *p=__real_heap_caps_calloc(count,size,caps);scope_end(scope);return p;
}
void *IRAM_ATTR __wrap_heap_caps_realloc(void *ptr,size_t size,uint32_t caps)
{
    if(s_registration!=0)return __real_heap_caps_realloc(ptr,size,caps);
    heap_scope scope=scope_begin(REQUEST_PC());void *p=__real_heap_caps_realloc(ptr,size,caps);scope_end(scope);return p;
}
void *IRAM_ATTR __wrap_heap_caps_aligned_alloc(size_t alignment,size_t size,uint32_t caps)
{
    if(s_registration!=0)return __real_heap_caps_aligned_alloc(alignment,size,caps);
    heap_scope scope=scope_begin(REQUEST_PC());void *p=__real_heap_caps_aligned_alloc(alignment,size,caps);scope_end(scope);return p;
}
#endif

void nos_heap_diag_tick(void)
{
    uint32_t now=hook_ms();if(s_poll_attempted && now-s_last_tick<1000u)return;
    s_poll_attempted=1;s_last_tick=now;
    if(!s_crash_checked) {s_crash_checked=1;nos_crash_refresh();}
    const uint32_t internal=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT,dma=MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA;
    nos_heap_failure metrics={.metrics_ms=now,
        .internal_free=(uint32_t)heap_caps_get_free_size(internal),
        .internal_largest=(uint32_t)heap_caps_get_largest_free_block(internal),
        .internal_min=(uint32_t)heap_caps_get_minimum_free_size(internal),
        .dma_free=(uint32_t)heap_caps_get_free_size(dma),
        .dma_largest=(uint32_t)heap_caps_get_largest_free_block(dma),
        .dma_min=(uint32_t)heap_caps_get_minimum_free_size(dma)};
    if(try_lock()) {s_metrics=metrics;s_ticked=1;store32(&s_lock,0);}
    nos_heap_failure f=nos_heap_diag_snapshot();
    if(!f.busy && f.sequence && f.total!=s_reported) {
        uint32_t delta=f.total-s_reported;s_reported=f.total;
        diag_log_event(DIAG_ALLOC_FAILURE,(int32_t)f.size,(int32_t)f.caps,(int32_t)f.caller_pc);
        diag_log_event(DIAG_ALLOC_CONTEXT,(int32_t)f.sequence,(int32_t)f.task,(int32_t)((f.core&1u)|(f.flags<<8)));
        diag_log_event(DIAG_ALLOC_HEAP,(int32_t)f.sequence,(int32_t)delta,(int32_t)f.uptime_ms);
        diag_log_event(DIAG_DMA_HEAP,(int32_t)metrics.dma_free,(int32_t)metrics.dma_min,(int32_t)metrics.dma_largest);
    }
}

void nos_heap_diag_report(void)
{
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    nos_heap_failure f = nos_heap_diag_snapshot();
    printf("NOSHEAP internal_free=%u internal_largest=%u internal_min=%u dma_free=%u dma_largest=%u dma_min=%u\n",
           (unsigned)heap_caps_get_free_size(internal),
           (unsigned)heap_caps_get_largest_free_block(internal),
           (unsigned)heap_caps_get_minimum_free_size(internal),
           (unsigned)heap_caps_get_free_size(dma),
           (unsigned)heap_caps_get_largest_free_block(dma),
           (unsigned)heap_caps_get_minimum_free_size(dma));
    printf("NOSALLOC registered=%d total=%u dropped=%u busy=%d sequence=%u size=%u caps=0x%08x function=%s\n",
           s_registration == 0, (unsigned)f.total, (unsigned)f.dropped, f.busy,
           (unsigned)f.sequence, (unsigned)f.size, (unsigned)f.caps,
           f.function_name ? f.function_name : "-");
    printf("NOSALLOCCONTEXT v=1 sequence=%u boot=%u build=%08x ms=%u cycles=%u core=%u task=%08x caller=%08x flags=%u metrics_ms=%u internal_free=%u internal_largest=%u internal_min=%u dma_free=%u dma_largest=%u dma_min=%u\n",
        (unsigned)f.sequence,(unsigned)f.boot,(unsigned)f.build,(unsigned)f.uptime_ms,(unsigned)f.cycles,
        (unsigned)f.core,(unsigned)f.task,(unsigned)f.caller_pc,(unsigned)f.flags,(unsigned)f.metrics_ms,
        (unsigned)f.internal_free,(unsigned)f.internal_largest,(unsigned)f.internal_min,
        (unsigned)f.dma_free,(unsigned)f.dma_largest,(unsigned)f.dma_min);
    for(unsigned i=0;i<HEAP_RETAIN_CAP;i++) {
        nos_heap_failure retained;if(!nos_heap_diag_history(i,&retained))continue;
        printf("NOSALLOCFAULT v=1 slot=%u sequence=%u boot=%u build=%08x ms=%u core=%u task=%08x caller=%08x size=%u caps=%08x flags=%u metrics_ms=%u dma_free=%u dma_largest=%u dma_min=%u\n",
            i,(unsigned)retained.sequence,(unsigned)retained.boot,(unsigned)retained.build,(unsigned)retained.uptime_ms,
            (unsigned)retained.core,(unsigned)retained.task,(unsigned)retained.caller_pc,(unsigned)retained.size,
            (unsigned)retained.caps,(unsigned)retained.flags,(unsigned)retained.metrics_ms,
            (unsigned)retained.dma_free,(unsigned)retained.dma_largest,(unsigned)retained.dma_min);
    }
}

void nos_crash_refresh(void)
{
    nos_crash_info next={0};
#if defined(ESP_PLATFORM) && CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    const esp_partition_t *part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_COREDUMP,NULL);
    if(!part) {next.state=NOS_CRASH_IO_ERROR;next.error=ESP_ERR_NOT_FOUND;}
    else {
        uint32_t first=0;esp_err_t error=esp_partition_read(part,0,&first,sizeof first);
        if(error!=ESP_OK) {next.state=NOS_CRASH_IO_ERROR;next.error=error;}
        else if(first==UINT32_MAX)next.state=NOS_CRASH_EMPTY;
        else {
            size_t address=0,bytes=0;
            error=esp_core_dump_image_get(&address,&bytes);
            if(error!=ESP_OK || address<part->address || bytes>part->size ||
               address-part->address>part->size-bytes) {
                next.state=NOS_CRASH_INVALID;next.error=error?error:ESP_ERR_INVALID_SIZE;
            } else if((error=esp_core_dump_image_check())!=ESP_OK) {
                next.state=NOS_CRASH_INVALID;next.error=error;
            } else {
                esp_core_dump_summary_t summary={0};error=esp_core_dump_get_summary(&summary);
                if(error!=ESP_OK) {next.state=NOS_CRASH_INVALID;next.error=error;}
                else {
                    next.state=NOS_CRASH_VALID;next.address=(uint32_t)address;next.bytes=(uint32_t)bytes;
                    next.format_version=summary.core_dump_version;
                    next.pc=summary.exc_pc;next.sp=summary.ex_info.exc_a[1];
                    next.cause=summary.ex_info.exc_cause;next.vaddr=summary.ex_info.exc_vaddr;
                    next.task=summary.exc_tcb;next.depth=summary.exc_bt_info.depth;
                    if(next.depth>16)next.depth=16;
                    next.backtrace_corrupt=summary.exc_bt_info.corrupted;
                    memcpy(next.backtrace,summary.exc_bt_info.bt,next.depth*sizeof(uint32_t));
                    memcpy(next.task_name,summary.exc_task,sizeof next.task_name-1);
                    size_t hash_len=sizeof summary.app_elf_sha256;
                    if(hash_len>sizeof next.elf_sha256-1)hash_len=sizeof next.elf_sha256-1;
                    memcpy(next.elf_sha256,summary.app_elf_sha256,hash_len);
                }
            }
        }
    }
#endif
    s_crash=next;
    uint32_t build=diag_log_name_id(next.elf_sha256);
    diag_log_crash_state(next.state,next.pc,build);
    if(next.state==NOS_CRASH_VALID) {
        diag_log_event(DIAG_CRASH_ARTIFACT,(int32_t)build,(int32_t)next.bytes,(int32_t)next.task);
        diag_log_event(DIAG_CRASH_PC,(int32_t)next.pc,(int32_t)next.sp,(int32_t)next.cause);
    }
}
void nos_crash_get(nos_crash_info *out) {if(out)*out=s_crash;}
int nos_crash_read(uint32_t offset,void *buffer,uint32_t bytes)
{
    if(!buffer || !bytes || bytes>1024 || s_crash.state!=NOS_CRASH_VALID || offset>=s_crash.bytes)return -1;
#if defined(ESP_PLATFORM) && CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    const esp_partition_t *part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_COREDUMP,NULL);
    if(!part || s_crash.address<part->address)return -1;
    if(bytes>s_crash.bytes-offset)bytes=s_crash.bytes-offset;
    uint32_t within=s_crash.address-part->address+offset;
    if(within>part->size || bytes>part->size-within)return -1;
    if(esp_partition_read(part,within,buffer,bytes)!=ESP_OK)return -1;
    return (int)bytes;
#else
    return -1;
#endif
}
int nos_crash_clear(void)
{
#if defined(ESP_PLATFORM) && CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    if(esp_core_dump_image_erase()!=ESP_OK)return 0;
    nos_crash_refresh();return s_crash.state==NOS_CRASH_EMPTY;
#else
    return 0;
#endif
}
void nos_crash_report(void)
{
    nos_crash_refresh();nos_crash_info c=s_crash;
    printf("NOSCRASH v=1 state=%u bytes=%u address=%08x error=%ld pc=%08x sp=%08x cause=%u vaddr=%08x task=%08x name=%.15s elf_sha256=%.64s depth=%u corrupted=%u format=%u\n",
        (unsigned)c.state,(unsigned)c.bytes,(unsigned)c.address,(long)c.error,(unsigned)c.pc,(unsigned)c.sp,
        (unsigned)c.cause,(unsigned)c.vaddr,(unsigned)c.task,c.task_name,c.elf_sha256,(unsigned)c.depth,(unsigned)c.backtrace_corrupt,(unsigned)c.format_version);
    for(unsigned i=0;i<c.depth;i++)printf("NOSCRASHBT index=%u pc=%08x\n",i,(unsigned)c.backtrace[i]);
}
