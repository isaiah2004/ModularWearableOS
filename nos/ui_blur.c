#include "ui.h"
#include "nos.h"
#include "blur_cache.h"
#include "ui_blur_observe.h"
#include "sprite.h"
#include "hal.h"
#include "gfx_cost.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#endif

int nos_native_row_stage_mode=0;
/* Force this small shared raster owner into internal RAM on the device. */
#ifdef ESP_PLATFORM
DRAM_ATTR
#endif
uint8_t nos_native_row_stage_scratch[512] __attribute__((aligned(4)));


/* ------------------------------------------------ retained color motion cache
 * Persistent item storage is shared by all owners, with bounded LRU eviction.
 * Baking and filtering publish only complete revisions. Cold misses remain visible
 * to the caller; the scene compositor supplies motion coverage for live content. */
#define FB_COVER_RECTS 128
#define FB_EXCLUDE_RECTS 32
typedef struct { int x0,y0,x1,y1; } fb_coverage_rect;
typedef struct {
    blur_cache cache;
    gfx_rec record;
    fb_coverage_rect coverage[FB_COVER_RECTS];
    fb_coverage_rect exclusions[FB_EXCLUDE_RECTS];
} fb_storage;
/* Metadata, command recorder, filter sums and coverage all live in PSRAM too.
 * A fixed 32KiB prefix is counted inside the 2.6MiB ceiling, not above it. */
static fb_storage *FB_storage;
static void *FB_payload;
#define FB (FB_storage->cache)
#define FB_rec (FB_storage->record)
#define FB_cover (FB_storage->coverage)
#define FB_exclusions (FB_storage->exclusions)
static int FB_alloc_failed;
static unsigned FB_alloc_bytes,FB_alloc_failures;
static unsigned FB_active_frame;
static int FB_any_active;
static struct {
    const void *id;
    int W,H,X0,C,horiz,level;
    ui_blur_row_fn fn;
    void *ctx;
} FB_view;
static int FB_cover_enabled=1;

/* Diagnostic only: cache-draw processed destination area, not unique or final
 * visible pixels. Overdraw, black pixels and later overlays remain counted.
 * Existing FB_cover_enabled excludes offscreen layer preparation. */
static ui_fblur_observation FB_observation;
static int FB_observe_enabled;
void ui_fblur_observe_enable(int enabled) { FB_observe_enabled=enabled!=0; }
void ui_fblur_observe_begin(unsigned frame)
{
    if(!FB_observe_enabled) return;
    memset(&FB_observation,0,sizeof FB_observation);FB_observation.frame=frame;
}
void ui_fblur_observe_get(ui_fblur_observation *out) { if(out)*out=FB_observation; }
void ui_fblur_observe_native(const gfx_fb *fb,int x0,int y0,int x1,int y1,int blurred)
{
    if(!FB_observe_enabled || !fb || fb->rec || !FB_cover_enabled || !fb->px)return;
    int c0=fb->cx0,c1=fb->cx1;
    if(c1<=c0){c0=0;c1=GFX_W;}
    if(c0<fb->x_base)c0=fb->x_base;
    int right=fb->x_base+(fb->stride?fb->stride:GFX_W);
    if(c1>right)c1=right;
    if(c0<0)c0=0;
    if(c1>GFX_W)c1=GFX_W;
    x0+=fb->ox;x1+=fb->ox;y0+=fb->oy;y1+=fb->oy;
    if(x0<c0)x0=c0;
    if(x1>c1)x1=c1;
    if(y0<fb->y_base)y0=fb->y_base;
    if(y1>fb->y_base+fb->h)y1=fb->y_base+fb->h;
    if(x1<=x0 || y1<=y0)return;
    uint64_t area=(uint64_t)(unsigned)(x1-x0)*(unsigned)(y1-y0);
    FB_observation.native_processed_area+=area;
    if(blurred)FB_observation.native_blurred_processed_area+=area;
}
static unsigned fb_observe_request(const gfx_fb *fb,const void *id,int x,int y)
{
    if(!fb || fb->rec || !FB_cover_enabled || !fb->px) return 0;
    if(id!=FB_view.id || FB_view.W<=0 || FB_view.H<=0) {
        FB_observation.unknown_requests++;return 0;
    }
    int c0=fb->cx0,c1=fb->cx1;
    if(c1<=c0) {c0=0;c1=GFX_W;}
    if(c0<0)c0=0;
    if(c1>GFX_W)c1=GFX_W;
    int x0=x+fb->ox,x1=x0+FB_view.W;
    int y0=y+fb->oy,y1=y0+FB_view.H;
    if(x0<c0)x0=c0;
    if(x1>c1)x1=c1;
    if(y0<fb->y_base)y0=fb->y_base;
    if(y1>fb->y_base+fb->h)y1=fb->y_base+fb->h;
    unsigned area=x1>x0 && y1>y0?(unsigned)(x1-x0)*(unsigned)(y1-y0):0;
    FB_observation.requested_processed_area+=area;return area;
}


