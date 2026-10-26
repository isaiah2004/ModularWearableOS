#include "blur_cache.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint16_t reference(const blur_item *s,int x,int y,int level) {
 unsigned r=0,g=0,b=0;int len=blur_cache_length(level);
 for(int k=0;k<len;k++) {
  int sx=s->horiz?x+k-len/2:x;
  int sy=s->horiz?y+s->context:y+s->context+k-len/2;
  if(sx<0 || sx>=s->w || sy<0 || sy>=s->h+2*s->context) continue;
  unsigned p=s->sharp[sy*s->w+sx];r+=p>>11;g+=(p>>5)&63;b+=p&31;
 }
 return (uint16_t)(((r+len/2)/len)<<11|((g+len/2)/len)<<5|(b+len/2)/len);
}
static void filters(blur_cache *c,int horizontal,int context) {
 static const char owner=0;
 blur_item *s=blur_cache_get(c,&owner,horizontal,37,29,context,0,horizontal);assert(s);
 for(int i=0;i<s->w*(s->h+2*context);i++) s->sharp[i]=(uint16_t)((i*7919u)^(i>>3));
 s->sharp_rows=s->h+2*context;
 for(int lv=1;lv<=4;lv++) {
  int calls=0;
  while(!blur_cache_filter(c,s,lv)) {assert(s->filter_row<=BC_SLICE*(++calls));assert(calls<10);}
  assert(calls==3); /* 29 rows need four bounded slices */
  for(int y=0;y<s->h;y++) for(int x=0;x<s->w;x++) assert(s->blur[y*s->w+x]==reference(s,x,y,lv));
 }
 /* A changed requested shutter cannot publish partially filtered pixels. */
 uint16_t published[37*29];memcpy(published,s->blur,sizeof published);
 assert(!blur_cache_filter(c,s,1));
 assert(s->level==4 && s->pending==1 && s->filter_row==BC_SLICE);
 assert(!memcmp(published,s->blur,sizeof published));
 assert(!blur_cache_filter(c,s,3));
 assert(s->level==4 && s->pending==3 && s->filter_row==BC_SLICE);
 assert(!memcmp(published,s->blur,sizeof published));
 while(!blur_cache_filter(c,s,3)) {}
 for(int y=0;y<s->h;y++) for(int x=0;x<s->w;x++) assert(s->blur[y*s->w+x]==reference(s,x,y,3));
 /* Same dimensions and owner survive another owner arriving. */
 static const char other=0;
 assert(blur_cache_get(c,&other,0,11,13,0,0,0));
 assert(blur_cache_find(c,&owner,horizontal,37,29,context,0,horizontal)==s);
 blur_cache_invalidate(c,&owner,horizontal,horizontal);
 assert(!s->sharp_rows && !s->level && !s->pending);
 assert(!blur_cache_filter(c,s,1));
}
int main(void) {
 unsigned char *mem=malloc(BC_BYTES+64);assert(mem);memset(mem+BC_BYTES,0xa5,64);
 blur_cache c;blur_cache_init(&c,mem);
 for(int kernel=0;kernel<=1;kernel++) {
  blur_cache_packed=kernel;blur_cache_init(&c,mem);
  filters(&c,0,0);filters(&c,0,24);filters(&c,1,0);
 }
 /* Full-page pressure: bounded eviction retains valid block accounting. */
 static const char owner=0;
 for(int i=0;i<20;i++) assert(blur_cache_get(&c,&owner,i,466,466,0,0,1));
 assert(c.evictions>0);
 for(int i=0;i<64;i++) assert(mem[BC_BYTES+i]==0xa5);
 for(int a=0;a<BC_SLOTS;a++) if(c.slots[a].blocks)
  for(int b=a+1;b<BC_SLOTS;b++) if(c.slots[b].blocks)
   assert(c.slots[a].first+c.slots[a].blocks<=c.slots[b].first || c.slots[b].first+c.slots[b].blocks<=c.slots[a].first);
 /* Pinning protects visible content when speculative work cannot fit. */
 for(int a=0;a<BC_SLOTS;a++) c.slots[a].pinned=1;
 assert(!blur_cache_get(&c,&owner,100,466,466,0,0,1));
 assert(!blur_cache_get(&c,&owner,0,467,466,0,0,1));
 blur_cache empty;blur_cache_init(&empty,0);assert(!blur_cache_get(&empty,&owner,0,8,8,0,0,0));
 free(mem);puts("blur cache: reference filters, slices, retention, eviction, pinning and bounds passed");return 0;
}
