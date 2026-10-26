#include "../../nos/apps/sys_launcher.c"

#include <windows.h>
#include <stdio.h>
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
 assert(!assets_sim_main(argc,argv));
 for(int call=0;s_baked<napps() && call<50000;call++)sys_launcher_prewarm();
 FILE *curve=fopen("build/native_menu_asset_export/launcher_curve.bin","wb");assert(curve);assert(fwrite(L->curve,1,sizeof L->curve,curve)==sizeof L->curve);fclose(curve);
 FILE *pal=fopen("build/native_menu_asset_export/launcher_neutral.lut","wb");assert(pal);
 assert(fwrite(L->lut,1,sizeof L->lut,pal)==sizeof L->lut);fclose(pal);
 pal=fopen("build/native_menu_asset_export/launcher_press.lut","wb");assert(pal);
 assert(fwrite(L->lut_press,1,sizeof L->lut_press,pal)==sizeof L->lut_press);fclose(pal);
 for(int e=0;e<napps();e++){
  printf("APP,%d,%d,%s\n",e,nos_app_list[e].icon,nos_app_list[e].name);
  row_spr *r=&L->rows[e];assert(r->ready && r->blurred);
  for(int k=0;k<NVAR;k++)dump("icon",e,k,0,&r->icon[k],-ISPR/2,-ISPR/2,0);
  dump("name",e,0,0,&r->name,NAME_DX,-r->name_base+UI_F_TITLE->ascent/2-1,0);
  for(int k=0;k<NBLUR;k++){
   dump("icon",e,1,k+1,&r->bicon[k].s,-ISPR/2,r->bicon[k].y0,0);
   dump("name",e,0,k+1,&r->bname[k].s,NAME_DX,r->bname[k].y0,0);
  }
 }
 printf("TOTAL launcher %u\n",exported);return 0;
}