/* Raster-only coverage: temporal fallback must not blur a directional cache twice.
 * Offscreen layer bakers suspend coverage explicitly. Overflow is conservative:
 * unreported areas receive fallback blur instead of losing motion coverage. */
static unsigned FB_cover_frame;
static int FB_cover_n,FB_exclude_n,FB_cover_cancelled;
/* Coverage is constant between rectangle Y edges. Keep the compositor's first
 * 32 spans in internal RAM instead of sorting PSRAM metadata for every row. */
#define FB_SPAN_CACHE 32
static int FB_span_valid,FB_span_y0,FB_span_y1,FB_span_n;
static int FB_spans[FB_SPAN_CACHE][2];
static int fb_header(void) {
    if(FB_storage) return 1;
    if(FB_alloc_failed || sizeof(fb_storage)>BC_HEADER) return 0;
#ifdef NOS_PSRAM_TRACKING
    if(nos_mem_available()<BC_HEADER) return 0; /* optional coverage metadata */
#endif
    FB_storage=hal_big_alloc(BC_HEADER);
    if(!FB_storage) {FB_alloc_failed=1;FB_alloc_failures++;return 0;}
    memset(FB_storage,0,BC_HEADER);FB_alloc_bytes=BC_HEADER;
    return 1;
}
static void fb_cover_reset(void) {
    if(FB_cover_frame!=nos_frame) {FB_cover_frame=nos_frame;FB_cover_n=0;FB_exclude_n=0;FB_cover_cancelled=0;FB_span_valid=0;}
}
int ui_fblur_coverage_enable(int enabled) {
    int prev=FB_cover_enabled;FB_cover_enabled=enabled!=0;return prev;
}
static void fb_cover_add(const gfx_fb *fb,int x,int y,int w,int h) {
    if(!FB_cover_enabled || !fb || fb->rec || !fb_header()) return;
    fb_cover_reset();
    if(FB_cover_cancelled) return;
    int x0=x+fb->ox,y0=y+fb->oy,x1=x0+w,y1=y0+h;
    int c0=fb->cx0,c1=fb->cx1;
    if(c1<=c0) {c0=0;c1=GFX_W;}
    if(x0<c0) x0=c0;
    if(x1>c1) x1=c1;
    if(x0<0) x0=0;
    if(x1>GFX_W) x1=GFX_W;
    if(y0<fb->y_base) y0=fb->y_base;
    if(y1>fb->y_base+fb->h) y1=fb->y_base+fb->h;
    if(y0<0) y0=0;
    if(y1>GFX_H) y1=GFX_H;
    if(x1<=x0 || y1<=y0 || FB_cover_n==FB_COVER_RECTS) return;
    FB_span_valid=0;
    /* Adjacent stripes of the same item coalesce without claiming unseen rows. */
    for(int i=0;i<FB_cover_n;i++) if(FB_cover[i].x0==x0 && FB_cover[i].x1==x1 && FB_cover[i].y1==y0) {FB_cover[i].y1=y1;return;}
    FB_cover[FB_cover_n].x0=x0;FB_cover[FB_cover_n].x1=x1;
    FB_cover[FB_cover_n].y0=y0;FB_cover[FB_cover_n++].y1=y1;
}
void ui_fblur_coverage_exclude(const gfx_fb *fb,int x0,int y0,int x1,int y1)
{
    if(NOS_RENDER_SHARP)return;
    if(!fb || fb->rec || !FB_cover_enabled || !fb_header()) return;
    fb_cover_reset();
    if(FB_cover_cancelled) return;
    x0+=fb->ox;x1+=fb->ox;y0+=fb->oy;y1+=fb->oy;
    int c0=fb->cx0,c1=fb->cx1;
    if(c1<=c0) {c0=0;c1=GFX_W;}
    if(x0<c0) x0=c0;
    if(x1>c1) x1=c1;
    if(x0<0) x0=0;
    if(x1>GFX_W) x1=GFX_W;
    if(y0<fb->y_base) y0=fb->y_base;
    if(y1>fb->y_base+fb->h) y1=fb->y_base+fb->h;
    if(y0<0) y0=0;
    if(y1>GFX_H) y1=GFX_H;
    if(x1<=x0 || y1<=y0) return;
    FB_span_valid=0;
    for(int i=0;i<FB_exclude_n;i++) {
        fb_coverage_rect *r=&FB_exclusions[i];
        if(r->x0==x0 && r->x1==x1 && r->y0<=y1 && r->y1>=y0) {
            if(y0<r->y0) r->y0=y0;
            if(y1>r->y1) r->y1=y1;
            return;
        }
    }
    if(FB_exclude_n==FB_EXCLUDE_RECTS) {
        /* Losing a foreground exclusion would silently lose motion coverage.
         * Fall back conservatively for this frame instead of dropping it. */
        FB_cover_n=0;FB_cover_cancelled=1;return;
    }
    fb_coverage_rect *r=&FB_exclusions[FB_exclude_n++];
    r->x0=x0;r->x1=x1;r->y0=y0;r->y1=y1;
}
int ui_fblur_row_spans(int y,int spans[][2],int capacity)
{
    fb_cover_reset();
    if(!FB_storage || !spans || capacity<=0 || FB_cover_cancelled || (unsigned)y>=GFX_H) return 0;
    if(FB_span_valid && y>=FB_span_y0 && y<FB_span_y1 && capacity<=FB_SPAN_CACHE) {
        int n=FB_span_n<capacity?FB_span_n:capacity;
        memcpy(spans,FB_spans,(size_t)n*sizeof FB_spans[0]);
        return n;
    }
    int temp[FB_COVER_RECTS+FB_EXCLUDE_RECTS][2],n=0,next_y=GFX_H;
    for(int i=0;i<FB_cover_n;i++) {
        if(FB_cover[i].y0>y && FB_cover[i].y0<next_y) next_y=FB_cover[i].y0;
        if(FB_cover[i].y1>y && FB_cover[i].y1<next_y) next_y=FB_cover[i].y1;
        if(y<FB_cover[i].y0 || y>=FB_cover[i].y1) continue;
        int j=n++;
        while(j && temp[j-1][0]>FB_cover[i].x0) {temp[j][0]=temp[j-1][0];temp[j][1]=temp[j-1][1];j--;}
        temp[j][0]=FB_cover[i].x0;temp[j][1]=FB_cover[i].x1;
    }
    int merged=0;
    for(int i=0;i<n;i++) {
        if(merged && temp[i][0]<=temp[merged-1][1]) {
            if(temp[i][1]>temp[merged-1][1]) temp[merged-1][1]=temp[i][1];
        } else {temp[merged][0]=temp[i][0];temp[merged++][1]=temp[i][1];}
    }
    n=merged;
    for(int e=0;e<FB_exclude_n;e++) {
        const fb_coverage_rect *r=&FB_exclusions[e];
        if(r->y0>y && r->y0<next_y) next_y=r->y0;
        if(r->y1>y && r->y1<next_y) next_y=r->y1;
        if(y<r->y0 || y>=r->y1) continue;
        for(int i=0;i<n;i++) {
            int a=temp[i][0],b=temp[i][1];
            if(r->x1<=a || r->x0>=b) continue;
            if(r->x0<=a && r->x1>=b) {
                for(int j=i;j+1<n;j++) {temp[j][0]=temp[j+1][0];temp[j][1]=temp[j+1][1];}
                n--;i--;
            } else if(r->x0<=a) temp[i][0]=r->x1;
            else if(r->x1>=b) temp[i][1]=r->x0;
            else {
                for(int j=n;j>i+1;j--) {temp[j][0]=temp[j-1][0];temp[j][1]=temp[j-1][1];}
                temp[i][1]=r->x0;temp[i+1][0]=r->x1;temp[i+1][1]=b;n++;i++;
            }
        }
    }
    FB_span_n=n<FB_SPAN_CACHE?n:FB_SPAN_CACHE;
    memcpy(FB_spans,temp,(size_t)FB_span_n*sizeof FB_spans[0]);
    FB_span_y0=y;FB_span_y1=next_y;FB_span_valid=1;
    if(n>capacity) n=capacity; /* omitted claims receive conservative temporal blur */
    for(int i=0;i<n;i++) {spans[i][0]=temp[i][0];spans[i][1]=temp[i][1];}
    return n;
}

