/* Link in place of nos_ui_arc.o, with the original HEAD implementation renamed
 * legacy_* as an independent raster oracle. See check_arc_motion.py. */
#include "../nos/ui_arc.c"
#include <stdio.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(2); } } while(0)
typedef struct { int16_t x,y; uint16_t slot; } old_glyph;
typedef struct {
    old_glyph glyph[16];
    int16_t x0,y0,x1,y1;
    uint16_t n;
    unsigned hash,epoch;
} old_run;
extern void legacy_begin(void);
extern int legacy_prepare(old_run *,const font_t *,float,float,float,float,const char *,int,int);
extern void legacy_draw(gfx_fb *,const old_run *,uint16_t);
extern int sim_program_main(int,char **);
static uint16_t actual[GFX_W*GFX_H],expected[GFX_W*GFX_H];
static unsigned rng=17;
static unsigned random_u(void) { return rng=rng*1664525u+1013904223u; }
static void clear(void)
{ for(int i=0;i<GFX_W*GFX_H;i++)actual[i]=expected[i]=0x1234; }
static gfx_fb canvas(uint16_t *p)
{ gfx_fb fb={0}; fb.px=p; fb.h=GFX_H; fb.cx1=GFX_W; return fb; }
static void exact_oracle(void)
{
    ui_text_arc_motion(0);
    for(int trial=0;trial<3000;trial++) {
        const font_t *f=trial%2?UI_F_SMALL:UI_F_LABEL;
        float angle=trial<180?trial*2.0f:((int)(random_u()%720000)-360000)/1000.0f;
        float radius=trial%3?196.0f:156+(random_u()%6400)/100.0f;
        const char *str=trial%3==0?"0123456789ABCDEF":trial%3==1?"JAN FEB MAR":"2026 NOW";
        ui_arc_text run; old_run old;
        ui_text_arc_begin(); legacy_begin();
        CHECK(ui_text_arc_prepare(&run,f,233,233,radius,angle,str,UI_CENTER,1));
        CHECK(legacy_prepare(&old,f,233,233,radius,angle,str,UI_CENTER,1));
        CHECK(!pending && run.n==old.n);
        clear(); gfx_fb a=canvas(actual),b=canvas(expected);
        a.dim=b.dim=trial%33;
        ui_text_arc(&a,&run,0xa5b8); legacy_draw(&b,&old,0xa5b8);
        if(memcmp(actual,expected,sizeof actual)) {
            fprintf(stderr,"oracle trial=%d angle=%f radius=%f\n",trial,angle,radius);
            for(int i=0;i<run.n;i++) if(run.glyph[i].x!=old.glyph[i].x || run.glyph[i].y!=old.glyph[i].y)
                fprintf(stderr,"glyph %d position %d,%d != %d,%d\n",i,run.glyph[i].x,run.glyph[i].y,old.glyph[i].x,old.glyph[i].y);
            CHECK(0);
        }
    }
    puts("PASS 3000 rest rasters vs original implementation (2 fonts, wrap, fractional angles)");
}
static void motion_oracle(gfx_fb *fb,const ui_arc_text *run,uint16_t ink)
{
    ink=gfx_ink(fb,ink);
    for(int i=0;i<run->n;i++) {
        const ui_arc_glyph *p=run->glyph+i;
        const font_glyph *g=run->font->glyphs+p->glyph;
        int cs=arc_round(arc_trig[p->angle][1]*65536),sn=arc_round(arc_trig[p->angle][0]*65536);
        for(int y=0;y<p->h;y++)for(int x=0;x<p->w;x++) {
            int dx=p->x+fb->ox+x,dy=p->y+fb->oy+y-fb->y_base;
            if(dx<fb->cx0 || dx>=fb->cx1 || (unsigned)dx>=GFX_W || (unsigned)dy>=(unsigned)fb->h)continue;
            int sx=(p->left+x)*cs+(p->top+y)*sn+(cs+sn)/2-(g->xoff*65536-g->xadv*32768);
            int sy=-(p->left+x)*sn+(p->top+y)*cs+(cs-sn)/2-g->yoff*65536;
            int ix=sx>>16,iy=sy>>16;
            if((unsigned)ix>=g->w || (unsigned)iy>=g->h)continue;
            uint8_t alpha=run->font->bitmap[g->bitmap_off+iy*g->w+ix];
            if(alpha) { uint16_t *dst=gfx_row(fb,dy)+dx;*dst=alpha>=250?ink:gfx_blend565(*dst,ink,alpha); }
        }
    }
}
static void motion_and_settle(void)
{
    ui_text_arc_begin(); CHECK(ui_text_arc_drop()); ui_text_arc_motion(1);
    unsigned before=builds;
    for(int trial=0;trial<360;trial++) {
        ui_arc_text run; ui_text_arc_begin();
        CHECK(ui_text_arc_prepare(&run,UI_F_SMALL,233,233,196,trial+0.25f,"2026 JAN 31",UI_CENTER,1));
        CHECK(builds==before && !cache && !pending);
        clear(); gfx_fb a=canvas(actual);
        a.dim=trial%33; a.ox=trial%9-4; a.oy=trial%7-3;
        a.cx0=trial%60; a.cx1=GFX_W-trial%80;
        ui_text_arc(&a,&run,0xa5b8);
        gfx_fb oracle=a;oracle.px=expected;motion_oracle(&oracle,&run,0xa5b8);
        CHECK(!memcmp(actual,expected,sizeof actual));
        for(int i=0;i<GFX_W*GFX_H;i++)expected[i]=0x1234;
        for(int y=0;y<GFX_H;y+=13) {
            gfx_fb b=a; b.px=expected+y*GFX_W; b.y_base=y;
            b.h=y+13>GFX_H?GFX_H-y:13; ui_text_arc(&b,&run,0xa5b8);
        }
        CHECK(!memcmp(actual,expected,sizeof actual));
    }
    /* Cold start, then 12 independent runs: exact rebuild spreads across ticks
     * and each pass keeps earlier live slots immutable. */
    ui_arc_text runs[12];
    ui_text_arc_motion(0);
    int ticks=0;
    do {
        ui_text_arc_begin(); unsigned b=builds;
        for(int i=0;i<12;i++)CHECK(ui_text_arc_prepare(&runs[i],UI_F_SMALL,233,233,196,i*27.0f,"0123456789ABCDEF",UI_CENTER,1));
        CHECK(builds-b<=ARC_BUILD_LIMIT);
        CHECK(++ticks<=12);
    } while(pending);
    CHECK(ticks>1);
    for(int i=0;i<12;i++)for(int j=0;j<runs[i].n;j++)CHECK(runs[i].glyph[j].slot!=ARC_DIRECT);
    CHECK(!ui_text_arc_drop());
    unsigned settled=builds;
    ui_text_arc_begin();
    for(int i=0;i<12;i++)CHECK(ui_text_arc_prepare(&runs[i],UI_F_SMALL,233,233,196,i*27.0f,"0123456789ABCDEF",UI_CENTER,1));
    CHECK(builds==settled && !pending);
    /* Full cache, collisions and serial rollover cannot evict a pinned slot. */
    memset(pinned,1,sizeof pinned); serial=~0u-1;
    arc_key bounds={0}; const font_glyph *g=arc_glyph(UI_F_SMALL,'Z');
    CHECK(arc_bounds(g,179,&bounds));
    CHECK(mask_build(UI_F_SMALL,g,179,&bounds)<0);
    ui_text_arc_begin(); CHECK(ui_text_arc_drop());
    nos_mem_fail_after(0); ui_text_arc_motion(0);
    CHECK(ui_text_arc_prepare(&runs[0],UI_F_SMALL,233,233,196,0,"NOW",UI_CENTER,1));
    CHECK(!pending && !cache && allocation_failed);
    nos_mem_fail_after(-1);
    printf("PASS zero motion allocations/builds, 360 clipped stripe previews, %d settle ticks, warm hits, pins and allocation fallback\n",ticks);
}
int main(int argc,char **argv)
{
    /* sim main disables OS/CRT dialogs before running any tests. CHECK never aborts. */
    int rc=sim_program_main(argc,argv); if(rc)return rc;
    exact_oracle(); motion_and_settle();
    printf("ARC sizes: key=%u index=%u mask_pool=%u run=%u\n",
           (unsigned)sizeof(arc_key),(unsigned)(sizeof keys+sizeof buckets+sizeof pinned),
           (unsigned)(sizeof(arc_mask)*ARC_SLOTS),(unsigned)sizeof(ui_arc_text));
    return 0;
}
