/* Production Q5 kernels and every history path against separate RGB channels. */
#include "../nos/render_history.c"
#include <assert.h>
#include <stdio.h>
static int fail_allocate,allocations;
void *hal_big_alloc(uint32_t n) {allocations++;return fail_allocate?NULL:malloc(n);}
static uint32_t rng=14717;
static uint32_t random_word(void) {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static uint16_t reference(uint16_t c,uint16_t p,unsigned w) {
    unsigned r=((32-w)*(c>>11)+w*(p>>11)+16)/32;
    unsigned g=((32-w)*((c>>5)&63)+w*((p>>5)&63)+16)/32;
    unsigned b=((32-w)*(c&31)+w*(p&31)+16)/32;
    return (uint16_t)(r<<11|g<<5|b);
}
static uint32_t reference_pair(uint32_t c,uint32_t p,unsigned w) {
    return reference(c,p,w)|((uint32_t)reference(c>>16,p>>16,w)<<16);
}
static void kernels(void) {
    for(unsigned w=0;w<=32;w++) {
        for(int lane=0;lane<2;lane++) for(int ch=0;ch<3;ch++) {
            int shift=(ch==0?0:ch==1?5:11)+lane*16,max=ch==1?64:32;
            for(int c=0;c<max;c++) for(int p=0;p<max;p++) {
                uint32_t cc=(uint32_t)c<<shift,pp=(uint32_t)p<<shift;
                assert(expose_pair_q5(cc,pp,w)==reference_pair(cc,pp,w));
            }
        }
        for(int j=0;j<30000;j++) {
            uint32_t c=random_word(),p=random_word();
            assert(expose_pair_q5(c,p,w)==reference_pair(c,p,w));
            assert(expose_q5(c,p,w)==reference(c,p,w));
            assert(expose_pair(c,p)==reference_pair(c,p,8));
            assert(expose(c,p)==reference(c,p,8));
        }
    }
}
static void intervals(void) {
    const unsigned fps[]={30,40,60},weights[3][4]={{8,4,3,2},{11,5,4,3},{16,8,5,4}};
    uint32_t frame=0;
    nos_history_reset();nos_history_cadence=1;
    for(int rate=0;rate<3;rate++) for(unsigned skipped=1;skipped<=4;skipped++) {
        uint32_t us=skipped*1000000u/fps[rate];
        nos_history_begin_interval(frame++,us);
        assert(nos_history_interval_us()==us);
        assert(nos_history_previous_weight_q5()==weights[rate][skipped-1]);
    }
    nos_history_begin_interval(frame,25000);assert(nos_history_previous_weight_q5()==11);
    nos_history_cadence=0;
    nos_history_begin_interval(frame,16666);nos_history_begin(frame);
    assert(nos_history_previous_weight_q5()==11 && nos_history_interval_us()==25000);
    nos_history_begin_interval(++frame,25000);
    assert(nos_history_previous_weight_q5()==8 && nos_history_interval_us()==25000);
    nos_history_cadence=1;nos_history_begin_interval(frame,16666);
    assert(nos_history_previous_weight_q5()==8); /* mode is latched until next frame */
    nos_history_begin(++frame);nos_history_begin_interval(frame,25000);
    assert(nos_history_previous_weight_q5()==8 && !nos_history_interval_us());
    nos_history_begin_interval(++frame,0);assert(nos_history_previous_weight_q5()==8);
    nos_history_begin_interval(++frame,1);assert(nos_history_previous_weight_q5()==32);
    nos_history_begin_interval(++frame,UINT32_MAX);assert(nos_history_previous_weight_q5()==0);
    nos_history_reset();assert(nos_history_previous_weight_q5()==8 && !nos_history_interval_us());
}
#define HEIGHT 12
#define CAP (GFX_W*HEIGHT+4)
static uint16_t actual[CAP],expected[CAP],truth[GFX_W*HEIGHT];
static unsigned char valid[GFX_W*HEIGHT];
static uint16_t scene(int frame,int x,int y) {
    if(frame<4) return ((x/32+y)&1)?0xffff:0;
    if(frame<7 && x>GFX_W/2) return 0xffff;
    return (uint16_t)((unsigned)(frame*97+x*37+y*71));
}
static void oracle_window(const gfx_fb *f,int x0,int y0,int x1,int y1,unsigned weight,int enabled) {
    int stride=f->stride?f->stride:GFX_W;
    for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++) {
        int buffer=(int)(f->px-actual)+(y-f->y_base)*stride+x-f->x_base;
        int screen=y*GFX_W+x;
        uint16_t c=actual[buffer];
        expected[buffer]=valid[screen]&&enabled?reference(c,truth[screen],weight):c;
        truth[screen]=c;valid[screen]=1;
    }
}
static void paths(void) {
    const unsigned rates[]={30,40,60};
    for(int rate=0;rate<3;rate++) for(int disjoint=0;disjoint<2;disjoint++)
    for(int packed=0;packed<2;packed++) for(int uniform=0;uniform<2;uniform++)
    for(int narrow=0;narrow<2;narrow++) for(int offset=0;offset<2;offset++) {
        nos_history_reset();memset(valid,0,sizeof valid);memset(truth,0,sizeof truth);
        nos_history_packed=packed;nos_history_uniform=uniform;
        for(int frame=0;frame<12;frame++) {
            nos_history_cadence=frame%5!=2;
            nos_history_begin_interval((unsigned)frame,(unsigned)(frame%4+1)*1000000u/rates[rate]);
            unsigned weight=nos_history_previous_weight_q5();
            for(unsigned i=0;i<CAP;i++) actual[i]=0x7bad;
            gfx_fb f={0};f.px=actual+offset;f.x_base=narrow?17:0;f.stride=narrow?93:GFX_W;
            f.y_base=2;f.h=9;f.cx0=f.x_base;f.cx1=f.x_base+f.stride;
            for(int y=2;y<11;y++) for(int x=f.cx0;x<f.cx1;x++)
                f.px[(y-2)*f.stride+x-f.x_base]=scene(frame,x,y);
            memcpy(expected,actual,sizeof actual);
            /* Four disjoint windows include holes, odd starts, warm uniform
             * blocks, the18px final block and directional copy regions. */
            int mid=f.cx0+f.stride/2,x0=f.cx0+(frame%3),x1=f.cx1-(frame%4);
            int ranges[4][4]={{x0,2,mid,6},{mid+1,2,x1,6},{x0,7,mid,11},{mid+1,7,x1,11}};
            for(int win=0;win<4;win++) {
                int *r=ranges[win],enabled=(frame+win)%4!=0;
                oracle_window(&f,r[0],r[1],r[2],r[3],weight,enabled);
                if(disjoint) assert(nos_history_apply_disjoint(&f,r[0],r[1],r[2],r[3],enabled));
                else assert(nos_history_apply(&f,r[0],r[1],r[2],r[3],enabled));
                assert(!memcmp(actual,expected,sizeof actual));
                if(!disjoint) {
                    /* A repeated window may neither feed back the exposed
                     * output into history nor change its first output. */
                    assert(nos_history_apply(&f,r[0],r[1],r[2],r[3],enabled));
                    assert(!memcmp(actual,expected,sizeof actual));
                }
            }
            for(int y=0;y<HEIGHT;y++) for(int x=0;x<GFX_W;x++)
                if(valid[y*GFX_W+x]) assert(s_sharp[y*GFX_W+x]==truth[y*GFX_W+x]);
            const nos_history_stats *s=nos_history_get_stats();
            assert(s->pixels==s->seeded+s->unchanged+s->changed+s->copied);
            assert(disjoint?!s->duplicates:s->duplicates>0);
        }
    }
}
static void tail_and_coverage(void) {
    nos_history_reset();nos_history_cadence=1;
    uint16_t pixels[3]={0};gfx_fb f={0};f.px=pixels;f.h=1;f.stride=3;f.cx1=3;
    nos_history_begin_interval(1,25000);assert(nos_history_apply(&f,0,0,3,1,1));
    /* A directional region stores its actual pre-temporal pixels. The history
     * API cannot reconstruct raw scene pixels from an existing spatial blur. */
    pixels[1]=0x8410;nos_history_begin_interval(2,25000);
    assert(nos_history_apply_disjoint(&f,0,0,3,1,0));assert(pixels[1]==0x8410);
    pixels[1]=0;nos_history_begin_interval(3,50000);
    assert(nos_history_apply_disjoint(&f,0,0,3,1,1));
    assert(pixels[1]==reference(0,0x8410,5));assert(s_sharp[1]==0);
    pixels[1]=0;nos_history_begin_interval(4,16666);
    assert(nos_history_apply_disjoint(&f,0,0,3,1,1));assert(!pixels[1]);
    assert(s_sharp[1]==0); /* tail retires after one frame, despite weight change */
}
int main(void) {
    fail_allocate=1;assert(!nos_history_init());assert(!nos_history_storage_bytes());
    gfx_fb f={0};uint16_t p=0x1234;f.px=&p;f.h=1;f.stride=1;f.cx1=1;
    nos_history_begin_interval(1,25000);assert(!nos_history_apply(&f,0,0,1,1,1));assert(p==0x1234);
    fail_allocate=0;assert(nos_history_init());assert(nos_history_storage_bytes()==HISTORY_STORAGE);
    int before=allocations;kernels();intervals();paths();tail_and_coverage();
    assert(allocations==before);assert(nos_history_init() && allocations==before);
    puts("PASS cadence history: exact RGB565,30/40/60+skips, toggles, all paths, sharp history, coverage/tails, allocation");
    return 0;
}