static unsigned fb_sig(int i, ui_blur_row_fn fn, void *ctx)
{
    gfx_fb r; memset(&r,0,sizeof r);
    r.h=GFX_H;r.cx1=GFX_W;r.rec=&FB_rec;
    FB_rec.n=0;FB_rec.overflow=0;
    int y=FB_view.horiz?0:UI_CY-FB_view.H/2;
    fn(&r,i,FB_view.X0,y,FB_view.X0+FB_view.W,FB_view.H,ctx);
    unsigned h=gfx_hash_u(0x5bd1e995u,FB_rec.overflow?nos_frame:0);
    for(int k=0;k<FB_rec.n && k<GFX_MAX_CMDS;k++) {
        const gfx_cmd *c=&FB_rec.c[k];
        if(c->y1<=y-FB_view.C || c->y0>=y+FB_view.H+FB_view.C) continue;
        h=gfx_hash_u(gfx_hash_u(h,c->hash),(unsigned)(c->x0*7919+c->y0*31+c->x1*131+c->y1));
    }
    return h;
}
static blur_item *fb_find(int i)
{
    if(!FB_storage) return 0;
    return blur_cache_find(&FB,FB_view.id,i,FB_view.W,FB_view.H,FB_view.C,FB_view.X0,FB_view.horiz);
}
#define FB_WORK_SLICES 6
static unsigned fb_capture_delay(const blur_item *s)
{
    unsigned rows=(unsigned)(s->h+2*s->context);
    unsigned ticks=(rows+FB_WORK_SLICES*BC_SLICE-1u)/(FB_WORK_SLICES*BC_SLICE);
    return ticks<2u?2u:ticks;
}
/* Validate lazily when cached pixels are actually eligible to replace live draws.
 * Resting screens need no permanent command recording just to retain warm items. */
