/* Actual cache callbacks versus the authored live renderer, not settled snapshots. */
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
int nos_mblur=2;
const nos_screen *nos_top(void) { return NULL; }
static int test_transitioning=1;
int nos_transitioning(void) { return test_transitioning; }
uint32_t hal_micros(void) { return 0; }
uint64_t hal_epoch_seconds(void) { return 1711454400u; } /* fixed Tuesday, 12:00 UTC */
void nos_ink_dim(gfx_fb *f,float k) { int keep=(int)lroundf((32-f->dim)*m_clamp(k,0,1));f->dim=32-keep; }
void *(hal_big_alloc)(uint32_t n) { return malloc(n); }
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


#if APP_CASE == 1
#include "../nos/apps/out_weather.c"
#elif APP_CASE == 2
#include "../nos/apps/set_about.c"
#elif APP_CASE == 3
#include "../nos/apps/set_modules.c"
#else
#error APP_CASE required
#endif
#include "../nos/ui_blur.c"
static uint16_t got[GFX_W*240],expected[GFX_W*240];
#if APP_CASE == 1
static uint16_t previous[GFX_W*240];
#endif
static unsigned signature(int item) { return fb_sig(item,FB_view.fn,FB_view.ctx); }
/* Complete a stable revision to test publication pixels and invalidation.
 * This helper deliberately freezes state during baking: it does not establish
 * scheduler fairness or cache coverage while authored animation advances. */
static blur_item *retain(int item)
{
    blur_item *s=blur_cache_get(&FB,FB_view.id,item,FB_view.W,FB_view.H,FB_view.C,FB_view.X0,FB_view.horiz);
    assert(s);s->pinned=1;
    unsigned work=0;
    while(s->sharp_rows<s->h+2*s->context || s->level!=2) {
        assert(fb_work(s,FB_view.fn,FB_view.ctx,2)>=0);
        assert(++work<100);
    }
    return s;
}
static gfx_fb pixels(uint16_t *out,int y,int h)
{
    gfx_fb f={0};f.px=out;f.h=h;f.y_base=y;f.cx1=GFX_W;return f;
}
static void compare(gfx_fb *f,void (*actual)(gfx_fb *),void (*reference)(gfx_fb *))
{
    for(unsigned i=0;i<sizeof got/sizeof *got;i++) got[i]=expected[i]=(uint16_t)(i*743u);
    f->px=got;actual(f);f->px=expected;reference(f);
    assert(!memcmp(got,expected,sizeof got));
}

#if APP_CASE == 1
static int item;
static const int canonical=188;
static void actual(gfx_fb *f) { band_draw(f,item,BAND_X0,canonical,BAND_X1,BAND_H,NULL); }
static void reference(gfx_fb *f)
{
    float off=(float)(item*BAND_H-canonical);
    weather_live_body(f,&off);
}
static void setup(void)
{
    memset(&W,0,sizeof W);memset(&s_cur,0,sizeof s_cur);
    s_cur.ws=WS_OK;s_have_view=1;s_cur.d.temp_c=21;s_cur.d.feels_c=19;
    s_cur.d.humidity=55;s_cur.d.wind_kph=12;s_cur.d.hi_c=24;s_cur.d.lo_c=16;
    /* Forecast labels now belong to acquisition epochs in the snapshot;
     * raster/cache tests do not synthesize a second app-local clock. */
    strcpy(s_cur.d.loc,"TEST");
    W.t0=-100;W.frame=0;nos_t=10;nos_frame=1;OTH=NULL;SK=TG=1;
    ui_fblur_tick(&W,0,290.f/M_FPS,0,5,NBANDS,BAND_H,BAND_X0,BAND_X1,1,band_draw,NULL);
}
static void animation(void)
{
    setup();
    for(int condition=0;condition<8;condition++) for(int sample=0;sample<4;sample++) {
        int frame=sample==0?0:sample==1?37:sample==2?119:REST;
        s_cur.d.cond=condition;W.frame=frame;nos_frame++;
        for(item=0;item<5;item++) {
            gfx_fb f=pixels(NULL,canonical-24,138);compare(&f,actual,reference);
            f=pixels(NULL,canonical+(frame%90),16);f.ox=7;f.oy=-11;f.cx0=30;f.cx1=432;f.dim=9;
            compare(&f,actual,reference);
        }
    }
    s_cur.d.cond=0;W.frame=0;
    unsigned animated=signature(1),stationary=signature(5);
    blur_item *moving=retain(1),*still=retain(5);
    W.frame=37;nos_frame++;
    assert(signature(1)!=animated && signature(5)==stationary);
    assert(!fb_validate(moving) && fb_validate(still));
    item=1;gfx_fb f=pixels(got,canonical-24,138);
    memset(got,0,sizeof got);W.frame=0;actual(&f);memcpy(previous,got,sizeof got);
    memset(got,0,sizeof got);W.frame=37;actual(&f);
    assert(memcmp(previous,got,sizeof got));
}

