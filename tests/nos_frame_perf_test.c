#include "nos_frame_perf.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    nos_frame_perf s = {0};
    nos_frame_perf_add(&s, 1000, 21000, 26000, 1);
    nos_frame_perf_add(&s, 26000, 47000, 51000, 1);
    assert(s.frames == 2 && s.painted == 2 && s.intervals == 1);
    assert(s.e2e_max == 21000 && s.present_max == 26000);
    assert(s.deadline_misses == 0 && s.floor_misses == 0);
    assert(nos_frame_perf_percentile(&s, 95) == 26);
    nos_frame_perf_add(&s, 51000, 87000, 76000, 1);
    assert(s.deadline_misses == 1 && s.floor_misses == 1);
    assert(nos_frame_perf_percentile(&s, 99) == 40);
    nos_frame_perf_reset(&s);
    assert(s.previous_present == 87000 && s.frames == 0);
    nos_frame_perf_add(&s, 88000, 112000, 113000, 1);
    assert(s.present_max == 25000); /* interval across report boundary */
    nos_frame_perf_add(&s, 113000, 114000, 138000, 0);
    nos_frame_perf_add(&s, 1000000, 1020000, 1025000, 1);
    assert(s.intervals == 1 && s.floor_misses == 0); /* idle is not a hitch */
    nos_frame_perf_add(&s, 1025000, 1120000, 1050000, 1);
    assert(nos_frame_perf_percentile(&s, 99) == 100); /* overflow returns conservative upper bound */
    assert(s.present_max == 100000);
    nos_frame_perf boundary = {0};
    nos_frame_perf_add(&boundary, 0, 1000, 25000, 1);
    nos_frame_perf_add(&boundary, 1000, 34333, 50000, 1);
    assert(boundary.floor_misses == 0);
    nos_frame_perf_add(&boundary, 34333, 67667, 75000, 1);
    assert(boundary.floor_misses == 1);
    nos_frame_capture capture = {0};
    nos_frame_capture_add(&capture, 0, 90000, 25000, 1);
    assert(!capture.perf.frames && capture.state == NOS_CAPTURE_EMPTY);
    nos_frame_capture_begin(&capture);
    nos_frame_capture_add(&capture, 1000, 90000, 26000, 1); /* slow acknowledgement excluded */
    assert(capture.state == NOS_CAPTURE_ACTIVE && capture.start == 90000 && !capture.perf.frames);
    nos_frame_capture_add(&capture, 91000, 111000, 116000, 1);
    nos_frame_capture_add(&capture, 116000, 151000, 141000, 1);
    assert(capture.perf.frames == 2 && capture.perf.intervals == 1);
    assert(capture.perf.e2e_max == 35000 && capture.perf.floor_misses == 1);
    nos_frame_capture_stop(&capture);
    nos_frame_capture_add(&capture, 152000, 500000, 177000, 1); /* report frame excluded */
    assert(capture.state == NOS_CAPTURE_COMPLETE && capture.completed == 151000);
    assert(capture.perf.frames == 2);
    nos_frame_capture_begin(&capture);
    nos_frame_capture_stop(&capture);
    assert(capture.state == NOS_CAPTURE_INCOMPLETE && !capture.perf.frames);
    nos_frame_capture_begin(&capture);
    nos_frame_capture_add(&capture, 0, 1000, 25000, 0);
    nos_frame_capture_add(&capture, 25000, 26000, 50000, 0);
    nos_frame_capture_add(&capture, 50000, 70000, 75000, 1);
    assert(capture.perf.painted == 1 && !capture.perf.intervals); /* sparse capture is not FPS proof */
    nos_frame_capture_abort(&capture);
    nos_frame_capture_stop(&capture);
    nos_frame_capture_add(&capture, 75000, 95000, 100000, 1);
    assert(capture.state == NOS_CAPTURE_ABORTED && capture.perf.frames == 2);
    puts("NOS frame metrics passed");
    return 0;
}