static int fb_validate(blur_item *s) {
    if(!s || s->sharp_rows<s->h+2*s->context) return 0;
    if(s->checked!=nos_frame) {
        s->checked=nos_frame;
        unsigned sig=fb_sig(s->item,FB_view.fn,FB_view.ctx);
        if(sig!=s->sig) {
            blur_cache_invalidate(&FB,FB_view.id,s->item,s->item);
            /* A revision can change after capture, during sliced filtering.
             * Defer this implicit rejection too; explicit invalidation above
             * remains able to request an immediate, fresh capture. */
            s->sig=sig;s->checked=nos_frame;s->capture_wait=fb_capture_delay(s);
            return 0;
        }
    }
    return 1;
}
void ui_fblur_coverage_claim(const gfx_fb *fb,int x0,int y0,int x1,int y1)
{
    if(NOS_RENDER_SHARP)return;
    if(!fb || x1<=x0 || y1<=y0) return;
    fb_cover_add(fb,x0,y0,x1-x0,y1-y0);
}
void ui_fblur_idle(void)
{
    const nos_screen *screen=nos_top();
    if(!FB_payload || nos_transitioning() || !screen || !(screen->flags&SCR_NATIVE_CACHE)) return;
    hal_big_free(FB_payload);FB_payload=0;
    blur_cache_init_sized(&FB,0,0);
    FB_alloc_bytes=BC_HEADER;FB_alloc_failed=0;
    FB_view.id=0;FB_view.level=0;FB_any_active=0;FB_active_frame=0;
    /* Keep frame coverage/exclusions: specialized blur can already have drawn. */
}
void ui_fblur_release(void)
{
    hal_big_free(FB_payload);FB_payload=0;
    hal_big_free(FB_storage);FB_storage=0;FB_alloc_bytes=0;FB_alloc_failed=0;
    FB_view.id=0;FB_view.level=0;FB_any_active=0;FB_active_frame=0;
    FB_cover_n=FB_exclude_n=FB_cover_cancelled=0;
    FB_span_valid=0;
}
void ui_fblur_mem(void)
{
    unsigned used=0,ready=0;
    if(FB_storage) for(int i=0;i<BC_SLOTS;i++) if(FB.slots[i].blocks) {used++;ready+=FB.slots[i].level!=0;}
    printf("MEM_CACHE directional bytes=%u slots=%u published=%u failures=%u\n",FB_alloc_bytes,used,ready,FB_alloc_failures);
}
int ui_fblur_level_early(float step)
{
    if(NOS_RENDER_SHARP)return 0;
    float speed=fabsf(step)*M_FPS;
    return speed>=1920?4:speed>=960?3:speed>=300?2:speed>=150?1:0;
}
static int fb_level(float step)
{
    float speed=fabsf(step)*M_FPS;
    /* Retained 2/8/16/40 px shutters keep their reference-30Hz speed ladder. */
    return speed>=1800?4:speed>=720?3:speed>=150?2:speed>=15?1:0;
}
/* Public ladder is also the index into upstream specialized four-image caches. */
int ui_fblur_level(float step)
{
    if(NOS_RENDER_SHARP)return 0;
    float speed=fabsf(step)*M_FPS;
    return speed>=1680?3:speed>=840?2:speed>=360?1:0;
}
void ui_fblur_invalidate(const void *id,int first,int last)
{
    if(FB_storage) blur_cache_invalidate(&FB,id,first,last);
}
void ui_fblur_stats(ui_blur_stats *out)
{
    if(!out) return;
    memset(out,0,sizeof *out);
    out->allocation_failures=FB_alloc_failures;
    if(!FB_storage) return;
    out->bytes=FB_alloc_bytes;out->hits=FB.hits;out->misses=FB.misses;
    out->evictions=FB.evictions;out->work_pixels=FB.work_pixels;
}
/* A changing first band must not consume every frame recapturing pixels that
 * fail the completion check. Probe a rejected source until it is unchanged for
 * at least the minimum capture duration. This is a retry policy, not a promise
 * about future animation; the final capture check remains authoritative. */
