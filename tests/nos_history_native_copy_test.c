#include "../nos/render_history.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
void *hal_big_alloc(uint32_t n){return calloc(1,n);}
static uint16_t px[GFX_W*8+32];
static uint32_t rng=79;
static unsigned rand32(void){rng=rng*1664525u+1013904223u;return rng;}
static unsigned long long hash(const void *p,size_t n){const unsigned char *b=p;unsigned long long h=17;while(n--)h=(h^*b++)*1099511628211ULL;return h;}
static unsigned long long reference[600][10];
static void exact_tag_case(void){
 gfx_fb f={0};f.px=px;f.h=1;f.y_base=37;f.cx1=GFX_W;
 memset(px,0,sizeof px);nos_history_reset();nos_history_uniform=1;
 nos_history_native_copy=0;
 for(int frame=0;frame<3;frame++){nos_history_begin(frame);nos_history_apply_disjoint(&f,0,37,GFX_W,38,0);}
 assert(s_stats.uniform_pixels==GFX_W && s_stats.copied==0);
 uint32_t before=s_uniform[37];nos_history_native_copy=1;nos_history_begin(3);
 nos_history_apply_disjoint(&f,37,37,91,38,0);
 assert(s_stats.copied==54 && s_stats.uniform_pixels==0);
 assert(s_uniform[37]==(before&~((3u<<2)|(3u<<4))));
 /* Enabled spans retain the legacy uniform read-skip optimization even
  * while native copying is selected globally. */
 nos_history_apply_disjoint(&f,0,37,32,38,1);
 assert(s_stats.copied==54 && s_stats.uniform_pixels==32 && s_stats.uniform_read_bytes==64);
 /* The next generic call must use the copied sharp value, including overlap. */
 px[50]=0xffff;nos_history_begin(4);nos_history_apply(&f,45,37,60,38,1);
 assert(s_sharp[37*GFX_W+50]==0xffff && px[50]!=0xffff);
 nos_history_apply(&f,45,37,60,38,1);assert(s_stats.duplicates==15);
}
int main(void){assert(nos_history_init());exact_tag_case();
for(int pass=0;pass<2;pass++){
 rng=79;nos_history_reset();memset(s_sharp,0,HISTORY_BYTES);memset(s_seen,0,HISTORY_BITS);
 for(int frame=0;frame<600;frame++){
  if(frame%71==0)nos_history_reset();
  nos_history_native_copy=pass && frame%5!=0; /* runtime1/0 reversals */
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
    else {
uint32_t before=s_uniform[yy];int warm=x>=s_valid_lo[yy] && end<=s_valid_hi[yy];
      nos_history_apply_disjoint(&f,x,yy,end,yy+1,enabled);
if(nos_history_native_copy){
      if(!enabled && warm){int first=x/32,last=(end-1)/32;uint32_t mask=((1u<<(2*(last-first+1)))-1u)<<(2*first);assert(s_uniform[yy]==(before&~mask));}
}
    }
    assert(s_stats.pixels==s_stats.seeded+s_stats.unchanged+s_stats.changed+s_stats.copied);
    assert(s_stats.uniform_write_bytes+s_stats.uniform_read_bytes==2u*s_stats.uniform_pixels);
    x=end;
   }
  }
  /* Generic overlap remains legal and duplicate suppression must survive. */
  if(frame%17==0){gfx_fb f={0};f.px=px;f.h=1;f.y_base=40;f.cx1=GFX_W;nos_history_apply(&f,17,40,90,41,0);}
  unsigned long long actual[10]={hash(px,sizeof px),hash(s_sharp,HISTORY_BYTES),hash(s_valid,HISTORY_BITS),hash(s_seen,HISTORY_BITS),hash(s_valid_lo,sizeof s_valid_lo),hash(s_valid_hi,sizeof s_valid_hi),hash(s_row_generation,sizeof s_row_generation),s_stats.pixels,s_stats.blended,s_stats.seeded};
  if(!pass)memcpy(reference[frame],actual,sizeof actual);else assert(!memcmp(reference[frame],actual,sizeof actual));
 }
}
 puts("PASS native copy:600 flag0/1 toggled frames, exact sharp/output/valid/seen/intervals, touched tags, honest copied counters, generic overlap afterward");
}
