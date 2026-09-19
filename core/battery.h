/* Shared view of the board's cached power observations. Updating the adapter
 * does not resample, smooth, or infer current from the display frame rate. */
#ifndef WATCH_BATTERY_H
#define WATCH_BATTERY_H

/* Preserve legacy IDs. This board cannot establish CHARGING or FULL, so the
 * adapter never emits either. DISCHARGING means inferred battery supply. */
enum { BATT_DISCHARGING = 0, BATT_CHARGING = 1, BATT_FULL = 2,
       BATT_EXTERNAL = 3, BATT_UNKNOWN = 4 };

void battery_update(void);   /* latch one qualified HAL snapshot for this frame */
int  battery_mv(void);       /* measured system rail in mV, or 0 if unavailable */
int  battery_pct(void);      /* last valid estimate 0-100; legacy initial value 50 */
int  battery_valid(void);    /* whether the percentage is currently meaningful */
int  battery_state(void);    /* inferred battery supply, external power, unknown */

#endif
