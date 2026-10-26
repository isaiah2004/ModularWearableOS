/* Actual app source, independent full/stripe output and metadata budget. */
#include APP_SOURCE
#include <assert.h>
#include <stdio.h>
#include <windows.h>
#include "render_exposure.h"
#include "render_history.h"
extern int sim_program_main(int,char **);
static uint16_t full[GFX_W*GFX_H],striped[GFX_W*GFX_H],sharp[GFX_W*GFX_H],exposed[GFX_W*GFX_H],previous[GFX_W*GFX_H];
static int coverage_full[GFX_H][129],coverage_striped[GFX_H][129];
static const font_t *body,*label,*title;
static unsigned cases;
static unsigned long long full_visits,striped_visits,pixels,commands;

static void scene(int mode,int phase,int direction,int exposure)
{
    nos_mem_fail_after(-1);nos_t=100;nos_frame++;
    UI_F_BODY=body;UI_F_LABEL=label;UI_F_TITLE=title;ui_theme_inverted(phase&1);
    nos_native_menu_rom_set(mode!=1);
    if(mode==1){UI_F_BODY=label;UI_F_LABEL=title;UI_F_TITLE=body;}
#if APP_ID == 7
    sys_launcher_cache_reset();memset(&S,0,sizeof S);memset(&P,0,sizeof P);P.id=-1;
    launcher_rom_sync();l_enter();S.x=368.25f;S.mode=M_DRAG;S.t_enter=mode==3?nos_t-.1:nos_t-10;
    native_pose=-64;native_exposure=exposure;native_direction=direction;
    if(mode!=1){assert(ensure_tables());for(int i=0;i<napps();i++)assert(launcher_adopt_rom(i) || L->rows[i].ready);}
    if(mode==2){P.id=4;P.until=nos_t+1;}
    geometry_prepare(floorf(S.x+.5f));
#else
    set_settings_cache_release();memset(&S,0,sizeof S);
    memset(row_retry_at,0,sizeof row_retry_at);memset(row_retry_room,0,sizeof row_retry_room);
    rows_retry_at=0;rows_retry_room=0;row_baking=row_overlay=row_neutral=0;entry_draw_valid=0;
    settings_enter();S.build_t0=mode==3?nos_t-.1:nos_t-100;
    S.L.sc.x=1000.25f;S.L.sc.v=direction*1800;S.L.bar_alpha=0;S.flash_e=S.arm_e=S.L.pressed=-1;
    for(int id=0;id<I_COUNT;id++)S.vt0[id]=nos_t-100;
    native_pose=-64;native_exposure=exposure;native_direction=direction;row_level=exposure?3:0;shutter_enabled=0;
    set_settings_bake_budget_us=0;
    if(mode==1){set_settings_cache_release();rows_retry_at=nos_t+10;rows_retry_room=nos_mem_available();}
    else {assert(rows_init());controls_tick();rows_work(0);
        if(mode==2)for(int e=0;e<NE;e++)if(LTOP+S.start[e]*UNIT-list_off()>60 && entry_split(e)){
            S.flash_e=e;S.flash_t=nos_t;break;}}
#endif
    /* Settings entry intentionally retires the shared coverage arena. Initialize
     * its optional header with an empty off-glass claim before draw allocation
     * assertions, leaving the actual coverage union empty. */
    gfx_fb header={0};header.px=full;header.h=GFX_H;header.cx1=GFX_W;
    ui_fblur_coverage_claim(&header,0,-2,1,-1);
}
static void draw_app(gfx_fb *fb)
{
#if APP_ID == 7
    l_draw(fb);
#else
    settings_draw(fb);
#endif
}
static void spans(int target[GFX_H][129])
{
    memset(target,0,sizeof coverage_full);
    for(int y=0;y<GFX_H;y++)target[y][0]=ui_fblur_row_spans(y,(int(*)[2])&target[y][1],64);
}
static void render(uint16_t *output,int rows,int phase,gfx_rec *record,ui_fblur_observation *observation)
{
    gfx_fb fb={0};fb.h=GFX_H;fb.cx1=GFX_W;fb.rec=record;
    fb.ox=phase&2?-19:0;fb.oy=phase&2?13:0;fb.dim=phase&4?7:0;
    if(record){draw_app(&fb);return;}
    for(unsigned i=0;i<GFX_W*GFX_H;i++)output[i]=(uint16_t)(i*31u+143u);
    fb.px=output;
    ui_fblur_observe_begin(nos_frame);
    for(int y=0;y<GFX_H;y+=rows){gfx_fb part=fb;part.px=output+y*GFX_W;
        part.y_base=y;part.h=GFX_H-y<rows?GFX_H-y:rows;draw_app(&part);}
    ui_fblur_observe_get(observation);observation->frame=0;
}
static void expose(uint16_t *output)
{
    nos_history_reset();nos_history_begin(nos_frame-1);
    gfx_fb old={0};old.px=previous;old.h=GFX_H;old.cx1=GFX_W;
    assert(nos_history_apply(&old,0,0,GFX_W,GFX_H,0));nos_history_begin(nos_frame);
    for(int y=0;y<GFX_H;y+=16){gfx_fb part={0};part.px=output+y*GFX_W;part.y_base=y;
        part.h=GFX_H-y<16?GFX_H-y:16;part.cx1=GFX_W;nos_exposure_job job;
        assert(nos_exposure_prepare(&job,&part,0,y,GFX_W,y+part.h,1,0));nos_exposure_apply(&job);}
}
static void check(int mode,int phase,int direction,int exposure)
{
    gfx_rec a={0},b={0};ui_fblur_observation oa={0},ob={0};
    /* Settle shared title/font/coverage first-use owners; app caches are then
     * recreated below so the measured publication is still independently built. */
    scene(mode,phase,direction,exposure);render(striped,GFX_H,phase,NULL,&oa);
    scene(mode,phase,direction,exposure);render(full,GFX_H,phase,&a,&oa);assert(a.n && !a.overflow);
    unsigned used=nos_mem_used(),failures=nos_mem_failures();
    stripe_test_metadata=0;render(full,GFX_H,phase,NULL,&oa);
    unsigned long long whole=stripe_test_metadata;spans(coverage_full);
    assert(nos_mem_used()==used && nos_mem_failures()==failures);
    memcpy(sharp,full,sizeof full);
    expose(full);memcpy(exposed,full,sizeof full);
    scene(mode,phase,direction,exposure);
    /* Recorders retain the whole logical scene even for an off-row view. */
    gfx_fb record={0};record.rec=&b;record.h=16;record.y_base=211;record.cx1=GFX_W;
    record.ox=phase&2?-19:0;record.oy=phase&2?13:0;record.dim=phase&4?7:0;draw_app(&record);
    assert(a.n==b.n && !b.overflow && !memcmp(a.c,b.c,(size_t)a.n*sizeof *a.c));
    used=nos_mem_used();failures=nos_mem_failures();
    stripe_test_metadata=0;render(striped,16,phase,NULL,&ob);
    unsigned long long pieces=stripe_test_metadata;spans(coverage_striped);
    assert(!memcmp(sharp,striped,sizeof sharp));
    assert(nos_mem_used()==used && nos_mem_failures()==failures);
    assert(!memcmp(coverage_full,coverage_striped,sizeof coverage_full));
    assert(!memcmp(&oa,&ob,sizeof oa));
    /* Every16-row stripe touches at most two disjoint authored native rows.
     * This budgets real metadata selections, independently of their code shape.
     * The unculled implementation visits6/7 screen-visible rows per stripe. */
    unsigned budget=2u*((GFX_H+15u)/16u);
    if(mode==0 && (whole==0 || pieces>budget)){
        fprintf(stderr,"METADATA_BUDGET app=%d whole=%llu striped=%llu bound=%u\n",APP_ID,whole,pieces,budget);abort();}
    expose(striped);assert(!memcmp(exposed,striped,sizeof striped));
    full_visits+=whole;striped_visits+=pieces;pixels+=GFX_W*GFX_H;commands+=(unsigned)a.n;cases++;
}
int main(int argc,char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);assert(!sim_program_main(argc,argv));
    body=UI_F_BODY;label=UI_F_LABEL;title=UI_F_TITLE;nos_mblur=2;
    ui_fblur_coverage_enable(1);ui_fblur_observe_enable(1);assert(nos_history_init());
    gfx_fb header={0};header.px=full;header.h=GFX_H;header.cx1=GFX_W;ui_fblur_coverage_claim(&header,0,0,1,1);
    for(unsigned i=0;i<GFX_W*GFX_H;i++)previous[i]=(uint16_t)(i*73u+519u);
    const int lengths[]={0,1,15*256,30*256,60*256,90*256};
    for(unsigned i=0;i<6;i++)for(int direction=-1;direction<=1;direction+=2)check(0,(int)i,direction,lengths[i]);
    for(int mode=1;mode<=3;mode++)check(mode,mode,1,30*256);
    printf("PASS native stripe metadata app=%d cases=%u pixels=%llu commands=%llu full=%llu striped=%llu\n",
        APP_ID,cases,pixels,commands,full_visits,striped_visits);
    return 0;
}
