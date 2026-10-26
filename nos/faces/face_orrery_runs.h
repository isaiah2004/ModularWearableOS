/* Tick-only Wu coverage; replay fetches only the requested rows. */
static int orr_baked_background;
static int orr_floor(float x) { int i=(int)x; return i-(x<i); }
static int orr_ceil(float x) { int i=(int)x; return i+(x>i); }
static unsigned orr_orbit_shape_key(void)
{
    unsigned h=0x0B17u;
    float f[]={S.tiltk,S.cam_tilt,S.az,S.tx};
    for(unsigned i=0;i<sizeof f/sizeof f[0];i++) h=gfx_hash_f(h,f[i]);
    h=gfx_hash_u(gfx_hash_u(h,(unsigned)S.view),(unsigned)S.from);
    h=gfx_hash_u(h,(unsigned)S.show[0]);
    return gfx_hash_u(h,(unsigned)(S.drag==3));
}
static unsigned orr_orbit_key(void)
{ return gfx_hash_f(gfx_hash_f(orr_orbit_shape_key(),S.zoom),S.ringe); }
static void orr_orbit_emit(orr_orbit_cache *c,int x,int y,float a,int colour)
{
    if(a<=0.02f) return;
    x-=c->origin_x; y-=c->origin_y;
    if((unsigned)(y+ORR_ORBIT_BIAS)>=ORR_ORBIT_ROWS || x<-32768 || x>32767 || c->n==c->capacity) {
        c->valid=0; return;
    }
    orr_orbit_pixel *p=&c->scratch[c->n++];
    p->x=(int16_t)x; p->y=(int16_t)y;
    /* Store the exact 0..32 blend lane weight used by gfx_blend565. */
    p->alpha=a>=0.98f?32:(uint8_t)(((unsigned)(a*255)+7)>>3);
    p->colour=(uint8_t)colour;
    c->row[y+ORR_ORBIT_BIAS+1]++;
}
static void orr_orbit_wu(orr_orbit_cache *c,float x0,float y0,float x1,float y1,int colour)
{
    float dx=x1-x0,dy=y1-y0;
    if(fabsf(dx)>=fabsf(dy)) {
        if(x0>x1) { float t=x0;x0=x1;x1=t;t=y0;y0=y1;y1=t;dx=-dx;dy=-dy; }
        float g=dx!=0?dy/dx:0;
        for(int x=orr_ceil(x0),end=orr_floor(x1);x<=end;x++) {
            float y=y0+g*(x-x0); int yi=orr_floor(y); float f=y-yi;
            orr_orbit_emit(c,x,yi,1-f,colour); orr_orbit_emit(c,x,yi+1,f,colour);
        }
    } else {
        if(y0>y1) { float t=x0;x0=x1;x1=t;t=y0;y0=y1;y1=t;dx=-dx;dy=-dy; }
        float g=dx/dy;
        for(int y=orr_ceil(y0),end=orr_floor(y1);y<=end;y++) {
            float x=x0+g*(y-y0); int xi=orr_floor(x); float f=x-xi;
            orr_orbit_emit(c,xi,y,1-f,colour); orr_orbit_emit(c,xi+1,y,f,colour);
        }
    }
}
/* Stable x order within each row preserves blending at ellipse joins. It
 * allows span replay to seek directly to its pixels, including translated pan. */
static unsigned orr_orbit_lower(const orr_orbit_cache *c,unsigned a,unsigned z,int x)
{
    while(a<z) { unsigned m=a+(z-a)/2; if(c->px[m].x<x)a=m+1; else z=m; }
    return a;
}
/* Coverage and ink are optional accelerators. Admission is tick-only and must
 * not turn an expected cache miss into repeated allocator failures. The direct
 * retained commands remain available until there is room to publish a cache. */
