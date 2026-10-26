/* Cached chat pages: draw never touches codecs, storage or the network.
 * Animation geometry and word wrapping are prepared once in tick. */
#include "apps/nos_apps.h"
#include "voice_chat.h"
#include "ui.h"
#include "dot.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#define LINES 128
#define VISIBLE 5
static struct {
    voice_chat_snapshot view;
    voice_saved_chat saved[VOICE_SAVED_CHATS];
    char lines[LINES][64],delete_title[40];
    uint8_t role[LINES];
    int count,offset,drag_base,pressed,saved_count,bar_cell;
    uint32_t refreshed,text_generation,delete_id;
    float level,wave[11],entry;
    nos_time_t entered;
} *s;
static const nos_screen history_screen,audio_screen,delete_screen;
static int busy(void){return s && s->view.busy;}
static int limit(void){return s->count>VISIBLE?s->count-VISIBLE:0;}
static void append(const char *text,int role)
{
    char line[64];int n=0;const unsigned char *p=(const unsigned char *)text;
    while(*p && s->count<LINES) {
        unsigned char c=*p++;
        if(c>=128){while((*p&0xc0)==0x80)p++;c='?';}
        if(c<32 && c!='\n')c=' ';
        if(c=='\n') {
            line[n]=0;strcpy(s->lines[s->count],line);s->role[s->count++]=(uint8_t)role;n=0;continue;
        }
        line[n++]=(char)c;line[n]=0;
        if(ui_text_width(UI_F_BODY,line,0)>288 || n==62) {
            char *space=strrchr(line,' ');int split=space && space>line?(int)(space-line):n-1;
            int start=space && space>line?split+1:split;char carry[64];strcpy(carry,line+start);
            line[split]=0;strcpy(s->lines[s->count],line);s->role[s->count++]=(uint8_t)role;
            strcpy(line,carry);n=(int)strlen(line);
        }
    }
    if(n && s->count<LINES){line[n]=0;strcpy(s->lines[s->count],line);s->role[s->count++]=(uint8_t)role;}
}
static void transcript(void)
{
    s->count=0;int answer=0;
    const voice_saved_chat *chat=NULL;
    if(!busy()) {
        int count=voice_chat_history_list(s->saved,VOICE_SAVED_CHATS);
        if(count>=0)s->saved_count=count;
        for(int i=0;i<s->saved_count;i++)if(s->saved[i].id==s->view.chat_id)chat=&s->saved[i];
    }
    if(chat)for(unsigned i=0;i+1<chat->count;i++) {
        append("YOU",1);append(chat->turn[i].input,0);append("NOS",1);append(chat->turn[i].reply,0);
        if(chat->turn[i].excerpt)append("SAVED EXCERPT",1);
    }
    if(s->view.input[0]){append("YOU",1);append(s->view.input,0);}
    if(s->view.reply[0]){answer=s->count;append("NOS",1);append(s->view.reply,0);}
    s->offset=answer<limit()?answer:limit();
}
static void refresh(int force)
{
    if(!s)return;
    uint32_t now=hal_millis();if(!force && now-s->refreshed<80)return;s->refreshed=now;
    int was_busy=s->view.busy,previous_offset=s->offset;uint32_t previous_id=s->view.chat_id;
    voice_chat_get(&s->view);
    int changed=s->text_generation!=s->view.text_generation || was_busy!=s->view.busy || previous_id!=s->view.chat_id;
    if(force || changed) {
        s->text_generation=s->view.text_generation;transcript();
        if(!changed)s->offset=previous_offset<limit()?previous_offset:limit();
    }
}
static void enter(void)
{
    if(!s){s=nos_mem_alloc(sizeof *s,"chat.ui");if(!s)return;memset(s,0,sizeof *s);}
    s->pressed=0;s->entered=nos_t;s->entry=0;refresh(1);
}
static void leave(void){if(!nos_leaving_covered())voice_chat_cancel();}
static void tick(float dt)
{
    if(!s)return;
    refresh(0);
    float step=dt*9;if(step>1)step=1;if(step<0)step=0;
    s->level+=(s->view.level/100.0f-s->level)*step;
    double elapsed=nos_t-s->entered;
    s->entry=elapsed>.24?1:elapsed<0?0:(float)(elapsed/.24);
    s->bar_cell=(int)(fmod(elapsed,1.3)*20/1.3);
    for(int i=0;i<11;i++) {
        float shape=.25f+.75f*fabsf(sinf((float)fmod(elapsed,4)*5+i*.73f));
        s->wave[i]=6+58*s->level*shape;
    }
}
static uint16_t ink(uint16_t c){return ui_dim(c,.4f+.6f*s->entry);}
static void small_button(gfx_fb *fb,int x,int y,int w,int h,const char *text,int selected,int action)
{
    uint16_t fill=selected?C_PRI:s->pressed==action?C_SURF2:C_SURF;
    ui_rrect(fb,x-w*.5f,y-h*.5f,w,h,h*.5f,fill);
    ui_text_fit(fb,UI_F_SMALL,x,y+5,text,selected?C_BG:C_PRI,UI_CENTER,w-12);
}
static void progress_bar(gfx_fb *fb)
{
    if(s->view.progress_total) {
        float fraction=(float)s->view.progress_done/s->view.progress_total;if(fraction>1)fraction=1;
        ui_segments(fb,93,325,280,5,20,20*fraction,C_PRI,C_LINE);
    }else for(int i=0;i<20;i++) {
        int distance=(i-s->bar_cell+20)%20;
        uint16_t c=busy() && distance<3?C_PRI:s->view.state==VOICE_DONE?C_SEC:C_LINE;
        ui_rrect(fb,93+i*14.2f,325,10,5,2,c);
    }
    static const char *stages[]={"REC","SEND","THINK","VOICE"};
    int stage=s->view.state==VOICE_LISTENING?0:s->view.state==VOICE_TRANSCRIBING?1:
        s->view.state==VOICE_THINKING?2:s->view.state==VOICE_SYNTHESIZING || s->view.state==VOICE_SPEAKING?3:-1;
    for(int i=0;i<4;i++)ui_text(fb,UI_F_SMALL,128+i*70,348,stages[i],
        stage==i?C_PRI:s->view.state==VOICE_DONE || (stage>=0 && i<stage)?C_SEC:C_DIS,UI_CENTER,0);
}
static void draw(gfx_fb *fb)
{
    ui_title(fb,"CHAT",0);
    if(!s){ui_state(fb,233,233,"UNAVAILABLE",C_SEC);return;}
    char text[96];snprintf(text,sizeof text,"RECENTS %u",(unsigned)s->view.saved_count);
    small_button(fb,154,110,130,38,text,0,6);small_button(fb,312,110,130,38,"AUDIO",0,7);
    for(int i=0;i<3;i++) {
        const voice_profile *p=voice_profile_get(i);int x=121+i*112,selected=s->view.tier==i;
        uint16_t color=selected?C_BG:C_PRI;
        ui_rrect(fb,x-54,132,108,42,21,selected?C_PRI:s->pressed==i+1?C_SURF2:C_SURF);
        ui_text(fb,UI_F_SMALL,x,148,p->name,color,UI_CENTER,0);
        snprintf(text,sizeof text,"%uc/min",p->cents_per_minute);
        ui_text(fb,UI_F_SMALL,x,168,text,color,UI_CENTER,0);
    }
    if(!s->view.available) {
        ui_text_wrap(fb,UI_F_BODY,233,222,290,27,3,"Voice chat needs the C board mic and speaker.",C_SEC,UI_CENTER);
    }else if(s->count) {
        ui_rrect(fb,75,180,316,120,20,C_SURF);
        for(int i=0;i<VISIBLE && i+s->offset<s->count;i++) {
            int k=i+s->offset;
            ui_text(fb,s->role[k]?UI_F_SMALL:UI_F_BODY,89,195+i*24,s->lines[k],ink(s->role[k]?C_SEC:C_PRI),UI_LEFT,0);
        }
        if(s->count>VISIBLE)ui_scrollbar(fb,(float)s->offset,s->count,VISIBLE,1);
    }else if(busy()) {
        if(s->view.state==VOICE_LISTENING) {
            for(int i=0;i<11;i++)ui_rrect(fb,146+i*16,235-s->wave[i]*.5f,9,s->wave[i],4,C_PRI);
            ui_text(fb,UI_F_BODY,233,282,"I'm listening",C_PRI,UI_CENTER,0);
        }else {
            const char *label=s->view.state==VOICE_THINKING?"Thinking it over":s->view.state==VOICE_SYNTHESIZING?"Finding my voice":
                s->view.state==VOICE_TRANSCRIBING?"Hearing your words":"Getting ready";
            for(int i=0;i<5;i++)ui_circle(fb,193+i*20,224,3+((s->bar_cell/3)%5==i?3:0),C_PRI);
            ui_text(fb,UI_F_BODY,233,278,label,C_PRI,UI_CENTER,0);
        }
    }else {
        dot_icon_anim(fb,DOT_ICON_MIC,233,220,5,3.7f,C_PRI,s->entry,DOT_REVEAL_SCAN);
        ui_text(fb,UI_F_BODY,233,276,"Ask your watch",ink(C_PRI),UI_CENTER,0);
    }
    if(s->view.state==VOICE_LISTENING)snprintf(text,sizeof text,"LISTENING %.1fs / %us",s->view.record_ms/1000.0,
        (unsigned)(s->view.record_limit_ms?s->view.record_limit_ms/1000:20));
    else if(s->view.progress_phase==VOICE_PHASE_UPLOAD && s->view.progress_total)
        snprintf(text,sizeof text,"SENDING %u%%",(unsigned)((uint64_t)s->view.progress_done*100/s->view.progress_total));
    else if(s->view.state==VOICE_SPEAKING)snprintf(text,sizeof text,"SPEAKING  %d%% VOLUME",s->view.volume);
    else if(s->view.storage_error)snprintf(text,sizeof text,s->view.storage_error==2?"SAVED CHATS UNAVAILABLE":"HISTORY NOT SAVED; WILL RETRY");
    else if(!s->view.configured)snprintf(text,sizeof text,"ADD YOUR KEY OVER USB");
    else voice_text_copy(text,sizeof text,s->view.status);
    ui_text_fit(fb,UI_F_SMALL,233,315,text,s->view.state==VOICE_ERROR || s->view.storage_error?C_RED:C_SEC,UI_CENTER,328);
    progress_bar(fb);
    small_button(fb,154,386,140,56,busy()?"CANCEL":"NEW CHAT",0,4);
    const char *label=s->view.state==VOICE_LISTENING?"SEND":busy()?"AUDIO":"TALK";
    ui_button(fb,312,386,140,56,label,s->view.state==VOICE_LISTENING?BTN_RED:BTN_PRIMARY,s->pressed==5);
    snprintf(text,sizeof text,"%s%.3fc  /  %s",s->view.estimated?"~":"",s->view.cost_usd*100,s->view.volume && s->view.speak?"VOICE":"TEXT");
    ui_text_fit(fb,UI_F_SMALL,233,435,text,C_DIS,UI_CENTER,220);
}
static void history_enter(void){if(!s)return;s->pressed=0;refresh(1);}
static void history_draw(gfx_fb *fb)
{
    ui_title(fb,"SAVED CHATS",0);if(!s)return;
    char text[80];snprintf(text,sizeof text,"%d OF %d SAVED",s->saved_count,VOICE_SAVED_CHATS);
    ui_text(fb,UI_F_SMALL,233,108,text,C_SEC,UI_CENTER,0);
    ui_segments(fb,173,119,120,4,VOICE_SAVED_CHATS,(float)s->saved_count,C_PRI,C_LINE);
    for(int i=0;i<s->saved_count;i++) {
        const voice_saved_chat *c=&s->saved[i];int y=157+i*57;
        ui_rrect(fb,78,y-26,310,52,18,C_SURF);
        ui_text_fit(fb,UI_F_BODY,94,y,c->title,C_PRI,UI_LEFT,226);
        snprintf(text,sizeof text,"%u TURNS / %s",(unsigned)c->turns,voice_profile_get(c->tier)->name);
        ui_text(fb,UI_F_SMALL,95,y+18,text,C_SEC,UI_LEFT,0);
        small_button(fb,362,y,44,32,"DEL",0,20+i);
    }
    if(!s->saved_count) {
        dot_icon_anim(fb,DOT_ICON_MIC,233,216,6,4,C_DIS,s->entry,DOT_REVEAL_SCAN);
        ui_text(fb,UI_F_BODY,233,282,"Your chats will appear here",C_PRI,UI_CENTER,0);
    }
    ui_text_fit(fb,UI_F_SMALL,233,378,s->view.storage_error==2?"CHATS ARE NOT BEING SAVED":s->view.storage_error?"SAVE FAILED; RETRY PENDING":
        s->view.storage_dirty?"SAVING TEXT...":"LAST 2 TURNS / TEXT ONLY",s->view.storage_error?C_RED:C_DIS,UI_CENTER,310);
    small_button(fb,233,411,220,42,"NEW CHAT",0,30);
}
static int child_back(const nos_input *in)
{if(in->type==IN_BTN){nos_pop();return 1;}return 0;}
static int history_input(const nos_input *in)
{
    if(child_back(in))return 1;
    if(!s || in->type!=IN_TAP)return 0;
    if(in->x>=78 && in->x<=388)for(int i=0;i<s->saved_count;i++)if(in->y>=131+i*57 && in->y<183+i*57) {
        if(in->x>=336) {
            s->delete_id=s->saved[i].id;voice_text_copy(s->delete_title,sizeof s->delete_title,s->saved[i].title);
            nos_push(&delete_screen,TX_NONE);
        }else if(voice_chat_history_select(s->saved[i].id))nos_pop();
        else nos_toast("Finish or cancel the current turn first");
        return 1;
    }
    if(in->x>=123 && in->x<=343 && in->y>=389 && in->y<=433) {
        if(s->saved_count==VOICE_SAVED_CHATS)nos_toast("Delete a saved chat to make room");
        else if(voice_chat_clear())nos_pop();
        return 1;
    }
    return 0;
}
static void delete_draw(gfx_fb *fb)
{
    ui_title(fb,"DELETE CHAT",0);if(!s)return;
    ui_text(fb,UI_F_BODY,233,156,"Delete this conversation?",C_PRI,UI_CENTER,0);
    ui_text_wrap(fb,UI_F_BODY,233,214,285,28,2,s->delete_title,C_SEC,UI_CENTER);
    ui_text(fb,UI_F_SMALL,233,291,"OTHER CHATS STAY SAVED",C_DIS,UI_CENTER,0);
    small_button(fb,154,372,140,64,"KEEP",0,1);
    ui_button(fb,312,372,140,64,"DELETE",BTN_DANGER,s->pressed==2);
}
static int delete_input(const nos_input *in)
{
    if(child_back(in))return 1;
    if(!s || in->type!=IN_TAP || in->y<340 || in->y>404 || in->x<84 || in->x>382)return 0;
    if(in->x<233)nos_pop();
    else if(voice_chat_history_delete(s->delete_id)){nos_pop();nos_feedback(FB_TICK);}
    else nos_toast("Could not delete this chat");
    return 1;
}
static void audio_draw(gfx_fb *fb)
{
    ui_title(fb,"CHAT AUDIO",0);if(!s)return;char text[16];snprintf(text,sizeof text,"%d%%",s->view.volume);
    ui_text(fb,UI_F_SMALL,233,107,"SPEAKER VOLUME",C_SEC,UI_CENTER,0);
    ui_text(fb,UI_F_DISPLAY,233,167,text,C_PRI,UI_CENTER,0);
    ui_slider_p(fb,80,184,306,50,s->view.volume/100.0f,DOT_ICON_SPEAKER,s->pressed==40);
    small_button(fb,129,266,70,42,"-",0,41);small_button(fb,233,266,116,42,s->view.volume?"MUTE":"UNMUTE",0,42);
    small_button(fb,337,266,70,42,"+",0,43);
    ui_text(fb,UI_F_SMALL,233,316,"MIC SENSITIVITY",C_SEC,UI_CENTER,0);
    static const char *labels[]={"LOW","NORMAL","HIGH"};
    for(int i=0;i<3;i++)small_button(fb,133+i*100,348,92,36,labels[i],s->view.mic_gain==24+i*6,44+i);
    ui_text(fb,UI_F_SMALL,105,408,"SPOKEN REPLIES",C_PRI,UI_LEFT,0);ui_toggle(fb,337,402,(float)s->view.speak);
}
static int audio_hit(const nos_input *in)
{
    if(in->x>=80 && in->x<=386 && in->y>=180 && in->y<=238)return 40;
    if(in->y>=243 && in->y<=289) {
        if(in->x>=94 && in->x<=164)return 41;
        if(in->x>=175 && in->x<=291)return 42;
        if(in->x>=302 && in->x<=372)return 43;
    }
    if(in->y>=328 && in->y<=368 && in->x>=87 && in->x<=379)return 44+(in->x-87)/100;
    if(in->x>=90 && in->x<=378 && in->y>=382 && in->y<=426)return 47;
    return 0;
}
static void slider(int x)
{int volume=(int)lroundf((x-104)*100.0f/258);if(volume<0)volume=0;if(volume>100)volume=100;voice_chat_volume(volume);refresh(1);}
static int audio_input(const nos_input *in)
{
    if(child_back(in))return 1;
    if(!s)return 0;
    if(in->type==IN_DOWN){s->pressed=audio_hit(in);return s->pressed!=0;}
    if(in->type==IN_CANCEL || in->type==IN_UP){s->pressed=0;return 0;}
    if((in->type==IN_DRAG || in->type==IN_DRAG_START) && in->y0>=180 && in->y0<=238 && in->x0>=80 && in->x0<=386) {
        slider(in->x);return 1;
    }
    if(in->type!=IN_TAP)return 0;
    int action=audio_hit(in);s->pressed=0;
    if(action==40)slider(in->x);
    else if(action==41 || action==43) {
        int volume=s->view.volume+(action==41?-10:10);if(volume<0)volume=0;if(volume>100)volume=100;voice_chat_volume(volume);
    }else if(action==42)voice_chat_volume(s->view.volume?0:s->view.last_volume);
    else if(action>=44 && action<=46)voice_chat_audio(s->view.speak,24+(action-44)*6);
    else if(action==47)voice_chat_audio(!s->view.speak,s->view.mic_gain);
    else return 0;
    refresh(1);nos_feedback(FB_TICK);return 1;
}
static int hit(const nos_input *in)
{
    if(in->y>=89 && in->y<=131 && in->x>=89 && in->x<=377)return in->x<233?6:7;
    if(in->y>=132 && in->y<=175 && in->x>=67 && in->x<=399)return 1+(in->x-67)/112;
    if(in->y>=357 && in->y<=415 && in->x>=84 && in->x<=382)return in->x<233?4:5;
    return 0;
}
static int input(const nos_input *in)
{
    if(!s)return 0;
    if(in->type==IN_DOWN){s->pressed=hit(in);s->drag_base=s->offset;return s->pressed!=0;}
    if(in->type==IN_CANCEL || in->type==IN_UP){s->pressed=0;return 0;}
    if(in->type==IN_DRAG && in->axis==AXIS_Y && in->y0>176 && in->y0<300) {
        s->offset=s->drag_base-(int)lroundf(in->dy/24);if(s->offset<0)s->offset=0;if(s->offset>limit())s->offset=limit();return 1;
    }
    if(in->type!=IN_TAP)return 0;
    int action=hit(in);s->pressed=0;
    if(action>=1 && action<=3){if(!voice_chat_tier(action-1))nos_toast("Finish or cancel this turn first");}
    else if(action==4) {
        if(busy())voice_chat_cancel();
        else if(s->view.saved_count==VOICE_SAVED_CHATS)nos_toast("Delete a saved chat in RECENTS first");
        else voice_chat_clear();
    }else if(action==5) {
        if(s->view.state==VOICE_LISTENING)voice_chat_send();
        else if(busy())nos_push(&audio_screen,TX_NONE);
        else voice_chat_start();
    }else if(action==6) {
        if(busy())nos_toast("Finish or cancel this turn first");else nos_push(&history_screen,TX_NONE);
    }else if(action==7)nos_push(&audio_screen,TX_NONE);
    else return 0;
    refresh(1);nos_feedback(FB_TICK);return 1;
}
static const nos_screen history_screen={"SAVED CHATS",history_enter,0,tick,history_draw,history_input,SCR_KEEPAWAKE};
static const nos_screen audio_screen={"CHAT AUDIO",history_enter,0,tick,audio_draw,audio_input,SCR_KEEPAWAKE|SCR_HDRAG};
static const nos_screen delete_screen={"DELETE CHAT",0,0,tick,delete_draw,delete_input,SCR_KEEPAWAKE};
const nos_screen scr_chat={"VOICE CHAT",enter,leave,tick,draw,input,SCR_KEEPAWAKE};
