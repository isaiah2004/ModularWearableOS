/* Radio discovery is asynchronous. Draw only consumes tick-owned snapshots.
 * Lists use the OS scrolling/blur path; password entry never predicts or logs text. */
#include "nos.h"
#include "ui.h"
#include "prefs.h"
#include "diag_log.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const nos_screen networks, keyboard, setup, device;
static struct {
    int bt, count, field, symbols, upper, show;
    uint32_t scan_at;
    ui_list list;
    hal_wifi_ap aps[32];
    hal_ble_dev devs[24];
    char state[96], ssid[33], pass[65], error[64];
    char selected[18], name[24], ap[33], ap_pass[24];
    int setup_active;
} *network_state;
#define N (*network_state)

static void small(gfx_fb *fb,int y,const char *s,uint16_t ink)
{ ui_text(fb,UI_F_SMALL,233,y,s,ink,UI_CENTER,0); }
static void key(gfx_fb *fb,int x,int y,int w,const char *s,int on)
{ ui_button(fb,x,y,w,40,s,on?BTN_PRIMARY:BTN_SECONDARY,0); }
static void fit_text(char *s,const font_t *font,int width)
{
    size_t n=strlen(s);int shortened=0;
    while(n>3 && ui_text_width(font,s,0)>width) {s[--n]=0;shortened=1;}
    if(shortened && n>=3) {s[n-1]='.';s[n-2]='.';s[n-3]='.';}
}
static void scan(void)
{
    N.count=N.bt?hal_ble_scan(N.devs,24):(nos_prefs.wifi?hal_wifi_scan(N.aps,32):0);
    ui_list_set_n(&N.list,4+(N.count?N.count:1));
    N.scan_at=hal_millis();
}
static void row(gfx_fb *fb,int i,int x0,int y,int x1,int h,float fade,int pressed)
{
    (void)h;
    char title[48]="",sub[96]="";
    int tail=2+(N.count?N.count:1);
    if (N.bt) {
        if(i==0) { snprintf(title,sizeof title,"Connect your phone"); snprintf(sub,sizeof sub,"Gadgetbridge > Bangle.js NOS"); }
        else if(i==1) { snprintf(title,sizeof title,"Device connection"); snprintf(sub,sizeof sub,"%.80s",N.state); }
        else if(i==tail) { snprintf(title,sizeof title,"Disconnect accessory"); snprintf(sub,sizeof sub,"Keeps the phone link"); }
        else if(i==tail+1) { snprintf(title,sizeof title,"Refresh nearby BLE"); snprintf(sub,sizeof sub,"BLE only; no audio profiles"); }
        else if(i-2<N.count) {
            hal_ble_dev *d=&N.devs[i-2];
            snprintf(title,sizeof title,"%s",d->name[0]?d->name:"Unnamed device");
            snprintf(sub,sizeof sub,"%s / %d dBm",d->addr,d->rssi);
        } else { snprintf(title,sizeof title,"Looking for devices"); snprintf(sub,sizeof sub,"Wake the device and enable pairing"); }
    } else {
        if(i==0) { snprintf(title,sizeof title,"Wi-Fi %s",nos_prefs.wifi?"on":"off"); snprintf(sub,sizeof sub,"%.80s",N.state); }
        else if(i==1) { snprintf(title,sizeof title,"Phone setup"); snprintf(sub,sizeof sub,"Hotspot, Wi-Fi and watch stats"); }
        else if(i==tail) { snprintf(title,sizeof title,"Refresh networks"); snprintf(sub,sizeof sub,"2.4 GHz networks only"); }
        else if(i==tail+1) { snprintf(title,sizeof title,"Other network"); snprintf(sub,sizeof sub,"Enter a hidden network name"); }
        else if(i-2<N.count) {
            hal_wifi_ap *a=&N.aps[i-2];
            snprintf(title,sizeof title,"%s",a->ssid);
            snprintf(sub,sizeof sub,"%d dBm / channel %d",a->rssi,a->chan);
        } else { snprintf(title,sizeof title,nos_prefs.wifi?"Scanning...":"Wi-Fi is off"); snprintf(sub,sizeof sub,"Turn on Wi-Fi to find networks"); }
    }
    if(pressed) ui_rrect(fb,x0,y+3,x1-x0,76,20,ui_dim(C_SURF2,fade));
    /* Bound labels before drawing so long SSIDs cannot run into the bezel. */
    fit_text(title,UI_F_BODY,x1-x0-16);
    fit_text(sub,UI_F_SMALL,x1-x0-16);
    ui_text(fb,UI_F_BODY,x0+8,y+32,title,ui_dim(C_PRI,fade),UI_LEFT,0);
    ui_text(fb,UI_F_SMALL,x0+8,y+58,sub,ui_dim(C_SEC,fade),UI_LEFT,0);
}
/* Fixed command IDs only: no SSID, address, device name or credential hashes. */
static uint32_t radio_action(uint16_t action, int target, int old)
{
    uint32_t request = diag_action_request(DIAG_DOMAIN_CONTROL, action, target);
    diag_action_context(request, (int32_t)diag_log_name_id("NETWORKS"), old);
    return request;
}
static void tap(int i)
{
    int tail=2+(N.count?N.count:1);
    if(N.bt) {
        if(i==0) { N.selected[0]=0; nos_push(&device,TX_PUSH); }
        else if(i==tail) {
            uint32_t request=radio_action(DIAG_ACT_STOP,0x6102,0);
            hal_ble_disconnect_device(); diag_action_result(request,DIAG_RESULT_PENDING,0);
        }
        else if(i==tail+1) {
            uint32_t request=radio_action(DIAG_ACT_START,0x6103,0);
            scan(); diag_action_result(request,DIAG_RESULT_PENDING,0);
        }
        else if(i>=2 && i-2<N.count) {
            snprintf(N.selected,sizeof N.selected,"%s",N.devs[i-2].addr);
            snprintf(N.name,sizeof N.name,"%s",N.devs[i-2].name);
            nos_push(&device,TX_PUSH);
        }
        return;
    }
    if(i==0) {
        uint32_t request=radio_action(DIAG_ACT_SET,0x6101,nos_prefs.wifi);
        nos_prefs.wifi=!nos_prefs.wifi; prefs_save(); if(!nos_prefs.wifi) hal_setup_set(0); N.scan_at=0;
        diag_action_result(request,DIAG_RESULT_SUCCESS,nos_prefs.wifi); /* RAM policy */
    }
    else if(i==1) nos_push(&setup,TX_PUSH);
    else if(i==tail) {
        uint32_t request=radio_action(DIAG_ACT_START,0x6104,nos_prefs.wifi);
        if(!nos_prefs.wifi) { nos_prefs.wifi=1; prefs_save(); } N.scan_at=0;
        diag_action_result(request,DIAG_RESULT_PENDING,0);
    }
    else if(i==tail+1 || (i>=2 && i-2<N.count)) {
        N.field=i==tail+1 || !strcmp(N.aps[i-2].ssid,"(hidden)")?0:1;
        N.ssid[0]=N.pass[0]=N.error[0]=0; N.symbols=N.upper=N.show=0;
        if(N.field) snprintf(N.ssid,sizeof N.ssid,"%s",N.aps[i-2].ssid);
        nos_push(&keyboard,TX_PUSH);
    }
}
static void enter(void)
{
    ui_list_init(&N.list,5,84,100,row,tap);
    N.scan_at=0;
}
static void tick(float dt)
{
    if(N.bt) hal_ble_device_status(N.state,sizeof N.state);
    else hal_wifi_info(N.state,sizeof N.state);
    /* Don't reorder the item under a finger. */
    if(!N.list.dragging && N.list.pressed<0 &&
       (!N.scan_at || hal_millis()-N.scan_at>3000)) scan();
    ui_list_tick(&N.list,dt);
}
static void draw(gfx_fb *fb)
{
    ui_list_draw(fb,&N.list);
    gfx_fill_rect(fb,0,0,GFX_W,96,C_BG);
    ui_title(fb,N.bt?"BLUETOOTH":"WI-FI",0);
}
static int input(const nos_input *in)
{
    if(in->type==IN_TAP && in->y<96) return 1;
    return ui_list_input(&N.list,in);
}
static const nos_screen networks={"NETWORKS",enter,0,tick,draw,input,0};
void set_connectivity_open(int bt)
{
    if(!network_state) {
        network_state=nos_mem_alloc(sizeof *network_state,"connectivity.ui");
        if(!network_state) return;
        memset(network_state,0,sizeof *network_state);
    }
    N.bt=bt;nos_push(&networks,TX_PUSH);
}