static int orr_orbit_room(uint32_t bytes)
{
#ifdef NOS_PSRAM_TRACKING
    return nos_mem_available() >= bytes;
#else
    (void)bytes;
    return 1;
#endif
}
static void orr_orbits_build(void)
{
    if(!S.orbit_pixels) {
        if(!orr_orbit_room(sizeof *S.orbit_pixels)) return;
        S.orbit_pixels=hal_big_alloc(sizeof *S.orbit_pixels);
        if(!S.orbit_pixels) return;
        memset(S.orbit_pixels,0,sizeof *S.orbit_pixels);
    }
    orr_orbit_cache *c=S.orbit_pixels;
    unsigned key=orr_orbit_key();
    float fx=S.panx-floorf(S.panx+0.5f),fy=S.pany-floorf(S.pany+0.5f);
    /* Zoom is an explicitly temporary nearest-coverage preview. Keep the
     * complete source shape, including offscreen rows, until release. */
    int scaling=S.drag==2 || S.drag==4 || S.drag==5 || S.ta_n || S.vel!=0 || S.belt_left || S.small_left || (S.ringk>0 && S.ringk<1);
    if(c->valid && (key==c->key || (scaling && c->shape_key==orr_orbit_shape_key()))) return;
    orr_cat(OP_ORBITS); orr_prof_start(OP_BUCKET);
    /* Complete ellipses, including pixels a subsequent pan reveals. */
    int step=S.drag==3?2:1;
    int need=0;
    for(int p=0;p<EP_N;p++) for(int i=0;i<NORB;i+=step) {
        int j=(i+step)%NORB;
        const float *a=S.orbit_shift[p][i],*b=S.orbit_shift[p][j];
        need+=2*(orr_ceil(fmaxf(fabsf(a[0]-b[0]),fabsf((a[1]+a[2])-(b[1]+b[2]))))+3);
    }
    c->valid=0;
    if(need>c->capacity) {
        int cap=(need+4095)&~4095;
        /* The old coverage is invalid; do not hold both generations while growing. */
        hal_big_free(c->px); hal_big_free(c->scratch);
        hal_big_free(c->ink);c->ink=0;c->ink_key=0;
        c->px=c->scratch=0; c->capacity=0;
        uint32_t bytes=(uint32_t)cap*sizeof(orr_orbit_pixel);
        /* Leave room for both allocations and their tracking/alignment overhead. */
        if(!orr_orbit_room(2u*bytes+16u)) { orr_prof_stop(); return; }
        orr_orbit_pixel *a=hal_big_alloc(bytes),*b=0;
        if(a && orr_orbit_room(bytes)) b=hal_big_alloc(bytes);
        if(!a || !b) { hal_big_free(a); hal_big_free(b); orr_prof_stop(); return; }
        c->px=a; c->scratch=b; c->capacity=cap;
    }
    if (!c->scratch) {
        if(!orr_orbit_room(c->capacity*sizeof *c->scratch)) { orr_prof_stop(); return; }
        c->scratch=hal_big_alloc(c->capacity*sizeof *c->scratch);
        if (!c->scratch) { orr_prof_stop(); return; }
    }
    c->n=0; c->valid=1; c->phase_x=fx; c->phase_y=fy;
    c->origin_x=CX+orr_floor(S.panx+0.5f); c->origin_y=CY+orr_floor(S.pany+0.5f);
    memset(c->row,0,sizeof c->row);
    for(int p=0;p<EP_N;p++) {
        if(!S.pl_on[p] && p!=EP_EARTH) continue;
        if((p>EP_MARS && zone_mix(V_INNER)>0.5f) || (p<=EP_MARS && zone_mix(V_BEYOND)>0.5f)) continue;
        int colour=(p<=EP_MARS && zone_mix(V_INNER)>0.5f) || (p>EP_MARS && zone_mix(V_OUTER)>0.5f);
        for(int i=0;i<NORB;i+=step) {
            int j=(i+step)%NORB;
            const float *a=S.orbit_shift[p][i],*b=S.orbit_shift[p][j];
            orr_orbit_wu(c,(CX+S.panx)-a[0],((CY+S.pany)-a[1])-a[2],
                          (CX+S.panx)-b[0],((CY+S.pany)-b[1])-b[2],colour);
        }
    }
    /* Two stable radix passes on signed x, then the existing stable row
     * scatter: O(n), no allocation and no quadratic sort at shallow tilts. */
    for(int shift=0;shift<16;shift+=8) {
        orr_orbit_pixel *src=shift?c->px:c->scratch,*dst=shift?c->scratch:c->px;
        memset(c->next,0,256*sizeof c->next[0]);
        for(int i=0;i<c->n;i++) c->next[(((uint16_t)src[i].x^0x8000u)>>shift)&255u]++;
        unsigned sum=0;
        for(int i=0;i<256;i++) { unsigned n=c->next[i]; c->next[i]=sum; sum+=n; }
        for(int i=0;i<c->n;i++) dst[c->next[(((uint16_t)src[i].x^0x8000u)>>shift)&255u]++]=src[i];
    }
    for(int y=1;y<=ORR_ORBIT_ROWS;y++) c->row[y]+=c->row[y-1];
    memcpy(c->next,c->row,sizeof c->next);
    for(int i=0;i<c->n;i++) { const orr_orbit_pixel *p=&c->scratch[i]; c->px[c->next[p->y+ORR_ORBIT_BIAS]++]=*p; }
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,80*(2*c->n+512));
    c->key=key; c->shape_key=orr_orbit_shape_key(); c->zoom=S.zoom; c->shrink=S.sc; c->builds++;
    GFX_COST_PX(GFX_COST_ORR_BUILD,EP_N*NORB/step+c->n/8);
    orr_prof_stop();
}
static void orr_orbits_ink(void)
{
    orr_orbit_cache *c=S.orbit_pixels;
    if(!c || !c->valid || S.zoom!=c->zoom || S.sc!=c->shrink)return;
    unsigned key=gfx_hash_u(gfx_hash_u(gfx_hash_u(c->builds,C_BG),C_LINE),C_LINE2);
    if(c->ink && c->ink_key==key)return;
    if(!c->ink && !orr_orbit_room(c->capacity*sizeof *c->ink))return;
    if(!c->ink)c->ink=hal_big_alloc(c->capacity*sizeof *c->ink);
    if(!c->ink)return;
    uint16_t colours[2]={C_LINE,C_LINE2};
    for(int y=0;y<ORR_ORBIT_ROWS;y++) {
        int last=-32769;uint16_t ink=C_BG;
        for(unsigned i=c->row[y];i<c->row[y+1];i++) {
            const orr_orbit_pixel *p=c->px+i;
            if(p->x!=last)ink=C_BG;
            last=p->x;
            ink=p->alpha==32?colours[p->colour]:gfx_blend565(ink,colours[p->colour],p->alpha*8);
            c->ink[i]=(orr_orbit_ink){p->x,ink};
        }
    }
    c->ink_key=key;
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,500*c->n+80*ORR_ORBIT_ROWS);
}
static void orr_orbits_scaled(gfx_fb *fb,int lo,int hi)
{
    const orr_orbit_cache *c=S.orbit_pixels;
    float scale=(S.zoom/c->zoom)*(S.sc/c->shrink), inv=1/scale;
    float cx=CX+S.panx,cy=CY+S.pany;
    int x0=fb->cx0,x1=fb->cx1; if(x1<=x0){x0=0;x1=GFX_W;}
    uint16_t colour[2]={gfx_ink(fb,C_LINE),gfx_ink(fb,C_LINE2)};
    unsigned visits=0,writes=0;
    for(int y=lo;y<hi;y++) {
        int sy=orr_floor((y-cy)*inv+c->phase_y+0.5f)+ORR_ORBIT_BIAS;
        if((unsigned)sy>=ORR_ORBIT_ROWS)continue;
        uint16_t *row=gfx_row(fb,y+fb->oy-fb->y_base);
        int sx0=orr_floor((x0-fb->ox-cx)*inv+c->phase_x-2);
        int sx1=orr_ceil((x1-fb->ox-cx)*inv+c->phase_x+2);
        unsigned first=orr_orbit_lower(c,c->row[sy],c->row[sy+1],sx0);
        unsigned end=orr_orbit_lower(c,first,c->row[sy+1],sx1);
        for(unsigned i=first;i<end;i++) {
            const orr_orbit_pixel *p=&c->px[i]; visits++;
            int a=orr_floor(cx+(p->x-c->phase_x-0.5f)*scale+0.5f)+fb->ox;
            int z=orr_floor(cx+(p->x-c->phase_x+0.5f)*scale+0.5f)+fb->ox;
            if(a<x0)a=x0;
            if(z>x1)z=x1;
            if(a<0)a=0;
            if(z>GFX_W)z=GFX_W;
            for(int x=a;x<z;x++) {
                unsigned alpha=p->alpha;
                if(alpha==32) row[x]=colour[p->colour];
                else {
                    uint32_t src=(colour[p->colour]|((uint32_t)colour[p->colour]<<16))&0x07E0F81F;
                    uint32_t dst=(row[x]|((uint32_t)row[x]<<16))&0x07E0F81F;
                    uint32_t r=((src*alpha+dst*(32-alpha)+0x02008010u)>>5)&0x07E0F81F;
                    row[x]=(uint16_t)(r|(r>>16));
                }
                writes++;
            }
        }
    }
    GFX_COST_PX(GFX_COST_ORR_RUN,visits);
    /* Scaling/address generation plus variable-width preview writes. */
    GFX_COST_PX(GFX_COST_ORR_CPU,visits*120+writes*100);
}
static void orr_orbits_replay(gfx_fb *fb,int lo,int hi)
{
    const orr_orbit_cache *c=S.orbit_pixels;
    if(S.zoom!=c->zoom || S.sc!=c->shrink) { orr_orbits_scaled(fb,lo,hi); return; }
    int dx=CX+orr_floor(S.panx-c->phase_x+0.5f),dy=CY+orr_floor(S.pany-c->phase_y+0.5f);
    int a=lo-dy+ORR_ORBIT_BIAS,z=hi-dy+ORR_ORBIT_BIAS;
    if(a<0)a=0;
    if(z>ORR_ORBIT_ROWS)z=ORR_ORBIT_ROWS;
    if(a>=z) return;
    int x0=fb->cx0,x1=fb->cx1; if(x1<=x0){x0=0;x1=GFX_W;}
    uint16_t colour[2]={gfx_ink(fb,C_LINE),gfx_ink(fb,C_LINE2)};
    uint32_t lanes[2];
    for(int i=0;i<2;i++) lanes[i]=(colour[i]|((uint32_t)colour[i]<<16))&0x07E0F81F;
    unsigned visits=0;
    /* Orbits are the first layer over C_BG. Store the exact repeated AA
     * composition once; dimmed/transitional layers retain coverage blending. */
    int baked=orr_baked_background && !fb->dim && c->ink && c->ink_key==gfx_hash_u(gfx_hash_u(gfx_hash_u(c->builds,C_BG),C_LINE),C_LINE2);
    for(int sy=a;sy<z;sy++) {
        unsigned first=orr_orbit_lower(c,c->row[sy],c->row[sy+1],x0-dx-fb->ox);
        unsigned end=orr_orbit_lower(c,first,c->row[sy+1],x1-dx-fb->ox);
        uint16_t *row=gfx_row(fb,sy-ORR_ORBIT_BIAS+dy+fb->oy-fb->y_base);
        visits+=end-first;
        if(baked) {
            for(unsigned i=first;i<end;i++) {
                int x=c->ink[i].x+dx+fb->ox;
                if((unsigned)x<GFX_W)row[x]=c->ink[i].ink;
            }
            continue;
        }
        for(unsigned i=first;i<end;i++) {
            const orr_orbit_pixel *p=&c->px[i];
            int x=p->x+dx+fb->ox;
            if((unsigned)x>=GFX_W)continue;
            uint16_t *dst=&row[x];
            unsigned alpha=p->alpha;
            if(alpha==32) *dst=colour[p->colour];
            else {
                uint32_t d=(*dst|((uint32_t)*dst<<16))&0x07E0F81F;
                uint32_t r=((lanes[p->colour]*alpha+d*(32-alpha)+0x02008010u)>>5)&0x07E0F81F;
                *dst=(uint16_t)(r|(r>>16));
            }
        }
    }
    if(baked)GFX_COST_PX(GFX_COST_ORR_CPU,120*visits);
    else GFX_COST_PX(GFX_COST_ORR_RUN,visits);
    GFX_COST_PX(GFX_COST_ORR_CPU,(z-a)*1200); /* row seeks/address setup */
}
