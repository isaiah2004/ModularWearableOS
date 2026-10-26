#include "blur_cache.h"
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t rng=0x9187ab32;
static unsigned random_u(void) {rng=rng*1664525u+1013904223u;return rng;}
static uint16_t reference(const blur_item *s,int x,int y,int level)
{
 unsigned r=0,g=0,b=0;
 int length=blur_cache_length(level);
 for(int k=0;k<length;k++) {
  int xx=s->horiz?x+k-length/2:x;
  int yy=s->horiz?y+s->context:y+s->context+k-length/2;
  if(xx<0 || xx>=s->w || yy<0 || yy>=s->h+2*s->context) continue;
  unsigned p=s->sharp[yy*s->w+xx];r+=p>>11;g+=(p>>5)&63;b+=p&31;
 }
 return (uint16_t)(((r+length/2)/length)<<11|((g+length/2)/length)<<5|(b+length/2)/length);
}
static void bake(blur_item *s)
{
 assert(s);
 for(int i=0;i<s->w*(s->h+2*s->context);i++) s->sharp[i]=(uint16_t)random_u();
 s->sharp_rows=s->h+2*s->context;
}
static void complete(blur_cache *c,blur_item *s,int level)
{
 unsigned calls=0;
 while(!blur_cache_filter(c,s,level)) assert(++calls<70);
 for(int y=0;y<s->h;y++) for(int x=0;x<s->w;x++)
  assert(s->blur[y*s->w+x]==reference(s,x,y,level));
}
static void accounting(const blur_cache *c)
{
 unsigned char expected[BC_BLOCKS]={0};
 for(int i=0;i<c->stage_blocks;i++) expected[c->stage_first+i]++;
 for(int s=0;s<BC_SLOTS;s++) for(int i=0;i<c->slots[s].blocks;i++)
  expected[c->slots[s].first+i]++;
 for(int s=0;s<BC_SLOTS;s++) for(int i=0;i<c->slots[s].blur_blocks;i++)
  expected[c->slots[s].blur_first+i]++;
 for(int i=0;i<c->block_count;i++) assert(expected[i]<=1 && c->used[i]==expected[i]);
}
static void kernel_switches(void)
{
 unsigned char *arena=malloc(BC_BYTES+64);assert(arena);
 memset(arena+BC_BYTES,0xa5,64);
 blur_cache c;static const int widths[]={1,2,465,466},contexts[]={0,1,24,32};
 static const uint16_t primary[]={0xf800,0x07e0,0x001f,0xffff,0};
 unsigned cases=0;
 for(int wi=0;wi<4;wi++) for(int ci=0;ci<4;ci++) for(int h=1;h<=29;h+=28)
 for(int hor=0;hor<2;hor++) for(int level=1;level<=4;level++) for(int pattern=0;pattern<4;pattern++) {
  blur_cache_init(&c,arena);
  blur_item *s=blur_cache_get(&c,&c,0,widths[wi],h,contexts[ci],0,hor);assert(s);
  for(int i=0;i<s->w*(s->h+2*s->context);i++)
   s->sharp[i]=pattern==0?0xffff:pattern==1?0:pattern==2?primary[i%5]:(uint16_t)random_u();
  s->sharp_rows=s->h+2*s->context;
  int calls=0,done=0;
  /* Toggle even while a vertical job is suspended between slices. The saved
   * sum representation must be reseeded without restarting/publication drift. */
  do {
   blur_cache_packed=(calls+pattern)&1;
   done=blur_cache_filter(&c,s,level);calls++;
   int expected_rows=calls*BC_SLICE;if(expected_rows>h)expected_rows=h;
   assert(s->filter_row==expected_rows);
   if(!done)assert(!s->level && !s->generation);
  } while(!done && calls<10);
  assert(done && calls==(h+BC_SLICE-1)/BC_SLICE && s->generation==1);
  for(int y=0;y<h;y++) for(int x=0;x<s->w;x++)assert(s->blur[y*s->w+x]==reference(s,x,y,level));
  accounting(&c);cases++;
 }
 for(int i=0;i<64;i++)assert(arena[BC_BYTES+i]==0xa5);
 blur_cache_packed=1;free(arena);
 printf("PASS %u packed/scalar switch cases: dimensions, channel extremes, context and publication\n",cases);
}
static void directed_cases(void)
{
 unsigned char *arena=malloc(BC_BYTES+64);assert(arena);
 memset(arena+BC_BYTES,0xa5,64);
 blur_cache c;blur_cache_init(&c,arena);
 static int owner,other;
 for(int trial=0;trial<500;trial++) {
  blur_item *a=blur_cache_get(&c,&owner,0,1+random_u()%100,17+random_u()%80,random_u()%25,0,random_u()%2);
  bake(a);a->pinned=1;complete(&c,a,1+random_u()%4);
  size_t bytes=(size_t)a->w*a->h*2;
  uint16_t *saved=malloc(bytes);assert(saved);memcpy(saved,a->blur,bytes);
  unsigned generation=a->generation;
  int level=a->level==4?1:a->level+1;
  assert(!blur_cache_filter(&c,a,level));
  assert(!memcmp(saved,a->blur,bytes) && a->generation==generation);
  blur_item *b=blur_cache_get(&c,&other,trial,31,43,24,0,trial&1);
  bake(b);b->pinned=1;
  /* Another item/level interrupts only unpublished scratch. */
  complete(&c,b,1+random_u()%4);
  assert(!memcmp(saved,a->blur,bytes) && a->generation==generation);
  complete(&c,a,level);
  assert(a->generation==generation+1);
  assert(!blur_cache_filter(&c,a,level==4?1:level+1));
  blur_cache_invalidate(&c,&owner,0,0);
  assert(!c.stage_item && !a->level && !a->pending && !a->sharp_rows);
  free(saved);a->pinned=0;b->pinned=0;
  accounting(&c);
 }
 /* A full seven-item Weather viewport fits in 1,486,848 bytes including the
  * existing separate metadata header. Guard every completed output with oracle. */
 const size_t payload=7u*48u*BC_BLOCK+19u*BC_BLOCK;
 blur_cache_init_sized(&c,arena,payload);
 for(int i=0;i<7;i++) {
  blur_item *s=blur_cache_get(&c,&owner,i,418,90,24,0,0);
  bake(s);s->pinned=1;complete(&c,s,3);
 }
 assert(c.stage_blocks==19);
 for(int i=0;i<7;i++) {
  blur_item *s=blur_cache_find(&c,&owner,i,418,90,24,0,0);
  assert(s && s->level==3 && s->blocks==29 && s->blur_blocks==19);
 }
 /* Reserve growth under fully pinned pressure may fail. It cannot evict or
  * overwrite any published item; later smaller scratch can recover. */
 assert(!blur_cache_get(&c,&other,99,466,466,0,0,1));
 for(int i=0;i<7;i++) {
  blur_item *s=blur_cache_find(&c,&owner,i,418,90,24,0,0);
  assert(s && s->level==3);
  for(int y=0;y<s->h;y++) for(int x=0;x<s->w;x++)
   assert(s->blur[y*s->w+x]==reference(s,x,y,3));
  complete(&c,s,4);
 }
 accounting(&c);
 /* Eviction of a partially filtered owner must detach the shared-stage owner
  * before the same slot is reused for another owner/dimension. */
 blur_cache_init_sized(&c,arena,3u*BC_BLOCK);
 blur_item *old=blur_cache_get(&c,&owner,0,20,30,0,0,0);
 bake(old);complete(&c,old,1);
 assert(!blur_cache_filter(&c,old,3));
 assert(c.stage_item==old);
 blur_item *replacement=blur_cache_get(&c,&other,0,44,44,0,0,1);
 assert(replacement && c.stage_item==0 && c.evictions);
 bake(replacement);complete(&c,replacement,4);
 assert(!blur_cache_find(&c,&owner,0,20,30,0,0,0));
 accounting(&c);
 for(int i=0;i<64;i++) assert(arena[BC_BYTES+i]==0xa5);
 free(arena);
 puts("PASS 500 interrupted random shutters, publication, invalidation, accounting, pinned seven-Weather capacity and stage-growth recovery");
}

