/* Pip's behavior model. No drawing, allocations, IO, or platform clock reads.
 * Times are elapsed unsigned milliseconds, including across clock wrap. */
#include "pip.h"
#include <math.h>
#include <string.h>
#define PI 3.14159265f
#define MEMORY_MAGIC 0x50495031u
static float limit(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static float follow(float x, float target, float rate, float dt)
{ return x + (target - x) * (1.0f - expf(-rate * dt)); }
static float smooth(float x)
{ x=limit(x,0,1);return x*x*(3-2*x); }
static float random01(pip_model *m)
{
    uint32_t x = m->memory.seed;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    m->memory.seed = x ? x : 0x19a781u;
    return (m->memory.seed & 65535u) / 65535.0f;
}
static const char *const names[] = {"idle","curious","happy","petted","boop",
    "startled","dizzy","rocked","sleepy","asleep","wake","play","bored","stretch","wink","peek",
    "grumpy","lonely","excited","relieved","notify"};
static const uint32_t durations[] = {7000,2600,2200,2800,850,1000,3600,3200,4200,60000,2200,8500,3000,2600,1400,2700,
    3800,5200,2800,3000,1650};
const char *pip_action_name(pip_action a) { return a >= 0 && a < PIP_ACTION_COUNT ? names[a] : "unknown"; }
int pip_memory_valid(const pip_memory *m)
{ return m && m->magic == MEMORY_MAGIC && m->version == 1 && m->familiarity <= 1000 && m->visits <= 10000000 && m->seed; }
static void enter(pip_model *m, pip_action a, uint32_t now)
{
    m->previous = m->action; m->action = a; m->action_ms = now;
    m->duration_ms = durations[a];
    m->gesture_side=random01(m)<.5f ? -1 : 1;
    if(a==PIP_BOOP || a==PIP_PETTED)m->gesture_side=m->touch_x<120 ? -1 : 1;
    if (a == PIP_PLAY) { m->ball_x = 0; m->ball_y = -46; m->ball_vx = 26; m->ball_vy = -12; }
}
static void sustain(pip_model *m, pip_action action, uint32_t now)
{
    if(m->action!=action) enter(m,action,now);
    else m->duration_ms=now-m->action_ms+2200; /* retain animation phase */
}
static void attention(pip_model *m, uint32_t now)
{ m->last_attention = now; m->wake_ms = now; m->woke = 1; }
static void reward(pip_model *m, uint32_t now)
{
    if (now - m->reward_ms < 10000u) return;
    m->reward_ms = now;
    if (m->memory.familiarity < 1000) m->memory.familiarity++;
    m->dirty = 1;
}
void pip_init(pip_model *m, const pip_memory *memory, uint32_t now)
{
    pip_memory saved = {MEMORY_MAGIC,1,80,0,0x19a781u};
    if (pip_memory_valid(memory)) saved = *memory;
    memset(m, 0, sizeof *m); m->memory = saved;
    if (m->memory.visits < 10000000) m->memory.visits++;
    m->dirty = 1; m->initialized = 1; m->last_ms = now;
    m->last_attention = m->reward_ms = m->motion_ms = m->last_motion_ms = now;
    m->startle_ms=now;
    m->blink_ms = m->gaze_ms = now; m->blink_wait = 2500; m->gaze_wait = 1600;
    m->comfort = .65f; m->energy = .8f; m->curiosity = .7f;
    m->context_energy=.8f;m->context_social=m->social=.65f;m->context_rest=m->rest=.8f;
    m->last_idle_action=PIP_IDLE;
    m->quiet_override=-1;
    m->pose.width = 26; m->pose.height = 33; m->pose.glow = .85f;
    enter(m, PIP_WAKE, now);
}
void pip_set_context(pip_model *m, float energy, float valence,
                     float social, float rest, float grumpy)
{
    if(isfinite(energy))m->context_energy=limit(energy,0,1);
    if(isfinite(valence))m->context_valence=limit(valence,-1,1);
    if(isfinite(social))m->context_social=limit(social,0,1);
    if(isfinite(rest))m->context_rest=limit(rest,0,1);
    if(isfinite(grumpy))m->context_grumpy=limit(grumpy,0,1);
    m->context_active=1;
}
void pip_set_quiet(pip_model *m,int quiet)
{
    int next=quiet<0 ? -1 : quiet!=0;
    if(m->quiet_override==1 && next!=1)m->routine_wake=1;
    m->quiet_override=next;
}
void pip_center(pip_model *m)
{
    m->neutral_x = m->gravity_x; m->neutral_y = m->gravity_y;
}
void pip_pointer(pip_model *m, int phase, float x, float y, uint32_t now)
{
    x = limit(x,0,239.9f); y = limit(y,0,239.9f);
    if (phase == 1) {
        m->touch_ms = now; m->path = 0; m->stroke = 0;
        m->touch_zone = y > 64 && y < 174 && x > 28 && x < 212;
        attention(m, now);
        if (m->action == PIP_ASLEEP || m->action == PIP_SLEEPY) enter(m,PIP_WAKE,now);
    } else if (phase == 2 && m->down) {
        float dx=x-m->touch_x, dy=y-m->touch_y;
        m->path += sqrtf(dx*dx+dy*dy);
        if (m->touch_zone && m->path > 24 && now-m->touch_ms > 180) {
            m->stroke = 1;
            if (now-m->last_stroke_ms > 300) {
                m->last_stroke_ms = now; sustain(m,PIP_PETTED,now);
                m->comfort = limit(m->comfort+.07f,0,1); reward(m,now);
            }
        }
    }
    m->touch_x=x; m->touch_y=y; m->down = phase != 0;
    if (!phase && m->stroke) { sustain(m,PIP_PETTED,now); attention(m,now); }
}
void pip_tap(pip_model *m, uint32_t now)
{
    attention(m,now); reward(m,now);
    if (m->had_tap && now-m->tap_ms < 480) { enter(m,PIP_PLAY,now); m->had_tap=0; }
    else {
        m->had_tap=1; m->tap_ms=now;
        enter(m, (m->action == PIP_ASLEEP || m->action == PIP_SLEEPY) ? PIP_WAKE : PIP_BOOP, now);
    }
    m->arousal=limit(m->arousal+.12f,0,1);
}
void pip_hold(pip_model *m, uint32_t now)
{
    attention(m,now); reward(m,now); m->comfort=limit(m->comfort+.15f,0,1);
    sustain(m,PIP_PETTED,now);
}
int pip_force(pip_model *m, const char *name, uint32_t now)
{
    for (int a=0; a<PIP_ACTION_COUNT; a++) if (!strcmp(name,names[a])) {
        attention(m,now); enter(m,(pip_action)a,now); return 1;
    }
    return 0;
}
static int finite_imu(const hal_imu *i)
{ return isfinite(i->ax)&&isfinite(i->ay)&&isfinite(i->az)&&isfinite(i->gx)&&isfinite(i->gy)&&isfinite(i->gz); }
static void senses(pip_model *m, const pip_input *in, float dt)
{
    uint32_t now=in->now;
    m->imu_valid = in->imu_valid && finite_imu(&in->imu);
    if (!m->imu_valid) {
        m->imu_ready=0; m->motion=m->shaking=m->rocking=m->spin=0;
        m->face_down=0; return;
    }
    const hal_imu *i=&in->imu;
    if (!m->imu_ready) {
        m->gravity_x=i->ax; m->gravity_y=i->ay; m->gravity_z=i->az;
        m->bias_x=m->bias_y=m->bias_z=0;
        pip_center(m); m->imu_ready=1; m->still_time=0;
    }
    float gx=i->gx-m->bias_x, gy=i->gy-m->bias_y, gz=i->gz-m->bias_z;
    float speed=sqrtf(gx*gx+gy*gy+gz*gz);
    float dx=i->ax-m->gravity_x, dy=i->ay-m->gravity_y, dz=i->az-m->gravity_z;
    float dynamic=sqrtf(dx*dx+dy*dy+dz*dz);
    m->gravity_x=follow(m->gravity_x,i->ax,3,dt);
    m->gravity_y=follow(m->gravity_y,i->ay,3,dt);
    m->gravity_z=follow(m->gravity_z,i->az,3,dt);
    float mag=sqrtf(i->ax*i->ax+i->ay*i->ay+i->az*i->az);
    /* Learn small stationary gyro bias slowly; never integrate it into yaw. */
    if (dynamic < .035f && speed < 9 && mag > .85f && mag < 1.15f) {
        m->still_time += dt;
        if (m->still_time > 1) {
            m->bias_x=follow(m->bias_x,i->gx,.8f,dt);
            m->bias_y=follow(m->bias_y,i->gy,.8f,dt);
            m->bias_z=follow(m->bias_z,i->gz,.8f,dt);
        }
    } else m->still_time=0;
    m->motion=follow(m->motion,limit(dynamic*1.6f+speed/260,0,1),5,dt);
    m->shaking=limit(m->shaking+dt*(dynamic>.6f ? 2.7f : -1.0f),0,1);
    m->spin=limit(m->spin+dt*(speed>160 ? 2 : -1.2f),0,1);
    float rock_axis=fabsf(gx)>fabsf(gy) ? gx : gy;
    int sign=rock_axis>12 ? 1 : rock_axis< -12 ? -1 : 0;
    if(now-m->rock_ms>1800) m->rock_reversals=0;
    if(sign && sign!=m->rock_sign && now-m->rock_ms>180) {
        if(m->rock_sign && m->rock_reversals<3) m->rock_reversals++;
        m->rock_sign=sign;m->rock_ms=now;
    }
    int gentle=m->rock_reversals>=2 && speed>12 && speed<75 && dynamic<.28f;
    m->rocking=limit(m->rocking+dt*(gentle ? .8f : -.8f),0,1);
    if (m->gravity_z > .72f && m->motion<.15f && speed<12) {
        if (!m->face_down) { m->face_down=1; m->face_down_ms=now; }
    } else m->face_down=0;
    int moving=dynamic>.16f || speed>25;
    if (moving) {
        if ((m->action==PIP_ASLEEP || m->action==PIP_SLEEPY) && !m->face_down) {
            attention(m,now); enter(m,PIP_WAKE,now);
        }
        m->last_motion_ms=now;
    }
    if (now-m->motion_ms > 4500 && m->action!=PIP_DIZZY) {
        if (m->shaking>.75f || m->spin>.85f) {
            enter(m,PIP_DIZZY,now); m->motion_ms=now; attention(m,now);
            m->shaking=m->spin=0; m->arousal=.85f;
        } else if (dynamic>1.1f && m->action!=PIP_STARTLED && now-m->startle_ms>4500) {
            enter(m,PIP_STARTLED,now); m->arousal=.9f;
            m->startle_ms=now;
            /* Allow sustained motion to graduate from surprise to dizziness. */
        } else if (m->rocking>.8f && m->action!=PIP_ROCKED && m->action!=PIP_PETTED && !m->down) {
            enter(m,PIP_ROCKED,now); m->motion_ms=now; m->comfort=limit(m->comfort+.1f,0,1);
        }
    }
}
static pip_action resting_action(const pip_model *m)
{
    if(m->context_active) {
        if(m->grumpy>.45f)return PIP_GRUMPY;
        if(m->social<.25f || m->valence<-.5f)return PIP_LONELY;
    }
    return PIP_IDLE;
}
static void recover(pip_model *m,uint32_t now)
{
    enter(m,resting_action(m),now);
    if(m->action==PIP_IDLE)m->duration_ms=4500+(uint32_t)(random01(m)*6500);
}
static void behavior(pip_model *m, const pip_input *in, float dt)
{
    uint32_t now=in->now, age=now-m->action_ms;
    int night=m->quiet_override>=0 ? m->quiet_override : (in->hour>=22 || in->hour<7);
    if(m->routine_wake) {
        if(!night && (m->action==PIP_ASLEEP || m->action==PIP_SLEEPY)) {
            attention(m,now);enter(m,PIP_WAKE,now);age=0;
        }
        m->routine_wake=0;
    }
    int recently_woken=m->woke && now-m->wake_ms<45000;
    if(m->context_active) {
        m->energy=follow(m->energy,m->context_energy,.45f,dt);
        m->valence=follow(m->valence,m->context_valence,.7f,dt);
        m->social=follow(m->social,m->context_social,.6f,dt);
        m->rest=follow(m->rest,m->context_rest,.6f,dt);
        m->grumpy=follow(m->grumpy,m->context_grumpy,.7f,dt);
    } else {
        m->energy=follow(m->energy,(night ? .38f : .83f)-(in->battery<15 ? .12f : 0),.15f,dt);
    }
    m->comfort=follow(m->comfort,m->context_active ? m->social : .55f,.006f,dt);
    m->arousal=follow(m->arousal,limit(m->motion*.6f+fmaxf(0,m->valence)*.13f+m->grumpy*.12f,0,1),.8f,dt);
    float seek=now-m->last_attention>18000 ? .9f : .4f;
    if(m->context_active)seek=limit(seek+(1-m->social)*.18f-m->grumpy*.25f,0,1);
    m->curiosity=follow(m->curiosity,seek,.1f,dt);
    if (m->down) { m->wake_ms=now; m->last_attention=now; }
    int want_sleep=!m->down && ((m->face_down && now-m->face_down_ms>2500) ||
        (night && !recently_woken && now-m->last_attention>18000));
    if (want_sleep && m->action!=PIP_SLEEPY && m->action!=PIP_ASLEEP &&
        m->action!=PIP_STARTLED && m->action!=PIP_DIZZY && age>1200) enter(m,PIP_SLEEPY,now);
    age=now-m->action_ms;
    if (age<m->duration_ms) return;
    switch (m->action) {
    case PIP_SLEEPY: if(want_sleep)enter(m,PIP_ASLEEP,now);else recover(m,now);break;
    case PIP_ASLEEP: enter(m,want_sleep ? PIP_ASLEEP : PIP_WAKE,now); break;
    case PIP_STARTLED: enter(m,PIP_CURIOUS,now); break;
    case PIP_DIZZY: enter(m,PIP_STRETCH,now); break;
    case PIP_BOOP: enter(m,PIP_HAPPY,now); break;
    case PIP_WAKE: enter(m,PIP_STRETCH,now); break;
    case PIP_PETTED: case PIP_ROCKED: enter(m,PIP_HAPPY,now); break;
    case PIP_RELIEVED:case PIP_EXCITED:
        enter(m,PIP_HAPPY,now);m->duration_ms=1400;break;
    case PIP_GRUMPY:case PIP_LONELY:
        enter(m,PIP_IDLE,now);m->duration_ms=6000+(uint32_t)(random01(m)*4000);break;
    case PIP_IDLE: {
        static const pip_action choices[]={PIP_CURIOUS,PIP_WINK,PIP_PEEK,PIP_STRETCH,PIP_PLAY,PIP_BORED};
        unsigned pick=(unsigned)(random01(m)*6); if(pick>5)pick=5;
        pip_action a=choices[pick];
        if(m->context_active) {
            if(m->grumpy>.55f)a=PIP_GRUMPY;
            else if(m->social<.25f || m->valence<-.5f)a=PIP_LONELY;
            else if(m->valence>.6f && m->energy>.6f && random01(m)<.3f)a=PIP_EXCITED;
        }
        if (a==m->last_idle_action) a=a==PIP_CURIOUS ? PIP_PEEK : PIP_CURIOUS;
        if (m->curiosity>.8f && a==PIP_BORED) a=PIP_CURIOUS;
        if (a==PIP_PLAY && m->memory.familiarity<120) a=PIP_PEEK;
        if (m->energy<.5f && a==PIP_PLAY) a=PIP_SLEEPY;
        m->last_idle_action=a;enter(m,a,now); break;
    }
    default: recover(m,now);break;
    }
}
static void animate(pip_model *m, float dt, uint32_t now)
{
    float age=(now-m->action_ms)/1000.0f;
    m->breath_phase=fmodf(m->breath_phase+dt*(m->action==PIP_ASLEEP ? .9f : 1.9f),2*PI);
    pip_pose p={0};p.wink_side=m->gesture_side;
    p.width=26; p.height=33+sinf(m->breath_phase)*1.2f+m->arousal*2;
    p.glow=.72f+m->energy*.20f+m->arousal*.04f;
    if(m->context_active) {
        float tired=1-m->energy*(.55f+.45f*m->rest);
        p.upper=tired*.18f+m->grumpy*.12f;p.y=tired*2;
        p.height-=tired*3;p.anger=m->grumpy*.6f;
        p.joy=fmaxf(0,m->valence)*.15f;
        p.lower=fmaxf(0,m->valence)*.08f;
    }
    if (now-m->gaze_ms>=m->gaze_wait) {
        m->gaze_ms=now; m->gaze_wait=1000+(uint32_t)(random01(m)*2200+(1-m->curiosity)*1300);
        m->gaze_x=(random01(m)-.5f)*(7+m->curiosity*8); m->gaze_y=(random01(m)-.5f)*5;
    }
    p.x=m->gaze_x;p.y+=m->gaze_y;
    if(m->imu_valid) {
        p.x+=limit(-(m->gravity_y-m->neutral_y)*24,-12,12);
        p.y+=limit((m->gravity_x-m->neutral_x)*22,-9,9);
        p.lean=limit(-(m->gravity_y-m->neutral_y)*5,-4,4);
    }
    if(m->down) { p.x=limit((m->touch_x-120)*.16f,-14,14); p.y=limit((m->touch_y-128)*.14f,-10,10); }
    switch(m->action) {
    case PIP_CURIOUS: p.wink=.18f;p.asymmetry=p.wink_side*.8f;p.y-=4;p.width-=2;break;
    case PIP_HAPPY: p.upper=0;p.anger=0;p.lower=.48f;p.joy=.8f;p.y-=fabsf(sinf(age*4))*3;break;
    case PIP_PETTED: p.upper=0;p.anger=0;p.lower=.64f+m->comfort*.12f;p.joy=1;p.x+=sinf(age*3)*2;p.y+=3;break;
    case PIP_BOOP: {
        float kick=expf(-age*6)*sinf(age*12)*(1-m->memory.familiarity/2500.0f);
        p.width-=kick*8; p.height+=kick*6; p.y+=kick*5; p.wink=.45f*expf(-age*4); break;
    }
    case PIP_STARTLED:
        p.anger=0;p.joy=0;p.lower=0;
        if(age<.07f){p.upper=.8f;p.height-=5;}
        else {p.width+=6*expf(-(age-.07f)*1.5f);p.height+=7;p.surprise=1;p.y-=7*expf(-(age-.07f)*3);}
        break;
    case PIP_DIZZY: p.anger=0;p.x=sinf(age*7)*9;p.y=cosf(age*7)*4;p.dizzy=1;p.upper=.26f;break;
    case PIP_ROCKED: p.anger=0;p.upper=0;p.x+=sinf(age*2.8f)*7;p.lower=.5f;p.joy=.6f;p.lean+=sinf(age*2.8f)*3;break;
    case PIP_SLEEPY: p.upper=limit(age/3,0,.82f); p.y+=5; p.sleep=.4f; break;
    case PIP_ASLEEP: p.anger=0;p.upper=p.lower=p.joy=0;p.height=3+sinf(m->breath_phase)*.6f;p.y=9;p.x=0;p.glow=.42f;p.sleep=1;break;
    case PIP_WAKE:
        p.upper=limit(1-age/.65f,0,1);p.wink=limit(1-(age-.25f)/.65f,0,1)*.6f;
        p.height+=sinf(limit(age/2.2f,0,1)*PI)*8;p.y-=3;break;
    case PIP_PLAY: {
        float tx=m->imu_valid ? -(m->gravity_y-m->neutral_y) : 0;
        m->ball_vx+=tx*130*dt; m->ball_vy+=24*dt;
        m->ball_x+=m->ball_vx*dt; m->ball_y+=m->ball_vy*dt;
        if(m->ball_x>68){m->ball_x=68;m->ball_vx=-fabsf(m->ball_vx)*.9f;}
        if(m->ball_x< -68){m->ball_x=-68;m->ball_vx=fabsf(m->ball_vx)*.9f;}
        if(m->ball_y> -32){m->ball_y=-32;m->ball_vy=-38;}
        if(m->ball_y< -63){m->ball_y=-63;m->ball_vy=fabsf(m->ball_vy);}
        m->ball_vx=limit(m->ball_vx,-65,65);
        p.ball_visible=1;p.ball_x=m->ball_x;p.ball_y=m->ball_y;
        p.x=m->ball_x*.13f;p.y=-3;p.lower=.14f;break;
    }
    case PIP_BORED: p.upper=.38f;p.y+=5;p.x=sinf(age*.8f)*9;break;
    case PIP_STRETCH: { float a=sinf(limit(age/2.6f,0,1)*PI);p.height+=a*9;p.width-=a*5;p.y-=a*4;break; }
    case PIP_WINK: p.wink=sinf(limit(age/1.4f,0,1)*PI)*.95f;p.lower=.2f;p.joy=.5f;break;
    case PIP_PEEK: p.x=sinf(age*1.4f)*13;p.width=21;p.wink=.2f;p.y+=3;break;
    case PIP_GRUMPY: {
        float side=(float)m->gesture_side;
        float away=smooth((age-.15f)/.4f),check=smooth((age-1.3f)/.55f);
        p.x=side*(14*away-11*check);p.y=3;p.width-=2;
        p.upper=.12f;p.lower=0;p.joy=0;
        p.anger=.88f-.23f*check;p.asymmetry=side*(.7f-.4f*check);
        p.wink=.15f*check;p.lean=-side*(1.5f-check);
        break;
    }
    case PIP_LONELY: {
        float side=(float)m->gesture_side;
        float across=smooth((age-1)/1.0f),back=smooth((age-3.2f)/.9f);
        p.x=side*(8-16*across)*(1-back);
        p.y=4-6*sinf(limit(age/5.2f,0,1)*PI);
        p.upper=.18f;p.lower=0;p.anger=0;p.joy=0;
        p.width-=2;p.height-=3;p.asymmetry=side*.45f;p.glow*=.91f;
        break;
    }
    case PIP_EXCITED: {
        float start=smooth(age/.2f),bounce=fabsf(sinf(fmaxf(0,age-.18f)*6))*expf(-age*.55f);
        p.upper=0;p.anger=0;p.joy=1;p.lower=.48f*start;
        p.width+=3*(1-start);p.height+=5*bounce-4*(1-start);
        p.y-=8*bounce;p.asymmetry=m->gesture_side*.5f*sinf(age*4);
        p.glow=fminf(1,p.glow+.06f);break;
    }
    case PIP_RELIEVED: {
        float release=smooth((age-.2f)/.85f);
        p.anger=0;p.upper=.25f*(1-release);p.lower=.47f*release;
        p.joy=.75f*release;p.width-=2*(1-release);
        p.height+=3*sinf(release*PI);p.y=3*release;
        p.x*=1-release*.8f;p.lean*=1-release;
        break;
    }
    case PIP_NOTIFY: {
        float notice=smooth(age/.18f),settle=smooth((age-1.05f)/.6f);
        float focus=notice*(1-settle);
        p.anger=0;p.upper=p.lower=0;p.joy=0;
        p.x=m->gesture_side*2*focus;p.y=-11*focus;
        p.width+=2*focus;p.height+=3*focus;
        p.asymmetry=m->gesture_side*.6f*focus;
        p.surprise=.4f*focus*(1-smooth((age-.2f)/.45f));
        break;
    }
    default:break;
    }
    /* Touch has gaze priority over idle play; reflex startle/dizzy stays readable. */
    if(m->down && m->action!=PIP_ASLEEP && m->action!=PIP_STARTLED && m->action!=PIP_DIZZY) {
        p.x=limit((m->touch_x-120)*.16f,-14,14);p.y+=limit((m->touch_y-128)*.12f,-8,8);
    }
    uint32_t blink_duration=170+(uint32_t)((1-m->energy)*40);
    if(now-m->blink_ms>=m->blink_wait+blink_duration) {
        m->blink_ms=now; m->blink_wait=2100+(uint32_t)(random01(m)*3600+(1-m->energy)*1000);
    }
    uint32_t blink=now-m->blink_ms;
    if(blink>m->blink_wait && m->action!=PIP_ASLEEP && m->action!=PIP_STARTLED) {
        float b=(float)(blink-m->blink_wait);
        if(b<45) p.blink=b/45;
        else if(b<70) p.blink=1;
        else {float opening=limit(1-(b-70)/(blink_duration-70),0,1);p.blink=opening*opening;}
    }
    float rate=m->action==PIP_STARTLED ? 22 : 11;
    /* Every animated property is updated once per step, with time-based easing. */
    float pose_alpha=1.0f-expf(-rate*dt);
#define EASE(field) m->pose.field+=(p.field-m->pose.field)*pose_alpha
    m->pose.x=follow(m->pose.x,p.x,20+m->arousal*12,dt);
    m->pose.y=follow(m->pose.y,p.y,18+m->arousal*10,dt);
    EASE(width);EASE(height);EASE(upper);EASE(lower);EASE(wink);EASE(lean);EASE(glow);EASE(asymmetry);
    EASE(joy);EASE(surprise);EASE(dizzy);EASE(sleep);EASE(ball_visible);EASE(anger);
#undef EASE
    m->pose.ball_x=p.ball_x;m->pose.ball_y=p.ball_y;
    m->pose.blink=p.blink;m->pose.wink_side=p.wink_side;
}
void pip_step(pip_model *m, const pip_input *in)
{
    if(!m->initialized) pip_init(m,NULL,in->now);
    uint32_t elapsed=in->now-m->last_ms;
    if(!elapsed) return;
    m->last_ms=in->now;
    /* Re-entering the face must not interpret a long pause as a violent motion. */
    if(elapsed>2500) {
        m->imu_ready=0;m->shaking=m->spin=m->rocking=0;m->rock_reversals=0;
        if(in->now-m->last_attention>2500){attention(m,in->now);enter(m,PIP_WAKE,in->now);}
        if(in->now-m->touch_ms>2500)m->down=0;
    }
    float dt=limit(elapsed/1000.0f,.001f,.08f);
    senses(m,in,dt); behavior(m,in,dt); animate(m,dt,in->now);
}
