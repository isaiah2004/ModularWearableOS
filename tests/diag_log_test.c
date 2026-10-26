/* Host filesystem and retained-memory model; no device or serial connection. */
#define DIAG_LOG_TEST 1
#define DIAG_FILE_LIMIT 512u
#include "../nos/diag_log.c"
#include <assert.h>
#include <pthread.h>
static uint32_t test_ms=1000;
uint32_t hal_millis(void) {return test_ms;}
uint64_t hal_epoch_seconds(void) {return 1792497600u+test_ms/1000;}
void hal_power_get(hal_power *p) {*p=(hal_power){3700,20,1,0,1,7};}
static void flush_all(void)
{
    for(int i=0;i<100;i++) {
        diag_log_status s;diag_log_get(&s);if(!s.queued)return;
        diag_log_service();
    }
    assert(!"diagnostic queue failed to drain");
}
static void restart(unsigned reason)
{
    /* A real reset reinitializes normal RAM while RTC_NOINIT remains. */
    free(s_memory);s_memory=NULL;memset(&s_status,0,sizeof(s_status));
    s_head=s_tail=s_sequence=s_last_sample=s_reported_drops=0;
    s_priority_head=s_priority_tail=s_request=0;memset(s_fault_reported,0,sizeof s_fault_reported);
    s_init_ms=0;s_init_attempted=0;
    s_have_sample=s_scanned=s_rotate=s_last_storage_error=0;
    s_host_service_at=0;s_host_serviced=0;
    s_slot=-1;s_generation=0;s_file_bytes=0;s_host_reset=reason;
    s_test_write_after=-1;s_test_sync_fail=0;test_ms=1000;
    diag_log_init();
}
static void *producer(void *argument)
{
    int id=(int)(intptr_t)argument;
    for(int i=0;i<1000;i++)diag_log_event(DIAG_ACTION,id,i,0);
    return NULL;
}
static unsigned records(int code,int argument,int flags)
{
    unsigned count=0;
    for(unsigned i=0;i<FILE_COUNT;i++) {
        char path[224];file_path(path,sizeof(path),i);FILE *f=fopen(path,"rb");if(!f)continue;
        diag_file_header h;assert(fread(&h,sizeof(h),1,f)==1);
        diag_record r;
        while(fread(&r,sizeof(r),1,f)==1)if(valid_record(&r) &&
            (code<0 || r.code==code) && (argument<0 || r.a==argument) &&
            (flags<0 || r.flags==flags))count++;
        fclose(f);
    }
    return count;
}
int main(int argc,char **argv)
{
    assert(argc==3);assert(diag_log_set_directory(argv[2]));
    diag_log_init();assert(!diag_log_set_directory("other"));
    assert(sizeof(diag_record)==44 && sizeof(diag_file_header)==32);
    assert(diag_log_name_id("HOME")==0x10b8c84eu);
    diag_log_status s;diag_log_get(&s);assert(s.ready && s.queued==2 && !s.crash_boots && !s.previous_events);
    if(!strcmp(argv[1],"concurrent")) {
        pthread_t threads[4];
        for(int i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,producer,(void *)(intptr_t)i));
        for(int i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
        diag_log_get(&s);assert(s_sequence==4002 && s.queued==QUEUE_CAP+2 && s.dropped==3872);
        for(unsigned i=0;i<RETAIN_CAP;i++)assert(valid_record(&s_retained[i]) && s_retained[i].boot==s.boot);
        restart(4);diag_log_get(&s);assert(s.previous_events==32 && s.queued==34);
        flush_all();
    } else if(!strcmp(argv[1],"throttle")) {
        for(int i=0;i<64;i++)diag_log_event(DIAG_ACTION,i,0,0);
        diag_log_tick();diag_log_get(&s);assert(s.queued==35);
        for(int i=0;i<60;i++)diag_log_tick();
        diag_log_get(&s);assert(s.queued==35); /* no per-frame filesystem work */
        test_ms+=1000;diag_log_tick();diag_log_get(&s);assert(s.queued==3);
        flush_all();
    } else if(!strcmp(argv[1],"queue")) {
        struct stat st;assert(stat(argv[2],&st)!=0); /* producers do no I/O */
        for(int i=0;i<200;i++)diag_log_event(DIAG_ACTION,i,0,0);
        diag_log_get(&s);assert(s.queued==QUEUE_CAP+2 && s.dropped==72 && s.routine_dropped==72 && !s.priority_dropped);
        /* Overflow still retains the latest event in RTC, independently. */
        assert(valid_record(&s_retained[s_sequence%RETAIN_CAP]));
        assert(s_retained[s_sequence%RETAIN_CAP].a==199);
        diag_log_service();diag_log_get(&s);assert(s.queued==98 && s.written==32);
        flush_all();diag_log_tick();diag_log_get(&s);assert(!s.queued);
        assert(records(DIAG_QUEUE_DROPS,72,-1)==1);
        assert(records(DIAG_POWER_SAMPLE,20,-1)==1);
    } else if(!strcmp(argv[1],"fault")) {
        flush_all();
        char path[224];file_path(path,sizeof(path),0);
        struct stat before,after;assert(!stat(path,&before));
        for(int i=0;i<5;i++)diag_log_event(DIAG_ACTION,i,0,0);
        s_test_write_after=44+7;diag_log_service();
        diag_log_get(&s);assert(s.queued==6 && s.errors==1 && !s.available);
        assert(!stat(path,&after));assert(after.st_size==before.st_size+44+7);
        flush_all();assert(!stat(path,&before));assert(before.st_size==after.st_size);
        /* Torn evidence was preserved; retry moved to the next slot. */
        assert(records(DIAG_ACTION,0,-1)==2);assert(records(DIAG_ACTION,4,-1)==1);
        diag_log_event(DIAG_ACTION,99,0,0);s_test_sync_fail=1;diag_log_service();
        diag_log_get(&s);assert(s.queued>=1 && s.errors==2);
        flush_all();assert(records(DIAG_ACTION,99,-1)==2);
    } else if(!strcmp(argv[1],"rotation")) {
        for(int i=0;i<300;i++) {diag_log_event(DIAG_ACTION,i,0,0);if(i%20==19)flush_all();}
        flush_all();unsigned files=0;
        for(unsigned i=0;i<FILE_COUNT;i++) {
            char path[224];file_path(path,sizeof(path),i);struct stat st;
            if(!stat(path,&st)) {assert(st.st_size<=(long)DIAG_FILE_LIMIT);files++;}
        }
        assert(files==8 && s_generation>8);
        assert(!records(DIAG_ACTION,0,-1));assert(records(DIAG_ACTION,299,-1)==1);
    } else if(!strcmp(argv[1],"restart")) {
        for(int i=0;i<8;i++)diag_log_event(DIAG_ACTION,i,0,0);
        flush_all();uint32_t old_boot=s_status.boot;
        s_retained[3].check^=1; /* one torn breadcrumb does not lose the others */
        restart(4);diag_log_get(&s);
        assert(s.boot!=old_boot && s.previous_events==9 && s.crash_boots==1);
        flush_all();assert(records(DIAG_ACTION,7,FLAG_RETAINED)==1);
        restart(3);diag_log_get(&s);assert(s.crash_boots==1 && s.previous_events==2);
        flush_all();restart(1);diag_log_get(&s);assert(!s.crash_boots && !s.previous_events);
        flush_all();
    } else if(!strcmp(argv[1],"unavailable")) {
        /* Parent is deliberately absent: no queued event may be acknowledged. */
        diag_log_service();diag_log_get(&s);assert(!s.available && s.errors==1 && s.queued==3 && !s.written);
        diag_log_service();diag_log_get(&s);assert(s.errors==2 && s.queued==3);
    } else if(!strcmp(argv[1],"semantic")) {
        uint32_t request=diag_action_request(DIAG_DOMAIN_NOTIFICATION,DIAG_ACT_DISMISS,721);
        assert(request);diag_action_context(request,(int32_t)diag_log_name_id("NOTIFICATIONS"),17);
        diag_action_result(request,DIAG_RESULT_SUCCESS,721);
        diag_log_get(&s);assert(s.last_request==request && s.last_target==721 &&
            s.last_result_request==request && s.last_result==DIAG_RESULT_SUCCESS && s.last_result_detail==721);
        flush_all();assert(records(DIAG_ACTION_REQUEST,(int)request,-1)==1 && records(DIAG_ACTION_RESULT,(int)request,-1)==1);
    } else if(!strcmp(argv[1],"emergency")) {
        for(unsigned i=0;i<QUEUE_CAP+100;i++)diag_log_event(DIAG_ACTION,(int)i,0,0);
        uint32_t first=s_sequence+1;
        for(int i=0;i<1000;i++) {test_ms++;diag_log_event(DIAG_STORAGE_ERROR,EIO,4,2);}
        diag_fault_summary f;assert(diag_log_fault_get(0,&f));
        assert(f.first_sequence==first && f.count==1000 && f.first_ms==1001 && f.last_ms==2000);
        diag_log_get(&s);assert(s.coalesced==999 && s.evidence_lost && s.faults==1 && !s.priority_dropped);
        for(int i=0;i<1000;i++)diag_log_event(DIAG_ACTION,i,0,0);
        assert(diag_log_fault_get(0,&f) && f.first_sequence==first && f.count==1000);
        restart(4);assert(diag_log_fault_get(0,&f) && f.first_sequence==first && f.count==1000);
        /* Simulated reset at mutable suffix publication preserves first fault. */
        s_faults[0].last.check^=1;restart(4);
        assert(diag_log_fault_get(0,&f) && f.first_sequence==first && f.count==1);
        diag_log_get(&s);assert(s.evidence_lost);
        flush_all();assert(records(DIAG_STORAGE_ERROR,EIO,-1)>=1);
    } else if(!strcmp(argv[1],"timeline")) {
        test_ms=2000;diag_log_event(DIAG_POWER_STATE,0,3,0);
        diag_log_event(DIAG_POWER_SAMPLE,20,3700,7);
        test_ms=3000;diag_log_event(DIAG_POWER_POLICY,1,15,0);
        test_ms=4000;diag_log_event(DIAG_POWER_STATE,3,2,1);
        test_ms=9000;diag_log_event(DIAG_WAKE,2,2,1);diag_log_event(DIAG_POWER_STATE,2,0,1);
        test_ms=10000;diag_log_event(DIAG_POWER_POLICY,0,20,0);
        test_ms=11000;diag_log_event(DIAG_POWER_SAMPLE,18,3600,7);
        /* External supply percentages must not enter the battery slope. */
        diag_log_event(DIAG_POWER_SAMPLE,100,4300,9);
        flush_all();
    } else assert(!"unknown case");
    puts("PASS diagnostics queue, durability, retention and rotation scenario");
    return 0;
}
