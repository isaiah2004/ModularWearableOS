/* Exercise the actual service owners; only clocks, storage and engine edges are mocked. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../nos/svc.c"
nos_time_t nos_t;
nos_prefs_t nos_prefs;
static uint32_t clock_ms=1000;
static uint64_t clock_epoch=1760000000;
static hal_weather weather;
static int stored_count,save_ok=1,pushes,pops,present;
static const nos_screen *top;
const nos_screen scr_alarm_ring={.name="ALARM"},scr_timer_ring={.name="TIMER"},
    scr_call={.name="CALL"},scr_find_watch={.name="FIND"};
uint32_t hal_millis(void) {return clock_ms;}
uint64_t hal_epoch_seconds(void) {return clock_epoch;}
void hal_weather_get(hal_weather *out) {*out=weather;}
void hal_get_datetime(hal_datetime *out) {*out=(hal_datetime){.year=2026,.month=10,.day=21,.hour=12};}
int hal_store_save_checked(const char *key,const void *data,int size)
{(void)size;assert(!strcmp(key,"nosalarm"));stored_count=((const alarm_saved *)data)->count;return save_ok;}
int hal_phone_connected(void) {return 0;}
void hal_phone_write_line(const char *line) {(void)line;}
const nos_screen *nos_top(void) {return top;}
int nos_contains(const nos_screen *screen) {(void)screen;return present;}
int nos_transitioning(void) {return 0;}
void nos_push(const nos_screen *screen,int kind) {(void)kind;top=screen;pushes++;present=1;}
void nos_pop(void) {top=NULL;pops++;present=0;}
void nos_banner(const char *a,const char *b,int icon,void (*tap)(void)) {(void)a;(void)b;(void)icon;(void)tap;}
void sys_notif_open(int index) {(void)index;}
uint32_t diag_action_request(uint16_t domain,uint16_t action,int32_t target)
{(void)domain;(void)action;(void)target;static uint32_t n;return ++n;}
void diag_action_context(uint32_t r,int32_t s,int32_t v) {(void)r;(void)s;(void)v;}
void diag_action_result(uint32_t r,int32_t s,int32_t d) {(void)r;(void)s;(void)d;}
static void parse(gb_json *json,const char *text) {assert(gb_parse(json,text,(int)strlen(text))==0);}
int main(void)
{
    assert(svc_alarm_add(7,15)==0 && stored_count==1);
    uint32_t first=svc_alarm_id(0),generation=svc_alarm_generation();assert(first);
    svc_alarm_save();assert(svc_alarm_generation()==generation);
    assert(svc_alarm_add(8,30)==1 && stored_count==2);
    uint32_t second=svc_alarm_id(1);assert(second && second!=first);
    svc_alarm_remove(0);assert(svc_alarm_id(0)==second && svc_alarm_find(first)==-1);
    gb_json json;parse(&json,"{\"d\":[{\"h\":8,\"m\":30,\"on\":true}]}");
    generation=svc_alarm_generation();gb_alarms(&json);
    assert(svc_alarm_generation()!=generation && svc_alarm_id(0)!=second && stored_count==1);
    puts("PASS: alarm save includes new count; stable IDs survive shifts and remote replacement invalidates drafts");

    static svc_big notifications;B=&notifications;
    svc_notif_add_local("test","first","body");
    uint32_t revision=svc_notif_revision(),instance=svc_notif_get(0)->instance;
    int id=svc_notif_get(0)->id;
    notif_post("test","updated","changed",id,1,0,0,0);
    assert(svc_notif_revision()!=revision && svc_notif_get(0)->instance==instance);
    revision=svc_notif_revision();parse(&json,"{\"id\":-1,\"body\":\"update in place\"}");gb_notify(&json,1);
    assert(svc_notif_revision()!=revision);
    svc_notif_dismiss(0);notif_post("test","reused ID","new",id,1,0,0,0);
    assert(svc_notif_get(0)->instance!=instance);
    puts("PASS: notification updates revise store; remove/reuse changes lifetime identity");

    weather=(hal_weather){.valid=1,.generation=5,.age_seconds=800,.stale=1,.utc_offset_seconds=19800,.observed_epoch=1759999200};
    snprintf(weather.city,sizeof weather.city,"WiFi location");
    weather.hours[0]=(weather_hour){.epoch=1760000400,.temp=26,.code=0};
    weather.days[0]=(weather_day){.epoch=1759968000,.hi=29,.lo=19};
    wx_from_hal();assert(s_wx.valid && s_wx.source==2 && s_wx.stale && s_wx.generation==5);
    assert(s_wx.hours_epoch[0]==1760000400 && s_wx.utc_offset_seconds==19800);
    weather.valid=0;weather.generation++;weather.fetching=1;
    snprintf(weather.status,sizeof weather.status,"Location changed");wx_from_hal();
    assert(!s_wx.valid && s_wx.fetching && !s_wx.nhours && s_wx.generation==6);
    weather.valid=1;nos_prefs.tz_min=-240;
    parse(&json,"{\"temp\":300,\"hi\":302,\"lo\":290,\"loc\":\"Phone location\"}");gb_weather(&json);
    assert(s_wx.source==1 && !s_wx.nhours && s_wx.ndays==1 && s_wx.utc_offset_seconds==-14400);
    clock_ms+=3600001;wx_tick(clock_ms);assert(s_wx.source==1 && s_wx.stale && s_wx.age_seconds>=3600);
    clock_ms+=7200000;weather.valid=0;wx_tick(clock_ms);assert(!s_wx.valid && s_wx.source==2);
    puts("PASS: location invalidation clears old forecast; UTC epochs/provenance/age survive service; phone data expires");

    ring_scr ring={.s=&scr_alarm_ring};top=NULL;present=0;
    assert(!ring_screen(&ring,1) && pushes==1);
    top=&scr_call;present=1;assert(!ring_screen(&ring,1) && pushes==1);
    assert(!ring_screen(&ring,0) && ring.pushed);
    clock_ms+=3000;assert(!ring_screen(&ring,0) && ring.pushed && pops==0);
    top=&scr_alarm_ring;assert(!ring_screen(&ring,0) && !ring.pushed && pops==1);
    puts("PASS: covered alert retains ownership until revealed and ended; no duplicate push");
    return 0;
}
