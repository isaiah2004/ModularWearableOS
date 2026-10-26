#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "render_layers.h"
#include "render_bounds.h"
#undef hal_big_alloc
#define NB ((GFX_H + 15) / 16)
#define N (GFX_W * GFX_H)
static int fail_at = -1, allocations, coverage = 1, directional;
static uint32_t blur_epoch;
uint32_t nos_mblur_record_epoch(void) { return blur_epoch; }
static uint8_t *blocks[16]; static uint32_t sizes[16];
void *hal_big_alloc(uint32_t n) {
    int i = allocations++;
    if (i == fail_at) return NULL;
    assert(i < 16); blocks[i] = malloc(n + 64); assert(blocks[i]); sizes[i] = n;
    memset(blocks[i], 0xb3, n + 64); return blocks[i] + 32;
}
void *nos_mem_alloc(uint32_t n, const char *owner) { (void)owner; return hal_big_alloc(n); }
void nos_mem_free(void *p) {
    if (!p) return;
    for (int i=0;i<16;i++) if (blocks[i] && blocks[i]+32==p) {
        for (int j=0;j<32;j++) { assert(blocks[i][j]==0xb3); assert(blocks[i][32+sizes[i]+j]==0xb3); }
        free(blocks[i]); blocks[i]=NULL; return;
    }
    assert(!"unknown or duplicate cache free");
}
uint32_t nos_mem_used(void) { uint32_t n=0; for(int i=0;i<16;i++) if(blocks[i]) n+=sizes[i]; return n; }
uint32_t nos_mem_available(void) { return NOS_MEM_BUDGET-nos_mem_used(); }
const nos_screen *nos_top(void) { return NULL; }
int nos_transitioning(void) { return 0; }
uint32_t hal_micros(void) { static uint32_t t; return ++t; }
int ui_fblur_coverage_enable(int on) { int old = coverage; coverage = on; return old; }
int ui_fblur_any_active(void) { return directional; }
static uint16_t actual[N], expected[N];
static int position[4] = {30, 70, 110, 150}, overflow, raster_calls, record_calls, raster_output;
static void scene(gfx_fb *fb, int id) {
    if (fb->rec) record_calls++; else raster_calls++;
    if (!fb->rec && (uintptr_t)fb->px >= (uintptr_t)actual &&
        (uintptr_t)fb->px < (uintptr_t)(actual + N)) raster_output++;
    gfx_fill_rect(fb, 0, 0, GFX_W, GFX_H, 0);
    gfx_fill_rect(fb, 21 + id * 17, position[id], 59, 20, (uint16_t)(0xf800u >> id));
    if (overflow) for (int i = 0; i < GFX_MAX_CMDS + 4; i++)
        gfx_fill_rect(fb, 240 + (i % 70), 210 + (i % 21), 3, 4, (uint16_t)(i * 79));
}
static void a(gfx_fb *f) { scene(f, 0); }
static void b(gfx_fb *f) { scene(f, 1); }
static void c(gfx_fb *f) { scene(f, 2); }
static void d(gfx_fb *f) { scene(f, 3); }
static const nos_screen screens[4] = {
    {.name="a",.draw=a}, {.name="b",.draw=b}, {.name="c",.draw=c}, {.name="d",.draw=d}
};
static gfx_fb full(uint16_t *p) { gfx_fb f = {0}; f.px=p; f.h=GFX_H; f.cx1=GFX_W; return f; }
static void compare_full(int id) {
    memset(actual, 0x5a, sizeof actual); memset(expected, 0x5a, sizeof expected);
    gfx_fb f = full(actual), ref = full(expected);
    assert(nos_layers_draw(&f, &screens[id])); screens[id].draw(&ref);
    assert(!memcmp(actual, expected, sizeof actual)); assert(coverage == 1);
}
static void transforms(void) {
    for (int tx = -300; tx < 400; tx += 83)
    for (int ty = -130; ty < 400; ty += 97) {
        memset(actual, 0, sizeof actual); memset(expected, 0, sizeof expected);
        gfx_fb f = full(actual), ref = full(expected); f.ox=ref.ox=tx; f.oy=ref.oy=ty;
        f.cx0=ref.cx0=19; f.cx1=ref.cx1=411;
        assert(nos_layers_draw(&f, &screens[0])); screens[0].draw(&ref);
        assert(!memcmp(actual, expected, sizeof actual));
    }
    /* A packed column buffer must clip its writes to its actual allocation. */
    uint16_t packed[29 * 33 + 2], reference[29 * 33 + 2];
    for (int i=0;i<29*33+2;i++) packed[i]=reference[i]=0x7bad;
    gfx_fb f={0}, ref={0}; f.px=packed+1; ref.px=reference+1;
    f.stride=ref.stride=29; f.x_base=ref.x_base=45;
    f.y_base=ref.y_base=50; f.h=ref.h=33;
    f.ox=ref.ox=25; f.oy=ref.oy=-13;
    f.cx0=-31; f.cx1=GFX_W+41; /* intentionally wider than storage */
    ref.cx0=45; ref.cx1=74;
    nos_layers_invalidate();
    assert(nos_layers_draw(&f,&screens[0])); screens[0].draw(&ref);
    assert(!memcmp(packed,reference,sizeof packed));
}
static void scratch_views(void) {
    for (int y=0;y<GFX_H;y+=16)
    for (int glass=0;glass<2;glass++)
    for (int inset=0;inset<2;inset++)
    for (int transform=0;transform<3;transform++) {
        int h=GFX_H-y<16?GFX_H-y:16, x0=0, x1=GFX_W;
        nos_glass_span(y,y+h,&x0,&x1);
        if (inset) { x0++;x1--; }
        for (int i=0;i<N;i++) actual[i]=expected[i]=0x7bad;
        gfx_fb f=full(actual+y*GFX_W),ref=full(expected+y*GFX_W);
        f.y_base=ref.y_base=y; f.h=ref.h=h;
        f.cx0=ref.cx0=x0; f.cx1=ref.cx1=x1;
        f.ox=ref.ox=transform==1;f.oy=ref.oy=transform==2;
        f.glass_only=glass;
        nos_layers_invalidate();nos_layers_begin(20);
        int before=raster_output;
        assert(nos_layers_draw(&f,&screens[0]));screens[0].draw(&ref);
        int borrowed=!transform && !inset && (glass || (x0==0 && x1==GFX_W));
        assert((raster_output>before)==borrowed);
        for(int yy=0;yy<GFX_H;yy++)for(int xx=0;xx<GFX_W;xx++) {
            /* Only explicitly sacrificial, physically invisible corner pixels
             * can differ from a clipped draw; no other layer may be damaged. */
            if(borrowed && glass && yy>=y && yy<y+h && (xx<x0 || xx>=x1)) continue;
            assert(actual[yy*GFX_W+xx]==expected[yy*GFX_W+xx]);
        }
    }
}
/* A list-sized changing body has the same retained-band churn as scrolling,
 * independently of whether the frame budget admitted a directional filter. */
