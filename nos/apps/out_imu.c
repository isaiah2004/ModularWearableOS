#if defined(__MINGW32__)
#define __USE_MINGW_ANSI_STDIO 1
#endif
/* IMU: live motion, recording, and an asynchronous SD browser. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#define HIST 64
typedef struct {
    int page, rate, valid, confirm, review, pos, filled, bx, by;
    uint32_t last, hashes[2];
    uint32_t stamps[HIST], span_ms;
    int16_t trace[6][HIST];
    hal_imu_record rec;
    hal_imu_file file;
    char values[2][48], magnitude[16], elapsed[20], counts[48], size[40], loss[48];
    char duration[20], file_size[40];
    char axis[40];
} imu_ui;
static imu_ui *s;
static int busy(void) { return s->rec.state>=HAL_IMU_STARTING && s->rec.state<=HAL_IMU_STOPPING; }
static int clamp(int x,int a,int b) { return x<a?a:x>b?b:x; }
static void time_text(char *out, size_t n, uint32_t ms)
{ unsigned t=ms/1000; snprintf(out,n,"%02u:%02u",t/60,t%60); }
static void refresh(void)
{
    int was=busy();
    hal_imu_record_get(&s->rec); hal_imu_file_get(&s->file);
    if (was && !busy() && s->rec.name[0]) s->review=1;
    time_text(s->elapsed,sizeof s->elapsed,s->rec.elapsed_ms);
    snprintf(s->counts,sizeof s->counts,"%u HZ   %u SAMPLES",s->rec.hz,(unsigned)s->rec.samples);
    snprintf(s->size,sizeof s->size,"%llu B WRITTEN",(unsigned long long)s->rec.bytes);
    snprintf(s->loss,sizeof s->loss,"DROP %u / MISS %u / ERR %u",(unsigned)s->rec.dropped,(unsigned)s->rec.missed,(unsigned)s->rec.errors);
    time_text(s->duration,sizeof s->duration,s->file.duration_ms);
    snprintf(s->file_size,sizeof s->file_size,"%llu BYTES",(unsigned long long)s->file.bytes);
}
static void enter(void)
{
    if (!s) { s=nos_mem_alloc(sizeof *s,"imu.ui"); if (!s) return; memset(s,0,sizeof *s); s->rate=1; }
    s->confirm=0; refresh();
    if (s->page==2 && !busy()) hal_imu_files_request(0,0);
}
static void tick(float dt)
{
    (void)dt; if (!s) return;
    refresh();
    if (s->page || (uint32_t)(hal_millis()-s->last)<50) return;
    s->last=hal_millis();
    hal_imu v; s->valid=hal_imu_read(&v);
    if (!s->valid) return;
    float a=sqrtf(v.ax*v.ax+v.ay*v.ay+v.az*v.az);
    if(!isfinite(a)||!isfinite(v.gx)||!isfinite(v.gy)||!isfinite(v.gz)) {s->valid=0;return;}
    snprintf(s->magnitude,sizeof s->magnitude,"%.2f",(double)a);
    snprintf(s->values[0],sizeof s->values[0],"X %+.2f  Y %+.2f  Z %+.2f",(double)v.ax,(double)v.ay,(double)v.az);
    snprintf(s->values[1],sizeof s->values[1],"X %+.0f  Y %+.0f  Z %+.0f",(double)v.gx,(double)v.gy,(double)v.gz);
    s->bx=clamp((int)lroundf(v.ax*24),-24,24);
    s->by=clamp((int)lroundf(-v.ay*24),-24,24);
    float vv[6]={v.ax,v.ay,v.az,v.gx,v.gy,v.gz};
    for (int c=0;c<6;c++) s->trace[c][s->pos]=(int16_t)clamp((int)lroundf(vv[c]*(c<3?12:0.2f)),-24,24);
    s->stamps[s->pos]=s->last;
    s->pos=(s->pos+1)%HIST; if (s->filled<HIST) s->filled++;
    int oldest=(s->pos-s->filled+HIST)%HIST;
    s->span_ms=s->last-s->stamps[oldest];
    snprintf(s->axis,sizeof s->axis,"ACCEL +/-2G / %.1fS",s->span_ms/1000.0);
    /* Hash chronological pixels, not the ring cursor: at rest the graph settles
     * and sends zero pixels, even though acquisition continues. */
    for (int g=0;g<2;g++) {
        unsigned h=2166136261u;
        for (int c=0;c<3;c++) for (int i=0;i<s->filled;i++) {
            int k=(oldest+i)%HIST;
            h=gfx_hash_u(h,(unsigned)s->trace[g*3+c][k]);
            h=gfx_hash_u(h,s->span_ms?(s->stamps[k]-s->stamps[oldest])*318u/s->span_ms:0);
        }
        s->hashes[g]=h;
    }
}
static void label(gfx_fb *fb,int y,const char *t,uint16_t ink)
{ ui_label(fb,233,y,t,ink,UI_CENTER); }
static void button(gfx_fb *fb,int x,int y,int width,const char *text,int on)
{
    ui_rrect(fb,x-width/2,y-30,width,60,30,on?C_PRI:C_SURF2);
    ui_label(fb,x,y+6,text,on?C_BG:C_PRI,UI_CENTER);
}
static void hero(gfx_fb *fb,int x,int y,const char *text,float pitch)
{
    dot_style d={DOT_FONT_TALL,pitch,pitch*0.8f,C_WHITE,0,1,1};
    dot_text_a(fb,&d,x,y,text,1);
}
static void trace(gfx_fb *fb,int group,int cy)
{
    if (fb->rec) { gfx_rec_cmd(fb,72,cy-26,394,cy+27,s->hashes[group]); return; }
    int top=fb->y_base-fb->oy, bottom=top+fb->h;
    if (bottom<cy-26 || top>cy+26) return;
    gfx_fill_rect(fb,74,cy,318,1,C_LINE);
    const uint16_t inks[3]={C_PRI,C_SEC,C_DIS};
    int oldest=(s->pos-s->filled+HIST)%HIST;
    for (int c=2;c>=0;c--) for (int i=1;i<s->filled && s->span_ms;i++) {
        int previous=(oldest+i-1)%HIST,next=(oldest+i)%HIST;
        if(s->stamps[next]-s->stamps[previous]>250u)continue;
        int y0=cy-s->trace[group*3+c][previous];
        int y1=cy-s->trace[group*3+c][next];
        if ((y0<top-2 && y1<top-2)||(y0>bottom+2 && y1>bottom+2)) continue;
        float x0=74.0f+(s->stamps[previous]-s->stamps[oldest])*318.0f/s->span_ms;
        float x1=74.0f+(s->stamps[next]-s->stamps[oldest])*318.0f/s->span_ms;
        gfx_line(fb,x0,y0,x1,y1,1.2f,inks[c]);
    }
}
static void draw(gfx_fb *fb)
{
    ui_title(fb,"IMU",0);
    if (!s) { ui_state(fb,233,233,"NO PSRAM",C_SEC); return; }
    if (busy()) { dot_disc(fb,191,85,7,C_RED); ui_label(fb,207,91,"REC",C_PRI,UI_LEFT); }
    if (s->page==0) {
        if (!s->valid) ui_state(fb,233,230,"IMU UNAVAILABLE",C_SEC);
        else {
            hero(fb,180,110,s->magnitude,6);
            ui_label(fb,180,181,"|A| G",C_SEC,UI_CENTER);
            gfx_ring(fb,337,141,32,1,C_LINE2);
            gfx_fill_rect(fb,309,141,56,1,C_LINE2); gfx_fill_rect(fb,337,113,1,56,C_LINE2);
            gfx_fill_circle(fb,337+s->bx,141+s->by,5,C_PRI);
            ui_label(fb,337,187,"TILT",C_DIS,UI_CENTER);
            label(fb,213,s->values[0],C_PRI);
            trace(fb,0,247);
            label(fb,282,s->axis,C_DIS);
            label(fb,305,s->values[1],C_PRI);
            trace(fb,1,339);
            /* scale key shares the three trace shades */
            ui_label(fb,233,380,"GYRO +/-120 DPS",C_DIS,UI_CENTER);
        }
    } else if (s->page==1) {
        hero(fb,233,112,s->elapsed,7);
        if (busy() || s->review) {
            label(fb,215,s->counts,C_PRI); label(fb,243,s->size,C_SEC); label(fb,271,s->loss,C_SEC);
            label(fb,297,s->rec.state==HAL_IMU_ERROR?s->rec.message:s->rec.name,C_PRI);
        } else {
            const char *rates[]={"25 HZ","50 HZ","100 HZ"};
            for (int i=0;i<3;i++) button(fb,127+i*106,218,96,rates[i],s->rate==i);
            label(fb,272,s->rec.message[0]?s->rec.message:"FIXED-RATE / RAW IMU",C_SEC);
            label(fb,299,s->rec.name[0]?s->rec.name:"KEEPS RECORDING ASLEEP",C_DIS);
        }
        button(fb,233,342,224,s->rec.state==HAL_IMU_STARTING?"STARTING":s->rec.state==HAL_IMU_STOPPING?"SAVING":busy()?"STOP + SAVE":s->review?"NEW RECORDING":"START RECORDING",!busy());
    } else {
        if (busy()) { label(fb,215,"RECORDING IN PROGRESS",C_PRI); label(fb,247,"STOP TO BROWSE FILES",C_SEC); }
        else if (s->file.busy) ui_state(fb,233,230,"READING CARD",C_SEC);
        else if (!s->file.valid) ui_state(fb,233,230,s->file.message,C_SEC);
        else {
            label(fb,143,s->file.name,C_PRI); hero(fb,233,170,s->duration,7);
            label(fb,261,s->file_size,C_SEC);
            label(fb,287,s->confirm?"TAP AGAIN TO DELETE":s->file.message,C_DIS);
            button(fb,111,338,88,"<",0); button(fb,355,338,88,">",0);
            button(fb,233,338,144,s->confirm?"CONFIRM":"DELETE",0);
        }
    }
    const char *tabs[]={"LIVE","RECORD","FILES"};
    for (int i=0;i<3;i++) {
        int x=127+i*106;
        ui_label(fb,x,420,tabs[i],s->page==i?C_PRI:C_DIS,UI_CENTER);
        if (s->page==i) gfx_fill_rect(fb,x-16,433,32,2,C_PRI);
    }
}
static int input(const nos_input *in)
{
    if (!s) return 0;
    if (in->type==IN_CANCEL) { s->confirm=0; return 1; }
    if (in->type!=IN_TAP) return 0;
    if (in->y>=378 && in->y<466 && in->x>=74 && in->x<392) {
        int page=(in->x-74)/106;
        s->page=page; s->confirm=0;
        if (page==2 && !busy()) hal_imu_files_request(0,0);
        nos_feedback(FB_TICK); return 1;
    }
    int ok=0;
    if (s->page==1) {
        if (in->y>=174 && in->y<262 && !busy() && !s->review && in->x>=74 && in->x<392) {
            s->rate=(in->x-74)/106; nos_feedback(FB_TICK); return 1;
        }
        if (in->y>=290 && in->y<378 && in->x>=105 && in->x<361) {
            if (s->review && !busy()) { s->review=0; nos_feedback(FB_TAP); return 1; }
            const unsigned hz[]={25,50,100};
            ok=busy()?hal_imu_record_stop():hal_imu_record_start(hz[s->rate]);
        } else return 0;
    } else if (s->page==2 && s->file.valid && !busy() && !s->file.busy && in->y>=290 && in->y<378) {
        if (in->x<155) { s->confirm=0; ok=hal_imu_files_request(s->file.id,-1); }
        else if (in->x>311) { s->confirm=0; ok=hal_imu_files_request(s->file.id,1); }
        else if (!s->confirm) { s->confirm=1; ok=1; }
        else { s->confirm=0; ok=hal_imu_file_delete(s->file.id); }
    } else return 0;
    if (!ok) nos_toast("IMU BUSY / UNAVAILABLE");
    nos_feedback(ok?FB_TAP:FB_ERROR); refresh(); return 1;
}
const nos_screen scr_imu={"IMU",enter,NULL,tick,draw,input,0};

