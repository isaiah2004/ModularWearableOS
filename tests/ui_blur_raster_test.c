/* Raster reference independent of incremental capture, sliding sums and damage.
 * Uses real alpha glyphs + colored controls, then compares every RGB565 pixel. */
#include "../nos/ui_blur.c"
#include "font.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
uint32_t nos_frame;
int nos_mblur=2;double nos_t;
uint32_t hal_micros(void){return 0;}
void *hal_big_alloc(uint32_t n){return calloc(1,n);}
const nos_screen *nos_top(void) {return 0;}
int nos_transitioning(void){return 0;}
void nos_ink_dim(gfx_fb *fb,float k){fb->dim=32-(int)((32-fb->dim)*k+0.5f);}
#define W 440
#define H 90
#define C 24
#define X 13
static const char owner=0;
static void body_row(gfx_fb *fb,int i,int y) {
 const char *label=i?"Always-on display":"Brightness";
 font_draw(fb,&font_small,X+24,y+35,label,0xffff);
 font_draw(fb,&font_small,X+290,y+35,i?"ON":"AUTO 70%",0x7bef);
 gfx_fill_pill(fb,X+340,y+65,X+378,11,0xf800);
 gfx_fill_circle(fb,X+375,y+65,8,0xffff);
 font_draw(fb,&font_small,X+24,y+72,"COLOR AND GLYPH REFERENCE",0x07e0);
}
static void row(gfx_fb *fb,int i,int x0,int y,int x1,int h,void *ctx) {
 (void)x0;(void)x1;(void)h;(void)ctx;body_row(fb,i,y);
}
static void native_body(gfx_fb *fb,void *ctx) {
 (void)ctx;
 for(int i=0;i<2;i++) {
  int y=i*H;
  if(y+H+32+fb->oy<fb->y_base) continue;
  if(y-32+fb->oy>=fb->y_base+fb->h) break;
  body_row(fb,i,y);
 }
}
static uint16_t naive(const uint16_t *src,int x,int y,int length) {
 unsigned r=0,g=0,b=0;
 for(int k=0;k<length;k++) {
  int sy=y+C-length/2+k;
  if(sy<0 || sy>=H+2*C) continue;
  unsigned p=src[sy*W+x];r+=p>>11;g+=(p>>5)&63;b+=p&31;
 }
 return (uint16_t)(((r+length/2)/length)<<11|((g+length/2)/length)<<5|(b+length/2)/length);
}
int main(void) {
 for(int k=0;k<30;k++) {
  nos_frame++;nos_t=nos_frame/40.0f;
  ui_fblur_tick(&owner,1,1350.0f/M_FPS,0,0,1,H,X,X+W,1,row,0);
 }
 assert(ui_fblur_ready(&owner,0,0));blur_item *s=fb_find(0);assert(s && s->level==3);
 uint16_t reference[W*(H+2*C)];memset(reference,0,sizeof reference);
 gfx_fb rf={0};rf.px=reference;rf.h=H+2*C;rf.y_base=UI_CY-H/2-C;rf.stride=W;rf.x_base=X;rf.cx0=X;rf.cx1=X+W;
 body_row(&rf,0,UI_CY-H/2); /* One complete raster, not the sliced callback path. */
 assert(!memcmp(reference,s->sharp,sizeof reference));
 unsigned label_ink=0;
 for(int y=0;y<H;y++) for(int x=0;x<W;x++) {
  uint16_t expected=naive(reference,x,y,16);
  assert(s->blur[y*W+x]==expected);
  if(x>=24 && x<220 && y>=15 && y<48) label_ink+=expected!=0;
 }
 assert(label_ink>100); /* Real left-label strokes survive directional filtering. */
 nos_frame++;ui_fblur_tick(&owner,1,1350.0f/M_FPS,0,1,2,H,X,X+W,1,row,0);
 assert(ui_fblur_ready(&owner,0,0) && !ui_fblur_ready(&owner,1,1));
 uint16_t actual[GFX_W*H*2],expected[GFX_W*H*2];
 memset(actual,0,sizeof actual);memset(expected,0,sizeof expected);
 gfx_fb fb={0};fb.px=actual;fb.h=H*2;fb.cx1=GFX_W;
 assert(ui_fblur_draw_bands(&fb,&owner,0,1,0,H,0,native_body,0)==1);
 for(int y=0;y<H;y++) for(int x=0;x<W;x++) expected[y*GFX_W+X+x]=naive(reference,x,y,16);
 fb.px=expected;body_row(&fb,1,H);
 assert(!memcmp(actual,expected,sizeof actual));
 printf("UI raster: full capture/naive16px blur/mixed native glyph+control reference exact, left_label_pixels=%u\n",label_ink);
 return 0;
}
