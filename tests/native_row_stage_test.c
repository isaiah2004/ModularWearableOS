/* Actual native row staging: independent ink oracle and mode0 versus1/2/3. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static void *tracked_copy(void *,const void *,size_t);
#define memcpy tracked_copy
#include "apps/native_tile.h"
#undef memcpy

static unsigned long long nr_copy_calls,nr_copy_bytes,nr_palette_copies;
static uint8_t *tracked_scratch;
static const uint16_t *tracked_lut;
int nos_native_row_stage_mode;
uint8_t nos_native_row_stage_scratch[512] __attribute__((aligned(4)));
static void *tracked_copy(void *out,const void *source,size_t count){
 uintptr_t start=(uintptr_t)tracked_scratch,dest=(uintptr_t)out;
 if(dest>=start && dest<start+512){
  assert(count>0 && count<=128 && count<=start+512-dest);
  nr_copy_calls++;nr_copy_bytes+=count;
 }else{
  assert(count==512 && source==tracked_lut);
  nr_palette_copies++;
 }
 return memcpy(out,source,count);
}
void *hal_big_alloc(uint32_t n){(void)n;assert(!"no drawing allocation");return NULL;}
static unsigned claims,observations;
static uint64_t claim_hash,observe_hash;
static uint64_t coord_hash(uint64_t h,int a,int b,int c,int d){
 h=h*131u+(unsigned)a;h=h*131u+(unsigned)b;h=h*131u+(unsigned)c;return h*131u+(unsigned)d;
}
void ui_fblur_coverage_claim(const gfx_fb *fb,int a,int b,int c,int d){
 assert(fb && !fb->rec);claims++;claim_hash=coord_hash(claim_hash,a,b,c,d);
}
void ui_fblur_observe_native(const gfx_fb *fb,int a,int b,int c,int d,int blur){
 assert(fb && !fb->rec);observations++;observe_hash=coord_hash(observe_hash,a,b,c,d)*3u+(unsigned)blur;
}
static uint32_t rng=17;
static unsigned next(void){rng=1664525u*rng+1013904223u;return rng;}
static void clear_observe(void){claims=observations=0;claim_hash=observe_hash=0;}

static void check_rows(void){
 uint8_t planes[4][GFX_W+64];uint16_t lut[256],expected[GFX_W+4],actual[GFX_W+4];
 struct {uint32_t left;uint8_t scratch[2*GFX_W];uint32_t right;} guard;
 unsigned long long pixels=0,rows=0,copy_bytes=0;
 tracked_scratch=guard.scratch;
 const unsigned weights[]={0,1,31,127,128,255,256};
 const int lengths[]={0,1,2,63,64,65,127,128,129,255,256,257,465,466,467,-1};
 for(unsigned trial=0;trial<30000;trial++){
  for(unsigned k=0;k<sizeof planes;k++)((uint8_t *)planes)[k]=next()>>24;
  for(unsigned k=0;k<256;k++)lut[k]=(uint16_t)next();
  int count=trial<sizeof lengths/sizeof *lengths?lengths[trial]:1+(int)(next()%GFX_W);
  unsigned mix=weights[trial%7],phase=weights[(trial/7)%7],mask=trial%16;
  const uint8_t *p[4];
  for(int i=0;i<4;i++)p[i]=(mask&(1u<<i))?native_zero:planes[i]+(next()%32);
  /* Oversized fallback has actual backing storage, and no zero sentinel. */
  if(count>GFX_W)for(int i=0;i<4;i++)p[i]=planes[i];
  for(unsigned i=0;i<sizeof actual/sizeof *actual;i++)actual[i]=expected[i]=0xFACE;
  memset(guard.scratch,0xA7,sizeof guard.scratch);guard.left=0xDEADBEEF;guard.right=0x10203040;
  unsigned long long before=nr_copy_bytes;
  native_prepared_row(expected+1,p[0],p[1],p[2],p[3],count,lut,mix,phase);
  native_prepared_row_staged(actual+1,p[0],p[1],p[2],p[3],count,lut,mix,phase,guard.scratch);
  assert(!memcmp(actual,expected,sizeof actual));
  assert(guard.left==0xDEADBEEF && guard.right==0x10203040);
  for(int i=4*NATIVE_ROW_STAGE_CHUNK;i<2*GFX_W;i++)assert(guard.scratch[i]==0xA7);
  unsigned used=0;
  if(count>0 && count<=GFX_W && !((!mix || mix==256) && !phase)){
   if(!mix || mix==256){int low=mix==256?2:0;used=(p[low]!=native_zero)+(p[low+1]!=native_zero);}
   else {used=(p[0]!=native_zero)+(p[2]!=native_zero);if(phase)used+=(p[1]!=native_zero)+(p[3]!=native_zero);}
  }
  assert(nr_copy_bytes-before==(unsigned long long)(count>0?count:0)*used);
  copy_bytes+=nr_copy_bytes-before;
  for(int i=0;i<count;i++){
   unsigned long long sum=(unsigned long long)p[0][i]*(256-mix)*(256-phase)+(unsigned long long)p[2][i]*mix*(256-phase)+(unsigned long long)p[1][i]*(256-mix)*phase+(unsigned long long)p[3][i]*mix*phase+32768;
   assert(actual[i+1]==lut[sum>>16]);pixels++;
  }
  rows++;
 }
 /* Inactive planes may be absent; none is copied or accessed. */
 native_prepared_row_staged(actual,planes[0],NULL,NULL,NULL,466,lut,0,0,guard.scratch);
 native_prepared_row_staged(actual,planes[0],NULL,planes[2],NULL,466,lut,127,0,guard.scratch);
 native_prepared_row_staged(actual,NULL,NULL,planes[2],planes[3],466,lut,256,127,guard.scratch);
 puts("PASS inactive planes, oversized/null-scratch fallback, 128px boundary and guarded halo tail");
 native_prepared_row_staged(actual,planes[0],planes[1],planes[2],planes[3],466,lut,127,128,NULL);
 printf("PASS rows=%llu exact_independent_pixels=%llu source_bytes_copied=%llu source_copy_calls=%llu\n",rows,pixels,copy_bytes,nr_copy_calls);
}

