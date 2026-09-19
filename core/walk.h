/* Conservative walking-like cadence estimate, not a fitness measurement.
 * Called once per fresh IMU sample, independently of rendering. No allocation. */
#ifndef WATCH_WALK_H
#define WATCH_WALK_H
#include <stdint.h>
#include "hal.h"

typedef struct {
    uint32_t steps, active_seconds;
    int walking, valid;
    uint64_t active_ms;
    uint32_t last_ms, peak_ms, first_peak_ms, high_ms;
    uint32_t intervals[7];
    float baseline, filtered, peak, valley;
    int ready, high, candidates, interval_count;
} walk_state;

void walk_init(walk_state *state);
void walk_update(walk_state *state, uint32_t now_ms,
                 const hal_imu *sample, int sample_valid);
#endif
