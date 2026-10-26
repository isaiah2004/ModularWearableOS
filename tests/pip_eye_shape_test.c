/* Pixel-for-pixel regression against the pre-span eye raster. Keep its raster
 * and AA unchanged; the record adapter follows gfx's logical-coordinate
 * contract. Also independently check that damage encloses every changed pixel.
 * Optional --bench measures the helper, not watch FPS. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "../core/apps/pip_eye_shape.h"
static void pip_eye_blob_reference(gfx_fb *fb, float cx, float cy,
                         float half_width, float half_height, uint16_t color)
{
    float hw=half_width, hh=half_height;
    if(hw<=0 || hh<=0) return;
    if(fb->rec) {
        unsigned hash=gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(
            gfx_hash_u(0x50495042u,color),cx),cy),hw),hh);
        gfx_rec_cmd(fb,(int)floorf(cx-hw-1),
            (int)floorf(cy-hh-1),
            (int)ceilf(cx+hw+1),
            (int)ceilf(cy+hh+1),hash);
        return;
    }
    int c0=fb->cx0,c1=fb->cx1;
    if(c1<=c0){c0=0;c1=GFX_W;}
    if(c0<0)c0=0;
    if(c1>GFX_W)c1=GFX_W;
    int y0=(int)floorf(cy-hh-1)+fb->oy-fb->y_base;
    int y1=(int)ceilf(cy+hh+1)+fb->oy-fb->y_base;
    if(y0<0)y0=0;
    if(y1>fb->h)y1=fb->h;
    if(c1<=c0 || y1<=y0 || cx+hw+1+fb->ox<c0 || cx-hw-1+fb->ox>=c1)return;

    float r=fminf(hw,hh)*.62f, r2=r*r;
    float outside=r+.5f, outside2=outside*outside;
    float inside=fmaxf(0,r-.5f), inside2=inside*inside;
    float corner_x=hw-r, corner_y=hh-r;
    for(int by=y0;by<y1;by++) {
        float dy=fabsf((float)(by+fb->y_base-fb->oy)+.5f-cy);
        float qy=dy-corner_y;
        if(qy>=outside)continue;
        /* One sqrt at most per row finds its outer AA extent. All subsequent
         * coverage work uses squared distances; the solid middle needs none. */
        float extent=qy>0 ? corner_x+sqrtf(outside2-qy*qy) : hw+.5f;
        int x0=(int)floorf(cx-extent)+fb->ox;
        int x1=(int)ceilf(cx+extent)+fb->ox;
        if(x0<c0)x0=c0;
        if(x1>c1)x1=c1;
        uint16_t *row=&fb->px[by*GFX_W];
        for(int bx=x0;bx<x1;bx++) {
            float dx=fabsf((float)(bx-fb->ox)+.5f-cx);
            float qx=dx-corner_x, coverage;
            if(qx<=0 || qy<=0) {
                coverage=.5f+fminf(hw-dx,hh-dy);
            } else {
                float d2=qx*qx+qy*qy;
                if(d2<=inside2){row[bx]=color;continue;}
                if(d2>=outside2)continue;
                /* Pade distance near the circular rim: exact at radius r,
                 * smooth across pixel boundaries, and avoids per-pixel sqrt.
                 * For Pip's radii its AA error is well below one alpha step. */
                coverage=.5f+2*r*(r2-d2)/(d2+3*r2);
            }
            if(coverage>=1)row[bx]=color;
            else if(coverage>0)row[bx]=gfx_blend565(row[bx],color,
                (uint8_t)(coverage*255.0f));
        }
    }
}

