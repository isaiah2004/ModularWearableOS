
#include "../nos/apps/set_settings.c"
#include <windows.h>
#include <assert.h>
#include <time.h>
#include "../nos/render_exposure.h"
#include "../nos/render_history.h"
int settings_sim_main(int,char **);
static uint16_t full[GFX_W*GFX_H],striped[GFX_W*GFX_H],reference[ROW_W*ROW_MAX_H];


static void immutable_rom_ready(void)
{
 assert(native_menu_assets_compatible() && RC && RC->rom_palette && row_bakes==0);
 for(int theme=0;theme<2;theme++){
  ui_theme_inverted(theme);S.L.sc.x=1000.25f;S.L.bar_alpha=0;S.build_t0=nos_t-100;
  native_pose=-64;native_exposure=8*256;nos_frame++;controls_tick();
  for(int e=0;e<NE;e++)assert(RC->entry[e].ready && RC->entry[e].rom);
  const uint16_t *white=native_menu_settings_palette(0,0),*red=native_menu_settings_palette(1,0);
  assert(white && red && !RC->job.active);
  memset(full,0,sizeof full);gfx_fb fb={0};fb.px=full;fb.h=GFX_H;fb.cx1=GFX_W;settings_draw(&fb);
  nos_native_menu_rom_set(0);set_settings_cache_release();assert(rows_init());while(!RC->lut_ready)rows_palette_step();
  assert(!memcmp(white,RC->lut[0],sizeof RC->lut[0]));assert(!memcmp(red,RC->lut[1],sizeof RC->lut[1]));
  for(int e=0;e<NE;e++){row_start(e);int steps=0;while(RC->job.active){row_step_job();assert(++steps<2000);}}
  controls_tick();memset(striped,0,sizeof striped);fb.px=striped;settings_draw(&fb);
  assert(!memcmp(full,striped,sizeof full));
  nos_native_menu_rom_set(1);set_settings_cache_release();assert(rows_init());controls_tick();
  for(int e=0;e<NE;e++)assert(RC->entry[e].ready && RC->entry[e].rom);
 }
 ui_theme_inverted(0);controls_tick();
 unsigned used=nos_mem_used();
 for(int frame=0;frame<14;frame++){
  nos_frame++;S.L.sc.x=frame*90;S.L.sc.v=1800;controls_tick();assert(nos_mem_used()==used);
 }
 unsigned occupied=0;for(int i=0;i<8;i++)occupied+=RC->controls[i].entry>=0;
 assert(occupied==8);
 unsigned captures=control_rasters;nos_frame++;S.L.sc.x-=90;S.L.sc.v=-1800;controls_tick();
 assert(control_rasters==captures);assert(nos_mem_used()==used);
 puts("PASS Settings ROM: first-entry36/36ready/zero bakes, both palettes/all static frames exact runtime reference, all8controlslots used, immediate reverse reuses sources, no per-frame allocations");
}

