#ifdef _WIN32
#include <windows.h>
#undef near
#undef far
#endif
/* Private renderer regression harness. Link in place of face_orrery.o with the
 * simulator's main renamed to sim_program_main (see tools/orrery/check_round3.ps1).
 * The normal simulator boot prepares fonts/data; no board or serial is used. */
#include "../nos/faces/face_orrery.c"
#include <assert.h>
/* Fail in CI without the Windows CRT assertion dialog. */
#undef assert
#define assert(x) do { if(!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(2); } } while(0)
extern int sim_program_main(int, char **);
static uint32_t rng=17;
static unsigned random_u(void) { rng=rng*1664525u+1013904223u; return rng; }
static uint16_t actual[GFX_W*40+2], expected[GFX_W*40+2];
static uint16_t full_a[GFX_W*GFX_H], full_b[GFX_W*GFX_H];
static orr_pixel reference_dots[ORR_DOTS];
/* Independent legacy stable counting sort, deliberately unlike row chains. */
static void reference_dot_sort(void)
{
    unsigned row[ORR_ORBIT_ROWS+1]={0};
    for(int i=0;i<S.list->dot_items;i++)if(S.list->dot_scratch[i].layer!=255)
        row[S.list->dot_scratch[i].y+ORR_ORBIT_BIAS+1]++;
    for(int y=1;y<=ORR_ORBIT_ROWS;y++)row[y]+=row[y-1];
    assert(row[ORR_ORBIT_ROWS]==(unsigned)S.list->ndots);
    for(int i=0;i<S.list->dot_items;i++) {
        const orr_pixel *p=S.list->dot_scratch+i;
        if(p->layer!=255)reference_dots[row[p->y+ORR_ORBIT_BIAS]++]=*p;
    }
}