#define GUARD 32
#define CAPACITY (GFX_W*GFX_H)
static uint16_t expected[CAPACITY+2*GUARD],actual[CAPACITY+2*GUARD];
static unsigned checked,seed=0x70ef3321u;
static unsigned next_random(void)
{ seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
static uint16_t background(unsigned index)
{ return (uint16_t)((index*37u)^(index>>3)^0xa52du); }
static void verify(float x,float y,float w,float h,int ox,int oy,
                   int y_base,int rows,int cx0,int cx1,uint16_t color)
{
    int count=rows*GFX_W+2*GUARD;
    for(int i=0;i<count;i++)expected[i]=actual[i]=background((unsigned)i);
    gfx_fb a={.px=expected+GUARD,.ox=ox,.oy=oy,.y_base=y_base,
        .h=rows,.cx0=cx0,.cx1=cx1};
    gfx_fb b=a;b.px=actual+GUARD;
    pip_eye_blob_reference(&a,x,y,w,h,color);
    pip_eye_blob(&b,x,y,w,h,color);
    if(memcmp(expected,actual,(size_t)count*sizeof *expected)) {
        for(int i=0;i<count;i++)if(expected[i]!=actual[i]) {
            fprintf(stderr,"case %u pixel %d: %04x != %04x; shape %.9g %.9g %.9g %.9g; translate %d %d; stripe %d %d; clip %d %d\n",
                checked,i-GUARD,actual[i],expected[i],x,y,w,h,ox,oy,y_base,rows,cx0,cx1);
            abort();
        }
    }
    for(int i=0;i<GUARD;i++) {
        assert(actual[i]==background((unsigned)i));
        int tail=count-GUARD+i;
        assert(actual[tail]==background((unsigned)tail));
    }
    int lo=cx0,hi=cx1;if(hi<=lo){lo=0;hi=GFX_W;}
    if(lo<0)lo=0;
    if(hi>GFX_W)hi=GFX_W;
    for(int py=0;py<rows;py++)for(int px=0;px<GFX_W;px++)
        if(px<lo || px>=hi)assert(actual[GUARD+py*GFX_W+px]==background((unsigned)(GUARD+py*GFX_W+px)));
    gfx_rec ra={0},rb={0};a.px=b.px=NULL;a.rec=&ra;b.rec=&rb;
    pip_eye_blob_reference(&a,x,y,w,h,color);pip_eye_blob(&b,x,y,w,h,color);
    assert(ra.n==rb.n && ra.n==(w>0 && h>0 ? 1 : 0));
    assert(!memcmp(&ra,&rb,sizeof ra));
    if(rb.n)for(int py=0;py<rows;py++)for(int px=0;px<GFX_W;px++)
        if(actual[GUARD+py*GFX_W+px]!=background((unsigned)(GUARD+py*GFX_W+px)))
            if(!(px>=rb.c[0].x0 && px<rb.c[0].x1 && py+y_base>=rb.c[0].y0 && py+y_base<rb.c[0].y1)) {
                fprintf(stderr,"Eye damage escapes case %u: pixel %d %d, bounds %d %d %d %d; shape %.9g %.9g %.9g %.9g; translate %d %d; stripe %d %d; clip %d %d\n",
                    checked,px,py+y_base,rb.c[0].x0,rb.c[0].y0,rb.c[0].x1,rb.c[0].y1,x,y,w,h,ox,oy,y_base,rows,cx0,cx1);
                abort();
            }
    checked++;
}
static double seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER tick,frequency;QueryPerformanceCounter(&tick);QueryPerformanceFrequency(&frequency);
    return (double)tick.QuadPart/(double)frequency.QuadPart;
#else
    return (double)clock()/CLOCKS_PER_SEC;
#endif
}
typedef void (*draw_fn)(gfx_fb*,float,float,float,float,uint16_t);
static volatile unsigned benchmark_checksum;
static double timed(draw_fn draw,int stripes)
{
    gfx_fb fb={.px=actual+GUARD,.h=GFX_H,.cx1=GFX_W};
    double start=seconds();
    for(int frame=0;frame<1200;frame++) {
        float dx=(frame%17)*.13f,dy=(frame%13)*.11f;
        float hw=45+(frame%23)*.35f,hh=57+(frame%19)*.7f;
        for(int y=0;y<(stripes?4:1);y++) {
            if(stripes){fb.h=48;fb.y_base=160+y*48;}
            for(int side=-1;side<=1;side+=2) {
                float x=233+side*74+dx;
                draw(&fb,x,248+dy,hw+8,hh+8,0x192a);
                draw(&fb,x,248+dy,hw,hh,0x97ed);
            }
        }
    }
    double elapsed=seconds()-start;
    unsigned sum=0;for(int i=0;i<CAPACITY;i++)sum+=actual[GUARD+i];
    benchmark_checksum=sum;
    return elapsed;
}
static void benchmark(void)
{
    for(int stripes=0;stripes<2;stripes++) {
        double old_best=1e9,new_best=1e9;
        for(int run=0;run<3;run++) {
            double a=timed(pip_eye_blob_reference,stripes),b=timed(pip_eye_blob,stripes);
            if(a<old_best)old_best=a;
            if(b<new_best)new_best=b;
        }
        printf("BENCH %s 1200 frames, two eyes + halos: reference %.1f ms, span %.1f ms, %.2fx helper speedup\n",
            stripes?"48-row stripes":"full buffers",old_best*1000,new_best*1000,old_best/new_best);
    }
}
int main(int argc,char **argv)
{
#ifdef _WIN32
    /* a failed check prints and exits non-zero; no Windows crash dialog on the desktop */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    const float shapes[][4]={
        {158.3f,240.4f,50.7f,66.3f},{318.8f,265.6f,50.4f,5.3f},
        {.1f,.1f,42.7f,36.9f},{465.8f,465.8f,42.7f,36.9f},
        {-600,99,50,60},{230.25f,150.75f,.4f,.3f},
        {158.5f,240.5f,50,66},{158,240,50,66},
        {158.5f,240.5f,.5f,30},{158.5f,240.5f,30,.5f},
        {158.5f,240.5f,1,1},{158.5f,240.5f,.01f,.01f},
        {233,233,220,110},{233,233,110,220},
        {100,231.5f,80,1.61290323f},{100,231.5f,1.61290323f,80},
        {200,231.25f,100,0},{200,231.25f,-1,100}};
    const int shifts[][2]={{0,0},{-31,23},{75,-59}};
    const int starts[]={0,21,97,172,231,298,410,462};
    const int clips[][2]={{0,GFX_W},{79,157},{151,155},{-7,12},{460,480},{0,0}};
    for(unsigned i=0;i<sizeof shapes/sizeof shapes[0];i++)
      for(unsigned j=0;j<sizeof shifts/sizeof shifts[0];j++) {
        const float *s=shapes[i];
        verify(s[0],s[1],s[2],s[3],shifts[j][0],shifts[j][1],0,GFX_H,0,GFX_W,GFX_ACCENT);
        for(unsigned k=0;k<sizeof starts/sizeof starts[0];k++)
          for(unsigned c=0;c<sizeof clips/sizeof clips[0];c++) {
            int rows=GFX_H-starts[k]<60 ? GFX_H-starts[k] : 60;
            verify(s[0],s[1],s[2],s[3],shifts[j][0],shifts[j][1],starts[k],rows,clips[c][0],clips[c][1],GFX_ACCENT);
          }
      }
    for(int i=0;i<3000;i++) {
        float x=(int)(next_random()%76000)/100.0f-150;
        float y=(int)(next_random()%76000)/100.0f-150;
        float w=(next_random()%22000+1)/100.0f,h=(next_random()%22000+1)/100.0f;
        int ox=(int)(next_random()%241)-120,oy=(int)(next_random()%241)-120;
        int y_base=(int)(next_random()%GFX_H),rows=1+(int)(next_random()%64);
        if(rows>GFX_H-y_base)rows=GFX_H-y_base;
        int cx0=(int)(next_random()%500)-16,cx1=cx0+1+(int)(next_random()%GFX_W);
        verify(x,y,w,h,ox,oy,y_base,rows,cx0,cx1,(uint16_t)next_random());
    }
    /* Place corners and centers immediately around representable boundaries. */
    for(int i=0;i<64;i++)for(int side=-1;side<=1;side+=2) {
        float w=.49f+i*.803f,h=.49f+i*1.03f;
        float x=nextafterf(158.5f+w,side<0?-INFINITY:INFINITY);
        float y=nextafterf(240.5f+h,side<0?-INFINITY:INFINITY);
        verify(x,y,w,h,-7,3,200,100,101,267,GFX_PINK);
    }
    printf("PASS Pip eye shape: %u old/new RGB565 comparisons; translated/clipped stripes, canaries, one recorded command and bounds\n",checked);
    if(argc>1 && !strcmp(argv[1],"--bench"))benchmark();
    return 0;
}

