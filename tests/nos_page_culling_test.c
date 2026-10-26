/* Real page renderers against their original unculled page loops. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "prefs.h"
#include "svc.h"
#include "hal_ext.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>
nos_prefs_t nos_prefs;
uint32_t nos_frame; double nos_t; float nos_dt;
int pblur_baking;
void *(hal_big_alloc)(uint32_t n) { return malloc(n); }
void ui_fblur_coverage_exclude(const gfx_fb *f,int a,int b,int c,int d) { (void)f;(void)a;(void)b;(void)c;(void)d; }
static int blur_calls;
float nos_mb_track_step(nos_mb_track *t, float p) { (void)t; (void)p; return 10; }
float nos_mblur_step(float p, int held) { (void)held; return p; }
void nos_mblur_v(gfx_fb *f,int a,int b,float s) { (void)f; (void)a; (void)b; (void)s; blur_calls++; }
int nos_subview(const gfx_fb *fb, gfx_fb *out, int x0, int y0, int x1, int y1, int ox, int oy)
{
    *out = *fb;
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (x0 > c0) c0 = x0;
    if (x1 < c1) c1 = x1;
    if (c1 <= c0) return 0;
    out->cx0 = c0; out->cx1 = c1;
    out->ox = fb->ox + ox; out->oy = fb->oy + oy;
    if (fb->rec) return 1;
    int r0 = y0 - fb->y_base, r1 = y1 - fb->y_base;
    if (r0 < 0) r0 = 0;
    if (r1 > fb->h) r1 = fb->h;
    if (r1 <= r0) return 0;
    out->px = gfx_row(fb, r0) + fb->x_base;    /* gfx_row subtracts x_base */
    out->y_base = fb->y_base + r0;
    out->h = r1 - r0;
    return 1;
}

static unsigned rng=7477;
static unsigned random_word(void) { rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng; }
static uint16_t actual[GFX_W*32+8],expected[GFX_W*32+8];
static void fill(void) { for (unsigned i=0;i<sizeof actual/sizeof *actual;i++) actual[i]=expected[i]=(uint16_t)random_word(); }
#ifdef TEST_HEART
#include "../nos/apps/fit_heart.c"
static void heart_draw_reference(gfx_fb *fb)
{
    float off = floorf(H.vp.sp.x + 0.5f);
    for (int pg = 0; pg < NPAGES; pg++) {
        float oy = pg * PAGE_H - off;
        if (oy <= -PAGE_H || oy >= PAGE_H) continue;
        if (pg == 0) {
            ui_label(fb, UI_CX, (int)(UI_TITLE_Y + oy), "HEART RATE", C_SEC, UI_CENTER);
            dl_draw(fb, &H.trace, oy);              /* the lists carry their own alpha */
            dl_draw(fb, &H.illu, oy);
            if (H.kout > 0.01f) draw_mode(fb, oy, H.prev_mode, H.kout, 1);
            if (H.kin > 0.01f) draw_mode(fb, oy, H.mode, H.kin, 0);
        } else draw_history(fb, oy);
    }
    /* motion blur on the pages as they slide (nos.h), not on the scroll bar */
    static nos_mb_track mb;
    float mbs = nos_mblur_step(nos_mb_track_step(&mb, off), H.vp.drag);
    if (mbs > 0 && mbs < PAGE_H * 0.5f) nos_mblur_v(fb, 0, GFX_H, mbs);
    float ba = vp_bar_alpha(&H.vp);
    if (ba > 0) ui_scrollbar(fb, H.vp.sp.x, PAGE_H * NPAGES, PAGE_H, ba);
}