static void pixel_reference(gfx_fb *fb,int layer)
{
    for(int i=0;i<S.list->ndots;i++) {
        const orr_pixel *d=&reference_dots[i];
        if(d->layer!=layer)continue;
        for(int dy=-1;dy<=1;dy++) for(int dx=-1;dx<=1;dx++) {
            int hit=d->shape==0 ? dx==0 && dy==0 :
                    d->shape==1 ? dx>=0 && dy>=0 : abs(dx)+abs(dy)<=1;
            float scale=S.zoom/S.list->dot_zoom;
            int px=scale==1 ? d->x+orr_floor(S.panx-S.list->dot_panx+0.5f) : orr_floor(CX+S.panx+(d->x-CX-S.list->dot_panx)*scale+0.5f);
            int py=scale==1 ? d->y+orr_floor(S.pany-S.list->dot_pany+0.5f) : orr_floor(CY+S.pany+(d->y-CY-S.list->dot_pany)*scale+0.5f);
            int x=px+dx+fb->ox,y=py+dy+fb->oy-fb->y_base;
            if(hit && x>=fb->cx0 && x<fb->cx1 && (unsigned)y<(unsigned)fb->h)
                fb->px[y*fb->stride+x-fb->x_base]=gfx_ink(fb,d->ink);
        }
    }
}
static void test_pixels(void)
{
    S.panx=S.pany=0; S.zoom=1;
    S.nbelt=1024;
    S.show[1]=1; S.ncm=0;
    for(int i=0;i<S.nbelt;i++) {
        S.bxs[i]=(float)(random_u()%(GFX_W+4))-2.25f;
        S.bys[i]=(float)(random_u()%(GFX_H+4))-2.25f;
        S.bh[i]=(uint8_t)(i%3);
    }
    S.bxs[0]=NAN; S.bys[1]=INFINITY; S.bxs[2]=-INFINITY;
    /* Put the larger small bodies on stripe seams and on either glass edge. */
    for(int i=0;i<S.nsm;i++) {
        S.sm_on[i]=1; S.smx[i]=(i&1)?GFX_W-0.5f:0.5f;
        S.smy[i]=(float)(i*16)-0.5f;
    }
    orr_pixels_build();
    reference_dot_sort();
    for(int i=1;i<S.list->ndots;i++) assert(reference_dots[i-1].y<=reference_dots[i].y);
    for(int trial=0;trial<500;trial++) {
        S.zoom=trial%3 ? 0.5f+(random_u()%350)/100.0f : 1;
        S.panx=S.list->dot_panx+(int)(random_u()%601)-300;
        S.pany=S.list->dot_pany+(int)(random_u()%601)-300;
        int x0=(int)(random_u()%GFX_W),w=1+(int)(random_u()%(GFX_W-x0));
        gfx_fb a={0}; a.px=actual+1; a.stride=w; a.x_base=x0;
        a.cx0=x0; a.cx1=x0+w; a.h=1+(int)(random_u()%33);
        a.ox=(int)(random_u()%61)-30; a.oy=(int)(random_u()%61)-30;
        a.y_base=(int)(random_u()%(GFX_H+40))-20; a.dim=trial%4?0:12;
        gfx_fb b=a; b.px=expected+1;
        for(int i=0;i<w*a.h+2;i++) actual[i]=expected[i]=0x1234;
        int lo=a.y_base-a.oy,hi=lo+a.h;
        if(lo<0)lo=0; if(hi>GFX_H)hi=GFX_H;
        /* Full replay clips the logical canvas, including translated halos. */
        for(int layer=0;layer<2;layer++) {
            if(lo<hi)orr_pixels_replay(&a,layer,lo,hi);
            pixel_reference(&b,layer);
        }
        /* Ignore logical rows outside the face, which replay never owns. */
        for(int y=0;y<a.h;y++) if(y+a.y_base-a.oy<0 || y+a.y_base-a.oy>=GFX_H)
            memcpy(expected+1+y*w,actual+1+y*w,w*sizeof(uint16_t));
        assert(!memcmp(actual,expected,(w*a.h+2)*sizeof(uint16_t)));
    }
}
static void test_orbits(void)
{
    for(int pose=0;pose<24;pose++) {
        S.view=pose%4; S.from=-1; S.tx=1; S.drag=0; S.pvx=S.pvy=0; S.home_t=0;
        S.zoom=0.5f+(pose%6)*0.7f; S.panx=pose<4?0:(pose%3-1)*75.5f; S.pany=pose<4?0:(pose%4-2)*33.25f;
        S.tiltk=(pose&1)?1:0; S.az=pose*17.0f; S.ringe=0;
        layout(); orr_orbits_build(); assert(S.orbit_pixels->valid);
        unsigned builds=S.orbit_pixels->builds;
        orr_orbits_build(); assert(S.orbit_pixels->builds==builds);
        S.list->n=S.list->nt=S.list->na=S.list->overflow=0;
        gfx_fb f={0}; f.px=full_a; f.h=GFX_H; f.cx1=GFX_W; f.dim=pose%3?0:13;
        fk_view v; fk_view_of(&f,&v);
        orr_capture=1; draw_orbits(&f,&v); orr_capture=0; orr_bucket();
        assert(!S.list->overflow);
        for(int i=0;i<GFX_W*GFX_H;i++)full_a[i]=full_b[i]=(uint16_t)random_u();
        draw_orbits_direct(&f,&v);
        for(int y=0;y<GFX_H;) {
            int h=1+(int)(random_u()%33); if(h>GFX_H-y)h=GFX_H-y;
            gfx_fb b=f; b.px=full_b+y*GFX_W; b.y_base=y; b.h=h;
            orr_replay(&b); y+=h;
        }
        assert(!memcmp(full_a,full_b,sizeof full_a));
        /* The resting snapshot (face_orrery_snap.h) is the same replay over C_BG. */
        if(!f.dim) {
            if(!orr_snap)orr_snap=hal_big_alloc(GFX_W*GFX_H*2);
            assert(orr_snap);
            S.scene_ink=0x5A5Au+(unsigned)pose; orr_snap_bake_key=S.scene_ink; orr_snap_row=0; orr_snap_valid=0;
            while(orr_snap_row>=0) orr_snap_bake(0);
            assert(orr_snap_valid && orr_snap_key==S.scene_ink);
            for(int i=0;i<GFX_W*GFX_H;i++)full_b[i]=C_BG;
            gfx_fb g=f; g.px=full_b; orr_baked_background=1; orr_replay(&g); orr_baked_background=0;
            assert(!memcmp(orr_snap,full_b,sizeof full_b));
        }
        /* Allocation/capacity fallback: retained orbit segments must dim too. */
        memset(full_a,0,sizeof full_a);memset(full_b,0,sizeof full_b);
        draw_orbits_direct(&f,&v);
        S.list->n=S.list->nt=S.list->na=S.list->overflow=0;
        orr_capture=1;draw_orbits_direct(&f,&v);orr_capture=0;orr_bucket();
        gfx_fb fallback=f;fallback.px=full_b;orr_replay(&fallback);
        assert(!memcmp(full_a,full_b,sizeof full_a));
        /* Time, active and settled fractional pan all retain their coverage phase. */
        S.drag=1; S.panx+=0.3f; S.pany-=0.2f; S.jd+=12;
        orr_orbits_build(); assert(S.orbit_pixels->builds==builds);
        S.drag=0; layout(); orr_orbits_build();
        assert(S.orbit_pixels->builds==builds);
        builds=S.orbit_pixels->builds;
        S.panx+=301; S.pany-=177; layout(); orr_orbits_build();
        assert(S.orbit_pixels->builds==builds);
        for(int trial=0;trial<8;trial++) {
            gfx_fb a={0}; a.px=full_a; a.h=GFX_H; a.stride=GFX_W;
            a.cx0=random_u()%200; a.cx1=GFX_W-random_u()%100;
            a.ox=(int)(random_u()%61)-30; a.oy=(int)(random_u()%61)-30;
            for(int j=0;j<GFX_W*GFX_H;j++)full_a[j]=full_b[j]=0x1234;
            int lo=a.oy<0?-a.oy:0,hi=GFX_H-a.oy; if(hi>GFX_H)hi=GFX_H;
            orr_orbits_replay(&a,lo,hi);
            for(int y=0;y<GFX_H;) {
                int h=1+random_u()%33;if(y+h>GFX_H)h=GFX_H-y;
                gfx_fb b=a;b.px=full_b+y*GFX_W;b.y_base=y;b.h=h;
                lo=y-b.oy;hi=lo+h;if(lo<0)lo=0;if(hi>GFX_H)hi=GFX_H;
                if(lo<hi)orr_orbits_replay(&b,lo,hi);
                y+=h;
            }
            assert(!memcmp(full_a,full_b,sizeof full_a));
        }
    }
}
static void test_zoom_preview(void)
{
    S.view=V_FACE; S.from=-1; S.ringe=0; S.tiltk=0; S.az=0;
    S.panx=S.pany=0; S.zoom=1; S.drag=0;
    layout(); orr_orbits_build();
    unsigned builds=S.orbit_pixels->builds;
    S.drag=2;
    for(int pose=0;pose<30;pose++) {
        S.zoom=0.5f+pose*3.5f/29; S.panx=pose*3.7f; S.pany=-pose*2.3f;
        layout(); orr_orbits_build(); assert(S.orbit_pixels->builds==builds);
        gfx_fb a={0}; a.px=full_a;a.h=GFX_H;a.cx0=13;a.cx1=GFX_W-7;a.ox=11;a.oy=-9;
        gfx_fb b=a;b.px=full_b;
        for(int j=0;j<GFX_W*GFX_H;j++)full_a[j]=full_b[j]=0x1234;
        orr_orbits_replay(&a,9,GFX_H);
        for(int y=0;y<GFX_H;) {
            int h=1+random_u()%33;if(y+h>GFX_H)h=GFX_H-y;
            b.px=full_b+y*GFX_W;b.h=h;b.y_base=y;
            int lo=y-b.oy,hi=lo+h;if(hi>GFX_H)hi=GFX_H;
            if(lo<hi)orr_orbits_replay(&b,lo,hi);
            y+=h;
        }
        assert(!memcmp(full_a,full_b,sizeof full_a));
    }
    S.drag=0;layout();orr_orbits_build();assert(S.orbit_pixels->builds==builds+1);
}
static void test_wake_and_pan(void)
{
    face_ctx c={0}; c.dt.year=2026;c.dt.month=10;c.dt.day=15;c.dt.hour=12;
    S.panx=60.3f;S.pany=-40.7f;S.zoom=2;S.off=300;S.az=85;S.azv=3;
    c.aod=1; tick(&c,1.0f/NOS_FPS); tick(&c,1.0f/NOS_FPS);
    /* Tick must never pollute iris u/o/sel/clr, even on a full recapture. */
    extern uint32_t nos_dbg_us[4];
    uint32_t debug[4]={101,202,303,404};memcpy(nos_dbg_us,debug,sizeof debug);
    S.scene_valid=0;tick(&c,1.0f/NOS_FPS);
    assert(!memcmp(nos_dbg_us,debug,sizeof debug));
    unsigned key=S.scene_input, builds=S.orbit_pixels->builds,db=S.dot_builds;
    int ndots=S.list->ndots;
    assert(S.layout_valid && S.scene_valid && S.az==0 && S.zoom==1);
    c.aod=0;tick(&c,1.0f/NOS_FPS);
    assert(S.scene_input==key && S.orbit_pixels->builds==builds && S.list->ndots==ndots);
    orr_pixel saved[ORR_DOTS];memcpy(saved,S.list->dot_scratch,sizeof saved);
    S.drag=1;
    for(int i=0;i<10;i++) {
        S.panx+=31.3f;S.pany-=25.7f;scene_update(&c);
        assert(S.dot_builds==db && !memcmp(saved,S.list->dot_scratch,sizeof saved));
        assert(S.orbit_pixels->builds==builds);
    }
    S.drag=0;scene_update(&c);
    assert(S.dot_builds==db && !memcmp(saved,S.list->dot_scratch,sizeof saved));
    assert(S.orbit_pixels->builds==builds);
}
static void test_transition_spans(void)
{
    face_ctx c={0}; c.dt.year=2026;c.dt.month=10;c.dt.day=15;c.dt.hour=12;
    c.aod=1;tick(&c,1.0f/NOS_FPS);c.aod=0;tick(&c,1.0f/NOS_FPS);
    unsigned key=S.scene_input,ob=S.orbit_pixels->builds,db=S.dot_builds,lb=S.label_builds;
    for(int trial=0;trial<48;trial++) {
        gfx_fb a={0};a.px=full_a;a.h=GFX_H;a.cx1=GFX_W;
        a.dim=trial%32;a.ox=(trial%3-1)*17;a.oy=(trial%5-2)*9;
        memset(full_a,0,sizeof full_a);memset(full_b,0,sizeof full_b);
        draw(&a,&c);
        for(int y=0;y<GFX_H;y+=16) for(int x=0;x<GFX_W;) {
            int w=1+random_u()%97;if(x+w>GFX_W)w=GFX_W-x;
            gfx_fb b=a;b.px=full_b+y*GFX_W;b.y_base=y;b.h=GFX_H-y<16?GFX_H-y:16;
            b.cx0=x;b.cx1=x+w;draw(&b,&c);x+=w;
        }
        assert(!memcmp(full_a,full_b,sizeof full_a));
        assert(key==S.scene_input && ob==S.orbit_pixels->builds && db==S.dot_builds && lb==S.label_builds);
    }
    /* Exact neutral-grey and coloured dim rounding, including black. */
    for(int dim=1;dim<32;dim++) {
        gfx_fb f={0};f.dim=dim;
        for(unsigned ink=0;ink<65536;ink++)
            assert((ink?gfx_ink(&f,(uint16_t)ink):0)==gfx_ink(&f,(uint16_t)ink));
    }
}
static void test_pan_controls(void)
{
    face_ctx c={0};c.dt.year=2026;c.dt.month=10;c.dt.day=15;c.dt.hour=12;
    for(int zone=0;zone<2;zone++) {
    S.view=zone?V_INNER:V_FACE;S.from=-1;S.tx=1;
    S.locked=0;S.lockk=0;S.drag=1;S.pvx=S.pvy=0;S.panx=S.pany=0;
    S.scene_valid=0;scene_update(&c);assert(S.ui_cache && S.ui_cache->valid);
    unsigned labels=S.label_builds;
    for(int i=0;i<12;i++) {
        unsigned builds=S.ui_cache->builds,hits=S.ui_cache->hits,db=S.dot_builds,ob=S.orbit_pixels->builds;
        S.panx+=13.3f;S.pany-=7.7f;scene_update(&c);
        assert(S.ui_cache->builds==builds && S.ui_cache->hits==hits+1);
        assert(S.dot_builds==db && S.orbit_pixels->builds==ob);
        gfx_fb a={0};a.px=full_a;a.h=GFX_H;a.cx1=GFX_W;
        memset(full_a,0,sizeof full_a);draw(&a,&c);
        S.ui_cache->valid=0;S.scene_valid=0;scene_update(&c);
        memset(full_b,0,sizeof full_b);a.px=full_b;draw(&a,&c);
        assert(!memcmp(full_a,full_b,sizeof full_a));
    }
    assert(S.label_builds==labels);
    S.drag=0;scene_update(&c);assert(S.label_builds==labels+1);
    scene_update(&c);assert(S.label_builds==labels+1);
    }
}
static void test_ring_text(void)
{
    /* Missing retained storage must not crash the direct-draw fallback. */
    orr_list *saved_list=S.list;
    S.list=0;
    gfx_fb missing={0};
    orr_ring_text(&missing,0,196,0,"2026",0xffff);
    S.list=saved_list;
    /* Tangent orientation oracle: rotate the source atlas independently at
     * cardinal angles, on a nonblack canvas. A rotated F cannot pass upright. */
    const font_glyph *g=0;
    for(int i=0;i<UI_F_SMALL->nglyphs;i++)if(UI_F_SMALL->glyphs[i].code=='F')g=&UI_F_SMALL->glyphs[i];
    assert(g);
    for(int turn=0;turn<4;turn++) {
        ui_arc_text run; ui_text_arc_begin();
        assert(ui_text_arc_prepare(&run,UI_F_SMALL,233,233,200,turn*90,"F",UI_CENTER,0));
        gfx_fb f={0}; f.px=full_a; f.h=GFX_H; f.cx1=GFX_W;
        for(int i=0;i<GFX_W*GFX_H;i++)full_a[i]=full_b[i]=0x1234;
        ui_text_arc(&f,&run,0xffff);
        int bx=turn==1?433:turn==3?33:233,by=turn==0?33:turn==2?433:233;
        for(int y=0;y<g->h;y++)for(int x=0;x<g->w;x++) {
            int u=g->xoff-g->xadv/2+x,v=g->yoff+y;
            int dx=turn==1?-v-1:turn==2?-u-1:turn==3?v:u;
            int dy=turn==1?u:turn==2?-v-1:turn==3?-u-1:v;
            uint8_t alpha=UI_F_SMALL->bitmap[g->bitmap_off+y*g->w+x];
            if(alpha)full_b[(by+dy)*GFX_W+bx+dx]=alpha>=250?0xffff:gfx_blend565(0x1234,0xffff,alpha);
        }
        if(memcmp(full_a,full_b,sizeof full_a)) {
            int bad=0;while(bad<GFX_W*GFX_H && full_a[bad]==full_b[bad])bad++;
            fprintf(stderr,"cardinal turn=%d slot=%u pos=%d,%d pixel=%d,%d actual=%04x expected=%04x\n",
                    turn,run.glyph[0].slot,run.glyph[0].x,run.glyph[0].y,
                    bad%GFX_W,bad/GFX_W,full_a[bad],full_b[bad]);
        }
        assert(!memcmp(full_a,full_b,sizeof full_a));
    }
    for(int trial=0;trial<180;trial++) {
        float angle=trial*2.0f+0.25f;
        gfx_fb a={0};a.px=full_a;a.h=GFX_H;a.cx1=GFX_W;a.dim=trial%33;
        a.ox=trial%9-4; a.oy=trial%7-3;
        a.cx0=trial%60; a.cx1=GFX_W-trial%80;
        S.list->n=S.list->nt=S.list->na=S.list->overflow=0;
        ui_text_arc_begin();
        orr_capture=1;orr_ring_text(&a,0,196,angle,"2026",0xa5b8);orr_capture=0;orr_bucket();
        assert(!S.list->overflow && S.list->ring_text[0].n==4);
        for(int i=0;i<GFX_W*GFX_H;i++)full_a[i]=full_b[i]=0x1234;
        ui_text_arc(&a,&S.list->ring_text[0],0xa5b8);
        for(int y=0;y<GFX_H;y+=13) {
            gfx_fb b=a; b.px=full_b+y*GFX_W; b.y_base=y; b.h=y+13>GFX_H?GFX_H-y:13;
            orr_replay(&b);
        }
        assert(!memcmp(full_a,full_b,sizeof full_a));
        gfx_rec rec={0}; gfx_fb rf=a; rf.rec=&rec;
        ui_text_arc(&rf,&S.list->ring_text[0],0xa5b8);
        assert(rec.n==1 && !rec.overflow);
        assert(!ui_text_arc_drop()); /* live retained glyphs are pinned */
    }
    /* Every atlas entry can be evicted after replacing the retained scene. */
    ui_text_arc_begin();assert(ui_text_arc_drop());
    unsigned bytes;ui_text_arc_stats(&bytes,0,0);assert(bytes==0);
    ui_arc_text run;
    assert(!ui_text_arc_prepare(&run,UI_F_SMALL,233,233,0,0,"A",UI_CENTER,0));
    assert(!ui_text_arc_prepare(&run,UI_F_SMALL,233,233,196,0,"12345678901234567",UI_CENTER,0));
    for(int a=0;a<360;a+=2) {
        ui_text_arc_begin();
        assert(ui_text_arc_prepare(&run,UI_F_SMALL,233,233,196,(float)a,"0123456789ABCDEF",UI_CENTER,1));
    }
    ui_text_arc_stats(&bytes,0,0);assert(bytes<=448*1024);
    printf("ARC allocated: bytes=%u limit=%u\n",bytes,448u*1024);
    ui_text_arc_begin();assert(ui_text_arc_drop());
}
static void test_ring_bands(void)
{
    for(int i=0;i<16;i++)orr_band_prepare();
    assert(orr_band && orr_band->ready);
    for(int dim=0;dim<=32;dim+=4) {
        gfx_fb a={0};a.px=full_a;a.h=GFX_H;a.cx1=GFX_W;a.dim=dim;
        gfx_fb b=a;b.px=full_b;
        for(int j=0;j<GFX_W*GFX_H;j++)full_a[j]=full_b[j]=0x1234;
        for(int i=0;i<3;i++)for(int e=0;e<2;e++) {
            orr_arc_geom arc;
            orr_arc_prepare(&arc,CX,CY,RING_R[i]+(e?1:-1)*RING_W/2,1.2f,-124*D2RF,124*D2RF);
            orr_arc_replay(&a,&arc,C_LINE2);
        }
        orr_band_draw(&b,C_LINE2);
        assert(!memcmp(full_a,full_b,sizeof full_a));
    }
}
/* Independent point-sampling oracle, plus clipped/translated stripe replay.
 * The production kernel intersects horizontal intervals; this oracle only
 * tests circle/ray membership at subpixels. */
