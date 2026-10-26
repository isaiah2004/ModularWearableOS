"""Actual face contracts with private objects, SD state and outputs on A:.

No shared build, device, COM port or external astronomy service is used.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/alpha05/checks/faces/contracts"
FLAGS = ["-O2", "-std=c99", "-DNOS_PSRAM_TRACKING", "-DGFX_COST_MODEL",
         "-Icore", "-Inos", "-Inos/fonts", "-Isim", "-Wall", "-Wextra",
         "-Wno-unused-parameter", "-Wno-format-truncation", "-Werror"]
SOURCES = ["sim/nos_sim.c", "sim/nos_hal_sim.c", "core/ephem.c", "nos/nos.c", "nos/apps/sys_home.c", "nos/faces/faces.c",
           "nos/faces/face_matrix.c", "nos/faces/face_split.c", "nos/faces/face_segment.c",
           "nos/faces/face_orrery.c", "nos/faces/orrery_events.c", "nos/faces/orrery_card.c"]
PREFIX = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "prefs.h"
#include "nos.h"
extern int sim_program_main(int,char**);
extern double sim_vclock_ms;
void record_fb(gfx_fb *fb,gfx_rec *rec) {
 memset(fb,0,sizeof *fb);memset(rec,0,sizeof *rec);
 fb->rec=rec;fb->h=GFX_H;fb->cx1=GFX_W;
}
'''
CASES = {
"matrix": ("nos/faces/face_matrix.c", r'''
int main(int argc,char **argv) {
 assert(!sim_program_main(argc,argv));
 if(M)hal_big_free(M);
 M=0;s_last_hm=-1;
 face_ctx c={0};c.enter=1;c.dt.hour=13;c.dt.minute=37;c.dt.second=20;c.t=nos_t;
 nos_prefs.hour24=1;face_matrix.tick(&c,1.f/M_FPS);assert(M&&M->built);
 unsigned char before[N*N],expected[N*N];memcpy(before,M->now,sizeof before);
 nos_prefs.hour24=0;face_matrix.tick(&c,1.f/M_FPS);pattern(expected,13,37);
 assert(!memcmp(M->now,expected,sizeof expected));assert(memcmp(before,M->now,sizeof before));
 assert(!memcmp(M->now,M->prev,sizeof expected));
 nos_prefs.hour24=1;c.dt.hour=1;face_matrix.tick(&c,1.f/M_FPS);
 memcpy(before,M->now,sizeof before);nos_prefs.hour24=0;face_matrix.tick(&c,1.f/M_FPS);
 assert(memcmp(before,M->now,sizeof before));
 hal_big_free(M);M=0;s_last_hm=-1;nos_mem_fail_after(0);
 face_matrix.tick(&c,1.f/M_FPS);assert(!M);
 gfx_rec rec={0};gfx_fb fb={0};record_fb(&fb,&rec);face_matrix.draw(&fb,&c);assert(rec.n>0);
 unsigned first=rec.c[0].hash;c.dt.minute++;record_fb(&fb,&rec);face_matrix.draw(&fb,&c);
 assert(rec.n>0&&first!=rec.c[0].hash);nos_mem_fail_after(-1);
 face_matrix.tick(&c,1.f/M_FPS);assert(M&&M->built);
 puts("PASS MATRIX immediate 12/24 format refresh, denied-allocation live clock and recovery");return 0;
}
'''),
"split": ("nos/faces/face_split.c", r'''
int main(int argc,char **argv) {
 assert(!sim_program_main(argc,argv));face_ctx c={0};c.enter=1;c.weather_valid=1;c.temp_c=20;
 char text[16];const char *label;uint16_t color;
 nos_prefs.units_metric=1;face_split.tick(&c,1.f/M_FPS);row_text(2,4,text,&label,&color);
 assert(!strcmp(text,"20\xB0" "C"));
 nos_prefs.units_metric=0;face_split.tick(&c,1.f/M_FPS);row_text(2,4,text,&label,&color);
 assert(!strcmp(text,"68\xB0" "F"));
 c.temp_c=-40;face_split.tick(&c,1.f/M_FPS);row_text(2,4,text,&label,&color);
 assert(!strcmp(text,"-40\xB0" "F"));
 nos_prefs.units_metric=1;c.temp_c=-.5;face_split.tick(&c,1.f/M_FPS);row_text(2,4,text,&label,&color);
 assert(!strcmp(text,"-1\xB0" "C"));
 puts("PASS SPLIT unit selection, C/F cue and signed rounding");return 0;
}
'''),
"segment": ("nos/faces/face_segment.c", r'''
int main(int argc,char **argv) {
 assert(!sim_program_main(argc,argv));
 for(int i=0;i<12;i++)if(s_font.g[i].px)hal_big_free(s_font.g[i].px);
 memset(&s_font,0,sizeof s_font);
 if(G){for(int q=0;q<4;q++)if(G->q[q].px)hal_big_free(G->q[q].px);hal_big_free(G);}G=0;s_cold=1;s_cold_i=0;
 svc_activity_t *a=(svc_activity_t*)svc_activity();a->steps=1100;a->kcal=44;a->active_min=0;a->goal=8000;
 face_ctx c={0};c.enter=1;c.steps=1100;c.dt.hour=13;c.dt.minute=37;
 nos_t+=1.f/M_FPS;c.t=nos_t;nos_mem_fail_after(0);face_segment.tick(&c,1.f/M_FPS);
 assert(s_val[2]==0&&s_target[2]==0&&!s_font.ok);
 gfx_rec rec={0};gfx_fb fb={0};record_fb(&fb,&rec);draw_time(&fb,&c,1,0,0);assert(rec.n>0);
 c.aod=1;record_fb(&fb,&rec);draw_time(&fb,&c,0,0,0);assert(rec.n>0);c.aod=0;
 nos_mem_fail_after(-1);int frames=0,cache_denial=0;
 while(s_cold&&frames++<200){
  int glyph=s_font.next,track=s_cold_i,allocated=G?G->allocated:0,cleared=G?G->clear_y:0,spanned=G?G->span_y:0;
  int deny=!cache_denial&&G&&G->allocated==2;
  if(deny)nos_mem_fail_after(0);
  nos_t+=1.f/M_FPS;c.t=nos_t;face_segment.tick(&c,1.f/M_FPS);
  if(deny){assert(!G);nos_mem_fail_after(-1);cache_denial=1;}
  assert(s_font.next-glyph<=1&&s_cold_i-track<=WARM_SEGS);
  if(G)assert(G->allocated-allocated<=1&&G->clear_y-cleared<=16&&G->span_y-spanned<=16);
  record_fb(&fb,&rec);draw_time(&fb,&c,1,0,0);assert(rec.n>0);
 }
 assert(!s_cold&&s_font.ok&&G&&G->ok&&s_cold_i==180&&cache_denial);
 printf("PASS SEGMENT authoritative zero minutes, denied-allocation clock, bounded font/quadrant/row/track prep (%d frames)\n",frames);return 0;
}
'''),
"cadence": ("nos/faces/faces.c", r'''
int main(int argc,char **argv) {
 assert(!sim_program_main(argc,argv));fk_xfade xf={0};float af,at;
 fk_xfade_tick(&xf,"MON 1",0,1);fk_xfade_tick(&xf,"TUE 2",1,0);
 const float old[]={.5625,.1875,0,0,0},now[]={0,0,.1875,.5625,1};
 for(int i=0;i<5;i++){fk_xfade_k(&xf,1+(float)i/M_FPS,&af,&at);assert(af==old[i]&&at==now[i]);}
 face_ctx c={0};c.dt.second=5;c.enter=0;s_sk.sec=-1;
 for(int i=0;i<M_FPS;i++){
  c.t=10+(float)i/M_FPS;c.dt.millis=(int)(1000.f*i/M_FPS+.5f);sec_tick(&c);
  float b=fk_blink(&c);
  if(i==M_FPS/2-1)assert(b==1);
  if(i==M_FPS/2)assert(b==.75);
 }
 s_sk.sec=-1;c.dt.millis=500;assert(fk_sec_k(&c)==M_FPS/2);
 printf("PASS face cross-fade counted at %d Hz; blink falling edge at 500 ms; fallback second phase\n",M_FPS);return 0;
}
'''),
"orrery": ("nos/faces/face_orrery.c", r'''
static double bisect_belt(double M,double e) {
 double lo=0,hi=2*3.14159265358979323846;
 for(int i=0;i<100;i++){double mid=(lo+hi)*.5;if(mid-e*sin(mid)<M)lo=mid;else hi=mid;}
 return (lo+hi)*.5;
}
int main(int argc,char **argv) {
 assert(!sim_program_main(argc,argv));orr_state *saved=s_orr;s_orr=0;
 face_ctx c={0};c.dt.hour=13;c.dt.minute=37;c.enter=1;
 gfx_rec rec={0};gfx_fb fb={0};record_fb(&fb,&rec);nos_mem_fail_after(0);
 face_orrery.draw(&fb,&c);assert(rec.n>0&&!s_orr);nos_mem_fail_after(-1);s_orr=saved;
 assert(orr_ok());S.jd_now=2461334.5;S.jd=S.jd_now;
 S.off=1e9;S.vel=100;S.ta_n=1;travel_limit();assert(S.jd==EP_JD_MAX&&S.vel==0&&!S.ta_n);
 S.off=-1e9;travel_limit();assert(S.jd==EP_JD_MIN);
 orrery_jump(1e20);assert(S.ta_to==EP_JD_MAX-S.jd_now);
 orrery_jump(-1e20);assert(S.ta_to==EP_JD_MIN-S.jd_now);
 orrery_jump(NAN);assert(isfinite(S.ta_to));
 S.off=EP_JD_MAX-S.jd_now-.01;S.jd=S.jd_now+S.off;S.d_ring=0;S.menu_open=0;S.ct=S.sc=S.zoom=1;
 tap_unlocked(CX+100,CY,6);assert(S.ta_to<=EP_JD_MAX-S.jd_now);
 S.locked=0;S.drag=4;S.d_a=0;S.d_ring=0;S.off=EP_JD_MAX-S.jd_now-.01;S.jd=S.jd_now+S.off;
 nos_input in={0};in.type=IN_DRAG;in.x=CX-100;in.y=CY;assert(face_orrery_input(&in));
 assert(S.jd==EP_JD_MAX&&S.vel==0);
 S.d_a=0;S.off=EP_JD_MIN-S.jd_now+.01;S.jd=S.jd_now+S.off;in.x=CX+100;in.y=CY;
 assert(face_orrery_input(&in));assert(S.jd==EP_JD_MIN&&S.vel==0);
 S.drag=0;S.time_touch=0;S.ta_n=0;S.belt_left=S.small_left=0;S.scan_start=0;S.scanning=0;
 events_update();assert(S.scan.jd0-1>=EP_JD_MIN&&S.scan.jd0+S.scan.days+1<=EP_JD_MAX);
 while(S.scanning)events_scan_step();
 unsigned gen=S.events_generation;events_update();assert(gen==S.events_generation);
 S.jd=EP_JD_MAX;S.off=S.jd-S.jd_now;S.scan_start=0;events_update();
 assert(S.scan.jd0-1>=EP_JD_MIN&&S.scan.jd0+S.scan.days+1<=EP_JD_MAX);
 rings_layout();assert(strstr(S.ring_state,"LIMIT"));assert(orrery_jd()==EP_JD_MAX);
 /* An irrelevant zero-initialized 'a' must not alter a line-up's rendering. */
 S.scanning=0;S.ringe=0;S.lines=S.show[0]=1;S.chip_ev=0;S.nev=1;
 S.ev[0]=(ep_event){.jd=S.jd,.kind=EV_LINEUP,.mask=0x78};
 for(int p=0;p<EP_N;p++){S.px[p]=40+45*p;S.py[p]=100+12*p;}
 fk_view view={0,0,GFX_W,GFX_H};gfx_rec reference_rec={0};
 record_fb(&fb,&reference_rec);draw_event(&fb,&view,&c);assert(reference_rec.n>0);
 S.ev[0].a=EP_MARS;record_fb(&fb,&rec);draw_event(&fb,&view,&c);
 assert(rec.n==reference_rec.n&&!memcmp(rec.c,reference_rec.c,(size_t)rec.n*sizeof *rec.c));
 for(int i=0;i<80&&!S.bk;i++){sim_vclock_ms+=40;hal_frame_end();belt_init(0);}assert(S.bk);S.view=V_BEYOND;S.from=-1;S.tx=1;S.sc=S.zoom=S.ct=S.ca=1;S.st=S.sa=S.panx=S.pany=0;
 const double dates[]={EP_JD_MIN+1,2415020,2451545,2461334.5,2465445,EP_JD_MAX-1};
 float worst=0;unsigned high_e=0;
 for(unsigned d=0;d<sizeof dates/sizeof *dates;d++)for(int k=0;k<S.nbelt;k++){
  const ast_k *a=S.bk+k;if(a->e<.9)continue;high_e++;
  float t=(float)(dates[d]-2451545.0),M=a->M0+a->n*(t-a->ep);
  M-=(int)(M/360.f)*360.f;if(M<0)M+=360;M*=D2RF;
  double E=bisect_belt(M,a->e),xp=a->a*(cos(E)-a->e),yp=a->b*sin(E);
  pp want=mkpp((float)(a->Px*xp+a->Qx*yp),(float)(a->Py*xp+a->Qy*yp),0),got=belt_exact(k,dates[d]);
  float gx,gy,rx,ry;proj(&got,&gx,&gy);proj(&want,&rx,&ry);
  float error=hypotf(gx-rx,gy-ry);if(error>worst)worst=error;
  if(error>=.1f)fprintf(stderr,"belt k=%d date=%.4f e=%.9f M=%.9f error=%.9f got=%.9f,%.9f want=%.9f,%.9f\n",k,dates[d],a->e,M,error,gx,gy,rx,ry);
  assert(error<.1f);
 }
 printf("PASS Orrery live clock under allocation denial, all travel inputs/model-edge scan limits, mask-only line-up rings; %u high-e belt/date cases vs bisection, max %.6f px\n",high_e,worst);return 0;
}
'''),
"events": ("nos/faces/orrery_events.c", r'''
static void frame(void){sim_vclock_ms+=25;nos_frame_run(25);}
int main(int argc,char **argv) {
 assert(!sim_program_main(argc,argv));nos_home_now();nos_prefs.face=8;nos_aod=0;
 int before=0;
 for(int i=0;i<180;i++){frame();orrery_events(&before);if(before&&orrery_events_progress()<100)break;}
 assert(before&&orrery_events_progress()<100);nos_push(&scr_orrery_events,TX_NONE);assert(E.n>0);
 int rows=E.n;item_t held=E.it[0];nos_input down={0};down.type=IN_DOWN;down.x=UI_CX;down.y=125;
 assert(scr_orrery_events.input(&down));assert(E.list.pressed==0);
 for(int i=0;i<35;i++)frame();
 assert(E.n==rows&&E.it[0].jd==held.jd&&!strcmp(E.it[0].title,held.title));
 down.type=IN_UP;assert(scr_orrery_events.input(&down));
 for(int i=0;i<200;i++)frame();
 int after;orrery_events(&after);assert(nos_top()==&scr_orrery_events);
 assert(orrery_events_progress()==100&&after>before&&E.n>rows&&E.generation==orrery_events_generation());
 /* Inserting a new earlier result retains the visible row and its pixel offset. */
 assert(E.n>8);E.list.sc.x=E.list.top+2*E.list.row_h+11;E.list.sc.sp.x=E.list.sc.sp.target=E.list.sc.x;
 held=E.it[2];float y=E.list.top+2*E.list.row_h-E.list.sc.x;
 ep_event *ev=(ep_event*)orrery_events(&after);assert(after>2);double oldjd=ev[after-1].jd;
 ev[after-1].jd=orrery_now()+.01;build();
 int located=-1;for(int i=0;i<E.n;i++)if(E.it[i].jd==held.jd&&!strcmp(E.it[i].title,held.title))located=i;
 assert(located>=0&&fabsf(E.list.top+located*E.list.row_h-E.list.sc.x-y)<.01);
 ev[after-1].jd=oldjd;
 printf("PASS Events scan completes while top (%d->%d events, %d->%d rows), held selection and anchored publication\n",before,after,rows,E.n);return 0;
}
'''),
"orrery_sd": ("nos/faces/orrery_sd.c", r'''
extern int sim_sd_present;
extern void hal_asset_sim_delay(int);
static unsigned char *slurp(const char *path,unsigned *n){
 FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long z=ftell(f);fseek(f,0,SEEK_SET);
 unsigned char *b=malloc((size_t)z);assert(b&&fread(b,1,(size_t)z,f)==(size_t)z);fclose(f);*n=(unsigned)z;return b;
}
static void put(const char *name,const void *d,unsigned n){
 char p[512];snprintf(p,sizeof p,"%s/sky/%s",hal_sd_root(),name);
 if(!d){remove(p);return;}
 FILE *f=fopen(p,"wb");assert(f&&fwrite(d,1,n,f)==n);fclose(f);
}
static void tick(unsigned ms){sim_vclock_ms+=ms;hal_frame_end();od_sd_pump();}
static void run(int n,unsigned ms){for(int i=0;i<n;i++)tick(ms);}
static void reset(int k){if(I[k].st==ST_READY||I[k].st==ST_CRC)hal_big_free(I[k].buf);memset(&I[k],0,sizeof I[k]);}
static void recrc(unsigned char *img,unsigned n){uint32_t c=~crc_add(~0u,img+HEAD,n-HEAD);memcpy(img+20,&c,4);}
/* one belt attempt on a doctored image: 1 published, 0 refused */
static int belt_try(const unsigned char *img,unsigned n){
 reset(K_BELT);put("belt.bin",img,n);od_belt_want();run(30,1);
 int ok=od_belt_rows()!=NULL;if(ok)od_belt_drop();return ok;
}
int main(int argc,char **argv) {
 assert(!sim_program_main(argc,argv));
 unsigned bn,mn;unsigned char *belt=slurp("assets/sd/sky/belt.bin",&bn),*moons=slurp("assets/sd/sky/moons.bin",&mn);
 assert(bn==want_bytes(K_BELT)&&mn==want_bytes(K_MOONS)&&bn==128016&&mn==40616);
 assert(belt_try(belt,bn));unsigned used0=nos_mem_used();   /* after one full cycle: the worker's own one-time buffers are out of the way */
 /* no file: nothing published, no error path, retry delay doubles */
 put("belt.bin",0,0);put("moons.bin",0,0);
 od_belt_want();run(4,10);assert(!od_belt_rows()&&I[K_BELT].st==ST_FAIL&&I[K_BELT].back==2000);
 run(1,1900);assert(I[K_BELT].st==ST_FAIL&&I[K_BELT].back==2000);
 run(1,200);run(1,1);assert(I[K_BELT].st==ST_FAIL&&I[K_BELT].back==4000);
 /* valid file, asynchronous worker: published only after header + CRC, rows identical to the file */
 put("belt.bin",belt,bn);hal_asset_sim_delay(3);run(1,5000);
 int ticks=0;while(!od_belt_rows()&&ticks++<80){tick(10);assert(!od_belt_rows()||I[K_BELT].st==ST_READY);}
 assert(od_belt_rows()&&ticks>4&&I[K_BELT].back==0);hal_asset_sim_delay(0);
 assert(!memcmp(od_belt_rows(),belt+HEAD,bn-HEAD));
 od_belt_drop();assert(!od_belt_rows()&&nos_mem_used()==used0);run(5,100);assert(!od_belt_rows()&&s_job<0);
 /* refused: bad CRC, other pack date, bad magic, wrong count, truncated */
 unsigned char *bad=malloc(bn);
 assert(belt_try(belt,bn));
 memcpy(bad,belt,bn);bad[HEAD+1000]^=1;assert(!belt_try(bad,bn)&&I[K_BELT].st==ST_FAIL&&I[K_BELT].back==60000);
 memcpy(bad,belt,bn);bad[24]^=1;assert(!belt_try(bad,bn)&&I[K_BELT].back==60000);
 memcpy(bad,belt,bn);bad[0]='X';assert(!belt_try(bad,bn));
 memcpy(bad,belt,bn);bad[8]--;assert(!belt_try(bad,bn));
 memcpy(bad,belt,bn);assert(!belt_try(bad,bn-1)&&I[K_BELT].st==ST_FAIL);
 assert(belt_try(belt,bn)&&nos_mem_used()==used0);
 /* moons: the open planet's block through the index; planets without moons need nothing */
 put("moons.bin",moons,mn);reset(K_MOONS);int n=-1;
 assert(od_moons_use(5)==0&&od_moons_use(0)==1&&od_moons_use(1)==1&&od_moons_known(5)==291&&od_moons_known(99)==0);
 for(int i=0;i<40&&!od_moons_ready();i++)tick(5);
 assert(od_moons_ready()&&od_moons_use(5)==1);
 const od_moon *m=od_moons_of(5,&n);uint32_t off;memcpy(&off,moons+HEAD+5*8,4);
 assert(n==291&&m&&!memcmp(m,moons+off,291u*88)&&m[0].planet==5&&m[n-1].planet==5&&m[0].name[0]);
 assert(od_moons_of(4,&n)&&n==115&&od_moons_of(10,&n)&&n==5&&od_moons_of(3,&n)&&n==2);
 assert(!od_moons_of(0,&n)&&n==0&&!od_moons_of(11,&n)&&!od_moons_of(-1,&n));
 od_moons_release();assert(!od_moons_ready()&&!od_moons_of(5,&n)&&nos_mem_used()==used0);
 /* an index that does not tile the records is refused even with a valid CRC */
 memcpy(bad,moons,mn);{uint32_t c=292;memcpy(bad+HEAD+5*8+4,&c,4);}recrc(bad,mn);
 put("moons.bin",bad,mn);reset(K_MOONS);od_moons_use(5);run(30,1);assert(!od_moons_ready()&&I[K_MOONS].st==ST_FAIL);
 od_moons_release();
 /* released while the job is in flight: its pages are freed when taken */
 put("moons.bin",moons,mn);od_moons_use(5);tick(1);assert(s_job==K_MOONS);od_moons_release();run(3,1);
 assert(s_job<0&&!od_moons_ready()&&nos_mem_used()==used0);
 /* no card (1.43C): no job at all, a quiet failure with a retry delay */
 sim_sd_present=0;od_moons_use(5);run(3,10);assert(!hal_asset_busy()&&s_job<0&&I[K_MOONS].st==ST_FAIL&&od_moons_use(5)==-1);
 od_moons_release();sim_sd_present=1;
 /* the one job slot is taken by someone else (the card's textures): wait, then go */
 asset_request rq={0};snprintf(rq.path,sizeof rq.path,"%s/sky/belt.bin",hal_sd_root());
 void *page=hal_big_alloc(1024);assert(page);rq.pages[0]=page;rq.page_bytes=rq.bytes=1024;rq.page_count=1;rq.tag=777;
 hal_asset_sim_delay(3);assert(hal_asset_submit(&rq)==1&&hal_asset_busy());
 reset(K_BELT);od_belt_want();run(3,10);assert(s_job<0&&I[K_BELT].st==ST_IDLE);
 asset_result res;asset_request got;for(int i=0;i<20&&!hal_asset_take(&got,&res);i++)hal_asset_poll();
 assert(got.tag==777&&res.code==ASSET_OK);hal_big_free(got.pages[0]);hal_asset_sim_delay(0);
 run(30,1);assert(od_belt_rows());od_belt_drop();
 assert(nos_mem_used()==used0);
 puts("PASS orrery_sd: belt.bin/moons.bin published only after header, pack date, counts, index and CRC32; missing file, no card, bad files, busy slot, release in flight");return 0;
}
'''),
"orrery_snap": ("nos/faces/face_orrery.c", r"""
static uint16_t live[GFX_W*GFX_H],snap[GFX_W*GFX_H];
static void render(uint16_t *px,int ox){
 for(int i=0;i<GFX_W*GFX_H;i++)px[i]=C_BG;
 gfx_fb f={0};f.px=px;f.h=GFX_H;f.cx1=GFX_W;f.ox=ox;face_ctx c={0};c.dt.hour=13;c.dt.minute=37;
 face_orrery.draw(&f,&c);
}
int main(int argc,char **argv) {
 assert(!sim_program_main(argc,argv));assert(orr_ok());
 face_ctx c={0};c.dt.hour=13;c.dt.minute=37;c.enter=1;
 for(int i=0;i<400&&!(S.bk&&S.scene_valid&&!S.belt_left&&!S.small_left&&i>40);i++){
  sim_vclock_ms+=40;hal_frame_end();face_orrery.tick(&c,.04f);c.enter=0;}
 assert(S.bk&&S.scene_valid&&S.clock_overlay&&!S.belt_left&&!S.small_left);
 /* the sky key ignores the minute: the clock stays live over the picture */
 unsigned ink=S.scene_ink;c.dt.minute=38;face_orrery.tick(&c,.04f);assert(S.scene_ink==ink);
 c.dt.minute=37;face_orrery.tick(&c,.04f);
 if(!orr_snap){orr_snap=hal_big_alloc(GFX_W*GFX_H*2);}
 assert(orr_snap);
 orr_snap_bake_key=S.scene_ink;orr_snap_row=0;orr_snap_valid=0;
 int slices=0;while(orr_snap_row>=0){orr_snap_bake(0);slices++;}
 assert(orr_snap_ready()&&slices==(GFX_H+15)/16);
 const int offsets[]={0,-37,-233,151};
 for(unsigned k=0;k<sizeof offsets/sizeof *offsets;k++){
  orr_snap_valid=0;render(live,offsets[k]);
  orr_snap_valid=1;render(snap,offsets[k]);
  assert(!memcmp(live,snap,sizeof live));
 }
 /* dimmed backdrops replay; a moved sky key retires the picture */
 gfx_fb d={0};d.px=snap;d.h=GFX_H;d.cx1=GFX_W;d.dim=13;assert(!orr_snap_draw(&d));
 S.scene_ink^=1;assert(!orr_snap_ready());S.scene_ink^=1;assert(orr_snap_ready());
 orr_snap_free();assert(!orr_snap&&!orr_snap_ready());
 puts("PASS orrery_snap: resting picture == live replay at rest and translated by the pager; clock live; dim and stale key fall back");return 0;
}
"""),
"belt": ("nos/faces/face_orrery.c", r'''
static unsigned char *slurp(const char *path,unsigned *n){
 FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long z=ftell(f);fseek(f,0,SEEK_SET);
 unsigned char *b=malloc((size_t)z);assert(b&&fread(b,1,(size_t)z,f)==(size_t)z);fclose(f);*n=(unsigned)z;return b;
}
static void btick(void){sim_vclock_ms+=40;hal_frame_end();}
/* the expressions belt_init carried inline before the table moved to SD (and before it was baked): the reference */
static void reference_basis(ast_k *a,const od_ast *o){
 float cw = fk_cos(o->w * D2RF), sw = fk_sin(o->w * D2RF), cO = fk_cos(o->node * D2RF), sO = fk_sin(o->node * D2RF);
 float cI = fk_cos(o->i * D2RF), sI = fk_sin(o->i * D2RF);
 a->Px = cw * cO - sw * sO * cI; a->Py = cw * sO + sw * cO * cI; a->Pz = sw * sI;
 a->Qx = -sw * cO - cw * sO * cI; a->Qy = -sw * sO + cw * cO * cI; a->Qz = cw * sI;
 a->a = o->a; a->e = o->e; a->b = o->a * gfx_fast_sqrt(1 - o->e * o->e);
 float root = gfx_fast_sqrt(o->a); root = 0.5f * (root + o->a / root);
 a->n = 0.9856076686f / (o->a * root); a->M0 = o->ma; a->ep = o->ep;
 a->refresh=(1-a->e)*(1-a->e)/a->n;
}
static void release_belt(void){
 hal_big_free((void*)S.bk);hal_big_free(S.bp);hal_big_free(S.bxs);hal_big_free(S.bh);hal_big_free(S.belt_queue);
 S.bk=0;S.bp=0;S.bxs=S.bys=0;S.bh=0;S.belt_queue=0;S.nbelt=0;S.jd_belt=0;
}
static void load_rows(void){od_belt_want();for(int i=0;i<80&&!od_belt_rows();i++){btick();od_sd_pump();}assert(od_belt_rows());}
int main(int argc,char **argv) {
 assert(!sim_program_main(argc,argv));assert(orr_ok());
 unsigned bn;unsigned char *img=slurp("assets/sd/sky/belt.bin",&bn);const od_ast *rows=(const od_ast*)(img+48);
 load_rows();od_belt_drop();unsigned used0=nos_mem_used();   /* after one full load: the worker's one-time buffers are out of the way */
 /* always-on display: the card is never asked */
 for(int i=0;i<40;i++){btick();belt_init(1);}
 assert(!S.bk&&!od_belt_rows()&&!hal_asset_busy());
 /* loaded, derived over several ticks, published whole */
 int ticks=0;while(!S.bk&&ticks<80){btick();belt_init(0);ticks++;if(!S.bk)assert(!S.nbelt&&!S.bp&&!S.bh&&!S.bxs&&!S.belt_queue);}
 assert(S.bk&&S.nbelt==od_belt_n&&od_belt_n==3999&&ticks>=2+(od_belt_n+BELT_SLICE-1)/BELT_SLICE);
 assert(!od_belt_rows());
 ast_k want;int queue_ok=1;
 for(int k=0;k<od_belt_n;k++){
  reference_basis(&want,&rows[k]);assert(!memcmp(&S.bk[k],&want,sizeof want));
  assert(S.bh[k]==(rows[k].h10<80?0:rows[k].h10<100?1:2)&&isnan(S.bxs[k])&&S.bys[k]==0);
  if(k&&S.belt_queue[(k-1)/2].due>S.belt_queue[k].due)queue_ok=0;
 }
 assert(queue_ok);
 /* positions at both ends of the model's range come from that basis */
 const double dates[]={EP_JD_MIN+1,2415020,2451545,2461334.5,EP_JD_MAX-1};
 for(unsigned d=0;d<5;d++)for(int k=0;k<od_belt_n;k+=37){pp p=belt_exact(k,dates[d]);assert(isfinite(p.au)&&p.au>0);}
 release_belt();
 /* denied allocations leave nothing behind and the next tick tries again */
 load_rows();
 for(int fail=0;fail<4;fail++){
  unsigned used=nos_mem_used();nos_mem_fail_after(fail);belt_init(0);nos_mem_fail_after(-1);
  assert(!S.bk&&!BI.k&&!BI.bp&&!BI.f&&!BI.bh&&!BI.q&&nos_mem_used()==used&&od_belt_rows());
 }
 nos_mem_fail_after(4);belt_init(0);nos_mem_fail_after(-1);
 while(!S.bk){btick();belt_init(0);}
 assert(!S.belt_queue&&S.nbelt==od_belt_n&&!od_belt_rows());
 for(int k=0;k<od_belt_n;k+=101){reference_basis(&want,&rows[k]);assert(!memcmp(&S.bk[k],&want,sizeof want));}
 release_belt();assert(nos_mem_used()==used0);
 puts("PASS belt: AOD never loads; basis from SD rows bit-identical to the original formula, published whole after sliced ticks; allocation denial and queue loss");return 0;
}
'''),
"moons": ("nos/faces/orrery_card.c", r'''
extern int sim_sd_present;
static unsigned char *slurp(const char *path,unsigned *n){
 FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long z=ftell(f);fseek(f,0,SEEK_SET);
 unsigned char *b=malloc((size_t)z);assert(b&&fread(b,1,(size_t)z,f)==(size_t)z);fclose(f);*n=(unsigned)z;return b;
}
static void put(const char *name,const void *d,unsigned n){
 char p[512];snprintf(p,sizeof p,"%s/sky/%s",hal_sd_root(),name);
 if(!d){remove(p);return;}
 FILE *f=fopen(p,"wb");assert(f&&fwrite(d,1,n,f)==n);fclose(f);
}
static void frames(int n,unsigned ms){for(int i=0;i<n;i++){sim_vclock_ms+=ms;hal_frame_end();tick(ms/1000.f);orrery_card_asset_poll();}}
int main(int argc,char **argv) {
 assert(!sim_program_main(argc,argv));release_pending=0;
 unsigned mn;unsigned char *moons=slurp("assets/sd/sky/moons.bin",&mn);
 put("moons.bin",0,0);assert(allocate());body_reset(OB_SATURN);
 /* no moons.bin: the card still prepares; the moons page says so instead of "0 KNOWN" */
 frames(60,25);assert(C.prepared&&C.b->ob==OB_SATURN);
 for(int i=0;i<400&&C.b->moons_state!=2;i++)frames(1,25);   /* first read as loading, then the failure refreshes the open card */
 assert(C.b->moons_state==2&&strstr(C.b->count,"MOON DATA MISSING")&&!strstr(C.b->count,"KNOWN")&&!od_moons_ready());
 static body_state mercury,earth,saturn;
 prepare(&mercury,OB_MERCURY,card_jd());assert(mercury.moons_state==0&&!strncmp(mercury.count,"0 KNOWN",7));
 prepare(&earth,OB_EARTH,card_jd());assert(earth.moons_state==0&&!strncmp(earth.count,"1 KNOWN",7)&&earth.n>=1&&!strcmp(earth.items[0].name,"MOON"));
 prepare(&saturn,OB_SATURN,card_jd());assert(saturn.moons_state==2);
 /* the file appears (card inserted, sdsync): the retry finds it and the open card refreshes in place */
 put("moons.bin",moons,mn);
 for(int i=0;i<400&&C.b->moons_state;i++)frames(1,100);
 assert(!C.b->moons_state&&!strncmp(C.b->count,"291 KNOWN",9)&&C.b->n>5&&od_moons_ready());
 prepare(&saturn,OB_SATURN,card_jd());assert(!saturn.moons_state&&saturn.n==C.b->n&&!strncmp(saturn.count,"291 KNOWN",9));
 /* closing the card frees the catalog */
 unsigned used=nos_mem_used();release_buffers();assert(!od_moons_ready()&&nos_mem_used()<used);
 puts("PASS moons: missing data reads MOON DATA MISSING (never 0 KNOWN), Earth/Mercury need no SD, a late file refreshes the open card, release frees it");return 0;
}
'''),
"card": ("nos/faces/orrery_card.c", r'''
static void pump(unsigned ms){
 for(unsigned i=0;i<ms;i+=25){hal_frame_end();sim_vclock_ms+=25;load_tick();orrery_card_asset_poll();}
}
static void write_asset(const char *name,unsigned size,unsigned char value){
 char path[512];snprintf(path,sizeof path,"%s/sky/%s",hal_sd_root(),name);FILE *f=fopen(path,"wb");assert(f);
 unsigned char bytes[4096];memset(bytes,value,sizeof bytes);
 while(size){unsigned n=size>sizeof bytes?sizeof bytes:size;assert(fwrite(bytes,1,n,f)==n);size-=n;}assert(!fclose(f));
}
int main(int argc,char **argv) {
 assert(!sim_program_main(argc,argv));release_pending=0;assert(allocate());body_reset(OB_SUN);pump(2500);
 assert(C.load_stage==3&&!C.tex_ok&&(C.load_failed&1u)&&!asset_pending_generation);
 unsigned failures=asset_status.failed;pump(1000);assert(asset_status.failed==failures);
 write_asset("sun.565",TEX_BYTES,0x11);pump(6500);assert(C.tex_ok&&!C.load_failed&&C.load_stage==3);
 for(unsigned p=0;p<TEX_PAGES;p++)assert(C.texture[p]&&C.texture[p][0]==0x1111);
 write_asset("earth.565",TEX_BYTES,0x22);body_reset(OB_EARTH);pump(2500);
 assert(C.tex_ok&&!C.night_ok&&(C.load_failed&2u));
 write_asset("earth_night.565",TEX_BYTES,0x33);pump(6500);
 assert(C.tex_ok&&C.night_ok&&!C.load_failed&&C.night[0][0]==0x3333);
 write_asset("saturn.565",TEX_BYTES,0x44);body_reset(OB_SATURN);pump(2500);
 assert(C.tex_ok&&!C.ring_ok&&(C.load_failed&4u));
 write_asset("saturn_ring.rgba",2048,0x55);pump(6500);assert(C.ring_ok&&!C.load_failed&&C.ring_tex[0]==0x55);
 release_buffers();orrery_card_asset_poll();assert(!asset_pending_generation);
 puts("PASS card missing PHOTO/night/rings recover in place on bounded retry; success pages and release ownership preserved");return 0;
}
'''),
}

def sky_bins(d):
    for n in ("belt.bin", "moons.bin"): shutil.copy(ROOT/"assets/sd/sky"/n, d/n)

@unittest.skipUnless(shutil.which("gcc"), "gcc required")
class FaceContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        # Public structs include timestamp types. Borrowing old simulator
        # objects silently mixed ABIs after changes to ui_list or face_ctx.
        from tools.build_sim import CORE
        sources=sorted({p.relative_to(ROOT).as_posix() for p in [
            *list((ROOT/"nos").rglob("*.c")), *list((ROOT/"sim").glob("nos_*.c")),
            *list((ROOT/"core/fonts").glob("*.c")),
            *[ROOT/"core"/(name+".c") for name in CORE]]})
        def compile_current(src):
            obj = OUT / (src.replace("/", "_").replace(".c", ".o"))
            extra = ["-Dmain=sim_program_main"] if src == "sim/nos_sim.c" else []
            cls.command("compile-"+obj.stem, ["gcc", *FLAGS, *extra, "-c", src, "-o", str(obj)])
            return src,obj
        with ThreadPoolExecutor(max_workers=4) as pool:
            cls.fresh=dict(pool.map(compile_current,sources))
        cls.base=[]

    @classmethod
    def command(cls, name, cmd, env=None):
        result = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True, timeout=120)
        (OUT/(name+".log")).write_text(result.stdout+result.stderr, encoding="utf-8")
        if result.returncode:
            raise AssertionError(name+" (see private log)\n"+(result.stdout+result.stderr)[-6000:])
        return result

    def case(self, name, fps=40):
        included, body = CASES[name]
        source = OUT/(name+str(fps)+".c")
        source.write_text(PREFIX+'\n#include "'+(ROOT/included).as_posix()+'"\n'+body)
        binary = source.with_suffix(".exe")
        objects = [p for src,p in self.fresh.items() if src != included]+self.base
        self.command("compile-"+source.stem, ["gcc", *FLAGS, "-DNOS_FPS="+str(fps), str(source),
                     *map(str,objects), "-o", str(binary), "-lgdi32", "-luser32", "-lwinmm", "-lm"])
        script=OUT/"boot.txt";script.write_text("skipboot\nface 0\nhome_now\nwait 25\n")
        sd=Path(tempfile.mkdtemp(prefix=source.stem+"-sd-",dir=OUT));(sd/"sky").mkdir();sky_bins(sd/"sky")
        result=self.command(source.stem, [str(binary), "--script", str(script), "--out",
                            str(OUT/(source.stem+"-frames")), "--no-media", "--quiet"],
                            os.environ|{"NOS_SD_ROOT":str(sd)})
        self.assertIn("PASS", result.stdout)
        print(result.stdout[-1600:])

    def test_matrix(self): self.case("matrix")
    def test_split(self): self.case("split")
    def test_segment(self): self.case("segment")
    def test_orrery(self): self.case("orrery")
    def test_orrery_snap(self): self.case("orrery_snap")
    def test_orrery_sd(self): self.case("orrery_sd")
    def test_belt(self): self.case("belt")
    def test_moons(self): self.case("moons")
    def test_events(self): self.case("events")
    def test_card(self): self.case("card")
    def test_cadence40(self): self.case("cadence",40)
    def test_cadence30(self): self.case("cadence",30)

    def existing(self, name, source, excluded, script_text="skipboot\nface 0\nhome_now\nwait 25\n"):
        exe=OUT/(name+".exe")
        objects=[obj for src,obj in self.fresh.items() if src not in excluded]+self.base
        self.command("compile-"+name,["gcc",*FLAGS,str(source),*map(str,objects),"-o",str(exe),
                                     "-lgdi32","-luser32","-lwinmm","-lm"])
        script=OUT/(name+".txt");script.write_text(script_text)
        sd=Path(tempfile.mkdtemp(prefix=name+"-sd-",dir=OUT));sky=sd/"sky";sky.mkdir();sky_bins(sky)
        if name=="existing-card-assets":
            for asset,value,size in [("earth.565",0x11,1048576),("earth_night.565",0x22,1048576),
                                     ("mars.565",0x33,1048576),("saturn.565",0x44,1048576),
                                     ("saturn_ring.rgba",0x77,2048)]:
                (sky/asset).write_bytes(bytes([value])*size)
        result=self.command(name,[str(exe),"--script",str(script),"--out",str(OUT/(name+"-frames")),
                                  "--no-media","--quiet"],os.environ|{"NOS_SD_ROOT":str(sd)})
        self.assertIn("PASS",result.stdout)
        print(result.stdout[-1600:])

    def test_existing_gestures(self):
        self.existing("existing-gestures",ROOT/"tests/orrery_edge_swipe_test.c",{"nos/faces/face_orrery.c"},
                      "skipboot\nface 8\nhome_now\nwait 500\n")

    def test_existing_card_assets(self):
        self.existing("existing-card-assets",ROOT/"tests/card_asset_test.c",{"nos/faces/orrery_card.c"})

    def test_existing_card_prepare(self):
        source=OUT/"existing-card-prepare.c"
        includes=["nos/faces/orrery_card.c","nos/faces/orrery_card_prepare.inc",
                  "tests/card_prepare_reference.h","tests/card_prepare_incremental_test.inc"]
        source.write_text('#include <assert.h>\n'+''.join('#include "'+(ROOT/p).as_posix()+'"\n' for p in includes)+
                          'extern int sim_program_main(int,char**);\nint main(int argc,char**argv){'
                          'assert(!sim_program_main(argc,argv));card_prepare_incremental_tests();return 0;}\n')
        self.existing("existing-card-prepare",source,{"nos/faces/orrery_card.c"},
                      "skipboot\nface 8\nhome_now\nwait 3000\n")

    def test_ringplate_rejected(self):
        source=OUT/"ringplate-removed.c"
        source.write_text('extern int sim_program_main(int,char**);\n'
                          'int main(int argc,char**argv){return sim_program_main(argc,argv);}\n')
        exe=source.with_suffix(".exe")
        self.command("compile-ringplate",["gcc",*FLAGS,str(source),*map(str,list(self.fresh.values())+self.base),
                                          "-o",str(exe),"-lgdi32","-luser32","-lwinmm","-lm"])
        for variant in ("A","B"):
            script=OUT/("ringplate-"+variant+".txt");script.write_text("ringplate "+variant+"\n")
            sd=Path(tempfile.mkdtemp(prefix="ringplate-sd-",dir=OUT))
            result=subprocess.run([str(exe),"--script",str(script),"--out",str(OUT/("ringplate-"+variant)),
                                   "--no-media","--quiet"],cwd=ROOT,capture_output=True,text=True,
                                  env=os.environ|{"NOS_SD_ROOT":str(sd)},timeout=30)
            (OUT/("ringplate-"+variant+".log")).write_text(result.stdout+result.stderr)
            self.assertNotEqual(result.returncode,0)
            self.assertIn("ringplate A/B removed: final dock only",result.stdout)

if __name__ == "__main__": unittest.main()
