#include "nos_frame_wait.h"

static void clear_timer(const nos_frame_wait_ops *ops)
{
    ops->stop(ops->context);
    /* Binary semaphore holds at most one notification. A callback dispatched
     * before stop may race the drain; deadline checks reject its stale signal. */
    for(int i=0;i<2;i++)if(!ops->take(ops->context,0))break;
}
static int fail(const nos_frame_wait_ops *ops,int64_t requested,int error,int phase)
{
    clear_timer(ops);
    if(ops->fault)ops->fault(ops->context,requested,error,phase);
    return 0;
}
int nos_frame_wait_until(const nos_frame_wait_ops *ops,int64_t deadline_us,uint32_t max_delay_us)
{
    int64_t now=ops->now_us(ops->context);
    if(deadline_us<=now)return 1;
    int64_t requested=deadline_us-now;
    if(requested>(int64_t)max_delay_us)
        return fail(ops,requested,NOS_FRAME_WAIT_BAD_DELAY,NOS_FRAME_WAIT_DEADLINE);
    clear_timer(ops);
    now=ops->now_us(ops->context);
    if(now>=deadline_us)return 1;
    int error=ops->arm(ops->context,(uint64_t)(deadline_us-now));
    if(error)return fail(ops,requested,error,NOS_FRAME_WAIT_ARM);
    for(int attempts=0;attempts<4;attempts++) {
        now=ops->now_us(ops->context);
        if(now>=deadline_us) {clear_timer(ops);return 1;}
        uint32_t wait_ms=(uint32_t)((deadline_us-now+999)/1000)+20u;
        if(!ops->take(ops->context,wait_ms))
            return fail(ops,requested,NOS_FRAME_WAIT_NO_CALLBACK,NOS_FRAME_WAIT_TIMEOUT);
        /* A signal alone is insufficient: callbacks already in flight can
         * arrive after stop. Never consume one as the next frame's deadline. */
    }
    if(ops->now_us(ops->context)>=deadline_us) {clear_timer(ops);return 1;}
    return fail(ops,requested,NOS_FRAME_WAIT_STALE_SIGNALS,NOS_FRAME_WAIT_EARLY);
}
