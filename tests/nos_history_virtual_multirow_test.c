#include "../nos/render_history.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
void *hal_big_alloc(uint32_t n){return calloc(1,n);}
static uint16_t pixels[3*467+2],reference[3*467+2],history[3*GFX_W];
int main(void){assert(nos_history_init());
 for(int pass=0;pass<2;pass++){
  nos_history_reset();memset(s_sharp,0,HISTORY_BYTES);nos_history_virtual_set(pass);nos_history_native_copy=1;nos_history_uniform=1;
  for(unsigned i=0;i<sizeof pixels/sizeof *pixels;i++)pixels[i]=0xFACE;
  gfx_fb f={0};f.px=pixels+1;f.stride=467;f.h=3;f.y_base=10;f.cx1=GFX_W;
  for(int y=0;y<3;y++)for(int x=0;x<GFX_W;x++)pixels[1+y*467+x]=0xffff;
  nos_history_begin(1);nos_history_apply(&f,0,10,GFX_W,13,0);
  for(int y=0;y<3;y++)for(int x=0;x<GFX_W;x++)pixels[1+y*467+x]=0;
  nos_history_begin(2);nos_history_apply_disjoint(&f,0,10,GFX_W,13,0);
  if(pass)for(int y=10;y<13;y++)assert(((s_uniform[y]>>28)&3)==3);
  for(int x=459;x<464;x++)pixels[1+467+x]=0x1357;
  nos_history_begin(3);nos_history_apply(&f,459,11,464,12,1);
  for(int x=449;x<452;x++)pixels[1+2*467+x]=0x0001;
  nos_history_begin(4);nos_history_apply_disjoint(&f,449,12,452,13,0);
  nos_history_uniform=0;nos_history_native_copy=0;
  nos_history_begin(5);nos_history_apply_disjoint(&f,11,10,15,11,1);
  assert(!s_uniform[10]);
  uint16_t before[3*467+2];memcpy(before,pixels,sizeof pixels);nos_history_materialize();assert(!memcmp(before,pixels,sizeof pixels));
  assert(pixels[0]==0xFACE && pixels[3*467+1]==0xFACE);for(int y=0;y<3;y++)assert(pixels[1+y*467+466]==0xFACE);
  if(!pass){memcpy(reference,pixels,sizeof pixels);memcpy(history,s_sharp+10*GFX_W,sizeof history);}else{assert(!memcmp(reference,pixels,sizeof pixels));assert(!memcmp(history,s_sharp+10*GFX_W,sizeof history));}
 }
 puts("PASS virtual odd467stride:3rows alternating alignment, last18px virtualblock, partialgeneric/nativehandoff, whole-row uniformoff, physical materialization and guards");
}
