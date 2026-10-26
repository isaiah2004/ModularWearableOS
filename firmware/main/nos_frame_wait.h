#ifndef NOS_FRAME_WAIT_H
#define NOS_FRAME_WAIT_H
#include <stdint.h>
enum { NOS_FRAME_WAIT_DEADLINE=1, NOS_FRAME_WAIT_ARM, NOS_FRAME_WAIT_TIMEOUT, NOS_FRAME_WAIT_EARLY };
enum { NOS_FRAME_WAIT_BAD_DELAY=-1, NOS_FRAME_WAIT_NO_CALLBACK=-2, NOS_FRAME_WAIT_STALE_SIGNALS=-3 };
typedef struct {
    void *context;
    int64_t (*now_us)(void *context);
    int (*arm)(void *context,uint64_t delay_us); /* zero succeeds; preserve ESP error */
    void (*stop)(void *context);              /* stopping an inactive timer is harmless */
    int (*take)(void *context,uint32_t timeout_ms); /* timeout=0 drains one signal */
    void (*fault)(void *context,int64_t requested_us,int error,int phase);
} nos_frame_wait_ops;
/* Pacing only: a missing callback must never block the UI indefinitely.
 * Returns one on deadline reached, zero on fault (caller rebases its pacer).
 * This hardens timer delivery; it does not identify a historical hang cause. */
int nos_frame_wait_until(const nos_frame_wait_ops *ops,int64_t deadline_us,uint32_t max_delay_us);
#endif
