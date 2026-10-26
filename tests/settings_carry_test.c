/* Actual Settings owner, stripe routing and allocation-failure fallback. */
#include "../nos/apps/set_settings.c"
#include "render_bounds.h"
#include <assert.h>
extern int sim_program_main(int,char**);
static uint16_t reference[GFX_W*GFX_H],candidate[GFX_W*GFX_H];
static void draw(uint16_t *pixels){
 memset(pixels,0xFA,sizeof reference);
 for(int y=0;y<GFX_H;y+=16){gfx_fb f={0};f.px=pixels+y*GFX_W;f.y_base=y;f.h=GFX_H-y<16?GFX_H-y:16;f.glass_only=1;int a=0,b=GFX_W;nos_glass_span(y,y+f.h,&a,&b);f.cx0=a;f.cx1=b;settings_draw(&f);}
}
int main(int argc,char**argv){
 assert(!sim_program_main(argc,argv));set_settings_carry_set(1);assert(set_settings_carry_get());
 assert(rows_init());while(!RC->lut_ready)rows_palette_step();assert(sizeof RC->carry_pool<=4096);
 S.build_t0=nos_t-100;S.flash_e=-1;S.L.pressed=-1;S.arm_e=-1;S.L.bar_alpha=0;
 for(int id=0;id<I_COUNT;id++)S.vt0[id]=nos_t-100;
 uint64_t hits=0,saved=0,copied=0;
 for(int pose=0;pose<48;pose++){
  nos_frame++;S.L.sc.x=(pose%24)*90+.25f;controls_tick();native_pose=-64;native_exposure=(pose&1?40:4)*256;
  set_settings_carry_set(0);draw(reference);
  set_settings_carry_set(1);draw(candidate);assert(!memcmp(reference,candidate,sizeof reference));
  hits+=settings_carry_stats.hits;saved+=settings_carry_stats.saved_reads;copied+=settings_carry_stats.copy_bytes;
  /* Toggle clears old continuations and diagnostics even within this frame. */
  set_settings_carry_set(0);assert(!RC->carry_pool.used && !settings_carry_stats.calls);
  set_settings_carry_set(1);assert(!RC->carry_pool.used);
 }
 assert(hits>100 && saved>100000 && copied>0);
 /* Re-publication, font and palette changes invalidate the borrowed sums. */
 RC->carry_pool.used=12;controls_tick();assert(!RC->carry_pool.used);
 const font_t *font=UI_F_LABEL;UI_F_LABEL=UI_F_BODY;RC->carry_pool.used=12;rows_palette();assert(!RC->carry_pool.used);UI_F_LABEL=font;rows_palette();controls_tick();
 ui_theme_inverted(1);RC->carry_pool.used=12;rows_palette();assert(!RC->carry_pool.used);ui_theme_inverted(0);rows_palette();controls_tick();
 S.flash_e=0;S.flash_t=nos_t;set_settings_carry_set(0);draw(reference);set_settings_carry_set(1);draw(candidate);assert(!memcmp(reference,candidate,sizeof reference));S.flash_e=-1;
 set_settings_cache_release();assert(!RC && !settings_carry_stats.calls);
 unsigned before=nos_mem_used();nos_mem_fail_after(0);rows_retry_at=0;rows_retry_room=0;
 assert(!rows_init() && !RC);draw(candidate);assert(!RC); /* Live fallback remains drawable. */
 nos_mem_fail_after(-1);rows_retry_at=0;rows_retry_room=0;assert(rows_init());assert(!RC->carry_pool.used);assert(nos_mem_used()-before==sizeof *RC);
 set_settings_carry_report();set_settings_cache_release();assert(nos_mem_used()==before);
 puts("PASS Settings carry:48 glass-clipped poses exact, external staging, toggle/font/theme/republish/pressed lifecycle, failed owner allocation and recovery");return 0;
}
