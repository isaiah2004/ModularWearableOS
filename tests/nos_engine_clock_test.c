/* Link the real engine against private simulator service objects. */
#include "nos.h"
#include "motion.h"
#include "render_history.h"
#include "nos_frame_pacer.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

extern int sim_program_main(int, char **);
static float tick_sum;
static nos_time_t authored_origin;
static unsigned ticks;
static unsigned draw_calls,bg_calls;
static void bg_tick(float dt) {(void)dt;bg_calls++;}
static int authored_done;
static unsigned actions[IN_CANCEL+1],banner_taps;
static int pull_on_drag;
static const nos_screen pulled={.name="CANCELLED PULL"};
static int clock_input(const nos_input *in) {
    actions[in->type]++;
    if(pull_on_drag && in->type==IN_DRAG_START)nos_pull(&pulled,TX_RISE,in);
    return 1;
}
static void banner_tapped(void) {banner_taps++;}
static void clock_tick(float dt) {
    assert(isfinite(dt) && dt > 0);
    tick_sum += dt;
    ticks++;
    authored_done = m_frame_k(authored_origin) >= (int)(.3f * NOS_FPS + .5f);
}
static void clock_draw(gfx_fb *fb) {
    draw_calls++;
    gfx_fill_rect(fb, 100, 100, 20, 20, (uint16_t)(ticks | 0x8000));
}
static const nos_screen clock_screen = {
    .name="CLOCK TEST", .tick=clock_tick, .draw=clock_draw,.input=clock_input,.flags=SCR_HDRAG,
};
static void step(float milliseconds, float expected) {
    nos_time_t before=nos_t;float sum=tick_sum;
    unsigned frames=nos_frame, count=ticks;
    nos_frame_run(milliseconds);
    assert(nos_frame==frames+1 && ticks==count+1);
    assert(fabsf(nos_dt-expected)<.000001f);
    assert(nos_history_interval_us()==(uint32_t)lroundf(expected*1000000.f));
    assert(fabs((nos_t-before)-expected)<.00002);
    assert(fabsf((tick_sum-sum)-expected)<.00002f);
}
static void pacing(int enabled) {
    nos_frame_pacer p={0};
    int64_t start=1000000, previous=start-25000;
    nos_time_t origin=nos_t;float expected=0;
    for(unsigned i=0;i<80;i++) {
        int64_t interval=start-previous;
        previous=start;
        float dt=(float)interval/1000000.f;
        step((float)interval/1000.f,dt);
        expected+=dt;
        int64_t done=start+(i&1?28000:12000);
        int64_t next=nos_frame_pacer_next(&p,enabled,1,start,done,start+25000,25000);
        start=next>done?next:done;
    }
    assert(fabs((nos_t-origin)-expected)<.0001);
    assert(fabs(tick_sum-nos_t)<.0002);
}
int main(int argc,char **argv) {
    assert(!sim_program_main(argc,argv));
    nos_set_root(&clock_screen);
    /* Use a small clock origin so accumulated-float precision cannot hide a
     * missing or fabricated period. The app sees the exact same clock. */
    nos_t=tick_sum=0; ticks=0; authored_origin=0;
    const float irregular[]={12,28,8,25};
    for(unsigned i=0;i<4;i++)step(irregular[i],irregular[i]/1000.f);
    assert(fabs(nos_t-.073)<.000001);
    step(1000,4.f/NOS_FPS);
    step(0,1.f/NOS_FPS);step(-1,1.f/NOS_FPS);
    step(NAN,1.f/NOS_FPS);step(INFINITY,1.f/NOS_FPS);
    for(int mode=0;mode<2;mode++)pacing(mode);
    /* Authored duration is tested at the same wall-time boundary under two
     * different presentation cadences, independent of the number of ticks. */
    for(int cadence=0;cadence<2;cadence++) {
        authored_origin=nos_t;authored_done=0;
        int elapsed=0, index=0;
        while(elapsed<280) {
            int ms=cadence?(index++&1?28:12):25;
            if(elapsed+ms>280)ms=280-elapsed;
            step((float)ms,ms/1000.f);elapsed+=ms;
        }
        assert(!authored_done);
        step(25,.025f);assert(authored_done);
    }
    /* Both absolute origins and their stored deadline owners must preserve the
     * same relative progress at the old float failure boundaries. */
    uint16_t *pixels=calloc(GFX_W*GFX_H,sizeof *pixels);assert(pixels);
    nos_set_stripes(NULL,NULL,0,NULL,NULL);nos_init(pixels);nos_debug_force_full=1;
    const nos_time_t origins[]={65536,131072,262144,524288,1073741824};
    for(unsigned origin=0;origin<sizeof origins/sizeof *origins;origin++) {
        nos_set_root(&clock_screen);nos_t=origins[origin];authored_origin=nos_t;
        memset(actions,0,sizeof actions);tick_sum=0;authored_done=0;
        mtween tween;mtw_start(&tween,0,100,M_FRAMES(12),&EASE_LINEAR);
        nos_time_t exact=nos_enter_t();assert(exact==nos_t);
        nos_toast("CLOCK");nos_touch(200,250,1);
        for(int frame=0;frame<80;frame++) {
            step(25,.025f);
            if(frame==5) {
                assert(mtw_value(&tween)>45 && mtw_value(&tween)<55 && !mtw_done(&tween));
                unsigned lit=0;for(int y=0;y<95;y++)for(int x=80;x<386;x++)lit+=pixels[y*GFX_W+x]!=0;
                assert(lit);
            }
            if(frame==11)assert(mtw_done(&tween) && mtw_value(&tween)==100 && authored_done);
        }
        assert(actions[IN_LONG]==1 && !actions[IN_TAP]);
        nos_touch_at(200,250,2,hal_millis());step(25,.025f);
        assert(actions[IN_CANCEL]==1 && !actions[IN_TAP] && !actions[IN_UP]);
        unsigned lit=0;for(int y=0;y<95;y++)for(int x=80;x<386;x++)lit+=pixels[y*GFX_W+x]!=0;
        assert(!lit); /* toast deadline completed at every large origin */
        nos_banner("PRECISE CLOCK","BANNER DEADLINE",0,banner_tapped);
        for(int frame=0;frame<15;frame++)step(25,.025f);
        nos_touch(233,100,1);step(25,.025f);nos_touch_cancel();step(25,.025f);assert(!banner_taps);
        for(int frame=0;frame<220;frame++)step(25,.025f);
        for(int x=100;x<350;x++)assert(!pixels[60*GFX_W+x]);
        printf("PASS large origin %.0f: exact frame clock, tween, long press, toast/banner deadlines, touch cancellation\n",origins[origin]);
    }
    nos_set_root(&clock_screen);memset(actions,0,sizeof actions);pull_on_drag=1;
    nos_touch(233,400,1);step(25,.025f);nos_touch(233,300,1);step(25,.025f);
    assert(nos_contains(&clock_screen) && nos_contains(&pulled) && nos_depth()==2);
    nos_touch_cancel();for(int frame=0;frame<60;frame++)nos_frame_run(25);
    assert(nos_depth()==1 && nos_top()==&clock_screen && !nos_contains(&pulled));
    assert(!actions[IN_TAP] && !actions[IN_DRAG_END]);pull_on_drag=0;
    static const nos_screen covered={.name="COVERED"},replacement={.name="REPLACEMENT"};
    nos_push(&covered,TX_NONE);nos_replace(&replacement,TX_PUSH);
    assert(nos_contains(&clock_screen) && nos_contains(&covered) && nos_contains(&replacement));
    for(int frame=0;frame<60;frame++)nos_frame_run(25);
    assert(!nos_contains(&covered) && nos_contains(&replacement));
    nos_set_root(&clock_screen);memset(actions,0,sizeof actions);nos_bg_tick=bg_tick;
    nos_touch(233,300,1);nos_frame_run(25);assert(actions[IN_DOWN]==1);
    nos_time_t frozen=nos_t;unsigned old_ticks=ticks,old_bg=bg_calls,old_draws=draw_calls;
    nos_touch(233,100,1);nos_button(1);nos_air(1);
    for(int frame=0;frame<20;frame++)nos_frame_run_readonly(25);
    assert(nos_t==frozen && ticks==old_ticks && bg_calls==old_bg && draw_calls>old_draws);
    assert(!actions[IN_CANCEL] && !actions[IN_TAP] && !actions[IN_AIR] && !actions[IN_BTN]);
    nos_button(0);nos_frame_run(25);
    assert(actions[IN_CANCEL]==1 && !actions[IN_TAP] && !actions[IN_UP] && !actions[IN_DRAG_END] && !actions[IN_BTN]);
    assert(bg_calls==old_bg+1 && ticks==old_ticks+1 && nos_t>frozen);nos_bg_tick=NULL;
    puts("PASS read-only OTA presentation: frozen clock/tick/background, cached drawing, discarded input, deferred cancel under next mutation scope");
    nos_t=1e12;assert(m_frame_k(-1e12)==INT_MAX);nos_t=-1e12;assert(m_frame_k(1e12)==INT_MIN);
    free(pixels);
    puts("PASS actual engine elapsed clock: irregular cadence, stall cap, invalid inputs, pacing and authored duration");
    return 0;
}
