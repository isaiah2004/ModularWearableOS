/* Private to face_orrery.c. Capture is tick-only; replay never scans another
 * stripe's items. A compact command plus uint16 indices replaces RGB565 pixels. */
enum { B_CIRCLE, B_DOT, B_RECT, B_PILL, B_RING, B_ARC, B_LINE, B_HAIR,
       B_LIT, B_SWEEP, B_BANDS, B_RTICKS, B_ATEXT, B_HOT, B_TEXT, B_LABEL, B_DTEXT, B_ZOOM, B_BELT, B_ORBIT, B_ORBIT_SEG };
/* The stripe references already live in the retained PSRAM list. A second
 * internal copy cost 2,176 bytes for only ~400 per-command reads per frame. */
static int orr_capture;
static void hairline(gfx_fb *, const fk_view *, float, float, float, float, uint16_t);
static void lit_disc(gfx_fb *, const fk_view *, float, float, float, float, float, uint16_t);
static void hl_begin(void);
static void hl_end(void);
#include "face_orrery_controls.h"
#include "face_orrery_ringcache.h"
#include "face_orrery_platecache.h"

static orr_cmd *orr_item_raw(int type, float y0, float y1, uint16_t ink)
{
    orr_list *l = S.list;
    if (y1 < 0 || y0 >= GFX_H || !isfinite(y0) || !isfinite(y1)) return 0;
    if (l->n == ORR_COMMANDS) { l->overflow = 1; return 0; }
    orr_cmd *q = &l->cmd[l->n++];
    q->x0=0; q->x1=GFX_W;
    q->category=(uint8_t)orr_category;
    q->type = (uint8_t)type; q->ink = ink; q->aux = 0;
    q->first = (uint8_t)(fmaxf(0, floorf(y0)) / 16);
    q->last = (uint8_t)(fminf(GFX_H - 1, ceilf(y1)) / 16);
    return q;
}
static orr_cmd *orr_item(int type,float y0,float y1,uint16_t ink)
{
    GFX_COST_PX(GFX_COST_ORR_CAPTURE,1);
    return orr_item_raw(type,y0,y1,ink);
}
static void orr_xbox(orr_cmd *q,float x0,float x1)
{
    if(q) { q->x0=(int16_t)clampf(floorf(x0),-32768,32767); q->x1=(int16_t)clampf(ceilf(x1),-32768,32767); }
}
static void orr_ring_highlight(gfx_fb *fb,int i,float r,float w,float phi,float half,uint16_t ink)
{
    if(!S.list)return;
    orr_hot_mask *m=&S.list->ring_hot[i];
    if(!orr_capture) { orr_hot_draw(fb,m,ink); return; }
    orr_hot_prepare(m,r,w,phi,half);
    if(!m->ready)return;
    orr_cmd *q=orr_item(B_HOT,m->y,m->y+m->h,ink);
    orr_xbox(q,m->x,m->x+m->w);
    if(q)q->aux=(uint16_t)i;
}
static void orr_round(int type, float x, float y, float r, float w, uint16_t c)
{
    orr_cmd *q = orr_item(type, y-r-w-2, y+r+w+2, c);
    orr_xbox(q,x-r-w-2,x+r+w+2);
    if (q) { q->f[0]=x; q->f[1]=y; q->f[2]=r; q->f[3]=w; }
}
static void orr_circle(gfx_fb *fb, float x, float y, float r, uint16_t c)
{ if (orr_capture) orr_round(B_CIRCLE,x,y,r,0,c); else gfx_fill_circle(fb,x,y,r,c); }
static void orr_dot(gfx_fb *fb, float x, float y, float d, uint16_t c)
{ if (orr_capture) orr_round(B_DOT,x,y,d/2,0,c); else dot_disc(fb,x,y,d,c); }
static void orr_ring(gfx_fb *fb, float x, float y, float r, float w, uint16_t c)
{ if (orr_capture) orr_round(B_RING,x,y,r,w/2,c); else gfx_ring(fb,x,y,r,w,c); }
static void orr_pill(gfx_fb *fb, float x, float y, float x1, float r, uint16_t c)
{
    if (!orr_capture) { gfx_fill_pill(fb,x,y,x1,r,c); return; }
    orr_cmd *q=orr_item(B_PILL,y-r-2,y+r+2,c);
    orr_xbox(q,fminf(x,x1)-r-2,fmaxf(x,x1)+r+2);
    if(q) { q->f[0]=x; q->f[1]=y; q->f[2]=x1; q->f[3]=r; }
}
static void orr_line_item(int type, float x, float y, float x1, float y1, float w, uint16_t c)
{
    orr_cmd *q=orr_item(type,fminf(y,y1)-w-2,fmaxf(y,y1)+w+2,c);
    orr_xbox(q,fminf(x,x1)-w-2,fmaxf(x,x1)+w+2);
    if(q) { q->f[0]=x; q->f[1]=y; q->f[2]=x1; q->f[3]=y1; q->f[4]=w; }
}
static void orr_line(gfx_fb *fb, float x, float y, float x1, float y1, float w, uint16_t c)
{ if (orr_capture) orr_line_item(B_LINE,x,y,x1,y1,w,c); else gfx_line(fb,x,y,x1,y1,w,c); }
static void orr_arc(gfx_fb *fb, float x, float y, float r, float w, float a, float b, uint16_t c)
{
    if (!orr_capture) { gfx_arc(fb,x,y,r,w,a,b,c); return; }
    /* Use the primitive's own recorder, including its approximate trig and caps. */
    /* Tick owns this recorder until the final retained damage pass. Reuse it
     * rather than zeroing a 6.4 KB local for every text/arc (or using task stack). */
    gfx_rec *rec=&S.scene_rec; rec->n=rec->overflow=0;
    gfx_fb rf={0}; rf.h=GFX_H; rf.cx1=GFX_W; rf.rec=rec;
    gfx_arc(&rf,x,y,r,w,a,b,c);
    if(!rec->n) return;
    orr_cmd *q=orr_item(B_ARC,(float)rec->c[0].y0-1,(float)rec->c[0].y1+1,c);
    orr_xbox(q,(float)rec->c[0].x0-1,(float)rec->c[0].x1+1);
    if(q) {
        q->f[0]=x; q->f[1]=y; q->f[2]=r; q->f[3]=w; q->f[4]=a; q->f[5]=b;
        if(S.list->na<ORR_ARCS && b>a && b-a<6.2831f) {
            q->aux=(uint16_t)(++S.list->na);
            orr_arc_prepare(&S.list->arc[q->aux-1],x,y,r,w,a,b);
        }
    }
}
static int orr_words_impl(gfx_fb *fb, int type, const font_t *font, const dot_style *ds,
                     float x, float y, const char *s, uint16_t c, int align, int tracking)
{
    if (!orr_capture) {
        if (type==B_DTEXT) return dot_text_a(fb,ds,x,y,s,align);
        if (type==B_LABEL) return ui_label(fb,(int)x,(int)y,s,c,align);
        return ui_text(fb,font,(int)x,(int)y,s,c,align,tracking);
    }
    /* Ask the real recorder for text bounds; stack strings are copied once. */
    /* Tick owns this recorder until the final retained damage pass. Reuse it
     * rather than zeroing a 6.4 KB local for every text/arc (or using task stack). */
    gfx_rec *rec=&S.scene_rec; rec->n=rec->overflow=0;
    gfx_fb rf={0}; rf.h=GFX_H; rf.cx1=GFX_W; rf.rec=rec;
    int width;
    if (type==B_DTEXT) width=dot_text_a(&rf,ds,x,y,s,align);
    else if (type==B_LABEL) width=ui_label(&rf,(int)x,(int)y,s,c,align);
    else width=ui_text(&rf,font,(int)x,(int)y,s,c,align,tracking);
    int y0=GFX_H, y1=0, x0=GFX_W, x1=0;
    for (int i=0;i<rec->n && i<GFX_MAX_CMDS;i++) { if(rec->c[i].y0<y0)y0=rec->c[i].y0; if(rec->c[i].y1>y1)y1=rec->c[i].y1; if(rec->c[i].x0<x0)x0=rec->c[i].x0; if(rec->c[i].x1>x1)x1=rec->c[i].x1; }
    if (!rec->n) return width;
    if (S.list->nt==ORR_TEXTS || strlen(s)>=sizeof S.list->text[0].s) { S.list->overflow=1; return width; }
    orr_cmd *q=orr_item(type,(float)y0-1,(float)y1+1,c);
    orr_xbox(q,(float)x0-1,(float)x1+1);
    if(q) {
        q->f[0]=x; q->f[1]=y; q->aux=(uint16_t)S.list->nt++;
        orr_text *t=&S.list->text[q->aux]; t->font=font;
        if(ds)t->dot=*ds;
        strcpy(t->s,s); t->align=align; t->tracking=tracking;
    }
    return width;
}
static int orr_words(gfx_fb *fb,int type,const font_t *font,const dot_style *ds,
                     float x,float y,const char *s,uint16_t c,int align,int tracking)
{
    int category=orr_category;
    orr_cat(OP_TEXT);
    int width=orr_words_impl(fb,type,font,ds,x,y,s,c,align,tracking);
    orr_cat(category);
    return width;
}
/* Run preparation is part of tick capture; replay only stamps pinned masks. */
static void orr_ring_text(gfx_fb *fb,int id,float radius,float phi,const char *s,uint16_t ink)
{
    if(!S.list)return; /* Optional retained allocation can fail under pressure. */
    ui_arc_text *run=&S.list->ring_text[id];
    if(!orr_capture) { ui_text_arc(fb,run,ink); return; }
    int category=orr_category; orr_cat(OP_TEXT);
    float baseline=radius-5;   /* labels never flip: one baseline all round */
    unsigned key=gfx_hash_f(gfx_hash_f(0xA4C1u,baseline),phi);
    for(const char *p=s;*p;p++)key=gfx_hash_u(key,(unsigned char)*p);
    uint32_t stamp=orr_prof.enabled?orr_prof_now():0;
    int retained=S.list->ring_text_key[id]==key && ui_text_arc_retain(run);
    if(orr_prof.enabled)orr_prof.arc_us[1]+=orr_prof_now()-stamp;
    stamp=orr_prof.enabled?orr_prof_now():0;
    int prepared=retained || ui_text_arc_prepare(run,UI_F_SMALL,CX,CY,baseline,phi,s,UI_CENTER,1);
    if(orr_prof.enabled)orr_prof.arc_us[0]+=orr_prof_now()-stamp;
    if(prepared && run->n) {
        S.list->ring_text_key[id]=key;
        orr_cmd *q=orr_item_raw(B_ATEXT,run->y0,run->y1,ink);
        orr_xbox(q,run->x0,run->x1);
        if(q)q->aux=(uint16_t)id;
    }
    orr_cat(category);
}
static int orr_text_draw(gfx_fb *fb,const font_t *f,int x,int y,const char *s,uint16_t c,int a,int t)
{ return orr_words(fb,B_TEXT,f,0,(float)x,(float)y,s,c,a,t); }
static int orr_label(gfx_fb *fb,int x,int y,const char *s,uint16_t c,int a)
{ return orr_words(fb,B_LABEL,0,0,(float)x,(float)y,s,c,a,0); }
static int orr_dot_text(gfx_fb *fb,const dot_style *st,float x,float y,const char *s,int a)
{ return orr_words(fb,B_DTEXT,0,st,x,y,s,st->color,a,0); }
static void orr_zoom(gfx_fb *fb,int i,float frac,uint16_t on,uint16_t off)
{
    float k=clampf(frac*20-i,0,1);
    int step=(int)(k*8+0.5f);
    if(!orr_capture) {
        if(!orr_zmask || !orr_zmask->ready[i][8] || (step && !orr_zmask->ready[i][step])) {
            prog_arc_segment(fb,PROG_SEGMENTS,CX,CY,ZOOM_R+1,9,ZB_P1*D2RF,ZB_P0*D2RF,20,frac,on,off,i);
        } else if(k>0 && k<1) {
            if(off)orr_zoom_mask(fb,i,8,off);
            orr_zoom_mask(fb,i,step,on);
        } else if(k>0 || off)orr_zoom_mask(fb,i,8,k>0?on:off);
        return;
    }
    orr_zoom_prepare(i,8); orr_zoom_prepare(i,step);
    float a=ZB_P1*D2RF+(ZB_P0-ZB_P1)*D2RF*((float)i+0.5f)/20;
    float y=CY-(ZOOM_R+1)*gfx_fast_cos(a);
    orr_cmd *q=orr_item(B_ZOOM,y-14,y+14,on);
    float x=CX+(ZOOM_R+1)*gfx_fast_sin(a); orr_xbox(q,x-14,x+14);
    if(q) { q->aux=off; q->f[0]=(float)i; q->f[1]=frac; }
}
static void orr_sweep(gfx_fb *fb,int ring,float r,float a0,float a1,uint16_t ink)
{
    if(orr_capture && orr_sweep_prepare(ring,r,a0,a1,3)) {
        orr_cmd *q=orr_item(B_SWEEP,0,GFX_H-1,ink); if(q)q->aux=(uint16_t)ring;
    } else orr_arc(fb,CX,CY,r,3,a0*D2RF,a1*D2RF,ink);
}
static uint16_t orr_belt_ink(int i)
{
    static const uint16_t shade[3]={GFX_RGB(0x77,0x77,0x77),GFX_RGB(0x4a,0x4a,0x4a),GFX_RGB(0x33,0x33,0x33)};
    return shade[S.bh[i]];
}
/* Quantise only refreshed catalog bodies. Row chains retain stable catalog
 * order; replay includes a halo for stamps crossing a stripe or glass edge. */
