/* Per-ring tick coverage. Rotate the polar basis once in capture, then retain
 * exact Wu blend lanes in row order. Replay does no slope/trig/line setup.
 * One shared build workspace; each optional cache is below 64 KiB. */
#define ORR_TICK_PIXELS 8192
typedef struct { uint16_t x; uint8_t alpha, major; } orr_tick_pixel;
typedef struct {
    orr_tick_pixel px[ORR_TICK_PIXELS];
    uint16_t row[GFX_H+1], next[GFX_H];
    unsigned key, builds;
    int n,valid;
} orr_tick_cache;
static orr_tick_cache *orr_ticks[3];
static struct { orr_tick_pixel px; uint16_t y; } *orr_tick_work;
static int orr_ticks_begin(int ring,unsigned key)
{
    orr_tick_cache *c=orr_ticks[ring];
    if(c && c->valid && c->key==key)return 0;
    if(!c) {
        c=hal_big_alloc(sizeof *c);orr_ticks[ring]=c;
        if(c)memset(c,0,sizeof *c);
    }
    if(c)c->valid=0;
    if(!orr_tick_work)orr_tick_work=hal_big_alloc(ORR_TICK_PIXELS*sizeof *orr_tick_work);
    if(!c || !orr_tick_work)return -1;
    c->key=key;c->n=0;c->valid=0;
    memset(c->row,0,sizeof c->row);
    return 1;
}
static void orr_tick_emit(orr_tick_cache *c,int x,int y,float a,int major)
{
    if((unsigned)x>=GFX_W || (unsigned)y>=GFX_H || a<=.02f)return;
    if(c->n>=ORR_TICK_PIXELS) { c->n=ORR_TICK_PIXELS+1;return; }
    orr_tick_work[c->n].px=(orr_tick_pixel){(uint16_t)x,a>=.98f?32:(uint8_t)(((unsigned)(a*255)+7)>>3),(uint8_t)major};
    orr_tick_work[c->n++].y=(uint16_t)y;c->row[y+1]++;
}
static void orr_tick_line(int ring,float x0,float y0,float x1,float y1,int major)
{
    orr_tick_cache *c=orr_ticks[ring];
    float dx=x1-x0,dy=y1-y0;
    if(fabsf(dx)>=fabsf(dy)) {
        if(x0>x1) { float t=x0;x0=x1;x1=t;t=y0;y0=y1;y1=t;dx=-dx;dy=-dy; }
        float g=dx!=0?dy/dx:0;
        for(int x=orr_ceil(x0),end=orr_floor(x1);x<=end;x++) {
            float y=y0+g*(x-x0);int yi=orr_floor(y);float f=y-yi;
            orr_tick_emit(c,x,yi,1-f,major);orr_tick_emit(c,x,yi+1,f,major);
        }
    } else {
        if(y0>y1) { float t=x0;x0=x1;x1=t;t=y0;y0=y1;y1=t;dx=-dx;dy=-dy; }
        float g=dx/dy;
        for(int y=orr_ceil(y0),end=orr_floor(y1);y<=end;y++) {
            float x=x0+g*(y-y0);int xi=orr_floor(x);float f=x-xi;
            orr_tick_emit(c,xi,y,1-f,major);orr_tick_emit(c,xi+1,y,f,major);
        }
    }
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,1400);
}
static void orr_ticks_finish(int ring)
{
    orr_tick_cache *c=orr_ticks[ring];
    if(c->n>ORR_TICK_PIXELS)return;
    for(int y=1;y<=GFX_H;y++)c->row[y]+=c->row[y-1];
    memcpy(c->next,c->row,sizeof c->next);
    for(int i=0;i<c->n;i++)c->px[c->next[orr_tick_work[i].y]++]=orr_tick_work[i].px;
    c->valid=1;c->builds++;
    /* Wu samples plus sequential scratch/scatter and row prefix sums. */
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,450*c->n+80*GFX_H);
}
static int orr_ticks_draw_ink(gfx_fb *fb,int ring,int override)
{
    const orr_tick_cache *c=orr_ticks[ring];
    if(!c || !c->valid)return 0;
    int lo=fb->y_base-fb->oy,hi=lo+fb->h;
    if(lo<0)lo=0;
    if(hi>GFX_H)hi=GFX_H;
    int x0=fb->cx0,x1=fb->cx1;if(x1<=x0){x0=0;x1=GFX_W;}
    uint16_t ink[2]={gfx_ink(fb,GFX_RGB(0x2c,0x2c,0x2c)),gfx_ink(fb,GFX_RGB(0x44,0x44,0x44))};
    if(override>=0)ink[0]=ink[1]=gfx_ink(fb,(uint16_t)override);
    unsigned visits=0;
    for(int y=lo;y<hi;y++) {
        uint16_t *dst=gfx_row(fb,y+fb->oy-fb->y_base);
        for(int i=c->row[y];i<c->row[y+1];i++) {
            const orr_tick_pixel *p=c->px+i;int x=p->x+fb->ox;
            if(x<x0 || x>=x1 || (unsigned)x>=GFX_W)continue;
            dst[x]=p->alpha==32?ink[p->major]:gfx_blend565(dst[x],ink[p->major],(uint8_t)(p->alpha*8));visits++;
        }
    }
    GFX_COST_PX(GFX_COST_ORR_RUN,visits);
    return 1;
}

