/* Host-side scheduling metrics; all times are monotonic microseconds. */
#ifndef NOS_FRAME_PERF_H
#define NOS_FRAME_PERF_H
#include <stdint.h>
#include <string.h>
typedef struct {
    uint32_t frames, painted, deadline_misses, floor_misses, intervals;
    int64_t e2e_max, present_max, previous_present, first_begin, last_begin;
    uint32_t interval_ms[64]; /* ceil-ms bins; final bin includes overflow */
} nos_frame_perf;
static inline void nos_frame_perf_add(nos_frame_perf *s, int64_t begin,
                                      int64_t completed, int64_t deadline, int painted)
{
    if (!s->frames) s->first_begin = begin;
    s->last_begin = begin;
    s->frames++;
    int64_t elapsed = completed - begin;
    if (elapsed > s->e2e_max) s->e2e_max = elapsed;
    if (completed > deadline) s->deadline_misses++;
    if (painted) {
        s->painted++;
        if (s->previous_present) {
            int64_t interval = completed - s->previous_present;
            if (interval > s->present_max) s->present_max = interval;
            if (interval > 1000000 / 30) s->floor_misses++; /* 33,334 us is below 30 Hz */
            unsigned bin = (unsigned)((interval + 999) / 1000);
            s->interval_ms[bin < 64 ? bin : 63]++; s->intervals++;
        }
        s->previous_present = completed;
    } else s->previous_present = 0; /* idle frames are not missed animation frames */
}
static inline unsigned nos_frame_perf_percentile(const nos_frame_perf *s, unsigned pct)
{
    if (!s->intervals) return 0;
    uint32_t rank = (s->intervals * pct + 99) / 100, count = 0;
    for (unsigned i = 0; i < 64; i++) {
        count += s->interval_ms[i];
        if (count >= rank) return i < 63 ? i : (unsigned)((s->present_max + 999) / 1000);
    }
    return 63;
}
static inline void nos_frame_perf_reset(nos_frame_perf *s)
{
    int64_t previous = s->previous_present;
    memset(s, 0, sizeof *s); s->previous_present = previous;
}
/* Optional capture without periodic performance logging. Arming excludes the command/ack frame;
 * stopping happens before the next frame, so reporting cannot alter its data.
 * Power changes abort the window rather than silently dropping their delay. */
enum { NOS_CAPTURE_EMPTY, NOS_CAPTURE_ARMED, NOS_CAPTURE_ACTIVE,
       NOS_CAPTURE_COMPLETE, NOS_CAPTURE_INCOMPLETE, NOS_CAPTURE_ABORTED };
typedef struct {
    nos_frame_perf perf;
    int state;
    int64_t start, completed;
} nos_frame_capture;
static inline int nos_frame_capture_live(const nos_frame_capture *c)
{
    return c->state == NOS_CAPTURE_ARMED || c->state == NOS_CAPTURE_ACTIVE;
}
static inline void nos_frame_capture_begin(nos_frame_capture *c)
{
    memset(c, 0, sizeof *c); c->state = NOS_CAPTURE_ARMED;
}
static inline void nos_frame_capture_add(nos_frame_capture *c, int64_t begin,
                                        int64_t completed, int64_t deadline, int painted)
{
    if (c->state == NOS_CAPTURE_ARMED) {
        c->start = c->completed = completed;
        c->state = NOS_CAPTURE_ACTIVE;
    } else if (c->state == NOS_CAPTURE_ACTIVE) {
        nos_frame_perf_add(&c->perf, begin, completed, deadline, painted);
        c->completed = completed;
    }
}
static inline void nos_frame_capture_stop(nos_frame_capture *c)
{
    if (c->state == NOS_CAPTURE_ARMED ||
        (c->state == NOS_CAPTURE_ACTIVE && !c->perf.frames))
        c->state = NOS_CAPTURE_INCOMPLETE;
    else if (c->state == NOS_CAPTURE_ACTIVE)
        c->state = NOS_CAPTURE_COMPLETE;
}
static inline void nos_frame_capture_abort(nos_frame_capture *c)
{
    if (nos_frame_capture_live(c)) c->state = NOS_CAPTURE_ABORTED;
}
#endif
