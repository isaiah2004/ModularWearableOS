#ifndef WORKOUT_MOTION_H
#define WORKOUT_MOTION_H
#include <stdint.h>
/* Conservative wrist movement episodes, never shots/repetitions. Units g, deg/s.
 * Durations only cover adjacent valid samples <=250 ms apart. */
typedef struct {
    uint32_t valid_ms, movement_ms, bursts;
    uint32_t last_ms, candidate_ms, quiet_ms;
    uint8_t have, active;
    uint16_t invalid, saturated, gaps;
    uint32_t accepted_ms, rejected_ms;
    uint8_t reject_reason; /* 1 invalid, 2 saturated, 3 gap */
} workout_motion;
void workout_motion_reset(workout_motion *s);
void workout_motion_boundary(workout_motion *s);
void workout_motion_feed(workout_motion *s,uint32_t now,
                         float ax,float ay,float az,float gx,float gy,float gz);
#endif
