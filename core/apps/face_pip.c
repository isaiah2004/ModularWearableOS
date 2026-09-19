/* Expressive eyes with a deterministic behavior model. Pose advances once per
 * frame; fixed drawing topology keeps striped damage rendering predictable. */
#include "apps.h"
#include "../font.h"
#include "../pip.h"
#include "../pip_care.h"
#include "../notif.h"
#include "../settings.h"
#include "pip_eye_shape.h"
#include "pip_eye_lid.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#define OFF (-GFX_S(300))
static pip_model s_pet;
static pip_care_model s_care;
static hal_power s_power;
static hal_activity s_activity;
static uint32_t s_frame=0xffffffffu, s_save_ms, s_service_ms, s_pet_ms, s_alarm_sequence;
static uint32_t s_history_head, s_episode_ms;
static int s_episode_pending;
static int s_ready, s_display=1, s_care_page, s_save_failed;
static pip_care_input care_input(void)
{
    hal_datetime t;hal_get_datetime(&t);
    hal_power_get(&s_power);hal_activity_get(&s_activity);
    uint64_t epoch=hal_epoch_seconds();
    pip_care_input in={.now_ms=hal_millis(),.epoch_s=epoch,.clock_valid=epoch!=0,
        .local_hour=t.hour,.local_minute=t.minute,.local_day=epoch ? (uint32_t)(t.year*10000+t.month*100+t.day) : 0,
        .battery_percent=s_power.battery_percent,.battery_valid=s_power.battery_valid,
        .external_power=s_power.external_power,.active_seconds=s_activity.active_seconds,
        .steps=s_activity.steps,.activity_valid=s_activity.valid,.walking=s_activity.walking,
        .visible=s_display && g_saved_face==1 && wos_app_visible(0)};
    return in;
}
static void init(void)
{
    if(s_ready) return;
    pip_memory saved;
    int loaded=hal_store_load("pip_memory",&saved,sizeof saved);
    pip_init(&s_pet,loaded ? &saved : NULL,hal_millis());
    pip_care_memory a,b;
    int va=hal_store_load("pip_care_a",&a,sizeof a)&&pip_care_memory_valid(&a);
    int vb=hal_store_load("pip_care_b",&b,sizeof b)&&pip_care_memory_valid(&b);
    const pip_care_memory *best=va ? &a : NULL;
    if(vb && (!va || (int32_t)(b.generation-a.generation)>0))best=&b;
    pip_care_input in=care_input();pip_care_init(&s_care,best,&in);
    s_care.have_notification=1;s_care.notification_sequence=notif_sequence();
    s_history_head=s_care.memory.history_head;
    s_save_ms=hal_millis(); s_ready=1;
}
void face_pip_display(int on) { s_display=on!=0; }
void face_pip_alarm(void)
{ init();pip_care_alarm(&s_care,++s_alarm_sequence,hal_millis()); }
static void context(void)
{
    const pip_care_output *o=&s_care.output;
    float valence=((float)o->social+o->stimulation+o->comfort)/1500.0f-1;
    float grumpy=o->mood==PIP_CARE_GRUMPY ? fmaxf(.65f,o->intensity/1000.0f) : 0;
    if(o->mood==PIP_CARE_LONELY)valence=-.65f;
    if(o->mood>=PIP_CARE_HAPPY)valence=.7f;
    pip_set_context(&s_pet,o->energy/1000.0f,valence,o->social/1000.0f,o->rest/1000.0f,grumpy);
    pip_set_quiet(&s_pet,o->quiet);
}
static void care_reaction(uint32_t now)
{
    if(!s_care.input.visible || s_pet.down || now-s_pet.last_attention<1200 ||
       ((s_pet.action==PIP_STARTLED||s_pet.action==PIP_DIZZY)&&now-s_pet.action_ms<s_pet.duration_ms))return;
    if((s_pet.action==PIP_PETTED||s_pet.action==PIP_BOOP||s_pet.action==PIP_HAPPY||s_pet.action==PIP_PLAY||
        s_pet.action==PIP_RELIEVED||s_pet.action==PIP_EXCITED) &&
       now-s_pet.action_ms<s_pet.duration_ms)return;
    pip_care_event ev;
    if(!pip_care_take_event(&s_care,now,&ev))return;
    const char *action=NULL;
    switch(ev.type) {
    case PIP_CARE_EVENT_GREETING:action=s_care.output.mood==PIP_CARE_GRUMPY?"grumpy":"wake";break;
    case PIP_CARE_EVENT_LOW_POWER:action="sleepy";wos_callout("Pip needs power",2200);break;
    case PIP_CARE_EVENT_OVERDUE:action="grumpy";break;
    case PIP_CARE_EVENT_POWER_RELIEF:action="relieved";break;
    case PIP_CARE_EVENT_WALK:action="excited";break;
    case PIP_CARE_EVENT_NOTIFICATION:if(!s_care.output.quiet)action="notify";break;
    case PIP_CARE_EVENT_ALARM:action="wake";break;
    default:break; /* touch/play already have immediate raw-pointer performances */
    }
    if(action)pip_force(&s_pet,action,now);
}
void face_pip_service(int save_now)
{
    init();
    uint32_t now=hal_millis();
    char app[20],title[40];
    for(int i=0;i<8 && hal_notification_poll(app,sizeof app,title,sizeof title);i++)
        notif_push(app,title,GFX_ACCENT);
    if(save_now || now-s_service_ms>=1000) {
        pip_care_input in=care_input();pip_care_step(&s_care,&in);
        pip_care_notify(&s_care,notif_sequence(),now);
        context();care_reaction(now);s_service_ms=now;
    }
    if(s_care.memory.history_head!=s_history_head) {
        s_history_head=s_care.memory.history_head;s_episode_ms=now;s_episode_pending=1;
    }
    int episode_settled=s_episode_pending && now-s_episode_ms>=15000 && now-s_save_ms>=30000;
    if((s_pet.dirty||s_care.dirty) && (save_now || now-s_save_ms>=300000 || episode_settled)) {
        s_save_failed=0;
        if(s_pet.dirty) {
            if(hal_store_save_checked("pip_memory",&s_pet.memory,sizeof s_pet.memory))s_pet.dirty=0;
            else s_save_failed=1;
        }
        if(s_care.dirty) {
            pip_care_memory snapshot;pip_care_snapshot(&s_care,&snapshot);
            const char *key=snapshot.generation&1 ? "pip_care_a" : "pip_care_b";
            if(hal_store_save_checked(key,&snapshot,sizeof snapshot))pip_care_saved(&s_care);
            else s_save_failed=1;
        }
        if(s_save_failed)hal_log("Pip memory save failed; will retry");
        s_episode_pending=0;
        s_save_ms=now;
    }
}
static void latch(const hal_datetime *t)
{
    init();
    if(s_frame==wos_frame_id) return;
    s_frame=wos_frame_id;
    pip_input in={.now=hal_millis(),.hour=t->hour,.battery=hal_battery_percent()};
    in.imu_valid=hal_imu_read(&in.imu);
    pip_step(&s_pet,&in);
}
static void blob(gfx_fb *fb,float x,float y,float hw,float hh,uint16_t c)
{
    pip_eye_blob(fb,x,y,hw,hh,c);
}
static void eye(gfx_fb *fb,int side)
{
    const pip_pose *p=&s_pet.pose;
    typedef struct {
        uint32_t frame;
        int ready;
        float x,y,w,h,upper,lower,shine_x,dizzy_x,dizzy_y;
        uint16_t halo,color,shine;
    } eye_layout;
    static eye_layout cached[2];
    eye_layout *e=&cached[side>0];
    if(!e->ready || e->frame!=wos_frame_id) {
        e->ready=1;e->frame=wos_frame_id;
        e->x=GFX_CX+GFX_S(side*38+p->x);e->y=GFX_S(128+p->y+side*(p->lean+p->asymmetry*2));
        e->w=GFX_S(p->width);e->h=GFX_S(p->height*(1+side*p->asymmetry*.03f));
        e->upper=fmaxf(p->blink,fminf(1,p->upper+(side==p->wink_side ? p->wink : 0)));
        e->lower=fmaxf(p->blink*.35f,fminf(.85f,p->lower));
        float g=p->glow;
        e->halo=GFX_RGB((int)(9*g),(int)(48*g),(int)(57*g));
        e->color=GFX_RGB((int)((58+32*p->joy)*g),(int)(235*g),(int)((228-25*p->joy)*g));
        e->shine=GFX_RGB((int)(125*g),(int)(255*g),(int)(245*g));
        float shine=fmaxf(0,1-fmaxf(e->upper,e->lower)*2);
        e->shine_x=shine>.05f ? e->x-e->w*.46f-GFX_S(p->x*.1f) : OFF;
        float phase=(s_pet.last_ms-s_pet.action_ms)/1000.0f*7+side;
        e->dizzy_x=p->dizzy>.08f ? e->x+GFX_S(cosf(phase)*7) : OFF;
        e->dizzy_y=e->y+GFX_S(sinf(phase)*5);
    }
    float x=e->x,y=e->y,w=e->w,h=e->h,upper=e->upper,lower=e->lower;
    /* Black lid masks only affect this eye. Limit both raster work and damage
     * to its halo, instead of repainting the masks' empty background below it.
     * Keep all command slots, including invisible ones, for stable diffing. */
    int x0=pip_floor(x-w-GFX_S(4)-1)+fb->ox,x1=pip_ceil(x+w+GFX_S(4)+1)+fb->ox;
    int y0=pip_floor(y-h-GFX_S(4)-1)+fb->oy,y1=pip_ceil(y+h+GFX_S(4)+1)+fb->oy;
    gfx_fb cropped=*fb;
    int rec_start=fb->rec ? fb->rec->n : 0;
    if(!fb->rec) {
        int c0=fb->cx0,c1=fb->cx1;if(c1<=c0){c0=0;c1=GFX_W;}
        cropped.cx0=x0>c0?x0:c0;cropped.cx1=x1<c1?x1:c1;
        int start=y0-fb->y_base,end=y1-fb->y_base;
        if(start<0)start=0;
        if(end>fb->h)end=fb->h;
        if(end<=start || cropped.cx1<=cropped.cx0)return;
        cropped.px+=start*GFX_W;cropped.y_base+=start;cropped.h=end-start;
        fb=&cropped;
    }
    blob(fb,x,y,w+GFX_S(4),h+GFX_S(4),e->halo); blob(fb,x,y,w,h,e->color);
    gfx_fill_circle(fb,e->shine_x,y-h*.52f,GFX_S(2.2f),e->shine);
    float pad=GFX_S(5);
    gfx_fill_rect(fb,x-w-pad,y-h-pad,2*(w+pad),upper*(2*h+pad),GFX_BLACK);
    float lid=lower*(2*h+pad);
    gfx_fill_rect(fb,x-w-pad,y+h+pad-lid,2*(w+pad),lid,GFX_BLACK);
    gfx_fill_circle(fb,lower>.1f ? x : OFF,y+h+GFX_S(22)-lower*GFX_S(12),w*1.02f,GFX_BLACK);
    pip_eye_lid(fb,x,y,w,h,p->anger,side);
    gfx_ring(fb,e->dizzy_x,e->dizzy_y,GFX_S(7),GFX_S(2),GFX_BLACK);
    if(fb->rec)for(int i=rec_start;i<fb->rec->n && i<GFX_MAX_CMDS;i++) {
        gfx_cmd *c=&fb->rec->c[i];
        if(c->x0<x0)c->x0=x0;
        if(c->y0<y0)c->y0=y0;
        if(c->x1>x1)c->x1=x1;
        if(c->y1>y1)c->y1=y1;
        if(c->x1<=c->x0 || c->y1<=c->y0)c->x0=c->y0=c->x1=c->y1=0;
    }
}
static void chrome(gfx_fb *fb,const hal_datetime *t)
{
    gfx_clear(fb,GFX_BLACK);
    int baseline=(int)GFX_S(30);
    if(!fb->rec && (baseline-font_medium.ascent-1+fb->oy>=fb->y_base+fb->h ||
        baseline+font_medium.line_height-font_medium.ascent+1+fb->oy<=fb->y_base))return;
    static char time[8];static int minute=-1;
    int key=t->hour*60+t->minute;
    if(key!=minute){minute=key;snprintf(time,sizeof time,"%02d:%02d",t->hour,t->minute);}
    font_draw_centered(fb,&font_medium,GFX_CX,baseline,time,GFX_RGB(148,158,166));
}
void face_pip_draw(gfx_fb *fb,const hal_datetime *t)
{
    latch(t);
    /* A black background needs no full-screen PSRAM cache. The clock's own
     * damage command also avoids a full-panel repaint at each minute change. */
    chrome(fb,t);
    eye(fb,-1);eye(fb,1);
    const pip_pose *p=&s_pet.pose;
    typedef struct { float x,y,r; } particle;
    static struct {
        uint32_t frame;int ready;
        float bx,by,surprise[2];particle joy[3],sleep[2];
    } fx;
    if(!fx.ready || fx.frame!=wos_frame_id) {
        fx.ready=1;fx.frame=wos_frame_id;
        fx.bx=p->ball_visible>.1f ? GFX_S(120+p->ball_x) : OFF;fx.by=GFX_S(128+p->ball_y);
        for(int i=0;i<3;i++) {
            float phase=fmodf(s_pet.last_ms/1500.0f+i*.34f,1);
            fx.joy[i]=(particle){p->joy>.25f ? GFX_S(80+i*40+3*sinf(phase*6)) : OFF,
                GFX_S(191-phase*14),GFX_S(1.5f+1.2f*sinf(phase*3.14159f))};
        }
        for(int i=0;i<2;i++) {
            float phase=fmodf(s_pet.last_ms/2600.0f+i*.5f,1);
            fx.sleep[i]=(particle){p->sleep>.5f ? GFX_S(177+phase*8) : OFF,
                GFX_S(95-phase*20),GFX_S(2+phase*3)};
            fx.surprise[i]=p->surprise>.25f ? GFX_S(38+i*164) : OFF;
        }
    }
    gfx_fill_circle(fb,fx.bx,fx.by,GFX_S(6),GFX_RGB(13,54,59));
    gfx_fill_circle(fb,fx.bx,fx.by,GFX_S(3.5f),GFX_RGB(193,252,240));
    for(int i=0;i<3;i++) {
        particle *v=&fx.joy[i];
        gfx_fill_circle(fb,v->x,v->y,v->r,GFX_RGB(85,168,150));
    }
    for(int i=0;i<2;i++) {
        particle *v=&fx.sleep[i];
        gfx_ring(fb,v->x,v->y,v->r,GFX_S(1),GFX_RGB(53,93,116));
        float x=fx.surprise[i];
        gfx_line(fb,x,GFX_S(98),x+GFX_S(i ? 5 : -5),GFX_S(90),GFX_S(2),GFX_RGB(140,229,235));
    }
    int baseline=(int)GFX_S(211);
    if(!fb->rec && (baseline-font_small.ascent-1+fb->oy>=fb->y_base+fb->h ||
        baseline+font_small.line_height-font_small.ascent+1+fb->oy<=fb->y_base))return;
    const char *cue="PIP  /  CARE";
    switch(s_care.output.reason) {
    case PIP_CARE_REASON_SOCIAL:cue="I MISSED YOU";break;
    case PIP_CARE_REASON_POWER:cue=s_care.output.mood==PIP_CARE_RELIEVED?"THAT'S BETTER":
        s_care.output.charge_overdue?"STILL NEED POWER":"A LITTLE LOW";break;
    case PIP_CARE_REASON_REST:cue="GETTING SLEEPY";break;
    case PIP_CARE_REASON_STIMULATION:cue="LET'S GO EXPLORE";break;
    case PIP_CARE_REASON_WALK:cue="THAT WAS FUN";break;
    case PIP_CARE_REASON_TOUCH:cue="MORE OF THAT";break;
    case PIP_CARE_REASON_REUNION:cue="THERE YOU ARE";break;
    default:break;
    }
    font_draw_centered(fb,&font_small,GFX_CX,baseline,cue,GFX_RGB(74,119,124));
}
int face_pip_event(const wos_event *ev)
{
    init();uint32_t now=hal_millis();
    float x=ev->x*(240.0f/GFX_W),y=ev->y*(240.0f/GFX_H);
    switch(ev->type) {
    case WOS_EV_TOUCH_DOWN:pip_pointer(&s_pet,1,x,y,now);return 1;
    case WOS_EV_TOUCH_MOVE:
        pip_pointer(&s_pet,2,x,y,now);
        if(s_pet.stroke && now-s_pet_ms>=1000){pip_care_pet(&s_care,now);s_pet_ms=now;}
        return 1;
    case WOS_EV_TOUCH_UP:pip_pointer(&s_pet,0,x,y,now);return 1;
    case WOS_EV_TAP:
        if(!s_pet.stroke) {
            if(y>185 && y<222){face_pip_command("care");return 1;}
            pip_tap(&s_pet,now);
            if(s_pet.action==PIP_PLAY)pip_care_play(&s_care,now);
            else pip_care_greet(&s_care,now);
        }
        return 1;
    case WOS_EV_LONG_PRESS:pip_hold(&s_pet,now);pip_care_pet(&s_care,now);return 1;
    case WOS_EV_SWIPE_LEFT:case WOS_EV_SWIPE_RIGHT:case WOS_EV_SWIPE_UP:case WOS_EV_SWIPE_DOWN:
        return s_pet.stroke;
    default:return 0;
    }
}
int face_pip_command(const char *cmd)
{
    init();
    if(!strcmp(cmd,"center")){pip_center(&s_pet);return 1;}
    if(!strcmp(cmd,"save")){face_pip_service(1);return 1;}
    if(!strcmp(cmd,"care")) {
        for(int i=0;i<wos_app_count();i++)if(!strcmp(wos_app_name(i),"Pet care")){wos_open_app(i);return 1;}
        return 0;
    }
    if(!strcmp(cmd,"hello")){pip_care_greet(&s_care,hal_millis());return pip_force(&s_pet,"wake",hal_millis());}
    if(!strcmp(cmd,"pet")){pip_hold(&s_pet,hal_millis());pip_care_pet(&s_care,hal_millis());return 1;}
    if(!strcmp(cmd,"play")){pip_care_play(&s_care,hal_millis());return pip_force(&s_pet,"play",hal_millis());}
    int wake,bed;char extra;
    if(sscanf(cmd,"routine %d %d %c",&wake,&bed,&extra)==2) {
        if(wake<0||wake>23||bed<0||bed>23||wake==bed)return 0;
        s_care.memory.awake_start_hour=(uint32_t)wake;s_care.memory.awake_end_hour=(uint32_t)bed;
        s_care.dirty=1;face_pip_service(1);return !s_save_failed;
    }
    if(!strcmp(cmd,"live"))return pip_force(&s_pet,"idle",hal_millis());
    if(!strncmp(cmd,"demo ",5))return pip_force(&s_pet,cmd+5,hal_millis());
    return 0;
}
const char *face_pip_debug(char *buf,int n)
{
    init();
    snprintf(buf,n,"state=%s comfort=%.2f energy=%.2f curiosity=%.2f arousal=%.2f familiar=%lu imu=%d motion=%.2f stroke=%d down=%d mood=%s reason=%s social=%u stimulation=%u rest=%u care_comfort=%u care_energy=%u request=%lu wait_ms=%lu walks=%lu active_s=%lu power=%d battery_valid=%d quiet=%d routine=%lu-%lu memory_gen=%lu save_error=%d notify_seq=%lu notifications=%d",
        pip_action_name(s_pet.action),(double)s_pet.comfort,(double)s_pet.energy,
        (double)s_pet.curiosity,(double)s_pet.arousal,(unsigned long)s_pet.memory.familiarity,
        s_pet.imu_valid,(double)s_pet.motion,s_pet.stroke,s_pet.down,
        pip_care_mood_name(s_care.output.mood),pip_care_reason_name(s_care.output.reason),
        (unsigned)s_care.output.social,(unsigned)s_care.output.stimulation,(unsigned)s_care.output.rest,
        (unsigned)s_care.output.comfort,(unsigned)s_care.output.energy,
        (unsigned long)s_care.memory.request_state,(unsigned long)s_care.memory.request_observed_ms,
        (unsigned long)s_care.memory.walk_rewards,(unsigned long)s_activity.active_seconds,
        s_power.external_power,s_power.battery_valid,s_care.output.quiet,
        (unsigned long)s_care.memory.awake_start_hour,(unsigned long)s_care.memory.awake_end_hour,
        (unsigned long)s_care.memory.generation,s_save_failed,(unsigned long)notif_sequence(),notif_count());
    return buf;
}

