#include "../nos/apps/sys_notifs.c"
#include <assert.h>
#include <time.h>

extern int sim_program_main(int,char **);
#include "sys_notifs_stripe_reference.h"
static uint16_t A[GFX_W*48+8],B[GFX_W*48+8];
static unsigned seed=19;
static unsigned rnd(void){seed=seed*1664525u+1013904223u;return seed;}
int main(int argc,char **argv){
 int rc=sim_program_main(argc,argv);if(rc)return rc;
 printf("FONT body asc=%d line=%d label asc=%d line=%d\n",UI_F_BODY->ascent,UI_F_BODY->line_height,UI_F_LABEL->ascent,UI_F_LABEL->line_height);
 for(int font=0;font<2;font++) {
  const font_t *ff=font?UI_F_LABEL:UI_F_BODY;int lo=1000,hi=-1000;
  for(int i=0;i<ff->nglyphs;i++){int y=ff->glyphs[i].yoff,z=y+ff->glyphs[i].h;if(y<lo)lo=y;if(z>hi)hi=z;}
  printf("FONT extent%d=%d..%d\n",font,lo,hi);
  assert(lo>=-(int)ff->ascent-2 && hi<=(int)ff->line_height-ff->ascent+2);
 }
 clock_t oldtime=0,newtime=0;unsigned samples=0;

 svc_notif note;memset(&note,0,sizeof note);
 strcpy(note.app,"Messages");strcpy(note.title,"Long title with descenders gypq and numbers123456789");
 strcpy(note.body,"First body line with gypq and a newline\nSecond body line goes off the card for clipping.");
 N.now=1700000400;note.t_epoch=N.now-50;
 for(int j=0;j<6000;j++) {
  note.icon=1+rnd()%(DOT_ICON_COUNT-1);
  if(j%3==0)note.body[0]=0;else if(!note.body[0])strcpy(note.body,"First line gypq\nSecond body line with descenders and words");
  float h=(j%7==0)?(float)((int)(rnd()%350)-50):card_h_of(&note),y=((int)(rnd()%1000)-300)+(rnd()%4)*.25f,x=(int)(rnd()%90)+20;
  float fade=(rnd()%17)/16.f;int pressed=rnd()&1;

  int packed=rnd()%3==0,width=packed?89:GFX_W,origin=packed?25:0;
  for(unsigned k=0;k<sizeof A/sizeof A[0];k++)A[k]=B[k]=0x2945;
  gfx_fb a={0},b={0};a.px=A+1;b.px=B+1;a.stride=b.stride=width;a.x_base=b.x_base=origin;
  a.cx0=b.cx0=origin;a.cx1=b.cx1=origin+width;
  a.y_base=b.y_base=(int)(rnd()%520)-20;a.h=b.h=1+rnd()%48;
  a.oy=b.oy=(int)(rnd()%161)-80;a.ox=b.ox=(int)(rnd()%81)-40;
  a.dim=b.dim=rnd()%33;
  clock_t t=clock();draw_card_reference(&a,&note,x,y,h,fade,pressed);oldtime+=clock()-t;
  t=clock();draw_card(&b,&note,x,y,h,fade,pressed);newtime+=clock()-t;
  if(memcmp(A,B,sizeof A)) {printf("FAIL callback%d y%.2f h%.1f stripe%d+%d oy%d\n",j,y,h,a.y_base,a.h,a.oy);return 2;}
  if(j%17==0) {
   static gfx_rec ra,rb;memset(&ra,0,sizeof ra);memset(&rb,0,sizeof rb);
   a.rec=&ra;b.rec=&rb;
   draw_card_reference(&a,&note,x,y,h,fade,pressed);draw_card(&b,&note,x,y,h,fade,pressed);
   if(memcmp(&ra,&rb,sizeof ra)) {puts("FAIL recorder changed");return 2;}
  }
  samples++;
 }
 printf("PASS %u randomized callback stripes; recorder unchanged; HOST_ONLY old=%.3fms candidate=%.3fms\n",samples,1000.*oldtime/CLOCKS_PER_SEC,1000.*newtime/CLOCKS_PER_SEC);
 return 0;
}
