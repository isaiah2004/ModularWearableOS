#include "diag_log.h"
#include "hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "board.h"
#include "sports.h"
#include <unistd.h>
static portMUX_TYPE s_mux=portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&s_mux)
#define UNLOCK() portEXIT_CRITICAL(&s_mux)
#define MAKE_DIR(p) mkdir(p,0777)
#define DEFAULT_DIRECTORY "/sdcard/watch/diagnostics"
#else
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MAKE_DIR(p) _mkdir(p)
#define fsync _commit
#else
#include <unistd.h>
#define MAKE_DIR(p) mkdir(p,0777)
#endif
#define RTC_NOINIT_ATTR
#define DEFAULT_DIRECTORY "build/nos_diagnostics"
static volatile int s_mux;
#define LOCK() do {while(__sync_lock_test_and_set(&s_mux,1)) {}} while(0)
#define UNLOCK() __sync_lock_release(&s_mux)
#endif

#define QUEUE_CAP 128u
#define PRIORITY_CAP 32u
#define FAULT_CAP 8u
#define RETAIN_CAP 32u
#define BATCH_CAP 32u
#define FILE_COUNT 8u
#ifndef DIAG_FILE_LIMIT
#define DIAG_FILE_LIMIT (1024u*1024u)
#endif
#define RECORD_MAGIC 0x31474c44u /* DLG1 */
#define FILE_MAGIC 0x46474c44u   /* DLGF */
#define RETAIN_MAGIC 0x52474c44u
#define FLAG_RETAINED 1u
#define FAULT_MAGIC 0x32464c44u
#define LOSS_MAGIC 0x324c4c44u
/* Wire format is little endian; ESP32 and the supported simulators are LE.
 * CRC32 is IEEE/zlib over all preceding bytes. No pointers or padding on disk. */
typedef struct {
    uint32_t magic;
    uint16_t version,bytes;
    uint32_t boot,sequence,uptime,epoch;
    uint16_t code,flags;
    int32_t a,b,c;
    uint32_t check;
} diag_record;
typedef struct {
    uint32_t magic,version,bytes,generation,boot,epoch,reserved,check;
} diag_file_header;
typedef char record_size_check[sizeof(diag_record)==44?1:-1];
typedef char header_size_check[sizeof(diag_file_header)==32?1:-1];
typedef struct {uint32_t magic,boot,crash_boots,check;} retained_header;
static RTC_NOINIT_ATTR retained_header s_retained_header;
static RTC_NOINIT_ATTR diag_record s_retained[RETAIN_CAP];
typedef struct {
    uint32_t magic,version,count;
    diag_record first,last;
    uint32_t check;
} retained_fault;
typedef struct {
    uint32_t magic,version,routine,priority,emergency,coalesced,lost,check;
} retained_loss;
static RTC_NOINIT_ATTR retained_fault s_faults[FAULT_CAP];
static RTC_NOINIT_ATTR retained_loss s_loss;
/* Queue and the consumer's batch are PSRAM; only the crash breadcrumbs use RTC. */
typedef struct {diag_record queue[QUEUE_CAP],priority[PRIORITY_CAP],batch[BATCH_CAP];} diag_memory;
static diag_memory *s_memory;
static diag_log_status s_status;
static uint32_t s_head,s_tail,s_sequence,s_last_sample,s_reported_drops;
static uint32_t s_priority_head,s_priority_tail,s_request;
static uint32_t s_fault_reported[FAULT_CAP];
static int s_have_sample,s_scanned,s_slot=-1,s_rotate,s_last_storage_error;
static uint32_t s_init_ms;
static int s_init_attempted;
#ifndef ESP_PLATFORM
static uint32_t s_host_service_at;
static int s_host_serviced;
#endif
static uint32_t s_generation;
static long s_file_bytes;
static char s_directory[192]=DEFAULT_DIRECTORY;
#ifndef ESP_PLATFORM
static uint32_t s_host_boot;
static unsigned s_host_reset=1; /* ESP_RST_POWERON */
#endif
#ifdef DIAG_LOG_TEST
static int s_test_write_after=-1,s_test_sync_fail;
#endif

