#include "../../nos/apps/set_settings.c"

#include <windows.h>
#include <assert.h>
int assets_sim_main(int,char **);
static unsigned exported;
static void dump(const char *family,int e,int layout,int level,const sprite8 *s,int x,int y,int steps){
 char path[256];snprintf(path,sizeof path,"build/native_menu_asset_export/raw/%s_%02d_%d_%d.raw",family,e,layout,level);
 FILE *f=fopen(path,"wb");assert(f);unsigned bytes=(unsigned)s->w*s->h;
 if(bytes)assert(fwrite(s->px,1,bytes,f)==bytes);fclose(f);
 printf("ASSET,%s,%d,%d,%d,%d,%d,%d,%d,%u,%d\n",family,e,layout,level,s->w,s->h,x,y,bytes,steps);exported+=bytes;
}

int main(int argc,char **argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 nos_native_menu_rom_set(0);
 assert(!assets_sim_main(argc,argv));assert(rows_init());while(!RC->lut_ready)rows_palette_step();
 S.build_t0=nos_t-100;S.arm_e=-1;
 for(int theme=0;theme<2;theme++){
  ui_theme_inverted(theme);rows_palette();while(!RC->lut_ready)rows_palette_step();
  char path[256];snprintf(path,sizeof path,"build/native_menu_asset_export/settings_red_%d.lut",theme);
  FILE *pal=fopen(path,"wb");assert(pal);assert(fwrite(RC->lut[1],1,sizeof RC->lut[1],pal)==sizeof RC->lut[1]);fclose(pal);
 }
 FILE *pal=fopen("build/native_menu_asset_export/settings_neutral.lut","wb");assert(pal);
 assert(fwrite(RC->lut[0],1,sizeof RC->lut[0],pal)==sizeof RC->lut[0]);fclose(pal);
 ui_theme_inverted(0);
 for(int e=0;e<NE;e++){
  int id=E[e].id;
  for(int layout=0;layout<(E[e].kind==K_NAV || E[e].kind==K_FLIP || E[e].kind==K_DANGER?2:1);layout++){
   if(E[e].kind==K_DANGER)ui_theme_inverted(layout);
   if(id!=I_NONE){strcpy(S.val[id],layout?"WWWWWWWWWWWWWWWWWWWWWWW":"");S.unit[id][0]=0;S.vt0[id]=nos_t-100;}
   row_cancel();row_start(e);int steps=0;
   while(RC->job.active){row_step_job();assert(++steps<2000);}
   assert(RC->entry[e].ready);
   for(int k=0;k<9;k++){entry_image *im=&RC->entry[e].image[k];dump("settings",e,layout,k,&im->s,im->x,im->y,steps);}
   entry_free(&RC->entry[e]); /* Exported bytes are on disk; bound host cache memory. */
  }
 }
 printf("TOTAL settings %u\n",exported);return 0;
}
