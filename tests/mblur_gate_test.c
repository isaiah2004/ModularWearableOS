/* Include the engine to exercise its private budget state; unused engine sections
 * are removed at link time. No renderer, board, or serial port is instantiated. */
#include "../nos/nos.c"
#include <assert.h>
#include <stdio.h>
uint32_t hal_micros(void) { return 0; }
unsigned gfx_hash_u(unsigned h, unsigned v) { return (h ^ v) * 16777619u; }
static int deny_alloc = 1;
void *hal_big_alloc(uint32_t bytes) { return deny_alloc ? 0 : calloc(1, bytes); }
unsigned gfx_hash_f(unsigned h, float v) { return gfx_hash_u(h, (unsigned)v); }
int gfx_rec_cmd(gfx_fb *fb,int x0,int y0,int x1,int y1,unsigned h)
{ (void)x0;(void)y0;(void)x1;(void)y1;(void)h;return fb->rec!=0; }
void ui_fblur_coverage_claim(const gfx_fb *fb,int x0,int y0,int x1,int y1)
{ (void)fb;(void)x0;(void)y0;(void)x1;(void)y1;assert(!"record-only test rasterized"); }
void mb_h4_rows(gfx_fb *fb,int a,int b,int k,uint32_t *line,mb_h4_state *s)
{ (void)fb;(void)a;(void)b;(void)k;(void)line;(void)s;assert(!"record-only test rasterized"); }
void mb_v_columns(gfx_fb *fb,int a,int b,int y0,int y1,int k,mb_v_columns_state *s)
{ (void)fb;(void)a;(void)b;(void)y0;(void)y1;(void)k;(void)s;assert(!"record-only test rasterized"); }

void mb_v_columns_reset(mb_v_columns_state *s) { (void)s; }

static const nos_screen live_screen={.name="live"},other_screen={.name="other"};
static void live_sample(unsigned cpu,unsigned area,unsigned baked,unsigned reused)
{
    nos_frame++;
    memset(&nos_last,0,sizeof nos_last);
    nos_last.raster_us=cpu;nos_last.dirty_px=area;
    nos_last.layer_bands=baked;nos_last.layer_reused=reused;
    s_mb_live_owner=&live_screen;s_mb_live_frame=nos_frame-1;
    s_stack[0].s=&live_screen;s_depth=1;T.state=TXS_IDLE;
}
static int full_v(void) { return mb_region_ok(1,0,0,GFX_W,GFX_H,0); }

static void cadence(void)
{
    const float dt[]={1.0f/30,1.0f/40,2.0f/40,3.0f/30};
    for(unsigned i=0;i<sizeof dt/sizeof *dt;i++) {
        nos_dt=dt[i];
        assert(fabsf(nos_mblur_step(720*nos_dt,0)-24)<0.001f);
        assert(fabsf(nos_mblur_step(-720*nos_dt,1)-24)<0.001f);
        assert(mb_len_log2(nos_mblur_step(250*nos_dt,0))==2);
        assert(mb_len_log2(nos_mblur_step(230*nos_dt,0))==0);
    }
    nos_mblur=1;assert(nos_mblur_step(20,1)==0);
    nos_mblur=0;assert(nos_mblur_step(20,0)==0);
    nos_mblur=2;nos_dt=0;assert(nos_mblur_step(20,0)==0);
    nos_dt=1.0f/30;
}

