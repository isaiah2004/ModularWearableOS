#include "../nos/render_history.c"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static int fail_alloc;
void *hal_big_alloc(uint32_t n){return fail_alloc?NULL:calloc(1,n);}
static uint16_t row[GFX_W],out[2][14][GFX_W],logical[2][14][GFX_W];
static uint32_t valid[2][14][HISTORY_WORDS],seen[2][14][HISTORY_WORDS];
static void snapshot(int pass,int step){memcpy(out[pass][step],row,sizeof row);for(int x=0;x<GFX_W;x++)logical[pass][step][x]=((s_uniform[10]>>(2*(x/32)))&3)==3?0:s_sharp[10*GFX_W+x];memcpy(valid[pass][step],s_valid+10*HISTORY_WORDS,sizeof valid[0][0]);memcpy(seen[pass][step],s_seen+10*HISTORY_WORDS,sizeof seen[0][0]);assert(s_stats.pixels==s_stats.seeded+s_stats.unchanged+s_stats.changed+s_stats.copied);}
int main(void){assert(!nos_history_virtual_get());nos_history_virtual_set(1);assert(nos_history_virtual_get());nos_history_virtual_set(0);fail_alloc=1;assert(!nos_history_init());nos_history_virtual_set(0);nos_history_materialize();fail_alloc=0;assert(nos_history_init());
 for(int pass=0;pass<2;pass++){
  nos_history_reset();memset(s_sharp,0,HISTORY_BYTES);memset(s_seen,0,HISTORY_BITS);nos_history_virtual_set(pass);nos_history_uniform=1;nos_history_native_copy=1;
  gfx_fb f={0};f.px=row;f.h=1;f.y_base=10;f.cx1=GFX_W;
  for(int step=0;step<14;step++){
   nos_history_begin(step);
   if(step==0){for(int x=0;x<GFX_W;x++)row[x]=0xffff;nos_history_apply(&f,0,10,GFX_W,11,0);}
   if(step==1){memset(row,0,sizeof row);nos_history_apply_disjoint(&f,0,10,96,11,0);if(pass){assert(s_uniform[10]%64==63);}}
   if(step==2){for(int x=0;x<GFX_W;x++)row[x]=0x1234;nos_history_apply(&f,5,10,11,11,1);}
   if(step==3){for(int x=0;x<GFX_W;x++)row[x]=0xabcd;nos_history_apply_disjoint(&f,40,10,43,11,0);}
   if(step==4){nos_history_uniform=0;nos_history_native_copy=0;for(int x=0;x<GFX_W;x++)row[x]=0xa55a;nos_history_apply_disjoint(&f,100,10,105,11,1);assert(!s_uniform[10]);}
   if(step==5){nos_history_virtual_set(0);nos_history_uniform=1;nos_history_native_copy=1;memset(row,0,sizeof row);nos_history_apply_disjoint(&f,0,10,GFX_W,11,0);}
   if(step==6){nos_history_virtual_set(pass);memset(row,0,sizeof row);nos_history_apply_disjoint(&f,0,10,GFX_W,11,0);}
   if(step==7){for(int x=0;x<GFX_W;x++)row[x]=(uint16_t)(x*193);nos_history_apply(&f,0,10,GFX_W,11,1);}
   if(step==8){nos_history_reset();nos_history_begin(step);for(int x=0;x<GFX_W;x++)row[x]=0x42a1;nos_history_apply_disjoint(&f,0,10,GFX_W,11,1);assert(s_stats.seeded==GFX_W);for(int x=0;x<GFX_W;x++)assert(row[x]==0x42a1);}
   if(step==9){nos_history_reset();nos_history_begin(step);for(int x=0;x<GFX_W;x++)row[x]=0xa5a5;nos_history_apply_disjoint(&f,3,10,25,11,0);}
   if(step==10){memset(row,0,sizeof row);nos_history_apply_disjoint(&f,0,10,96,11,0);}
   if(step==11){for(int x=0;x<GFX_W;x++)row[x]=0x9f35;nos_history_apply(&f,1,10,10,11,1);nos_history_apply(&f,1,10,10,11,1);assert(s_stats.duplicates==9);}
   if(step==12){memset(row,0,sizeof row);nos_history_apply_disjoint(&f,0,10,GFX_W,11,0);nos_history_materialize();for(int x=0;x<GFX_W;x++)assert(s_sharp[10*GFX_W+x]==0);}
   if(step==13){memset(row,0,sizeof row);nos_history_apply_disjoint(&f,0,10,GFX_W,11,0);nos_history_virtual_set(0);for(int x=0;x<GFX_W;x++)assert(s_sharp[10*GFX_W+x]==0);assert(!nos_history_virtual_get());}
   snapshot(pass,step);
  }
 }
 assert(!memcmp(out[0],out[1],sizeof out[0]));assert(!memcmp(logical[0],logical[1],sizeof logical[0]));assert(!memcmp(valid[0],valid[1],sizeof valid[0]));assert(!memcmp(seen[0],seen[1],sizeof seen[0]));
 const nos_history_virtual_stats *stats=nos_history_virtual_get_stats();
 assert(stats->virtual_pixels && stats->avoided_bytes==2*stats->virtual_pixels && stats->materialize_bytes && stats->classify_pixels>=stats->virtual_pixels);
 nos_history_virtual_stats before=*stats;nos_history_reset();assert(!memcmp(&before,stats,sizeof before));
 puts("PASS adversarial virtual black: stale-white source, partial generic/native handoff, uniform0 whole-row clearing, runtime off/on, full live overlay, reset/cold holes, duplicates, raw materialization, toggle-off draining, allocation failure");
}