static void test_ring_highlights(void)
{
    for(int ring=0;ring<3;ring++)for(int turn=-3;turn<=3;turn++) {
        float r=RING_R[ring],w=RING_W,phi=turn*5*D2RF;
        float half=(ring==0?62:ring==1?50:34)*.5f/r;
        orr_hot_mask *m=&S.list->ring_hot[ring];
        orr_hot_prepare(m,r,w,phi,half);
        assert(m->ready && m->y>=7 && m->h<=ORR_HOT_H);
        double sa=sin(phi-half),ca=cos(phi-half),sb=sin(phi+half),cb=cos(phi+half);
        double ri=r-w/2,ro=r+w/2;
        for(int y=-1;y<=m->h;y++)for(int x=-1;x<=m->w;x++) {
            int actual_alpha=0;
            if(y>=0 && y<m->h && x>=0 && x<m->w)for(int k=0;k<2;k++) {
                if(x>=m->row[y][k].lo && x<m->row[y][k].hi)
                    actual_alpha=x>=m->row[y][k].solid_lo && x<m->row[y][k].solid_hi?255:m->alpha[y][x];
            }
            int samples=0;
            for(int sy=0;sy<4;sy++)for(int sx=0;sx<64;sx++) {
                double dx=m->x+x+(sx+.5)/64-CX,dy=CY-(m->y+y+(sy+.5)/4);
                double d2=dx*dx+dy*dy;
                samples+=d2>=ri*ri && d2<=ro*ro && dx*ca-dy*sa>=0 && dx*cb-dy*sb<=0;
            }
            assert(abs(actual_alpha-(samples*255+128)/256)<=4);
        }
        gfx_fb a={0};a.px=full_a;a.h=GFX_H;a.cx1=GFX_W;a.dim=ring*8;
        a.ox=turn;a.oy=turn+2;a.cx0=190;a.cx1=285;
        S.list->n=S.list->nt=S.list->na=S.list->overflow=0;
        orr_capture=1;orr_ring_highlight(&a,ring,r,w,phi,half,0xa5b8);orr_capture=0;orr_bucket();
        assert(!S.list->overflow);
        for(int j=0;j<GFX_W*GFX_H;j++)full_a[j]=full_b[j]=0x1234;
        orr_hot_draw(&a,m,0xa5b8);
        for(int y=0;y<GFX_H;y+=11) {
            gfx_fb b=a;b.px=full_b+y*GFX_W;b.y_base=y;b.h=y+11>GFX_H?GFX_H-y:11;
            orr_replay(&b);
        }
        assert(!memcmp(full_a,full_b,sizeof full_a));
        /* Roots are retained on a pure angular update; cache hits do no work. */
        float root=m->radial[10][2][0];
        orr_hot_prepare(m,r,w,phi+.001f,half);assert(m->radial[10][2][0]==root);
    }
    for(int variant=0;variant<2;variant++) {
        /* The merged two-state white plate ignores the legacy style selector.
         * Its NOW target is on the right and exists only after expansion. */
        orrery_ring_plate=variant;int x=290;
        S.platee=1;S.off=0;assert(!ring_sector_hit(x,392));
        S.off=99;S.platee=0;assert(!ring_sector_hit(x,392));
        S.platee=1;assert(ring_sector_hit(x,392));
        assert(!ring_sector_hit(x,443));assert(!ring_sector_hit(171,392));
        S.locked=0;S.rings_open=1;S.ringe=1;S.menue=0;S.menu_open=0;
        nos_input in={0};in.type=IN_DOWN;in.x=x;in.y=392;
        face_orrery_input(&in);assert(s_target==2);
        in.type=IN_UP;face_orrery_input(&in);in.type=IN_TAP;face_orrery_input(&in);
        assert(S.ta_to==0 && S.ta_from==99 && S.ta_n>0);
    }
    orrery_ring_plate=0;
    puts("PASS ringfix: 21 independent sector coverage oracles, clipped/dimmed/translated stripe replays, cached radial roots, compact/expanded NOW target");
}
static void test_controls(void)
{
    for(int trial=0;trial<120;trial++) {
        float a=(float)(random_u()%600)/100,span=0.01f+(float)(random_u()%620)/100;
        float r=20+(float)(random_u()%200),w=0.5f+(float)(random_u()%200)/10;
        orr_arc_geom g; orr_arc_prepare(&g,CX,CY,r,w,a,a+span);
        gfx_fb f={0}; f.px=actual+1; f.h=1+random_u()%33; f.y_base=random_u()%(GFX_H-33);
        f.cx0=random_u()%100; f.cx1=GFX_W-random_u()%100; f.stride=f.cx1-f.cx0; f.x_base=f.cx0;
        f.ox=(int)(random_u()%21)-10; f.oy=(int)(random_u()%21)-10; f.dim=trial%3?0:11;
        gfx_fb b=f;b.px=expected+1;
        for(int j=0;j<f.h*f.stride+2;j++)actual[j]=expected[j]=(uint16_t)random_u();
        /* Escape private wrapper macros to compare the original core kernel. */
        (gfx_arc)(&f,CX,CY,r,w,a,a+span,0xa59b);
        orr_arc_replay(&b,&g,0xa59b);
        assert(!memcmp(actual,expected,(f.h*f.stride+2)*sizeof(uint16_t)));
    }
    for(int i=0;i<20;i++) for(int k=0;k<=32;k++) {
        float frac=((float)i+(float)k/32)/20;
        orr_zoom_prepare(i,8); orr_zoom_prepare(i,(k+2)/4);
        gfx_fb f={0};f.px=full_a;f.h=GFX_H;f.cx1=GFX_W;f.dim=k%3?0:9;
        gfx_fb b=f;b.px=full_b;
        for(int j=0;j<GFX_W*GFX_H;j++)full_a[j]=full_b[j]=(uint16_t)random_u();
        prog_arc_segment(&f,PROG_SEGMENTS,CX,CY,ZOOM_R+1,9,ZB_P1*D2RF,ZB_P0*D2RF,20,frac,0xb59a,0x2123,i);
        orr_zoom(&b,i,frac,0xb59a,0x2123);
        assert(!memcmp(full_a,full_b,sizeof full_a));
    }
}
static void test_lit(void)
{
    for(int trial=0;trial<160;trial++) {
        gfx_fb f={0};f.px=full_a;f.h=GFX_H;f.cx1=GFX_W;f.dim=trial%3?0:7;
        gfx_fb b=f;b.px=full_b;
        float x=100+(random_u()%20000)/100.0f,y=100+(random_u()%20000)/100.0f;
        float r=4.5f+(random_u()%900)/100.0f,ang=trial*0.13f,ux=gfx_fast_cos(ang),uy=gfx_fast_sin(ang);
        uint16_t ink=0xa5b8,day=gfx_ink(&f,ink),night=gfx_ink(&f,ui_dim(ink,0.3f));
        for(int j=0;j<GFX_W*GFX_H;j++)full_a[j]=full_b[j]=(uint16_t)random_u();
        for(int py=(int)floorf(y-r-1);py<=(int)ceilf(y+r+1);py++)
            for(int px=(int)floorf(x-r-1);px<=(int)ceilf(x+r+1);px++) {
                float dx=px+0.5f-x,dy=py+0.5f-y,d2=dx*dx+dy*dy;
                if(d2>(r+1)*(r+1))continue;
                float cov=fminf(1,r+0.5f-gfx_fast_sqrt(d2));if(cov<=0)continue;
                float t=fminf(1,fmaxf(0,0.5f+dx*ux+dy*uy));
                uint16_t c=t>=0.98f?day:t<=0.02f?night:gfx_blend565(night,day,(uint8_t)(t*255));
                hl_plot(&f,px,py,c,cov);
            }
        fk_view v;fk_view_of(&b,&v);lit_disc(&b,&v,x,y,r,ux,uy,ink);
        assert(!memcmp(full_a,full_b,sizeof full_a));
    }
}
/* Retained dispatch versus independent primitive calls on a nonblack canvas.
 * A post-composition dim would corrupt untouched pixels and fail this test. */
