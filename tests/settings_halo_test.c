/* Independent padded-cache reference, one entry resident at a time. */
#include "../nos/apps/set_settings.c"
#include <assert.h>
#include "settings_bake_reference.h"
int capture_sim_main(int,char**);
int main(int argc,char**argv){nos_native_menu_rom_set(0);assert(!capture_sim_main(argc,argv));assert(rows_init());while(!RC->lut_ready)rows_palette_step();S.build_t0=nos_t-100;S.arm_e=-1;S.flash_e=-1;for(int id=0;id<I_COUNT;id++)S.vt0[id]=nos_t-100;row_cancel();for(int e=0;e<NE;e++)entry_free(&RC->entry[e]);
for(int e=0;e<NE;e++){assert(entry_bake_ref(e));entry_cache expected=RC->entry[e];memset(&RC->entry[e],0,sizeof(entry_cache));row_start(e);int steps=0;while(RC->job.active){row_step_job();assert(++steps<3000);}assert(RC->entry[e].ready);for(int k=0;k<9;k++){entry_image *a=&expected.image[k],*b=&RC->entry[e].image[k];assert(a->x==b->x && a->y==b->y && a->s.w==b->s.w && a->s.h==b->s.h);assert(!memcmp(a->s.px,b->s.px,a->s.w*a->s.h));}entry_free(&expected);entry_free(&RC->entry[e]);}puts("PASS all Settings padded levels match independent reference");return 0;}