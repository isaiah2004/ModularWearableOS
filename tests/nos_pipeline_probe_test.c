#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "../firmware/main/nos_pipeline_probe.h"
#define FRAMES 5000
static nos_pipeline_live live;
static atomic_int phase;
static const uint64_t base=(1ULL<<40)+UINT32_MAX-10;
static void await(int n) {while(atomic_load_explicit(&phase,memory_order_acquire)!=n) {}}
static void publish(int n) {atomic_store_explicit(&phase,n,memory_order_release);}
static void *worker(void *unused) {
    (void)unused;
    for(unsigned i=0;i<FRAMES;++i) {
        await(1);uint64_t begin=base+i*25000ULL;
        for(unsigned chunk=0;chunk<2;++chunk) {
            nos_pipeline_raw(&live,chunk?7:9999); /* first raw wait excluded */
            nos_pipeline_dmawait(&live,2);nos_pipeline_rotation(&live,3);
            nos_pipeline_enter(&live,begin+15+chunk*20,16);
            publish(2);await(3); /* completion precedes API return */
            nos_pipeline_return(&live,begin+15+chunk*20,begin+25+chunk*20);
        }
        publish(4);
    }
    return NULL;
}
static void *completion(void *unused) {
    (void)unused;
    for(unsigned i=0;i<FRAMES;++i)for(unsigned chunk=0;chunk<2;++chunk) {
        await(2);nos_pipeline_done(&live,base+i*25000ULL+20+chunk*20);publish(3);
    }
    return NULL;
}
int main(void) {
    assert(sizeof(nos_pipeline_live)+sizeof(nos_pipeline_capture)<=2048);
    nos_pipeline_capture capture={0};pthread_t w,isr;
    assert(!pthread_create(&w,NULL,worker,NULL));assert(!pthread_create(&isr,NULL,completion,NULL));
    for(unsigned i=0;i<FRAMES;++i) {
        uint64_t begin=base+i*25000ULL;nos_pipeline_begin(&live,begin);publish(1);
        nos_pipeline_submit(&live,2);nos_pipeline_submit(&live,5);await(4);
        nos_pipeline_frame f=nos_pipeline_finish(&live,i+1,begin+50,begin+51,
                                               begin+60+(i==FRAMES-1?1000:0),4,5,6,7);
        assert(!live.active && f.begin_us==begin && f.frame_id==i+1);
        assert(f.rawjobs==2 && f.chunks==2 && f.done==2 && f.bytes==32);
        assert(f.first_enter==15 && f.first_done==20 && f.first_return==25);
        assert(f.last_done==40 && f.last_return==45 && f.done_gap_max==20);
        assert(f.api_sum==20 && f.api_max==10 && f.dmawait_sum==4 && f.rotation_sum==6);
        assert(f.queuewait_sum==7 && f.queuewait_max==7 && f.tick==4 && f.history==5 && f.raster==6 && f.ui_wait==7);
        assert(f.submit_sum==7 && f.submit_max==5);
        f.record=i;f.compare=2;f.layer_record=3;f.layer_raster=4;f.layer_bands=5;f.layer_reused=6;
        nos_pipeline_capture_add(&capture,&f,1);
    }
    assert(!pthread_join(w,NULL));assert(!pthread_join(isr,NULL));
    assert(capture.frames==FRAMES && capture.chunks==2*FRAMES && !capture.mismatch);
    assert(capture.worst_valid && capture.worst_interval==26000);
    assert(capture.worst_current.frame_id==FRAMES && capture.worst_previous.frame_id==FRAMES-1);
    assert(capture.worst_current.record==FRAMES-1 && capture.worst_previous.record==FRAMES-2);
    assert(capture.worst_current.layer_bands==5 && capture.worst_current.submit_sum==7);
    nos_pipeline_frame f=capture.previous;f.begin_us+=1000000;
    nos_pipeline_capture_add(&capture,&f,0);f.begin_us+=1000000;nos_pipeline_capture_add(&capture,&f,1);
    assert(capture.worst_interval==26000); /* idle breaks presentation pair */
    puts("PASS pipeline: 5000 worker/ISR/UI fenced frames, rollover, early callbacks, quiet worst-pair and idle break");
}