#elif APP_CASE == 2
static int row;
static float row_y;
static void actual(gfx_fb *f) { band_draw(f,0,20,(int)row_y,446,BAND_H,NULL); }
static void reference(gfx_fb *f) { draw_entry(f,row,row_y,0); }
static void layout(void)
{
    int units=0;
    for(int r=0;r<R_COUNT;r++){A.start[r]=units;units+=k_units[r];}
    A.nunits=units;
}
static void setup(void)
{
    memset(&A,0,sizeof A);A.t0=0;nos_t=2;nos_frame=1;layout();
    strcpy(A.version,"test");strcpy(A.psram,"2 MB");strcpy(A.sram,"20 KB");
    amt_set(&A.live[R_UPTIME],"1:02:10",1);amt_set(&A.live[R_RAIL],"3.95",1);
    ui_fblur_tick(&A,0,290.f/M_FPS,0,5,(A.nunits*AU+89)/90,BAND_H,20,446,1,band_draw,NULL);
}
static void isolate(int r)
{
    for(int i=0;i<R_COUNT;i++) A.start[i]=10000;
    A.start[r]=0;row=r;
}
static void animation(void)
{
    setup();isolate(R_LOGO);row_y=100;
    for(int phase=0;phase<40;phase++) {
        nos_t=1.05f+phase*.025f;nos_frame++;
        gfx_fb f=pixels(NULL,90,210);compare(&f,actual,reference);
        f=pixels(NULL,100+phase*3,16);f.ox=-9;f.oy=7;f.dim=11;compare(&f,actual,reference);
    }
    isolate(R_UPTIME);row_y=199;
    strcpy(A.live[R_UPTIME].from,"1:02:09");strcpy(A.live[R_UPTIME].to,"1:02:10");
    A.live[R_UPTIME].t0=2;
    for(int phase=0;phase<=8;phase++) {
        nos_t=2+(float)phase/M_FPS;nos_frame++;
        gfx_fb f=pixels(NULL,190,90);compare(&f,actual,reference);
    }
    layout();nos_t=1.2f;
    unsigned glint=signature(0),stationary=signature(4);
    blur_item *moving=retain(0),*still=retain(4);
    nos_t=1.65f;nos_frame++;
    assert(signature(0)!=glint && signature(4)==stationary);
    assert(!fb_validate(moving) && fb_validate(still));
    A.live[R_UPTIME].t0=2;nos_t=2;
    unsigned morph=signature(6);stationary=signature(4);
    moving=retain(6);still=retain(4);
    nos_t=2+4.f/M_FPS;nos_frame++;
    assert(signature(6)!=morph && signature(4)==stationary);
    assert(!fb_validate(moving) && fb_validate(still));
}

#elif APP_CASE == 3
static void actual(gfx_fb *f) { mband_draw(f,0,20,100,446,BAND_H,NULL); }
static void reference(gfx_fb *f) { draw_lrow(f,0,100,lunits(LE[0].kind)*MU,0); }
static void layout(void)
{
    int units=0;
    for(int e=0;e<NLE;e++){ML.start[e]=units;units+=lunits(LE[e].kind);}
    ML.nunits=units;
}
static void setup(void)
{
    memset(&ML,0,sizeof ML);ML.t0=-100;nos_t=2;nos_frame=1;layout();
    for(int m=0;m<M_COUNT;m++)V.ok[m]=1;
    strcpy(ML.sum.from,"8/16");strcpy(ML.sum.to,"9/16");ML.sum.t0=2;
    ui_fblur_tick(&ML,0,290.f/M_FPS,0,5,mbands(),BAND_H,20,446,1,mband_draw,NULL);
}
static void animation(void)
{
    setup();
    for(int e=1;e<NLE;e++)ML.start[e]=10000;
    for(int phase=0;phase<=8;phase++) {
        nos_t=2+(float)phase/M_FPS;nos_frame++;
        gfx_fb f=pixels(NULL,95,180);compare(&f,actual,reference);
        f=pixels(NULL,100+phase*7,16);f.oy=-9;f.ox=11;f.dim=7;compare(&f,actual,reference);
    }
    layout();nos_t=2;
    unsigned morph=signature(0),stationary=signature(4);
    blur_item *moving=retain(0),*still=retain(4);
    nos_t=2+4.f/M_FPS;nos_frame++;
    assert(signature(0)!=morph && signature(4)==stationary);
    assert(!fb_validate(moving) && fb_validate(still));
    const float speeds[]={0,15,150,290,359,360,900,1800};
    for(unsigned i=0;i<sizeof speeds/sizeof *speeds;i++)
    for(int mode=0;mode<=2;mode++) for(int held=0;held<=1;held++)
    for(int sign=-1;sign<=1;sign+=2) for(int skip=1;skip<=4;skip+=3) {
        nos_mblur=mode;ML.L.dragging=held;ML.L.sc.x=100;
        float dt=(float)skip/M_FPS;
        ML.fb_prev=100-sign*speeds[i]*dt;
        mband_tick(dt);
        int hold=mode && !(held && mode<2) && speeds[i]>=360;
        assert(ML.sample_hold==hold);
        assert(ML.fb_lv==ui_fblur_active(&ML));
        if(mode==2 && speeds[i]>=15 && speeds[i]<360)assert(ML.fb_lv && !ML.sample_hold);
    }
}
#endif
int main(void)
{
    dot_prebake_all();animation();ui_fblur_release();
    printf("PASS app%d authored band pixels and selective signatures at %gHz\n",APP_CASE,(double)M_FPS);
    return 0;
}
