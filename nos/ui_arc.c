#include "psram.h"
/* Exact retained masks at rest; fixed-point upright-atlas sampling in motion. */
#include "ui.h"
#include "hal.h"
#include "gfx_cost.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define ARC_SLOTS 384
#define ARC_SIDE 32
#define RAD (0.01745329251994329577f)
typedef struct {
    uint8_t lo[ARC_SIDE], hi[ARC_SIDE];
    uint8_t alpha[ARC_SIDE * ARC_SIDE];
} arc_mask;
/* Budgeted PSRAM: metadata is touched per glyph, never per output pixel.
 * Chained 256-bucket hash, <=384 entries; no tombstones after eviction. */
typedef struct {
    const font_t *font;
    uint32_t stamp;
    uint16_t key, next; /* byte code | quantised angle << 8; links are slot+1 */
    int8_t left, top;
    uint8_t w, h;
} arc_key;
typedef struct {
    arc_key keys[ARC_SLOTS];
    uint16_t buckets[256];
    uint8_t pinned[ARC_SLOTS];
} arc_index;
static arc_index *index_cache;
#define keys (index_cache->keys)
#define buckets (index_cache->buckets)
#define pinned (index_cache->pinned)
#define ARC_BUILD_LIMIT 24
#define ARC_DIRECT 65535u
static int moving, pending, frame_builds, allocation_failed;
#include "ui_arc_trig.h"
static arc_mask *cache;
/* Through the NOS PSRAM budget (owner "ui_arc"; nos/psram.c), capped at 448 KB of its own. */
static struct { uint32_t used, peak; } ui_arc_budget;
static void *ui_arc_alloc(uint32_t bytes)
{
    if (ui_arc_budget.used > 448 * 1024 || bytes > 448 * 1024 - ui_arc_budget.used) return 0;
    void *p = nos_mem_alloc(bytes, "ui_arc");
    if (p) {
        ui_arc_budget.used += bytes;
        if (ui_arc_budget.used > ui_arc_budget.peak) ui_arc_budget.peak = ui_arc_budget.used;
    }
    return p;
}
static void ui_arc_free(void *p, uint32_t bytes)
{
    if (!p) return;
    nos_mem_free(p);
    ui_arc_budget.used = bytes <= ui_arc_budget.used ? ui_arc_budget.used - bytes : 0;
}
static unsigned serial, epoch = 1, builds, hits;

static const font_glyph *arc_glyph(const font_t *f, unsigned code)
{
    int lo=0, hi=f->nglyphs;
    while(lo<hi) { int m=(lo+hi)/2;
        if(f->glyphs[m].code<code)lo=m+1; else hi=m; }
    return lo<f->nglyphs && f->glyphs[lo].code==code ? &f->glyphs[lo] : 0;
}
void ui_text_arc_begin(void)
{
    ++epoch;
    if(index_cache)memset(pinned,0,sizeof pinned);
    frame_builds=0; pending=0; allocation_failed=0;
}
int ui_text_arc_drop(void)
{
    if(index_cache)for(int i=0;i<ARC_SLOTS;i++) if(pinned[i])return 0;
    ui_arc_free(cache,sizeof(arc_mask)*ARC_SLOTS);
    cache=0;
    ui_arc_free(index_cache,sizeof *index_cache);index_cache=0;
    pending=0; ++epoch; return 1;
}
void ui_text_arc_stats(unsigned *bytes,unsigned *b,unsigned *h)
{ if(bytes)*bytes=ui_arc_budget.used;
        if(b)*b=builds;
        if(h)*h=hits; }
static int alpha_at(const font_t *f,const font_glyph *g,int x,int y)
{
    return (unsigned)x<g->w && (unsigned)y<g->h ? f->bitmap[g->bitmap_off+y*g->w+x] : 0;
}
void ui_text_arc_motion(int on) { moving=!!on; }
int ui_text_arc_pending(void) { return pending; }
/* Re-pin unchanged geometry. Validate slots before publishing: a previous
 * label may have evicted a mask. Direct previews rebuild at rest. */
