/* Developer overlays, saved display controls and system trace. */
#include "nos.h"
#include "ui.h"
#include "developer.h"
#include "diag_log.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include <stdio.h>
static diag_log_status trace_status;
static unsigned trace_overlays;
static int trace_marked;
static int trace_page,trace_have_fault;
static uint32_t trace_mark_request;
static diag_fault_summary trace_fault;
static const char *reset_name(uint32_t reason)
{
    static const char *const names[]={"UNKNOWN","POWER ON","EXTERNAL","SOFTWARE","PANIC",
        "INTERRUPT WDT","TASK WDT","WATCHDOG","DEEP SLEEP","BROWNOUT","SDIO","USB",
        "JTAG","EFUSE","POWER GLITCH","CPU LOCKUP"};
    return reason<sizeof(names)/sizeof(names[0])?names[reason]:"UNKNOWN";
}
static void trace_tick(float dt) {(void)dt;diag_log_get(&trace_status);trace_have_fault=diag_log_fault_get(0,&trace_fault);}
static void trace_enter(void)
{
    if(nos_revealed()) {trace_tick(0);return;}
    trace_overlays=nos_dev_overlays;nos_dev_overlays=0;trace_marked=0;trace_page=0;trace_mark_request=0;trace_tick(0);
}
static void trace_exit(void) {if(!nos_leaving_covered())nos_dev_overlays=trace_overlays;}
static void trace_row(gfx_fb *fb,int y,const char *name,uint32_t value)
{
    char text[16];snprintf(text,sizeof(text),"%lu",(unsigned long)value);
    ui_text(fb,UI_F_BODY,90,y,name,C_PRI,UI_LEFT,0);
    ui_text(fb,UI_F_BODY,376,y,text,C_SEC,UI_RIGHT,0);
}
static void trace_draw(gfx_fb *fb)
{
    ui_title(fb,"SYSTEM TRACE",0);
    ui_label(fb,UI_CX,108,!trace_status.ready?"LOGGER NOT READY":trace_status.available?"SD AVAILABLE":"WAITING FOR SD",C_SEC,UI_CENTER);
    char text[80];
    if(trace_page) {
        trace_row(fb,145,"First fault",trace_have_fault?trace_fault.code:0);
        trace_row(fb,177,"Occurrences",trace_have_fault?trace_fault.count:0);
        trace_row(fb,209,"First ms",trace_have_fault?trace_fault.first_ms:0);
        trace_row(fb,241,"Last ms",trace_have_fault?trace_fault.last_ms:0);
        snprintf(text,sizeof text,"FAULTS %lu / MERGED %lu",(unsigned long)trace_status.faults,(unsigned long)trace_status.coalesced);
        ui_label(fb,UI_CX,278,text,C_PRI,UI_CENTER);
        snprintf(text,sizeof text,"CRASH %s / PRIORITY LOSS %lu",trace_status.crash_state==2?"SAVED":trace_status.crash_state==1?"ARMED":trace_status.crash_state==0?"DISABLED":"CHECK REQUIRED",(unsigned long)trace_status.priority_dropped);
        ui_text_fit(fb,UI_F_SMALL,UI_CX,309,text,C_SEC,UI_CENTER,320);
    } else {
    trace_row(fb,145,"Queued",trace_status.queued);
    trace_row(fb,177,"Written",trace_status.written);
    trace_row(fb,209,"Dropped",trace_status.dropped);
    trace_row(fb,241,"Errors",trace_status.errors);
    snprintf(text,sizeof(text),"RESET: %s",reset_name(trace_status.reset_reason));
    ui_label(fb,UI_CX,278,text,C_PRI,UI_CENTER);
    snprintf(text,sizeof(text),"PREVIOUS %lu / CRASHES %lu",(unsigned long)trace_status.previous_events,(unsigned long)trace_status.crash_boots);
    ui_text_fit(fb,UI_F_SMALL,UI_CX,309,text,C_SEC,UI_CENTER,320);
    }
    snprintf(text,sizeof text,"RESULT %lu: %ld / %s",(unsigned long)trace_status.last_result_request,
             (long)trace_status.last_result,trace_status.evidence_lost?"EVIDENCE LOSS":"NO KNOWN LOSS");
    ui_text_fit(fb,UI_F_SMALL,UI_CX,337,text,trace_status.evidence_lost?C_RED:C_DIS,UI_CENTER,330);
    ui_button(fb,UI_CX,382,252,46,"ADD TEST MARK",BTN_SECONDARY,0);
    if(trace_marked)snprintf(text,sizeof text,"MARK REQUEST %lu",(unsigned long)trace_mark_request);
    else snprintf(text,sizeof text,"TAP STATUS: SUMMARY / FAULTS");
    ui_text(fb,UI_F_SMALL,UI_CX,422,text,C_DIS,UI_CENTER,0);
}
static int trace_input(const nos_input *in)
{
    if(in->type==IN_TAP && in->y>=85 && in->y<=125) {trace_page=!trace_page;nos_invalidate();return 1;}
    if(in->type!=IN_TAP || in->x<107 || in->x>359 || in->y<359 || in->y>405)return 0;
    trace_mark_request=diag_action_request(DIAG_DOMAIN_DEVELOPER,DIAG_ACT_MARK,(int32_t)diag_log_name_id("diag.marker"));
    diag_action_result(trace_mark_request,trace_mark_request?DIAG_RESULT_SUCCESS:DIAG_RESULT_FAILED,0);
    trace_marked=1;nos_invalidate();nos_feedback(FB_TICK);return 1;
}
static const nos_screen scr_system_trace={"SYSTEM TRACE",trace_enter,trace_exit,trace_tick,trace_draw,trace_input,0};
static const uint8_t frame_caps[]={24,30,40,48,60,0};
static void cap_text(char *text,size_t size,unsigned fps)
{
    if(fps)snprintf(text,size,"%u FPS",fps);
    else snprintf(text,size,"Unlimited");
}
static void cap_draw(gfx_fb *fb)
{
    ui_title(fb,"FRAME RATE",0);
    for(unsigned i=0;i<sizeof frame_caps/sizeof *frame_caps;i++) {
        char text[16];cap_text(text,sizeof text,frame_caps[i]);
        int y=133+(int)i*45;
        ui_text(fb,UI_F_BODY,104,y,text,C_PRI,UI_LEFT,0);
        if(nos_prefs.fps_cap==frame_caps[i])
            ui_text(fb,UI_F_SMALL,362,y,"SET",C_SEC,UI_RIGHT,0);
    }
}
static int cap_input(const nos_input *in)
{
    if(in->type!=IN_TAP || in->x<80 || in->x>386)return 0;
    for(unsigned i=0;i<sizeof frame_caps/sizeof *frame_caps;i++) {
        int y=133+(int)i*45;
        if(in->y>=y-27 && in->y<y+16) {
            if(nos_prefs.fps_cap!=frame_caps[i]) {
                nos_prefs.fps_cap=frame_caps[i];prefs_save();
            }
            nos_invalidate();nos_feedback(FB_TICK);return 1;
        }
    }
    return 0;
}
static const nos_screen scr_frame_cap={"FRAME RATE",0,0,0,cap_draw,cap_input,0};
static const char *labels[]={"Loop FPS","Frame stats","Touch position","Screen name"};
static void dev_draw(gfx_fb *fb) {
    ui_title(fb,"DEVELOPER TOOLS",0);
    for(int i=0;i<4;i++){
        int y=116+i*39;
        ui_text(fb,UI_F_BODY,78,y,labels[i],C_PRI,UI_LEFT,0);
        ui_text(fb,UI_F_SMALL,380,y+4,(nos_dev_overlays&(1u<<i))?"ON":"OFF",C_SEC,UI_RIGHT,0);
    }
    char text[16];cap_text(text,sizeof text,nos_prefs.fps_cap);
    ui_text(fb,UI_F_BODY,78,272,"Frame rate",C_PRI,UI_LEFT,0);
    ui_text(fb,UI_F_SMALL,380,276,text,C_SEC,UI_RIGHT,0);
    int te=nos_dev_te_available();
    ui_text(fb,UI_F_BODY,78,311,"TE sync",te?C_PRI:C_DIS,UI_LEFT,0);
    ui_text(fb,UI_F_SMALL,380,315,te?(nos_prefs.display_te?"ON":"OFF"):"N/A",C_SEC,UI_RIGHT,0);
    ui_text(fb,UI_F_BODY,UI_CX,369,"Turn all off",C_PRI,UI_CENTER,0);
    ui_button(fb,UI_CX,418,234,38,"SYSTEM TRACE",BTN_SECONDARY,0);
}
static int dev_input(const nos_input *in) {
    if(in->type!=IN_TAP || in->x<60 || in->x>406)return 0;
    for(int i=0;i<4;i++)if(in->y>=90+i*39 && in->y<129+i*39){
        nos_dev_overlays^=1u<<i;nos_invalidate();nos_feedback(FB_TICK);return 1;
    }
    if(in->y>=246 && in->y<285){nos_push(&scr_frame_cap,TX_NONE);nos_feedback(FB_TICK);return 1;}
    if(in->y>=285 && in->y<337){
        if(nos_dev_te_available()) {
            nos_prefs.display_te=!nos_prefs.display_te;prefs_save();nos_invalidate();nos_feedback(FB_TICK);
        }
        return 1;
    }
    if(in->y>=355 && in->y<=399){nos_dev_overlays=0;nos_invalidate();return 1;}
    if(in->x>=116 && in->x<=350 && in->y>=400 && in->y<=437){
        nos_push(&scr_system_trace,TX_NONE);nos_feedback(FB_TICK);return 1;
    }
    return 0;
}
const nos_screen scr_developer={"DEVELOPER TOOLS",0,0,0,dev_draw,dev_input,0};
