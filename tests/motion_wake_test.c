#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../core/motion_wake.h"
static motion_wake_state s;
static uint32_t now;
static int wakes,last_reason,eligible=1,step=20;
static void reset(int pickup,int turn)
{
    motion_wake_config cfg={pickup,turn,{0,0,-1}};
    motion_wake_init(&s,&cfg);now=0;wakes=last_reason=0;eligible=1;
}
static void feed(hal_imu m,int valid)
{
    now+=(uint32_t)step;
    int reason=motion_wake_update(&s,&m,now,valid,eligible);
    if(reason){wakes++;last_reason=reason;}
}
static hal_imu pose(float degrees,int side)
{
    float rad=degrees*.01745329252f;
    return (hal_imu){.ay=side*sinf(rad),.az=-cosf(rad)};
}
static void hold(float degrees,int side,int ms)
{ for(int t=0;t<ms;t+=step)feed(pose(degrees,side),1); }
static void turn(float from,float to,int side,int ms)
{
    for(int t=step;t<=ms;t+=step) {
        hal_imu m=pose(from+(to-from)*(float)t/ms,side);
        m.gx=side*(from-to)*1000/ms;feed(m,1);
    }
}
static void pickup(void)
{
    hold(0,1,1000);
    for(int t=0;t<180;t+=step)feed((hal_imu){.az=-1.22f},1);
    for(int t=0;t<180;t+=step)feed((hal_imu){.az=-.82f},1);
    hold(0,1,400);
}
int main(void)
{
    for(step=20;step<=40;step+=20) {
        reset(1,1);hold(0,1,60000);assert(wakes==0); /* face-up is not a gesture */
        for(int side=-1;side<=1;side+=2) {
            reset(0,1);hold(90,side,800);turn(90,0,side,640);hold(0,side,500);
            assert(wakes==1 && last_reason==MOTION_WAKE_TURN);
            hold(0,side,5000);assert(wakes==1);
            hold(90,side,1000);turn(90,0,side,640);hold(0,side,500);assert(wakes==2);
        }
        reset(1,0);pickup();assert(wakes==1 && last_reason==MOTION_WAKE_PICKUP);
        reset(0,0);pickup();assert(wakes==0);
        reset(0,1);pickup();assert(wakes==0);
        reset(1,1);eligible=0;pickup();assert(wakes==0);
        reset(1,1);hold(0,1,1000); /* short tap/bump */
        feed((hal_imu){.az=-1.25f},1);feed((hal_imu){.az=-1.25f},1);
        hold(0,1,2000);assert(wakes==0);
        reset(0,1);hold(90,1,800);turn(90,65,1,400);hold(65,1,1000);assert(wakes==0);
        reset(0,1);hold(90,1,800);turn(90,0,1,640); /* no settled viewing hold */
        turn(0,90,1,640);hold(90,1,1000);assert(wakes==0);
        reset(1,1);hold(0,1,800);
        for(int t=0;t<2000;t+=step) {
            hal_imu m=pose((t/100)%2?55:0,1);m.gx=(t/100)%2?220:-220;
            feed(m,1);
        }
        hold(0,1,2000);assert(wakes==0); /* shake reversals */
        reset(1,1);hold(0,1,800);feed((hal_imu){.az=-.1f},1);
        feed((hal_imu){.az=-3},1);hold(0,1,1000);assert(wakes==0);
        reset(1,1);hold(0,1,1000);feed((hal_imu){.az=NAN},1);hold(0,1,1000);assert(wakes==0);
        reset(0,1);hold(90,1,800);turn(90,0,1,640);feed(pose(0,1),0);hold(0,1,500);assert(wakes==0);
        reset(0,1);hold(90,1,800);turn(90,0,1,640);now+=1000;hold(0,1,500);assert(wakes==0);
        reset(0,1);hold(90,1,800);turn(90,0,1,640);
        for(int i=0;i<200;i++)assert(!motion_wake_update(&s,&(hal_imu){.az=-1},now,1,1));
        assert(wakes==0);hold(0,1,500);assert(wakes==1); /* duplicate samples add no hold time */
        reset(0,1);now=UINT32_MAX-1000;hold(90,1,800);turn(90,0,1,640);hold(0,1,500);assert(wakes==1);
        reset(1,1);eligible=0;
        for(int t=0;t<4000;t+=step)feed((hal_imu){.az=-1,.gx=4,.gy=3,.gz=-.5f},1);
        assert(s.bias_ready && fabsf(s.bias[0]-4)<.01f && s.speed<.01f && wakes==0);
        /* Gravity alone cannot infer absolute yaw: spinning around Z must not wake. */
        reset(0,1);hold(90,1,800);
        for(int t=0;t<500;t+=step){hal_imu m=pose(90,1);m.gz=90;feed(m,1);}
        hold(90,1,800);assert(wakes==0);
        /* A learned tilted viewing pose works without relying on gyro sign. */
        reset(0,1);hal_imu target=pose(40,-1);
        motion_wake_config cfg={0,1,{target.ax,target.ay,target.az}};
        motion_wake_init(&s,&cfg);hold(120,-1,800);turn(120,40,-1,640);hold(40,-1,500);assert(wakes==1);
    }
    puts("PASS motion wake: both wrists at 25/50 Hz, pickup/turn separation, hold, face-up rest, bumps/shakes/falls, invalid/stale/duplicate samples, bias, learned pose, disabled state and clock wrap");
    return 0;
}
