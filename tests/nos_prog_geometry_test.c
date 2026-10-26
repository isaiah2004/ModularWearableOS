
#include "prog.h"
#include "ui.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "nos_prog_reference.inc"



static int fail_alloc,allocations,frees;
static uint32_t available=8u*1024u*1024u;
void *(hal_big_alloc)(uint32_t n){allocations++;return fail_alloc?NULL:malloc(n);}
void *nos_mem_alloc(uint32_t n,const char *owner){(void)owner;return (hal_big_alloc)(n);}
void nos_mem_free(void *p){if(p){frees++;free(p);}}
uint32_t nos_mem_available(void){return available;}

static uint32_t rng=7477;
static uint32_t random_word(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static uint16_t a[GFX_W*32+8],b[GFX_W*32+8];
static void check(int style,int n,float r,float t,float cx,float cy,float frac,uint16_t on,uint16_t off,int iteration) {
 gfx_fb f={0};f.h=(iteration%3==0?1:iteration%3==1?16:31);f.y_base=(int)(random_word()%600)-67;
 f.ox=(int)(random_word()%65)-32;f.oy=(int)(random_word()%65)-32;f.dim=(iteration%3==1?13:0);
 f.cx0=(int)(random_word()%300);f.cx1=f.cx0+(int)(random_word()%166)+1;
 if(iteration%3==0){f.x_base=f.cx0;f.stride=f.cx1-f.cx0;}
 for(unsigned i=0;i<sizeof a/sizeof*a;i++) a[i]=b[i]=(uint16_t)random_word();
 f.px=a;prog_ring_reference(&f,style,cx,cy,r,t,n,frac,on,off);
 f.px=b;prog_ring(&f,style,cx,cy,r,t,n,frac,on,off);
 if(memcmp(a,b,sizeof a)){fprintf(stderr,"pixel mismatch style%d n%d r%.4f t%.4f cx%.4f cy%.4f frac%.5f iteration%d\n",style,n,r,t,cx,cy,frac,iteration);abort();}
 gfx_rec ar={0},br={0};f.rec=&ar;prog_ring_reference(&f,style,cx,cy,r,t,n,frac,on,off);
 f.rec=&br;prog_ring(&f,style,cx,cy,r,t,n,frac,on,off);
 assert(ar.n==br.n && ar.overflow==br.overflow);assert(!memcmp(ar.c,br.c,(size_t)ar.n*sizeof(gfx_cmd)));
}
int main(void){
 /* Admission denial is not allocation failure, and release enables retry. */
#ifdef NOS_PSRAM_TRACKING
 available=512u*1024u+2087u;
 check(2,48,193,18,233,233,.5f,0xffff,0x2104,1);
 assert(!prog_geometry_bytes());
 available=8u*1024u*1024u;
#endif
 fail_alloc=1;int before=allocations;
 check(2,48,193,18,233,233,.5f,0xffff,0x2104,1);
 assert(!prog_geometry_bytes() && allocations==before+1);
 check(2,48,193,18,233,233.25f,.7f,0xf800,0,2);
 assert(allocations==before+1); /* failed optional allocation does not repeat */
 fail_alloc=0;prog_geometry_release();
 check(2,48,193,18,233,233,.5f,0xffff,0x2104,1);
 assert(prog_geometry_bytes()==2088);
 before=allocations;
 check(2,48,193,18,233,232.5f,.7f,0xf800,0,2);
 assert(allocations==before); /* scrolling/fill/ink reuse requires no allocation */
 prog_geometry_enabled=0;before=allocations;
 check(2,48,193,18,233,230.25f,.37f,0xf800,0x07e0,4);
 assert(allocations==before && prog_geometry_bytes()==2088);
 prog_geometry_enabled=1;
 check(2,48,193,18,233,229.25f,.81f,0x07e0,0x001f,5);
 assert(allocations==before);
 int old_frees=frees;prog_geometry_release();
#ifdef NOS_PSRAM_TRACKING
 assert(frees==old_frees+1);
#else
 (void)old_frees;
#endif
 assert(!prog_geometry_bytes());
 before=allocations;
 check(2,65,193,18,233,233,.5f,0xffff,0x2104,1);
 assert(!prog_geometry_bytes() && allocations==before);
 prog_geometry_enabled=0;
 check(2,48,193,18,233,233,.5f,0xffff,0x2104,1);
 assert(!prog_geometry_bytes() && allocations==before);
 prog_geometry_enabled=1;

 const int ns[]={7,24,48,60,64,65};unsigned cases=0;
 for(int style=0;style<6;style++)for(int nn=0;nn<6;nn++)for(int shape=0;shape<4;shape++) {
  float r=shape==0?193:shape==1?171:shape==2?25.25f:151.75f;
  float t=shape==0?18:shape==1?14:shape==2?3.25f:22.5f;
  for(int j=0;j<100;j++){
   float cx=233+(int)(random_word()%129)-64+.25f*(j%4),cy=233+(int)(random_word()%1001)-500+.25f*(j%4);
   float frac=(int)(random_word()%1537)/1024.f-.25f;
   check(style,ns[nn],r,t,cx,cy,frac,(uint16_t)random_word(),j%2?(uint16_t)random_word():0,j);cases++;
  }
 }
 /* Nonpositive thickness follows the original unretained fallback. */
 for(int style=0;style<6;style++) for(int j=0;j<8;j++) {check(style,48,193,j%2?0:-1,233,233,1,0xffff,0x2104,j);cases++;}
 printf("PASS %u exact pixels+record cases; bounded PSRAM geometry and exact allocation/reserve fallback\n",cases);
 prog_geometry_release();return 0;
}

uint16_t ui_mix(uint16_t a, uint16_t b, float t)
{
    if (t <= 0) return a;
    if (t >= 1) return b;
    /* lerp in 8-bit channels, then pack: a grey stays a neutral RGB565 grey (lerping
     * the 5- and 6-bit fields separately tints dark greys green or magenta) */
    int ra = (a >> 11) * 255 / 31, ga = ((a >> 5) & 63) * 255 / 63, ba = (a & 31) * 255 / 31;
    int rb = (b >> 11) * 255 / 31, gb = ((b >> 5) & 63) * 255 / 63, bb = (b & 31) * 255 / 31;
    int r = ra + (int)lroundf((rb - ra) * t), g = ga + (int)lroundf((gb - ga) * t), bl = ba + (int)lroundf((bb - ba) * t);
    /* two neutral greys always mix to a neutral grey: one 5-bit level, green the
     * nearest 6-bit match (2x below level 16, 2x + 1 from 16: 2x alone reads pink) */
    int na = (a >> 11) == (a & 31) && (((a >> 5) & 63) >> 1) == (a & 31);
    int nb = (b >> 11) == (b & 31) && (((b >> 5) & 63) >> 1) == (b & 31);
    if (na && nb) {
        int l5 = (r * 31 + 127) / 255;
        return (uint16_t)(l5 << 11 | (l5 * 2 + (l5 >= 16)) << 5 | l5);
    }
    return GFX_RGB(r, g, bl);
}
uint16_t ui_dim(uint16_t c, float k) { return ui_mix(0, c, k); }