int ui_text_arc_retain(ui_arc_text *run)
{
    if(!run->n || run->n>UI_ARC_GLYPHS || !run->font)return 0;
    for(int i=0;i<run->n;i++) {
        const ui_arc_glyph *p=run->glyph+i;
        if(p->glyph>=run->font->nglyphs)return 0;
        if(p->slot==ARC_DIRECT) { if(!moving)return 0;continue; }
        if(!cache || p->slot>=ARC_SLOTS || p->glyph>=run->font->nglyphs)return 0;
        const arc_key *k=keys+p->slot;
        if(k->font!=run->font || k->key!=(run->font->glyphs[p->glyph].code|(p->angle<<8)))return 0;
    }
    for(int i=0;i<run->n;i++)if(run->glyph[i].slot!=ARC_DIRECT) {
        unsigned slot=run->glyph[i].slot;pinned[slot]=1;keys[slot].stamp=++serial;
    }
    run->epoch=epoch;
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,250*run->n);
    return 1;
}
static int arc_round(float x)
{
    int i=(int)x; float f=x-i;
    return i+(f>=0.5f)-(f<=-0.5f);
}
static int arc_floor(float x) { int i=(int)x; return i-(x<i); }
static int arc_ceil(float x) { int i=(int)x; return i+(x>i); }
static float arc_mod(float x,float period)
{
    int neg=x<0; if(neg)x=-x;
    if(x>=period) {
        float step=period;
        while(step<=x*0.5f)step*=2;
        while(step>=period) { if(x>=step)x-=step; step*=0.5f; }
    }
    return neg && x ? period-x : x;
}
/* A fourth-order local rotation about a 2-degree table entry. The table is
 * sampled at the SAME float radian arguments as the old mask builder. Position
 * uses the original, unquantised angle; quantising the baseline moves pixels. */
