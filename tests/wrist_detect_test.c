#include "wrist_detect.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static uint32_t clock_ms;
uint32_t hal_millis(void) { return clock_ms; }
static wrist_detect_status status(void)
{ wrist_detect_status out; wrist_detect_get(&out); return out; }
static wrist_detect_status gesture(int mode,const uint8_t refs[4],float x,float y,float z,int pose,int period)
{
    clock_ms=0; assert(wrist_detect_begin(mode,refs));
    for(int ms=0;ms<=4000;ms+=period) {
        hal_imu sample={.az=pose?1:-1};
        if(ms>600 && ms<=2600) { sample.gx=x;sample.gy=y;sample.gz=z; }
        clock_ms=(uint32_t)ms; wrist_detect_feed(&sample,clock_ms);
    }
    return status();
}
int main(void)
{
    uint8_t refs[4]={0}; wrist_detect_status out;
    clock_ms=0; assert(!wrist_detect_begin(WRIST_DETECT,refs));
    assert(status().reason==WRIST_REASON_MISSING_REFS);
    out=gesture(WRIST_LEARN_LEFT,refs,90,0,0,0,20);
    assert(out.state==WRIST_VALID && out.candidate==WRIST_CANDIDATE_LEFT);
    assert(out.signature==0x81 && out.arc==90 && out.coverage_ms==4000 && out.received==201);
    refs[0]=out.signature;refs[1]=out.arc;
    out=gesture(WRIST_LEARN_RIGHT,refs,-90,0,0,0,10);
    assert(out.state==WRIST_VALID && out.signature==0x82 && out.arc==90);
    refs[2]=out.signature;refs[3]=out.arc;
    out=gesture(WRIST_DETECT,refs,90,0,0,0,20);
    assert(out.state==WRIST_VALID && out.candidate==WRIST_CANDIDATE_LEFT);
    out=gesture(WRIST_DETECT,refs,-90,0,0,0,20);
    assert(out.state==WRIST_VALID && out.candidate==WRIST_CANDIDATE_RIGHT);
    puts("Mirrored bilateral calibration/detection at 50/100Hz: PASS");
    uint8_t ambiguous[4]={0x81,90,0x81,90};
    clock_ms=0;assert(!wrist_detect_begin(WRIST_DETECT,ambiguous));
    assert(status().reason==WRIST_REASON_AMBIGUOUS_REFS);
    out=gesture(WRIST_LEARN_RIGHT,refs,90,0,0,0,20);
    assert(out.state==WRIST_UNKNOWN && out.reason==WRIST_REASON_AMBIGUOUS_REFS);
    out=gesture(WRIST_DETECT,refs,90,90,0,0,20);
    assert(out.state==WRIST_UNKNOWN && out.reason==WRIST_REASON_MIXED_AXIS);
    out=gesture(WRIST_DETECT,refs,0,90,0,0,20);
    assert(out.state==WRIST_UNKNOWN && out.reason==WRIST_REASON_NO_MATCH);
    out=gesture(WRIST_DETECT,refs,180,0,0,0,20);
    assert(out.state==WRIST_UNKNOWN && out.reason==WRIST_REASON_ARC);
    out=gesture(WRIST_DETECT,refs,40,0,0,0,20);
    assert(out.state==WRIST_UNKNOWN && out.reason==WRIST_REASON_NO_MATCH);
    out=gesture(WRIST_DETECT,refs,0,0,0,0,20);
    assert(out.state==WRIST_UNKNOWN && out.reason==WRIST_REASON_ARC);
    out=gesture(WRIST_DETECT,refs,3,2,1,0,20);
    assert(out.state==WRIST_UNKNOWN && out.reason==WRIST_REASON_ARC);
    out=gesture(WRIST_DETECT,refs,90,0,0,1,20);
    assert(out.state==WRIST_UNKNOWN && out.reason==WRIST_REASON_POSE);
    puts("Missing/ambiguous refs, axis/arc/pose mismatch and still/noise rejection: PASS");
    hal_imu sample={.az=-1};
    clock_ms=0;assert(wrist_detect_begin(WRIST_DETECT,refs));
    wrist_detect_feed(&sample,0); wrist_detect_feed(&sample,0);
    assert(status().received==1);
    clock_ms=200;wrist_detect_feed(&sample,clock_ms);
    assert(status().state==WRIST_UNKNOWN && status().reason==WRIST_REASON_GAP);
    clock_ms=0;assert(wrist_detect_begin(WRIST_DETECT,refs));
    clock_ms=200; assert(status().reason==WRIST_REASON_GAP); /* producer silent */
    clock_ms=0;assert(wrist_detect_begin(WRIST_DETECT,refs));
    sample.gx=NAN;wrist_detect_feed(&sample,0);assert(status().reason==WRIST_REASON_SENSOR);
    sample.gx=2000;assert(wrist_detect_begin(WRIST_DETECT,refs));
    wrist_detect_feed(&sample,0);assert(status().reason==WRIST_REASON_SENSOR);
    sample.gx=20;assert(wrist_detect_begin(WRIST_DETECT,refs));
    wrist_detect_feed(&sample,0);assert(status().reason==WRIST_REASON_NOT_QUIET);
    sample.gx=0;assert(wrist_detect_begin(WRIST_DETECT,refs));
    wrist_detect_feed(&sample,20);assert(status().state==WRIST_CAPTURING); /* UI clock trails sample */
    wrist_detect_cancel();assert(status().state==WRIST_IDLE);
    puts("Gap/deadline, duplicate timestamp, NaN/clipping, quiet start and cancel: PASS");
    /* Rotating the strap 180 degrees about gravity leaves gravity unchanged
     * and reverses X/Y gyro signs. That observation is identical to the other
     * calibrated wrist: it MUST NOT be advertised as anatomically identifiable. */
    out=gesture(WRIST_DETECT,refs,-90,0,0,0,20);
    assert(out.state==WRIST_VALID && out.candidate==WRIST_CANDIDATE_RIGHT);
    puts("180-degree strap reversal observational ambiguity demonstrated: PASS (confirmation required)");
    return 0;
}
