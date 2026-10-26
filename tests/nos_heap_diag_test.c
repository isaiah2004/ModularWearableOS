#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#define NOS_HEAP_DIAG_TEST 1
#include "../firmware/main/nos_heap_diag.c"

static esp_alloc_failed_hook_t registered;
static uint32_t heap_calls;
static unsigned diag_events;
static int mock_fail=1;
static char allocation;
void diag_log_event(uint16_t code,int32_t a,int32_t b,int32_t c){(void)code;(void)a;(void)b;(void)c;diag_events++;}
void diag_log_crash_state(uint32_t state,uint32_t pc,uint32_t build){(void)pc;(void)build;assert(state==NOS_CRASH_DISABLED);}
uint32_t diag_log_name_id(const char*s){(void)s;return 1;}
void *__real_heap_caps_malloc(size_t size,uint32_t caps){if(mock_fail){registered(size,caps,"heap_caps_malloc");return NULL;}return &allocation;}
void *__real_heap_caps_calloc(size_t count,size_t size,uint32_t caps){return __real_heap_caps_malloc(count*size,caps);}
void *__real_heap_caps_realloc(void*p,size_t size,uint32_t caps){(void)p;return __real_heap_caps_malloc(size,caps);}
void *__real_heap_caps_aligned_alloc(size_t align,size_t size,uint32_t caps){assert(align==64);return __real_heap_caps_malloc(size,caps);}
int heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t f) { registered=f; return 0; }
size_t heap_caps_get_free_size(uint32_t c) { ++heap_calls; return c+100; }
size_t heap_caps_get_largest_free_block(uint32_t c) { ++heap_calls; return c+50; }
size_t heap_caps_get_minimum_free_size(uint32_t c) { ++heap_calls; return c+20; }

static void *producer(void *arg)
{
    uint32_t base=(uint32_t)(uintptr_t)arg*100000;
    for (uint32_t i=1;i<=20000;++i) {
        uint32_t size=base+i;
        registered(size,size^0x5a5a,"heap_caps_aligned_alloc");
    }
    return NULL;
}
int main(void)
{
    nos_heap_diag_init(); assert(registered && !heap_calls);
    /* Hook must retain, never dereference, even a presently inaccessible name. */
    s_test_clock_ms=1000;registered(1600,8,(const char *)(uintptr_t)1);
    nos_heap_failure f=nos_heap_diag_snapshot();
    assert(!f.busy && f.total==1 && f.sequence==1 && f.size==1600 && f.caps==8);
    assert(f.function_name==(const char *)(uintptr_t)1);
    assert(f.uptime_ms==1000 && f.core==0 && !f.caller_pc && !(f.flags&NOS_HEAP_CALLER_KNOWN));
    nos_heap_failure first;assert(nos_heap_diag_history(0,&first));
    assert(first.sequence==1 && first.uptime_ms==1000 && !first.function_name);
    /* Deterministic contention: callback and reader each return immediately. */
    __atomic_store_n(&s_lock,1,__ATOMIC_RELEASE);
    registered(99,99,"discard"); f=nos_heap_diag_snapshot();
    assert(f.busy && f.total==2 && f.dropped==1 && !f.function_name);
    __atomic_store_n(&s_lock,0,__ATOMIC_RELEASE);
    assert(!__wrap_heap_caps_malloc(17,17^0x5a5a));f=nos_heap_diag_snapshot();
    assert(f.caller_pc==0x40001234 && (f.flags&NOS_HEAP_CALLER_KNOWN));
    registered(18,18^0x5a5a,"heap_caps_aligned_alloc");f=nos_heap_diag_snapshot();
    assert(!f.caller_pc && !(f.flags&NOS_HEAP_CALLER_KNOWN));
    heap_scope scope=scope_begin(123);assert(scope.index>=0);
    __atomic_store_n(&s_lock,1,__ATOMIC_RELEASE);scope_end(scope);
    __atomic_store_n(&s_lock,0,__ATOMIC_RELEASE);
    registered(19,19^0x5a5a,"heap_caps_aligned_alloc");f=nos_heap_diag_snapshot();assert(!f.caller_pc);
    mock_fail=0;assert(__wrap_heap_caps_calloc(2,40,8)==&allocation);
    assert(__wrap_heap_caps_realloc(&allocation,80,8)==&allocation);
    assert(__wrap_heap_caps_aligned_alloc(64,512,8)==&allocation);mock_fail=1;
    uint32_t before=f.total;
    pthread_t threads[4];
    for(uintptr_t i=0;i<4;++i) assert(!pthread_create(&threads[i],NULL,producer,(void *)i));
    for(int i=0;i<60000;++i) {
        f=nos_heap_diag_snapshot();
        if(!f.busy) assert(f.caps==(f.size^0x5a5a) && f.sequence<=f.total);
    }
    for(int i=0;i<4;++i) assert(!pthread_join(threads[i],NULL));
    f=nos_heap_diag_snapshot();
    assert(!f.busy && f.total==before+80000 && f.dropped>=1 && f.dropped<f.total);
    assert(!heap_calls); /* No heap queries/printf/registration inside hook. */
    nos_heap_diag_tick();assert(heap_calls==6 && diag_events==4);
    nos_heap_diag_tick();assert(heap_calls==6);
    s_test_clock_ms+=1000;nos_heap_diag_tick();assert(heap_calls==12 && diag_events==4);
    registered(20,20^0x5a5a,"heap_caps_aligned_alloc");f=nos_heap_diag_snapshot();
    assert(f.flags&NOS_HEAP_METRICS_CACHED);assert(f.metrics_ms==2000&&f.dma_free==MALLOC_CAP_INTERNAL+MALLOC_CAP_DMA+100);
    assert(f.dma_min==MALLOC_CAP_INTERNAL+MALLOC_CAP_DMA+20 && heap_calls==12);
    assert(nos_heap_diag_history(0,&first) && first.sequence==1 && first.uptime_ms==1000);
    nos_heap_diag_report(); assert(heap_calls==18);
    puts("PASS heap failure capture, bounded contention, coherent concurrent snapshots");
    return 0;
}
