/* Energy / battery / journal / tuning share the same dated timeline.
 * Screen changes are cuts; chart selections track the finger without animation. */
#include "apps/nos_apps.h"
#include "ui.h"
#include "dot.h"
#include "svc.h"
#include "sports.h"
#include "tuning.h"
#include "wake_service.h"
#include "prefs.h"
#include "energy.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

typedef struct {
    int kind,day,start,end,metric,selected,drag,anchor;
    int editor,water,amount,log_offset,feature,outcome,seconds,training;
    uint32_t daykey,next,query;
    sports_bin bins[24];
    sports_record event;
    int have_event,fields,raw,tune_param;
    char message[40];
} energy_state;
/* Children share drawing callbacks, but each stack screen retains its own
 * timeline/editor. Uncovering selects the parent's saved state. */
static energy_state *states[5],*active;
#define E (*active)
static int trial_feature,trial_expected=10,trial_report;
static uint32_t trial_started,trial_duration;
static const char *const features[]={"GENERAL","WALKING","KNOCK LOCK","RAISE WAKE","PICKUP","TURN WRIST","AIR GESTURE","TOUCH","GPS","RECORDER"};
static const char *const outcomes[]={"MISSED","FALSE TRIGGER","WORKED"};
static void refresh(void)
{
    E.daykey=sports_day(E.day);
    if(E.kind<2) {
        memset(E.bins,0,sizeof E.bins);
        E.query=sports_query_request(E.daykey,E.start,E.end);
        strcpy(E.message,E.query?"LOADING HISTORY":"HISTORY UNAVAILABLE");
    }
    E.next=hal_millis();
}
static void init(int kind)
{
    int fresh=states[kind]==NULL;
    if(fresh)states[kind]=nos_mem_alloc(sizeof *active,"energy.ui");
    active=states[kind];if(!active)return;
    if(nos_revealed() && !fresh)return;
    memset(&E,0,sizeof E);E.kind=kind;E.end=1440;E.selected=-1;E.amount=250;E.seconds=30;
    refresh();
}
static void energy_enter(void) {init(0);}
static void battery_enter(void) {init(1);}
static void logs_enter(void) {init(2);}
static void report_enter(void) {
    init(3);
    if(!active)return;
    if(nos_revealed())return;
    if(trial_report){E.feature=trial_feature;E.seconds=(int)trial_duration;trial_report=0;}
    strcpy(E.message,sports_report(E.feature,-1,E.seconds)?"TIME MARKED":"MARK SAVE FAILED");
}
static void tune_enter(void) {init(4);if(!active || nos_revealed())return;E.feature=trial_feature?trial_feature:1;E.training=trial_started!=0;}
static void tick(float dt)
{
    if(!active)return;
    (void)dt;
    if(E.query) {
        int result=sports_query_poll(E.query,E.bins);
        if(result) {E.query=0;strcpy(E.message,result>0?"":"HISTORY UNAVAILABLE");E.next=hal_millis();}
    }
    if(E.kind<2 && !E.query && !E.editor && !E.drag && hal_millis()-E.next>=10000)refresh();
    if(E.kind==2 && (!E.next || hal_millis()-E.next>=1000)) {
        E.have_event=E.raw?sports_raw(E.daykey,E.log_offset,&E.event):
            sports_archived(E.daykey,E.log_offset,&E.event) || (E.day==0 && sports_recent(E.log_offset,&E.event));
        E.next=hal_millis();
    }
    if(E.kind==4 && E.training && E.feature>=3 && E.feature<=5) {
        wake_info w;wake_service_get(&w);
        strcpy(E.message,w.learning?"HOLD STILL IN READING POSITION":w.learn_result==1?"VIEW LEARNED / END TRIAL":w.learn_result<0?"LEARN FAILED / RETRY TRIAL":"PERFORM GESTURE THEN END TRIAL");
    }
}
static void label(gfx_fb *fb,int y,const char *s,uint16_t c)
{
    int x0,x1;ui_row_span((float)y,12,&x0,&x1);
    const font_t *font=UI_F_LABEL;int tracking=2;
    if(ui_text_width(font,s,tracking)>x1-x0)tracking=0;
    if(ui_text_width(font,s,tracking)>x1-x0)font=UI_F_SMALL;
    ui_text(fb,font,UI_CX,y,s,c,UI_CENTER,tracking);
}
static void button(gfx_fb *fb,int x,int y,int w,const char *s)
{
    ui_rrect(fb,x-w/2,y-23,w,46,23,C_SURF2);
    ui_text(fb,UI_F_SMALL,x,y+5,s,C_PRI,UI_CENTER,0);
}
static float value(const sports_bin *b)
{
    if(E.kind==1)return E.metric?b->mv:b->battery;
    return E.metric==0?b->burn:E.metric==1?b->food:b->water;
}
static void draw_chart(gfx_fb *fb)
{
    float max=1;for(int i=0;i<24;i++)if(value(&E.bins[i])>max)max=value(&E.bins[i]);
    if(E.kind==1 && !E.metric)max=100;
    for(int i=0;i<24;i++) {
        float v=value(&E.bins[i]);int h=(int)(v/max*105);
        if(h<1 && v>0)h=1;
        int x=65+i*14;uint16_t c=E.selected==i?C_RED:C_PRI;
        ui_rrect(fb,x,287-h,10,h?h:1,2,h?c:C_LINE);
        if(E.kind==1 && E.bins[i].modes) {
            uint16_t mc=E.bins[i].external_samples?C_RED:(E.bins[i].modes&(1u<<SP_ACTIVE))?C_WHITE:C_SEC;
            ui_rrect(fb,x,291,10,3,1,mc);
        }
    }
    char b[80];
    snprintf(b,sizeof b,"%02d:%02d                   %02d:%02d",E.start/60,E.start%60,E.end/60,E.end%60);
    label(fb,311,b,C_DIS);
    if(E.selected>=0) {
        int a=E.start+(E.end-E.start)*E.selected/24;
        int z=E.start+(E.end-E.start)*(E.selected+1)/24;
        sports_bin *s=&E.bins[E.selected];
        if(E.kind==1 && !(E.metric?s->voltage_samples:s->samples))snprintf(b,sizeof b,"%02d:%02d-%02d:%02d NO VALID SAMPLE",a/60,a%60,z/60,z%60);
        else snprintf(b,sizeof b,"%02d:%02d-%02d:%02d  %.0f %s",a/60,a%60,z/60,z%60,
            (double)value(s),E.kind==1?(E.metric?"mV":"%"):(E.metric==2?"mL":"kcal"));
        label(fb,338,b,C_PRI);
    } else label(fb,338,"TAP BAR / DRAG TO ZOOM",C_DIS);
}
static void draw_editor(gfx_fb *fb)
{
    ui_title(fb,E.water?"ADD WATER":"ADD FOOD",0);
    char b[40];snprintf(b,sizeof b,"%d %s",E.amount,E.water?"mL":"kcal");label(fb,170,b,C_WHITE);
    button(fb,125,235,90,"-50");button(fb,233,235,90,"+50");button(fb,341,235,90,"+250");
    button(fb,150,300,120,"-1");button(fb,316,300,120,"+1");
    button(fb,150,365,140,"CANCEL");button(fb,316,365,140,"SAVE");
    label(fb,410,E.message,C_RED);
}
/* Full-day resting energy from the stored body profile (energy.h); defaults are flagged by the caller. */
static float rest_day(int *assumed)
{
    hal_datetime d;hal_get_datetime(&d);
    energy_body b={nos_prefs.sports_mass_kg,nos_prefs.body_height_cm,
        (uint8_t)energy_age_from_birth(nos_prefs.body_birth_year,d.year),nos_prefs.body_sex};
    *assumed=energy_assumed(&b);
    return energy_bmr_day(&b);
}
static void draw_history(gfx_fb *fb)
{
    if(E.editor){draw_editor(fb);return;}
    ui_title(fb,E.kind?"BATTERY HISTORY":"ENERGY",0);
    char b[70];snprintf(b,sizeof b,"<  %04lu-%02lu-%02lu  >",(unsigned long)E.daykey/10000,
        (unsigned long)E.daykey/100%100,(unsigned long)E.daykey%100);label(fb,108,b,C_PRI);
    if(E.kind==0) {
        float food=0,water=0,burn=0,total=0;int full=E.start==0 && E.end==1440,assumed=0;
        for(int i=0;i<24;i++){food+=E.bins[i].food;water+=E.bins[i].water;if(E.bins[i].burn>burn)burn=E.bins[i].burn;}
        const svc_activity_t *act=svc_activity();
        /* ACTIVE = steps outside workouts + workouts (svc.c). TOTAL adds resting energy: today by the
         * clock so far, a past day for the whole day from the current profile. */
        if(E.day==0 && full) {burn=act->kcal;total=act->kcal_total;assumed=act->kcal_assumed;}
        else if(full)total=burn+rest_day(&assumed);
        snprintf(b,sizeof b,"%.0f ACTIVE / %.0f EAT / %.0f mL",(double)burn,(double)food,(double)water);
        label(fb,137,b,C_SEC);
        static const char *const names[]={"ACTIVE ENERGY: DAILY ESTIMATE","FOOD: PER INTERVAL","WATER: PER INTERVAL"};
        if(E.metric==0 && full) {snprintf(b,sizeof b,"TOTAL %s%.0f KCAL INCL. REST",assumed?"~":"",(double)total);label(fb,165,b,C_WHITE);}
        else label(fb,165,names[E.metric],C_WHITE);
    } else {
        hal_power p;hal_power_get(&p);
        snprintf(b,sizeof b,"%d mV / %s",p.rail_mv,p.external_power==1?"EXTERNAL POWER":p.external_power==0?"ON BATTERY":"POWER UNKNOWN");
        label(fb,137,b,C_SEC);label(fb,165,E.metric?"VOLTAGE / TAP TO CHANGE":"LEVEL / TAP TO CHANGE",C_WHITE);
    }
    draw_chart(fb);
    if(E.kind==0) {button(fb,117,378,100,"+ FOOD");button(fb,233,378,100,"+ WATER");button(fb,349,378,100,"UNDO");}
    else {button(fb,150,378,140,"MODE LOG");button(fb,316,378,140,nos_prefs.sensor_idle?"IDLE 80MHz ON":"IDLE 80MHz OFF");}
    label(fb,411,E.message[0]?E.message:(E.kind?"ACTIVE WHITE / IDLE GREY":
        svc_activity()->kcal_assumed?"~ DEFAULT BODY: SPORTS > SETUP":"ACTIVE = STEPS + WORKOUTS"),C_DIS);
}
static void draw_logs(gfx_fb *fb)
{
    ui_title(fb,E.raw?"RAW SENSORS":"EVENT LOGS",0);sports_status st;sports_get_status(&st);
    char b[80];snprintf(b,sizeof b,"SD %s / LOST %lu / ERR %lu",st.available?"WRITING":"UNAVAILABLE",
        (unsigned long)st.dropped,(unsigned long)st.errors);label(fb,105,b,C_SEC);
    sports_record r=E.event;
    if(E.have_event) {
        snprintf(b,sizeof b,"%s  %02u:%02u",sports_name(r.type),r.minute/60,r.minute%60);label(fb,151,b,C_WHITE);
        snprintf(b,sizeof b,"%08lu / UP %lu.%03lu s",(unsigned long)r.day,(unsigned long)r.uptime/1000,(unsigned long)r.uptime%1000);label(fb,180,b,C_DIS);
        for(int i=0;i<4;i++) {
            int f=i+E.fields*4;
            snprintf(b,sizeof b,"%s: %.3f",sports_field(r.type,f),(double)r.v[f]);
            if((r.type==SP_REPORT||r.type==SP_LABEL) && f==0 && r.v[f]>=0 && r.v[f]<10)
                snprintf(b,sizeof b,"FEATURE: %s",features[(int)r.v[f]]);
            if(r.type==SP_REPORT && f==1 && r.v[f]>=0 && r.v[f]<3)
                snprintf(b,sizeof b,"RESULT: %s",outcomes[(int)r.v[f]]);
            if(r.type==SP_REPORT && f==1 && r.v[f]<0)snprintf(b,sizeof b,"RESULT: UNLABELLED MARK");
            if(r.type==SP_POWER && f==0 && r.v[0]>=0 && r.v[0]<=SP_AOD) {
                const char *m[]={"ACTIVE","SENSOR IDLE","DISPLAY OFF","DEEP SLEEP","AOD"};
                snprintf(b,sizeof b,"MODE: %s",m[(int)r.v[0]]);
            }
            label(fb,218+i*29,b,C_PRI);
        }
    } else label(fb,220,"NO MORE EVENTS",C_DIS);
    button(fb,120,365,100,"NEWER");button(fb,233,365,100,"OLDER");button(fb,346,365,100,"REPORT");
    snprintf(b,sizeof b,"< %08lu >",(unsigned long)E.daykey);label(fb,411,b,C_DIS);
}
static void draw_report(gfx_fb *fb)
{
    ui_title(fb,"QUICK REPORT",0);
    button(fb,233,145,300,features[E.feature]);button(fb,233,207,300,outcomes[E.outcome]);
    char b[50];snprintf(b,sizeof b,"LAST %d SECONDS",E.seconds);button(fb,233,269,300,b);
    button(fb,233,340,240,"SAVE LABEL");label(fb,402,E.message,C_SEC);
}
static void draw_tune(gfx_fb *fb)
{
    ui_title(fb,"WRIST TUNING",0);button(fb,233,115,280,features[E.feature]);
    sports_tuning c;tuning_get(&c);char b[70];
    if(E.feature<=2) {
        snprintf(b,sizeof b,"THRESHOLD %.2f g",(double)(E.feature==1?c.walk_g:c.knock_g));label(fb,164,b,C_WHITE);
        if(E.tune_param) {
            snprintf(b,sizeof b,E.feature==1?"ENTRY %d PEAKS":"QUIET BEFORE %d ms",E.feature==1?c.walk_peaks:c.quiet_ms);
            ui_rrect(fb,65,140,336,38,0,C_BG);label(fb,164,b,C_WHITE);
        }
        button(fb,150,210,145,"MORE SENSITIVE");button(fb,316,210,145,"LESS SENSITIVE");
    } else label(fb,174,"LEARN VIEW: HOLD WATCH TO READ",C_SEC);
    button(fb,233,270,300,E.training?"END TRIAL / LABEL RESULT":E.feature>=3&&E.feature<=5?"LEARN VIEW + START TRIAL":"START LABELLED TRIAL");
    button(fb,150,338,140,"RESET");button(fb,316,338,140,"LOGS");
    snprintf(b,sizeof b,"TRIAL: %d REPS (TAP TO CHANGE)",trial_expected);
    label(fb,388,E.message[0]?E.message:b,C_DIS);
    snprintf(b,sizeof b,"ACTIVE CONFIG REV %u",tuning_revision());label(fb,418,b,C_DIS);
}
static void draw(gfx_fb *fb)
{
    if(!active) {ui_title(fb,"HEALTH HISTORY",0);ui_state(fb,UI_CX,UI_CY,"MEMORY UNAVAILABLE",C_DIS);return;}
    if(E.kind<2)draw_history(fb);else if(E.kind==2)draw_logs(fb);else if(E.kind==3)draw_report(fb);else draw_tune(fb);
}
static int bar_at(int x) {int i=(x-65)/14;return i<0?0:i>23?23:i;}
static int input(const nos_input *in)
{
    if(in->type==IN_CANCEL) {if(active) E.drag=0;return 1;}
    if(!active)return 0;
    if(E.kind<2 && !E.editor) {
        if(in->type==IN_DOWN && in->y>=180 && in->y<=300) {E.anchor=bar_at(in->x);E.selected=E.anchor;}
        if(in->type==IN_DRAG_START && in->axis==AXIS_X && in->y>=180 && in->y<=300){E.drag=1;return 1;}
        if(in->type==IN_DRAG && E.drag){E.selected=bar_at(in->x);return 1;}
        if(in->type==IN_DRAG_END && E.drag) {
            E.drag=0;int a=E.anchor,z=E.selected;if(a>z){int t=a;a=z;z=t;}
            if(z>a) {int span=E.end-E.start;int begin=E.start+span*a/24;E.end=E.start+span*(z+1)/24;E.start=begin;E.selected=-1;refresh();}
            return 1;
        }
    }
    if(in->type!=IN_TAP)return 0;
    int x=in->x,y=in->y;
    if(E.kind<2) {
        if(E.editor) {
            if(y>=210&&y<260)E.amount+=x<180?-50:x<290?50:250;
            else if(y>=275&&y<325)E.amount+=x<233?-1:1;
            else if(y>=340&&y<395) {
                if(x<233)E.editor=0;
                else if(sports_intake(E.water,E.amount)) {E.editor=0;strcpy(E.message,"ENTRY SAVED");refresh();}
                else strcpy(E.message,"SAVE FAILED - TRY AGAIN");
            }
            if(E.amount<1)E.amount=1;
            if(E.amount>10000)E.amount=10000;
        } else if(y>=85&&y<125) {E.day+=x<233?-1:1;if(E.day>0)E.day=0;E.start=0;E.end=1440;E.selected=-1;refresh();}
        else if(y>=140&&y<180)E.metric=(E.metric+1)%(E.kind?2:3);
        else if(y>=180&&y<=300)E.selected=bar_at(x);
        else if(y>=350&&y<405) {
            if(E.kind) {if(x<233)nos_push(&scr_sports_logs,TX_NONE);else {nos_prefs.sensor_idle=!nos_prefs.sensor_idle;prefs_save();}}
            else if(x<290) {E.water=x>=175;E.editor=1;E.message[0]=0;}
            else strcpy(E.message,sports_undo()?"LAST ENTRY UNDONE":"NOTHING TO UNDO / SAVE FAILED");
        } else if(y>=405){E.start=0;E.end=1440;E.selected=-1;refresh();}
    } else if(E.kind==2) {
        E.next=0;
        if(y>=40&&y<85){E.raw=!E.raw;E.log_offset=0;}
        if(y>=125&&y<330)E.fields=!E.fields;
        if(y>=395) {E.day+=x<233?-1:1;if(E.day>0)E.day=0;E.daykey=sports_day(E.day);E.log_offset=0;}
        if(y>=340&&y<395) {if(x<175){if(E.log_offset>0)E.log_offset--;}else if(x<290)E.log_offset++;else nos_push(&scr_quick_report,TX_NONE);}
    } else if(E.kind==3) {
        if(y>=120&&y<175)E.feature=(E.feature+1)%10;
        else if(y>=180&&y<235)E.outcome=(E.outcome+1)%3;
        else if(y>=240&&y<298)E.seconds=E.seconds==30?60:E.seconds==60?300:30;
        else if(y>=310&&y<370){strcpy(E.message,sports_report(E.feature,E.outcome,E.seconds)?"LABEL SAVED":"SAVE FAILED");}
    } else {
        if(y>=370&&!E.training){trial_expected=trial_expected==10?20:trial_expected==20?50:10;E.message[0]=0;}
        if(y>=140&&y<185)E.tune_param=!E.tune_param;
        if(y>=90&&y<140 && !E.training)E.feature=E.feature==9?1:E.feature+1;
        else if(y>=185&&y<240 && E.feature<=2) {
            strcpy(E.message,tuning_adjust(E.feature-1+E.tune_param*2,x<233?-1:1)?"SENSITIVITY SAVED":"SAVE FAILED");
        } else if(y>=245&&y<300) {
            E.training=!E.training;tuning_session(E.feature,E.training,trial_expected);
            trial_feature=E.feature;
            if(E.training)trial_started=hal_millis()?hal_millis():1;
            else {trial_duration=(hal_millis()-trial_started)/1000;trial_started=0;trial_report=1;}
            if(E.training && E.feature>=3 && E.feature<=5)wake_service_learn();
            if(!E.training) {nos_push(&scr_quick_report,TX_NONE);return 1;}
            strcpy(E.message,"TRIAL STARTED - PERFORM THE GESTURE");
        } else if(y>=310&&y<365) {
            if(x<233) {strcpy(E.message,tuning_adjust(0,0)?"DEFAULT THRESHOLDS RESTORED":"RESET SAVE FAILED / TAP TO RETRY");}
            else nos_push(&scr_sports_logs,TX_NONE);
        }
    }
    nos_feedback(FB_TAP);return 1;
}
const nos_screen scr_energy={"ENERGY",energy_enter,0,tick,draw,input,SCR_HDRAG};
const nos_screen scr_battery_history={"BATTERY HISTORY",battery_enter,0,tick,draw,input,SCR_HDRAG};
const nos_screen scr_sports_logs={"SENSOR LOGS",logs_enter,0,tick,draw,input,0};
const nos_screen scr_quick_report={"QUICK REPORT",report_enter,0,tick,draw,input,0};
const nos_screen scr_wrist_tuning={"WRIST TUNING",tune_enter,0,tick,draw,input,SCR_KEEPAWAKE};
