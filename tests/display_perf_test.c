#include <assert.h>
#include <string.h>
#include "display_perf.h"

int main(void)
{
    display_perf_snapshot s;
    display_perf_trace t;
    display_perf_init();

    display_perf_frame(100, 1, false, 500, 16001, 0, 17000);
    display_perf_add_stage(DISPLAY_PERF_ROTATE, 33000);
    display_perf_queue_depth(0); /* measured zero must differ from unavailable */
    display_perf_submit(200, 1000);
    display_perf_complete_isr(0, 2000);
    display_perf_submit(300, 3000);
    display_perf_cancel_submit();
    display_perf_frame(0, 0, false, 20, 0, 9, 30);
    display_perf_snapshot_get(&s);
    assert(s.frames == 2 && s.render_frames == 2 && s.present_frames == 1);
    assert(s.present.samples == 1); /* no-damage evaluation is not a present */
    assert(s.deadline_misses == 1 && s.completed_bytes == 200);
    assert(s.cancelled_transactions == 1 && s.cancelled_bytes == 300);
    assert(s.submitted_transactions == s.completed_transactions +
                                        s.cancelled_transactions);
    assert(s.submitted_bytes == s.completed_bytes + s.cancelled_bytes);
    assert(s.wall.p50_us == 1000);        /* 30 us sample's 1 ms bucket */
    assert(s.wall.p95_us == 17000);       /* upper sample at nearest rank */
    assert(s.stage[DISPLAY_PERF_ROTATE].p50_us == 40000); /* 8 ms buckets */
    assert(s.queue_depth_valid && s.queue_highwater == 0);

    display_perf_submit(50, 9000);        /* predates trace */
    display_perf_trace_start("quote\"test", 1, 10000);
    display_perf_complete_isr(0, 11000);
    display_perf_submit(75, 12000);
    display_perf_complete_isr(0, 13000);
    assert(display_perf_tick(1010000, &t));
    assert(!strcmp(t.name, "quote\"test"));
    assert(t.perf.completed_bytes == 125);
    assert(t.perf.submit_to_complete.samples == 1); /* excludes pre-trace submit */
    assert(t.perf.unmatched_completions == 1);
    assert(t.perf.completion_cadence.samples == 1);
    assert(t.perf.elapsed_us == 1000000);
    assert(display_perf_trace_count() == 1);
    display_perf_print("PERF_APP", &t.perf);
    return 0;
}
