#include "../nos/apps/set_settings.c"
#include <assert.h>
#include <time.h>

extern int sim_program_main(int,char **);
#include "set_settings_stripe_reference.h"
static uint16_t A[GFX_W*48+8],B[GFX_W*48+8];
static unsigned seed=19;
static unsigned rnd(void){seed=seed*1664525u+1013904223u;return seed;}
int main(int argc,char **argv){
    nos_native_menu_rom_set(0); /* explicit authored runtime-bake reference mode */
 int rc=sim_program_main(argc,argv);if(rc)return rc;
 printf("FONT body asc=%d line=%d label asc=%d line=%d\n",UI_F_BODY->ascent,UI_F_BODY->line_height,UI_F_LABEL->ascent,UI_F_LABEL->line_height);
 for(int font=0;font<2;font++) {
  const font_t *ff=font?UI_F_LABEL:UI_F_BODY;int lo=1000,hi=-1000;
  for(int i=0;i<ff->nglyphs;i++){int y=ff->glyphs[i].yoff,z=y+ff->glyphs[i].h;if(y<lo)lo=y;if(z>hi)hi=z;}
  printf("FONT extent%d=%d..%d\n",font,lo,hi);
  assert(lo>=-(int)ff->ascent-2 && hi<=(int)ff->line_height-ff->ascent+2);
 }
 clock_t oldtime=0,newtime=0;unsigned samples=0;

 S.ext=~0u;S.arm_t=nos_t-1;S.build_t0=nos_t-1;
 for(int j=0;j<6000;j++) {
  int e=rnd()%NE;float h=units_of(E[e].kind)*30.f;if(j%7==0)h=(rnd()%160)+.5f;
  float y=((int)(rnd()%1000)-300)+(rnd()%4)*.25f;
  int pressed=rnd()&1;row_baking=(j%11)==0;
  S.build_t0=nos_t-(rnd()%12)/30.f;
  tw_set(&S.arm_tw[0],(rnd()%9)/8.f);tw_set(&S.arm_tw[1],(rnd()%9)/8.f);tw_set(&S.grey_tw,(rnd()%9)/8.f);
  for(int k=0;k<I_COUNT;k++) {
   strcpy(S.val[k],(k&1)?"VERY LONG VALUE":"29");strcpy(S.oval[k],"OLD VALUE");strcpy(S.unit[k],"GB");strcpy(S.ounit[k],"s");
   S.vt0[k]=nos_t-(rnd()%12)/30.f;tw_set(&S.tog[k],(rnd()%9)/8.f);
  }

  entry_draw_prepare(); /* synthetic mutations above precede this draw generation */
  int packed=rnd()%3==0,width=packed?89:GFX_W,origin=packed?25:0;
  for(unsigned k=0;k<sizeof A/sizeof A[0];k++)A[k]=B[k]=0x2945;
  gfx_fb a={0},b={0};a.px=A+1;b.px=B+1;a.stride=b.stride=width;a.x_base=b.x_base=origin;
  a.cx0=b.cx0=origin;a.cx1=b.cx1=origin+width;
  a.y_base=b.y_base=(int)(rnd()%520)-20;a.h=b.h=1+rnd()%48;
  a.oy=b.oy=(int)(rnd()%161)-80;a.ox=b.ox=(int)(rnd()%81)-40;
  a.dim=b.dim=rnd()%33;
  clock_t t=clock();draw_entry_reference(&a,e,y,h,pressed);oldtime+=clock()-t;
  t=clock();draw_entry(&b,e,y,h,pressed);newtime+=clock()-t;
  if(memcmp(A,B,sizeof A)) {printf("FAIL callback%d y%.2f h%.1f stripe%d+%d oy%d\n",j,y,h,a.y_base,a.h,a.oy);return 2;}
  if(j%17==0) {
   static gfx_rec ra,rb;memset(&ra,0,sizeof ra);memset(&rb,0,sizeof rb);
   a.rec=&ra;b.rec=&rb;
   draw_entry_reference(&a,e,y,h,pressed);draw_entry(&b,e,y,h,pressed);
   if(memcmp(&ra,&rb,sizeof ra)) {puts("FAIL recorder changed");return 2;}
  }
  samples++;
 }
 printf("PASS %u randomized callback stripes; recorder unchanged; HOST_ONLY old=%.3fms candidate=%.3fms\n",samples,1000.*oldtime/CLOCKS_PER_SEC,1000.*newtime/CLOCKS_PER_SEC);
 return 0;
}
