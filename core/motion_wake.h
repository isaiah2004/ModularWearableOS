/* Small, allocation-free gesture detector. Raw accelerometer includes gravity.
 * Supply each fresh sensor sample once, using its acquisition timestamp. */
#ifndef WATCH_MOTION_WAKE_H
#define WATCH_MOTION_WAKE_H
#include "hal.h"
#include <stdint.h>

enum { MOTION_WAKE_NONE=0, MOTION_WAKE_PICKUP=1, MOTION_WAKE_TURN=2 };
enum { MW_DISABLED=0, MW_WAITING, MW_ARMED, MW_MOVING, MW_COOLDOWN };
typedef struct { int pickup,turn;float view[3]; } motion_wake_config;
typedef struct {
    motion_wake_config cfg;
    float gravity[3],previous_accel[3],bias[3],origin[3],previous_gyro[3];
    float magnitude,speed,view_dot,angle,peak_linear;
    uint32_t last_ms,still_ms,gyro_still_ms,away_ms,hold_ms,started_ms,cooldown_ms,motion_ms,linear_ms;
    int initialized,bias_ready,eligible,pickup_armed,turn_armed,candidate,reversals,stage;
} motion_wake_state;

void motion_wake_init(motion_wake_state *s,const motion_wake_config *cfg);
int motion_wake_update(motion_wake_state *s,const hal_imu *imu,
                       uint32_t sample_ms,int valid,int eligible);
const char *motion_wake_reason(int reason);
#endif