static uint32_t crc32(const void *data,size_t size)
{
    const unsigned char *p=data;uint32_t crc=~0u;
    while(size--) {crc^=*p++;for(int i=0;i<8;i++)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);}
    return ~crc;
}
static int valid_record(const diag_record *r)
{ return r->magic==RECORD_MAGIC && r->version==1 && r->bytes==sizeof(*r) && r->check==crc32(r,offsetof(diag_record,check)); }
static int valid_fault(const retained_fault *r)
{ return r->magic==FAULT_MAGIC && r->version==1 && valid_record(&r->first); }
static int fault_copy(const retained_fault *slot,retained_fault *out)
{
    *out=*slot;if(!valid_fault(out))return 0;
    if(out->count && valid_record(&out->last) && out->check==crc32(out,offsetof(retained_fault,check)))return 1;
    /* A reset during mutable last/count publication cannot invalidate the
     * separately checksummed immutable first fault. Its suffix is unknown. */
    out->last=out->first;out->count=1;out->check=crc32(out,offsetof(retained_fault,check));return 2;
}
static void loss_commit(void)
{
    retained_loss next={LOSS_MAGIC,1,s_status.routine_dropped,s_status.priority_dropped,
                        s_status.emergency_dropped,s_status.coalesced,s_status.evidence_lost,0};
    next.check=crc32(&next,offsetof(retained_loss,check));s_loss.magic=0;
    memcpy((unsigned char *)&s_loss+4,(unsigned char *)&next+4,sizeof next-4);
    __sync_synchronize();s_loss.magic=LOSS_MAGIC;
}
static int is_fault(const diag_record *r)
{
    return r->code==DIAG_STORAGE_ERROR || r->code==DIAG_IMU_ERROR || r->code==DIAG_DISPLAY_FAULT ||
        r->code==DIAG_ALLOC_FAILURE || (r->code==DIAG_ACTION_RESULT && r->b<0) ||
        (r->code==DIAG_WORKOUT && (r->a<0 || r->b<0));
}
static int is_priority(const diag_record *r)
{
    return is_fault(r) || r->code==DIAG_BOOT || r->code==DIAG_RESET_CONTEXT ||
        r->code==DIAG_QUEUE_DROPS || r->code==DIAG_FAULT_SUMMARY ||
        r->code==DIAG_POWER_STATE || r->code==DIAG_POWER_POLICY ||
        r->code==DIAG_WORKOUT || r->code==DIAG_WORKOUT_RECOVERY ||
        (r->code>=DIAG_IMU_START && r->code<=DIAG_IMU_ERROR) ||
        (r->code>=DIAG_ACTION_REQUEST && r->code<=DIAG_ACTION_CONTEXT) ||
        r->code==DIAG_CRASH_ARTIFACT || r->code==DIAG_CRASH_PC ||
        r->code==DIAG_ALLOC_CONTEXT || r->code==DIAG_ALLOC_HEAP;
}
static int enqueue(const diag_record *r)
{
    int priority=is_priority(r);
    if(priority && s_priority_head-s_priority_tail<PRIORITY_CAP) {
        s_memory->priority[s_priority_head++%PRIORITY_CAP]=*r;return 1;
    }
    if(!priority && s_head-s_tail<QUEUE_CAP) {
        s_memory->queue[s_head++%QUEUE_CAP]=*r;return 1;
    }
    if(priority)s_status.priority_dropped++;else s_status.routine_dropped++;
    s_status.dropped++;s_status.evidence_lost=1;loss_commit();return 0;
}
/* Called under the producer lock; no allocation, SD or unbounded scan. */
static int fault_retain(const diag_record *r)
{
    int empty=-1;
    for(unsigned i=0;i<FAULT_CAP;i++) {
        if(!valid_fault(&s_faults[i])) {if(empty<0)empty=(int)i;continue;}
        retained_fault *old=&s_faults[i];
        if(old->first.code!=r->code || old->first.a!=r->a || old->first.b!=r->b || old->first.c!=r->c)continue;
        retained_fault next;int recovered=fault_copy(old,&next);
        if(recovered==2)s_status.evidence_lost=1;
        next.last=*r;if(next.count<UINT32_MAX)next.count++;
        next.check=crc32(&next,offsetof(retained_fault,check));
        old->last.magic=0;
        memcpy((unsigned char *)&old->last+4,(unsigned char *)&next.last+4,sizeof next.last-4);
        __sync_synchronize();old->last.magic=RECORD_MAGIC;
        old->count=next.count;__sync_synchronize();old->check=next.check;
        s_status.coalesced++;loss_commit();return 0;
    }
    if(empty<0) {
        s_status.emergency_dropped++;s_status.evidence_lost=1;loss_commit();return 1;
    }
    retained_fault next={.magic=FAULT_MAGIC,.version=1,.count=1,.first=*r,.last=*r};
    next.check=crc32(&next,offsetof(retained_fault,check));s_faults[empty].magic=0;
    memcpy((unsigned char *)&s_faults[empty]+4,(unsigned char *)&next+4,sizeof next-4);
    __sync_synchronize();s_faults[empty].magic=FAULT_MAGIC;s_status.faults++;return 1;
}
static uint32_t millis(void)
{
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time()/1000);
#else
    return hal_millis();
