/* Real assets and an independent scalar mix oracle, no simulator object mutation. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../nos/apps/sys_launcher.c"
#include "../nos/render_history.h"
#include "../nos/render_exposure.h"
extern int sim_program_main(int,char **);
static unsigned random_state=0x713ab2u;
static unsigned next_random(void){random_state=random_state*1664525u+1013904223u;return random_state;}
static uint16_t pixels[466*36+32];
static uint64_t image_energy(const row_spr *row,const row_geo *g,int old,int weighted) {
    uint64_t sum=0;int lev=g->lev;
    uint16_t old_lut[256];
    if(old) {
        float u=fabsf((float)g->cy-UI_CY)/ROW_P;
        float f=1-.40f*m_smoothstep(0,1,u)-.26f*m_smoothstep(1,2,u);
        lev=(int)(f*16+.5f);
        for(int i=0;i<256;i++)old_lut[i]=grey565((i*lev+8)/16);
    }
    int var=old?(fabsf((float)g->cy-UI_CY)/ROW_P<.25f?2:fabsf((float)g->cy-UI_CY)/ROW_P<.5f?1:0):g->var;
    const uint8_t *a=row->icon[var].px,*b=row->icon[g->other].px;
    for(int i=0;i<ISPR*ISPR;i++) {
        unsigned index=old?a[i]:(a[i]*(64-g->mix)+b[i]*g->mix+32)>>6;
        uint16_t c=old?old_lut[index]:L->lut[lev][index];
        if(weighted) {
            unsigned red=(c>>11)*255/31,green=((c>>5)&63)*255/63,blue=(c&31)*255/31;
            sum+=(uint64_t)red*2126u+(uint64_t)green*7152u+(uint64_t)blue*722u;
        } else sum+=(c>>11)+((c>>5)&63)+(c&31);
    }
    return sum;
}
static void assets_and_continuity(void) {
    assert(L&&L->rows[0].ready&&s_tables_ready);
    for(int distance=0;distance<=2;distance++) {
        row_geo g;assert(geo(0,(float)(distance*ROW_P),&g));
        int old_level=distance==0?16:distance==1?10:5;
        assert(g.lev==old_level*4&&g.mix==0);
        assert(g.var==(distance?0:2));
        for(int i=0;i<256;i++)assert(L->lut[g.lev][i]==grey565((i*old_level+8)/16));
    }
    for(int i=0;i<256;i++)for(int level=0;level<=NLEV;level++) {
        uint16_t c=L->lut[level][i];int red=c>>11,green=(c>>5)&63,blue=c&31;
        assert(red==blue&&abs(green-2*red)<=1);
        if(!(level&3))assert(c==grey565((i*(level>>2)+8)/16));
        if(level){uint16_t p=L->lut[level-1][i];assert(red>=(p>>11)&&green>=((p>>5)&63));}
    }
    for(int weighted=0;weighted<2;weighted++) {
    uint64_t previous_old=0,previous_new=0,max_old=0,max_new=0;int max_new_at=0;
    for(int d=14;d<=54;d++) {
        row_geo g;assert(geo(0,(float)d,&g));
        uint64_t a=image_energy(&L->rows[0],&g,1,weighted),b=image_energy(&L->rows[0],&g,0,weighted);
        if(d>14){uint64_t da=previous_old>a?previous_old-a:a-previous_old;
            uint64_t db=previous_new>b?previous_new-b:b-previous_new;
            if(da>max_old)max_old=da;
            if(db>max_new){max_new=db;max_new_at=d;}}
        previous_old=a;previous_new=b;
    }
    printf("LAUNCHER continuity weighted=%d old_max_step=%llu new=%llu at_distance=%d\n",weighted,(unsigned long long)max_old,(unsigned long long)max_new,max_new_at);
    assert(max_new<max_old);
    }
    row_geo a={0},b={0};a.lev=44;a.var=1;a.other=1;b.lev=0;
    assert(row_image_key(0,&a,0,0,0)!=row_image_key(1,&b,0,0,0));
    b=a;b.mix=1;assert(row_image_key(0,&a,0,0,0)!=row_image_key(0,&b,0,0,0));
    assert(row_image_key(0,&a,0,0,0)!=row_image_key(0,&a,1,0,0));
    assert(row_image_key(0,&a,0,0,0)!=row_image_key(0,&a,0,1,0));
    assert(row_image_key(0,&a,0,0,0)!=row_image_key(0,&a,0,0,1));
}
static void clipping_oracle(void) {
    uint8_t a[76*76],b[76*76];uint16_t lut[256];
    for(int i=0;i<76*76;i++){a[i]=(uint8_t)next_random();b[i]=(uint8_t)next_random();}
    for(int i=0;i<256;i++)lut[i]=(uint16_t)next_random();
    sprite8 sa={a,76,76},sb={b,76,76};
    for(int test=0;test<500;test++) {
        int width=100+(int)(next_random()%300),base=(int)(next_random()%(467-width));
        gfx_fb fb={0};fb.px=pixels+16;fb.stride=width;fb.x_base=base;
        fb.h=1+(int)(next_random()%32);fb.y_base=(int)(next_random()%440);
        fb.ox=(int)(next_random()%81)-40;fb.oy=(int)(next_random()%81)-40;
        fb.cx0=base+(int)(next_random()%(unsigned)(width/3));
        fb.cx1=base+width-(int)(next_random()%(unsigned)(width/3));
        fb.dim=(uint8_t)(test%3?0:127);
        int x=base-40+(int)(next_random()%(unsigned)width),y=fb.y_base-70+(int)(next_random()%110);
        unsigned mix=next_random()%65;
        for(unsigned i=0;i<sizeof pixels/sizeof pixels[0];i++)pixels[i]=0x5a5a;
        icon_mix_blit(&fb,&sa,&sb,x,y,lut,123,mix);
        for(int row=0;row<fb.h;row++)for(int col=0;col<width;col++) {
            int sx=base+col-x-fb.ox,sy=fb.y_base+row-y-fb.oy;
            uint16_t expected=0x5a5a;
            if(sx>=0&&sx<76&&sy>=0&&sy<76&&base+col>=fb.cx0&&base+col<fb.cx1) {
                unsigned index=(a[sy*76+sx]*(64-mix)+b[sy*76+sx]*mix+32)/64;
                expected=gfx_ink(&fb,lut[index]);
            }
            assert(fb.px[row*width+col]==expected);
        }
        for(int i=0;i<16;i++)assert(pixels[i]==0x5a5a);
        for(unsigned i=16+(unsigned)width*fb.h;i<sizeof pixels/sizeof pixels[0];i++)assert(pixels[i]==0x5a5a);
    }
    gfx_rec record={0};gfx_fb fb={0};fb.rec=&record;
    icon_mix_blit(&fb,&sa,&sb,10,20,lut,456,32);assert(record.n==1);
    assert(record.c[0].x0==10&&record.c[0].y0==20&&record.c[0].x1==86&&record.c[0].y1==96);
}


static void native_backing_history(void)
{
 static uint16_t previous[GFX_W*GFX_H],current[GFX_W*GFX_H],painted[GFX_W*GFX_H];
 S.mode=M_DRAG;S.t_enter=nos_t-100;P.id=-1;P.down=0;P.until=0;
 for(int attempt=0;s_baked<napps() && attempt<50000;attempt++)sys_launcher_prewarm();
 for(int i=0;i<napps();i++)assert(L->rows[i].ready && L->rows[i].blurred);
 ui_fblur_coverage_enable(1);
 unsigned ghosts=0,temporal=0;
 for(int edge=0;edge<3;edge++)for(int cold=0;cold<(edge?1:2);cold++)for(int direction=-1;direction<=1;direction+=2){
  float origin=edge==0?3*ROW_P+.25f:edge==1?-69.25f:max_x()+69.25f;
  S.x=origin;native_pose=(int)lroundf((floorf(S.x+.5f)-S.x)*256);native_exposure=27*256;
  nos_frame++;memset(previous,0,sizeof previous);
  gfx_fb fb={0};fb.px=previous;fb.h=GFX_H;fb.cx1=GFX_W;l_draw(&fb);
  nos_history_reset();nos_history_begin(nos_frame);assert(nos_history_apply(&fb,0,0,GFX_W,GFX_H,0));
  S.x=origin+direction*54;
  int victim=(int)floorf(S.x/ROW_P+.5f);assert(!cold || (victim>=0 && victim<napps()));
  if(cold)L->rows[victim].blurred=0;
  nos_frame++;nos_history_begin(nos_frame);memset(current,0,sizeof current);fb.px=current;l_draw(&fb);
  memcpy(painted,current,sizeof current);fb.px=painted;
  for(int y=0;y<GFX_H;y+=16){
   gfx_fb stripe=fb;stripe.px=painted+y*GFX_W;stripe.y_base=y;stripe.h=GFX_H-y<16?GFX_H-y:16;
   nos_exposure_job job;assert(nos_exposure_prepare(&job,&stripe,0,y,GFX_W,y+stripe.h,1,0));nos_exposure_apply(&job);
  }
  int cy=(int)lroundf(UI_CY+victim*ROW_P-floorf(S.x+.5f));
  for(int y=0;y<GFX_H;y++)for(int x=ICON_X-ISPR/2;x<GFX_W-NAME_MARGIN;x++){
   int at=y*GFX_W+x;
   if(cold && y>=cy-ROW_P/2 && y<cy+ROW_P/2){temporal+=painted[at]!=current[at];continue;}
   if(painted[at]!=current[at]){fprintf(stderr,"BACKING mismatch edge%d cold%d dir%d x%d y%d prev%x current%x painted%x victim%d cy%d\n",edge,cold,direction,x,y,previous[at],current[at],painted[at],victim,cy);abort();}ghosts+=previous[at]!=0 && current[at]==0;
  }
  if(cold)L->rows[victim].blurred=1;
 }
 assert(ghosts>1000 && temporal>0);
 printf("PASS launcher native backing: %u old-ink pixels stay current black, %u cold-row temporal changes; prefix/suffix/both directions\n",ghosts,temporal);
}
static void fractional_motion(void){
 static uint16_t a[GFX_W*GFX_H],b[GFX_W*GFX_H],previous[GFX_W*GFX_H];
 S.mode=M_DRAG;S.t_enter=nos_t-100;P.id=-1;P.down=0;P.until=0;
 unsigned changes=0;
 for(int frame=0;frame<12;frame++){
  S.x=3*ROW_P+frame*.25f;
  float rounded=floorf(S.x+.5f);
  native_pose=(int)lroundf((rounded-S.x)*256);native_exposure=3*256+80;
  gfx_fb fb={0};fb.px=a;fb.h=GFX_H;fb.cx1=GFX_W;memset(a,0,sizeof a);l_draw(&fb);
  memset(b,0,sizeof b);
  for(int y=0;y<GFX_H;y+=16){gfx_fb st=fb;st.px=b+y*GFX_W;st.y_base=y;st.h=GFX_H-y<16?GFX_H-y:16;l_draw(&st);}
  assert(!memcmp(a,b,sizeof a));
  if(frame){assert(memcmp(a,previous,sizeof a));changes++;}
  memcpy(previous,a,sizeof a);
 }
 float x=S.x,v=S.v;int mode=S.mode;
 sys_launcher_cache_reset();assert(!L && !s_tables_ready && J.item<0);
 assert(S.x==x && S.v==v && S.mode==mode);
 puts("PASS launcher fractional pose everyquarterpixel, full/stripeexact and coldresetpreservesinput");
 assert(changes==11);
}
int main(int argc,char **argv) {
    nos_native_menu_rom_set(0);
    assert(!sim_program_main(argc,argv));assets_and_continuity();clipping_oracle();native_backing_history();fractional_motion();
    puts("PASS launcher continuity, resting ink endpoints, parameter keys and500 packed/translated/dim/clipping cases");
    return 0;
}