static void check_blits(void){
 static uint8_t planes[3][GFX_W*73];
 static uint16_t a[GFX_W*82+2],b[GFX_W*82+2],saved[GFX_W*82+2];
 uint16_t lut[256];unsigned long long checked=0,commands=0;
 tracked_scratch=nos_native_row_stage_scratch;tracked_lut=lut;
 for(unsigned trial=0;trial<1400;trial++){
  for(unsigned i=0;i<sizeof planes;i++)((uint8_t *)planes)[i]=next()>>24;
  for(unsigned i=0;i<256;i++)lut[i]=(uint16_t)next();
  int aw=1+(int)(next()%GFX_W),bw=1+(int)(next()%GFX_W);
  sprite8 sa={planes[0],aw,1+(int)(next()%73)},sb={planes[1],bw,1+(int)(next()%73)},sv={planes[2],aw,sa.h};
  int ax=(int)(next()%101)-50,bx=(int)(next()%101)-50;
  int ay=(int)(next()%31)-15,by=(int)(next()%31)-15,pose=(int)(next()%513)-256;
  unsigned mix=trial%5==0?0:trial%5==1?256:next()%257,vmix=next()%65;
  int blurred=trial%5==0?0:trial%5==1?2:1;
  int stride=1+(int)(next()%GFX_W),xbase=(int)(next()%(GFX_W-stride+1));
  gfx_fb fb={0};fb.px=a+1;fb.stride=stride;fb.x_base=xbase;fb.y_base=(int)(next()%31)-15;fb.h=1+(int)(next()%82);
  fb.cx0=xbase+(int)(next()%stride);fb.cx1=xbase+stride;fb.dim=trial%7==0?1+(int)(next()%31):0;
  fb.ox=(int)(next()%15)-7;fb.oy=(int)(next()%15)-7;
  const sprite8 *variant=trial%11==0?&sv:NULL;
  for(unsigned i=0;i<sizeof a/sizeof *a;i++)a[i]=b[i]=(uint16_t)next();
  memcpy(saved,b,sizeof b);
  clear_observe();nos_native_row_stage_mode=0;native_prepared_blit(&fb,&sa,ax,ay,&sb,bx,by,mix,lut,71,pose,-10,78,blurred,variant,vmix);
  unsigned want_claims=claims,want_observations=observations;
  uint64_t want_claim_hash=claim_hash,want_observe_hash=observe_hash;
  /* Byte-for-byte command recorder comparison, including clipped origins and keys. */
  gfx_rec ra={0},rb={0};fb.rec=&ra;fb.px=a+1;
  nos_native_row_stage_mode=0;native_prepared_blit(&fb,&sa,ax,ay,&sb,bx,by,mix,lut,71,pose,-10,78,blurred,variant,vmix);
  for(int mode=0;mode<4;mode++){
   unsigned long long before=nr_copy_bytes;
   clear_observe();nos_native_row_stage_mode=mode;fb.px=b+1;fb.rec=NULL;memcpy(b,saved,sizeof b);
   native_prepared_blit(&fb,&sa,ax,ay,&sb,bx,by,mix,lut,71,pose,-10,78,blurred,variant,vmix);
   assert(!memcmp(a,b,sizeof a));assert(claims==want_claims && observations==want_observations);
   assert(claim_hash==want_claim_hash && observe_hash==want_observe_hash);
   if(!(mode&1) || blurred==2 || (variant && vmix))assert(nr_copy_bytes==before);
   checked+=sizeof a/sizeof *a;
   memset(&rb,0,sizeof rb);fb.rec=&rb;fb.px=b+1;before=nr_copy_bytes;
   unsigned long long palette_before=nr_palette_copies;
   native_prepared_blit(&fb,&sa,ax,ay,&sb,bx,by,mix,lut,71,pose,-10,78,blurred,variant,vmix);
   assert(!memcmp(&ra,&rb,sizeof ra));assert(nr_copy_bytes==before && nr_palette_copies==palette_before);
   commands+=(unsigned)ra.n;
  }
 }
 assert(nr_palette_copies>0);
 printf("PASS blits=5600 modes=0,1,2,3 full_guarded_pixels=%llu raw_record_commands=%llu coverage_observation_equal mode2_bypass variant_weighted_unchanged palette_copies=%llu palette_copy_bytes=%llu\n",checked,commands,nr_palette_copies,nr_palette_copies*512);
}
int main(void){check_rows();check_blits();assert(nr_copy_bytes>0 && nr_copy_calls>0 && nr_palette_copies>0);return 0;}
