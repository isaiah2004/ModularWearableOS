/* Integration of retained cache with recorded draw callbacks and RGB565 blits. */
#include "ui.h"
#include "nos.h"
#include "hal.h"
#include "ui_blur_observe.h"
#include "../nos/apps/native_tile.h"
#include "../firmware/main/nos_cache_capture.h"
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint32_t nos_frame;
int nos_mblur=2;
double nos_t;
static int changing,draws,fail_alloc;
static uint16_t ink=0xf800;
uint32_t hal_micros(void) {return 0;}
void *hal_big_alloc(uint32_t n) {return fail_alloc?NULL:malloc(n);}
static const nos_screen *test_screen;
const nos_screen *nos_top(void) {return test_screen;}
int nos_transitioning(void) {return 0;}
void nos_ink_dim(gfx_fb *fb,float k) {fb->dim=32-(int)((32-fb->dim)*k+0.5f);}
static void row(gfx_fb *fb,int i,int x0,int y,int x1,int h,void *ctx) {
 (void)i;(void)ctx;draws++;
 gfx_fill_rect(fb,x0,y,x1,y+h,ink);
 if(changing) ink=(uint16_t)(ink^0x07e0);
}
static void tick(const void *id,int h) {
 nos_frame++;nos_t=nos_frame/30.0f;
 ui_fblur_tick(id,1,6,0,0,1,h,0,32,0,row,0);
}
static void live_increment(gfx_fb *fb,void *ctx) {
 (void)ctx;
 if(fb->rec) {gfx_rec_cmd(fb,0,0,32,64,0x111);return;}
 int stride=fb->stride?fb->stride:GFX_W;
 for(int r=0;r<fb->h;r++) for(int x=fb->cx0;x<fb->cx1;x++) fb->px[r*stride+x-fb->x_base]++;
}
/* Independent per-pixel union/subtraction oracle, including changing metadata
 * after a cached lookup and queries on both sides of every Y boundary. */
