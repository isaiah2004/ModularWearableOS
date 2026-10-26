#include "ui_blur_test_support.h"

static uint16_t colors[16];
static unsigned record_calls,raster_calls,record_us;
static void mutable_row(gfx_fb *fb,int item,int x0,int y,int x1,int h,void *ctx)
{
    (void)ctx;
    if(fb->rec) {record_calls++;test_clock_us+=record_us;}
    else raster_calls++;
    gfx_fill_rect(fb,x0,y,x1-x0,h,colors[item]);
}
static void tick(const void *id,int first,int last,int total)
{
    unsigned before=FB_storage?FB.work_pixels:0;
    nos_frame++;
    ui_fblur_tick(id,1,290.f/M_FPS,first,last,total,90,0,64,1,mutable_row,NULL);
    assert(FB.work_pixels-before<=6u*BC_SLICE*64u);
}
static void published_pixels(const blur_item *s)
{
    assert(s && s->level);
    static const int lengths[]={2,8,16,40};
    int len=lengths[s->level-1];
    for(int y=0;y<s->h;y++) for(int x=0;x<s->w;x++) {
        unsigned r=0,g=0,b=0;
        for(int k=0;k<len;k++) {
            int source_y=y+k-len/2;
            unsigned p=(unsigned)source_y<(unsigned)s->h?colors[s->item]:0;
            r+=p>>11;g+=(p>>5)&63;b+=p&31;
        }
        uint16_t expected=(uint16_t)(((r+len/2)/len)<<11|
                                     ((g+len/2)/len)<<5|(b+len/2)/len);
        assert(s->blur[y*s->w+x]==expected);
    }
}
static void wait_contract(void)
{
    static int owner;
    used=3000000;colors[0]=0xf800;
    tick(&owner,0,0,1);
    blur_item *s=fb_find(0);assert(s);
    blur_cache_invalidate(&FB,&owner,0,0);
    assert(fb_capture_delay(s)==3);
    s->capture_wait=3;s->sig=fb_sig(0,mutable_row,NULL);
    s->checked=nos_frame;
    unsigned records=record_calls;
    for(int repeat=0;repeat<5;repeat++) assert(!fb_capture_ready(s,mutable_row,NULL));
    assert(s->capture_wait==3 && record_calls==records);
    nos_frame+=5;
    assert(!fb_capture_ready(s,mutable_row,NULL) && s->capture_wait==3);
    nos_frame++;
    assert(!fb_capture_ready(s,mutable_row,NULL) && s->capture_wait==2);
    colors[0]=0x07e0;nos_frame++;
    assert(!fb_capture_ready(s,mutable_row,NULL) && s->capture_wait==3);

    /* Consecutive observations remain consecutive across uint32 frame wrap. */
    s->checked=UINT32_MAX-1u;s->capture_wait=3;
    nos_frame=UINT32_MAX;
    assert(!fb_capture_ready(s,mutable_row,NULL) && s->capture_wait==2);
    nos_frame=0;
    assert(!fb_capture_ready(s,mutable_row,NULL) && s->capture_wait==1);
    nos_frame=1;
    assert(fb_capture_ready(s,mutable_row,NULL) && !s->capture_wait);

    /* A deferred source cannot be substituted for live pixels, and explicit
     * app invalidation clears the wait immediately rather than extending it. */
    s->capture_wait=3;
    assert(!ui_fblur_ready(&owner,0,0));
    gfx_fb f={0};f.h=90;f.cx1=64;
    assert(!ui_fblur_draw_at(&f,&owner,0,0,0,1));
    static int other_owner;
    s->checked=nos_frame;
    ui_fblur_tick(&other_owner,1,290.f/M_FPS,0,0,1,90,0,64,1,mutable_row,NULL);
    ui_fblur_tick(&owner,1,290.f/M_FPS,0,0,1,90,0,64,1,mutable_row,NULL);
    assert(s->owner==&owner && s->capture_wait==3 && s->checked==nos_frame);
    ui_fblur_invalidate(&owner,0,0);assert(!s->capture_wait);
    /* Continuing a partial capture probes once per contributing frame, even
     * if selected repeatedly. A changed next frame is rejected before writes. */
    s->sharp_rows=8;s->sig=fb_sig(0,mutable_row,NULL);s->checked=nos_frame;
    records=record_calls;nos_frame++;
    assert(fb_capture_ready(s,mutable_row,NULL) && record_calls==records+1);
    for(int repeat=0;repeat<5;repeat++) assert(fb_capture_ready(s,mutable_row,NULL));
    assert(record_calls==records+1);
    colors[0]^=0x001f;nos_frame++;
    assert(!fb_capture_ready(s,mutable_row,NULL));
    assert(!s->sharp_rows && s->capture_wait==3 && record_calls==records+2);
    ui_fblur_release();assert(used==3000000);

    /* Eviction/reuse clears all waiting-source metadata. */
    uint8_t memory[3u*BC_BLOCK];blur_cache cache;
    blur_cache_init_sized(&cache,memory,sizeof memory);
    blur_item *old=blur_cache_get(&cache,&owner,0,20,30,0,0,0);assert(old);
    old->capture_wait=7;
    static int next;
    blur_item *replacement=blur_cache_get(&cache,&next,0,44,44,0,0,0);
    assert(replacement && !replacement->capture_wait && !old->capture_wait);
}
static void periodic_revisions(int period)
{
    static int owner;
    colors[1]=0x8410;colors[2]=0xffff;nos_frame=0;
    int capture_rejected=0,captured_then_rejected=0,filtered_then_rejected=0;
    for(int frame=0;frame<100;frame++) {
        colors[0]=period==0?(frame&1?0x07e0:0xf800):
            period==1?(uint16_t)(0x4000+37*frame):
            (frame/period)&1?0x07e0:0xf800;
        blur_item *before=FB_storage?fb_find(0):NULL;
        int complete=before && before->sharp_rows==138;
        int partial=before && before->sharp_rows>0 && before->sharp_rows<138;
        int filtering=before && before->pending && before->filter_row>0;
        tick(&owner,0,1,3);
        blur_item *s=fb_find(0);assert(s);
        if(period==0) assert(!s->level); /* A/B/A never publishes mixed rows. */
        if(complete && s->capture_wait) captured_then_rejected=1;
        if(partial && s->capture_wait) capture_rejected=1;
        if(filtering && s->capture_wait) filtered_then_rejected=1;
        if(ui_fblur_ready(&owner,0,0)) {
            assert(s->sig==fb_sig(0,mutable_row,NULL));published_pixels(s);
        }
    }
    if(period<=1) assert(capture_rejected);
    else assert(captured_then_rejected);
    if(period==4) assert(filtered_then_rejected);
    blur_item *visible=fb_find(1),*incoming=fb_find(2);
    assert(visible && visible->level && incoming && incoming->level);
    published_pixels(visible);published_pixels(incoming);
    unsigned recovery=0;
    while(!ui_fblur_ready(&owner,0,0)) {
        tick(&owner,0,1,3);assert(++recovery<80);
    }
    published_pixels(fb_find(0));
    ui_fblur_release();assert(used==3000000);
}
static void probe_budget(void)
{
    static int owner;
    tick(&owner,0,2,3);
    for(int item=0;item<3;item++) {
        blur_item *s=blur_cache_get(&FB,&owner,item,64,90,24,0,0);assert(s);
        blur_cache_invalidate(&FB,&owner,item,item);
        s->capture_wait=3;s->sig=fb_sig(item,mutable_row,NULL);s->checked=nos_frame;
    }
    unsigned before=FB.work_pixels;
    record_calls=raster_calls=0;record_us=800;test_clock_us=0;
    tick(&owner,0,2,3);
    assert(record_calls==2 && !raster_calls && FB.work_pixels==before);
    assert(fb_find(0)->capture_wait==2 && fb_find(1)->capture_wait==2);
    assert(fb_find(2)->capture_wait==3 && test_clock_us==1600);
    /* A second same-frame call cannot count the first two observations twice. */
    record_us=0;
    ui_fblur_tick(&owner,1,290.f/M_FPS,0,2,3,90,0,64,1,mutable_row,NULL);
    assert(record_calls==3 && fb_find(0)->capture_wait==2 && fb_find(1)->capture_wait==2);
    ui_fblur_release();assert(used==3000000);
}
int main(void)
{
    wait_contract();periodic_revisions(0);periodic_revisions(1);
    periodic_revisions(3);periodic_revisions(4);probe_budget();
    assert(!failures);
    puts("PASS retry stability, wrap/gaps/repeated calls, implicit/explicit rejection, slot reuse, quantized animation recovery and bounded probes");
    return 0;
}