static hdot dots[TRACE_MAX],illus[ILLU_MAX];
static dl_band bands[(TRACE_MAX+DL_GROUP-1)/DL_GROUP],ibands[(ILLU_MAX+DL_GROUP-1)/DL_GROUP];
static void state(int iteration) {
 H.trace.d=dots;H.trace.band=bands;H.trace.cap=TRACE_MAX;
 H.illu.d=illus;H.illu.band=ibands;H.illu.cap=ILLU_MAX;
 dl_reset(&H.trace);dl_reset(&H.illu);
 H.u=(iteration%17)/16.f;H.hy=m_lerp(134,112,H.u);H.heart_scale=1.f+(iteration%17)*(.55f/16.f);
 H.mode=iteration%4;H.prev_mode=(iteration/4)%4;H.prev_locked=(iteration/7)%2;H.prev_u=1-H.u;
 H.locked=H.mode==M_LOCKED;H.lock_t=(iteration%21)*.1f;H.t=H.lock_t;H.mode_t=(iteration%21)*.025f;
 H.kin=(iteration%17)/16.f;H.kout=1-H.kin;H.lock_in=H.u;H.lock_out=1-H.u;
 H.bpm_shown=60+iteration%120;H.bpm_prev=H.bpm_shown-3;H.prev_bpm=120;
 H.spo2=iteration%101;H.prev_spo2=98;H.r.quality=(iteration%33)/32.f;H.bpm_morph=H.kin;
 H.now_a=H.kin;H.last_a=H.kout;H.foot_last=73;H.hd.last=82;H.hany=iteration%2;
 H.hd.mn=40;H.hd.avg=72;H.hd.mx=180;H.now_h=(iteration%240)/10.f;
 for(int i=0;i<3;i++)H.blk_p[i]=H.kin;
 for(int i=0;i<48;i++)H.hcols[i]=(uint8_t)random_word();
 for(int i=0;i<100;i++)dl_add(&H.trace,80+i*3.1f,(iteration%2?324:238)+(iteration%2?19:50)*gfx_fast_sin(i*.2f),5.5f,C_PRI);
 build_illu(iteration*.025f,H.kin);
 H.vp.sp.x=(int)(random_word()%670)-100;H.vp.bar_k=H.kin;
}
#define DRAW heart_draw_reference
#define CULLED heart_draw
#else
#include "../nos/apps/fit_activity.c"
static void act_draw_reference(gfx_fb *fb)
{
    float off = vp_off(&A.vp);
    for (int pg = 0; pg < NPAGES; pg++) {
        float oy = pg * PAGE_H - off;
        if (oy <= -PAGE_H || oy >= PAGE_H) continue;
        if (pg == 0) draw_hero(fb, oy, &A.hero, 0);
        else if (pg == 1) draw_today(fb, oy);
        else draw_week(fb, oy);
    }
    /* motion blur on the pages as they slide (nos.h), not on the scroll bar */
    static nos_mb_track mb;
    float mbs = nos_mblur_step(nos_mb_track_step(&mb, off), A.vp.drag);
    if (mbs > 0 && mbs < PAGE_H * 0.5f) nos_mblur_v(fb, 0, GFX_H, mbs);
    vp_draw_bar(fb, &A.vp);
}

static void state(int iteration) {
 s_act.steps=(int)(random_word()%100000);s_act.goal=8000;
 s_act.km=(random_word()%10001)/1000.f;s_act.kcal=(random_word()%10001)/100.f;s_act.active_min=(int)(random_word()%1441);
 for(int i=0;i<24;i++){s_act.hourly[i]=(uint16_t)random_word();A.col_g[i]=(random_word()%1025)/1024.f;}
 for(int i=0;i<7;i++){s_act.week[i]=random_word()%50000;A.bar_g[i]=(random_word()%1025)/1024.f;}
 for(int i=0;i<3;i++)A.blk_p[i]=(random_word()%1025)/1024.f;
 A.wk_p=(random_word()%1025)/1024.f;A.now_on=random_word()%2;A.goal_on=random_word()%2;
 s_hour=(int)(random_word()%24);s_wday=(int)(random_word()%7);nos_prefs.units_metric=random_word()%2;
 hero_start(&A.hero,0);A.hero.t=(iteration%21)*.1f;A.hero.fill=(iteration%97)*1.f;
 A.hero.morph=(iteration%17)/16.f;A.hero.prev=A.hero.value+18;
 nos_step_style=iteration%PROG_NSTYLES;
 A.vp.sp.x=(int)(random_word()%1150)-100;A.vp.bar_k=(iteration%17)/16.f;
 activity_changed();
}
#define DRAW act_draw_reference
#define CULLED act_draw
#endif
int main(void) {
 dot_prebake_all();
 for(int i=0;i<1600;i++) {
  nos_frame++;state(i);fill();
  gfx_fb f={0};f.h=i%4==0?1:i%4==1?16:i%4==2?18:31;
  f.y_base=(int)(random_word()%650)-90;f.oy=(int)(random_word()%401)-200;
  f.ox=(int)(random_word()%201)-100;f.dim=i%3?0:13;
  f.cx0=(int)(random_word()%220);f.cx1=f.cx0+1+(int)(random_word()%245);
  if(i%4==0){f.x_base=f.cx0;f.stride=f.cx1-f.cx0;}
  blur_calls=0;f.px=expected;DRAW(&f);assert(blur_calls==1);
  blur_calls=0;f.px=actual;CULLED(&f);assert(blur_calls==1);
  if(memcmp(actual,expected,sizeof actual)){fprintf(stderr,"pixel mismatch case%d ybase%d oy%d h%d\n",i,f.y_base,f.oy,f.h);abort();}
  gfx_rec a={0},b={0};f.rec=&a;DRAW(&f);f.rec=&b;CULLED(&f);
  assert(!memcmp(&a,&b,sizeof a));
 }
 puts("PASS 1600 pixel+record cases: page motion, translated iris buffers, stripe/dirty/packed/dim, live entry/morphs and blur preserved");return 0;
}