static int fb_capture_ready(blur_item *s,ui_blur_row_fn fn,void *ctx)
{
    if(!s->capture_wait) {
        /* Observe each frame that contributes rows. A source that returns to
         * its starting signature at completion can still have changed between
         * frames; reject before adding those different rows to the capture. */
        if(s->sharp_rows>0 && s->sharp_rows<s->h+2*s->context && s->checked!=nos_frame) {
            unsigned sig=fb_sig(s->item,fn,ctx);
            if(sig!=s->sig) {
                blur_cache_invalidate(&FB,s->owner,s->item,s->item);
                s->sig=sig;s->checked=nos_frame;s->capture_wait=fb_capture_delay(s);
                return 0;
            }
            s->checked=nos_frame;
        }
        return 1;
    }
    uint32_t elapsed=nos_frame-s->checked;
    if(!elapsed) return 0; /* repeated calls cannot manufacture stable frames */
    unsigned sig=fb_sig(s->item,fn,ctx);
    if(elapsed!=1u || sig!=s->sig) s->capture_wait=fb_capture_delay(s);
    else --s->capture_wait;
    s->sig=sig;s->checked=nos_frame;
    return !s->capture_wait;
}
static int fb_work(blur_item *s,ui_blur_row_fn fn,void *ctx,int level)
{
    int hc=s->h+2*s->context;
    if(!s->sharp_rows) {s->sig=fb_sig(s->item,fn,ctx);s->checked=nos_frame;}
    if(s->sharp_rows<hc) {
        int rows=hc-s->sharp_rows;if(rows>BC_SLICE) rows=BC_SLICE;
        int y=s->horiz?0:UI_CY-s->h/2;
        gfx_fb f;memset(&f,0,sizeof f);
        f.px=s->sharp+(size_t)s->sharp_rows*s->w;f.h=rows;
#ifdef GFX_COST_MODEL
        f.cost_psram=1;
#endif
        f.y_base=y-s->context+s->sharp_rows;f.stride=s->w;
        f.x_base=s->x0;f.cx0=s->x0;f.cx1=s->x0+s->w;
        memset(f.px,0,(size_t)rows*s->w*2);
        fn(&f,s->item,s->x0,y,s->x0+s->w,s->h,ctx);
        s->sharp_rows+=rows;FB.work_pixels+=(unsigned)(rows*s->w);
        GFX_COST_PX(GFX_COST_FILL,(unsigned)(rows*s->w));
        if(s->sharp_rows==hc) {
            /* A live callback may change while strips are made. Never publish a
             * mixture of revisions: validate again before exposing sharp or blur. */
            unsigned sig=fb_sig(s->item,fn,ctx);
            if(sig!=s->sig) {
                s->sig=sig;s->checked=nos_frame;s->capture_wait=fb_capture_delay(s);
                s->sharp_rows=0;s->level=0;s->pending=0;return -1;
            }
            s->generation++;s->checked=nos_frame;
        }
        return 0;
    }
    unsigned before=FB.work_pixels;
    int ready=blur_cache_filter(&FB,s,level);
    GFX_COST_PX(GFX_COST_DIM,(FB.work_pixels-before)*8u);
    return ready;
}
/* Size a new arena for the visible working set plus two incoming items. Keep
 * the established minimum for later owners, but allow large bands to use the
 * existing ceiling. This runs only on allocation: a resident arena and its
 * published blur are never resized during a gesture. The allocator below still
 * applies the shared reserve and may admit less than a complete viewport. */
