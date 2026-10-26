#include "../nos/apps/native_tile.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
int nos_native_row_stage_mode;
uint8_t nos_native_row_stage_scratch[512] __attribute__((aligned(4)));
void *hal_big_alloc(uint32_t n){(void)n;assert(!"carry must not allocate");return NULL;}
static uint64_t claim_hash;static unsigned claim_count;
void ui_fblur_observe_native(const gfx_fb *fb,int x0,int y0,int x1,int y1,int blurred)
{assert(fb && !fb->rec && x1>x0 && y1>y0 && (blurred==0 || blurred==1));}
void ui_fblur_coverage_claim(const gfx_fb*f,int a,int b,int c,int d){(void)f;assert(c>a&&d>b);claim_count++;claim_hash=claim_hash*131+(unsigned)a;claim_hash=claim_hash*131+(unsigned)b;claim_hash=claim_hash*131+(unsigned)c;claim_hash=claim_hash*131+(unsigned)d;}
static unsigned rng=9;static unsigned rnd(void){rng=rng*1664525u+1013904223u;return rng;}
static uint8_t source[10][80*90];static uint16_t a[100*160],b[100*160],lut[256];
static struct {unsigned left;native_carry_pool pool;unsigned right;} guarded;
int main(void){
 for(int i=0;i<256;i++)lut[i]=(uint16_t)(0x1000+i);
 unsigned long long checked=0,oracle=0;native_carry_stats stats={0};
 for(int trial=0;trial<1200;trial++){
  native_carry_pool *pool=&guarded.pool;guarded.left=0x12345678;guarded.right=0xDEADBEEF;native_carry_reset(pool,trial);
  for(int i=0;i<80*90;i++)source[0][i]=trial%11?(rnd()>>24):0;
  for(int i=0;i<100*160;i++)a[i]=b[i]=0xFACE;
  sprite8 s={source[0],1+(int)(rnd()%80),1+(int)(rnd()%90)};
  int pose=(int)(rnd()%513)-256,exposure=trial%17?rnd()%12289:0;unsigned generation=1;if(trial%31==0)exposure=-exposure;
  for(int stripe=0;stripe<10;stripe++){
   int yy=stripe*16;if(trial%7==0&&stripe==5)yy=16;
   if(trial%13==0&&stripe==4){for(int i=0;i<s.w*s.h;i++)source[0][i]^=0x39;generation++;}
   if(trial%19==0&&stripe==3)pose=-pose;
   gfx_fb f={0};f.px=a+yy*100;f.stride=100;f.x_base=7;f.y_base=yy;f.h=16;f.cx0=10;f.cx1=104;f.ox=trial%3-1;f.oy=trial%5-2;
   if(trial%5==0&&stripe%2)f.cx0=20;
   if(trial%23==0&&stripe==4){gfx_rec rec={0};gfx_fb rf=f;rf.rec=&rec;native_tile_blit_carry(&rf,&s,NULL,0,24,32,lut,1,pose,exposure,3,143,pool,trial,generation,NATIVE_CARRY_INTERNAL,&stats);}
   claim_hash=0;claim_count=0;native_tile_blit(&f,&s,NULL,0,24,32,lut,1,pose,exposure,3,143);f.px=b+yy*100;uint64_t expected_claim=claim_hash;unsigned expected_claims=claim_count;claim_hash=0;claim_count=0;
   native_tile_blit_carry(&f,&s,NULL,0,24,32,lut,1,pose,exposure,3,143,pool,trial,generation,trial&1?NATIVE_CARRY_INTERNAL:NATIVE_CARRY_EXTERNAL,&stats);
   assert(claim_hash==expected_claim && claim_count==expected_claims);assert(!memcmp(a,b,sizeof a));checked+=1600;assert(guarded.left==0x12345678&&guarded.right==0xDEADBEEF);
   int absolute_exposure=abs(exposure);int width=absolute_exposure<256?256:absolute_exposure,halo=(width+511)/512+1;
   for(int row=yy;row<yy+16;row++)for(int col=0;col<100;col++){
    int xx=7+col-f.ox-24,sy=row-f.oy-32;
    if(7+col<f.cx0||7+col>=f.cx1||xx<0||xx>=s.w||sy< -halo||sy>=s.h+halo||sy+32<3||sy+32>=143)continue;
    double left=sy-pose/256.0-width/512.0,right=left+width/256.0,total=0;
    for(int k=0;k<s.h;k++){double lo=left>k-.5?left:k-.5,hi=right<k+.5?right:k+.5;if(hi>lo)total+=(hi-lo)*source[0][k*s.w+xx];}
    int expected=(int)(total/(width/256.0)+.5);assert(abs((int)b[row*100+col]-(0x1000+expected))<=1);oracle++;
   }
  }
 }
 native_carry_reset(&guarded.pool,9999);
 for(int e=0;e<10;e++){sprite8 s={source[e],80,90};gfx_fb f={0};f.px=a;f.h=16;f.stride=100;f.cx1=100;native_tile_blit_carry(&f,&s,NULL,0,0,0,lut,1,-64,10240,0,150,&guarded.pool,9999,e,NATIVE_CARRY_INTERNAL,&stats);}
 /* Within-frame widening must retire the old source key, not leave an
  * unreachable duplicate. Both initial and subsequent contiguous rows agree. */
 native_carry_reset(&guarded.pool,10000);
 for(int width=10;width<=80;width+=10){
  sprite8 s={source[0],width,90};
  for(int stripe=0;stripe<2;stripe++){
   gfx_fb f={0};f.px=a;f.h=16;f.stride=100;f.cx1=100;f.y_base=16+16*stripe;
   memset(a,0xFA,sizeof a);memset(b,0xFA,sizeof b);
   native_tile_blit(&f,&s,NULL,0,0,0,lut,1,-64,10240,0,150);f.px=b;
   native_tile_blit_carry(&f,&s,NULL,0,0,0,lut,1,-64,10240,0,150,&guarded.pool,10000,width,NATIVE_CARRY_EXTERNAL,&stats);
   assert(!memcmp(a,b,sizeof a));
   int matches=0;for(int i=0;i<8;i++)matches+=guarded.pool.slots[i].source==s.px;assert(matches==1);
  }
 }
 assert(stats.full_misses>=2&&stats.hits>100&&stats.copy_bytes>0);
 printf("PASS carry bytes=%zu compared=%llu independent_box=%llu hits=%llu saved_reads=%llu copybytes=%llu full=%llu\n",sizeof(native_carry_pool),checked,oracle,stats.hits,stats.saved_reads,stats.copy_bytes,stats.full_misses);
}