static const char *care_phrase(pip_care_reason reason)
{
    switch(reason) {
    case PIP_CARE_REASON_SOCIAL:return "I could use some company";
    case PIP_CARE_REASON_POWER:return s_care.output.mood==PIP_CARE_RELIEVED ? "That feels better already" :
        s_care.output.charge_overdue ? "I asked for a little power" : "A little power would help";
    case PIP_CARE_REASON_REST:return "Time to take it easy";
    case PIP_CARE_REASON_STIMULATION:return "Let's explore together";
    case PIP_CARE_REASON_TOUCH:return "I love spending time with you";
    case PIP_CARE_REASON_WALK:return "That was a lovely outing";
    case PIP_CARE_REASON_REUNION:return "It's good to see you";
    case PIP_CARE_REASON_NOTIFICATION:return "Something arrived";
    case PIP_CARE_REASON_ALARM:return "Time to wake up";
    default:return "Happy here with you";
    }
}
static void care_text(gfx_fb *fb,int y,const char *text,uint16_t color)
{ font_draw_centered(fb,&font_small,GFX_CX,GFX_S(y),text,color); }
static void care_meter(gfx_fb *fb,int y,const char *label,unsigned value,uint16_t color)
{
    font_draw(fb,&font_small,GFX_S(38),GFX_S(y),label,GFX_GREY);
    gfx_fill_rect(fb,GFX_S(107),GFX_S(y-7),GFX_S(70),GFX_S(5),GFX_DGREY);
    gfx_fill_rect(fb,GFX_S(107),GFX_S(y-7),GFX_S(70)*value/1000,GFX_S(5),color);
    char text[8];snprintf(text,sizeof text,"%u",value/10);
    font_draw(fb,&font_small,GFX_S(186),GFX_S(y),text,GFX_WHITE);
}
static void care_start(void) { init();s_care_page=0; }
static void care_draw(gfx_fb *fb)
{
    gfx_clear(fb,GFX_BLACK);
    const pip_care_output *o=&s_care.output;
    char text[80];
    const char *title=s_care_page==0?"PIP'S WORLD":s_care_page==1?"OUR ROUTINE":"OUR MEMORIES";
    font_draw_centered(fb,&font_medium,GFX_CX,GFX_S(43),title,GFX_RGB(105,221,212));
    if(s_care_page==0) {
        static const char *moods[]={"Content","A little grumpy","Missing you","Sleepy","Happy","Excited","Relieved"};
        font_draw_centered(fb,&font_medium,GFX_CX,GFX_S(70),moods[o->mood],GFX_WHITE);
        care_text(fb,88,care_phrase(o->reason),GFX_GREY);
        care_meter(fb,113,"Company",o->social,GFX_PINK);
        care_meter(fb,134,"Play",o->stimulation,GFX_ACCENT);
        care_meter(fb,155,"Rest",o->rest,GFX_PURPLE);
        care_meter(fb,176,"Comfort",o->comfort,GFX_GREEN);
        if(s_power.battery_valid)snprintf(text,sizeof text,"Energy %u%% / estimated",(unsigned)o->energy/10);
        else snprintf(text,sizeof text,"%s",s_power.external_power==1?"On power / battery unknown":"Battery estimate unavailable");
        care_text(fb,199,text,GFX_GREY);
        care_text(fb,219,"TAP TO SAY HI",GFX_RGB(80,150,150));
    } else if(s_care_page==1) {
        care_text(fb,66,"Tap left or right to adjust",GFX_GREY);
        gfx_fill_rect(fb,GFX_S(43),GFX_S(80),GFX_S(154),GFX_S(38),GFX_RGB(13,27,30));
        snprintf(text,sizeof text,"<  WAKE  %02lu:00  >",(unsigned long)s_care.memory.awake_start_hour);
        care_text(fb,105,text,GFX_WHITE);
        gfx_fill_rect(fb,GFX_S(43),GFX_S(128),GFX_S(154),GFX_S(38),GFX_RGB(13,27,30));
        snprintf(text,sizeof text,"<  REST  %02lu:00  >",(unsigned long)s_care.memory.awake_end_hour);
        care_text(fb,153,text,GFX_WHITE);
        care_text(fb,186,"Quiet nights, warm greetings",GFX_GREY);
        care_text(fb,204,"Power request: 30 min",GFX_GREY);
        care_text(fb,222,"SWIPE PAGES",GFX_RGB(80,150,150));
    } else {
        snprintf(text,sizeof text,"%lu shared outings",(unsigned long)s_care.memory.walk_rewards);
        care_text(fb,70,text,GFX_WHITE);
        snprintf(text,sizeof text,"%lu active min / estimate",(unsigned long)s_activity.active_seconds/60);
        care_text(fb,89,text,GFX_GREY);
        if(!s_care.memory.history_count)care_text(fb,132,"Our story starts here",GFX_GREY);
        for(unsigned i=0;i<s_care.memory.history_count && i<PIP_CARE_HISTORY_COUNT;i++) {
            unsigned index=(s_care.memory.history_head+PIP_CARE_HISTORY_COUNT-1-i)%PIP_CARE_HISTORY_COUNT;
            const pip_care_history *event=&s_care.memory.history[index];
            const char *label;
            switch(event->event) {
            case PIP_CARE_EVENT_GREETING:label="A hello together";break;
            case PIP_CARE_EVENT_PETTED:label="A lovely head rub";break;
            case PIP_CARE_EVENT_PLAY:label="A moment of play";break;
            case PIP_CARE_EVENT_LOW_POWER:label="Asked for some power";break;
            case PIP_CARE_EVENT_OVERDUE:label="Waited for power";break;
            case PIP_CARE_EVENT_POWER_RELIEF:label="Power, at last";break;
            case PIP_CARE_EVENT_WALK:label="Explored together";break;
            case PIP_CARE_EVENT_ALARM:label="Woke for an alarm";break;
            default:label="A little moment";break;
            }
            care_text(fb,117+(int)i*22,label,i ? GFX_GREY:GFX_RGB(128,224,204));
        }
        care_text(fb,218,s_save_failed?"MEMORY SAVE PENDING":"SWIPE PAGES",GFX_RGB(80,150,150));
    }
    for(int i=0;i<3;i++)gfx_fill_circle(fb,GFX_S(111+i*9),GFX_S(233),GFX_S(1.8f),i==s_care_page?GFX_ACCENT:GFX_DGREY);
}
static int care_event(const wos_event *ev)
{
    if(ev->type==WOS_EV_SWIPE_LEFT){s_care_page=(s_care_page+1)%3;return 1;}
    if(ev->type==WOS_EV_SWIPE_RIGHT){s_care_page=(s_care_page+2)%3;return 1;}
    if(ev->type!=WOS_EV_TAP)return 0;
    int x=(int)(ev->x*240.0f/GFX_W),y=(int)(ev->y*240.0f/GFX_H);
    if(s_care_page==0 && y>190){face_pip_command("hello");g_saved_face=1;settings_save();wos_open_app(0);return 1;}
    if(s_care_page==1) {
        int wake=(int)s_care.memory.awake_start_hour,bed=(int)s_care.memory.awake_end_hour;
        int delta=x<120?-1:1;
        if(y>=80 && y<=118)wake=(wake+delta+24)%24;
        else if(y>=128 && y<=166)bed=(bed+delta+24)%24;
        else return 1;
        if(wake!=bed){char cmd[32];snprintf(cmd,sizeof cmd,"routine %d %d",wake,bed);face_pip_command(cmd);}
    }
    return 1;
}
const wos_app app_pet_care={"Pet care",care_start,care_event,care_draw,GFX_RGB(105,221,212),ICON_ACTIVITY};