static void coverage_reference(void) {
 static unsigned char truth[GFX_H][GFX_W];
 unsigned seed=731;
 gfx_fb f={0};f.h=GFX_H;f.cx1=GFX_W;
 for(int trial=0;trial<24;trial++) {
  nos_frame++;memset(truth,0,sizeof truth);
  for(int k=0;k<44;k++) {
   seed=seed*1664525u+1013904223u;int x0=(int)(seed%(GFX_W-1));
   seed=seed*1664525u+1013904223u;int y0=(int)(seed%(GFX_H-1));
   seed=seed*1664525u+1013904223u;int x1=x0+1+(int)(seed%(GFX_W-x0));
   seed=seed*1664525u+1013904223u;int y1=y0+1+(int)(seed%(GFX_H-y0));
   int exclude=k>=32;
   if(exclude) ui_fblur_coverage_exclude(&f,x0,y0,x1,y1);
   else ui_fblur_coverage_claim(&f,x0,y0,x1,y1);
   for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++) truth[y][x]|=exclude?2:1;
   /* Populate a lookup before the next metadata mutation. */
   int scratch[8][2];ui_fblur_row_spans(y0,scratch,8);
  }
  for(int direction=0;direction<2;direction++) for(int row=0;row<GFX_H;row++) {
   int y=direction?GFX_H-row-1:row,expected[160][2],n=0;
   for(int x=0;x<GFX_W;) {
    if(truth[y][x]!=1) {x++;continue;}
    expected[n][0]=x;
    while(x<GFX_W && truth[y][x]==1) x++;
    expected[n++][1]=x;
   }
   for(int cap=1;cap<=40;cap+=13) {
    int actual[40][2],want=n<cap?n:cap;
    int until=0;
    assert(ui_fblur_row_spans_run(y,actual,cap,&until)==want);
    assert(until>y && until<=GFX_H);
    for(int yy=y;yy<until;yy++) {
      int check[40][2];assert(ui_fblur_row_spans(yy,check,cap)==want);
      assert(!memcmp(check,actual,(size_t)want*sizeof actual[0]));
    }
    assert(!memcmp(actual,expected,(size_t)want*sizeof actual[0]));
   }
  }
 }
}
static void native_observation_reference(void) {
 uint16_t pixels[24*8],before[24*8],lut[256];uint8_t source[4]={20,60,100,200};
 for(int i=0;i<256;i++)lut[i]=(uint16_t)(i+1);
 memset(pixels,0x5a,sizeof pixels);memcpy(before,pixels,sizeof pixels);
 gfx_fb f={0};f.px=pixels;f.h=8;f.stride=24;f.x_base=2;f.y_base=10;
 f.ox=3;f.oy=4;f.cx0=7;f.cx1=20;
 int cover=ui_fblur_coverage_enable(1);ui_fblur_observation o;
 ui_fblur_observe_enable(1);ui_fblur_observe_begin(123);
 /* Requested local box becomes[3,23)x[8,20); independent clip is13x8. */
 ui_fblur_observe_native(&f,0,4,20,16,1);ui_fblur_observe_get(&o);
 assert(o.frame==123 && o.native_processed_area==104 && o.native_blurred_processed_area==104);
 assert(!memcmp(pixels,before,sizeof pixels));
 ui_fblur_observe_native(&f,0,4,20,16,0);ui_fblur_observe_get(&o);
 assert(o.native_processed_area==208 && o.native_blurred_processed_area==104);
 /* Recorder, offscreen bake, empty stripe and absent pixels count nothing. */
 gfx_rec rec={0};f.rec=&rec;ui_fblur_observe_native(&f,0,4,20,16,1);f.rec=NULL;
 ui_fblur_coverage_enable(0);ui_fblur_observe_native(&f,0,4,20,16,1);ui_fblur_coverage_enable(1);
 ui_fblur_observe_native(&f,0,30,20,40,1);f.px=NULL;ui_fblur_observe_native(&f,0,4,20,16,1);f.px=pixels;
 ui_fblur_observe_get(&o);assert(o.native_processed_area==208 && o.native_blurred_processed_area==104);
 /* Actual prepared ROM raster path emits its clipped source-union area. */
 ui_fblur_observe_begin(124);sprite8 s={source,2,2};
 native_prepared_blit(&f,&s,5,7,&s,5,7,0,lut,77,0,0,40,1,NULL,0);
 ui_fblur_observe_get(&o);assert(o.frame==124 && o.native_processed_area==8 && o.native_blurred_processed_area==8);
 assert(memcmp(pixels,before,sizeof pixels));
 /* A generic backing coverage claim cannot invent native blur evidence. */
 ui_fblur_observe_begin(125);ui_fblur_coverage_claim(&f,0,4,20,16);ui_fblur_observe_get(&o);
 assert(o.native_processed_area==0 && o.native_blurred_processed_area==0);
 nos_cache_capture capture;nos_cache_capture_begin(&capture);
 o.frame=125;o.native_processed_area=13;o.native_blurred_processed_area=8;
 nos_cache_capture_add(&capture,&o,125,1);nos_cache_capture_add(&capture,&o,126,1);
 assert(capture.frames==2 && capture.painted==2 && capture.stale_frames==1);
 assert(capture.native_frames==1 && capture.native_blurred_frames==1);
 assert(capture.native_processed_area==13 && capture.native_blurred_processed_area==8);
 ui_fblur_observe_enable(0);ui_fblur_observe_begin(126);ui_fblur_observe_native(&f,0,4,20,16,1);
 ui_fblur_observe_get(&o);assert(o.frame==125 && o.native_processed_area==0);
 ui_fblur_coverage_enable(cover);
 puts("Native blur observation: actual prepared pixels, clipping, disabled/record/bake gates and capture totals passed");
}
int main(int argc,char **argv) {
 int invalid_until=0;ui_fblur_row_spans_run(INT32_MAX,NULL,0,&invalid_until);assert(invalid_until==GFX_H);
 ui_fblur_row_spans_run(INT32_MIN,NULL,0,&invalid_until);assert(invalid_until==GFX_H);
 static const char a=0,b=0,c=0;
 assert(ui_fblur_level(0)==0 && ui_fblur_level(360.0f/M_FPS)==1 && ui_fblur_level(840.0f/M_FPS)==2 && ui_fblur_level(30000.0f/M_FPS)==3);
 if(argc>1 && !strcmp(argv[1],"--allocation-failure")) {
  fail_alloc=1;
  ui_fblur_invalidate(&a,0,100);
  int spans[1][2];assert(!ui_fblur_row_spans(0,spans,1));
  for(int i=0;i<10;i++) tick(&a,32);
  ui_blur_stats stats;ui_fblur_stats(&stats);
  assert(stats.allocation_failures==1 && stats.bytes==0);
  assert(!ui_fblur_ready(&a,0,0));
  puts("UI blur cache: allocation failure preserves live fallback");return 0;
 }
 for(int i=0;i<30 && !ui_fblur_ready(&a,0,0);i++) tick(&a,32);
 assert(ui_fblur_ready(&a,0,0));
 uint16_t pixels[32*32];memset(pixels,0,sizeof pixels);
 gfx_fb fb;memset(&fb,0,sizeof fb);fb.px=pixels;fb.h=32;fb.stride=32;fb.cx1=32;
 assert(ui_fblur_draw_at(&fb,&a,0,0,0,1));assert(pixels[16*32+16]==0xf800);
 int spans[8][2];
 assert(ui_fblur_any_active());
 assert(ui_fblur_row_spans(16,spans,8)==1 && spans[0][0]==0 && spans[0][1]==32);
 assert(ui_fblur_row_spans(32,spans,8)==0);
 nos_frame++;
 assert(!ui_fblur_any_active());assert(!ui_fblur_row_spans(16,spans,8));
 int previous=ui_fblur_coverage_enable(0);
 assert(ui_fblur_draw_at(&fb,&a,0,0,0,1));assert(!ui_fblur_row_spans(16,spans,8));
 ui_fblur_coverage_enable(previous);
 fb.y_base=8;fb.h=8;fb.cx0=4;fb.cx1=24;
 assert(ui_fblur_draw_at(&fb,&a,0,0,0,1));
 assert(ui_fblur_row_spans(7,spans,8)==0);
 assert(ui_fblur_row_spans(8,spans,8)==1 && spans[0][0]==4 && spans[0][1]==24);
 assert(ui_fblur_row_spans(16,spans,8)==0);
 fb.y_base=0;fb.h=32;fb.cx0=0;fb.cx1=32;
 int before=draws;
 tick(&b,32);tick(&a,32);
 assert(ui_fblur_ready(&a,0,0));assert(draws-before<16);
 ink=0x001f;ui_fblur_invalidate(&a,0,0);assert(!ui_fblur_ready(&a,0,0));
 for(int i=0;i<30 && !ui_fblur_ready(&a,0,0);i++) tick(&a,32);
 assert(ui_fblur_ready(&a,0,0));
 assert(ui_fblur_draw_at(&fb,&a,0,0,0,1));assert(pixels[16*32+16]==0x001f);
 /* Warm idle data stays retained without permanent recording or filtering. */
 int idle_draws=draws;
 ui_blur_stats idle_before,idle_after;ui_fblur_stats(&idle_before);
 for(int i=0;i<30;i++) {
  nos_frame++;
  ui_fblur_tick(&a,0,0,0,0,1,32,0,32,0,row,0);
 }
 ui_fblur_stats(&idle_after);
 assert(draws==idle_draws && idle_before.work_pixels==idle_after.work_pixels);
 /* Sharp cache use is still validated lazily before replacing live content. */
 ink=0x07e0;nos_frame++;
 ui_fblur_tick(&a,0,0,0,0,1,32,0,32,0,row,0);
 assert(!ui_fblur_ready(&a,0,0));
 changing=1;
 for(int i=0;i<12;i++) tick(&c,64);
 assert(!ui_fblur_ready(&c,0,0));
 /* Shutter changes cannot drop a warm moving item back to temporal ghosts. */
 changing=0;ink=0xffff;static const char shutter=0;
 for(int i=0;i<50;i++) {
  nos_frame++;ui_fblur_tick(&shutter,1,6,0,0,1,128,0,32,0,row,0);
 }
 assert(ui_fblur_ready(&shutter,0,0));
 for(int i=0;i<30;i++) {
  nos_frame++;ui_fblur_tick(&shutter,1,i%2?6:60,0,0,1,128,0,32,0,row,0);
  assert(ui_fblur_ready(&shutter,0,0));
  assert(ui_fblur_draw_at(&fb,&shutter,0,0,0,1));
 }
 /* A cold visible band wins work over refining three usable published levels. */
 static const char priority=0;
 for(int i=0;i<40;i++) {nos_frame++;ui_fblur_tick(&priority,0,0,0,2,3,90,0,32,1,row,0);}
 for(int i=0;i<5;i++) {
  nos_frame++;ui_fblur_tick(&priority,1,6,0,3,4,90,0,32,1,row,0);
  assert(ui_fblur_ready(&priority,0,2));
 }
 assert(ui_fblur_ready(&priority,3,3));
 /* One cold neighbor preserves the warm band's directional pixels and coverage.
  * Each live pixel is incremented exactly once, including translated stripes. */
 static const char mixed=0;
 for(int i=0;i<20;i++) {nos_frame++;ui_fblur_tick(&mixed,1,6,0,0,1,32,0,32,0,row,0);}
 nos_frame++;ui_fblur_tick(&mixed,1,6,0,1,2,32,0,32,0,row,0);
 assert(ui_fblur_ready(&mixed,0,0) && !ui_fblur_ready(&mixed,1,1));
 uint16_t mixed_pixels[32*64+1];memset(mixed_pixels,0,sizeof mixed_pixels);mixed_pixels[32*64]=0x1234;
 gfx_fb mf={0};mf.px=mixed_pixels;mf.h=64;mf.stride=32;mf.cx1=32;
 assert(ui_fblur_draw_bands(&mf,&mixed,0,1,0,32,0,live_increment,0)==1);
 for(int y=32;y<64;y++) for(int x=0;x<32;x++) assert(mixed_pixels[y*32+x]==1);
 assert(mixed_pixels[16*32+16]==0xffff && mixed_pixels[32*64]==0x1234);
 assert(ui_fblur_row_spans(16,spans,8)==1 && !ui_fblur_row_spans(48,spans,8));
 gfx_rec rec={0};mf.rec=&rec;
 assert(ui_fblur_draw_bands(&mf,&mixed,0,1,0,32,0,live_increment,0)==1);
 assert(rec.n==2 && rec.c[0].y0==0 && rec.c[0].y1==32 && rec.c[1].y0==32 && rec.c[1].y1==64);
 mf.rec=0;mf.oy=5;mf.y_base=16;mf.h=24;nos_frame++;
 memset(mixed_pixels,0,sizeof mixed_pixels);
 assert(ui_fblur_draw_bands(&mf,&mixed,0,1,0,32,0,live_increment,0)==1);
 for(int y=21;y<24;y++) for(int x=0;x<32;x++) assert(mixed_pixels[y*32+x]==1);
 assert(!ui_fblur_row_spans(37,spans,8));
 /* Foreground exclusions subtract overlapping/touching rectangles exactly,
  * honor clipping and capacity, and cannot suppress foreground on overflow. */
 mf.oy=0;mf.y_base=0;mf.h=64;nos_frame++;
 assert(ui_fblur_draw_at(&mf,&mixed,0,0,0,1));
 ui_fblur_coverage_exclude(&mf,8,0,16,32);
 ui_fblur_coverage_exclude(&mf,12,0,20,32);
 ui_fblur_coverage_exclude(&mf,20,0,24,32);
 ui_fblur_coverage_exclude(&mf,-10,-10,4,50);
 assert(ui_fblur_row_spans(16,spans,8)==2);
 assert(spans[0][0]==4 && spans[0][1]==8 && spans[1][0]==24 && spans[1][1]==32);
 assert(ui_fblur_row_spans(16,spans,1)==1 && spans[0][0]==4 && spans[0][1]==8);
 assert(!ui_fblur_row_spans(16,spans,0));
 nos_frame++;assert(ui_fblur_draw_at(&mf,&mixed,0,0,0,1));
 ui_fblur_coverage_exclude(&mf,8,-10,24,4);
 assert(ui_fblur_row_spans(0,spans,8)==2 && ui_fblur_row_spans(8,spans,8)==1);
 /* A moving foreground's changing box remains exposed, cached neighbors stay claimed. */
 for(int x=4;x<=20;x+=4) {
  nos_frame++;assert(ui_fblur_draw_at(&mf,&mixed,0,0,0,1));
  ui_fblur_coverage_exclude(&mf,x,10,x+6,20);
  assert(ui_fblur_row_spans(15,spans,8)==2 && spans[0][1]==x && spans[1][0]==x+6);
 }
 nos_frame++;assert(ui_fblur_draw_at(&mf,&mixed,0,0,0,1));
 mf.rec=&rec;ui_fblur_coverage_exclude(&mf,0,0,32,32);mf.rec=0;
 previous=ui_fblur_coverage_enable(0);ui_fblur_coverage_exclude(&mf,0,0,32,32);ui_fblur_coverage_enable(previous);
 assert(ui_fblur_row_spans(15,spans,8)==1);
 for(int i=0;i<33;i++) {int x=(i%16)*2,y=(i/16)*2;ui_fblur_coverage_exclude(&mf,x,y,x+1,y+1);}
 assert(!ui_fblur_row_spans(15,spans,8));
 assert(ui_fblur_draw_at(&mf,&mixed,0,0,0,1));assert(!ui_fblur_row_spans(15,spans,8));
 /* Two visible pages plus two speculative pages exceed the pool. The nearest
  * prefetch remains pinned; warm idle must not evict/rebake it forever. */
 changing=0;ink=0xffff;static const char pages=0;
 for(int i=0;i<120;i++) {
  nos_frame++;ui_fblur_tick(&pages,0,0,0,1,4,466,0,466,2,row,0);
 }
 ui_blur_stats pressure_before,pressure_after;ui_fblur_stats(&pressure_before);
 for(int i=0;i<30;i++) {
  nos_frame++;ui_fblur_tick(&pages,0,0,0,1,4,466,0,466,2,row,0);
 }
 ui_fblur_stats(&pressure_after);
 assert(pressure_before.evictions==pressure_after.evictions);
 assert(pressure_before.work_pixels==pressure_after.work_pixels);
 /* Specialized blur coverage works without allocating generic pixel caches. */
 ui_fblur_release();nos_frame++;
 gfx_fb external;memset(&external,0,sizeof external);
 external.h=20;external.y_base=10;external.ox=3;external.oy=4;
 ui_fblur_coverage_claim(&external,2,8,12,18);
 assert(ui_fblur_any_active());
 int external_spans[4][2];
 assert(ui_fblur_row_spans(14,external_spans,4)==1);
 assert(external_spans[0][0]==5 && external_spans[0][1]==15);
 ui_fblur_stats(&pressure_after);assert(pressure_after.bytes==32768);
 ui_fblur_release();assert(!ui_fblur_any_active());
 nos_frame++;tick(&a,32);ui_fblur_stats(&pressure_after);
 assert(pressure_after.bytes==1024u*1024u);
 static const nos_screen native_screen={.flags=SCR_NATIVE_CACHE};
 test_screen=&native_screen;
 ui_fblur_coverage_claim(&external,2,8,12,18);
 ui_fblur_idle();ui_fblur_stats(&pressure_after);
 assert(pressure_after.bytes==32768 && ui_fblur_any_active());
 assert(ui_fblur_row_spans(14,external_spans,4)==1);
 test_screen=0;tick(&a,32);ui_fblur_stats(&pressure_after);
 assert(pressure_after.bytes==1024u*1024u);

 coverage_reference();
 native_observation_reference();
 puts("UI blur cache: color, held motion, retained owner, invalidation, atomic capture, idle bounds passed");
 return 0;
}