static void measured_admission(void)
{
    const unsigned full=GFX_W*GFX_H;
    live_sample(2000,full,0,0);s_mb_live_owner=0;
    assert(full_v()==0 && !s_mb_full_measured); /* unknown conservative seed */
    live_sample(2000,full,0,0);
    assert(full_v()==2 && s_mb_full_measured); /* actual live cost unlocks V */
    uint32_t spent=s_mb_committed;
    assert(full_v()==2 && s_mb_committed==spent); /* record/stripes reserve once */
    live_sample(2000,full/2,0,0);assert(full_v()==0 && !s_mb_full_measured);
    live_sample(2000,full,1,0);assert(full_v()==0 && !s_mb_full_measured);
    live_sample(2000,full,0,1);assert(full_v()==0 && !s_mb_full_measured);
    live_sample(2000,full,0,0);s_mb_live_owner=&other_screen;assert(full_v()==0);
    live_sample(2000,full,0,0);s_mb_live_frame--;assert(full_v()==0);
    live_sample(2000,full,0,0);T.state=TXS_AUTO;assert(full_v()==0);
    live_sample(18000,full,0,0);assert(full_v()==0); /* actual cost still vetoes */
    live_sample(2000,full,0,0);nos_last.record_us=13000;assert(full_v()==0);
    mb_learn(3,25000,full); /* slow measured kernel must count immediately */
    assert(s_mb_ns[3]>100);
    live_sample(2000,full,0,0);assert(full_v()==0);
    s_mb_ns[3]=100;
    /* Only admitted recording requests change the epoch. */
    gfx_rec rec={0};gfx_fb fb={0};fb.h=16;fb.cx1=GFX_W;fb.rec=&rec;
    live_sample(2000,full,0,0);uint32_t epoch=nos_mblur_record_epoch();
    nos_mblur_v(&fb,0,GFX_H,24);assert(nos_mblur_record_epoch()==epoch+1);
    nos_mblur_v(&fb,0,GFX_H,7);assert(nos_mblur_record_epoch()==epoch+1);
    live_sample(30000,full,0,0);nos_mblur_v(&fb,0,GFX_H,24);
    assert(nos_mblur_record_epoch()==epoch+1);
    live_sample(2000,full,0,0);nos_mblur_h(&fb,0,60,24);
    assert(nos_mblur_record_epoch()==epoch+2);
}

int main(void)
{
    cadence();
    assert(!mb_work_ready());
    deny_alloc = 0; assert(mb_work_ready()); /* recover after transient exhaustion */
    assert(nos_mblur_ns(2) == 35 && nos_mblur_ns(3) == 100);
    nos_frame = 100;
    nos_last.raster_us = 23000; nos_last.wait_us = 5000;
    nos_last.dirty_px = GFX_W * GFX_H;
    assert(nos_mblur_step(24, 0) == 24);       /* onset can blur */
    assert(mb_region_ok(0, 0, 0, 420, GFX_H, 0) == 2);
    uint32_t spent = s_mb_committed;
    assert(mb_region_ok(0, 0, 0, 420, GFX_H, 0) == 2);
    assert(s_mb_committed == spent && nos_mblur_count[0][1] == 1);
    assert(mb_region_ok(1, 0, 0, 200, 200, 0) == 0); /* combined frame budget */
    nos_frame++;
    assert(mb_region_ok(0, 0, 0, 60, GFX_H, 0) == 1); /* small pass stays full */
    nos_frame++;
    assert(mb_region_ok(0, 0, 0, 60, GFX_H, 1) == 2); /* cache miss is reduced only */
    nos_frame += 2;
    nos_last.raster_us = 500; nos_last.wait_us = 0; nos_last.dirty_px = 1000;
    assert(mb_region_ok(0, 0, 0, 420, GFX_H, 0) == 2); /* onset uses full repaint history */
    nos_frame++;
    nos_last.raster_us = 30000; nos_last.dirty_px = GFX_W * GFX_H;
    assert(mb_region_ok(0, 0, 0, GFX_W, GFX_H, 0) == 0);
    nos_frame++;
    nos_last.raster_us = 19000; nos_last.record_us = 2000;
    assert(mb_region_ok(1, 0, 0, GFX_W, GFX_H, 0) == 0); /* measured V plus recording */
    nos_mblur = 0; assert(nos_mblur_step(20, 0) == 0);
    nos_mblur = 1; assert(nos_mblur_step(20, 1) == 0);
    nos_mb_track track = {0};
    for (int i = 0; i < 100; i++) {
        nos_frame++;
        assert(nos_mb_track_step(&track, 0) == 0);
        assert(nos_mb_track_step(&track, 0) == 0); /* record + stripes cannot create motion */
    }
    nos_frame++; assert(nos_mb_track_step(&track, 12) == 12);
    assert(nos_mb_track_step(&track, 12) == 12);
    nos_frame++; assert(nos_mb_track_step(&track, 12) == 0);
    nos_mblur=2;measured_admission();
    puts("mblur gate: cadence, live calibration, cache/context rejection, cost learning, record epoch PASS");
    return 0;
}