static int orr_ticks_draw(gfx_fb *fb,int ring)
{ return orr_ticks_draw_ink(fb,ring,-1); }

/* Opening sweep: three concentric Wu hairlines, <=0.08 px chord error.
 * Coverage is built once per frame, never an annular scan per stripe.
 * These temporary samples share the ring tick storage; settled bands retain
 * the original exact arc mask. */
static int orr_sweep_prepare(int ring,float radius,float a0,float a1,float width)
{
    unsigned key=gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(0x5AEEu,radius),a0),a1),width);
    int build=orr_ticks_begin(ring,key);
    if(build<0)return 0;
    if(build) {
        int lines=width>2?3:1;
        int steps=(int)ceilf((a1-a0)/3);
        for(int line=0;line<lines;line++) {
            float r=radius+line-(lines-1)*.5f;
            float x0,y0;polar_xy(a0,r,&x0,&y0);
            for(int j=1;j<=steps;j++) {
                float x1,y1;polar_xy(a0+(a1-a0)*j/steps,r,&x1,&y1);
                orr_tick_line(ring,x0,y0,x1,y1,0); x0=x1;y0=y1;
            }
        }
        orr_ticks_finish(ring);
    }
    return orr_ticks[ring]->valid;
}

/* The six stationary bezel edges are one sparse coverage stream, baked once
 * in tick. Keeping exact blend lanes avoids a frame-sized RGB cache. */
