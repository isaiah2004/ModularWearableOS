#ifndef NOS_DISPLAY_TE_H
#define NOS_DISPLAY_TE_H
#include <stdint.h>

typedef struct {
    int gpio, ready, enabled, awake, error;
    uint32_t edges, period_us, edge_age_us, waits, synced, timeouts;
    uint32_t last_wait_us, max_wait_us, last_phase_us, max_phase_us;
    uint64_t wait_total_us;
} nos_display_te_stats;

/* C-board GPIO8, rising edge of CO5300 vertical blank (35h=0, 44h=0).
 * The B board has no TE route; all operations there are no-ops. */
void nos_display_te_init(void);
void nos_display_te_set_enabled(int enabled);
void nos_display_te_set_awake(int awake);
/* Flush owner only, immediately before the FIRST DMA submission in a frame.
 * Missing TE falls back after 25ms; this is not a DMA-completion fence. */
void nos_display_te_wait(void);
nos_display_te_stats nos_display_te_snapshot(void);
#endif
