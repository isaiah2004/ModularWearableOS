#include "../nos/render_history.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
void *hal_big_alloc(uint32_t n){return calloc(1,n);}
static uint16_t px[GFX_W*8+32];
static uint32_t rng=79;
static unsigned rand32(void){rng=rng*1664525u+1013904223u;return rng;}
static unsigned long long physical_hash(const void *p,size_t n){const unsigned char *b=p;unsigned long long h=17;while(n--)h=(h^*b++)*1099511628211ULL;return h;}
static unsigned long long logical_hash(void){
 unsigned long long h=17;for(int y=0;y<GFX_H;y++)for(int x=0;x<GFX_W;x++){
  uint16_t v=((s_uniform[y]>>(2*(x/32)))&3u)==3u?0:s_sharp[y*GFX_W+x];
  h=(h^(v&255))*1099511628211ULL;h=(h^(v>>8))*1099511628211ULL;
 }return h;
}
int main(void){assert(nos_history_init());memset(s_sharp,0,HISTORY_BYTES);memset(s_seen,0,HISTORY_BITS);
 for(int frame=0;frame<600;frame++){
  if(frame%71==0)nos_history_reset();
#ifdef CANDIDATE
  nos_history_virtual_set(frame%9!=0);
#else
  nos_history_virtual_set(0);
#endif
  nos_history_native_copy=frame%7!=0;
  nos_history_uniform=frame%11!=0;nos_history_cadence=frame%3==0;
  nos_history_begin_interval(frame,8000+rand32()%40000);
  for(int y=0;y<8;y++){
   int base=frame%6, width=GFX_W-base-(frame%7),yy=y+37;
   gfx_fb f={0};f.px=px;f.h=1;f.stride=width;f.x_base=base;f.y_base=yy;f.cx0=base;f.cx1=base+width;
   for(int x=0;x<width;x++){
    int b=(x+base)/32;unsigned q=(b+y+(frame/9))%7;
    px[x]=q<3?0:q<5?65535:q==5?(uint16_t)(rand32()>>9):(uint16_t)(b*193+17);
   }
   int x=base;
   while(x<base+width){int n=frame%5==0?1+(int)(rand32()%67):width;int end=x+n;if(end>base+width)end=base+width;
    int enabled=(frame+y)%7==0;
    if(frame%13==0)nos_history_apply(&f,x,yy,end,yy+1,enabled);
    else nos_history_apply_disjoint(&f,x,yy,end,yy+1,enabled);
    assert(s_stats.pixels==s_stats.seeded+s_stats.unchanged+s_stats.changed+s_stats.copied);
    x=end;
   }
  }
  /* Deliberately violate disjoint mixing once to require baseline equivalence;
   * this does not broaden the disjoint API contract. Generic-only overlap is
   * independently tested by the supported adversarial suite. */
  if(frame%17==0){gfx_fb f={0};f.px=px;f.h=1;f.y_base=40;f.cx1=GFX_W;nos_history_apply(&f,17,40,90,41,0);}
  printf("%d %llx %llx %llx %llx %llx %llx %llx %llx\n",frame,physical_hash(px,sizeof px),logical_hash(),physical_hash(s_valid,HISTORY_BITS),physical_hash(s_seen,HISTORY_BITS),physical_hash(s_valid_lo,sizeof s_valid_lo),physical_hash(s_valid_hi,sizeof s_valid_hi),(unsigned long long)s_stats.pixels,(unsigned long long)s_stats.blended);
 }
}