#define ORR_BAND_PIXELS 16000
static struct orr_band_cache {
    struct { uint16_t x; uint8_t alpha, pad; } px[ORR_BAND_PIXELS];
    uint16_t row[GFX_H+1];
    int ready, baked, n;
} *orr_band;
/* Fixed-size optional cache, like the face's existing geometry caches. */
static void *orr_band_alloc(void)
{
    return sizeof *orr_band <= 64 * 1024 ? hal_big_alloc(sizeof *orr_band) : 0;
}
static void orr_band_prepare(void)
{
    if(orr_band && orr_band->ready)return;
    if(!orr_band) {
        orr_band=orr_band_alloc();
        if(orr_band)memset(orr_band,0,sizeof *orr_band);
    }
    if(!orr_band)return;
    orr_arc_geom shape[6];
    for(int i=0;i<3;i++)for(int edge=0;edge<2;edge++)
        orr_arc_prepare(&shape[i*2+edge],CX,CY,RING_R[i]+(edge?1:-1)*RING_W/2,
                        1.2f,(-180+56)*D2RF,(180-56)*D2RF);
    uint16_t scratch[GFX_W];
    gfx_fb f={0}; f.px=scratch; f.h=1; f.cx1=GFX_W;
    int n=orr_band->n, end=orr_band->baked+32;
    if(end>GFX_H)end=GFX_H;
    int rows=end-orr_band->baked, old_n=n;
    for(int y=orr_band->baked;y<end;y++) {
        memset(scratch,0,sizeof scratch); f.y_base=y;
        for(int i=0;i<6;i++)orr_arc_replay(&f,&shape[i],0x07e0);
        orr_band->row[y]=(uint16_t)n;
        for(int x=0;x<GFX_W;x++)if(scratch[x]) {
            if(n==ORR_BAND_PIXELS) {
                free(orr_band); orr_band=0; return;
            }
            unsigned a=(((scratch[x]>>5)&63)+1)/2;
            orr_band->px[n].x=(uint16_t)x;
            orr_band->px[n++].alpha=(uint8_t)(a==32?255:a*8);
        }
    }
    orr_band->row[end]=(uint16_t)n; orr_band->n=n; orr_band->baked=end;
    if(end==GFX_H) { orr_band->ready=1; if(S.ringe>0)S.scene_valid=0; }
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,30*GFX_W*rows+100*(n-old_n)+12000);
}
static void orr_band_draw(gfx_fb *fb,uint16_t ink)
{
    if(!orr_band || !orr_band->ready)return;
    if(gfx_rec_cmd(fb,0,0,GFX_W,GFX_H,gfx_hash_u(0xBA4D,ink)))return;
    int lo=fb->y_base-fb->oy,hi=lo+fb->h;
    if(lo<0)lo=0;
    if(hi>GFX_H)hi=GFX_H;
    int x0=fb->cx0,x1=fb->cx1; if(x1<=x0){x0=0;x1=GFX_W;}
    ink=gfx_ink(fb,ink);
    unsigned n=0;
    for(int y=lo;y<hi;y++) {
        uint16_t *dst=gfx_row(fb,y+fb->oy-fb->y_base);
        for(int i=orr_band->row[y];i<orr_band->row[y+1];i++) {
            int x=orr_band->px[i].x+fb->ox;
            if(x<x0 || x>=x1 || (unsigned)x>=GFX_W)continue;
            uint8_t a=orr_band->px[i].alpha;
            dst[x]=a==255?ink:gfx_blend565(dst[x],ink,a); n++;
        }
    }
    GFX_COST_PX(GFX_COST_ORR_RUN,n);
}

/* Selected sectors: four vertical samples and exact horizontal coverage.
 * Radial roots depend only on band size, and are cached across scrubbing.
 * Each row has two solid spans and small AA edges: no mask scan of interiors. */