static void test_ink_dispatch(void)
{
    for(int dim=0;dim<=32;dim++) {
        gfx_fb a={0};a.px=full_a;a.h=GFX_H;a.cx1=GFX_W;a.dim=dim;
        gfx_fb b=a;b.px=full_b;
        for(int i=0;i<GFX_W*GFX_H;i++)full_a[i]=full_b[i]=0x18c3;
        dot_style ds={DOT_FONT_5X7,4.6f,3.6f,0xa59b,0,1,1};
        S.list->n=S.list->nt=S.list->na=S.list->overflow=0;
        orr_capture=1;
        orr_circle(&a,80,85,15,0xa59b);
        orr_dot(&a,100,88,8,0xb5ad);
        orr_pill(&a,80,110,130,7,0xa59b);
        orr_ring(&a,150,90,20,3,0xb5ad);
        orr_arc(&a,190,90,25,4,0.3f,2.9f,0xa59b);
        orr_line(&a,70,130,230,160,3,0xb5ad);
        orr_line_item(B_HAIR,70,135,230,165,1,0xa59b);
        orr_text_draw(&a,UI_F_SMALL,190,190,"INK DIM",0xb5ad,UI_CENTER,1);
        orr_label(&a,190,215,"REPLAY",0xa59b,UI_CENTER);
        orr_dot_text(&a,&ds,210,250,"12:34",UI_CENTER);
        orr_capture=0;orr_bucket();orr_replay(&b);
        (gfx_fill_circle)(&a,80,85,15,0xa59b);
        (dot_disc)(&a,100,88,8,0xb5ad);
        (gfx_fill_pill)(&a,80,110,130,7,0xa59b);
        (gfx_ring)(&a,150,90,20,3,0xb5ad);
        (gfx_arc)(&a,190,90,25,4,0.3f,2.9f,0xa59b);
        (gfx_line)(&a,70,130,230,160,3,0xb5ad);
        fk_view v;fk_view_of(&a,&v);
        hl_begin();hairline(&a,&v,70,135,230,165,gfx_ink(&a,0xa59b));hl_end();
        (ui_text)(&a,UI_F_SMALL,190,190,"INK DIM",0xb5ad,UI_CENTER,1);
        (ui_label)(&a,190,215,"REPLAY",0xa59b,UI_CENTER);
        (dot_text_a)(&a,&ds,210,250,"12:34",UI_CENTER);
        assert(!memcmp(full_a,full_b,sizeof full_a));
    }
}
static void test_arc_motion_scene(void)
{
    face_ctx c={0};c.dt.year=2026;c.dt.month=10;c.dt.day=15;c.dt.hour=12;
    S.locked=0;S.lockk=0;S.rings_open=1;S.ringk=S.ringe=1;S.rings_pull=0;
    S.view=V_FACE;S.from=-1;S.tx=1;S.zoom=1;S.panx=S.pany=0;S.home_t=0;
    S.pvx=S.pvy=0;S.menue=S.menuk=0;S.menu_open=0;S.tiltk=0;S.az=0;
    S.jd=S.jd_now+99;S.off=99;S.rings_valid=0;
    ui_text_arc_begin();assert(ui_text_arc_drop());
    unsigned before,after;ui_text_arc_stats(0,&before,0);
    for(int phase=0;phase<7;phase++) {
        S.drag=phase<3?phase+4:0;S.ta_n=phase==3;S.vel=phase==4?0.1:0;
        S.ringk=phase==5?0.9f:1;S.ringe=quad(S.ringk);S.rings_pull=phase==6;
        S.scene_valid=0;scene_update(&c);
        ui_text_arc_stats(0,&after,0);assert(after==before);
    }
    S.drag=S.ta_n=S.rings_pull=0;S.vel=0;S.ringk=S.ringe=1;
    unsigned ink=S.scene_ink;int ticks=0,changes=0;
    do {
        ui_text_arc_stats(0,&before,0);scene_update(&c);ui_text_arc_stats(0,&after,0);
        assert(after-before<=24);assert(++ticks<20);
        changes+=ink!=S.scene_ink;ink=S.scene_ink;
    } while(ui_text_arc_pending());
    if(!(ticks>1 && changes>1)) { printf("arc settle ticks=%d changes=%d builds=%u/%u\n",ticks,changes,before,after);nos_mem_dump(0); }
    assert(ticks>1 && changes>1);
    for(int i=0;i<S.list->n;i++)if(S.list->cmd[i].type==B_ATEXT) {
        const ui_arc_text *r=&S.list->ring_text[S.list->cmd[i].aux];
        for(int j=0;j<r->n;j++)assert(r->glyph[j].slot!=65535u);
    }
    scene_update(&c);ui_text_arc_stats(0,&before,0);
    assert(before==after && ink==S.scene_ink);
    puts("PASS arc scene: drag/scrub/NOW/coast/open modes build zero masks; bounded settle changes damage then idles");
}
static void test_tick_runs(void)
{
    /* Independent legacy Wu raster vs row-stream replay over arbitrary
     * backgrounds; translated, dimmed, narrow and split stripe destinations. */
    orr_capture=0;
    for(int phase=0;phase<36;phase++)for(int ring=0;ring<3;ring++) {
        float xy[40][4];
        assert(orr_ticks_begin(ring,0x900000u+phase*3+ring)==1);
        for(int j=0;j<40;j++) {
            float a=(-120+j*6+phase*.137f)*D2RF;
            float r0=RING_R[ring]-RING_W/2+1,r1=r0+(j%3?4:20);
            xy[j][0]=CX+fk_sin(a)*r0;xy[j][1]=CY-fk_cos(a)*r0;
            xy[j][2]=CX+fk_sin(a)*r1;xy[j][3]=CY-fk_cos(a)*r1;
            orr_tick_line(ring,xy[j][0],xy[j][1],xy[j][2],xy[j][3],j%3==0);
        }
        orr_ticks_finish(ring);assert(orr_ticks[ring]->valid);
        unsigned builds=orr_ticks[ring]->builds;
        assert(orr_ticks_begin(ring,0x900000u+phase*3+ring)==0);
        assert(builds==orr_ticks[ring]->builds);
        for(int k=0;k<GFX_W*GFX_H;k++)full_a[k]=full_b[k]=(uint16_t)(k*311+phase);
        gfx_fb a={0},b={0};a.px=full_a;b.px=full_b;
        a.h=b.h=GFX_H;a.cx0=b.cx0=phase%9;a.cx1=b.cx1=GFX_W-phase%13;
        a.ox=b.ox=phase%7-3;a.oy=b.oy=phase%5-2;a.dim=b.dim=phase%33;
        fk_view v;fk_view_of(&a,&v);
        hl_begin();
        for(int j=0;j<40;j++)hairline(&a,&v,xy[j][0],xy[j][1],xy[j][2],xy[j][3],
            gfx_ink(&a,j%3?GFX_RGB(0x2c,0x2c,0x2c):GFX_RGB(0x44,0x44,0x44)));
        hl_end();
        for(int y=0;y<GFX_H;y+=13) {
            gfx_fb f=b;f.px=full_b+y*GFX_W;f.y_base=y;f.h=GFX_H-y<13?GFX_H-y:13;
            assert(orr_ticks_draw(&f,ring));
        }
        assert(!memcmp(full_a,full_b,sizeof full_a));
    }
    puts("PASS tick streams: 108 exact Wu/translated/dimmed/13-row comparisons and warm-cache hits");
}
static void test_scrub_dependencies(void)
{
    face_ctx c={0};c.dt.year=2026;c.dt.month=10;c.dt.day=15;c.dt.hour=12;
    S.locked=0;S.lockk=0;S.rings_open=1;S.ringk=S.ringe=1;S.rings_pull=0;
    S.view=V_FACE;S.from=-1;S.tx=1;S.zoom=1;S.panx=S.pany=0;S.home_t=0;
    S.pvx=S.pvy=0;S.menue=S.menuk=0;S.menu_open=0;S.tiltk=0;S.az=0;
    S.drag=4;S.ta_n=0;S.vel=0;S.pulse=0;S.jd=S.jd_now;S.off=0;
    S.layout_valid=S.scene_valid=0;scene_update(&c);
    unsigned ob=S.orbit_pixels->builds,db=S.dot_builds,lb=S.label_builds,ub=S.ui_cache->builds;
    for(int i=0;i<60;i++) {
        /* Planet/trail-only dates must not touch belt bins, orbit runs, an
         * empty overview label layout, or the hidden controls prefix. */
        S.jd_sky+=.001;S.jd+=.001;S.off+=.001;S.jd_prev_frame=S.jd-.5;
        scene_update(&c);
        assert(S.orbit_pixels->builds==ob && S.dot_builds==db);
        assert(S.label_builds==lb && S.ui_cache->builds==ub);
    }
    S.jd_belt+=1;scene_update(&c);assert(S.dot_builds==db+1);
    S.jd_small+=1;scene_update(&c);assert(S.dot_builds==db+2);
    assert(S.orbit_pixels->builds==ob);
    S.ringk=.9f;S.ringe=quad(S.ringk);scene_update(&c);
    assert(S.orbit_pixels->builds==ob && S.dot_builds==db+2);
    S.ringk=S.ringe=0;scene_update(&c);assert(S.orbit_pixels->builds==ob);
    S.drag=0;S.jd_prev_frame=0;scene_update(&c);assert(S.orbit_pixels->builds==ob+1);
    puts("PASS scrub dependencies: planet-only updates reuse orbit/dots/labels/UI; belt/small/scale invalidate independently");
}
static void test_month_grip_overlap(void)
{
    S.locked=0;S.lockk=0;S.rings_open=1;S.ringk=S.ringe=1;S.rings_pull=0;
    S.menu_open=0;S.menuk=S.menue=0;S.off=0;S.platee=0;
    for(int radius=194;radius<=200;radius++)for(int angle=40;angle<=46;angle++) {
        float x,y;polar_xy((float)angle,(float)radius,&x,&y);
        nos_input in={0};in.type=IN_DOWN;in.x=(int)(x+.5f);in.y=(int)(y+.5f);
        face_orrery_input(&in);assert(s_target==6 && S.d_ring==1);
        in.type=IN_DRAG_START;in.x0=in.x;in.y0=in.y;in.x-=15;in.y-=10;in.dx=-15;in.dy=-10;
        face_orrery_input(&in);assert(S.drag==4 && !S.rings_pull);
        in.type=IN_DRAG;face_orrery_input(&in);assert(S.ringk==1 && S.ringe==1);
        S.drag=0;S.vel=0;S.off=0;
    }
    float x,y;polar_xy(TAB_PHI,RING_R[0],&x,&y);
    nos_input in={0};in.type=IN_DOWN;in.x=(int)(x+.5f);in.y=(int)(y+.5f);
    face_orrery_input(&in);assert(s_target==4);
    puts("PASS month/grip overlap: 49 visible month-band starts scrub time without folding; grip centre still closes");
}
static void test_sweep_stream(void)
{
    for(int phase=1;phase<=8;phase++)for(int ring=0;ring<3;ring++) {
        float e=phase*.1f,span=28+332*e,r=TAB_R[ring]+(RING_R[ring]-TAB_R[ring])*e;
        assert(orr_sweep_prepare(ring,r,TAB_PHI-span/2,TAB_PHI+span/2,3));
        const orr_tick_cache *c=orr_ticks[ring];assert(c->n<=ORR_TICK_PIXELS);
        for(int inkcase=0;inkcase<3;inkcase++) {
            uint16_t ink=inkcase==0?0:inkcase==1?0xffff:0xa5b8;
            for(int i=0;i<GFX_W*GFX_H;i++)full_a[i]=full_b[i]=0x1234;
            gfx_fb f={0};f.px=full_a;f.h=GFX_H;f.cx0=17;f.cx1=GFX_W-19;
            f.ox=-7;f.oy=5;f.dim=phase%3?0:12;
            uint16_t colour=gfx_ink(&f,ink);
            for(int y=0;y<GFX_H;y++)for(int j=c->row[y];j<c->row[y+1];j++) {
                const orr_tick_pixel *p=c->px+j;int x=p->x+f.ox,dy=y+f.oy;
                if(x<f.cx0 || x>=f.cx1 || (unsigned)dy>=GFX_H)continue;
                uint16_t *dst=full_b+dy*GFX_W+x;
                *dst=p->alpha==32?colour:gfx_blend565(*dst,colour,p->alpha*8);
            }
            for(int y=0;y<GFX_H;y+=13) {
                f.px=full_a+y*GFX_W;f.y_base=y;f.h=y+13>GFX_H?GFX_H-y:13;
                assert(orr_ticks_draw_ink(&f,ring,ink));
            }
            assert(!memcmp(full_a,full_b,sizeof full_a));
        }
    }
    puts("PASS 72 animated sweep streams: clipped/translated/dimmed stripes, including black ink");
}
static void test_ring_band_directions(void)
{
    S.locked=0;S.lockk=0;S.menu_open=0;S.menue=0;S.off=0;
    int cases=0;
    for(int closing=0;closing<2;closing++)for(int phase=0;phase<4;phase++)for(int ring=0;ring<3;ring++)
    for(int angle=-120;angle<=120;angle+=4)for(int dir=-1;dir<=1;dir+=2) {
        float e=phase==0?.32f:phase==1?.68f:phase==2?.88f:1;
        S.rings_open=!closing;S.ringk=phase==3?1:.6f;S.ringe=e;S.rings_pull=0;
        float r=TAB_R[ring]+(RING_R[ring]-TAB_R[ring])*e,x,y;
        polar_xy((float)angle,r,&x,&y);
        nos_input in={0};in.x=(int)(x+.5f);in.y=(int)(y+.5f);
        if(ring_band_hit(in.x,in.y)!=ring)continue;
        in.type=IN_DOWN;face_orrery_input(&in);
        in.x0=in.x;in.y0=in.y;
        in.dx=(int)(dir*12*fk_cos(angle*D2RF));in.dy=(int)(dir*12*fk_sin(angle*D2RF));
        in.x+=in.dx;in.y+=in.dy;in.type=IN_DRAG_START;face_orrery_input(&in);
        assert(S.drag==4 && S.d_ring==ring && !S.rings_pull);
        float k=S.ringk;
        in.type=IN_DRAG;face_orrery_input(&in);assert(S.ringk==k && S.rings_open);
        in.type=IN_DRAG_END;face_orrery_input(&in);assert(S.rings_open && !S.rings_pull);
        S.vel=0;S.off=0;cases++;
    }
    /* Supplied serial chord, both directions, including the .680 frame. */
    for(int phase=0;phase<2;phase++)for(int dir=0;dir<2;dir++) {
        S.ringk=phase?1:.6f;S.ringe=phase?1:.68f;S.rings_open=1;
        nos_input in={0};in.type=IN_DOWN;in.x=dir?297:368;in.y=dir?46:88;
        face_orrery_input(&in);assert(S.d_ring>=0);
        in.x0=in.x;in.y0=in.y;in.dx=dir?71:-71;in.dy=dir?42:-42;
        in.x+=in.dx;in.y+=in.dy;in.type=IN_DRAG_START;face_orrery_input(&in);
        assert(S.drag==4 && !S.rings_pull);
        in.type=IN_DRAG;face_orrery_input(&in);in.type=IN_DRAG_END;face_orrery_input(&in);
        assert(S.rings_open && !S.rings_pull);
    }
    /* A radial pull on the actual grip still folds. */
    S.ringk=S.ringe=1;S.rings_open=1;S.vel=0;
    float x,y;polar_xy(TAB_PHI,RING_R[0],&x,&y);
    nos_input in={0};in.type=IN_DOWN;in.x=(int)(x+.5f);in.y=(int)(y+.5f);face_orrery_input(&in);
    in.x0=in.x;in.y0=in.y;in.dx=-20;in.dy=20;in.type=IN_DRAG_START;face_orrery_input(&in);
    assert(S.drag==6 && S.rings_pull);
    S.drag=0;S.rings_pull=0;S.vel=0;
    printf("PASS ring ownership: %d tangents across 3 bands / 4 opening and closing phases, supplied chord both ways, radial grip pull\n",cases);
}
static void test_incremental_sky(void)
{
    face_ctx c={0};c.dt.year=2026;c.dt.month=10;c.dt.day=11;
    S.drag=4;S.vel=0;S.ta_n=0;S.ringk=S.ringe=1;S.rings_open=1;
    S.view=V_FACE;S.from=-1;S.zoom=1;S.panx=S.pany=0;
    S.layout_valid=S.scene_valid=0;S.jd_prev_frame=0;
    S.nbelt=od_belt_n;
    S.jd_belt=0;S.jd_small=0;S.jd=S.jd_now;
    sky_update(S.jd,0);scene_update(&c);
    S.jd+=1.0/86400;sky_update(S.jd,0);assert(S.jd_sky==S.jd);
    double start=S.jd;
    float preview_error=0;
    for(int frame=0;frame<40;frame++) {
        S.jd=start+(frame+1)*.75;sky_update(S.jd,0);
        assert(S.belt_count<=120 && S.belt_left>=0);
        ep_sky exact;ep_sky_at(S.jd,&exact);
        for(int p=0;p<EP_N;p++) {
            ep_vec v=exact.b[p].helio;pp q=mkpp((float)v.x,(float)v.y,(float)v.z);
            float x,y,px,py;proj(&q,&x,&y);proj(S.pl+p,&px,&py);
            assert(orr_hypot(x-px,y-py)<.02f);
        }
        scene_update(&c);
        assert(S.list->dot_quant_valid);
        for(int i=0;i<S.nbelt;i++) {
            pp exact=belt_exact(i,S.jd);float x,y;proj(&exact,&x,&y);
            if(isfinite(S.bxs[i])) {
                float error=orr_hypot(x-S.bxs[i],y-S.bys[i]);
                if(error>preview_error)preview_error=error;
            }
        }
        /* Partial projection/bin output must equal a full rebuild of these
         * same mixed-age positions, including overlap blend order. */
        int n=S.list->ndots;orr_pixel *saved=malloc(n*sizeof *saved);assert(saved);
        int out=0;
        for(int row=0;row<ORR_ORBIT_ROWS;row++) {
            unsigned prev=0;
            for(unsigned link=S.list->dot_head[row];link;link=S.list->dot_link[link-1]) {
                assert(out<n && link>prev && S.list->dot_prev[link-1]==prev);
                assert(S.list->dot_scratch[link-1].y+ORR_ORBIT_BIAS==row);
                saved[out++]=S.list->dot_scratch[link-1];prev=link;
            }
        }
        assert(out==n);
        S.belt_partial=0;S.dot_preview=0;layout();orr_pixels_build();
        reference_dot_sort();
        assert(n==S.list->ndots && !memcmp(saved,reference_dots,n*sizeof *saved));free(saved);
    }
    assert(preview_error<6); /* sustained month-speed preview envelope */
    /* Final jump and release on the same tick must retarget immediately. */
    S.drag=0;S.time_touch=0;S.jd+=5;
    int ticks=0;
    do { sky_update(S.jd,0);scene_update(&c);assert(S.belt_count<=448);assert(++ticks<=13); }
    while(S.belt_left || S.small_left);
    assert(S.jd_belt==S.jd && S.jd_small==S.jd);
    for(int i=0;i<S.nbelt;i++) { pp exact=belt_exact(i,S.jd);assert(!memcmp(S.bp+i,&exact,sizeof exact)); }
    for(int i=0;i<S.nsm+S.ncm;i++) {
        const ep_conic *el=i<S.nsm?&od_small_bodies[i].el:&od_comets[i-S.nsm].el;
        ep_vec v=ep_conic_pos(el,S.jd);pp exact=mkpp((float)v.x,(float)v.y,(float)v.z);
        assert(!memcmp(i<S.nsm?S.sm+i:S.cm+i-S.nsm,&exact,sizeof exact));
    }
    unsigned bv=S.belt_version,sv=S.small_version,db=S.dot_builds;
    for(int i=0;i<30;i++) { sky_update(S.jd,0);scene_update(&c); }
    assert(S.belt_version==bv && S.small_version==sv && S.dot_builds==db);
    printf("PASS bounded sky: %d bodies, bounded planets, 40 slice/full dot comparisons, convergence in %d ticks; .75 day/frame max belt preview error %.2f px\n",S.nbelt,ticks,preview_error);
}
static void test_cached_math(void)
{
    /* Independent legacy conic evaluator, including parabolic/hyperbolic
     * branches, negative dates and epochs far outside the supplied gesture. */
    for(int i=0;i<S.nsm+S.ncm;i++)for(int k=-40;k<=40;k++) {
        const ep_conic *el=i<S.nsm?&od_small_bodies[i].el:&od_comets[i-S.nsm].el;
        ep_conic_cache cache;ep_conic_prepare(&cache,el);
        double jd=el->tp+k*183.125;
        ep_vec a=ep_conic_pos(el,jd),b=ep_conic_cached(&cache,jd);
        assert(!memcmp(&a,&b,sizeof a));
    }
    for(int p=0;p<EP_N;p++)for(int k=-100;k<=100;k++) {
        double jd=2451545.0+k*365.25,E;
        ep_vec a=ep_helio(p,jd),b=ep_helio_anomaly(p,jd,&E);
        assert(!memcmp(&a,&b,sizeof a));assert(E==ep_ecc_anomaly(p,jd));
    }
    /* A live event scan must pause throughout touch/coast and resume at rest. */
    int old=S.scanning;S.scanning=1;int at=S.scan.i;
    S.time_touch=1;events_update();assert(S.scan.i==at);
    S.time_touch=0;S.drag=4;events_update();assert(S.scan.i==at);
    S.drag=0;S.vel=1;events_update();assert(S.scan.i==at);
    S.vel=0;S.scanning=old;
    puts("PASS cached conics bit-exact to legacy evaluator; shared planet anomalies; event scan motion guard");
}
static void test_preview_math(void)
{
    double worst=0;
    for(int body=0;body<EP_N+S.nsm+S.ncm;body++) {
        ep_conic_cache cc;
        if(body>=EP_N) {
            int i=body-EP_N;
            ep_conic_prepare(&cc,i<S.nsm?&od_small_bodies[i].el:&od_comets[i-S.nsm].el);
        }
        for(int j=-100;j<=100;j++) {
            double jd=j<0?2451545.0+j*730.5:body<EP_N?2451545.0+j*730.5:cc.el->tp+(j-50)*.01;
            double E;
            ep_vec a=body<EP_N?ep_helio(body,jd):ep_conic_pos(cc.el,jd);
            ep_vec b=body<EP_N?ep_helio_preview(body,jd,&E):ep_conic_preview(&cc,jd);
            pp pa=mkpp((float)a.x,(float)a.y,(float)a.z),pb=mkpp((float)b.x,(float)b.y,(float)b.z);
            S.from=-1;S.panx=S.pany=0;S.zoom=4;S.sc=1;S.ca=1;S.sa=0;S.ct=.438371f;S.st=.898794f;
            for(int view=0;view<4;view++) {
                S.view=view;float x,y,xx,yy;proj(&pa,&x,&y);proj(&pb,&xx,&yy);
                double error=hypot(x-xx,y-yy);if(error>worst)worst=error;
                if(!(error<.05))fprintf(stderr,"preview body=%d sample=%d view=%d error=%g\n",body,j,view,error);
                assert(error<.05);
            }
        }
    }
    S.view=V_FACE;S.zoom=1;S.tiltk=0;S.jd=S.jd_now;S.ringe=1;S.drag=5;S.time_touch=0;S.vel=0;S.ta_n=0;
    sky_update(S.jd,0);S.drag=0;sky_update(S.jd,0);
    for(int p=0;p<EP_N;p++) { ep_vec v=ep_helio(p,S.jd);assert(!memcmp(&v,&S.sky.b[p].helio,sizeof v)); }
    int ticks=0;while(S.small_left) { sky_update(S.jd,0);assert(++ticks<20); }
    for(int i=0;i<S.nsm+S.ncm;i++) {
        ep_vec v=ep_conic_pos(i<S.nsm?&od_small_bodies[i].el:&od_comets[i-S.nsm].el,S.jd);
        pp q=mkpp((float)v.x,(float)v.y,(float)v.z);
        assert(!memcmp(&q,i<S.nsm?S.sm+i:S.cm+i-S.nsm,sizeof q));
    }
    printf("PASS motion preview: 201 dates/body, four views at 4x/tilt, max %.6f px; unchanged-date release exact\n",worst);
}
static void test_baked_orbits(void)
{
    for(int pose=0;pose<8;pose++) {
        S.view=pose%4;S.from=-1;S.tx=1;S.zoom=1;S.panx=S.pany=0;
        S.tiltk=pose&1;S.cam_tilt=42;S.az=pose*7;S.drag=0;S.vel=0;S.ta_n=0;
        S.ringe=1;layout();orr_orbits_build();orr_orbits_ink();
        assert(S.orbit_pixels->ink);
        for(int i=0;i<GFX_W*GFX_H;i++)full_a[i]=full_b[i]=C_BG;
        gfx_fb a={0};a.px=full_a;a.h=GFX_H;a.cx1=GFX_W;
        orr_baked_background=0;orr_orbits_replay(&a,0,GFX_H);
        orr_baked_background=1;
        for(int y=0;y<GFX_H;y+=13) {
            gfx_fb b=a;b.px=full_b+y*GFX_W;b.y_base=y;b.h=y+13>GFX_H?GFX_H-y:13;
            orr_orbits_replay(&b,y,y+b.h);
        }
        orr_baked_background=0;
        assert(!memcmp(full_a,full_b,sizeof full_a));
    }
    puts("PASS baked orbit ink vs original sequential AA over background, 8 poses / 13-row stripes");
}
static void test_plate_cache(void)
{
    orr_plate_prepare();assert(orr_plate_masks);
    for(int i=0;i<3;i++)for(int trial=0;trial<40;trial++) {
        const orr_plate_mask *m=orr_plate_masks+i;assert(m->alpha);
        gfx_fb a={0};a.px=actual+1;a.h=1+random_u()%33;
        a.y_base=m->y+(int)(random_u()%(m->h+30))-15;
        a.cx0=random_u()%230;a.cx1=235+random_u()%230;
        a.stride=a.cx1-a.cx0;a.x_base=a.cx0;a.dim=trial%33;
        a.ox=(int)(random_u()%21)-10;a.oy=(int)(random_u()%21)-10;
        gfx_fb b=a;b.px=expected+1;
        for(int j=0;j<a.h*a.stride+2;j++)actual[j]=expected[j]=(uint16_t)random_u();
        uint16_t ink=trial%2?0xa5b8:0;
        (gfx_fill_pill)(&a,m->xa,m->cy,m->xb,m->r,ink);
        assert(orr_plate_pill(&b,m->xa,m->cy,m->xb,m->r,ink));
        assert(!memcmp(actual,expected,(a.h*a.stride+2)*sizeof(uint16_t)));
    }
    puts("PASS plate cache: 120 random-background, dimmed, clipped, translated stripe comparisons");
}
static void test_retained_arc_run(void)
{
    ui_arc_text run;
    ui_text_arc_motion(0);ui_text_arc_begin();
    assert(ui_text_arc_prepare(&run,UI_F_SMALL,CX,CY,200,15,"JAN",UI_CENTER,1));
    unsigned hash=run.hash;
    ui_text_arc_begin();assert(ui_text_arc_retain(&run));assert(run.hash==hash);
    ui_text_arc_begin();assert(ui_text_arc_drop());assert(!ui_text_arc_retain(&run));
    puts("PASS unchanged arc run re-pins masks and rejects dropped cache slots");
}
static void test_arc_index_budget(void)
{
    for(int fail=0;fail<2;fail++) {
        ui_arc_text run;unsigned bytes;
        ui_text_arc_begin();assert(ui_text_arc_drop());
        uint32_t used=nos_mem_used(),failures=nos_mem_failures();
        nos_mem_fail_after(fail);ui_text_arc_motion(0);
        assert(ui_text_arc_prepare(&run,UI_F_SMALL,CX,CY,200,15,"JAN",UI_CENTER,1));
        assert(run.n && nos_mem_failures()==failures+1);
        for(int j=0;j<run.n;j++)assert(run.glyph[j].slot==65535u);
        ui_text_arc_begin();assert(ui_text_arc_drop());
        assert(nos_mem_used()==used);ui_text_arc_stats(&bytes,0,0);assert(!bytes);
        ui_text_arc_begin();
        assert(ui_text_arc_prepare(&run,UI_F_SMALL,CX,CY,200,15,"JAN",UI_CENTER,1));
        for(int j=0;j<run.n;j++)assert(run.glyph[j].slot!=65535u);
        ui_text_arc_begin();assert(ui_text_arc_drop());assert(nos_mem_used()==used);
    }
    puts("PASS PSRAM arc index/mask allocation faults: drawable fallback, retry, drop accounting");
}
static void test_ring_command_caches(void)
{
    face_ctx c={0};c.dt.year=2026;c.dt.month=10;c.dt.day=11;
    S.locked=0;S.lockk=0;S.rings_open=1;S.view=V_FACE;S.from=-1;
    S.drag=4;S.vel=0;S.ta_n=0;S.panx=S.pany=0;S.zoom=1;
    static const float opening[]={1,.9f,1,.8f,1,.95f,1};
    for(int trial=0;trial<28;trial++) {
        S.ringk=S.ringe=opening[trial%7];S.platee=trial%3==0?0:trial%3==1?.8f:1;
        S.d_ring=trial%3;S.ring_now_down=trial&1;
        S.scene_valid=0;scene_update(&c);S.scene_valid=0;scene_update(&c);
        gfx_fb a={0};a.px=full_a;a.h=GFX_H;a.cx1=GFX_W;
        memset(full_a,0,sizeof full_a);draw(&a,&c);
        /* Force the original command construction, keeping the same input
         * geometry and background; no change to masks or pixel kernels. */
        for(int i=0;i<2;i++)if(S.plate_capture[i])S.plate_capture[i]->valid=0;
        for(int i=0;i<4;i++)if(S.ring_capture[i])S.ring_capture[i]->valid=0;
        S.scene_valid=0;scene_update(&c);
        a.px=full_b;memset(full_b,0,sizeof full_b);draw(&a,&c);
        assert(!memcmp(full_a,full_b,sizeof full_a));
    }
    puts("PASS 28 warm/cold ring and plate command captures: reopen geometry, three dials, NOW press, animated plate");
}
int main(int argc,char **argv)
{
#ifdef _WIN32
    /* a failed check prints and exits non-zero; no Windows crash dialog on the desktop */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    int rc=sim_program_main(argc,argv); if(rc)return rc;
    assert(S.list && S.nbelt>=1024);
    printf("ORR sizes: hot=%u list=%u cmd=%u entries=%d refs=%lu ui=%u\n",
        0u,(unsigned)sizeof(orr_list),(unsigned)sizeof(orr_cmd),
        S.list->n,(unsigned long)S.list->start[ORR_STRIPES],(unsigned)sizeof(orr_ui_cache));
    printf("ORR cache: entries=%d capacity=%d bytes=%u zoom_masks=%u\n",
        S.orbit_pixels->n,S.orbit_pixels->capacity,
        (unsigned)(sizeof *S.orbit_pixels+2*S.orbit_pixels->capacity*sizeof(orr_orbit_pixel)),
        (unsigned)sizeof *orr_zmask);
    test_transition_spans(); test_pan_controls(); test_wake_and_pan(); test_orbits(); test_zoom_preview(); test_pixels(); test_ring_text(); test_ring_bands(); test_controls(); test_lit(); test_ink_dispatch(); test_ring_highlights();
    nos_mem_card_prepare(); /* release unrelated app caches before expanded renderer fixtures */
    test_arc_motion_scene();
    test_tick_runs();test_scrub_dependencies();test_month_grip_overlap();
    test_sweep_stream();test_ring_band_directions();test_incremental_sky();
    test_cached_math();test_preview_math();test_baked_orbits();
    test_plate_cache();
    test_retained_arc_run();
    test_arc_index_budget();
    test_ring_command_caches();
    unsigned arcbytes,arcbuilds,archits;ui_text_arc_stats(&arcbytes,&arcbuilds,&archits);
    printf("ARC cache: bytes=%u builds=%u hits=%u band_bytes=%u\n",arcbytes,arcbuilds,archits,(unsigned)sizeof *orr_band);
    puts("PASS ORRERY: 48 dimmed/translated span partitions, 24 cached/uncached pan controls, label settle once, wake/pan cache invariants, 24 orbit poses, 30 scaled previews, 500 pan/clipped dot stripes, 4 independent cardinal glyph oracles, 180 curved/clipped/translated ring labels, cache eviction, 9 exact band masks, 120 arcs, 660 zoom masks, 160 lit discs, 33 ink-dispatch levels, independent compositor counters");
    return 0;
}