static const char *keys(int row)
{
    static const char *letters[]={"qwertyuiop","asdfghjkl","zxcvbnm"};
    static const char *symbols[]={"1234567890","!@#$%^&*()","-_=+.,/?'"};
    static const char *more[]={"[]{}<>\\|`~",":;\"!@#$%","&*()-_+="};
    return N.symbols==2?more[row]:N.symbols?symbols[row]:letters[row];
}
static void keyboard_leave(void)
{
    N.show=0;
    if(nos_leaving_covered()) return;  /* an alert preserves the in-memory draft */
    memset(N.pass,0,sizeof N.pass);
}
static void keyboard_draw(gfx_fb *fb)
{
    ui_title(fb,N.field?"PASSWORD":"NETWORK NAME",0);
    char value[66]; const char *text=N.field?N.pass:N.ssid;
    size_t n=strlen(text);
    if(N.field && !N.show) { memset(value,'*',n); value[n]=0; }
    else snprintf(value,sizeof value,"%s",text);
    small(fb,108,N.field?N.ssid:"Hidden Wi-Fi network",C_SEC);
    ui_rrect(fb,65,122,336,45,16,C_SURF2);
    small(fb,151,n?value+(n>23?n-23:0):"Tap keys below",C_PRI);
    small(fb,188,N.error[0]?N.error:N.field?"Empty password = open network":"Case-sensitive network name",N.error[0]?C_RED:C_SEC);
    for(int r=0;r<3;r++) {
        const char *k=keys(r); int len=(int)strlen(k),left=233-len*19;
        for(int c=0;c<len;c++) {
            char label[2]={N.upper&&!N.symbols?(char)toupper((unsigned char)k[c]):k[c],0};
            int x=left+c*38+19,y=220+r*46;
            ui_rrect(fb,x-17,y-20,34,40,12,C_SURF2);
            ui_text(fb,UI_F_BODY,x,y+8,label,C_PRI,UI_CENTER,0);
        }
    }
    key(fb,110,360,64,N.symbols?"ABC":"SHIFT",N.upper);
    key(fb,187,360,64,N.symbols==1?"#+=":"123",0);
    key(fb,264,360,64,"SPACE",0);
    key(fb,347,360,76,"DEL",0);
    key(fb,158,411,118,N.field?(N.show?"HIDE":"SHOW"):"BACK",0);
    key(fb,300,411,118,N.field?"CONNECT":"NEXT",1);
}
static int keyboard_input(const nos_input *in)
{
    if(in->type!=IN_TAP) return in->type==IN_DOWN || in->type==IN_UP;
    int x=in->x,y=in->y;
    char *value=N.field?N.pass:N.ssid; size_t cap=N.field?sizeof N.pass:sizeof N.ssid, n=strlen(value);
    if(y>=197 && y<335) {
        int r=(y-197)/46; const char *k=keys(r); int len=(int)strlen(k),left=233-len*19,c=(x-left)/38;
        if(x>=left && c<len && n+1<cap) { value[n]=N.upper&&!N.symbols?(char)toupper((unsigned char)k[c]):k[c]; value[n+1]=0; N.error[0]=0; }
    } else if(y>=340 && y<=380) {
        if(x>=78 && x<142) { if(N.symbols) N.symbols=0; else N.upper=!N.upper; }
        else if(x>=155 && x<219) N.symbols=N.symbols==1?2:1;
        else if(x>=232 && x<296 && n+1<cap) { value[n]=' ';value[n+1]=0; }
        else if(x>=309 && x<385 && n) value[n-1]=0;
    } else if(y>=391 && y<=431) {
        if(x>=99 && x<=217) { if(N.field) N.show=!N.show; else nos_pop(); }
        else if(x>=241 && x<=359) {
            if(!N.field) { if(N.ssid[0]) { N.field=1;N.symbols=0; } }
            else if(n && (n<8 || n>63)) snprintf(N.error,sizeof N.error,"Use 8-63 characters, or leave empty");
            else {
                nos_prefs.wifi=1; prefs_save();
                uint32_t request=radio_action(DIAG_ACT_START,0x6105,0);
                hal_wifi_connect(N.ssid,N.pass);
                diag_action_result(request,DIAG_RESULT_PENDING,0); /* connect is async */
                memset(N.pass,0,sizeof N.pass); N.show=0;
                nos_pop();
            }
        }
    }
    return 1;
}
static const nos_screen keyboard={"WIFI_KEYBOARD",0,keyboard_leave,0,keyboard_draw,keyboard_input,SCR_HDRAG|SCR_KEEPAWAKE};