static void orr_hot_prepare(orr_hot_mask *m,float r,float w,float phi,float half)
{
    if(m->ready && m->radius==r && m->thickness==w && m->phi==phi && m->half==half)return;
    float ri=r-w/2,ro=r+w/2;
    int top=(int)floorf(CY-ro);
    if(!m->radial_ready || m->radius!=r || m->thickness!=w) {
        for(int y=0;y<ORR_HOT_H;y++)for(int s=0;s<4;s++) {
            float dy=CY-(top+y+(s+.5f)/4);
            m->radial[y][s][0]=sqrtf(fmaxf(0,ro*ro-dy*dy));
            m->radial[y][s][1]=sqrtf(fmaxf(0,ri*ri-dy*dy));
        }
        m->radial_ready=1;
        GFX_COST_PX(GFX_COST_ORR_PRE_CPU,7200*ORR_HOT_H);
    }
    m->ready=0;m->radius=r;m->thickness=w;m->phi=phi;m->half=half;
    float a=phi-half,b=phi+half;
    float sa=sinf(a),sb=sinf(b),ca=cosf(a),cb=cosf(b);
    float ta=tanf(a),tb=tanf(b);
    m->x=(int)floorf(CX+fminf(sa*ri,sa*ro));m->y=top;
    m->w=(int)ceilf(CX+fmaxf(sb*ri,sb*ro))-m->x;
    m->h=(int)ceilf(CY-ri*fminf(ca,cb))-top;
    if(m->w<=0 || m->h<=0 || m->w>ORR_HOT_W || m->h>ORR_HOT_H)return;
    unsigned edges=0;
    for(int y=0;y<m->h;y++)for(int k=0;k<2;k++) {
        float left[4],right[4],minleft=1e9f,maxleft=-1e9f,minright=1e9f,maxright=-1e9f;
        for(int s=0;s<4;s++) {
            float dy=CY-(top+y+(s+.5f)/4);
            float xo=m->radial[y][s][0],xi=m->radial[y][s][1];
            left[s]=fmaxf(k?xi:-xo,dy*ta)+CX-m->x;
            right[s]=fminf(k?xo:-xi,dy*tb)+CX-m->x;
            minleft=fminf(minleft,left[s]);maxleft=fmaxf(maxleft,left[s]);
            minright=fminf(minright,right[s]);maxright=fmaxf(maxright,right[s]);
        }
        int lo=(int)floorf(minleft),hi=(int)ceilf(maxright);
        int sl=(int)ceilf(maxleft),sh=(int)floorf(minright);
        if(lo<0)lo=0;
        if(hi>m->w)hi=m->w;
        if(hi<lo)hi=lo=0;
        if(sl<lo)sl=lo;
        if(sl>hi)sl=hi;
        if(sh<sl)sh=sl;
        if(sh>hi)sh=hi;
        m->row[y][k].lo=(uint8_t)lo;m->row[y][k].hi=(uint8_t)hi;
        m->row[y][k].solid_lo=(uint8_t)sl;m->row[y][k].solid_hi=(uint8_t)sh;
        for(int x=lo;x<hi;x++) {
            if(x==sl && sh>sl) { x=sh-1;continue; }
            float coverage=0;
            for(int s=0;s<4;s++)coverage+=fmaxf(0,fminf(x+1,right[s])-fmaxf((float)x,left[s]));
            int alpha=(int)(coverage*63.75f+.5f);
            m->alpha[y][x]=(uint8_t)(alpha>255?255:alpha);edges++;
        }
    }
    m->ready=1;
    /* Actual work: intersections per row, four coverage samples per AA edge,
     * six trig calls. Core pixels are never baked or fetched from the mask. */
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,2400*m->h+180*edges+18000);
}
static void orr_hot_draw(gfx_fb *fb,const orr_hot_mask *m,uint16_t ink)
{
    if(!m->ready)return;
    unsigned key=gfx_hash_f(gfx_hash_f(gfx_hash_u(0x807,ink),m->phi),m->half);
    key=gfx_hash_f(gfx_hash_f(key,m->radius),m->thickness);
    if(gfx_rec_cmd(fb,m->x,m->y,m->x+m->w,m->y+m->h,key))return;
    int lo=fb->y_base-fb->oy-m->y,hi=lo+fb->h;
    if(lo<0)lo=0;
    if(hi>m->h)hi=m->h;
    int x0=fb->cx0,x1=fb->cx1;if(x1<=x0){x0=0;x1=GFX_W;}
    if(x0<0)x0=0;
    if(x1>GFX_W)x1=GFX_W;
    x0-=m->x+fb->ox;x1-=m->x+fb->ox;
    ink=gfx_ink(fb,ink);unsigned edges=0,solid=0;
    for(int y=lo;y<hi;y++) {
        uint16_t *dst=gfx_row(fb,m->y+y+fb->oy-fb->y_base);
        for(int k=0;k<2;k++) {
            int a=m->row[y][k].lo,b=m->row[y][k].hi;
            int sl=m->row[y][k].solid_lo,sh=m->row[y][k].solid_hi;
            if(a<x0)a=x0;
            if(b>x1)b=x1;
            if(sl<a)sl=a;
            if(sh>b)sh=b;
            for(int x=sl;x<sh;x++)dst[m->x+x+fb->ox]=ink;
            if(sh>sl)solid+=sh-sl;
            for(int x=a;x<b;x++) {
                if(x==sl && sh>sl) { x=sh-1;continue; }
                uint8_t alpha=m->alpha[y][x];int px=m->x+x+fb->ox;
                if(alpha)dst[px]=gfx_blend565(dst[px],ink,alpha);
                edges++;
            }
        }
    }
    GFX_COST(GFX_COST_FILL,solid);
    GFX_COST_PX(GFX_COST_ORR_RUN,edges);
}