static void font_switch_sources(void)
{
 const font_t *body=UI_F_BODY,*label=UI_F_LABEL,*title=UI_F_TITLE;
 unsigned checked=0;
 for(int style=0;style<3;style++){
  unsigned rasters=control_rasters;
  UI_F_BODY=style==0?label:style==1?title:body;
  UI_F_LABEL=style==0?title:style==1?body:label;
  controls_tick();
  assert(RC->font_body==UI_F_BODY && RC->font_label==UI_F_LABEL);
  if(style<2){
   assert(!RC->rom_palette);
   for(int e=0;e<NE;e++)assert(!RC->entry[e].ready);
   while(!RC->lut_ready)rows_palette_step();
   controls_tick();
   row_start(0);int steps=0;while(RC->job.active){row_step_job();assert(++steps<2000);}
  }else for(int e=0;e<NE;e++)assert(RC->entry[e].ready && RC->entry[e].rom);
  assert(control_rasters>rasters);
  for(int i=0;i<8;i++)if(control_current_mask&(1u<<i)){
   int e=RC->controls[i].entry,h=units_of(E[e].kind)*UNIT;
   memset(reference,0,sizeof reference);gfx_fb fb={0};fb.px=reference;fb.stride=ROW_W;fb.h=h;fb.ox=-UI_ROW_X0;fb.cx1=ROW_W;
   row_neutral=1;row_overlay=!control_combined(e);draw_entry(&fb,e,0,(float)h,0);row_neutral=row_overlay=0;
   for(int y=0;y<h;y++)for(int x=0;x<ROW_W;x++){
    unsigned green=(reference[y*ROW_W+x]>>5)&63,expected=(green*255+31)/63,actual=0;
    int sx=x-RC->controls[i].x,sy=y-RC->controls[i].y;
    if((unsigned)sx<(unsigned)RC->controls[i].s.w && (unsigned)sy<(unsigned)RC->controls[i].s.h)actual=RC->controls[i].pixels[sy*RC->controls[i].s.w+sx];
    assert(actual==expected);checked++;
   }
  }
 }
 assert(UI_F_BODY==body && UI_F_LABEL==label && UI_F_TITLE==title);
 printf("PASS font-switch source invalidation: %u exact overlay pixels across ROM->rawA->rawB->ROM; raw/static pending publications discarded\n",checked);
}
static void background_ghost_regression(void)
{
    nos_native_menu_rom_set(0);set_settings_cache_release();assert(rows_init());while(!RC->lut_ready)rows_palette_step();
 S.build_t0=nos_t-100;S.flash_e=-1;S.L.pressed=-1;S.arm_e=-1;S.L.bar_alpha=0;
    for(int id=0;id<I_COUNT;id++)S.vt0[id]=nos_t-100;
    for(int e=0;e<NE;e++){
        row_cancel();row_start(e);int steps=0;
        while(RC->job.active){row_step_job();assert(++steps<2000);}
    }
    ui_fblur_coverage_enable(1);
    unsigned ghosts=0,temporal=0;
    for(int cold=0;cold<2;cold++)for(int direction=-1;direction<=1;direction+=2){
        int victim=-1;
        float origin=1000.25f;
        S.L.sc.x=origin;native_pose=-64;native_exposure=27*256;
        nos_frame++;controls_tick();
        memset(full,0,sizeof full);gfx_fb fb={0};fb.px=full;fb.h=GFX_H;fb.cx1=GFX_W;
        settings_draw(&fb);
        nos_history_reset();nos_history_begin(nos_frame);
        assert(nos_history_apply(&fb,0,0,GFX_W,GFX_H,0));
        /* A54px movement leaves clearly separated old ink in today's black gaps. */
        S.L.sc.x=origin+direction*54;
        if(cold)for(int e=0;e<NE;e++){
            float y=LTOP+S.start[e]*UNIT-list_off();
            if(entry_split(e) && y>80 && y+units_of(E[e].kind)*UNIT<GFX_H-20){victim=e;break;}
        }
        assert(!cold || victim>=0);
        if(cold)RC->entry[victim].ready=0;
        nos_frame++;nos_history_begin(nos_frame);controls_tick();
        memset(striped,0,sizeof striped);fb.px=striped;settings_draw(&fb);
        static uint16_t current[GFX_W*GFX_H];memcpy(current,striped,sizeof current);
        for(int y=0;y<GFX_H;y+=16){
            gfx_fb st=fb;st.px=striped+y*GFX_W;st.y_base=y;st.h=GFX_H-y<16?GFX_H-y:16;
            nos_exposure_job job;assert(nos_exposure_prepare(&job,&st,0,y,GFX_W,y+st.h,1,0));nos_exposure_apply(&job);
        }
        int coldtop=cold?(int)(LTOP+S.start[victim]*UNIT-list_off()):-1;
        int coldend=cold?coldtop+units_of(E[victim].kind)*UNIT:-1;
        for(int y=0;y<GFX_H;y++)for(int x=UI_ROW_X0;x<=UI_ROW_X1;x++){
            int at=y*GFX_W+x;
            if(cold && y>=coldtop && y<coldend){temporal+=striped[at]!=current[at];continue;}
            assert(striped[at]==current[at]);
            ghosts+=full[at]!=0 && current[at]==0;
        }
        if(cold)RC->entry[victim].ready=1;
    }
    assert(ghosts>1000 && temporal>0);
    printf("PASS native backing: %u old-pose ink pixels stay current black; %u genuinely cold-row pixels retain temporal fallback, both directions\n",ghosts,temporal);
}
int main(int argc,char **argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 assert(!settings_sim_main(argc,argv));assert(rows_init());while(!RC->lut_ready)rows_palette_step();
 immutable_rom_ready();
 font_switch_sources();
 S.build_t0=nos_t-100;S.flash_e=-1;S.L.pressed=-1;S.arm_e=-1;
 for(int i=0;i<I_COUNT;i++)S.vt0[i]=nos_t-100;
 unsigned validated=0;clock_t started=clock();
 for(int e=0;e<NE;e++)if(entry_split(e)){
  int h=units_of(E[e].kind)*UNIT,id=E[e].id;
  S.L.sc.x=LTOP+S.start[e]*UNIT-150;
  for(int phase=0;phase<8;phase++){
   S.vt0[id]=nos_t-phase/(float)M_FPS;
   if(E[e].kind==K_TOG)tw_set(&S.tog[id],phase/7.f);
   controls_tick();
   int slot=-1;for(int i=0;i<8;i++)if(RC->controls[i].entry==e)slot=i;
   assert(slot>=0 && RC->controls[slot].sig==control_sig(e));
   memset(reference,0,sizeof reference);gfx_fb fb={0};fb.px=reference;fb.stride=ROW_W;fb.h=h;
   fb.ox=-UI_ROW_X0;fb.cx1=ROW_W;
   row_neutral=1;row_overlay=!control_combined(e);draw_entry(&fb,e,0,(float)h,0);row_neutral=row_overlay=0;
   for(int y=0;y<h;y++)for(int x=0;x<ROW_W;x++){
    unsigned green=(reference[y*ROW_W+x]>>5)&63,expected=(green*255+31)/63,actual=0;
    int sx=x-RC->controls[slot].x,sy=y-RC->controls[slot].y;
    if((unsigned)sx<(unsigned)RC->controls[slot].s.w && (unsigned)sy<(unsigned)RC->controls[slot].s.h)
     actual=RC->controls[slot].pixels[sy*RC->controls[slot].s.w+sx];
    assert(actual==expected);validated++;
   }
   unsigned rasters=control_rasters;controls_tick();assert(control_rasters==rasters);
   unsigned signature=entry_sig(e);
   if(E[e].kind==K_TOG){tw_set(&S.tog[id],.123f);assert(entry_sig(e)==signature);}
   else if(!control_combined(e)){
    char saved[sizeof S.val[id]];memcpy(saved,S.val[id],sizeof saved);
    strcpy(S.val[id],"2");if(!value_under(E[e].label,S.val[id],S.unit[id]))assert(entry_sig(e)==signature);
    memcpy(S.val[id],saved,sizeof saved);
   }
  }
 }
 /* Same atlas sampled through full frame or translated packed stripes is exact. */
 S.L.sc.x=113.25f;controls_tick();native_pose=-64;native_exposure=3*256+79;
 memset(full,0,sizeof full);memset(striped,0,sizeof striped);
 gfx_fb fb={0};fb.px=full;fb.h=GFX_H;fb.cx1=GFX_W;settings_draw(&fb);
 unsigned signature_walks=entry_signature_walks,geometry_work=entry_geometry_evaluations;
 for(int y=0;y<GFX_H;y+=16){gfx_fb st=fb;st.px=striped+y*GFX_W;st.y_base=y;st.h=GFX_H-y<16?GFX_H-y:16;settings_draw(&st);}
 assert(!memcmp(full,striped,sizeof full));
 assert(entry_signature_walks==signature_walks);
 assert(entry_geometry_evaluations==geometry_work);
 background_ghost_regression();
 printf("PASS Settings controls: %u independent atlas pixels; unchanged raster/signature reuse; exact full/stripe output; rasters%u workpixels%u hostCPU%.3fms\n",validated,control_rasters,control_work_pixels,1000.0*(clock()-started)/CLOCKS_PER_SEC);
 return 0;
}
