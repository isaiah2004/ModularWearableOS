/* SHARP Settings adopts level-0-only art from the SD sharp pack: no blur levels, no pixel allocations. */
#include "../nos/apps/set_settings.c"
#include <assert.h>
#include <windows.h>
extern int sim_program_main(int,char **);
static unsigned ready(void){unsigned n=0;if(RC)for(int e=0;e<NE;e++)n+=RC->entry[e].ready;return n;}
int main(int argc,char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    assert(NOS_RENDER_SHARP==1 && !sim_program_main(argc,argv));
    native_menu_pack_info pi;native_menu_pack_status(&pi);
    assert(pi.state==1 && pi.sharp==1 && pi.records==169);
    assert(native_menu_asset(0,0,0,0) && !native_menu_asset(0,0,0,1) && !native_menu_asset(1,0,1,1) && !native_menu_asset(2,0,0,1));
    for(int theme=0;theme<2;theme++){
        set_settings_cache_release();memset(&S,0,sizeof S);
        ui_theme_inverted(theme);nos_native_menu_rom_set(1);nos_t=100;nos_frame++;
        assert(native_menu_assets_compatible());
        settings_enter();S.build_t0=nos_t-100;S.flash_e=S.arm_e=S.L.pressed=-1;
        for(int i=0;i<I_COUNT;i++)S.vt0[i]=nos_t-100;
        unsigned used=nos_mem_used();
        controls_tick();rows_work(0);
        assert(ready()==NE && !RC->job.active && RC->rom_bound);
        for(int e=0;e<NE;e++){
            const entry_cache *r=&RC->entry[e];assert(r->rom && r->ready && r->image[0].s.px);
            for(int k=1;k<9;k++)assert(!r->image[k].s.px);
        }
        assert(nos_mem_used()==used); /* borrowed immutable pixels */
        gfx_fb fb={0};static uint16_t px[GFX_W*GFX_H];fb.px=px;fb.h=GFX_H;fb.cx1=GFX_W;settings_draw(&fb);
        unsigned lit=0;for(unsigned i=0;i<GFX_W*GFX_H;i++)lit+=px[i]!=0;assert(lit>1000);
    }
    set_settings_cache_release();
    printf("PASS Settings SHARP pack: 36/36 entries adopted from level-0 art, no blur levels, no allocations\n");
    return 0;
}