static unsigned list_pose, sparse_pose;
static void list_scene(gfx_fb *fb) {
    if (fb->rec) record_calls++; else raster_calls++;
    gfx_fill_rect(fb,0,0,GFX_W,GFX_H,0);
    for(int y=48;y<416;y+=8)
        gfx_fill_rect(fb,11,y,GFX_W-22,6,(uint16_t)(0x1021u*(list_pose+1)+(unsigned)y*97u));
    gfx_fill_rect(fb,5,18+(int)sparse_pose,20,3,0xffff);
}
static const nos_screen list_screen={.name="moving list",.draw=list_scene};
static void compare_list(int retained, int transformed) {
    for(int i=0;i<N;i++) actual[i]=expected[i]=0x59ed;
    gfx_fb f=full(actual), ref=full(expected);
    f.ox=ref.ox=transformed?17:0; f.oy=ref.oy=transformed?-11:0;
    f.dim=ref.dim=transformed?9:0;
    if(transformed) {f.cx0=ref.cx0=29;f.cx1=ref.cx1=401;}
    assert(nos_layers_draw(&f,&list_screen)==retained);
    if(!retained) list_screen.draw(&f);
    list_screen.draw(&ref);
    assert(!memcmp(actual,expected,sizeof actual));
    assert(coverage==1);
}
static void dynamic_layers(void) {
    list_pose=sparse_pose=0;
    nos_layers_begin(100); compare_list(1,0); /* cold bootstrap */
    assert(nos_layers_stats().bands_rendered==NB);
    nos_layers_begin(101); compare_list(1,0); /* warm static reuse */
    assert(!nos_layers_stats().bands_rendered && nos_layers_stats().bands_reused==NB);
    for(unsigned pose=1;pose<=4;pose++) {
        list_pose=pose;nos_layers_begin(101+pose);
        int records=record_calls;
        compare_list(0,0);compare_list(0,1); /* dim/transformed and same-frame decision */
        assert(record_calls==records+1);
        assert(!nos_layers_stats().bands_rendered && !nos_layers_stats().bands_reused);
        assert(!nos_layers_stats().blit_pixels);
    }
    nos_layers_begin(106);compare_list(1,0); /* settle: rebuild after live invalidation */
    assert(nos_layers_stats().bands_rendered==NB);
    nos_layers_begin(107);compare_list(1,1); /* then reuse under transform and dim */
    assert(!nos_layers_stats().bands_rendered && nos_layers_stats().bands_reused>0);
    sparse_pose=17;nos_layers_begin(108);compare_list(1,0);
    assert(nos_layers_stats().bands_rendered>0 && nos_layers_stats().bands_rendered<=3);
    assert(nos_layers_stats().bands_reused>NB/2); /* sparse updates keep caching */
    list_pose++;nos_layers_invalidate();nos_layers_begin(109);compare_list(1,0);
    assert(nos_layers_stats().bands_rendered==NB); /* explicit invalidate bootstraps anew */
    list_pose++;nos_layers_begin(110);compare_list(0,0);
    /* Bypass is fixed for all later stripes; the frame records only once. */
    int records=record_calls;
    for(int y=0;y<GFX_H;y+=16) {
        gfx_fb f=full(actual+y*GFX_W);f.y_base=y;f.h=GFX_H-y<16?GFX_H-y:16;
        assert(!nos_layers_draw(&f,&list_screen));
    }
    assert(record_calls==records && !nos_layers_stats().bands_rendered && !nos_layers_stats().bands_reused);
}
/* Instrument a continuing vertical filter, including its record epoch and
 * live-only coverage. Equal signatures intentionally omit the frame number:
 * filter admission alone must prevent retention, even on cold/static content. */
