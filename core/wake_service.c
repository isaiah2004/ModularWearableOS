#include "wake_service.h"
#include "motion_wake.h"
#include "hal.h"
#include <math.h>
#include <string.h>

extern int g_setting_tilt_wake;
#define WAKE_MAGIC 0x57414b31u
typedef struct {
    uint32_t magic,version,flags,wrist,learned;
    int32_t view[2][3];
    uint32_t checksum;
} wake_blob;
static wake_blob s_cfg;
static motion_wake_state s_model;
static wake_info s_info;
static uint32_t s_learn_start,s_previous_sample,s_learn_stable;
static float s_learn_last[3],s_learn_sum[3];
static unsigned s_learn_count;
static int s_ready,s_have_sample;

static uint32_t checksum(const wake_blob *b)
{
    const unsigned char *p=(const unsigned char*)b;uint32_t hash=2166136261u;
    for(unsigned i=0;i<sizeof *b-sizeof b->checksum;i++)hash=(hash^p[i])*16777619u;
    return hash;
}
static int valid_blob(const wake_blob *b)
{
    if(b->magic!=WAKE_MAGIC || b->version!=1 || b->flags>3 || b->wrist>1 ||
       b->learned>3 || b->checksum!=checksum(b))return 0;
    for(int hand=0;hand<2;hand++) {
        int64_t norm=0;
        for(int i=0;i<3;i++) {
            if(b->view[hand][i]<-1000 || b->view[hand][i]>1000)return 0;
            norm+=(int64_t)b->view[hand][i]*b->view[hand][i];
        }
        if(norm<900000 || norm>1100000)return 0;
    }
    return 1;
}
static void configure(void)
{
    motion_wake_config cfg={.pickup=(s_cfg.flags&1)!=0,.turn=(s_cfg.flags&2)!=0};
    for(int i=0;i<3;i++)cfg.view[i]=s_cfg.view[s_cfg.wrist][i]*.001f;
    motion_wake_init(&s_model,&cfg);
    s_info.wrist=(int)s_cfg.wrist;s_info.pickup=cfg.pickup;s_info.turn=cfg.turn;
    s_info.learned=(s_cfg.learned&(1u<<s_cfg.wrist))!=0;
    g_setting_tilt_wake=s_cfg.flags!=0;s_have_sample=0;
}
void wake_service_init(int legacy_enabled)
{
    memset(&s_info,0,sizeof s_info);
    s_cfg=(wake_blob){.magic=WAKE_MAGIC,.version=1,.flags=legacy_enabled?3:0,
        .wrist=0,.view={{0,0,-1000},{0,0,-1000}}};
    wake_blob stored;
    if(hal_store_load("wake_cfg",&stored,sizeof stored) && valid_blob(&stored))s_cfg=stored;
    configure();s_ready=1;
}
static int save(wake_blob *next)
{
    next->checksum=checksum(next);
    if(!valid_blob(next) || !hal_store_save_checked("wake_cfg",next,sizeof *next)) {
        s_info.learn_result=-2;return 0;
    }
    s_cfg=*next;configure();return 1;
}
int wake_service_set(const char *option,int value)
{
    if(!s_ready)wake_service_init(1);
    if(value<0 || value>1)return 0;
    wake_blob next=s_cfg;
    if(!strcmp(option,"wrist"))next.wrist=(uint32_t)value;
    else if(!strcmp(option,"pickup"))next.flags=(next.flags&~1u)|(unsigned)value;
    else if(!strcmp(option,"turn"))next.flags=(next.flags&~2u)|((unsigned)value<<1);
    else return 0;
    if(!save(&next))return 0;
    s_info.learning=0;s_info.learn_result=0;return 1;
}
void wake_service_learn(void)
{
    if(!s_ready)wake_service_init(1);
    s_info.learning=1;s_info.learn_result=0;s_info.learn_ms=0;
    s_learn_start=hal_millis();s_learn_stable=s_learn_count=0;
    memset(s_learn_sum,0,sizeof s_learn_sum);
}
void wake_service_get(wake_info *out)
{ if(!s_ready)wake_service_init(1);*out=s_info; }
int wake_service_tick(uint32_t now,int eligible)
{
    if(!s_ready)wake_service_init(1);
    hal_imu m={0};uint32_t sample_ms=now;
    int valid=hal_imu_sample(&m,&sample_ms);s_info.valid=valid;
    if(s_info.learning && now-s_learn_start>10000) {
        s_info.learning=0;s_info.learn_result=-1;
    }
    if(valid && s_have_sample && sample_ms==s_previous_sample)return 0;
    uint32_t dt=s_have_sample?sample_ms-s_previous_sample:0;
    s_previous_sample=sample_ms;s_have_sample=valid;
    int result=motion_wake_update(&s_model,&m,sample_ms,valid,eligible && !s_info.learning);
    s_info.stage=s_model.stage;s_info.magnitude=s_model.magnitude;
    s_info.speed=s_model.speed;s_info.view_dot=s_model.view_dot;
    memcpy(s_info.bias,s_model.bias,sizeof s_info.bias);
    if(result){s_info.last_reason=result;s_info.wakes++;}
    if(s_info.learning) {
        float a[3]={m.ax,m.ay,m.az};
        float norm=sqrtf(m.ax*m.ax+m.ay*m.ay+m.az*m.az);
        float speed=sqrtf(m.gx*m.gx+m.gy*m.gy+m.gz*m.gz),delta=0;
        for(int i=0;i<3;i++){float d=a[i]-s_learn_last[i];delta+=d*d;s_learn_last[i]=a[i];}
        if(valid && dt>0 && dt<=150 && isfinite(norm) && norm>.9f && norm<1.1f &&
           isfinite(speed) && speed<12 && delta<.0016f) {
            s_learn_stable+=dt;s_learn_count++;
            for(int i=0;i<3;i++)s_learn_sum[i]+=a[i]/norm;
            if(s_learn_stable>=800 && s_learn_count>=12) {
                float length=sqrtf(s_learn_sum[0]*s_learn_sum[0]+s_learn_sum[1]*s_learn_sum[1]+s_learn_sum[2]*s_learn_sum[2]);
                wake_blob next=s_cfg;
                for(int i=0;i<3;i++)next.view[next.wrist][i]=(int32_t)lroundf(s_learn_sum[i]/length*1000);
                next.learned|=1u<<next.wrist;
                s_info.learning=0;s_info.learn_result=save(&next)?1:-2;
            }
        } else {s_learn_stable=s_learn_count=0;memset(s_learn_sum,0,sizeof s_learn_sum);}
        s_info.learn_ms=s_learn_stable;
    }
    return result;
}