static void arc_sincos(float x,float *sn,float *cs)
{
    int neg=x<0; if(neg)x=-x;
    if(x>360*RAD)x=arc_mod(x,360*RAD);
    int q=arc_round(x/(2*RAD));
    float d=x-q*2*RAD,s=arc_trig[q][0],c=arc_trig[q][1];
    *sn=s+d*(c+d*(-0.5f*s+d*(-c/6+d*s/24)));
    *cs=c+d*(-s+d*(-0.5f*c+d*(s/6+d*c/24)));
    if(neg)*sn=-*sn;
}
static unsigned key_bucket(const font_t *f,unsigned key)
{
    uintptr_t p=(uintptr_t)f;
    unsigned h=(unsigned)(p>>3)^key*2654435761u;
    h^=h>>16; h^=h>>8; return h&255;
}
static int mask_find(const font_t *f,unsigned key)
{
    if(!index_cache)return -1;
    unsigned probes=0;
    for(unsigned link=buckets[key_bucket(f,key)];link;link=keys[link-1].next) {
        arc_key *m=&keys[link-1]; ++probes;
        if(m->font==f && m->key==key) {
            m->stamp=++serial; pinned[link-1]=1; ++hits;
            GFX_COST_PX(GFX_COST_ORR_PRE_CPU,80*probes+100);
            return (int)link-1;
        }
    }
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,80*probes+100);
    return -1;
}
static int arc_bounds(const font_glyph *g,int angle,arc_key *out)
{
    float sn=arc_trig[angle][0],cs=arc_trig[angle][1];
    float gx=g->xoff-g->xadv*0.5f, gy=g->yoff;
    float x0=1e9f,y0=1e9f,x1=-1e9f,y1=-1e9f;
    for(int i=0;i<4;i++) {
        float x=gx+((i&1)?g->w+1:-1),y=gy+((i&2)?g->h+1:-1);
        float rx=x*cs-y*sn,ry=x*sn+y*cs;
        if(rx<x0)x0=rx;
        if(rx>x1)x1=rx;
        if(ry<y0)y0=ry;
        if(ry>y1)y1=ry;
    }
    int left=arc_floor(x0),top=arc_floor(y0);
    int w=arc_ceil(x1)-left,h=arc_ceil(y1)-top;
    if(w>ARC_SIDE || h>ARC_SIDE || left<-128 || top<-128 || left>127 || top>127)return 0;
    out->left=(int8_t)left; out->top=(int8_t)top; out->w=(uint8_t)w; out->h=(uint8_t)h;
    return 1;
}
static int mask_build(const font_t *f,const font_glyph *g,int angle,const arc_key *bounds)
{
    if(!index_cache) {
        if(allocation_failed)return -1;
        index_cache=ui_arc_alloc(sizeof *index_cache);
        if(!index_cache) { allocation_failed=1;return -1; }
        memset(index_cache,0,sizeof *index_cache);
    }
    if(!cache) {
        if(allocation_failed)return -1;
        cache=ui_arc_alloc(sizeof(arc_mask)*ARC_SLOTS);
        if(!cache) { allocation_failed=1; return -1; }
    }
    int slot=-1; unsigned age=0;
    for(int i=0;i<ARC_SLOTS;i++) {
        if(!pinned[i] && (slot<0 || !keys[i].font || serial-keys[i].stamp>age)) {
            slot=i; age=serial-keys[i].stamp;
            if(!keys[i].font)break;
        }
    }
    if(slot<0)return -1;
    arc_key *k=&keys[slot];
    if(k->font) {
        uint16_t *link=&buckets[key_bucket(k->font,k->key)];
        while(*link && *link!=slot+1)link=&keys[*link-1].next;
        if(*link)*link=k->next;
    }
    *k=*bounds; k->font=f; k->key=(uint16_t)(g->code|(angle<<8));
    unsigned bucket=key_bucket(f,k->key);
    k->next=buckets[bucket]; buckets[bucket]=(uint16_t)(slot+1);
    arc_mask *m=cache+slot;
    float sn=arc_trig[angle][0],cs=arc_trig[angle][1];
    float gx=g->xoff-g->xadv*0.5f,gy=g->yoff;
    int left=k->left,top=k->top,w=k->w,h=k->h;
    /* Preserve the old exact bilinear kernel and rounding. */
    int dx=arc_round(cs*65536),dy=arc_round(-sn*65536);
    unsigned samples=0;
    for(int y=0;y<h;y++) {
        int sx=arc_round(((left+0.5f)*cs+(top+y+0.5f)*sn-gx-0.5f)*65536);
        int sy=arc_round((-(left+0.5f)*sn+(top+y+0.5f)*cs-gy-0.5f)*65536);
        m->lo[y]=(uint8_t)w; m->hi[y]=0;
        for(int x=0;x<w;x++,sx+=dx,sy+=dy) {
            int ix=sx>>16,iy=sy>>16,fx=(sx>>8)&255,fy=(sy>>8)&255;
            if(ix < -1 || iy < -1 || ix >= g->w || iy >= g->h) {
                m->alpha[y*w+x]=0; continue;
            }
            ++samples;
            int a=alpha_at(f,g,ix,iy)*(256-fx)+alpha_at(f,g,ix+1,iy)*fx;
            int b=alpha_at(f,g,ix,iy+1)*(256-fx)+alpha_at(f,g,ix+1,iy+1)*fx;
            int v=(a*(256-fy)+b*fy+32768)>>16;
            m->alpha[y*w+x]=(uint8_t)v;
            if(v) { if(x<m->lo[y])m->lo[y]=(uint8_t)x; m->hi[y]=(uint8_t)(x+1); }
        }
    }
    k->stamp=++serial; pinned[slot]=1; ++builds; ++frame_builds;
    /* Same build price as baseline; removing work does not discount its price.
     * Internal victim scan is bounded to 384 compact records, only on builds. */
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,350*samples+40*w*h+12000+ARC_SLOTS*20);
    return slot;
}
int ui_text_arc_prepare(ui_arc_text *run,const font_t *f,float cx,float cy,
                        float radius,float angle,const char *s,int align,int tracking)
{
    memset(run,0,sizeof *run);
    if(!f || !s || !isfinite(radius) || radius<=0 || !isfinite(angle) ||
       !isfinite(cx) || !isfinite(cy) || fabsf(cx)>16000 || fabsf(cy)>16000 ||
       radius>16000 || strlen(s)>UI_ARC_GLYPHS || align<0 || align>2)return 0;
    angle=arc_mod(angle,360);
        if(angle<0)angle+=360;
    int reverse=0;   /* no bottom-half flip (the user's call): glyph tops face out all the way round, like a bezel */
    float direction=reverse?-1:1;
    float pen=-ui_text_width(f,s,tracking)*(align==UI_CENTER?0.5f:align==UI_RIGHT?1:0);
    run->x0=run->y0=32767; run->x1=run->y1=-32767; run->epoch=epoch; run->font=f;
    unsigned hash=0xA4C;
    for(const unsigned char *c=(const unsigned char *)s;*c;c++) {
        const font_glyph *g=arc_glyph(f,*c);
        int advance=g?g->xadv:f->line_height/3;
        float phi=angle+direction*(pen+advance*0.5f)/radius/RAD;
        pen+=advance+tracking;
        if(!g || !g->w || !g->h)continue;
        float rot=phi+(reverse?180:0);
        if(!isfinite(rot) || fabsf(rot)>1000000) { run->n=0; return 0; }
        int q=arc_round(rot/2); q=(q%180+180)%180;
        int slot=mask_find(f,(unsigned)(g->code|(q<<8)));
        arc_key bounds={0}; const arc_key *m;
        if(slot>=0) m=&keys[slot];
        else {
            if(!arc_bounds(g,q,&bounds)) { run->n=0; return 0; }
            m=&bounds;
            if(!moving && frame_builds<ARC_BUILD_LIMIT)slot=mask_build(f,g,q,m);
            /* Allocation failure and >384 live keys still have a drawable
             * fallback. Only budget deferral requests another settle tick. */
            if(slot<0 && !moving && frame_builds>=ARC_BUILD_LIMIT)pending=1;
        }
        ui_arc_glyph *p=&run->glyph[run->n++];
        float sn,cs; arc_sincos(phi*RAD,&sn,&cs);
        p->x=(int16_t)arc_round(cx+radius*sn+m->left);
        p->y=(int16_t)arc_round(cy-radius*cs+m->top);
        p->slot=slot<0?ARC_DIRECT:(uint16_t)slot;
        p->glyph=(uint16_t)(g-f->glyphs); p->angle=(uint8_t)q;
        p->left=m->left; p->top=m->top; p->w=m->w; p->h=m->h;
        if(p->x<run->x0)run->x0=p->x;
        if(p->y<run->y0)run->y0=p->y;
        if(p->x+m->w>run->x1)run->x1=p->x+m->w;
        if(p->y+m->h>run->y1)run->y1=p->y+m->h;
        hash=gfx_hash_u(gfx_hash_u(gfx_hash_u(hash,*c),q|(slot<0?256:0)),(unsigned)(p->x+65536*p->y));
    }
    run->hash=gfx_hash_u(hash,(unsigned)(uintptr_t)f);
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,2200*run->n); /* table rotation, hash lookup and run layout */
    return 1;
}
/* Intersect a 16.16 DDA row with the upright source rectangle before
 * sampling. The old path visited every corner of the rotated bounding box. */
