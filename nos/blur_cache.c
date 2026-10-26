#include "blur_cache.h"
#include <string.h>
static const int lengths[] = {2,8,16,40};
int blur_cache_packed=1;
int blur_cache_length(int level) { return lengths[level < 1 ? 0 : level > 4 ? 3 : level-1]; }
/* Every shutter has a fixed divisor. Avoid three variable integer divisions
 * for each RGB565 pixel; the /40 arm also lowers to constant reciprocal math. */
static inline uint16_t filter_pixel(unsigned r,unsigned g,unsigned b,int level) {
 if(level==4) return (uint16_t)(((r+20u)/40u)<<11|((g+20u)/40u)<<5|(b+20u)/40u);
 int shift=level==1?1:level==2?3:4;
 unsigned half=1u<<(shift-1);
 return (uint16_t)(((r+half)>>shift)<<11|((g+half)>>shift)<<5|((b+half)>>shift));
}
/* B/R/G occupy bits0/11/21. Up to16 samples plus one sliding-window add fit
 * without overlapping lanes; /40 retains the scalar kernel. The per-lane
 * rounding bias and final mask reproduce filter_pixel exactly. */
#define FILTER_MASK 0x07e0f81fu
static inline uint32_t filter_expand(uint16_t p) {
 return ((uint32_t)p|((uint32_t)p<<16))&FILTER_MASK;
}
static inline uint16_t filter_packed_pixel(uint32_t sum,unsigned shift,unsigned bias) {
 uint32_t p=((sum+bias)>>shift)&FILTER_MASK;
 return (uint16_t)(p|(p>>16));
}
static void filter_packed_row(uint16_t *restrict dst,uint32_t *restrict sums,
 const uint16_t *add,const uint16_t *sub,int w,unsigned shift,unsigned bias) {
 if(add && sub) {
  for(int x=0;x<w;x++) {
   uint32_t sum=sums[x];dst[x]=filter_packed_pixel(sum,shift,bias);
   sums[x]=sum+filter_expand(add[x])-filter_expand(sub[x]);
  }
 } else for(int x=0;x<w;x++) {
  uint32_t sum=sums[x];dst[x]=filter_packed_pixel(sum,shift,bias);
  if(add) sum+=filter_expand(add[x]);
  if(sub) sum-=filter_expand(sub[x]);
  sums[x]=sum;
 }
}
/* Forty samples overflow the three-lane 32-bit representation. Pair B/R
 * in independent 16-bit lanes and keep G separately; sliding updates then
 * need two sum loads/stores rather than six narrow channel accesses. Lane
 * separation also holds for the transient add-before-subtract (41 samples). */
static inline uint32_t wide_br(uint16_t p) {
 return ((uint32_t)(p>>11)<<16)|(p&31u);
}
static inline uint16_t wide_pixel(uint32_t br,uint32_t g) {
 return (uint16_t)((((br>>16)+20u)/40u)<<11|((g+20u)/40u)<<5|(((br&65535u)+20u)/40u));
}
void blur_cache_init_sized(blur_cache *c,void *arena,size_t bytes) {
 memset(c,0,sizeof *c);c->arena=arena;
 size_t blocks=bytes/BC_BLOCK;
 c->block_count=(int)(blocks>BC_BLOCKS?BC_BLOCKS:blocks);
}
void blur_cache_init(blur_cache *c,void *arena) {blur_cache_init_sized(c,arena,BC_BLOCKS*BC_BLOCK);}
static void cancel_stage(blur_cache *c) {
 if(c->stage_item) {
  c->stage_item->pending=0;c->stage_item->filter_row=0;
  c->stage_item->staging=0;
 }
 c->stage_item=0;c->sum_item=0;
}
static void discard(blur_cache *c,blur_item *s) {
 if(c->stage_item==s) cancel_stage(c);
 if(c->sum_item==s) c->sum_item=0;
 for(int i=0;i<s->blocks;i++) c->used[s->first+i]=0;
 for(int i=0;i<s->blur_blocks;i++) c->used[s->blur_first+i]=0;
 memset(s,0,sizeof *s);
}
blur_item *blur_cache_find(blur_cache *c,const void *o,int item,int w,int h,int context,int x0,int horiz) {
 for(int i=0;i<BC_SLOTS;i++) { blur_item *s=&c->slots[i];
  if(s->owner==o && s->blocks && s->item==item && s->w==w && s->h==h && s->context==context && s->x0==x0 && s->horiz==horiz) return s;
 } return 0;
}
/* Reserve arena blocks; a live staging range is already marked used.
 * Only unpinned item ranges may be evicted. There is no heap allocation here. */