static void setup_tick(float dt) { (void)dt; hal_setup_info(N.ap,sizeof N.ap,N.ap_pass,sizeof N.ap_pass,&N.setup_active); }
static void setup_enter(void) { N.error[0]=0;setup_tick(0); }
static void setup_draw(gfx_fb *fb)
{
    ui_title(fb,"PHONE SETUP",0);
    small(fb,123,N.setup_active?"Join this hotspot on your phone":"Configure from your phone",C_PRI);
    small(fb,166,N.setup_active?N.ap:"Start a temporary watch hotspot",C_PRI);
    small(fb,206,N.setup_active?N.ap_pass:"Wi-Fi, display settings and stats",C_SEC);
    small(fb,251,N.setup_active?"Open http://192.168.4.1":"Your settings stay on the watch",C_PRI);
    small(fb,290,N.setup_active?"Stay connected without internet":"Hotspot closes after 10 minutes",C_SEC);
    small(fb,326,N.error,C_RED);
    ui_button(fb,233,380,270,64,N.setup_active?"STOP SETUP":"START SETUP",BTN_PRIMARY,0);
}
static int setup_input(const nos_input *in)
{
    if(in->type==IN_TAP && in->x>=98 && in->x<=368 && in->y>=348 && in->y<=412) {
        if(!N.setup_active) { nos_prefs.wifi=1; prefs_save(); }
        uint32_t request=radio_action(N.setup_active?DIAG_ACT_STOP:DIAG_ACT_START,0x6106,N.setup_active);
        int accepted=hal_setup_set(!N.setup_active);
        diag_action_result(request,accepted?DIAG_RESULT_PENDING:DIAG_RESULT_REJECTED,accepted);
        if(!accepted) snprintf(N.error,sizeof N.error,"Could not start; try again");
        else N.error[0]=0;
        setup_tick(0);return 1;
    }
    return 0;
}
static const nos_screen setup={"PHONE_SETUP",setup_enter,0,setup_tick,setup_draw,setup_input,SCR_KEEPAWAKE};

