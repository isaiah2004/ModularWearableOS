
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "render_history.h"
void *hal_big_alloc(uint32_t n){return malloc(n);}
static uint16_t pixels[GFX_W*GFX_H+3];
static unsigned rng;
static unsigned next(void){rng=rng*1664525u+1013904223u;return rng;}
static uint64_t hashes[160];
static nos_history_stats reference_stats[160];
static uint64_t hash(void){uint64_t h=1;for(unsigned i=0;i<sizeof pixels/sizeof pixels[0];i++)h=(h^pixels[i])*1099511628211ULL;return h;}
int main(void){
 assert(nos_history_init());
 unsigned copied_total=0, generic_clears=0, fast_clears=0;
 for(int pass=0;pass<2;pass++) {
  nos_history_reset();rng=17;
  for(int frame=0;frame<160;frame++) {
   nos_history_begin(frame);
   for(unsigned i=0;i<sizeof pixels/sizeof pixels[0];i++)pixels[i]=(uint16_t)next();
   /* Packed odd stride and halfword-misaligned origins alternate with full rows. */
   int packed=frame%3==0, width=packed?39:GFX_W;
   gfx_fb f={0};f.px=pixels+(frame&1);f.stride=width;f.x_base=packed?17:0;
   f.cx0=f.x_base;f.cx1=f.x_base+width;f.h=GFX_H;f.y_base=0;
   for(int y=0;y<GFX_H;y+=9) {
    int h=GFX_H-y<9?GFX_H-y:9;
    int x=f.cx0+((frame+y)%5),end=f.cx1-((frame+y)%7);
    int enabled=frame>=80 || (frame+y)%4!=0;
    if(pass)nos_history_apply_disjoint(&f,x,y,end,y+h,enabled);
    else nos_history_apply(&f,x,y,end,y+h,enabled);
   }
   const nos_history_stats *s=nos_history_get_stats();
   assert(s->pixels==s->seeded+s->changed+s->unchanged+s->copied);
   assert(!s->duplicates);
   if(!pass) {hashes[frame]=hash();reference_stats[frame]=*s;generic_clears+=s->rows_cleared;}
   else {
    copied_total+=s->copied;fast_clears+=s->rows_cleared;
    const nos_history_stats *r=&reference_stats[frame];
    assert(hashes[frame]==hash());
    assert(s->pixels==r->pixels && s->seeded==r->seeded && s->blended==r->blended);
    if(!s->copied) {
     assert(s->changed==r->changed && s->unchanged==r->unchanged);
     assert(s->equal_words==r->equal_words);
    }
   }
  }
 }
 assert(copied_total>0 && fast_clears<generic_clears);
 puts("history disjoint: 160 random cold/warm/hole/odd origin+stride frames and exact eligible stats; copy classification honest");
}
