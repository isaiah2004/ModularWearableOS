#ifndef NOS_POWER_H
#define NOS_POWER_H
#include <stdint.h>
/* UI task owns active_begin/end; dot prewarmer owns bake_begin/end. Never share
 * an esp_pm lock handle across concurrent callers.
 *
 * Automatic light sleep (docs/LIGHT_SLEEP.md) is part of the DFS configuration on
 * the 1.43-B. Two ESP_PM_NO_LIGHT_SLEEP locks gate it: the display lock (owned by
 * the main task, held while the display is ON) and the transfer lock (owned by the
 * command task, held during a BLE/SD transfer). They never raise the CPU clock. */
typedef struct {
    int enabled,error,cpu_mhz,min_mhz,max_mhz,ui_held,bake_held;
    unsigned ui_acquires,ui_releases,bake_acquires,bake_releases;
    int light_sleep,display_held,xfer_held;
    uint32_t sleep_ms,sleep_entries; /* real light sleeps since boot (32-bit, wrap) */
} nos_power_stats;
void nos_power_init(void); /* boot starts protected; failure keeps fixed 240MHz */
void nos_power_active_begin(void);
void nos_power_active_end(void);
void nos_power_bake_begin(void);
void nos_power_bake_end(void);
void nos_power_display_hold(int on); /* main task only; idempotent */
void nos_power_xfer_hold(int on);    /* command task only; idempotent */
/* Hooks run in the idle task inside the sleep critical section: IRAM-safe, never block.
 * before(): about to sleep. after(slept_us): woke, 0 when the sleep was not entered. */
void nos_power_set_sleep_hooks(void (*before)(void),void (*after)(int64_t slept_us));
void nos_power_sleep_stats(uint32_t *sleep_ms,uint32_t *entries);
nos_power_stats nos_power_get_stats(void);
int nos_power_set_max_mhz(int mhz); /* keeps DFS/lock ownership; 80 sensor idle or 240 active */
void nos_power_report(void);
#endif