static void published_samples(const blur_cache *c)
{
 for(int i=0;i<BC_SLOTS;i++) {
  const blur_item *s=&c->slots[i];
  if(!s->blocks) continue;
  assert((size_t)s->blocks*BC_BLOCK>=(size_t)s->w*(s->h+2*s->context)*2);
  assert((size_t)s->blur_blocks*BC_BLOCK>=(size_t)s->w*s->h*2);
  if(!s->level) continue;
  for(int j=0;j<5;j++) {
   int x=(int)(random_u()%(unsigned)s->w),y=(int)(random_u()%(unsigned)s->h);
   assert(s->blur[y*s->w+x]==reference(s,x,y,s->level));
  }
 }
}
int main(void)
{
 directed_cases();
 unsigned char *arena=malloc(64u*BC_BLOCK+64);assert(arena);
 memset(arena+64u*BC_BLOCK,0x7b,64);
 blur_cache c;blur_cache_init_sized(&c,arena,64u*BC_BLOCK);
 static int owner[32];
 for(int operation=0;operation<8000;operation++) {
  for(int i=0;i<BC_SLOTS;i++) c.slots[i].pinned=(int)(random_u()%3==0);
  unsigned id=random_u()%32;
  int w=17+(int)(id*31%79),h=17+(int)(id*47%97),context=(int)(id%25);
  blur_item *s=blur_cache_get(&c,owner+id,(int)id,w,h,context,0,id&1);
  if(s) {
   if(!s->sharp_rows) bake(s);
   s->pinned=1;
   int level=1+(int)(random_u()%4);
   if(random_u()%4==0) complete(&c,s,level);
   else (void)blur_cache_filter(&c,s,level);
   if(random_u()%11==0) blur_cache_invalidate(&c,owner+id,(int)id,(int)id);
  }
  accounting(&c);published_samples(&c);
 }
 for(int i=0;i<64;i++) assert(arena[64u*BC_BLOCK+i]==0x7b);

 /* Stage growth can succeed while the subsequent two-block item admission
  * cannot. Roll both reservations back without stranding scratch capacity. */
 blur_cache_init_sized(&c,arena,6u*BC_BLOCK);
 blur_item *old=blur_cache_get(&c,owner,0,20,30,0,0,0);
 bake(old);old->pinned=1;complete(&c,old,2);
 uint16_t *stage=c.stage,*published=old->blur;
 int first=c.stage_first,blocks=c.stage_blocks;
 unsigned generation=old->generation;
 assert(!blur_cache_get(&c,owner+1,1,64,40,0,0,0));
 assert(c.stage==stage && c.stage_first==first && c.stage_blocks==blocks);
 assert(old->blur==published && old->generation==generation && old->level==2);
 accounting(&c);published_samples(&c);
 assert(!blur_cache_get(&c,owner+1,1,466,466,0,0,0));
 assert(c.stage==stage && c.stage_first==first && c.stage_blocks==blocks);
 accounting(&c);published_samples(&c);
 complete(&c,old,4);
 /* Directed unequal-capacity exchange: a small item can inherit the larger
  * staging block, shrinking scratch. Fully pinned pressure must deny the
  * next large job without damaging either publication; exchanging the small
  * item again returns the larger block to scratch and permits recovery. */
 blur_cache_init_sized(&c,arena,26u*BC_BLOCK);
 blur_item *small=blur_cache_get(&c,owner,0,32,32,0,0,0);
 bake(small);small->pinned=1;complete(&c,small,1);
 blur_item *large=blur_cache_get(&c,owner+1,1,128,128,0,0,1);
 bake(large);large->pinned=1;complete(&c,large,1);
 assert(c.stage_blocks==8 && small->blur_blocks==1);
 complete(&c,small,2);
 assert(c.stage_blocks==1 && small->blur_blocks==8);
 unsigned small_generation=small->generation,large_generation=large->generation;
 stage=c.stage;first=c.stage_first;
 assert(!blur_cache_filter(&c,large,2));
 assert(c.stage==stage && c.stage_first==first && c.stage_blocks==1);
 assert(small->generation==small_generation && large->generation==large_generation);
 assert(!c.stage_item && !large->pending);
 accounting(&c);published_samples(&c);
 complete(&c,small,3);
 assert(c.stage_blocks==8 && small->blur_blocks==1);
 complete(&c,large,2);
 accounting(&c);published_samples(&c);
 free(arena);
 puts("PASS 8000 mixed-owner fragmented operations, sampled scalar publications, failed-stage and failed-item rollback");
 kernel_switches();
 return 0;
}