static int orr_pixel_make(orr_pixel *d,float x,float y,uint16_t ink,int shape,int layer)
{
    if(!(x>=-32760 && x<32760 && y>=-ORR_ORBIT_BIAS && y<ORR_ORBIT_BIAS)) return 0;
    d->x=(int16_t)orr_floor(x); d->y=(int16_t)orr_floor(y);
    d->ink=ink; d->shape=(uint8_t)shape; d->layer=(uint8_t)layer;
    return 1;
}
static uint16_t orr_belt_pixel_ink(int i)
{
    /* Preserve integrated light: bright 1.4 px AA discs become four dim
     * pixels; the two faint classes become one brighter pixel. */
    static const uint16_t ink[]={GFX_RGB(0x33,0x33,0x33),GFX_RGB(0x68,0x68,0x68),GFX_RGB(0x47,0x47,0x47)};
    return ink[S.bh[i]];
}
static void orr_dot_unlink(orr_list *l,int i)
{
    if(l->dot_scratch[i].layer==255)return;
    unsigned a=l->dot_prev[i],z=l->dot_link[i];
    if(a)l->dot_link[a-1]=(uint16_t)z;
    else l->dot_head[l->dot_scratch[i].y+ORR_ORBIT_BIAS]=(uint16_t)z;
    if(z)l->dot_prev[z-1]=(uint16_t)a;
    l->ndots--;
}
static void orr_dot_link(orr_list *l,int i)
{
    if(l->dot_scratch[i].layer==255)return;
    int row=l->dot_scratch[i].y+ORR_ORBIT_BIAS;
    unsigned a=0,z=l->dot_head[row],visits=0;
    while(z && z-1<(unsigned)i) { a=z;z=l->dot_link[z-1];visits++; }
    l->dot_prev[i]=(uint16_t)a;l->dot_link[i]=(uint16_t)z;
    if(a)l->dot_link[a-1]=(uint16_t)(i+1);else l->dot_head[row]=(uint16_t)(i+1);
    if(z)l->dot_prev[z-1]=(uint16_t)(i+1);
    l->ndots++;
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,180*visits+600);
}
static void orr_pixels_build(void)
{
    orr_list *l=S.list;
    unsigned style=gfx_hash_u(gfx_hash_u(gfx_hash_u(0xD07u,C_PRI),C_SEC),C_DIS);
    style=gfx_hash_u(style,S.show[1]);
    int partial=S.dot_slice && l->dot_quant_valid && l->dot_chain_valid && l->dot_style_key==style && l->dot_nbelt==S.nbelt;
    int count=partial?S.belt_count:S.nbelt;
    l->dot_panx=S.panx; l->dot_pany=S.pany; l->dot_zoom=S.zoom; l->dot_shrink=S.sc;
    /* Scratch is now indexed by catalog body, not compacted every frame.
     * Retain unchanged integer stamps; only the updated slice is quantised. */
    for(int j=0;j<count;j++) {
        int i=partial?S.belt_changed[j]:j;
        if(i>=ORR_DOTS)continue;
        orr_pixel *d=l->dot_scratch+i;
        orr_pixel next={0};
        if(!S.bk || !S.show[1] || !orr_pixel_make(&next,S.bxs[i],S.bys[i],orr_belt_pixel_ink(i),S.bh[i]==0,0))next.layer=255;
        /* A changed x/ink does not change the row's catalog ordering. Avoid
         * walking a dense row to reinsert its existing node at the same spot. */
        int same_row=partial && d->layer!=255 && next.layer!=255 && d->y==next.y;
        if(partial && !same_row)orr_dot_unlink(l,i);
        *d=next;
        if(partial && !same_row)orr_dot_link(l,i);
    }
    int n=S.nbelt<ORR_DOTS?S.nbelt:ORR_DOTS;
    if(partial)for(int i=n;i<l->dot_items;i++)orr_dot_unlink(l,i);
    S.dots_overflow=S.nbelt>ORR_DOTS;
    for(int i=0;i<S.ncm;i++) if(S.show[2] && S.cm_on[i]) {
        if(n==ORR_DOTS) { S.dots_overflow=1;break; }
        if(orr_pixel_make(l->dot_scratch+n,S.cmx[i]-.5f,S.cmy[i]-.5f,C_PRI,1,1))n++;
    }
    for(int i=0;i<S.nsm;i++) if(S.sm_on[i]) {
        if(n==ORR_DOTS) { S.dots_overflow=1;break; }
        const od_small *o=od_small_bodies+i;
        int big=o->kind==OD_DWARF || strstr(o->name,"Vesta") || strstr(o->name,"Pallas");
        if(orr_pixel_make(l->dot_scratch+n,S.smx[i],S.smy[i],big?C_SEC:C_DIS,big?2:1,1))n++;
    }
    if(partial) {
        for(int i=S.nbelt;i<n;i++)orr_dot_link(l,i);
        l->dot_items=n;l->dot_updates++;S.dot_slice=0;
        GFX_COST_PX(GFX_COST_ORR_PRE_CPU,950*(count+S.nsm+S.ncm));
        return;
    }
    l->ndots=0; S.dot_builds++;
    memset(l->dot_head,0,sizeof l->dot_head);
    /* Reverse catalog order makes full chain construction linear. */
    for(int i=n-1;i>=0;i--)if(l->dot_scratch[i].layer!=255) {
        l->ndots++;
        int row=l->dot_scratch[i].y+ORR_ORBIT_BIAS;
        unsigned z=l->dot_head[row];l->dot_prev[i]=0;l->dot_link[i]=(uint16_t)z;
        if(z)l->dot_prev[z-1]=(uint16_t)(i+1);
        l->dot_head[row]=(uint16_t)(i+1);
    }
    l->dot_items=n;l->dot_chain_valid=1;
    l->dot_nbelt=S.nbelt;l->dot_quant_valid=1;l->dot_style_key=style;S.dot_slice=0;
    /* Conservatively retain the former board 4.5 ms full-build price until
     * measured again. Partial updates above charge changed stamps and row
     * insertion visits, never a desktop timing multiplier. */
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,950*(count+S.nsm+S.ncm)+80*n+80*ORR_ORBIT_ROWS);
}
static void orr_pixel_draw(gfx_fb *fb,const orr_pixel *d)
{
    int cx0=fb->cx0,cx1=fb->cx1;
    if(cx1<=cx0) { cx0=0; cx1=GFX_W; }
    int n=d->shape==2?5:d->shape==1?4:1;
    static const int8_t dx[3][5]={{0},{0,1,0,1},{0,-1,1,0,0}};
    static const int8_t dy[3][5]={{0},{0,0,1,1},{0,0,0,-1,1}};
    int x=d->x+fb->ox,y=d->y+fb->oy-fb->y_base;
    /* Reject the entire stamp before ink conversion or halo tests. */
    if(x+1<cx0 || x-1>=cx1) { GFX_COST_PX(GFX_COST_ORR_CPU,100); return; }
    uint16_t ink=fb->dim?gfx_ink(fb,d->ink):d->ink;
    if(fb->dim) GFX_COST_PX(GFX_COST_ORR_CPU,120); /* one ink per stamp, not per pixel */
    if(x>cx0 && x+1<cx1 && x>0 && x+1<GFX_W && y>0 && y+1<fb->h) {
        uint16_t *row=gfx_row(fb,y); int stride=fb->stride?fb->stride:GFX_W;
        row[x]=ink;
        if(d->shape==1) { row[x+1]=ink; row[stride+x]=ink; row[stride+x+1]=ink; }
        else if(d->shape==2) { row[x-1]=row[x+1]=row[x-stride]=row[x+stride]=ink; }
        GFX_COST_PX(GFX_COST_ORR_PIXEL,1); return;
    }
    for(int k=0;k<n;k++) {
        int x=d->x+dx[d->shape][k]+fb->ox, y=d->y+dy[d->shape][k]+fb->oy-fb->y_base;
        if((unsigned)y<(unsigned)fb->h && x>=cx0 && x<cx1 && (unsigned)x<GFX_W)
            gfx_row(fb,y)[x]=ink;
    }
    /* Integer stamp + sequential record fetch, no primitive setup/AA/mask. */
    GFX_COST_PX(GFX_COST_ORR_PIXEL,1);
}
static void orr_small_pixel(gfx_fb *fb,float x,float y,uint16_t ink,int shape)
{
    orr_pixel d;
    if(!orr_pixel_make(&d,x,y,ink,shape,1)) return;
    if(fb->rec) gfx_rec_cmd(fb,d.x-1,d.y-1,d.x+3,d.y+3,
        gfx_hash_u(gfx_hash_u(gfx_hash_u(0xD073u,(unsigned)d.x),(unsigned)d.y),ink));
    else orr_pixel_draw(fb,&d);
}
static void orr_pixels_replay(gfx_fb *fb,int layer,int lo,int hi)
{
    orr_list *l=S.list;
    float scale=(S.zoom/l->dot_zoom)*(S.sc/l->dot_shrink);
    if(scale!=1) {
        float cx=CX+S.panx,cy=CY+S.pany,bx=CX+l->dot_panx,by=CY+l->dot_pany;
        int a=orr_floor((lo-2-cy)/scale+by)+ORR_ORBIT_BIAS;
        int z=orr_ceil((hi+2-cy)/scale+by)+ORR_ORBIT_BIAS;
        if(a<0)a=0;
        if(z>ORR_ORBIT_ROWS)z=ORR_ORBIT_ROWS;
        if(a>=z)return;
        for(int sy=a;sy<z;sy++)for(unsigned link=l->dot_head[sy];link;link=l->dot_link[link-1]) {
            orr_pixel d=l->dot_scratch[link-1];
            if(d.layer!=layer)continue;
            d.x=(int16_t)orr_floor(cx+(d.x-bx)*scale+0.5f);
            d.y=(int16_t)orr_floor(cy+(d.y-by)*scale+0.5f);
            orr_pixel_draw(fb,&d);
            GFX_COST_PX(GFX_COST_ORR_CPU,150);
        }
        return;
    }
    int dx=orr_floor(S.panx-l->dot_panx+0.5f),dy=orr_floor(S.pany-l->dot_pany+0.5f);
    int a=lo-dy-1+ORR_ORBIT_BIAS,z=hi-dy+1+ORR_ORBIT_BIAS;
    if(a<0)a=0;
    if(z>ORR_ORBIT_ROWS)z=ORR_ORBIT_ROWS;
    if(a>=z)return;
    gfx_fb shifted=*fb; shifted.ox+=dx; shifted.oy+=dy;
    if(layer==0 && !fb->dim) {
        /* Faint asteroids are single opaque pixels. Walk their existing row
         * bins with one framebuffer row lookup, avoiding a complete stamp
         * setup (shape array, ink and address generation) for every point. */
        int x0=fb->cx0,x1=fb->cx1;if(x1<=x0){x0=0;x1=GFX_W;}
        unsigned stamps=0;
        for(int sy=a;sy<z;sy++) {
            int y=sy-ORR_ORBIT_BIAS+shifted.oy-fb->y_base;
            uint16_t *dst=(unsigned)y<(unsigned)fb->h?gfx_row(fb,y):0;
            for(unsigned link=l->dot_head[sy];link;link=l->dot_link[link-1]) {
                const orr_pixel *d=l->dot_scratch+link-1;
                if(d->layer)continue;
                if(d->shape) { orr_pixel_draw(&shifted,d);continue; }
                int x=d->x+shifted.ox;
                if(dst && x>=x0 && x<x1 && (unsigned)x<GFX_W) { dst[x]=d->ink;stamps++; }
            }
        }
        /* Catalog chains trade sequential locality for sparse updates. Use
         * 600 ns (near the 625 ns generic stamp), not the old sequential
         * 350 ns price. This new kernel still needs board validation. */
        GFX_COST_PX(GFX_COST_ORR_CPU,600*stamps+150*(z-a));
        return;
    }
    for(int sy=a;sy<z;sy++)for(unsigned link=l->dot_head[sy];link;link=l->dot_link[link-1]) {
        const orr_pixel *d=&l->dot_scratch[link-1];
        if(d->layer==layer) orr_pixel_draw(&shifted,d);
    }
}
static void orr_bucket(void)
{
    orr_list *l=S.list;
    uint32_t count[ORR_STRIPES]={0}, next[ORR_STRIPES];
    for(int pass=0;pass<2;pass++) {
        for(int i=0;i<l->n;i++) {
            const orr_cmd *q=&l->cmd[i];
            orr_cat(q->category);
            for(int b=q->first;b<=q->last;b++) {
                if(!pass) count[b]++;
                else l->refs[next[b]++]=(uint16_t)i;
            }
            GFX_COST_PX(GFX_COST_ORR_BUCKET,2+q->last-q->first);
        }
        if(!pass) {
            l->start[0]=0;
            for(int b=0;b<ORR_STRIPES;b++) { l->start[b+1]=l->start[b]+count[b]; next[b]=l->start[b]; }
            if(l->start[ORR_STRIPES]>ORR_REFS) { l->overflow=1; return; }
        }
    }
    orr_cat(OP_OTHER);
}
static void orr_replay(gfx_fb *fb)
{
    /* Clip each sub-stripe separately: a translated/dirty buffer can cross a
     * bucket boundary. This avoids double blending and needs no dedup scan. */
    int lo=fb->y_base-fb->oy, hi=lo+fb->h;
    if(lo<0)lo=0;
    if(hi>GFX_H)hi=GFX_H;
    for(int y=lo;y<hi;) {
        int b=y/16, end=(b+1)*16; if(end>hi)end=hi;
        gfx_fb part=*fb;
        int row=y+fb->oy-fb->y_base;
        part.px=fb->px+row*(fb->stride?fb->stride:GFX_W);
        part.y_base=y+fb->oy; part.h=end-y;
        /* Retained coverage is uncoloured: keep the caller's ink dim through
         * every writer. Never scan/dim the composed stripe afterward. */
        fk_view v; fk_view_of(&part,&v);
        const uint16_t *refs=S.list->refs;
        for(uint32_t j=S.list->start[b];j<S.list->start[b+1];j++) {
            unsigned ref=refs[j];
            const orr_cmd *q=&S.list->cmd[ref];
            orr_cat(q->category);
            GFX_COST_PX(GFX_COST_ORR_INDEX,1);
            const float *f=q->f; uint16_t c=q->ink;
            if(q->x1<=v.x0 || q->x0>=v.x1) continue;
            switch(q->type) {
            case B_BANDS: orr_band_draw(&part,c); break;
            case B_HOT: orr_hot_draw(&part,&S.list->ring_hot[q->aux],c); break;
            case B_RTICKS: {
                int ring=q->aux;
                if(orr_ticks_draw(&part,ring))break;
                hl_begin();
                for(int k=0;k<S.list->ring_tick_count[ring][b];k++) {
                    int t=S.list->ring_tick_ref[ring][b][k];
                    hairline(&part,&v,S.list->ring_tick[ring][t].x0,S.list->ring_tick[ring][t].y0,
                             S.list->ring_tick[ring][t].x1,S.list->ring_tick[ring][t].y1,
                             gfx_ink(&part,S.list->ring_tick[ring][t].ink));
                }
                hl_end();
                GFX_COST_PX(GFX_COST_ORR_CPU,700*S.list->ring_tick_count[ring][b]);
                break;
            }
            case B_ATEXT: ui_text_arc(&part,&S.list->ring_text[q->aux],c); break;
            case B_BELT: orr_pixels_replay(&part,q->aux,y,end); break;
            case B_ORBIT: orr_orbits_replay(&part,y,end); break;
            case B_ORBIT_SEG: {
                int p=q->aux;
                c=gfx_ink(&part,c);
                hl_begin();
                for(int i=(int)f[0];i<(int)f[1];i++) {
                    int k=(i+1)%NORB;
                    float x0=S.ox[p][i],y0=S.oy[p][i],x1=S.ox[p][k],y1=S.oy[p][k];
                    if(fmaxf(y0,y1)+1<v.y0 || fminf(y0,y1)-1>=v.y1) continue;
                    hairline(&part,&v,x0,y0,x1,y1,c);
                }
                hl_end(); break;
            }
            case B_SWEEP: orr_ticks_draw_ink(&part,q->aux,c); break;
            case B_CIRCLE: gfx_fill_circle(&part,f[0],f[1],f[2],c); break;
            case B_DOT: dot_disc(&part,f[0],f[1],2*f[2],c); break;
            case B_RECT: gfx_fill_rect(&part,(int)f[0],(int)f[1],(int)f[2],(int)f[3],c); break;
            case B_PILL: if(!orr_plate_pill(&part,f[0],f[1],f[2],f[3],c))gfx_fill_pill(&part,f[0],f[1],f[2],f[3],c); break;
            case B_RING: gfx_ring(&part,f[0],f[1],f[2],2*f[3],c); break;
            case B_ARC: if(q->aux) orr_arc_replay(&part,&S.list->arc[q->aux-1],c);
                else gfx_arc(&part,f[0],f[1],f[2],f[3],f[4],f[5],c);
                break;
            case B_LINE: gfx_line(&part,f[0],f[1],f[2],f[3],f[4],c); break;
            case B_HAIR: hl_begin(); hairline(&part,&v,f[0],f[1],f[2],f[3],gfx_ink(&part,c)); hl_end(); break;
            case B_LIT: lit_disc(&part,&v,f[0],f[1],f[2],f[3],f[4],c); break;
            case B_ZOOM: orr_zoom(&part,(int)f[0],f[1],c,q->aux); break;
            default: {
                const orr_text *t=&S.list->text[q->aux];
                orr_words(&part,q->type,t->font,&t->dot,f[0],f[1],t->s,c,t->align,t->tracking); break;
            }
            }
        }
        y=end;
    }
}
/* These names are local to this translation unit; the wrappers above and
 * replay call the original rasterizers. Other faces/cards are unaffected. */
#define gfx_fill_circle orr_circle
#define gfx_fill_pill orr_pill
#define gfx_ring orr_ring
#define gfx_arc orr_arc
#define gfx_line orr_line
#define dot_disc orr_dot
#define ui_text orr_text_draw
#define ui_label orr_label
#define dot_text_a orr_dot_text
