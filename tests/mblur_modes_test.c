/* Production admission/dispatch/state plus the real full-column raster kernel. */
#include "../nos/nos.c"
#include <assert.h>
#include <stdio.h>

#define H 83
#define X0 93
#define X1 139
static uint16_t input[GFX_W*H],pixels[GFX_W*H],reference[GFX_W*H],primary[GFX_W*H];
static unsigned claims;
uint32_t hal_micros(void) { return 0; }
void *hal_big_alloc(uint32_t bytes) { return calloc(1,bytes); }
unsigned gfx_hash_u(unsigned h,unsigned v) { return (h^v)*16777619u; }
unsigned gfx_hash_f(unsigned h,float v) { return gfx_hash_u(h,(unsigned)v); }
int gfx_rec_cmd(gfx_fb *fb,int x0,int y0,int x1,int y1,unsigned hash)
{ (void)x0;(void)y0;(void)x1;(void)y1;(void)hash;return fb->rec!=0; }
void ui_fblur_coverage_claim(const gfx_fb *fb,int x0,int y0,int x1,int y1)
{ assert(!fb->rec && x0==X0 && x1==X1 && y0==0 && y1==H);claims++; }

static void run(int pattern,float step,int k)
{
    nos_frame+=10;memset(&nos_last,0,sizeof nos_last);
    s_mb_pixels[0]=s_mb_pixels[1]=0;claims=0;
    s_mb_ns[1]=150;s_mb_ns[3]=100;
    memcpy(pixels,input,sizeof pixels);memcpy(reference,input,sizeof reference);
    gfx_rec record={0};gfx_fb recorder={0};recorder.rec=&record;
    uint32_t epoch=nos_mblur_record_epoch();
    mb_v(&recorder,X0,0,X1,H,step,0);
    mb_v(&recorder,X0,0,X1,H,step,1);
    assert(nos_mblur_record_epoch()==epoch+2);
    assert(s_mb_pixels[1]==0 && claims==0); /* admission/recording is not execution */
    mb_v_columns_state state;mb_v_columns_reset(&state);
    unsigned area=0,calls=0;
    for(int y=0;y<H;y+=7) {
        int rows=H-y<7?H-y:7,inset=y/7%4;
        gfx_fb fb={0},ref={0};fb.px=pixels+y*GFX_W;ref.px=reference+y*GFX_W;
        fb.y_base=ref.y_base=y;fb.h=ref.h=rows;
        fb.cx0=ref.cx0=X0+inset;fb.cx1=ref.cx1=X1-inset;
        int fallback=pattern==1 || (pattern==2 && y/7%2);
        mb_v(&fb,X0,0,X1,H,step,fallback);
        mb_v_columns(&ref,fb.cx0,fb.cx1,0,H,k,&state);
        area+=(unsigned)(rows*(fb.cx1-fb.cx0));calls++;
    }
    assert(!memcmp(pixels,reference,sizeof pixels));
    if(pattern==0) memcpy(primary,pixels,sizeof primary);
    else assert(!memcmp(pixels,primary,sizeof pixels));
    assert(s_mb_pixels[0]==0 && s_mb_pixels[1]==area && claims==calls);
    assert(nos_mblur_record_epoch()==epoch+2); /* raster never advances record epoch */
}
int main(void)
{
    unsigned rng=17;
    for(int i=0;i<GFX_W*H;i++) {
        rng=rng*1664525u+1013904223u;
        input[i]=i%11==0?(uint16_t)(rng>>16):0;
    }
    const float step[]={8,12,24,60};
    for(int k=1;k<=4;k++) for(int pattern=0;pattern<3;pattern++) run(pattern,step[k-1],k);
    puts("V modes: identical primary/fallback/switched pixels, clip continuity, recording and actual-area counters PASS");
    return 0;
}