static int reserve_blocks(blur_cache *c,int blocks,const blur_item *keep) {
 if(blocks>c->block_count) return -1;
 for(;;) {
  int run=0;
  for(int i=0;i<c->block_count;i++) {
   run=c->used[i]?0:run+1;
   if(run==blocks) {
    int first=i-blocks+1;
    memset(c->used+first,1,(size_t)blocks);
    return first;
   }
  }
  blur_item *old=0;
  for(int i=0;i<BC_SLOTS;i++) {
   blur_item *s=&c->slots[i];
   if(s!=keep && s->blocks && !s->pinned && (!old || s->touched<old->touched)) old=s;
  }
  if(!old) return -1;
  discard(c,old);c->evictions++;
 }
}
static int reserve_stage(blur_cache *c,int w,int h,const blur_item *keep) {
 int blocks=(int)(((size_t)w*h*2+BC_BLOCK-1)/BC_BLOCK);
 if(c->stage_blocks>=blocks) return 1;
 cancel_stage(c);
 /* There are no external readers of scratch. Temporarily release its range
  * so a larger contiguous reservation can reuse it. If admission fails, put
  * the old reservation back; no pixels or pinned publications were changed. */
 int old_first=c->stage_first,old_blocks=c->stage_blocks;
 uint16_t *old_stage=c->stage;
 if(old_blocks) memset(c->used+old_first,0,(size_t)old_blocks);
 c->stage=0;c->stage_blocks=0;
 int first=reserve_blocks(c,blocks,keep);
 if(first<0) {
  if(old_blocks) memset(c->used+old_first,1,(size_t)old_blocks);
  c->stage_first=old_first;c->stage_blocks=old_blocks;c->stage=old_stage;
  return 0;
 }
 c->stage_first=first;c->stage_blocks=blocks;
 c->stage=(uint16_t *)(c->arena+(size_t)first*BC_BLOCK);
 return 1;
}
static void restore_stage(blur_cache *c,int first,int blocks,uint16_t *stage) {
 /* An unsuccessful item admission has written no pixels. Return its new
  * scratch reservation so a failed oversized owner cannot strand the pool. */
 if(c->stage_first==first && c->stage_blocks==blocks) return;
 cancel_stage(c);
 if(c->stage_blocks) memset(c->used+c->stage_first,0,(size_t)c->stage_blocks);
 if(blocks) memset(c->used+first,1,(size_t)blocks);
 c->stage_first=first;c->stage_blocks=blocks;c->stage=stage;
}
blur_item *blur_cache_get(blur_cache *c,const void *o,int item,int w,int h,int context,int x0,int horiz) {
 if(!c->arena || !o || w<=0 || h<=0 || w>466 || h>466 || context<0 || context>32) return 0;
 blur_item *s=blur_cache_find(c,o,item,w,h,context,x0,horiz);
 if(s) {s->touched=++c->clock;c->hits++;return s;}
 c->misses++;
 /* Reserve the maximum required scratch before admitting more pinned content.
  * Growth may discard unpublished work but never replaces a publication. */
 int stage_first=c->stage_first,stage_blocks=c->stage_blocks;
 uint16_t *stage=c->stage;
 if(!reserve_stage(c,w,h,0)) return 0;
 int blocks=(int)(((size_t)w*(h+2*context)*2+BC_BLOCK-1)/BC_BLOCK);
 int blur_blocks=(int)(((size_t)w*h*2+BC_BLOCK-1)/BC_BLOCK);
 s=0;
 for(int i=0;i<BC_SLOTS;i++) if(!c->slots[i].blocks) {s=&c->slots[i];break;}
 if(!s) {
  for(int i=0;i<BC_SLOTS;i++) if(!c->slots[i].pinned && (!s || c->slots[i].touched<s->touched)) s=&c->slots[i];
  if(!s) {restore_stage(c,stage_first,stage_blocks,stage);return 0;}
  discard(c,s);c->evictions++;
 }
 int first=reserve_blocks(c,blocks,0);
 if(first<0) {restore_stage(c,stage_first,stage_blocks,stage);return 0;}
 memset(s,0,sizeof *s);s->owner=o;s->item=item;s->w=w;s->h=h;
 s->context=context;s->x0=x0;s->horiz=horiz;
 s->first=first;s->blocks=blocks;s->touched=++c->clock;
 s->sharp=(uint16_t *)(c->arena+(size_t)first*BC_BLOCK);
 /* Sharp and published blur have independent block ownership. This permits
  * pointer/block ownership exchange with shared staging at publication. */
 int blur_first=reserve_blocks(c,blur_blocks,s);
 if(blur_first<0) {
  discard(c,s);restore_stage(c,stage_first,stage_blocks,stage);return 0;
 }
 s->blur_first=blur_first;s->blur_blocks=blur_blocks;
 s->blur=(uint16_t *)(c->arena+(size_t)blur_first*BC_BLOCK);
 return s;
}
void blur_cache_invalidate(blur_cache *c,const void *owner,int first,int last) {
 for(int i=0;i<BC_SLOTS;i++) { blur_item *s=&c->slots[i];
  if(s->owner==owner && s->item>=first && s->item<=last) {
   if(c->stage_item==s) cancel_stage(c);
   s->sharp_rows=0;s->filter_row=0;s->level=0;s->pending=0;s->capture_wait=0;s->generation++;}
 }
}
int blur_cache_filter(blur_cache *c,blur_item *s,int level) {
 if(!s || s->sharp_rows<s->h+2*s->context || level<1 || level>4) return 0;
 if(s->level==level) return 1;
 if(c->stage_item!=s || s->pending!=level) {
  cancel_stage(c);
  if(!reserve_stage(c,s->w,s->h,s)) return 0;
  c->stage_item=s;s->pending=level;s->filter_row=0;s->staging=c->stage;
 }
 int len=blur_cache_length(level),half=len/2,end=s->filter_row+BC_SLICE;
 if(end>s->h) end=s->h;
 int packed=blur_cache_packed && level!=4;
 unsigned shift=level==1?1:level==2?3:4;
 unsigned bias=(unsigned)half+((unsigned)half<<11)+((unsigned)half<<21);
 /* Sliding channel sums. Constant work per pixel, RGB565 channels never mix. */
 if(blur_cache_packed && level==4 && s->horiz) {
  for(int y=s->filter_row;y<end;y++) {
   const uint16_t *src=s->sharp+(size_t)(y+s->context)*s->w;
   uint16_t *dst=s->staging+(size_t)y*s->w;
   uint32_t br=0,g=0;
   for(int x=0;x<20 && x<s->w;x++) {br+=wide_br(src[x]);g+=(src[x]>>5)&63u;}
   for(int x=0;x<s->w;x++) {
    dst[x]=wide_pixel(br,g);
    int add=x+20,sub=x-20;
    if(add<s->w) {br+=wide_br(src[add]);g+=(src[add]>>5)&63u;}
    if(sub>=0) {br-=wide_br(src[sub]);g-=(src[sub]>>5)&63u;}
   }
  }
 } else if(blur_cache_packed && level==4) {
  uint32_t (*sums)[2]=c->wide_sums;
  int base=s->filter_row+s->context-half,hc=s->h+2*s->context;
  if(c->sum_item!=s || c->sum_next!=s->filter_row || !s->filter_row || c->sum_packed!=2) {
   memset(sums,0,(size_t)s->w*sizeof sums[0]);
   for(int y=base;y<base+len;y++) if(y>=0 && y<hc)
    for(int x=0;x<s->w;x++) {unsigned p=s->sharp[(size_t)y*s->w+x];sums[x][0]+=wide_br((uint16_t)p);sums[x][1]+=(p>>5)&63u;}
  }
  for(int y=s->filter_row;y<end;y++) {
   int add=y+s->context+20,sub=y+s->context-20;
   const uint16_t *a=add>=0 && add<hc?s->sharp+(size_t)add*s->w:0;
   const uint16_t *b=sub>=0 && sub<hc?s->sharp+(size_t)sub*s->w:0;
   for(int x=0;x<s->w;x++) {
    uint32_t br=sums[x][0],g=sums[x][1];
    s->staging[(size_t)y*s->w+x]=wide_pixel(br,g);
    if(a) {br+=wide_br(a[x]);g+=(a[x]>>5)&63u;}
    if(b) {br-=wide_br(b[x]);g-=(b[x]>>5)&63u;}
    sums[x][0]=br;sums[x][1]=g;
   }
  }
  c->sum_item=s;c->sum_next=end;c->sum_packed=2;
 } else if(s->horiz && packed) {
  for(int y=s->filter_row;y<end;y++) {
   const uint16_t *src=s->sharp+(size_t)(y+s->context)*s->w;
   uint16_t *dst=s->staging+(size_t)y*s->w;
   uint32_t sum=0;
   for(int x=0;x<len-half && x<s->w;x++) sum+=filter_expand(src[x]);
   for(int x=0;x<s->w;x++) {
    dst[x]=filter_packed_pixel(sum,shift,bias);
    int add=x-half+len,sub=x-half;
    if(add>=0 && add<s->w) sum+=filter_expand(src[add]);
    if(sub>=0 && sub<s->w) sum-=filter_expand(src[sub]);
   }
  }
 } else if(s->horiz) {
  for(int y=s->filter_row;y<end;y++) {
   const uint16_t *src=s->sharp+(size_t)(y+s->context)*s->w;
   uint16_t *dst=s->staging+(size_t)y*s->w;
   unsigned r=0,g=0,b=0;
   for(int x=0;x<len-half && x<s->w;x++) {unsigned p=src[x];r+=p>>11;g+=(p>>5)&63;b+=p&31;}
   for(int x=0;x<s->w;x++) {
    dst[x]=filter_pixel(r,g,b,level);
    int add=x-half+len,sub=x-half;
    if(add>=0 && add<s->w) {unsigned p=src[add];r+=p>>11;g+=(p>>5)&63;b+=p&31;}
    if(sub>=0 && sub<s->w) {unsigned p=src[sub];r-=p>>11;g-=(p>>5)&63;b-=p&31;}
   }
  }
 } else if(packed) {
  uint32_t *sums=c->packed_sums;
  int base=s->filter_row+s->context-half,hc=s->h+2*s->context;
  if(c->sum_item!=s || c->sum_next!=s->filter_row || !s->filter_row || c->sum_packed!=1) {
   memset(sums,0,(size_t)s->w*sizeof sums[0]);
   for(int y=base;y<base+len;y++) if(y>=0 && y<hc)
    for(int x=0;x<s->w;x++) sums[x]+=filter_expand(s->sharp[(size_t)y*s->w+x]);
  }
  for(int y=s->filter_row;y<end;y++) {
   int add=y+s->context-half+len,sub=y+s->context-half;
   const uint16_t *a=add>=0 && add<hc?s->sharp+(size_t)add*s->w:0;
   const uint16_t *b=sub>=0 && sub<hc?s->sharp+(size_t)sub*s->w:0;
   filter_packed_row(s->staging+(size_t)y*s->w,sums,a,b,s->w,shift,bias);
  }
  c->sum_item=s;c->sum_next=end;c->sum_packed=1;
 } else {
  /* Eight output rows at a time. Local sums fit in cache; initialization is bounded
   * by maximum shutter length (40), independently of the item height. */
  unsigned short (*sums)[3]=c->sums;
  int base=s->filter_row+s->context-half,hc=s->h+2*s->context;
  if(c->sum_item!=s || c->sum_next!=s->filter_row || !s->filter_row || c->sum_packed) {
   memset(sums,0,(size_t)s->w*sizeof sums[0]);
   for(int y=base;y<base+len;y++) if(y>=0 && y<hc)
    for(int x=0;x<s->w;x++) {unsigned p=s->sharp[(size_t)y*s->w+x];sums[x][0]+=p>>11;sums[x][1]+=(p>>5)&63;sums[x][2]+=p&31;}
  }
  for(int y=s->filter_row;y<end;y++) {
   for(int x=0;x<s->w;x++) s->staging[(size_t)y*s->w+x]=filter_pixel(sums[x][0],sums[x][1],sums[x][2],level);
   int add=y+s->context-half+len,sub=y+s->context-half;
   for(int x=0;x<s->w;x++) {
    if(add>=0 && add<hc) {unsigned p=s->sharp[(size_t)add*s->w+x];sums[x][0]+=p>>11;sums[x][1]+=(p>>5)&63;sums[x][2]+=p&31;}
    if(sub>=0 && sub<hc) {unsigned p=s->sharp[(size_t)sub*s->w+x];sums[x][0]-=p>>11;sums[x][1]-=(p>>5)&63;sums[x][2]-=p&31;}
   }
  }
  c->sum_item=s;c->sum_next=end;c->sum_packed=0;
 }
 c->work_pixels+=(unsigned)(end-s->filter_row)*s->w;
 s->filter_row=end;
 if(end==s->h) {
  /* UI-thread publication transfers arena ownership, never pixel data. The
   * previous publication becomes unpublished scratch. Block sizes may differ
   * after a dimension change; reserve_stage checks capacity before every job. */
  uint16_t *previous=s->blur;
  int first=s->blur_first,blocks=s->blur_blocks;
  s->blur=c->stage;s->blur_first=c->stage_first;s->blur_blocks=c->stage_blocks;
  c->stage=previous;c->stage_first=first;c->stage_blocks=blocks;
  c->stage_item=0;s->staging=0;
  s->level=level;s->pending=0;s->generation++;return 1;
 } return 0;
}
