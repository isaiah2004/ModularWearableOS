/* Background jobs: passive status, acquisition history and state transitions.
 * All snapshots/layout are prepared in tick; draw never polls a producer. */
#include "apps/nos_apps.h"
#include "background.h"
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define ROWS 4
static struct {
    int page,job,tab,metric,offset,selected,frozen,ready;
    int states[BG_COUNT];
    background_status latest[BG_COUNT];
    uint32_t refreshed,hash;
    float low,high;
    int16_t x[BG_HISTORY],y[BG_HISTORY];
    background_snapshot snapshot;
} *s;
static const nos_screen scr_background_detail;
static uint16_t ink(int st) {return st==BG_ERROR||st==BG_STALE?C_RED:st==BG_RUNNING?C_PRI:C_SEC;}
static void text(gfx_fb *fb,int y,const char *value,uint16_t color)
{int a,b;ui_row_span((float)y,18,&a,&b);ui_text_fit(fb,UI_F_SMALL,UI_CX,y,value,color,UI_CENTER,b-a);}
static void value_text(char *out,size_t n,float v,int valid,const char *unit)
{
    if(!valid || !isfinite(v))snprintf(out,n,"-- %s",unit);
    else if(fabsf(v)>=10000)snprintf(out,n,"%.0f %s",(double)v,unit);
    else if(fabsf(v)>=100)snprintf(out,n,"%.0f %s",(double)v,unit);
    else snprintf(out,n,"%.2f %s",(double)v,unit);
}
static void uptime(char *out,size_t n,uint32_t ms)
{unsigned sec=ms/1000;snprintf(out,n,"%02u:%02u:%02u",sec/3600,sec/60%60,sec%60);}
static void prepare(void)
{
    background_snapshot *v=&s->snapshot;int metric=s->metric;
    float low=INFINITY,high=-INFINITY;
    for(int i=0;i<v->count;i++)if(v->points[i].valid&(1<<metric)) {
        float a=v->points[i].v[metric];if(a<low)low=a;if(a>high)high=a;
    }
    if(!isfinite(low)) {low=0;high=1;}
    s->low=low;s->high=high;
    float margin=(high-low)*.1f;if(margin<.05f)margin=.05f;
    low-=margin;high+=margin;
    unsigned hash=2166136261u;
    for(int i=0;i<v->count;i++) {
        background_point *p=&v->points[i];
        int32_t age_ms=(int32_t)(v->now-p->ms);if(age_ms<0)age_ms=0;
        float age=(float)age_ms/BG_WINDOW_MS;
        s->x[i]=(int16_t)lroundf(390-age*314);
        float y=(p->valid&(1<<metric))?299-(p->v[metric]-low)/(high-low)*72:263;
        if(y<222)y=222;
        if(y>301)y=301;
        s->y[i]=(int16_t)lroundf(y);
        if(p->valid&(1<<metric)) {
            hash=gfx_hash_u(hash,(unsigned)(uint16_t)s->x[i]);
            hash=gfx_hash_u(hash,(unsigned)(uint16_t)s->y[i]);
            hash=gfx_hash_u(hash,i && (v->points[i-1].valid&(1<<metric)) && p->ms-v->points[i-1].ms<=2500u);
        }
    }
    s->hash=gfx_hash_u(hash,(unsigned)s->selected);
}
static void refresh(int force)
{
    if(!s)return;
    uint32_t now=hal_millis();
    if(!force && now-s->refreshed<1000u)return;
    s->refreshed=now;s->ready=background_summary(s->states);
    if(s->ready)for(int id=0;id<BG_COUNT;id++)background_status_get(id,&s->latest[id]);
    if(!s->frozen && s->job>=0) {
        s->ready=background_get(s->job,&s->snapshot);
        prepare();
    }
    /* The retained recorder detects changed text/graph pixels. Invalidating
     * the whole screen here would repaint every static control once a second. */
}
static void enter(void)
{
    if(!s) {s=nos_mem_alloc(sizeof *s,"background.ui");if(!s)return;memset(s,0,sizeof *s);s->job=-1;s->selected=-1;}
    refresh(1);
}
static void tick(float dt) {(void)dt;refresh(0);}
static void overview(gfx_fb *fb)
{
    ui_title(fb,"BACKGROUND JOBS",0);
    if(!s || !s->ready) {ui_state(fb,UI_CX,233,"MONITOR UNAVAILABLE",C_SEC);return;}
    int running=0,attention=0;
    for(int i=0;i<BG_COUNT;i++){running+=s->states[i]==BG_RUNNING;attention+=s->states[i]==BG_ERROR||s->states[i]==BG_STALE;}
    char b[64];snprintf(b,sizeof b,"%d RUNNING / %d NEED ATTENTION",running,attention);text(fb,105,b,C_SEC);
    for(int row=0;row<ROWS;row++) {
        int id=s->page*ROWS+row;if(id>=BG_COUNT)break;
        int y=145+row*61;
        const background_info *info=background_job(id);
        dot_disc(fb,81,y+3,4,ink(s->states[id]));
        ui_text_fit(fb,UI_F_BODY,99,y,info->name,C_PRI,UI_LEFT,285);
        ui_text(fb,UI_F_SMALL,99,y+23,background_state_name(s->states[id]),ink(s->states[id]),UI_LEFT,0);
        value_text(b,sizeof b,s->latest[id].v[0],s->latest[id].valid&1,info->unit[0]);
        ui_text_fit(fb,UI_F_SMALL,384,y+23,b,C_SEC,UI_RIGHT,185);
    }
    ui_button(fb,130,398,80,44,"<",BTN_SECONDARY,0);
    ui_button(fb,336,398,80,44,">",BTN_SECONDARY,0);
    snprintf(b,sizeof b,"%d / %d",s->page+1,(BG_COUNT+ROWS-1)/ROWS);text(fb,403,b,C_SEC);
    text(fb,438,"TAP JOB FOR DETAILS",C_DIS);
}
static void graph(gfx_fb *fb)
{
    if(fb->rec) {gfx_rec_cmd(fb,73,220,394,305,s->hash);return;}
    int top=fb->y_base-fb->oy,bottom=top+fb->h;
    if(bottom<220||top>305)return;
    gfx_fill_rect(fb,76,303,314,1,C_LINE);
    background_snapshot *v=&s->snapshot;
    for(int i=0;i<v->count;i++) {
        if(!(v->points[i].valid&(1<<s->metric)))continue;
        if(i && (v->points[i-1].valid&(1<<s->metric)) && v->points[i].ms-v->points[i-1].ms<=2500u)
            gfx_line(fb,s->x[i-1],s->y[i-1],s->x[i],s->y[i],1.5f,C_PRI);
        else gfx_fill_circle(fb,s->x[i],s->y[i],2,C_PRI);
    }
    if(s->selected>=0 && s->selected<v->count) {
        int i=s->selected;gfx_fill_rect(fb,s->x[i],222,1,81,C_SEC);
        if(v->points[i].valid&(1<<s->metric))gfx_fill_circle(fb,s->x[i],s->y[i],3,C_PRI);
    }
}
static void graph_view(gfx_fb *fb)
{
    const background_info *info=background_job(s->job);background_snapshot *v=&s->snapshot;
    const background_point *point=s->selected>=0?&v->points[s->selected]:0;
    int valid=point?point->valid:v->valid;
    if(!point && (v->state==BG_ABSENT || v->state==BG_STALE))valid=0;
    float val=point?point->v[s->metric]:v->v[s->metric];char b[96];
    value_text(b,sizeof b,val,valid&(1<<s->metric),info->unit[s->metric]);
    ui_text_fit(fb,UI_F_DISPLAY,UI_CX,164,b,C_PRI,UI_CENTER,340);
    text(fb,193,info->metric[s->metric],C_SEC);
    text(fb,215,"TAP VALUE TO CHANGE METRIC",C_DIS);
    graph(fb);
    int plotted=0;for(int i=0;i<v->count;i++)plotted+=(v->points[i].valid&(1<<s->metric))!=0;
    if(!plotted)ui_state(fb,UI_CX,271,"NO VALID READINGS",C_SEC);
    if(s->selected>=0) {
        char t[24];uptime(t,sizeof t,point->ms);
        snprintf(b,sizeof b,"UP %s / TAP GRAPH TO GO LIVE",t);
    } else if(plotted)snprintf(b,sizeof b,"2 MIN RAM / RANGE %.2f TO %.2f",(double)s->low,(double)s->high);
    else snprintf(b,sizeof b,"RAM HISTORY / RESTART CLEARS");
    text(fb,326,b,C_DIS);
    if(s->job==BG_MOTION && v->have_sample)
        snprintf(b,sizeof b,"X %+.2f  Y %+.2f  Z %+.2f g",(double)v->axes[0],(double)v->axes[1],(double)v->axes[2]);
    else if(s->job==BG_JOURNAL || s->job==BG_TRACE || s->job==BG_IMU_REC)
        snprintf(b,sizeof b,"ERRORS %lu / DROPPED %lu",(unsigned long)v->errors,(unsigned long)v->dropped);
    else snprintf(b,sizeof b,"%s",info->about);
    text(fb,350,b,C_SEC);
    ui_button(fb,UI_CX,381,246,44,"OPEN RECORDS",BTN_SECONDARY,0);
}
static void records_view(gfx_fb *fb)
{
    background_snapshot *v=&s->snapshot;const background_info *info=background_job(s->job);
    int count=s->tab==1?v->count:v->event_count;
    if(!count) {text(fb,218,"NO OBSERVATIONS YET",C_SEC);text(fb,250,info->about,C_DIS);}
    for(int row=0;row<4;row++) {
        int index=s->offset+row;if(index>=count)break;
        int y=147+row*48;char b[80],t[24];
        if(s->tab==1) {
            const background_point *p=&v->points[count-1-index];uptime(t,sizeof t,p->ms);
            snprintf(b,sizeof b,"UP %s / %s",t,background_state_name(p->state));text(fb,y,b,C_SEC);
            value_text(b,sizeof b,p->v[s->metric],p->valid&(1<<s->metric),info->unit[s->metric]);text(fb,y+21,b,C_PRI);
        } else {
            const background_event *e=&v->events[index];uptime(t,sizeof t,e->ms);
            snprintf(b,sizeof b,"UP %s / %s",t,background_state_name(e->state));text(fb,y,b,ink(e->state));
            snprintf(b,sizeof b,"UPDATES %lu / ERR %lu / LOST %lu",(unsigned long)e->updates,(unsigned long)e->errors,(unsigned long)e->dropped);text(fb,y+21,b,C_SEC);
        }
    }
    ui_button(fb,124,348,104,40,"NEWER",BTN_SECONDARY,0);
    ui_button(fb,342,348,104,40,"OLDER",BTN_SECONDARY,0);
    char b[40];snprintf(b,sizeof b,"%d / %d",count?s->offset/4+1:0,(count+3)/4);text(fb,353,b,C_DIS);
    ui_button(fb,UI_CX,391,200,36,"RESUME LIVE",BTN_GHOST,0);
}
static void detail(gfx_fb *fb)
{
    if(!s || s->job<0)return;
    const background_info *info=background_job(s->job);background_snapshot *v=&s->snapshot;
    ui_title(fb,info->title,0);
    char b[96];
    int32_t age=(int32_t)(v->now-v->last_ms);if(age<0)age=0;
    if(v->state==BG_ABSENT)snprintf(b,sizeof b,"NO MODULE / %lu EARLIER UPDATES",(unsigned long)v->updates);
    else if(v->have_sample)snprintf(b,sizeof b,"%s / LAST %lus / %lu %s",background_state_name(v->state),
        (unsigned long)age/1000,(unsigned long)v->updates,
        s->job==BG_STEPS||s->job==BG_CALORIES||s->job==BG_TRACE||s->job==BG_WORKOUT?"CHECKS":"UPDATES");
    else snprintf(b,sizeof b,"%s / NO READING YET",background_state_name(v->state));
    text(fb,103,b,ink(v->state));
    if(s->tab==0)graph_view(fb);else records_view(fb);
    const char *tabs[]={"GRAPH","READINGS","EVENTS"};
    for(int i=0;i<3;i++) {
        int x=125+i*108;ui_text(fb,UI_F_SMALL,x,427,tabs[i],s->tab==i?C_PRI:C_DIS,UI_CENTER,0);
        if(s->tab==i)gfx_fill_rect(fb,x-20,436,40,2,C_PRI);
    }
}
static int overview_input(const nos_input *in)
{
    if(!s || in->type!=IN_TAP)return 0;
    if(in->y>=121&&in->y<365&&in->x>=60&&in->x<=406) {
        int job=s->page*ROWS+(in->y-121)/61;if(job>=BG_COUNT)return 1;
        s->job=job;s->tab=s->metric=s->offset=s->frozen=0;s->selected=-1;refresh(1);
        nos_push(&scr_background_detail,TX_NONE);nos_feedback(FB_TAP);return 1;
    }
    if(in->y>=376&&in->y<=422) {
        int pages=(BG_COUNT+ROWS-1)/ROWS;
        if(in->x>=86&&in->x<=174)s->page=(s->page+pages-1)%pages;
        else if(in->x>=292&&in->x<=380)s->page=(s->page+1)%pages;
        else return 0;
        nos_feedback(FB_TICK);nos_invalidate();return 1;
    }
    return 0;
}
static const nos_screen *records_screen(void)
{
    switch(s->job) {
    case BG_MOTION:case BG_IMU_REC:return &scr_imu;
    case BG_STEPS:return &scr_activity;
    case BG_CALORIES:return &scr_energy;
    case BG_BATTERY:return &scr_battery_history;
    case BG_TRACE:return &scr_developer;
    case BG_WORKOUT:return &scr_workout;
    default:return &scr_sports_logs;
    }
}
static int detail_input(const nos_input *in)
{
    if(!s || in->type!=IN_TAP)return 0;
    if(in->y>=407&&in->y<=451&&in->x>=71&&in->x<395) {
        s->tab=(in->x-71)/108;s->offset=0;s->selected=-1;s->frozen=s->tab!=0;
        if(!s->frozen)refresh(1);
        nos_invalidate();nos_feedback(FB_TICK);return 1;
    }
    if(s->tab==0) {
        if(in->y>=120&&in->y<=216) {
            do{s->metric=(s->metric+1)%3;}while(!background_job(s->job)->metric[s->metric][0]);
            prepare();nos_invalidate();return 1;
        }
        if(in->y>=220&&in->y<=310&&in->x>=65&&in->x<=401) {
            if(s->frozen) {s->frozen=0;s->selected=-1;refresh(1);}
            else {
                int nearest=-1,distance=10000;
                for(int i=0;i<s->snapshot.count;i++){int d=s->x[i]-in->x;if(d<0)d=-d;if(d<distance){nearest=i;distance=d;}}
                s->selected=nearest;s->frozen=nearest>=0;prepare();
            }
            nos_invalidate();return 1;
        }
        if(in->y>=359&&in->y<=403&&in->x>=110&&in->x<=356) {
            nos_push(records_screen(),TX_NONE);return 1;
        }
    } else {
        int count=s->tab==1?s->snapshot.count:s->snapshot.event_count;
        if(in->y>=328&&in->y<=369) {
            if(in->x>=70&&in->x<=176)s->offset=s->offset>=4?s->offset-4:0;
            else if(in->x>=290&&in->x<=394 && s->offset+4<count)s->offset+=4;
            else return 0;
            nos_invalidate();return 1;
        }
        if(in->y>=373&&in->y<=407) {s->tab=s->frozen=s->offset=0;refresh(1);return 1;}
    }
    return 0;
}
static const nos_screen scr_background_detail={"JOB DETAILS",0,0,tick,detail,detail_input,SCR_LIVE_RENDER};
const nos_screen scr_background={"BACKGROUND JOBS",enter,0,tick,overview,overview_input,SCR_LIVE_RENDER};
