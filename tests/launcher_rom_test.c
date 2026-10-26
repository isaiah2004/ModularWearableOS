/* Actual immutable launcher art, mutable fallback and ownership transitions. */
#include "../nos/apps/sys_launcher.c"
#include "psram.h"
#include "fonts/nos_fonts.h"
#include <assert.h>
#include <stdio.h>
extern int sim_program_main(int,char **);
static row_spr rom[MAXAPP];
static uint16_t raw_frame[GFX_W*GFX_H],rom_frame[GFX_W*GFX_H];
static void equal(const sprite8 *a,const sprite8 *b){
 assert(a->w==b->w && a->h==b->h && a->px && b->px);
 assert(!memcmp(a->px,b->px,(size_t)a->w*a->h));
}
int main(int argc,char **argv){
 assert(!sim_program_main(argc,argv));
 nos_native_menu_rom_set(1);sys_launcher_cache_reset();
 unsigned before=nos_mem_used(),failures=nos_mem_failures(),calls=0;
 while(s_baked<napps() && calls++<12)sys_launcher_prewarm();
 assert(calls<12 && s_baked==napps() && L && s_tables_ready);
 assert(nos_mem_used()-before==sizeof *L); /* metadata/workspaces, no ROM pixel allocations */
 for(int i=0;i<napps();i++){
  assert(L->rows[i].rom && L->rows[i].ready && L->rows[i].blurred);rom[i]=L->rows[i];
  assert(native_menu_launcher_matches((unsigned)i,nos_app_list[i].name,nos_app_list[i].icon));
  assert(!native_menu_launcher_matches((unsigned)i,"different",nos_app_list[i].icon));
  assert(!native_menu_launcher_matches((unsigned)i,nos_app_list[i].name,-123));
 }
 for(int l=0;l<=NLEV;l++)for(int i=0;i<256;i++)assert(launcher_palette(l,0)[i]==level_ink(i,l));
 for(int i=0;i<256;i++)assert(launcher_palette(0,1)[i]==grey565(i<=26?i*51/26:51+(i-26)*(255-51)/(255-26)));
 S.x=8*ROW_P;sys_launcher_trim();sys_launcher_card_prepare();sys_launcher_cache_reset();
 assert(!L && nos_mem_used()==before && nos_mem_failures()==failures);
 nos_native_menu_rom_set(0);calls=0;
 while(s_baked<napps() && calls++<50000)sys_launcher_prewarm();
 assert(calls<50000 && s_baked==napps());
 for(int i=0;i<napps();i++){
  row_spr *r=&L->rows[i];assert(!r->rom);
  for(int k=0;k<NVAR;k++)equal(&rom[i].icon[k],&r->icon[k]);
  equal(&rom[i].name,&r->name);
  assert(rom[i].name_base==r->name_base);
  for(int k=0;k<NBLUR;k++){
   equal(&rom[i].bicon[k].s,&r->bicon[k].s);equal(&rom[i].bname[k].s,&r->bname[k].s);
   assert(rom[i].bicon[k].y0==r->bicon[k].y0 && rom[i].bname[k].y0==r->bname[k].y0);
  }
 }
 /* Same frozen source/pose through the complete renderer, including chord
  * crops, fractional placement, icon-size blends, blur levels and framebuffer dim. */
 nos_native_menu_rom_enabled=1;const int16_t *curve=native_menu_launcher_curve();nos_native_menu_rom_enabled=0;
 for(int y=0;y<(int)(sizeof L->curve/sizeof L->curve[0]);y++)assert(curve[y]==L->curve[y]);
 const float poses[]={-69.25f,0,.25f,18.49f,23.5f,46.25f,3*ROW_P+.5f,9*ROW_P+.75f};
 const int exposures[]={0,1*256+79,15*256,22*256,30*256,45*256,60*256,75*256,90*256,105*256,120*256};
 S.t_enter=nos_t-100;P.id=-1;P.down=0;P.until=0;
 for(unsigned pose=0;pose<sizeof poses/sizeof *poses;pose++)for(unsigned exposure=0;exposure<sizeof exposures/sizeof *exposures;exposure++){
  S.x=poses[pose];native_pose=(int)lroundf((floorf(S.x+.5f)-S.x)*256);native_exposure=exposures[exposure];
  gfx_fb f={0};f.px=raw_frame;f.h=GFX_H;f.cx1=GFX_W;f.dim=(pose&1)?9:0;
  memset(raw_frame,0,sizeof raw_frame);nos_frame++;l_draw(&f);
  /* Isolate immutable drawing without discarding the already-tested owned
   * reference. Normal selector changes are exercised separately below. */
  void *saved=L;L=NULL;nos_native_menu_rom_enabled=1;s_rom_mode=1;s_tables_ready=0;
  memset(rom_frame,0,sizeof rom_frame);f.px=rom_frame;nos_frame++;l_draw(&f);
  assert(!memcmp(raw_frame,rom_frame,sizeof raw_frame));
  L=saved;nos_native_menu_rom_enabled=0;s_rom_mode=0;s_tables_ready=1;
 }
 /* Selector changes release owned PSRAM before publishing borrowed views. */
 nos_native_menu_rom_set(1);calls=0;
 while((s_rom_mode!=1 || s_baked<napps()) && calls++<12)sys_launcher_prewarm();
 assert(calls<12 && L->rows[0].rom);
 /* Distinct unsupported fonts must not share the same mutable cache merely
  * because both select ROM=false. Compare actual complete renders and pixels. */
 const font_t *original_title=UI_F_TITLE;
 const font_t *sequence[]={&nf_body,&nf_display,original_title};
 uint64_t font_hash[3]={0};
 for(unsigned f=0;f<3;f++){
  UI_F_TITLE=sequence[f];unsigned steps=0;S.x=0;
  /* Large fallback fonts need only prepare the visible row. With the sports
   * entries, baking every oversized name exceeds the bounded cache budget.
   * Canonical ROM still binds every row without allocating pixel buffers. */
  while((s_source_title!=UI_F_TITLE || !L || !L->rows[0].ready ||
         (f==2 && s_baked<napps())) && steps++<50000)sys_launcher_prewarm();
  assert(steps<50000 && L && L->rows[0].ready);
  if(f==2)assert(s_baked==napps());
  assert(!!L->rows[0].rom==(f==2));
  S.x=0;S.t_enter=nos_t-100;native_pose=native_exposure=0;
  gfx_fb frame={0};frame.px=raw_frame;frame.h=GFX_H;frame.cx1=GFX_W;
  memset(raw_frame,0,sizeof raw_frame);nos_frame++;l_draw(&frame);
  uint64_t digest=1469598103934665603ULL;
  for(unsigned x=0;x<sizeof raw_frame/sizeof *raw_frame;x++)digest=(digest^raw_frame[x])*1099511628211ULL;
  font_hash[f]=digest;
  if(f==2){
   equal(&rom[0].name,&L->rows[0].name);
  } else {
   /* This row's own new title font is its published source, not the previous
    * unsupported font or a canonical ROM descriptor. */
   assert(L->rows[0].name_base==UI_F_TITLE->ascent+3);
  }
 }
 assert(font_hash[0]!=font_hash[1] && font_hash[1]!=font_hash[2] && font_hash[0]!=font_hash[2]);
 sys_launcher_cache_reset();
 /* ROM drawing remains available if its optional mutable owner cannot allocate. */
 nos_mem_fail_after(0);sys_launcher_prewarm();assert(!L);
 gfx_rec rec={0};gfx_fb fb={0};fb.rec=&rec;fb.h=GFX_H;fb.cx1=GFX_W;
 S.t_enter=nos_t-100;l_draw(&fb);assert(rec.n>1 && !L);
 nos_mem_fail_after(-1);sys_launcher_cache_reset();
 puts("PASS launcher ROM: all190images+48renderedposes exact to host raw bake; palettes/registry; bounded cold adoption; trim/reset/mode/font-A/font-B/free ownership; no-owner fallback");
 return 0;
}
