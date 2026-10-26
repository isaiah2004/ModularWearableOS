
#include "../nos/render_history.c"
#include <assert.h>
#include <stdio.h>
void *hal_big_alloc(uint32_t n){return malloc(n);}
static uint16_t pixels[GFX_W*GFX_H];
static uint64_t sums[16];
static uint64_t hash(void){uint64_t v=17;for(int i=0;i<GFX_W*GFX_H;i++)v=(v^pixels[i])*1099511628211ULL;return v;}
int main(void){
 nos_history_native_copy=0; /* Retain every legacy uniform-shortcut assertion. */
 assert(nos_history_init());unsigned shortcuts=0;
 for(int pass=0;pass<2;pass++) {
  nos_history_reset();
  for(int frame=0;frame<16;frame++) {
   for(int y=0;y<GFX_H;y++)for(int x=0;x<GFX_W;x++)
    pixels[y*GFX_W+x]=frame>=7?0xffff:((x/32+y)&1)?0:0xffff;
   if(frame==4) pixels[5]=0x1234;
   nos_history_begin(frame);
   gfx_fb f={0};f.px=pixels;f.h=GFX_H;f.cx1=GFX_W;
   int enabled=frame<12;
   nos_history_uniform=frame!=6;
   if(pass) {
    if(frame==4) {
     /* Disjoint generic partial write invalidates exactly its touched block. */
     nos_history_apply(&f,5,0,6,1,enabled);
     assert(!(s_uniform[0]&3u));
     nos_history_apply_disjoint(&f,0,0,5,1,enabled);
     nos_history_apply_disjoint(&f,6,0,GFX_W,1,enabled);
     nos_history_apply_disjoint(&f,0,1,GFX_W,GFX_H,enabled);
    } else nos_history_apply_disjoint(&f,0,0,GFX_W,GFX_H,enabled);
   } else nos_history_apply(&f,0,0,GFX_W,GFX_H,enabled);
   const nos_history_stats *s=nos_history_get_stats();
   assert(s->pixels==GFX_W*GFX_H);
   assert(s->uniform_read_bytes+s->uniform_write_bytes==2u*s->uniform_pixels);
   if(enabled)assert(!s->uniform_write_bytes);else assert(!s->uniform_read_bytes);
   if(!nos_history_uniform)assert(!s->uniform_pixels);
   assert(s->pixels==s->seeded+s->unchanged+s->changed+s->copied);
   if(pass) {assert(hash()==sums[frame]);shortcuts+=s->uniform_pixels;}
   else sums[frame]=hash();
   if(pass && (frame==2 || frame==3 || frame==10 || frame==13))
    assert(s->uniform_pixels==GFX_W*GFX_H && s->unchanged==GFX_W*GFX_H);
  }
 }
 assert(shortcuts>GFX_W*GFX_H*6u);
 printf("PASS solid black/white, final18px, partial invalidation, generic mix, transition and covered-copy: shortcut_pixels=%u avoided_history_read_bytes_at_least=%u\n",shortcuts,2u*GFX_W*GFX_H);
}
