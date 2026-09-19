#include "pip.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
static pip_input sample(uint32_t now)
{
    pip_input i={.now=now,.hour=14,.battery=80,.imu_valid=1,.imu={0,0,-1,3.5f,1.5f,-.5f}};
    return i;
}
static void run(pip_model *m,pip_input *i,unsigned ms,unsigned step)
{
    for(unsigned t=0;t<ms;t+=step){i->now+=step;pip_step(m,i);}
}
static void finite_pose(const pip_model *m)
{
    assert(isfinite(m->pose.x)&&isfinite(m->pose.y));
    assert(isfinite(m->pose.width)&&isfinite(m->pose.height));
    assert(m->pose.width>0&&m->pose.width<40&&m->pose.height>0&&m->pose.height<50);
    assert(m->pose.upper>=0&&m->pose.upper<=1.001f);
    assert(m->pose.glow>=0&&m->pose.glow<=1.001f);
    assert(isfinite(m->pose.blink)&&m->pose.blink>=0&&m->pose.blink<=1.001f);
    assert(isfinite(m->pose.anger)&&m->pose.anger>=0&&m->pose.anger<=1.001f);
}
static void resume_regressions(void)
{
    pip_model m;
    pip_input i=sample(1000);i.hour=23;i.imu_valid=0;
    pip_init(&m,NULL,i.now);
    run(&m,&i,27000,20);assert(m.action==PIP_ASLEEP);
    /* Opening the face after time elsewhere is attention, even at night. */
    i.now+=60000;pip_step(&m,&i);
    assert(m.action==PIP_WAKE);
    assert(m.last_attention==i.now);
    run(&m,&i,6000,20);assert(m.action!=PIP_ASLEEP);

    /* The main loop polls touch before drawing the first resumed frame. */
    i.now+=60000;
    pip_pointer(&m,1,170,130,i.now);
    i.now+=20;pip_step(&m,&i);
    assert(m.down&&m.touch_x==170&&m.touch_y==130);
    assert(m.action!=PIP_SLEEPY&&m.action!=PIP_ASLEEP);
    pip_pointer(&m,0,170,130,i.now+20);assert(!m.down);
}
static void startle_cooldown_regression(void)
{
    pip_model m;pip_input i=sample(1000);pip_init(&m,NULL,i.now);
    run(&m,&i,6000,20);
    i.imu.ax=1.6f;i.now+=20;pip_step(&m,&i);
    assert(m.action==PIP_STARTLED);
    uint32_t first=i.now;
    i.imu.ax=0;run(&m,&i,1380,20);
    assert(m.action!=PIP_STARTLED);
    i.imu.ax=1.6f;i.now+=20;pip_step(&m,&i);
    assert(i.now-first==1400);
    assert(m.action!=PIP_STARTLED); /* recovery cannot be restarted by this bump */
    i.imu.ax=0;run(&m,&i,3200,20);
    i.imu.ax=1.6f;i.now+=20;pip_step(&m,&i);
    assert(i.now-first>4500&&m.action==PIP_STARTLED);
}
static void face_down_priority_regressions(void)
{
    pip_model m;pip_input i=sample(1000);i.imu.az=1;i.imu.gx=50;
    pip_init(&m,NULL,i.now);
    /* Inverted orientation alone must not suggest rest during movement. */
    for(int n=0;n<750;n++) {
        i.now+=20;pip_step(&m,&i);
        assert(m.action!=PIP_SLEEPY&&m.action!=PIP_ASLEEP);
    }
    i.imu.gx=3.5f;run(&m,&i,12000,20);
    assert(m.action==PIP_ASLEEP); /* the same orientation is restful once still */
    pip_force(&m,"dizzy",i.now);
    run(&m,&i,1500,20);
    assert(m.action==PIP_DIZZY); /* finish the recovery before considering sleep */
    pip_force(&m,"startled",i.now);
    run(&m,&i,600,20);assert(m.action==PIP_STARTLED);
}
static void sustained_stroke_regression(void)
{
    pip_model m;pip_input i=sample(1000);i.imu_valid=0;pip_init(&m,NULL,i.now);
    run(&m,&i,6000,20);pip_pointer(&m,1,120,125,i.now);
    run(&m,&i,350,10);pip_pointer(&m,2,150,125,i.now);
    assert(m.stroke&&m.action==PIP_PETTED);
    uint32_t started=m.action_ms;
    for(int n=0;n<12;n++) {
        run(&m,&i,350,10);
        pip_pointer(&m,2,(n&1)?150:120,125,i.now);
        assert(m.action==PIP_PETTED);
        assert(m.action_ms==started); /* ongoing care extends, not restarts, animation */
    }
    pip_pointer(&m,0,150,125,i.now);assert(!m.down);
    run(&m,&i,1000,20);assert(m.action==PIP_PETTED);
    run(&m,&i,3000,20);assert(m.action!=PIP_PETTED);
}
static void low_rate_blink_regression(void)
{
    pip_model m;pip_input i=sample(1000);i.imu_valid=0;pip_init(&m,NULL,i.now);
    pip_force(&m,"idle",i.now);
    float closure=0;
    for(int n=0;n<80;n++) {
        i.now+=50;pip_step(&m,&i); /* 20 fps under render load */
        float lids=fmaxf(m.pose.upper,m.pose.blink)+fmaxf(m.pose.lower,m.pose.blink*.35f);
        if(lids>closure)closure=lids;
    }
    assert(closure>.9f); /* the blink closes visibly even at this render rate */
    /* The blink has fully reopened by the end of the trace. */
    assert(fmaxf(m.pose.upper,m.pose.blink)+fmaxf(m.pose.lower,m.pose.blink*.35f)<.1f);
}
static void care_context_regressions(void)
{
    pip_model m;pip_input i=sample(1000);i.imu_valid=0;
    pip_init(&m,NULL,i.now);pip_force(&m,"idle",i.now);
    uint32_t started=m.action_ms,seed=m.memory.seed;
    pip_set_context(&m,.2f,-.8f,.1f,.4f,.9f);
    assert(m.action==PIP_IDLE&&m.action_ms==started&&m.memory.seed==seed);
    run(&m,&i,16000,20);
    assert(fabsf(m.energy-.2f)<.002f&&m.grumpy>.89f&&m.social<.11f);
    /* Context energy remains authoritative across local-time/battery changes. */
    i.hour=23;i.battery=0;run(&m,&i,12000,20);
    i.hour=14;i.battery=100;run(&m,&i,12000,20);
    assert(fabsf(m.energy-.2f)<.001f);
    pip_set_context(&m,NAN,INFINITY,NAN,NAN,NAN);
    assert(m.context_energy==.2f&&m.context_valence==-.8f&&m.context_grumpy==.9f);
    pip_set_context(&m,3,-4,2,-1,2);
    assert(m.context_energy==1&&m.context_valence==-1&&m.context_social==1);
    assert(m.context_rest==0&&m.context_grumpy==1);
    finite_pose(&m);
}
static void grumpy_and_care_recovery(void)
{
    pip_model m;pip_input i=sample(1000);i.imu_valid=0;
    pip_init(&m,NULL,i.now);pip_set_context(&m,.8f,-.7f,.1f,.8f,.9f);
    pip_force(&m,"grumpy",i.now);
    run(&m,&i,800,20);
    assert(m.pose.anger>.8f&&fabsf(m.pose.x)>10&&m.pose.joy<.01f);
    run(&m,&i,1300,20);
    assert(m.pose.anger>.6f&&fabsf(m.pose.x)<4.5f); /* checks back after looking away */
    pip_hold(&m,i.now);uint32_t started=m.action_ms;
    for(int n=0;n<100;n++) {
        pip_set_context(&m,.8f,-.7f,.1f,.8f,.9f);
        i.now+=20;pip_step(&m,&i);
        assert(m.action_ms==started&&m.action==PIP_PETTED);
    }
    assert(m.pose.joy>.95f&&m.pose.anger<.01f);
    assert(m.context_grumpy==.9f&&m.grumpy>.8f); /* care overlay does not erase cause */
    run(&m,&i,3800,20);
    assert(m.action==PIP_GRUMPY&&m.pose.anger>.7f&&m.pose.joy<.02f);
    pip_set_context(&m,.8f,.6f,.8f,.8f,0);pip_force(&m,"relieved",i.now);
    run(&m,&i,1200,20);assert(m.pose.anger<.01f&&m.pose.joy>.6f);
    run(&m,&i,5500,20);assert(m.grumpy<.01f&&m.pose.anger<.03f);
}
static void contextual_performance_regressions(void)
{
    pip_model m;pip_input i=sample(1000);i.imu_valid=0;
    pip_init(&m,NULL,i.now);pip_set_context(&m,.75f,-.7f,.1f,.7f,.8f);
    run(&m,&i,6000,20);
    pip_force(&m,"notify",i.now);run(&m,&i,400,20);
    assert(m.pose.y< -9&&m.pose.anger<.02f); /* attends toward notification shade */
    run(&m,&i,1800,20);assert(m.action==PIP_GRUMPY&&m.pose.anger>.6f);
    pip_force(&m,"lonely",i.now);run(&m,&i,500,20);float first=m.pose.x;
    run(&m,&i,1800,20);assert(first*m.pose.x<0&&m.pose.anger<.01f);
    pip_force(&m,"excited",i.now);run(&m,&i,500,20);
    assert(m.pose.joy>.9f&&m.pose.anger<.01f&&m.pose.lower>.4f);
    /* The new mood layer must never filter away the fully-closed blink. */
    pip_force(&m,"grumpy",i.now);m.blink_ms=i.now;m.blink_wait=500;
    float blink=0;
    for(int n=0;n<18;n++){i.now+=50;pip_step(&m,&i);if(m.pose.blink>blink)blink=m.pose.blink;}
    assert(blink>.95f&&m.pose.blink==0&&m.pose.anger>.6f);
}
static void care_routine_regressions(void)
{
    pip_model m;pip_input i=sample(1000);i.hour=23;i.imu_valid=0;
    pip_init(&m,NULL,i.now);pip_set_context(&m,.8f,.4f,.8f,.9f,0);
    pip_set_quiet(&m,0);
    /* A late custom awake window must beat the previous hard-coded bedtime. */
    for(int n=0;n<6000;n++) {
        i.now+=20;pip_step(&m,&i);
        assert(m.action!=PIP_SLEEPY&&m.action!=PIP_ASLEEP);
    }
    /* A daytime quiet window must also work without remapping the RTC hour. */
    i.hour=14;pip_set_quiet(&m,1);run(&m,&i,11000,20);
    assert(m.action==PIP_ASLEEP);
    /* The wake boundary takes effect on the next step, not after a 60 s clip. */
    pip_set_quiet(&m,0);i.now+=20;pip_step(&m,&i);
    assert(m.action==PIP_WAKE&&m.last_attention==i.now);
    pip_set_quiet(&m,0);uint32_t started=m.action_ms;
    run(&m,&i,600,20);assert(m.action_ms==started);
    /* Hosts without care routines keep the original standalone behavior. */
    pip_init(&m,NULL,1000);i=sample(1000);i.hour=23;i.imu_valid=0;
    assert(m.quiet_override==-1);run(&m,&i,27000,20);assert(m.action==PIP_ASLEEP);
}
int main(void)
{
    resume_regressions();
    startle_cooldown_regression();
    face_down_priority_regressions();
    sustained_stroke_regression();
    low_rate_blink_regression();
    care_context_regressions();
    grumpy_and_care_recovery();
    contextual_performance_regressions();
    care_routine_regressions();
    pip_model m;pip_input i=sample(1000);pip_init(&m,NULL,i.now);
    assert(pip_memory_valid(&m.memory));
    pip_memory bad=m.memory;bad.familiarity=1001;assert(!pip_memory_valid(&bad));
    bad=m.memory;bad.version=99;assert(!pip_memory_valid(&bad));
    run(&m,&i,6000,20);
    assert(fabsf(m.bias_x-3.5f)<.2f); /* measured board gyro bias is not spinning */
    pip_model before=m;pip_step(&m,&i);assert(!memcmp(&m,&before,sizeof m));
    pip_pointer(&m,1,120,125,i.now);
    pip_pointer(&m,2,150,128,i.now+250);
    assert(m.stroke&&m.action==PIP_PETTED);
    pip_pointer(&m,0,150,128,i.now+300);assert(!m.down);
    pip_tap(&m,i.now+500);assert(m.action==PIP_BOOP);
    pip_tap(&m,i.now+750);assert(m.action==PIP_PLAY);
    uint32_t familiar=m.memory.familiarity;
    for(int n=0;n<100;n++)pip_tap(&m,i.now+800+n);
    assert(m.memory.familiarity==familiar); /* spam cannot farm memory */
    i.now+=1000;pip_force(&m,"idle",i.now);
    assert(!pip_force(&m,"not-a-state",i.now));
    i.imu_valid=0;run(&m,&i,6000,20);assert(!m.imu_valid&&!m.face_down);
    i.imu_valid=1;i.imu.ax=NAN;run(&m,&i,100,20);assert(!m.imu_valid);finite_pose(&m);
    i=sample(i.now);run(&m,&i,6000,20);pip_force(&m,"idle",i.now);
    i.imu.gx=230;run(&m,&i,700,20);assert(m.action==PIP_DIZZY);
    i=sample(i.now);run(&m,&i,5000,20);assert(m.action!=PIP_DIZZY);
    /* Shake detection needs sustained evidence and must recover afterward. */
    pip_force(&m,"idle",i.now);run(&m,&i,5000,20);
    for(int n=0;n<30;n++){i.now+=20;i.imu.ax=(n&1)?1.6f:-1.6f;pip_step(&m,&i);}
    assert(m.action==PIP_DIZZY);
    i=sample(i.now);run(&m,&i,6000,20);
    i.imu.gx=32;run(&m,&i,1700,20);assert(m.action!=PIP_ROCKED);
    for(int n=0;n<180;n++){i.imu.gx=(n/25&1)?32:-32;i.now+=20;pip_step(&m,&i);}
    assert(m.action==PIP_ROCKED);
    /* Missing sensors must not cause a false face-down sleep. */
    i.imu_valid=0;run(&m,&i,1000,20);assert(!m.face_down);
    /* Night, unattended -> drowsy -> asleep; touch wakes immediately. */
    pip_init(&m,NULL,1000);i=sample(1000);i.hour=23;i.imu_valid=0;
    run(&m,&i,27000,20);assert(m.action==PIP_ASLEEP);
    pip_pointer(&m,1,120,130,i.now);assert(m.action==PIP_WAKE);
    pip_pointer(&m,0,120,130,i.now+100);i.now+=100;
    run(&m,&i,6000,20);assert(m.action!=PIP_ASLEEP);
    /* Supplied mounting: face up is -Z, face down +Z, require dwell. */
    pip_init(&m,NULL,1000);i=sample(1000);i.imu.az=1;
    run(&m,&i,1000,20);assert(m.action!=PIP_ASLEEP);
    run(&m,&i,9000,20);assert(m.action==PIP_ASLEEP);
    i.imu.az=-1;i.imu.gx=50;run(&m,&i,1200,20);assert(m.action!=PIP_ASLEEP);
    /* Unsigned elapsed time works across millisecond rollover. */
    pip_init(&m,NULL,0xfffff000u);i=sample(0xfffff000u);run(&m,&i,8000,20);finite_pose(&m);
    /* Rendering rate does not decide which action finishes first. */
    pip_model a,b;pip_init(&a,NULL,0);pip_init(&b,NULL,0);
    pip_input ia=sample(0),ib=sample(0);ia.imu_valid=ib.imu_valid=0;
    pip_force(&a,"happy",0);pip_force(&b,"happy",0);
    run(&a,&ia,1500,10);run(&b,&ib,1500,50);
    assert(a.action==b.action&&fabsf(a.pose.lower-b.pose.lower)<.03f);
    /* Every demo has bounded, finite geometry for its entire lifetime. */
    for(int action=0;action<PIP_ACTION_COUNT;action++) {
        pip_init(&m,NULL,0);i=sample(0);i.imu_valid=0;
        assert(pip_force(&m,pip_action_name((pip_action)action),0));
        for(int n=0;n<800;n++){i.now+=16;pip_step(&m,&i);finite_pose(&m);}
    }
    puts("PASS Pip: context/energy, grumpy/care recovery, contextual performances, gestures, resume, startle cooldown, face-down priority, sustained care, blink, bias/filtering, shake/rock, recovery, sleep/wake, memory, wrap, frame rates, all poses");
}