#endif
}
uint32_t diag_log_name_id(const char *name)
{
    uint32_t h=2166136261u;
    if(name)while(*name)h=(h^(unsigned char)*name++)*16777619u;
    return h;
}
int diag_log_set_directory(const char *directory)
{
    if(s_status.ready || !directory || !*directory || strlen(directory)>=sizeof(s_directory)-24)return 0;
    strcpy(s_directory,directory);return 1;
}
static void retain(const diag_record *r)
{
    diag_record *slot=&s_retained[r->sequence%RETAIN_CAP];
    /* Commit magic last: a reset mid-copy invalidates only this slot. */
    slot->magic=0;
    memcpy((unsigned char *)slot+sizeof(slot->magic),(const unsigned char *)r+sizeof(r->magic),sizeof(*r)-sizeof(r->magic));
    __sync_synchronize();slot->magic=r->magic;
}
void diag_log_event(uint16_t code,int32_t a,int32_t b,int32_t c)
{
    if(!__atomic_load_n(&s_status.ready,__ATOMIC_ACQUIRE))return;
    diag_record r={.magic=RECORD_MAGIC,.version=1,.bytes=sizeof(r),.uptime=millis(),
        .epoch=(uint32_t)hal_epoch_seconds(),.code=code,.a=a,.b=b,.c=c};
    LOCK();
    r.boot=s_status.boot;r.sequence=++s_sequence;r.check=crc32(&r,offsetof(diag_record,check));
    retain(&r);
    if(!is_fault(&r) || fault_retain(&r))enqueue(&r);
    UNLOCK();
}
uint32_t diag_action_request(uint16_t domain,uint16_t action,int32_t target)
{
    if(!__atomic_load_n(&s_status.ready,__ATOMIC_ACQUIRE))return 0;
    LOCK();uint32_t request=++s_request;if(!request)request=++s_request;
    s_status.last_request=request;s_status.last_target=(uint32_t)target;UNLOCK();
    diag_log_event(DIAG_ACTION_REQUEST,(int32_t)request,(int32_t)(((uint32_t)domain<<16)|action),target);
    return request;
}
void diag_action_context(uint32_t request,int32_t source,int32_t value)
{ if(request)diag_log_event(DIAG_ACTION_CONTEXT,(int32_t)request,source,value); }
void diag_action_result(uint32_t request,int32_t result,int32_t detail)
{
    if(!request)return;
    LOCK();s_status.last_result_request=request;s_status.last_result=result;s_status.last_result_detail=detail;UNLOCK();
    diag_log_event(DIAG_ACTION_RESULT,(int32_t)request,result,detail);
}
void diag_log_crash_state(uint32_t state,uint32_t pc,uint32_t build)
{ LOCK();s_status.crash_state=state;s_status.crash_pc=pc;s_status.crash_build=build;UNLOCK(); }
int diag_log_fault_get(unsigned index,diag_fault_summary *out)
{
    if(index>=FAULT_CAP || !out)return 0;
    LOCK();retained_fault f;int valid=fault_copy(&s_faults[index],&f);UNLOCK();if(!valid)return 0;
    *out=(diag_fault_summary){.boot=f.first.boot,.last_boot=f.last.boot,.code=f.first.code,
        .first_sequence=f.first.sequence,.last_sequence=f.last.sequence,.first_ms=f.first.uptime,
        .last_ms=f.last.uptime,.count=f.count,.first_a=f.first.a,.first_b=f.first.b,.first_c=f.first.c,
        .last_a=f.last.a,.last_b=f.last.b,.last_c=f.last.c};
    return 1;
}
void diag_log_init(void)
{
    if(__atomic_load_n(&s_status.ready,__ATOMIC_ACQUIRE))return;
    uint32_t now=millis();if(s_init_attempted && now-s_init_ms<5000u)return;
    s_init_attempted=1;s_init_ms=now;
#ifdef ESP_PLATFORM
    s_memory=nos_mem_alloc(sizeof(*s_memory),"diagnostics.queue");
    if(s_memory)memset(s_memory,0,sizeof *s_memory);
    s_status.reset_reason=(uint32_t)esp_reset_reason();s_status.boot=esp_random();
#else
    s_memory=calloc(1,sizeof(*s_memory));s_status.reset_reason=s_host_reset;
    s_status.boot=(uint32_t)time(NULL)^(uint32_t)clock()^(++s_host_boot*0x9e3779b9u);
#endif
    if(!s_memory) {s_status.errors++;return;}
    if(!s_status.boot)s_status.boot=1;
    int retained_ok=s_retained_header.magic==RETAIN_MAGIC &&
        s_retained_header.check==crc32(&s_retained_header,offsetof(retained_header,check));
    uint32_t previous_boot=retained_ok?s_retained_header.boot:0,previous_seq=0;
    /* Panic and watchdog resets are crashes. Power-on, software restart, deep
     * sleep and brownout have their own reset reasons, never invented crashes. */
    unsigned reason=s_status.reset_reason;
    if(reason==1)previous_boot=0;
    int loss_ok=s_loss.magic==LOSS_MAGIC && s_loss.version==1 &&
        s_loss.check==crc32(&s_loss,offsetof(retained_loss,check));
    if(reason==1) {memset(s_faults,0,sizeof s_faults);memset(&s_loss,0,sizeof s_loss);}
    else if(loss_ok) {
        s_status.routine_dropped=s_loss.routine;s_status.priority_dropped=s_loss.priority;
        s_status.emergency_dropped=s_loss.emergency;s_status.coalesced=s_loss.coalesced;
        s_status.dropped=s_loss.routine+s_loss.priority;
        s_status.evidence_lost=s_loss.lost || s_status.dropped || s_status.emergency_dropped;
    }
    for(unsigned i=0;i<FAULT_CAP;i++) {
        retained_fault fault;int valid=fault_copy(&s_faults[i],&fault);if(!valid)continue;
        if(valid==2)s_status.evidence_lost=1;
        s_status.faults++;
        diag_record first=fault.first,last=fault.last;
        first.flags|=FLAG_RETAINED;first.check=crc32(&first,offsetof(diag_record,check));enqueue(&first);
        if(last.boot!=first.boot || last.sequence!=first.sequence) {
            last.flags|=FLAG_RETAINED;last.check=crc32(&last,offsetof(diag_record,check));enqueue(&last);
        }
    }
    loss_commit();
    int crash=reason==4 || reason==5 || reason==6 || reason==7 || reason==15;
    if(reason!=1 && retained_ok) {
        s_status.crash_boots=s_retained_header.crash_boots;
        for(unsigned i=0;i<RETAIN_CAP;i++) {
            diag_record r=s_retained[i];
            if(valid_record(&r) && r.boot==previous_boot) {
                r.flags|=FLAG_RETAINED;r.check=crc32(&r,offsetof(diag_record,check));
                /* Insertion sort of at most 32 entries, before producers run. */
                unsigned j=s_head;
                while(j && s_memory->queue[j-1].sequence>r.sequence) {s_memory->queue[j]=s_memory->queue[j-1];j--;}
                s_memory->queue[j]=r;s_head++;
                if(r.sequence>previous_seq)previous_seq=r.sequence;
            }
        }
        s_status.previous_events=s_head+s_priority_head;
    }
    if(crash)s_status.crash_boots++;
    memset(s_retained,0,sizeof(s_retained));
    retained_header header={RETAIN_MAGIC,s_status.boot,s_status.crash_boots,0};
    header.check=crc32(&header,offsetof(retained_header,check));
    s_retained_header.magic=0;
    memcpy((unsigned char *)&s_retained_header+4,(unsigned char *)&header+4,sizeof(header)-4);
    __sync_synchronize();s_retained_header.magic=header.magic;
    __atomic_store_n(&s_status.ready,1,__ATOMIC_RELEASE);
    diag_log_event(DIAG_BOOT,(int32_t)reason,(int32_t)s_status.previous_events,(int32_t)s_status.crash_boots);
    diag_log_event(DIAG_RESET_CONTEXT,(int32_t)previous_boot,(int32_t)previous_seq,crash);
}
void diag_log_get(diag_log_status *out)
{ if(out) {LOCK();*out=s_status;out->queued=s_head-s_tail+s_priority_head-s_priority_tail;UNLOCK();} }
void diag_log_report(void)
{
    diag_log_status s;diag_log_get(&s);
    printf("NOSLOG v=1 record_bytes=%u boot=%08lx reset=%lu ready=%d available=%d queued=%lu written=%lu dropped=%lu errors=%lu previous=%lu crash_boots=%lu files=8 file_limit=%u\n",
        (unsigned)sizeof(diag_record),(unsigned long)s.boot,(unsigned long)s.reset_reason,s.ready,s.available,
        (unsigned long)s.queued,(unsigned long)s.written,(unsigned long)s.dropped,(unsigned long)s.errors,
        (unsigned long)s.previous_events,(unsigned long)s.crash_boots,(unsigned)DIAG_FILE_LIMIT);
    printf("NOSEVIDENCE v=1 routine_drop=%lu priority_drop=%lu emergency_drop=%lu coalesced=%lu faults=%lu loss_sticky=%lu crash_state=%lu crash_pc=%08lx crash_build=%08lx result_request=%lu result=%ld detail=%ld\n",
        (unsigned long)s.routine_dropped,(unsigned long)s.priority_dropped,(unsigned long)s.emergency_dropped,
        (unsigned long)s.coalesced,(unsigned long)s.faults,(unsigned long)s.evidence_lost,
        (unsigned long)s.crash_state,(unsigned long)s.crash_pc,(unsigned long)s.crash_build,
        (unsigned long)s.last_result_request,(long)s.last_result,(long)s.last_result_detail);
    for(unsigned i=0;i<FAULT_CAP;i++) {
        diag_fault_summary f;if(!diag_log_fault_get(i,&f))continue;
        printf("NOSFAULT v=1 slot=%u boot=%08lx last_boot=%08lx code=%lu first_seq=%lu last_seq=%lu first_ms=%lu last_ms=%lu count=%lu a=%ld b=%ld c=%ld\n",
            i,(unsigned long)f.boot,(unsigned long)f.last_boot,(unsigned long)f.code,
            (unsigned long)f.first_sequence,(unsigned long)f.last_sequence,(unsigned long)f.first_ms,
            (unsigned long)f.last_ms,(unsigned long)f.count,(long)f.first_a,(long)f.first_b,(long)f.first_c);
    }
}
void diag_log_tick(void)
{
    if(!__atomic_load_n(&s_status.ready,__ATOMIC_ACQUIRE)) {
        diag_log_init();if(!__atomic_load_n(&s_status.ready,__ATOMIC_ACQUIRE))return;
    }
    uint32_t now=millis();
    if(!s_have_sample || now-s_last_sample>=30000) {
        s_have_sample=1;s_last_sample=now;
        hal_power p;hal_power_get(&p);
        uint32_t age=p.age_ms>0xffffffu?0xffffffu:p.age_ms;
        uint32_t flags=(p.sample_valid?1u:0u)|(p.battery_valid?2u:0u)|
            ((uint32_t)(p.external_power+1)&3u)<<2|age<<8;
        diag_log_event(DIAG_POWER_SAMPLE,p.battery_percent,p.rail_mv,(int32_t)flags);
#ifdef ESP_PLATFORM
        unsigned caps=MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT;
        diag_log_event(DIAG_HEAP,(int32_t)heap_caps_get_free_size(caps),
            (int32_t)heap_caps_get_minimum_free_size(caps),(int32_t)heap_caps_get_largest_free_block(caps));
        unsigned dma=MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA;
        diag_log_event(DIAG_DMA_HEAP,(int32_t)heap_caps_get_free_size(dma),
            (int32_t)heap_caps_get_minimum_free_size(dma),(int32_t)heap_caps_get_largest_free_block(dma));
#endif
        /* Snapshot repeated faults only on the bounded maintenance cadence.
         * First evidence remains pinned in RTC even while both queues overflow. */
        for(unsigned i=0;i<FAULT_CAP;i++) {
            LOCK();retained_fault f;
            int send=fault_copy(&s_faults[i],&f) && f.count!=s_fault_reported[i];
            if(send && s_priority_head-s_priority_tail<PRIORITY_CAP-2) {
                diag_record last=f.last;last.flags|=FLAG_RETAINED;
                last.check=crc32(&last,offsetof(diag_record,check));enqueue(&last);
                s_fault_reported[i]=f.count;
            } else send=0;
            UNLOCK();
            if(send)diag_log_event(DIAG_FAULT_SUMMARY,(int32_t)i,(int32_t)f.count,f.first.code);
        }
    }
    diag_log_status s;diag_log_get(&s);
    if(s.dropped!=s_reported_drops && s.queued<QUEUE_CAP-4) {
        diag_log_event(DIAG_QUEUE_DROPS,(int32_t)s.dropped,(int32_t)(s.dropped-s_reported_drops),(int32_t)s.queued);
        s_reported_drops=s.dropped;
    }
#ifndef ESP_PLATFORM
    if(!s_host_serviced || now-s_host_service_at>=1000) {
        s_host_serviced=1;s_host_service_at=now;diag_log_service();
    }
#endif
}
static void file_path(char *path,size_t size,unsigned slot)
{ snprintf(path,size,"%s/diag-%02u.bin",s_directory,slot); }
static int scan_files(void)
{
    unsigned occupied=0;s_slot=-1;s_generation=0;s_rotate=0;s_file_bytes=0;
    for(unsigned i=0;i<FILE_COUNT;i++) {
        char path[224];file_path(path,sizeof(path),i);
        struct stat st;
        if(stat(path,&st)!=0) {if(errno==ENOENT)continue;return 0;}
        occupied|=1u<<i;
        FILE *f=fopen(path,"rb");if(!f)return 0;
        diag_file_header h;
        int valid=fread(&h,sizeof(h),1,f)==1 && h.magic==FILE_MAGIC && h.version==1 &&
            h.bytes==sizeof(h) && h.check==crc32(&h,offsetof(diag_file_header,check));
        if(valid && (s_slot<0 || (int32_t)(h.generation-s_generation)>0)) {
            s_slot=(int)i;s_generation=h.generation;s_file_bytes=(long)st.st_size;
            long data=s_file_bytes-(long)sizeof(h);
            s_rotate=data<0 || data%(long)sizeof(diag_record)!=0 || s_file_bytes>(long)DIAG_FILE_LIMIT;
            if(!s_rotate && data) {
                diag_record tail;
                s_rotate=fseek(f,-(long)sizeof(tail),SEEK_END)!=0 || fread(&tail,sizeof(tail),1,f)!=1 || !valid_record(&tail);
            }
        }
        if(fclose(f)!=0)return 0;
    }
    if(s_slot<0) {
        /* Preserve damaged evidence while unused slots remain. Once all eight
         * slots are occupied, ordinary bounded retention may reclaim one. */
        unsigned first=0;while(first<FILE_COUNT && (occupied&(1u<<first)))first++;
        s_slot=first<FILE_COUNT?(int)first-1:(int)FILE_COUNT-1;s_rotate=1;
    }
    s_scanned=1;return 1;
}
static size_t checked_write(const void *p,size_t size,FILE *f)
{
#ifdef DIAG_LOG_TEST
    if(s_test_write_after>=0 && (size_t)s_test_write_after<size) {
        size_t partial=(size_t)s_test_write_after;s_test_write_after=-1;
        size_t written=fwrite(p,1,partial,f);errno=EIO;return written;
    }
    if(s_test_write_after>=0)s_test_write_after-=(int)size;
#endif
    return fwrite(p,1,size,f);
}
static int sync_file(FILE *f)
{
    if(fflush(f)!=0)return 0;
#ifdef DIAG_LOG_TEST
    if(s_test_sync_fail) {s_test_sync_fail=0;errno=EIO;return 0;}
#endif
    return fsync(fileno(f))==0;
}
static FILE *open_append(void)
{
    char path[224];
    if(s_rotate || s_file_bytes+(long)sizeof(diag_record)>(long)DIAG_FILE_LIMIT) {
        s_slot=(s_slot+1)%(int)FILE_COUNT;s_generation++;
        file_path(path,sizeof(path),(unsigned)s_slot);
        FILE *f=fopen(path,"wb");if(!f)return NULL;
        diag_file_header h={FILE_MAGIC,1,sizeof(h),s_generation,s_status.boot,(uint32_t)hal_epoch_seconds(),0,0};
        h.check=crc32(&h,offsetof(diag_file_header,check));
        if(checked_write(&h,sizeof(h),f)!=sizeof(h) || !sync_file(f)) {fclose(f);return NULL;}
        s_file_bytes=sizeof(h);s_rotate=0;return f;
    }
    file_path(path,sizeof(path),(unsigned)s_slot);
    FILE *f=fopen(path,"r+b");if(!f)return NULL;
    if(fseek(f,0,SEEK_END)!=0 || ftell(f)!=s_file_bytes) {fclose(f);errno=EIO;return NULL;}
    return f;
}
void diag_log_service(void)
{
    if(!__atomic_load_n(&s_status.ready,__ATOMIC_ACQUIRE))return;
    unsigned count,priority;LOCK();priority=s_priority_head-s_priority_tail;if(priority>BATCH_CAP)priority=BATCH_CAP;
    count=s_head-s_tail;if(count>BATCH_CAP-priority)count=BATCH_CAP-priority;
    for(unsigned i=0;i<priority;i++)s_memory->batch[i]=s_memory->priority[(s_priority_tail+i)%PRIORITY_CAP];
    for(unsigned i=0;i<count;i++)s_memory->batch[priority+i]=s_memory->queue[(s_tail+i)%QUEUE_CAP];
    count+=priority;
    UNLOCK();if(!count)return;
#ifdef ESP_PLATFORM
    if(!board_sd_asset_acquire()) {
        LOCK();s_status.available=0;s_status.errors++;UNLOCK();s_scanned=0;
        diag_log_event(DIAG_STORAGE_ERROR,ENODEV,0,-1);return;
    }
    if(!hal_sports_sd_lock()) {
        board_sd_asset_release();LOCK();s_status.available=0;s_status.errors++;UNLOCK();
        diag_log_event(DIAG_STORAGE_ERROR,EBUSY,0,-1);return;
    }
#endif
    int ok=1,phase=1;FILE *f=NULL;unsigned consumed=0;
#ifdef ESP_PLATFORM
    if(MAKE_DIR("/sdcard/watch")!=0 && errno!=EEXIST)ok=0;
#endif
    if(ok && MAKE_DIR(s_directory)!=0 && errno!=EEXIST)ok=0;
    if(ok && !s_scanned) {phase=2;ok=scan_files();}
    while(ok && consumed<count) {
        phase=3;f=open_append();if(!f) {ok=0;break;}
        unsigned in_file=0;
        while(consumed+in_file<count && s_file_bytes+(long)sizeof(diag_record)<=(long)DIAG_FILE_LIMIT) {
            phase=4;
            if(checked_write(&s_memory->batch[consumed+in_file],sizeof(diag_record),f)!=sizeof(diag_record)) {ok=0;break;}
            s_file_bytes+=sizeof(diag_record);in_file++;
        }
        if(ok) {phase=5;if(!sync_file(f))ok=0;}
        if(fclose(f)!=0)ok=0;
        f=NULL;
        if(ok)consumed+=in_file;
    }
    int error=errno;
#ifdef ESP_PLATFORM
    hal_sports_sd_unlock();board_sd_asset_release();
#endif
    LOCK();
    /* A failed batch can have reached disk before fsync/close failed. Retry
     * with the original IDs; the exporter deduplicates (boot,sequence). */
    unsigned priority_done=consumed<priority?consumed:priority;
    s_priority_tail+=priority_done;s_tail+=consumed-priority_done;
    s_status.written+=consumed;s_status.available=ok;
    if(!ok)s_status.errors++;
    UNLOCK();
    if(!ok) {
        s_scanned=0;
        if(!error)error=EIO;
        /* Repetition is counted/coalesced in the independent emergency ledger. */
        diag_log_event(DIAG_STORAGE_ERROR,error,phase,s_slot);
        s_last_storage_error=error;
    } else s_last_storage_error=0;
}
