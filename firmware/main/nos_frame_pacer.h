/* Completion-aware start scheduling. This never delays a DMA completion,
 * changes animation time, or skips a rendered frame. */
#ifndef NOS_FRAME_PACER_H
#define NOS_FRAME_PACER_H
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t cost[8], at;
    uint64_t advanced_us;
    uint32_t advanced_frames;
    int active;
} nos_frame_pacer;

static inline void nos_frame_pacer_reset(nos_frame_pacer *p)
{
    memset(p, 0, sizeof *p);
}

/* Keep the original start grid as an upper bound. After a cheap paint, reserve
 * enough of the next interval for the largest recent paint. Stable work still
 * sleeps to the usual grid; changing work can start earlier instead of adding
 * a full grid wait before an expensive frame. A new motion starts conservatively
 * for eight paints. Idle returns immediately to normal timer pacing. */
static inline int64_t nos_frame_pacer_next_floor(nos_frame_pacer *p, int enabled,
    int painted, int64_t begin, int64_t completed, int64_t grid, uint32_t period,
    int64_t earliest)
{
    if(grid<earliest)grid=earliest;
    if (!enabled || !painted || !period || completed < begin) {
        p->active = 0;
        return grid;
    }
    if (!p->active) {
        for (unsigned i = 0; i < 8; ++i) p->cost[i] = period;
        p->at = 0;
        p->active = 1;
    }
    uint64_t elapsed = (uint64_t)(completed - begin);
    p->cost[p->at++ & 7u] = elapsed > period ? period : (uint32_t)elapsed;
    uint32_t reserve = 0;
    for (unsigned i = 0; i < 8; ++i)
        if (p->cost[i] > reserve) reserve = p->cost[i];
    int64_t latest = completed + period - reserve;
    if(latest<earliest)latest=earliest;
    if (latest < grid) {
        p->advanced_us += (uint64_t)(grid - latest);
        ++p->advanced_frames;
        return latest;
    }
    return grid;
}
static inline int64_t nos_frame_pacer_next(nos_frame_pacer *p, int enabled,
    int painted, int64_t begin, int64_t completed, int64_t grid, uint32_t period)
{
    return nos_frame_pacer_next_floor(p,enabled,painted,begin,completed,grid,period,INT64_MIN);
}
/* A user limit bounds starts even when the adaptive pacer reserves more work.
 * Unlimited removes the timer deadline; rendering/DMA still finish in order. */
static inline int64_t nos_frame_pacer_capped(nos_frame_pacer *p,int enabled,
    int painted,int64_t begin,int64_t completed,int64_t grid,uint32_t period,
    uint32_t cap_period)
{
    if(!cap_period) {p->active=0;return completed;}
    return nos_frame_pacer_next_floor(p,enabled,painted,begin,completed,grid,period,begin+cap_period);
}
#endif
