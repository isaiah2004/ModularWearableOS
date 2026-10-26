/* Include a private, counter-only instrumented copy of the actual app. */
#include SETTINGS_SOURCE
#include <assert.h>
#include <windows.h>
extern int sim_program_main(int,char **);
extern const native_asset_desc *__real_native_menu_asset(unsigned,unsigned,unsigned,unsigned);
extern sprite8 __real_native_menu_sprite(const native_asset_desc *);
static unsigned missing_asset,missing_sprite,warm_pairs,raw_commands,invalidations;
static uint16_t cached[GFX_W*GFX_H],raw[GFX_W*GFX_H],stripes[GFX_W*GFX_H];
static gfx_rec cached_record;
static const font_t *body,*label,*title;

const native_asset_desc *__wrap_native_menu_asset(unsigned family,unsigned entry,unsigned layout,unsigned level)
{
    if(missing_asset){missing_asset--;return NULL;}
    return __real_native_menu_asset(family,entry,layout,level);
}
sprite8 __wrap_native_menu_sprite(const native_asset_desc *asset)
{
    if(missing_sprite){missing_sprite--;return(sprite8){0};}
    return __real_native_menu_sprite(asset);
}
static void reset_counts(void){test_sync_calls=test_sync_walks=test_entry_visits=0;}
static void assert_walks(unsigned count)
{
    assert(test_sync_walks==count && test_entry_visits==count*NE);
}
static unsigned ready(void)
{
    unsigned n=0;if(RC)for(int e=0;e<NE;e++)n+=RC->entry[e].ready;return n;
}
static void scene(int theme,float offset,int phase)
{
    set_settings_cache_release();memset(&S,0,sizeof S);
    memset(row_retry_at,0,sizeof row_retry_at);memset(row_retry_room,0,sizeof row_retry_room);
    rows_retry_at=0;rows_retry_room=0;row_baking=row_overlay=row_neutral=0;entry_draw_valid=0;
    UI_F_BODY=body;UI_F_LABEL=label;UI_F_TITLE=title;
    ui_theme_inverted(theme);nos_native_menu_rom_set(1);shutter_enabled=0;nos_t=100;nos_frame++;
    settings_enter();S.build_t0=nos_t-100;S.L.sc.x=offset;S.L.sc.v=phase&1?-1800:1800;
    S.L.bar_alpha=0;S.flash_e=S.arm_e=S.L.pressed=-1;
    for(int i=0;i<I_COUNT;i++)S.vt0[i]=nos_t-100;
    native_pose=phase&1?-137:79;native_exposure=(int[]){0,15*256,30*256+33,60*256,90*256}[phase%5];
    native_direction=phase&1?-1:1;row_level=native_exposure?3:0;
    set_settings_bake_budget_us=0;missing_asset=missing_sprite=0;reset_counts();
}
static void raster(uint16_t *pixels,int phase)
{
    for(unsigned i=0;i<GFX_W*GFX_H;i++)pixels[i]=stripes[i]=(uint16_t)(i*31u+143u);
    gfx_fb fb={0};fb.px=pixels;fb.h=GFX_H;fb.cx0=phase&1?17:0;fb.cx1=phase&1?441:GFX_W;
    fb.ox=phase&1?-19:0;fb.oy=phase&1?13:0;settings_draw(&fb);
    for(int y=0;y<GFX_H;y+=16){gfx_fb stripe=fb;stripe.px=stripes+y*GFX_W;
        stripe.y_base=y;stripe.h=GFX_H-y<16?GFX_H-y:16;settings_draw(&stripe);}
    assert(!memcmp(pixels,stripes,sizeof stripes));
    gfx_rec rec={0};fb.px=NULL;fb.rec=&rec;settings_draw(&fb);assert(rec.n && !rec.overflow);
    if(pixels==cached)cached_record=rec;
    else assert(rec.n==cached_record.n && !memcmp(rec.c,cached_record.c,(size_t)rec.n*sizeof *rec.c));
    raw_commands+=(unsigned)rec.n;
}
static void warm_scene(int theme,float offset,int phase)
{
    scene(theme,offset,phase);
    /* The real observer has its own first-use ledger, independent of ROM sync. */
    gfx_fb coverage={0};coverage.px=cached;coverage.h=GFX_H;coverage.cx1=GFX_W;
    ui_fblur_coverage_claim(&coverage,0,0,1,1);
    unsigned used=nos_mem_used();
    controls_tick();rows_work(0);
    assert(test_sync_calls==2);assert_walks(1);assert(ready()==NE && !RC->job.active);
    assert(nos_mem_used()==used);
    raster(cached,phase);assert_walks(1);
    /* Independent raw signature/layout path must select identical pixels and commands. */
    entry_draw_valid=0;rows_rom_sync();assert_walks(2);raster(raw,phase);
    assert(!memcmp(cached,raw,sizeof raw));assert(nos_mem_used()==used);warm_pairs++;
}
static void invalidation_cases(int theme)
{
    scene(theme,700.25f,1);controls_tick();rows_work(0);assert_walks(1);
    unsigned used=nos_mem_used(),frame=nos_frame;
    reset_counts();controls_tick();rows_work(0);assert_walks(1);
    assert(frame==nos_frame && nos_mem_used()==used);invalidations++;

    /* A same-frame layout change is a new publication, even if an earlier one was acknowledged. */
    snprintf(S.val[I_FACE],sizeof S.val[I_FACE],"1");S.vt0[I_FACE]=nos_t-100;
    controls_tick();int face=-1;
    for(int e=0;e<NE;e++)if(E[e].id==I_FACE){face=e;break;}
    assert(face>=0);unsigned sig=RC->entry[face].sig;
    snprintf(S.val[I_FACE],sizeof S.val[I_FACE],"WWWWWWWWWWWWWWWWWWWW");
    reset_counts();controls_tick();rows_work(0);assert_walks(1);
    assert(RC->entry[face].sig!=sig && RC->entry[face].sig==entry_sig(face));invalidations++;

    reset_counts();entry_free(&RC->entry[0]);rows_work(0);assert_walks(1);assert(ready()==NE);invalidations++;
    reset_counts();set_settings_cache_release();assert(rows_init());rows_work(0);assert_walks(1);assert(ready()==NE);invalidations++;

    /* Existing staged ownership must be cancelled, including already allocated images. */
    used=nos_mem_used();row_start(0);unsigned steps=0;
    while(!RC->job.staged.image[0].s.px){row_step_job();assert(RC->job.active && ++steps<100);}
    assert(nos_mem_used()>used);reset_counts();rows_work(0);assert_walks(1);
    assert(!RC->job.active && nos_mem_used()==used);invalidations++;

    reset_counts();ui_theme_inverted(!theme);rows_work(0);assert_walks(1);assert(ready()==NE);invalidations++;
    reset_counts();UI_F_BODY=label;UI_F_LABEL=title;rows_palette();assert(!RC->rom_palette);
    controls_tick();rows_work(0);assert_walks(0);
    UI_F_BODY=body;UI_F_LABEL=label;UI_F_TITLE=title;rows_palette();controls_tick();rows_work(0);
    assert_walks(1);assert(ready()==NE);invalidations++;

    reset_counts();nos_native_menu_rom_set(0);rows_palette();assert(!RC->rom_palette);
    while(!RC->lut_ready)rows_palette_step();
    row_start(0);steps=0;while(RC->job.active){row_step_job();assert(++steps<5000);}
    assert(RC->entry[0].ready && !RC->entry[0].rom);
    nos_native_menu_rom_set(1);rows_palette();rows_work(0);assert_walks(1);assert(ready()==NE);invalidations++;

    /* An incomplete descriptor or sprite lookup cannot acknowledge the publication. */
    for(int sprite=0;sprite<2;sprite++){
        entry_free(&RC->entry[0]);missing_asset=!sprite;missing_sprite=sprite;reset_counts();
        controls_tick();assert_walks(1);assert(ready()==NE-1);
        rows_work(0);assert_walks(2);assert(ready()==NE);
        rows_work(0);assert_walks(2);invalidations++;
    }

    reset_counts();entry_draw_valid=0;rows_rom_sync();rows_rom_sync();assert_walks(2);invalidations++;
#if SETTINGS_TEST_PUBLICATION_SERIAL
    /* Force a real unsigned wrap and a potentially matching stale acknowledgement. */
    entry_draw_publication=~0u;RC->rom_sync_publication=1;reset_counts();
    controls_tick();rows_work(0);assert_walks(1);assert(entry_draw_publication==1);invalidations++;
#endif
    set_settings_cache_release();reset_counts();unsigned failures=nos_mem_failures();
    nos_mem_fail_after(0);controls_tick();rows_work(0);nos_mem_fail_after(-1);
    assert(!RC && nos_mem_failures()==failures+1);assert_walks(0);invalidations++;
    nos_t+=2;controls_tick();rows_work(0);assert(RC && ready()==NE);assert_walks(1);
}
int main(int argc,char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    assert(!sim_program_main(argc,argv));body=UI_F_BODY;label=UI_F_LABEL;title=UI_F_TITLE;
    nos_mblur=2;ui_fblur_coverage_enable(1);
    unsigned failures=nos_mem_failures();const float offsets[]={0,700.25f,1831.75f,2850.25f};
    for(int theme=0;theme<2;theme++)for(unsigned pos=0;pos<4;pos++)for(int phase=0;phase<5;phase++)
        warm_scene(theme,offsets[pos],phase);
    for(int theme=0;theme<2;theme++)invalidation_cases(theme);
    assert(nos_mem_failures()==failures+2);set_settings_cache_release();
    UI_F_BODY=body;UI_F_LABEL=label;UI_F_TITLE=title;ui_theme_inverted(0);nos_native_menu_rom_set(1);
    printf("PASS Settings publication: warm_pairs=%u visits_per_pair=36 raw_commands=%u exact_pixels=%u invalidations=%u no_added_allocations=1\n",
        warm_pairs,raw_commands,warm_pairs*GFX_W*GFX_H,invalidations);
    return 0;
}