static void device_tick(float dt) { (void)dt; hal_ble_device_status(N.state,sizeof N.state); }
static void device_enter(void) { N.error[0]=0; device_tick(0); }
static void device_draw(gfx_fb *fb)
{
    ui_title(fb,N.selected[0]?"BLE DEVICE":"PHONE LINK",0);
    if(!N.selected[0]) {
        small(fb,150,"Open Gadgetbridge on Android",C_PRI);
        small(fb,200,"Add device > Bangle.js NOS",C_PRI);
        small(fb,250,"Allow notifications and location",C_SEC);
        small(fb,300,"Phone initiates the connection",C_SEC);
    } else {
        small(fb,135,N.name[0]?N.name:"Unnamed BLE device",C_PRI);
        small(fb,176,N.selected,C_SEC);
        small(fb,220,N.state,C_PRI);
        small(fb,265,"BLE connection only",C_SEC);
        small(fb,299,"Device features need a matching app",C_SEC);
        small(fb,330,N.error,C_RED);
        ui_button(fb,233,375,240,64,"CONNECT",BTN_PRIMARY,0);
    }
}
static int device_input(const nos_input *in)
{
    if(in->type==IN_TAP && N.selected[0] && in->x>=113 && in->x<=353 && in->y>=343 && in->y<=407) {
        uint32_t request=radio_action(DIAG_ACT_START,0x6102,0);
        int accepted=hal_ble_connect_device(N.selected);
        diag_action_result(request,accepted?DIAG_RESULT_PENDING:DIAG_RESULT_REJECTED,accepted);
        if(!accepted) snprintf(N.error,sizeof N.error,"Refresh or disconnect current device");
        else N.error[0]=0;
        device_tick(0); return 1;
    }
    return 0;
}
static const nos_screen device={"BLE_DEVICE",device_enter,0,device_tick,device_draw,device_input,0};