static int arc_div_floor(int a,int b) { int q=a/b; return q-(a%b<0); }
static void arc_clip_dda(int origin,int step,int limit,int *lo,int *hi)
{
    if(!step) { if(origin<0 || origin>=limit)*hi=*lo; return; }
    int a,b;
    if(step>0) { a=-arc_div_floor(origin,step);b=arc_div_floor(limit-1-origin,step)+1; }
    else { step=-step;a=-arc_div_floor(limit-1-origin,step);b=arc_div_floor(origin,step)+1; }
    if(a>*lo)*lo=a;
    if(b<*hi)*hi=b;
}
static void arc_direct(gfx_fb *fb,const ui_arc_text *run,const ui_arc_glyph *p,
                       uint16_t color,int x0,int x1,int gx,int gy,int a,int z)
{
    if(a>=z)return;
    const font_glyph *g=run->font->glyphs+p->glyph;
    int cs=arc_round(arc_trig[p->angle][1]*65536);
    int sn=arc_round(arc_trig[p->angle][0]*65536);
    int l=x0-gx,r=x1-gx;
    if(l<0)l=0;
    if(r>p->w)r=p->w;
    if(l>=r)return;
    /* Pixel centres, nearest source sample; 16.16 DDA, no per-pixel float,
     * multiply, division or libm. Products fit int32 for <=32-pixel bounds. */
    int sx0=(p->left+l)*cs+(p->top+a)*sn+(cs+sn)/2-
            (g->xoff*65536-g->xadv*32768);
    int sy0=-(p->left+l)*sn+(p->top+a)*cs+(cs-sn)/2-g->yoff*65536;
    unsigned visits=0,pixels=0;
    for(int y=a;y<z;y++,sx0+=sn,sy0+=cs) {
        int lo=0,hi=r-l;
        arc_clip_dda(sx0,cs,g->w*65536,&lo,&hi);
        arc_clip_dda(sy0,-sn,g->h*65536,&lo,&hi);
        if(lo>=hi)continue;
        int sx=sx0+lo*cs,sy=sy0-lo*sn;
        uint16_t *dst=gfx_row(fb,gy+y);
        for(int x=l+lo;x<l+hi;x++,sx+=cs,sy-=sn) {
            int ix=sx>>16,iy=sy>>16; ++visits;
            uint8_t alpha=run->font->bitmap[g->bitmap_off+iy*g->w+ix];
            if(!alpha)continue;
            dst[gx+x]=alpha>=250?color:gfx_blend565(dst[gx+x],color,alpha);
            ++pixels;
        }
    }
    GFX_COST_PX(GFX_COST_MASK,pixels);
    /* Provisional board proxy: DDA/bounds plus upright flash alpha read.
     * Keep mask blend pricing identical to the exact path. */
    GFX_COST_PX(GFX_COST_ORR_CPU,100*visits+1200*(z-a));
}
void ui_text_arc(gfx_fb *fb,const ui_arc_text *run,uint16_t color)
{
    if(!run->n || run->epoch!=epoch)return;
    if(gfx_rec_cmd(fb,run->x0,run->y0,run->x1,run->y1,gfx_hash_u(run->hash,color)))return;
    color=gfx_ink(fb,color);
    int x0=fb->cx0,x1=fb->cx1;
        if(x1<=x0){x0=0;x1=GFX_W;}
    if(x0<0)x0=0;
        if(x1>GFX_W)x1=GFX_W;
    unsigned pixels=0,visits=0;
    for(int i=0;i<run->n;i++) {
        const ui_arc_glyph *p=&run->glyph[i]; const arc_mask *m=p->slot==ARC_DIRECT?0:cache+p->slot;
        int gx=p->x+fb->ox,gy=p->y+fb->oy-fb->y_base;
        int a=gy<0?-gy:0,z=gy+p->h>fb->h?fb->h-gy:p->h;
        if(!m) { arc_direct(fb,run,p,color,x0,x1,gx,gy,a,z); continue; }
        for(int y=a;y<z;y++) {
            int l=m->lo[y],r=m->hi[y];
            if(l<x0-gx)l=x0-gx;
        if(r>x1-gx)r=x1-gx;
            if(l>=r)continue;
            uint16_t *dst=gfx_row(fb,gy+y);
            for(int x=l;x<r;x++) {
                uint8_t alpha=m->alpha[y*p->w+x]; ++visits;
                if(!alpha)continue;
                dst[gx+x]=alpha>=250?color:gfx_blend565(dst[gx+x],color,alpha); ++pixels;
            }
        }
    }
    GFX_COST(GFX_COST_MASK,pixels);
    GFX_COST_PX(GFX_COST_ORR_CPU,70*visits); /* PSRAM coverage fetch, incl holes */
}
