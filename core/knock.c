/* knock.c - see knock.h
 *
 * Tuned on real knocks (build/nos/smooth30/lead/knockrec2.txt, IMU at 500 Hz, accel LPF
 * ~67 Hz): a fingertip knock is a 0.9-4 g spike (into the glass, or sideways on the case),
 * followed by the wrist's reaction, 150-290 deg/s for ~100 ms; people double-knock fast,
 * the second knock often ~100 ms after the first, inside that reaction. So a knock is the
 * ONSET of a spike, the reaction after it is ignored, and the pair is judged by the
 * stillness around it: before the first knock and after the second. */
#include "knock.h"
#include <math.h>
#include <string.h>

#define SPIKE_G      0.60f   /* high-passed magnitude crossing this: a knock's onset */
#define REFRACT_MS   70      /* after an onset: the knock's ringing and the wrist's reaction */
#define QUIET_G      0.15f   /* "still": high-passed magnitude under this... */
#define QUIET_DPS    90.0f   /* ...and not rotating fast */
#define QUIET_BEFORE 250     /* ms of stillness before the first knock */
#define GAP_MIN      80      /* second knock this long after the first... */
#define GAP_MAX      520     /* ...and no later */
#define QUIET_AFTER  200     /* ms of stillness after the pair... */
#define AFTER_MAX    900     /* ...reached within this long, or it was not a deliberate pair */

void knock_init(knock_state *k) { memset(k, 0, sizeof *k); }

int knock_update(knock_state *k, float ax, float ay, float az, float gyro_dps, uint32_t now)
{
    float a[3] = { ax, ay, az };
    if (!k->init || now - k->last_ms > 200) {        /* first sample or a gap: start over */
        float threshold=k->threshold_g; unsigned quiet=k->quiet_before_ms;
        memset(k, 0, sizeof *k);
        k->threshold_g=threshold;k->quiet_before_ms=quiet;
        for (int i = 0; i < 3; i++) k->lp[i] = a[i];
        k->init = 1; k->last_ms = now;
        return 0;
    }
    float dt = (float)(now - k->last_ms) * 0.001f;
    uint32_t el = now - k->last_ms;
    k->last_ms = now;
    float hp[3];
    for (int i = 0; i < 3; i++) hp[i] = a[i] - k->lp[i];
    float hz = fabsf(hp[2]), hxy = sqrtf(hp[0] * hp[0] + hp[1] * hp[1]);
    float hm = sqrtf(hz * hz + hxy * hxy);
    k->hz_now = hz; k->hxy_now = hxy;
    /* gravity follows slowly (150 ms) and not through a knock, so the spike stays visible */
    if (hm < 0.30f) {
        float alpha = dt / (0.15f + dt);
        for (int i = 0; i < 3; i++) k->lp[i] += hp[i] * alpha;
    }
    if (hm > k->last_peak) k->last_peak = hm;
    k->last_peak *= 0.98f;

    int refract = k->above && now - k->spike_ms < REFRACT_MS;
    if (!refract) k->above = 0;
    float threshold=k->threshold_g>=.20f && k->threshold_g<=1.2f?k->threshold_g:SPIKE_G;
    unsigned before=k->quiet_before_ms>=80 && k->quiet_before_ms<=400?k->quiet_before_ms:QUIET_BEFORE;
    if (hm > threshold && !refract) {                  /* a knock's onset */
        k->above = 1;
        k->spike_ms = now;
        k->ev_hz = hz; k->ev_hxy = hxy;
        if (k->count == 0) {
            k->ev_ms = k->quiet_ms;
            if (k->quiet_ms >= before) { k->count = 1; k->first_ms = now; k->event = KNOCK_EV_FIRST; }
            else k->event = KNOCK_EV_NOT_QUIET;
        } else if (k->count == 1) {
            uint32_t gap = now - k->first_ms;
            k->ev_ms = gap;
            if (gap >= GAP_MIN && gap <= GAP_MAX) { k->count = 2; k->event = KNOCK_EV_SECOND; }
            else { k->count = 0; k->event = KNOCK_EV_BAD_GAP; }
        } else { k->count = 0; k->event = KNOCK_EV_THIRD; k->ev_ms = now - k->first_ms; }
        k->quiet_ms = 0;
        return 0;
    }
    int quiet = !refract && hm < QUIET_G && gyro_dps < QUIET_DPS;
    if (quiet) { if (k->quiet_ms < 10000) k->quiet_ms += el; }
    else k->quiet_ms = 0;
    if (k->count == 1 && now - k->first_ms > GAP_MAX) k->count = 0;   /* one knock only */
    if (k->count == 2) {
        if (k->quiet_ms >= QUIET_AFTER) { k->count = 0; k->event = KNOCK_EV_DOUBLE; return 1; }
        if (now - k->first_ms > AFTER_MAX) {          /* never settled: motion, not knocks */
            k->count = 0; k->event = KNOCK_EV_MOVED; k->ev_ms = now - k->first_ms;
        }
    }
    return 0;
}
