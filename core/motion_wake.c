#include "motion_wake.h"
#include <math.h>
#include <string.h>

static float length3(const float *v)
{ return sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); }
static float dot3(const float *a,const float *b)
{ return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static void cancel(motion_wake_state *s)
{
    s->still_ms=s->away_ms=s->hold_ms=s->motion_ms=s->linear_ms=0;
    s->pickup_armed=s->turn_armed=s->candidate=s->reversals=0;
    s->angle=s->peak_linear=0;
}
void motion_wake_init(motion_wake_state *s,const motion_wake_config *cfg)
{
    memset(s,0,sizeof *s);s->cfg=*cfg;
    float norm=length3(s->cfg.view);
    if(!isfinite(norm) || norm<.8f || norm>1.2f) {
        s->cfg.view[0]=s->cfg.view[1]=0;s->cfg.view[2]=-1;
    } else for(int i=0;i<3;i++)s->cfg.view[i]/=norm;
}
const char *motion_wake_reason(int reason)
{ return reason==MOTION_WAKE_PICKUP?"pickup":reason==MOTION_WAKE_TURN?"wrist":"none"; }
int motion_wake_update(motion_wake_state *s,const hal_imu *m,uint32_t now,int valid,int eligible)
{
    if(!valid || !isfinite(m->ax) || !isfinite(m->ay) || !isfinite(m->az) ||
       !isfinite(m->gx) || !isfinite(m->gy) || !isfinite(m->gz)) {
        cancel(s);s->gyro_still_ms=0;s->initialized=0;s->stage=MW_WAITING;return 0;
    }
    if(s->initialized && now==s->last_ms)return 0;
    float a[3]={m->ax,m->ay,m->az},raw_g[3]={m->gx,m->gy,m->gz};
    float norm=length3(a);s->magnitude=norm;
    if(norm<.35f || norm>2.5f) { /* free fall / impact is not a viewing gesture */
        cancel(s);s->gyro_still_ms=0;s->initialized=0;s->stage=MW_WAITING;return 0;
    }
    uint32_t elapsed=now-s->last_ms;
    if(!s->initialized || elapsed>200) {
        cancel(s);s->gyro_still_ms=0;s->initialized=1;s->last_ms=now;s->eligible=eligible;
        for(int i=0;i<3;i++){s->gravity[i]=a[i]/norm;s->previous_accel[i]=a[i];s->previous_gyro[i]=0;}
        s->view_dot=dot3(s->gravity,s->cfg.view);s->stage=eligible?MW_WAITING:MW_DISABLED;
        return 0;
    }
    s->last_ms=now;
    float dt=elapsed*.001f,delta[3],gyro[3];
    for(int i=0;i<3;i++) {
        delta[i]=a[i]-s->previous_accel[i];s->previous_accel[i]=a[i];
        gyro[i]=raw_g[i]-s->bias[i];
        s->gravity[i]+=(a[i]/norm-s->gravity[i])*(dt/(.08f+dt));
    }
    float gravity_norm=length3(s->gravity);
    if(gravity_norm>.1f)for(int i=0;i<3;i++)s->gravity[i]/=gravity_norm;
    s->speed=length3(gyro);s->view_dot=dot3(s->gravity,s->cfg.view);
    float linear=fabsf(norm-1),jitter=length3(delta),raw_speed=length3(raw_g);
    int steady=linear<.07f && jitter<.025f && raw_speed<12;
    if(steady) {
        if(s->still_ms<10000)s->still_ms+=elapsed;
        if(s->gyro_still_ms<10000)s->gyro_still_ms+=elapsed;
        if(s->gyro_still_ms>=1500 && !s->candidate) {
            for(int i=0;i<3;i++)s->bias[i]=s->bias_ready?
                s->bias[i]+(raw_g[i]-s->bias[i])*(dt/(3+dt)):raw_g[i];
            s->bias_ready=1;
        }
    } else s->still_ms=s->gyro_still_ms=0;
    if(!eligible || !(s->cfg.pickup||s->cfg.turn)) {
        s->eligible=0;cancel(s);s->stage=MW_DISABLED;return 0;
    }
    if(!s->eligible) { cancel(s);s->eligible=1; }
    if(s->stage==MW_COOLDOWN) {
        if(now-s->cooldown_ms<2000)return 0;
        s->stage=MW_WAITING;
    }
    if(linear>.65f || s->speed>280) {
        cancel(s);s->stage=MW_COOLDOWN;s->cooldown_ms=now;return 0;
    }
    if(!s->candidate) {
        if(s->cfg.pickup && s->still_ms>=500)s->pickup_armed=1;
        if(s->view_dot>.7071f && s->still_ms>=500)s->turn_armed=0;
        if(s->view_dot<.50f) {
            if(s->away_ms<2000)s->away_ms+=elapsed;
            if(s->cfg.turn && s->away_ms>=250)s->turn_armed=1;
        } else s->away_ms=0;
        int moving=s->speed>22 || linear>.12f;
        if(moving && (s->pickup_armed || s->turn_armed))s->motion_ms+=elapsed;
        else s->motion_ms=0;
        if(s->motion_ms>=40) {
            s->candidate=s->turn_armed?MOTION_WAKE_TURN:MOTION_WAKE_PICKUP;
            s->started_ms=now;s->angle=0;s->peak_linear=linear;s->reversals=0;s->hold_ms=0;
            s->linear_ms=linear>.08f?s->motion_ms:0;
            memcpy(s->origin,s->gravity,sizeof s->origin);
            memcpy(s->previous_gyro,gyro,sizeof gyro);
        }
        s->stage=s->candidate?MW_MOVING:(s->pickup_armed||s->turn_armed)?MW_ARMED:MW_WAITING;
        return 0;
    }
    if(now-s->started_ms>1800) { cancel(s);s->stage=MW_WAITING;return 0; }
    s->angle+=s->speed*dt;
    if(linear>.08f)s->linear_ms+=elapsed;
    if(linear>s->peak_linear)s->peak_linear=linear;
    if(s->speed>25 && length3(s->previous_gyro)>25 && dot3(gyro,s->previous_gyro)<-200)
        s->reversals++;
    if(s->speed>25)memcpy(s->previous_gyro,gyro,sizeof gyro);
    if(s->reversals>=3 || s->angle>180) {
        cancel(s);s->stage=MW_COOLDOWN;s->cooldown_ms=now;return 0;
    }
    int sufficient=s->candidate==MOTION_WAKE_TURN ?
        (s->angle>=20 && dot3(s->origin,s->gravity)<.91f) :
        (s->angle>=12 || (s->peak_linear>.12f && s->linear_ms>=100));
    /* End in the learned reading cone (45 degrees) and hold for 160 ms.
     * This avoids waking just because the watch happens to lie face-up. */
    if(sufficient && s->view_dot>.7071f && linear<.12f && s->speed<32 && jitter<.055f)
        s->hold_ms+=elapsed;
    else s->hold_ms=0;
    if(s->hold_ms>=160) {
        int result=s->candidate;cancel(s);s->stage=MW_COOLDOWN;s->cooldown_ms=now;
        return result;
    }
    return 0;
}