static unsigned fb_payload_target(int w,int h,int context,int first,int last,int total)
{
    if(first<0) first=0;
    if(last>=total) last=total-1;
    unsigned visible=last>=first?(unsigned)(last-first+1):0;
    if(visible>BC_SLOTS) visible=BC_SLOTS;
    unsigned incoming=(unsigned)total>visible?(unsigned)total-visible:0;
    if(incoming>2) incoming=2;
    unsigned items=visible+incoming;
    if(items>BC_SLOTS) items=BC_SLOTS;

    /* One block-rounded sharp allocation and one block-rounded publication
     * per item, plus one initially reserved shared staging publication. */
    uint64_t sharp=(uint64_t)(unsigned)w *
        ((unsigned)h+2ull*(unsigned)context)*sizeof(uint16_t);
    uint64_t publication=(uint64_t)(unsigned)w*(unsigned)h*sizeof(uint16_t);
    sharp=(sharp+BC_BLOCK-1u)&~(uint64_t)(BC_BLOCK-1u);
    publication=(publication+BC_BLOCK-1u)&~(uint64_t)(BC_BLOCK-1u);
    uint64_t goal=(sharp+publication)*items+publication;
    unsigned minimum=BC_DEFAULT_BYTES-BC_HEADER;
    unsigned maximum=BC_BYTES-BC_HEADER;
    if(goal<minimum) goal=minimum;
    if(goal>maximum) goal=maximum;
    return (unsigned)goal&~(BC_BLOCK-1u);
}
void ui_fblur_tick(const void *id,int held,float step,int i0,int i1,int n,int row_h,
                   int x0,int x1,int bands,ui_blur_row_fn fn,void *ctx)
{
    if(NOS_RENDER_SHARP)return;
    int mode=bands&3;
    if(!id || !fn || n<=0 || row_h<=0 || x1<=x0 || x1-x0>GFX_W ||
       (mode!=2 && row_h>GFX_H)) return;
    int early=(bands&UI_FBLUR_EARLY)!=0;
    if((bands&UI_FBLUR_PREWARM) && FB_active_frame==nos_frame && FB_any_active && FB_view.id!=id) return;
    FB_view.id=id;FB_view.W=x1-x0;FB_view.H=mode==2?GFX_H:row_h;
    FB_view.X0=x0;FB_view.C=mode==1?24:0;FB_view.horiz=mode==2;
    int requested=early?ui_fblur_level_early(step):fb_level(step);
    if(early && requested) requested=requested<3?requested+1:4;
    FB_view.level=nos_mblur && !NOS_HELD_SHARP(held)?requested:0;
    FB_view.fn=fn;FB_view.ctx=ctx;
    if(FB_active_frame!=nos_frame) {FB_active_frame=nos_frame;FB_any_active=0;}
    if(FB_view.level) FB_any_active=1;
    if(!FB_payload && !FB_alloc_failed && fb_header()) {
        unsigned bytes=fb_payload_target(FB_view.W,FB_view.H,FB_view.C,i0,i1,n);
#ifdef NOS_PSRAM_TRACKING
        unsigned available=nos_mem_available();
        available=available>512u*1024u?available-512u*1024u:0;
        if(bytes>available) bytes=available;
#endif
        bytes&=~(BC_BLOCK-1u);
        if(bytes<BC_BLOCK) return; /* admission skip retries cheaply after memory returns */
        FB_payload=hal_big_alloc(bytes);
        if(FB_payload) {
            FB_alloc_bytes+=bytes;
            blur_cache_init_sized(&FB,FB_payload,bytes);
        } else {FB_alloc_failed=1;FB_alloc_failures++;}
    }
    if(!FB_payload || FB_view.H>GFX_H) return;
    if(i0<0) i0=0;
    if(i1>=n) i1=n-1;
    if(i1<i0) return;
    /* Prepare at least a 150ms travel horizon, bounded to one viewport. Keep one
     * nearest band behind travel for reversals, before spending on distant
     * speculation. The same physical velocity gives the same horizon at every
     * frame cadence; no allocations or timers are needed for prediction. */
    int want[BC_SLOTS],nw=0;
    for(int i=i0;i<=i1 && nw<BC_SLOTS;i++) want[nw++]=i;
    int visible_count=nw,dir=step<0?-1:1;
    int count=i1-i0+1;
    /* Account for serial capture+filter service for two incoming bands at
     * the deterministic slice ceiling. The hardware time ceiling may stretch
     * this estimate; viewport and pool admission still bound speculation. */
    int service_slices=(FB_view.H+2*FB_view.C+BC_SLICE-1)/BC_SLICE+
                       (FB_view.H+BC_SLICE-1)/BC_SLICE;
    float horizon=2.0f*service_slices/(FB_WORK_SLICES*M_FPS);
    if(horizon<0.150f) horizon=0.150f;
    int ahead=1+(int)(fabsf(step)*M_FPS*horizon/row_h);
    if(ahead<2) ahead=2; /* nearest incoming pair amortizes capture latency */
    if(ahead<count && !FB_view.level) ahead=count; /* idle prewarm */
    if(ahead>count) ahead=count;
    int behind=dir>0?i0-1:i1+1;
    for(int j=1;j<=ahead && nw<BC_SLOTS;j++) {
        int i=dir>0?i1+j:i0-j;
        if(i>=0 && i<n) want[nw++]=i;
        if(j==1 && behind>=0 && behind<n && nw<BC_SLOTS) want[nw++]=behind;
    }
    /* In motion, validate all visible snapshots before publication. Idle sharp
     * users validate lazily in ready/draw; unused retained data costs no redraw. */
    blur_item *v;
    if(FB_view.level) for(int j=0;j<visible_count;j++) fb_validate(fb_find(want[j]));
    /* Touch all existing visible items before allocating predicted ones. */
    for(int j=0;j<BC_SLOTS;j++) FB.slots[j].pinned=0;
    for(int j=0;j<visible_count;j++) {v=fb_find(want[j]);if(v) {v->touched=++FB.clock;v->pinned=1;}}
    /* Protect an already prepared reversal reserve before any forward
     * admission can evict it. Cold reserve work still follows visible work. */
    if(behind>=0 && behind<n) {
        v=fb_find(behind);
        if(v) {v->touched=++FB.clock;v->pinned=1;}
    }
    if(nos_transitioning()) return;
    uint32_t t0=hal_micros();
    int slices=0;
    /* The time ceiling is checked between bounded operations; it cannot
     * preempt a callback or filter slice already in progress. */
    /* A missing incoming publication has priority over refining a usable blur.
     * Otherwise a speed-threshold crossing can starve a cold row behind six
     * already-published rows' shutter updates for many consecutive frames. */
    for(int phase=0;phase<3;phase++) for(int w=0;w<nw;w++) {
        blur_item *old=fb_find(want[w]);
        int visible=w<visible_count,missing=!old || !old->level;
        if(phase==0 && (!visible || !missing)) continue;
        if(phase==1) {
            if(visible) continue;
            if(!missing) {old->pinned=1;continue;}
        }
        if(phase==2 && missing) continue;
        blur_item *s=blur_cache_get(&FB,id,want[w],FB_view.W,FB_view.H,FB_view.C,x0,FB_view.horiz);
        if(!s) break;
        s->pinned=1; /* preserve nearest speculative items before considering farther ones */
        int capture_ready=fb_capture_ready(s,fn,ctx);
#ifndef GFX_COST_MODEL
        if(hal_micros()-t0>=1500u) return;
#endif
        if(!capture_ready) continue;
        /* Preserve the last demanded shutter at rest, including partial work.
         * A newly seen item prepares 16px, useful for a normal first fling. */
        /* Finish an in-flight shutter before following a new speed. A
         * threshold oscillation must not restart the shared scratch every frame.
         * The completed previous shutter remains usable during refinement. */
        int level=s->pending?s->pending:FB_view.level?FB_view.level:s->level?s->level:3;
        while(s->sharp_rows<s->h+2*s->context || s->level!=level) {
            int result=fb_work(s,fn,ctx,level);
            if(++slices>=FB_WORK_SLICES) return; /* deterministic work ceiling also applies on device */
#ifndef GFX_COST_MODEL
            if(hal_micros()-t0>=1500u) return;
#endif
            if(result<0) break; /* changing first item cannot starve its neighbors */
        }
    }
    (void)t0;
}
int ui_fblur_ready(const void *id,int i0,int i1)
{
    if(id!=FB_view.id || !FB_storage) return 0;
    for(int i=i0;i<=i1;i++) {
        blur_item *s=fb_find(i);
        if(!fb_validate(s) || (FB_view.level && !s->level)) return 0;
    }
    return 1;
}
int ui_fblur_draw_at(gfx_fb *fb,const void *id,int i,int x,int y,float fade)
{
    unsigned observed_area=FB_observe_enabled?fb_observe_request(fb,id,x,y):0;
    if(id!=FB_view.id || !FB_storage) return 0;
    blur_item *s=fb_find(i);
    if(!fb_validate(s) || (FB_view.level && !s->level)) return 0;
    gfx_fb f=*fb;nos_ink_dim(&f,fade);
    sprite sp={FB_view.level?s->blur:s->sharp+(size_t)s->context*s->w,s->w,s->h};
    sprite_blit(&f,&sp,x,y,gfx_hash_u(gfx_hash_u(s->sig,s->generation),(unsigned)(FB_view.level?s->level:0)));
    if(observed_area && sp.px) {
        if(FB_view.level)FB_observation.blurred_processed_area+=observed_area;
        else FB_observation.sharp_processed_area+=observed_area;
    }
    if(FB_view.level) fb_cover_add(&f,x,y,s->w,s->h);
    return 1;
}
int ui_fblur_draw(gfx_fb *fb,const void *id,int i,int y,float fade)
{
    return ui_fblur_draw_at(fb,id,i,FB_view.X0,y,fade);
}


