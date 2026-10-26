#include "mblur.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define H 83
static uint16_t input[GFX_W*H],actual[GFX_W*H],expected[GFX_W*H];
static unsigned sums[GFX_W][3];
static int next_row[GFX_W];
static unsigned channel(uint16_t c,int n)
{ return n==0?c&31u:n==1?(c>>5)&63u:c>>11; }

/* Independent integer channels, including fractional state and gaps. */
static void reference(gfx_fb *fb,int x0,int x1,int y0,int y1,int k)
{
    for(int r=0;r<fb->h;r++) {
        int y=fb->y_base+r-fb->oy;
        if(y<y0 || y>=y1) continue;
        uint16_t *row=gfx_row(fb,r);
        for(int x=x0;x<x1;x++) {
            unsigned out[3];
            for(int c=0;c<3;c++) {
                unsigned v=channel(row[x],c),s=sums[x][c];
                s=next_row[x]!=y?v<<5:s-(s>>k)+(v<<(5-k));
                sums[x][c]=s;out[c]=s>>5;
            }
            next_row[x]=y+1;
            row[x]=(uint16_t)(out[0] | out[1]<<5 | out[2]<<11);
        }
    }
}

/* Compare complete state after arbitrary separate calls, not just final
 * pixels. This catches deferred next-tag writes, empty clips and stale sums. */
static void randomized_calls(void)
{
    unsigned rng=37,tested[4]={0};
    mb_v_columns_state state;memset(&state,0,sizeof state);mb_v_columns_reset(&state);
    memset(sums,0,sizeof sums);for(int x=0;x<GFX_W;x++) next_row[x]=-1;
    for(int trial=0;trial<2000;trial++) {
        rng=rng*1664525u+1013904223u;int k=1+(int)((rng>>24)%4);tested[k-1]++;
        rng=rng*1664525u+1013904223u;int x0=(int)((rng>>8)%GFX_W);
        rng=rng*1664525u+1013904223u;int x1=x0+(int)((rng>>8)%(GFX_W-x0+1));
        rng=rng*1664525u+1013904223u;int ybase=(int)((rng>>16)%H);
        rng=rng*1664525u+1013904223u;int height=(int)((rng>>8)%(H+1));
        rng=rng*1664525u+1013904223u;int oy=(int)((rng>>16)%61)-30;
        rng=rng*1664525u+1013904223u;int y0=(int)((rng>>8)%H);
        rng=rng*1664525u+1013904223u;int y1=y0+(int)((rng>>8)%(H-y0+1));
        memcpy(actual,input,sizeof actual);memcpy(expected,input,sizeof expected);
        gfx_fb fb={0},ref={0};fb.px=actual;ref.px=expected;
        fb.h=ref.h=height;fb.y_base=ref.y_base=ybase;fb.oy=ref.oy=oy;
        mb_v_columns(&fb,x0,x1,y0,y1,k,&state);reference(&ref,x0,x1,y0,y1,k);
        assert(!memcmp(actual,expected,sizeof actual));
        for(int x=0;x<GFX_W;x++) {
            assert(state.next[x]==(uint16_t)next_row[x]);
            if(next_row[x]>=0) assert(state.sum[x]==(sums[x][0] | sums[x][1]<<21 | sums[x][2]<<11));
        }
    }
    for(int k=0;k<4;k++) assert(tested[k]>400);
}

int main(void)
{
    unsigned rng=11;
    for(int i=0;i<GFX_W*H;i++) {
        rng=rng*1664525u+1013904223u;
        input[i]=i%7==0?(uint16_t)(rng>>16):i%11==0?0xffff:0;
    }
    for(int k=1;k<=4;k++)
    for(int stripe=1;stripe<=32;stripe++)
    for(int changing_clip=0;changing_clip<2;changing_clip++) {
        memcpy(actual,input,sizeof actual);memcpy(expected,input,sizeof expected);
        mb_v_columns_state state;memset(&state,0xa5,sizeof state);
        mb_v_columns_reset(&state);
        memset(sums,0,sizeof sums);for(int x=0;x<GFX_W;x++) next_row[x]=-1;
        for(int y=0;y<H;y+=stripe) {
            int rows=H-y<stripe?H-y:stripe;
            int inset=changing_clip?(y/stripe%5)*13:0;
            gfx_fb fb={0},ref={0};fb.px=actual+y*GFX_W;ref.px=expected+y*GFX_W;
            fb.y_base=ref.y_base=y;fb.h=ref.h=rows;fb.oy=ref.oy=2;
            mb_v_columns(&fb,3+inset,462-inset,1,76,k,&state);
            reference(&ref,3+inset,462-inset,1,76,k);
        }
        assert(!memcmp(actual,expected,sizeof actual));
    }
    /* A one-column stroke never leaks sideways or becomes a four-column block. */
    memset(actual,0,sizeof actual);actual[10*GFX_W+123]=0xffff;
    mb_v_columns_state state;mb_v_columns_reset(&state);
    gfx_fb fb={0};fb.px=actual;fb.h=H;
    mb_v_columns(&fb,0,GFX_W,0,H,3,&state);
    assert(actual[10*GFX_W+123] && actual[11*GFX_W+123]);
    for(int y=0;y<H;y++) for(int x=0;x<GFX_W;x++)
        if(x!=123) assert(actual[y*GFX_W+x]==0);
    /* Disjoint damage rectangles can interleave their row visits. Each column
     * must retain its own previous row rather than inherit the last rectangle. */
    memcpy(actual,input,sizeof actual);memcpy(expected,input,sizeof expected);
    mb_v_columns_reset(&state);memset(sums,0,sizeof sums);
    for(int x=0;x<GFX_W;x++) next_row[x]=-1;
    gfx_fb ref={0};ref.px=expected;ref.h=H;
    reference(&ref,0,GFX_W,0,H,3);
    for(int y=0;y<H;y+=20) for(int side=0;side<2;side++) {
        fb.px=actual+y*GFX_W;fb.y_base=y;fb.h=H-y<20?H-y:20;
        mb_v_columns(&fb,side?201:0,side?GFX_W:201,0,H,3,&state);
    }
    assert(!memcmp(actual,expected,sizeof actual));
    randomized_calls();
    puts("full-column V: independent RGB oracle, fractional state, stripe/clip gaps and thin strokes PASS");
    return 0;
}
