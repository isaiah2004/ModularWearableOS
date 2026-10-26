#include "wrist_detect.h"
#include <math.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE wrist_lock=portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&wrist_lock)
#define UNLOCK() portEXIT_CRITICAL(&wrist_lock)
#else
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#endif
#define CAPTURE_MS 4000u
#define QUIET_MS 600u
static struct {
    wrist_detect_status out;
    uint8_t refs[4];
    int mode, have, moving;
    uint32_t start, last, quiet_end;
    float signed_arc[3], absolute_arc[3];
} s;
static int signature_valid(unsigned signature)
{ unsigned axis=signature&7u; return axis>=1 && axis<=6 && (signature&~0x87u)==0 && (signature&0x80u); }
static int pair_valid(unsigned a,unsigned arc_a,unsigned b,unsigned arc_b)
{
    if(!signature_valid(a)||!signature_valid(b)||arc_a<30||arc_b<30||arc_a>110||arc_b>110) return 0;
    unsigned x=a&7u,y=b&7u;
    return ((x-1)/2==(y-1)/2) && ((x^y)&1u) &&
           arc_a*100u<=arc_b*140u && arc_b*100u<=arc_a*140u &&
           (arc_a>arc_b?arc_a-arc_b:arc_b-arc_a)<=20;
}
static void reject(int reason)
{ s.out.state=WRIST_UNKNOWN; s.out.reason=reason; s.out.candidate=WRIST_CANDIDATE_UNKNOWN; s.out.remaining_ms=0; }
int wrist_detect_begin(int mode,const uint8_t refs[4])
{
    uint32_t now=hal_millis();
    LOCK(); memset(&s,0,sizeof s); s.mode=mode; s.start=s.last=now;
    if(refs) memcpy(s.refs,refs,4);
    s.out.state=WRIST_CAPTURING; s.out.remaining_ms=CAPTURE_MS;
    if(mode<0||mode>2) reject(WRIST_REASON_SENSOR);
    if(mode==WRIST_DETECT) {
        if(!s.refs[0]||!s.refs[1]||!s.refs[2]||!s.refs[3]) reject(WRIST_REASON_MISSING_REFS);
        else if(!pair_valid(s.refs[0],s.refs[1],s.refs[2],s.refs[3])) reject(WRIST_REASON_AMBIGUOUS_REFS);
    }
    int ok=s.out.state==WRIST_CAPTURING; UNLOCK(); return ok;
}
void wrist_detect_cancel(void)
{ LOCK(); memset(&s,0,sizeof s); s.out.reason=WRIST_REASON_CANCELLED; UNLOCK(); }
void wrist_detect_get(wrist_detect_status *out)
{
    if(!out)return;
    uint32_t now=hal_millis();
    LOCK();
    if(s.out.state==WRIST_CAPTURING && (int32_t)(now-s.last)>150) reject(WRIST_REASON_GAP);
    *out=s.out; UNLOCK();
}
static void complete(void)
{
    float total=s.absolute_arc[0]+s.absolute_arc[1]+s.absolute_arc[2];
    int axis=0;
    if(fabsf(s.signed_arc[1])>fabsf(s.signed_arc[axis]))axis=1;
    if(fabsf(s.signed_arc[2])>fabsf(s.signed_arc[axis]))axis=2;
    float arc=fabsf(s.signed_arc[axis]);
    if(!s.moving || arc<60 || arc>220) { reject(WRIST_REASON_ARC); return; }
    if(total<=0 || arc/total<.85f) { reject(WRIST_REASON_MIXED_AXIS); return; }
    if(s.quiet_end<400) { reject(WRIST_REASON_NOT_QUIET); return; }
    unsigned signature=0x80u+(unsigned)axis*2u+1u+(s.signed_arc[axis]<0);
    unsigned quantized=(unsigned)(arc*.5f+.5f);
    s.out.signature=(uint8_t)signature; s.out.arc=(uint8_t)quantized;
    int candidate=s.mode;
    if(s.mode==WRIST_DETECT) {
        candidate=WRIST_CANDIDATE_UNKNOWN;
        for(int i=0;i<2;i++) {
            unsigned ref=s.refs[i*2],ref_arc=s.refs[i*2+1];
            unsigned delta=quantized>ref_arc?quantized-ref_arc:ref_arc-quantized;
            if(signature==ref && delta<=10 && delta*100u<=ref_arc*25u) candidate=i+1;
        }
        if(!candidate) { reject(WRIST_REASON_NO_MATCH); return; }
    } else {
        int other=s.mode==WRIST_LEARN_LEFT?2:0;
        if(s.refs[other] && s.refs[other+1] &&
           !pair_valid(signature,quantized,s.refs[other],s.refs[other+1])) {
            reject(WRIST_REASON_AMBIGUOUS_REFS); return;
        }
    }
    s.out.state=WRIST_VALID; s.out.reason=WRIST_REASON_NONE;
    s.out.candidate=candidate; s.out.remaining_ms=0;
}
void wrist_detect_feed(const hal_imu *v,uint32_t now)
{
    if(!v)return;
    LOCK();
    if(s.out.state!=WRIST_CAPTURING) { UNLOCK(); return; }
    /* Same timestamp never counts twice, including multiple consumers of one read. */
    if(s.have && now==s.last) { UNLOCK(); return; }
    s.out.received++;
    float g[3]={v->gx,v->gy,v->gz};
    float a=v->ax*v->ax+v->ay*v->ay+v->az*v->az;
    if(!isfinite(a)||!isfinite(g[0])||!isfinite(g[1])||!isfinite(g[2])||
       a>9 || fabsf(g[0])>1900 || fabsf(g[1])>1900 || fabsf(g[2])>1900) {
        reject(WRIST_REASON_SENSOR); UNLOCK(); return;
    }
    if(!s.have) s.have=1;
    uint32_t dt=now-s.last, elapsed=now-s.start;
    s.last=now;
    if(dt>150) { reject(WRIST_REASON_GAP); UNLOCK(); return; }
    s.out.coverage_ms+=dt;
    s.out.remaining_ms=elapsed<CAPTURE_MS?CAPTURE_MS-elapsed:0;
    float speed=fabsf(g[0])+fabsf(g[1])+fabsf(g[2]);
    if(elapsed<=QUIET_MS) {
        if(a<.81f || a>1.21f || v->az>-.80f || fabsf(v->ax)>.45f || fabsf(v->ay)>.45f) reject(WRIST_REASON_POSE);
        else if(speed>12) reject(WRIST_REASON_NOT_QUIET);
    } else {
        if(speed>12) { s.moving=1;s.quiet_end=0; }
        else if(s.moving) s.quiet_end+=dt;
        /* Integrate only motion above the declared quiet threshold. */
        if(speed>12) for(int i=0;i<3;i++) {
            float degrees=g[i]*(float)dt*.001f;
            s.signed_arc[i]+=degrees; s.absolute_arc[i]+=fabsf(degrees);
        }
        if(elapsed>=CAPTURE_MS) complete();
    }
    UNLOCK();
}
