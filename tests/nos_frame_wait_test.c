#include "nos_frame_wait.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    int64_t now,alarm;
    int armed,signal,arm_error,missing,early,stall_setup;
    int arms,stops,waits,faults,phase,error;
    int64_t requested;
} timer;
static int64_t now_us(void *arg) {return ((timer *)arg)->now;}
static int arm(void *arg,uint64_t delay)
{
    timer *t=arg;t->arms++;
    if(t->arm_error)return t->arm_error;
    t->armed=1;t->alarm=t->now+(int64_t)delay;return 0;
}
static void stop(void *arg)
{
    timer *t=arg;t->stops++;t->armed=0;
    if(t->stall_setup) {t->now+=t->stall_setup;t->stall_setup=0;}
}
static int take(void *arg,uint32_t timeout_ms)
{
    timer *t=arg;
    if(!timeout_ms) {int signal=t->signal;t->signal=0;return signal;}
    t->waits++;assert(timeout_ms<=89); /* max expected frame + bounded grace */
    if(t->early) {t->early--;t->now+=1000;return 1;}
    if(t->missing || !t->armed) {t->now+=(int64_t)timeout_ms*1000;return 0;}
    assert(t->alarm<=t->now+(int64_t)timeout_ms*1000);
    t->now=t->alarm;t->armed=0;return 1;
}
static void fault(void *arg,int64_t requested,int error,int phase)
{timer *t=arg;t->faults++;t->requested=requested;t->error=error;t->phase=phase;}
static int run(timer *t,int64_t deadline)
{
    const nos_frame_wait_ops ops={t,now_us,arm,stop,take,fault};
    return nos_frame_wait_until(&ops,deadline,68666);
}
int main(void)
{
    timer t={0};assert(run(&t,33333));
    assert(t.now==33333 && t.arms==1 && t.waits==1 && !t.faults && !t.armed);
    /* A leftover binary token cannot make a future deadline complete early. */
    memset(&t,0,sizeof(t));t.signal=1;assert(run(&t,33333));assert(t.now==33333 && t.waits==1);
    /* Failure to arm must never enter a semaphore wait. Preserve SDK error. */
    memset(&t,0,sizeof(t));t.arm_error=0x103;assert(!run(&t,33333));
    assert(!t.waits && t.faults==1 && t.phase==NOS_FRAME_WAIT_ARM && t.error==0x103 && t.requested==33333 && !t.armed);
    /* A successful arm with no callback expires in finite time and is cleared. */
    memset(&t,0,sizeof(t));t.missing=1;assert(!run(&t,33333));
    assert(t.waits==1 && t.now==54000 && t.phase==NOS_FRAME_WAIT_TIMEOUT && t.error==NOS_FRAME_WAIT_NO_CALLBACK && !t.armed);
    /* In-flight old callback after stop: ignore its early signal, await deadline. */
    memset(&t,0,sizeof(t));t.early=1;assert(run(&t,33333));
    assert(t.waits==2 && t.now==33333 && !t.faults);
    /* A stream of spurious tokens is also bounded; never spin until forever. */
    memset(&t,0,sizeof(t));t.early=100;assert(!run(&t,33333));
    assert(t.waits==4 && t.phase==NOS_FRAME_WAIT_EARLY && t.error==NOS_FRAME_WAIT_STALE_SIGNALS && !t.armed);
    /* Corrupt/unreasonable future scheduling cannot become a multiday sleep. */
    memset(&t,0,sizeof(t));assert(!run(&t,1000000000));
    assert(!t.arms && !t.waits && t.phase==NOS_FRAME_WAIT_DEADLINE && t.requested==1000000000);
    /* Scheduler delay during setup can already satisfy the deadline. */
    memset(&t,0,sizeof(t));t.stall_setup=5000;assert(run(&t,1000));assert(!t.arms && !t.waits && !t.faults);
    memset(&t,0,sizeof(t));t.now=1000;assert(run(&t,500));assert(!t.arms && !t.waits);
    puts("PASS bounded frame timer: SDK arm failure, missing callback, stale signals, deadlines and recovery cleanup");
}