static int filter_admitted, stream_next;
static unsigned stream_tail;
static unsigned char claimed[GFX_H];
static void stream_scene(gfx_fb *fb) {
    gfx_fill_rect(fb,0,0,GFX_W,GFX_H,0);
    for(int y=0;y<GFX_H;y+=9)
        gfx_fill_rect(fb,0,y,GFX_W,4,(uint16_t)(0x1021u+(unsigned)y*97u));
    if(!filter_admitted) return;
    if(fb->rec) {
        blur_epoch++;
        gfx_rec_cmd(fb,0,0,GFX_W,GFX_H,0x7357u);
        return;
    }
    assert(stream_next==fb->y_base);
    for(int y=fb->y_base;y<fb->y_base+fb->h;y++) {
        uint16_t *row=gfx_row(fb,y-fb->y_base);
        stream_tail=(3u*stream_tail+(unsigned)row[0])/4u;
        for(int x=0;x<GFX_W;x++) row[x]=(uint16_t)stream_tail;
        if(coverage) claimed[y]++;
    }
    stream_next=fb->y_base+fb->h;
}
static const nos_screen stream_screen={.name="stateful directional",.draw=stream_scene};
static void stream_layers(void) {
    filter_admitted=1;
    for(int trial=0;trial<3;trial++) {
        /* Test first use, another admitted frame, and an explicit cold reset. */
        if(trial==2) nos_layers_invalidate();
        nos_layers_begin(120+trial);
        for(int pass=0;pass<2;pass++) {
            uint16_t *pixels=pass?expected:actual;
            memset(pixels,0,sizeof actual);memset(claimed,0,sizeof claimed);
            stream_next=0;stream_tail=0;
            uint32_t epoch=blur_epoch;
            for(int y=0;y<GFX_H;y+=16) {
                gfx_fb f=full(pixels+y*GFX_W);f.y_base=y;f.h=GFX_H-y<16?GFX_H-y:16;
                if(!pass) assert(!nos_layers_draw(&f,&stream_screen));
                stream_scene(&f);
            }
            assert(blur_epoch==epoch+(pass?0:1)); /* one record, no per-stripe re-record */
            assert(stream_next==GFX_H && coverage==1);
            for(int y=0;y<GFX_H;y++) assert(claimed[y]==1);
        }
        assert(!memcmp(actual,expected,sizeof actual));
        assert(!nos_layers_stats().bands_rendered && !nos_layers_stats().bands_reused);
    }
    /* Removing the full-screen filter changes every signature once. The next
     * settled record rebuilds sharp content, then later frames reuse it. */
    filter_admitted=0;nos_layers_begin(123);
    gfx_fb f=full(actual),ref=full(expected);
    assert(!nos_layers_draw(&f,&stream_screen));stream_scene(&f);stream_scene(&ref);
    assert(!memcmp(actual,expected,sizeof actual));
    nos_layers_begin(124);assert(nos_layers_draw(&f,&stream_screen));
    assert(!memcmp(actual,expected,sizeof actual) && nos_layers_stats().bands_rendered==NB);
    nos_layers_begin(125);assert(nos_layers_draw(&f,&stream_screen));
    assert(!nos_layers_stats().bands_rendered && nos_layers_stats().bands_reused==NB);
}
static void normal(void) {
    nos_layers_begin(0); compare_full(0);
    assert(nos_layers_stats().bands_rendered == NB);
    assert(raster_output==NB && nos_layers_stats().blit_pixels==0);
    int records=record_calls;
    compare_full(0); assert(record_calls==records);
    nos_layers_begin(1); compare_full(0);
    assert(nos_layers_stats().bands_rendered==0 && nos_layers_stats().bands_reused==NB);
    position[0]=70; nos_layers_begin(2); compare_full(0);
    assert(nos_layers_stats().bands_rendered==5); /* old three bands and new two bands */
    nos_layers_begin(3); compare_full(1); compare_full(2);
    /* All three slots used this frame: a fourth screen cannot evict them. */
    compare_full(0); gfx_fb f=full(actual);
    assert(!nos_layers_draw(&f,&screens[3]));
    nos_layers_begin(4); compare_full(0);
    assert(nos_layers_stats().bands_rendered==0);
    compare_full(3); assert(nos_layers_stats().bands_rendered==NB);
    compare_full(0); /* current frame's screen survived */
    assert(nos_layers_stats().bands_rendered==NB);
    nos_layers_invalidate(); nos_layers_begin(5); compare_full(0);
    assert(nos_layers_stats().bands_rendered==NB);
    nos_layers_begin(6); transforms();
    overflow=1; nos_layers_begin(7); compare_full(0);
    assert(nos_layers_stats().overflow_frames==1 && nos_layers_stats().bands_rendered==NB);
    nos_layers_begin(8); compare_full(0);
    assert(nos_layers_stats().overflow_frames==1 && nos_layers_stats().bands_rendered==NB);
    overflow=0; nos_layers_begin(9); compare_full(0);
    directional=1; assert(!nos_layers_draw(&f,&screens[0])); directional=0;
    nos_layer_cache_enabled=0; assert(!nos_layers_draw(&f,&screens[0])); nos_layer_cache_enabled=1;
    gfx_rec rec={0}; f.rec=&rec; assert(!nos_layers_draw(&f,&screens[0])); f.rec=NULL;
    assert(!nos_layers_draw(&f,NULL));
    nos_screen own_cache = screens[0]; own_cache.flags = SCR_NATIVE_CACHE;
    assert(!nos_layers_draw(&f,&own_cache));
    scratch_views();
}
int main(int argc,char **argv) {
    if (argc>1) fail_at=atoi(argv[1]);
    nos_layers_init(); nos_layer_stats s=nos_layers_stats();
    assert(s.bytes==0 && allocations==0); /* boot reserves no optional rasters */
    if (fail_at>=0) {
        nos_layers_begin(1); gfx_fb f=full(actual);
        for(int i=0;i<fail_at-1;i++) compare_full(i);
        int missing=fail_at>1?fail_at-1:0;
        assert(!nos_layers_draw(&f,&screens[missing]));
        assert(nos_layers_stats().allocation_failures==1);
        int attempted=allocations;
        for(int i=0;i<100;i++) assert(!nos_layers_draw(&f,&screens[missing]));
        assert(allocations==attempted);
    } else {
        normal();dynamic_layers();stream_layers();
        assert(nos_layers_stats().bytes==3u*N*2+sizeof(gfx_rec));
    }
    nos_layers_release(); assert(nos_layers_stats().bytes==0 && nos_mem_used()==0);
    fail_at=-1; nos_layers_begin(10); compare_full(0);
    assert(nos_layers_stats().bytes==N*2u+sizeof(gfx_rec));
    nos_layers_release(); assert(nos_mem_used()==0);
    puts("layers: dynamic live bypass, stateful pixels/coverage, sharp reuse, damage, clipping and allocation fallback match");
    return 0;
}
