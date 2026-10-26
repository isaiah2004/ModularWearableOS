/* knock.h - double-knock detector (knock to lock / unlock).
 *
 * Two sharp taps on the watch, 100-500 ms apart, while the arm is otherwise still. A tap on
 * the glass or case is a short spike of acceleration along the screen's normal (Z) that the
 * gravity-following low-pass doesn't follow; arm motion, steps and swings are slow, broad or
 * periodic and fail the quiet gates around the pair. Fed with raw IMU samples (g, deg/s) at
 * the IMU's rate; pure C, no HAL, so it can be unit tested. */
#ifndef WATCH_KNOCK_H
#define WATCH_KNOCK_H
#include <stdint.h>

typedef struct {
    float lp[3];             /* gravity estimate (low-pass), g */
    uint32_t last_ms;
    int init;
    int above;               /* inside a spike */
    uint32_t spike_ms;       /* start of the current / last spike */
    uint32_t first_ms;       /* the pair's first knock (0: none) */
    int count;               /* knocks in the pair so far */
    uint32_t quiet_ms;       /* time the arm has been still (for the quiet gate) */
    uint32_t move_ms;        /* time the arm has been moving (cancels a pair) */
    float last_peak;         /* debug: strongest spike seen recently, g */
    /* debug: the last notable event, for a log (the caller clears it) */
    int event;               /* KNOCK_EV_* */
    float ev_hz, ev_hxy;     /* the spike's Z and sideways high-passed g */
    uint32_t ev_ms;          /* quiet before / gap / spike length, per event */
    float hz_now, hxy_now;   /* debug: this sample's high-passed Z and sideways g */
    float threshold_g;      /* 0 = default; applied from wrist tuning */
    unsigned quiet_before_ms;
} knock_state;
enum { KNOCK_EV_NONE = 0, KNOCK_EV_FIRST, KNOCK_EV_SECOND, KNOCK_EV_DOUBLE, KNOCK_EV_NOT_QUIET,
       KNOCK_EV_BAD_GAP, KNOCK_EV_TOO_LONG, KNOCK_EV_THIRD, KNOCK_EV_MOVED, KNOCK_EV_SIDEWAYS };

void knock_init(knock_state *k);
/* Returns 1 on a confirmed double knock (reported ~250 ms after the second, once the
 * quiet after it shows it wasn't a run of steps or a third knock). */
int  knock_update(knock_state *k, float ax, float ay, float az, float gyro_dps, uint32_t now_ms);
#endif