int ui_fblur_active(const void *id) {return id==FB_view.id && FB_view.level>0;}

int ui_fblur_any_active(void) {fb_cover_reset();return (FB_active_frame==nos_frame && FB_any_active) || (FB_cover_n && !FB_cover_cancelled);}

/* Native fallback paints each cold band exactly once, at its actual position.
 * Its geometry/press/fade comes from the original live body renderer rather
 * than the normalized cache-bake callback. */
int ui_fblur_draw_bands(gfx_fb *fb,const void *id,int first,int last,int top,int row_h,
                       float (*fade)(float),ui_blur_live_fn live,void *ctx)
{
    if(!fb || !live || row_h<=0 || last<first) return 0;
    int cached=0;
    for(int i=first;i<=last;i++) {
        int y=top+i*row_h;
        int y0=y+fb->oy,y1=y0+row_h;
        if(y0<0) y0=0;
        if(y1>GFX_H) y1=GFX_H;
        if(y0<fb->y_base) y0=fb->y_base;
        if(y1>fb->y_base+fb->h) y1=fb->y_base+fb->h;
        if(y1<=y0) continue;
        float ink=fade?fade(y+row_h*0.5f):1.0f;
        if(ui_fblur_draw(fb,id,i,y,ink)) {cached++;continue;}
        gfx_fb band=*fb;
        if(!fb->rec) {
            int stride=fb->stride?fb->stride:GFX_W;
            band.px=fb->px+(size_t)(y0-fb->y_base)*stride;
        }
        band.y_base=y0;band.h=y1-y0;
        int record_start=fb->rec?fb->rec->n:0;
        live(&band,ctx);
        /* Recorders traditionally describe primitive bounds before stripe clips.
         * Narrow only this callback's new commands to its nonoverlapping band. */
        if(fb->rec && !fb->rec->overflow) {
            gfx_rec *r=fb->rec;int out=record_start;
            for(int k=record_start;k<r->n && k<GFX_MAX_CMDS;k++) {
                gfx_cmd c=r->c[k];
                if(c.y0<y0) c.y0=y0;
                if(c.y1>y1) c.y1=y1;
                if(c.x1>c.x0 && c.y1>c.y0) r->c[out++]=c;
            }
            r->n=out;
        }
    }
    return cached;
}

/* Conservative next-Y bound, without adding an exposure-side mask. */
int ui_fblur_row_spans_run(int y,int spans[][2],int capacity,int *until) {
    int n=ui_fblur_row_spans(y,spans,capacity);
    if(until) {
        *until=(unsigned)y<GFX_H?y+1:GFX_H;
        if(FB_storage && spans && capacity>0 && !FB_cover_cancelled &&
           (unsigned)y<GFX_H && FB_span_valid && y>=FB_span_y0 && y<FB_span_y1)
            *until=FB_span_y1;
    }
    return n;
}
