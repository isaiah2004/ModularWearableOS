
#include "../nos/apps/native_tile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
int nos_native_row_stage_mode;
uint8_t nos_native_row_stage_scratch[512] __attribute__((aligned(4)));
void *hal_big_alloc(uint32_t n){return malloc(n);}
static unsigned claims;
void ui_fblur_observe_native(const gfx_fb *fb,int x0,int y0,int x1,int y1,int blurred)
{ assert(fb && !fb->rec && x1>x0 && y1>y0 && (blurred==0 || blurred==1)); }
void ui_fblur_coverage_claim(const gfx_fb *fb,int x0,int y0,int x1,int y1)
{ (void)fb; assert(x1>x0 && y1>y0);claims++; }
static unsigned seed=9;
static unsigned rnd(void){seed=seed*1664525u+1013904223u;return seed;}
static double sample(const sprite8 *a,const sprite8 *b,int mix,int x,int y){
 if(y<0 || y>=a->h)return 0;
 unsigned v=a->px[y*a->w+x];
 return b?(v*(64-mix)+b->px[y*a->w+x]*mix+32)/64:v;
}
static void check_directional_trail(void){
 uint8_t impulse[257]={0},flat[257];impulse[128]=255;memset(flat,173,sizeof flat);
 const int widths[]={15,30,60,90,120};
 for(unsigned k=0;k<5;k++){
  int len=widths[k],support=(len>90?90:len)/2;unsigned peak=0;
  for(int age=-3;age<=support+1;age++){
   unsigned down=native_trail_pixel(impulse,1,257,0,128-age,len,1);
   unsigned up=native_trail_pixel(impulse,1,257,0,128+age,len,-1);
   assert(down==up);if(down>peak)peak=down;
   if(age==-3||age==support+1)assert(down==0);
  }
  assert(peak>0&&peak<255);
  for(int direction=-1;direction<=1;direction+=2)
   for(int y=support;y<257-support;y++)assert(native_trail_pixel(flat,1,257,0,y,len,direction)==173);
 }
 int low=0,high=1;native_direction_levels(-1,&low,&high);assert(low==0 && high==5);
 low=2;high=3;native_direction_levels(-1,&low,&high);assert(low==6 && high==7);
 low=2;high=3;native_direction_levels(1,&low,&high);assert(low==2 && high==3);
 puts("PASS directional trail: reversed impulses, normalized continuous tail, stable flat field, signed levels");
}
static void check_halo_overlap(void) {
 uint8_t px[2]={200,0};sprite8 s={px,1,2};uint16_t lut[256];
 for(int i=0;i<256;i++)lut[i]=(uint16_t)(((i>>3)<<11)|((i>>2)<<5)|(i>>3));
 uint16_t out[8]={0};gfx_fb fb={0};fb.px=out;fb.stride=1;fb.h=8;fb.cx1=1;
 native_prepared_blit(&fb,&s,0,2,NULL,0,2,0,lut,1,0,0,8,2,NULL,0);
 uint16_t kept=out[2];assert(kept==lut[200]);
 native_prepared_blit(&fb,&s,0,1,NULL,0,1,0,lut,2,0,0,8,2,NULL,0);
 assert(out[1]==lut[200] && out[2]==kept);
 assert(native_halo_merge(0xffff,0,0)==0);
 assert(native_halo_merge(0xf800,0x07e0,1)==0xffe0);
 puts("PASS overlapping halos preserve adjacent ink");
}
int main(void){
 check_halo_overlap();
 check_directional_trail();
 uint8_t p[47*61],q[47*61];uint16_t palette[256];
 for(int i=0;i<256;i++)palette[i]=(uint16_t)i;
 for(unsigned i=0;i<sizeof p;i++){p[i]=rnd()>>24;q[i]=rnd()>>24;}
 sprite8 a={p,47,61},b={q,47,61};
 unsigned checked=0;
 for(int trial=0;trial<1600;trial++){
  int pose=(int)(rnd()%513)-256,exposure=rnd()%12289,mix=rnd()%65;
  int stride=80,xbase=20,originx=32,originy=13;
  int ybase=(int)(rnd()%100)-20,height=1+rnd()%31;
  uint16_t storage[80*31+2];for(unsigned i=0;i<sizeof storage/sizeof *storage;i++)storage[i]=0xface;
  gfx_fb fb={0};fb.px=storage+1;fb.stride=stride;fb.x_base=xbase;
  fb.y_base=ybase;fb.h=height;fb.cx0=xbase+3;fb.cx1=xbase+stride-4;
  fb.ox=(int)(rnd()%9)-4;fb.oy=(int)(rnd()%9)-4;
  const sprite8 *other=trial%2?&b:NULL;
  native_tile_blit(&fb,&a,other,mix,originx,originy,palette,17,pose,exposure,0,93);
  int width=exposure<256?256:exposure,halo=(width+511)/512+1;
  for(int row=0;row<height;row++)for(int col=0;col<stride;col++){
   int screenx=xbase+col,logicalx=screenx-fb.ox-originx;
   int logicaly=ybase+row-fb.oy-originy;
   int visible=screenx>=fb.cx0 && screenx<fb.cx1 && logicalx>=0 && logicalx<a.w &&
    logicaly>=-halo && logicaly<a.h+halo && logicaly+originy>=0 && logicaly+originy<93;
   uint16_t actual=storage[1+row*stride+col];
   if(!visible){assert(actual==0xface);continue;}
   double left=logicaly-pose/256.0-width/512.0,right=left+width/256.0,total=0;
   for(int sy=0;sy<a.h;sy++){
    double lo=left>sy-.5?left:sy-.5,hi=right<sy+.5?right:sy+.5;
    if(hi>lo)total+=(hi-lo)*sample(&a,other,mix,logicalx,sy);
   }
   int expected=(int)(total/(width/256.0)+.5);
   assert(abs((int)actual-expected)<=1);checked++;
  }
  assert(storage[0]==0xface && storage[80*31+1]==0xface);
 }

 unsigned prepared=0;
 for(int trial=0;trial<800;trial++){
  uint16_t output[91*91];for(unsigned k=0;k<sizeof output/sizeof *output;k++)output[k]=0xface;
  gfx_fb fb={0};fb.px=output;fb.stride=91;fb.x_base=17;fb.h=91;fb.y_base=-9;fb.cx0=21;fb.cx1=103;
  int pose=(int)(rnd()%513)-256,ax=24,ay=3,bx=27,by=-4;
  unsigned mix=rnd()%257;
  native_prepared_blit(&fb,&a,ax,ay,&b,bx,by,mix,palette,71,pose,-1,74,1,NULL,0);
  int shift=(int)floor(-pose/256.0);double phase=-pose/256.0-shift;
  int x0=mix?(ax<bx?ax:bx):ax,x1=mix?(ax+a.w>bx+b.w?ax+a.w:bx+b.w):ax+a.w;
  int y0=mix?(ay<by?ay:by)-1:ay-1,y1=mix?(ay+a.h>by+b.h?ay+a.h:by+b.h)+1:ay+a.h+1;
  if(y0< -1)y0=-1;if(y1>74)y1=74;
  for(int row=0;row<91;row++)for(int col=0;col<91;col++){
   int x=17+col,y=-9+row;uint16_t actual=output[row*91+col];
   if(x<x0 || x>=x1 || x<21 || x>=103 || y<y0 || y>=y1){assert(actual==0xface);continue;}
   double va=0,vb=0;
   if(x>=ax && x<ax+a.w)va=sample(&a,NULL,0,x-ax,y+shift-ay)*(1-phase)+sample(&a,NULL,0,x-ax,y+shift+1-ay)*phase;
   if(x>=bx && x<bx+b.w)vb=sample(&b,NULL,0,x-bx,y+shift-by)*(1-phase)+sample(&b,NULL,0,x-bx,y+shift+1-by)*phase;
   int expected=(int)((va*(256-mix)+vb*mix)/256+.5);
   if(actual!=expected){printf("prepared mismatch trial%d mix%u pose%d x%d y%d actual%u expected%d va%.9f vb%.9f\n",trial,mix,pose,x,y,actual,expected,va,vb);return 2;}prepared++;
  }
 }
 unsigned variants=0;
 for(int trial=0;trial<4000;trial++){
  uint16_t output[91*91];for(unsigned k=0;k<sizeof output/sizeof *output;k++)output[k]=0xface;
  gfx_fb fb={0};fb.px=output;fb.stride=91;fb.x_base=17;fb.h=91;fb.y_base=-9;fb.cx0=21;fb.cx1=103;
  int pose=(int)(rnd()%513)-256,ax=24,ay=3,bx=27,by=-4;
  unsigned mix=rnd()%257,vmix=rnd()%65; a.w=1+rnd()%47;a.h=1+rnd()%61;b.w=1+rnd()%47;b.h=1+rnd()%61; sprite8 variant={q,a.w,a.h};
  native_prepared_blit(&fb,&a,ax,ay,&b,bx,by,mix,palette,71,pose,-1,74,1,&variant,vmix);
  int shift=(int)floor(-pose/256.0);double phase=-pose/256.0-shift;
  int x0=mix?(ax<bx?ax:bx):ax,x1=mix?(ax+a.w>bx+b.w?ax+a.w:bx+b.w):ax+a.w;
  int y0=mix?(ay<by?ay:by)-1:ay-1,y1=mix?(ay+a.h>by+b.h?ay+a.h:by+b.h)+1:ay+a.h+1;
  if(y0< -1)y0=-1;if(y1>74)y1=74;
  for(int row=0;row<91;row++)for(int col=0;col<91;col++){
   int x=17+col,y=-9+row;uint16_t actual=output[row*91+col];
   if(x<x0 || x>=x1 || x<21 || x>=103 || y<y0 || y>=y1){assert(actual==0xface);continue;}
   double va=0,vb=0;
   if(x>=ax && x<ax+a.w)va=sample(&a,&variant,vmix,x-ax,y+shift-ay)*(1-phase)+sample(&a,&variant,vmix,x-ax,y+shift+1-ay)*phase;
   if(x>=bx && x<bx+b.w)vb=sample(&b,NULL,0,x-bx,y+shift-by)*(1-phase)+sample(&b,NULL,0,x-bx,y+shift+1-by)*phase;
   int expected=(int)((va*(256-mix)+vb*mix)/256+.5);
   if(actual!=expected){printf("prepared mismatch trial%d mix%u pose%d x%d y%d actual%u expected%d va%.9f vb%.9f\n",trial,mix,pose,x,y,actual,expected,va,vb);return 2;}variants++;
  }
 }
 printf("PASS prepared variants: %u exact independent nested-rounding fractional/mixed/cropped pixels\n",variants);
 printf("PASS prepared tiles: %u exact independent fractional/mixed/cropped pixels\n",prepared);
 printf("PASS native tiles: %u independent fractional-box pixels, packed/offset clips, halos, mixes and guards; claims%u\n",checked,claims);
 return 0;
}
