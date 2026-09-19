/* Pixel/damage equivalence for cached launcher and app-list geometry.
 * Reference objects are built by tools/test_launcher.ps1 from git 7b5c1b3. */
#include "wos.h"
#include "icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const wos_app app_launcher_ref, app_launcher;
extern const wos_app app_applist_ref, app_applist;

uint32_t wos_frame_id = 1;
static uint32_t now_ms;
uint32_t hal_millis(void) { return now_ms; }

typedef struct { int x0,y0,x1,y1; } rect_t;
static rect_t dirty[2][128];
static int ndirty[2], dirty_side;
void wos_dirty(int x0,int y0,int x1,int y1) {
    if (ndirty[dirty_side] >= 128) { fputs("too many dirty rects\n",stderr); exit(2); }
    dirty[dirty_side][ndirty[dirty_side]++] = (rect_t){x0,y0,x1,y1};
}

static const char *names[24] = {
 "Watchface","Stopwatch","WiFi","Settings","Timer","Alarm","Calculator","Activity",
 "Music","Recorder","BLE","Level","Flashlight","Weather","Voice","Netmon",
 "Calendar","Sensors","World clock","Breathe","Find phone","2048","Battery","Pomodoro"
};
int wos_app_count(void) { return 24; }
const char *wos_app_name(int i) { return names[i]; }
uint16_t wos_app_accent(int i) { static const uint16_t c[]={GFX_ACCENT,GFX_GREEN,GFX_PURPLE,GFX_YELLOW,GFX_ORANGE,GFX_PINK}; return c[i%6]; }
int wos_app_icon(int i) { return 1 + i % (ICON_TRASH); }
int wos_current_app(void) { return 0; }
int wos_app_visible(int i) { (void)i; return 0; }
static int opened[2];
void wos_open_app(int i) { opened[dirty_side]=i; }
void wos_close_overlay(void) {}

static uint16_t *a, *b;
static unsigned checks;
static void fail(const char *which,const char *phase,const char *what,int detail) {
    fprintf(stderr,"FAIL %s %s: %s at %d (frame %u, ms %u)\n",which,phase,what,detail,wos_frame_id,now_ms); exit(1);
}
static void compare_dirty(const char *which,const char *phase) {
    if (ndirty[0]!=ndirty[1]) fail(which,phase,"dirty count",ndirty[1]);
    for(int i=0;i<ndirty[0];i++) if(memcmp(&dirty[0][i],&dirty[1][i],sizeof(rect_t))) fail(which,phase,"dirty rect",i);
}
static void raster_one(const wos_app *app,uint16_t *px,int ox,int oy,int ybase,int h,int cx0,int cx1) {
    size_t n=(size_t)GFX_W*h; for(size_t i=0;i<n;i++) px[i]=(uint16_t)(0x39E7u+(i*17u)%997u);
    gfx_fb fb={px,ox,oy,ybase,h,cx0,cx1,0}; app->on_draw(&fb);
}
static void check_pixels(const char *which,const char *phase,const wos_app *ref,const wos_app *cur,
                         int ox,int oy,int ybase,int h,int cx0,int cx1) {
    raster_one(ref,a,ox,oy,ybase,h,cx0,cx1); raster_one(cur,b,ox,oy,ybase,h,cx0,cx1);
    size_t n=(size_t)GFX_W*h; for(size_t i=0;i<n;i++) if(a[i]!=b[i]) fail(which,phase,"pixel",(int)i);
    checks++;
}
static void frame(const char *which,const char *phase,const wos_app *ref,const wos_app *cur,uint32_t ms) {
    now_ms=ms; wos_frame_id++;
    gfx_rec rr={0},rc={0}; gfx_fb fr={0,0,0,0,GFX_H,0,GFX_W,&rr},fc={0,0,0,0,GFX_H,0,GFX_W,&rc};
    ndirty[0]=ndirty[1]=0; dirty_side=0; ref->on_draw(&fr); dirty_side=1; cur->on_draw(&fc);
    compare_dirty(which,phase);
    check_pixels(which,phase,ref,cur,0,0,0,GFX_H,0,GFX_W);
    /* Same logical frame is rendered repeatedly by the real stripe/rect pipeline. */
    check_pixels(which,phase,ref,cur,0,0,0,37,0,GFX_W);
    check_pixels(which,phase,ref,cur,-19,-11,101,53,27,319);
    check_pixels(which,phase,ref,cur,23,17,389,29,141,466);
}
static void event_pair(const wos_app *r,const wos_app *c,wos_event_type t,int x,int y) {
    wos_event e={t,x,y}; dirty_side=0; int a0=r->on_event(&e); dirty_side=1; int b0=c->on_event(&e);
    if(a0!=b0) fail("event","dispatch","return",t);
}
static void event_one(const wos_app *app,wos_event_type t,int x,int y) {
    wos_event e={t,x,y}; dirty_side=1; (void)app->on_event(&e);
}
static void current_shot(uint16_t *out,uint32_t ms) {
    now_ms=ms; wos_frame_id++;
    gfx_fb fb={out,0,0,0,GFX_H,0,GFX_W,0}; app_launcher.on_draw(&fb);
}
static void require_same(const char *phase,const uint16_t *x,const uint16_t *y) {
    size_t n=(size_t)GFX_W*GFX_H;
    for(size_t i=0;i<n;i++) if(x[i]!=y[i]) fail("angular",phase,"unexpected pixel change",(int)i);
}
static void require_different(const char *phase,const uint16_t *x,const uint16_t *y) {
    size_t n=(size_t)GFX_W*GFX_H;
    for(size_t i=0;i<n;i++) if(x[i]!=y[i]) return;
    fail("angular",phase,"expected pixel change",0);
}
static void require_near(const char *phase,const uint16_t *x,const uint16_t *y,int limit) {
    size_t n=(size_t)GFX_W*GFX_H; int different=0;
    for(size_t i=0;i<n;i++) different += x[i]!=y[i];
    if(different>limit) fail("angular",phase,"too many differing pixels",different);
}
static void start_pair(const wos_app *r,const wos_app *c,uint32_t ms) { now_ms=ms; dirty_side=0;r->on_start();dirty_side=1;c->on_start(); }

static void launcher_case(void) {
    const wos_app *r=&app_launcher_ref,*c=&app_launcher;
    start_pair(r,c,1000);
    const uint32_t enter[]={1000,1060,1199,1320,1580,1800};
    for(unsigned i=0;i<sizeof enter/sizeof enter[0];i++) frame("launcher","entrance",r,c,enter[i]);
    /* Gesture mapping intentionally differs from the reference; animated rendering below
     * reaches states shared by both versions without using drag input. */
    /* Drive each input model clockwise, then let both snap to the exact same non-zero
     * group angle. This retains a baseline raster/damage check at rotated geometry. */
    now_ms=1810; event_one(r,WOS_EV_TOUCH_DOWN,200,80); event_one(c,WOS_EV_TOUCH_DOWN,233,33);
    now_ms=1826; event_one(r,WOS_EV_TOUCH_MOVE,220,80); event_one(c,WOS_EV_TOUCH_MOVE,263,35);
    event_one(r,WOS_EV_TOUCH_UP,220,80); event_one(c,WOS_EV_TOUCH_UP,263,35);
    for(int i=0;i<120;i++){ event_one(r,WOS_EV_TICK,16,0); event_one(c,WOS_EV_TICK,16,0); }
    frame("launcher","rotated snap",r,c,3900);
    /* Every group drill-in; reopening resets all cached geometry/glyph state. */
    for(int k=0;k<5;k++) {
        uint32_t base=3000u+(uint32_t)k*2000u; start_pair(r,c,base); frame("launcher","reopen",r,c,base+700);
        const float pi=3.14159265f,a0=2*pi*k/5; int x=(int)(GFX_CX+GFX_S(68)*gfx_fast_sin(a0)); int y=(int)(GFX_CY-GFX_S(68)*gfx_fast_cos(a0));
        event_pair(r,c,WOS_EV_TAP,x,y);
        const int dt[]={0,80,149,151,260,380,520,720,1000};
        for(unsigned j=0;j<sizeof dt/sizeof dt[0];j++) frame("launcher","drill",r,c,base+710+dt[j]);
        event_pair(r,c,WOS_EV_TAP,(int)GFX_CX,(int)GFX_CY); frame("launcher","back",r,c,base+1900);
    }
}

static void angular_input_case(void) {
    const int cx=(int)GFX_CX,cy=(int)GFX_CY;
    uint16_t *p=(uint16_t*)malloc((size_t)GFX_W*GFX_H*sizeof *p);
    uint16_t *q=(uint16_t*)malloc((size_t)GFX_W*GFX_H*sizeof *q);
    if(!p||!q) exit(2);

    /* Radial travel at a fixed angle carries no dial rotation. */
    now_ms=30000; app_launcher.on_start(); current_shot(p,30700);
    event_one(&app_launcher,WOS_EV_TOUCH_DOWN,cx,cy-80);
    now_ms=30716; event_one(&app_launcher,WOS_EV_TOUCH_MOVE,cx,cy-170); current_shot(q,30716);
    require_same("same-angle radial move",p,q);

    /* Clockwise arc travel rotates even when horizontal displacement is small. */
    now_ms=31000; app_launcher.on_start(); current_shot(p,31700);
    event_one(&app_launcher,WOS_EV_TOUCH_DOWN,cx,cy-150);
    now_ms=31716; event_one(&app_launcher,WOS_EV_TOUCH_MOVE,cx+106,cy-106); current_shot(q,31716);
    require_different("circular drag",p,q);

    /* Crossing atan2's +/-pi seam must unwrap to a short move. Compare the seam
     * path with the same endpoints expressed away from the seam. */
    now_ms=32000; app_launcher.on_start(); current_shot(p,32700);
    event_one(&app_launcher,WOS_EV_TOUCH_DOWN,cx+6,cy+150);
    now_ms=32716; event_one(&app_launcher,WOS_EV_TOUCH_MOVE,cx-6,cy+150);
    event_one(&app_launcher,WOS_EV_TOUCH_UP,cx-6,cy+150); event_one(&app_launcher,WOS_EV_TICK,16,0); current_shot(p,32732);
    now_ms=33000; app_launcher.on_start(); current_shot(q,33700);
    event_one(&app_launcher,WOS_EV_TOUCH_DOWN,cx,cy-150);
    now_ms=33716; event_one(&app_launcher,WOS_EV_TOUCH_MOVE,cx+12,cy-150);
    event_one(&app_launcher,WOS_EV_TOUCH_UP,cx+12,cy-150); event_one(&app_launcher,WOS_EV_TICK,16,0); current_shot(q,33732);
    require_near("angle wrap and bounded release",p,q,3000);

    /* The centre deadzone drops the old anchor: leaving it reanchors, and only
     * the following outer sample rotates the dial. */
    now_ms=34000; app_launcher.on_start(); current_shot(p,34700);
    event_one(&app_launcher,WOS_EV_TOUCH_DOWN,cx,cy-120);
    now_ms=34716; event_one(&app_launcher,WOS_EV_TOUCH_MOVE,cx+5,cy+5);
    now_ms=34732; event_one(&app_launcher,WOS_EV_TOUCH_MOVE,cx+120,cy); current_shot(q,34732);
    require_same("centre deadzone reanchor",p,q);
    now_ms=34748; event_one(&app_launcher,WOS_EV_TOUCH_MOVE,cx,cy+120); current_shot(q,34748);
    require_different("post-deadzone arc",p,q);

    /* Tap hit testing remains independent of drag-angle state. */
    now_ms=35000; opened[1]=-1; app_launcher.on_start();
    event_one(&app_launcher,WOS_EV_TAP,cx,cy-(int)GFX_S(68));
    if(opened[1]!=-1) fail("angular","group tap","opened app too early",opened[1]);
    current_shot(p,36000); event_one(&app_launcher,WOS_EV_TAP,cx,cy-(int)GFX_S(76));
    if(opened[1]!=0) fail("angular","app tap","wrong app",opened[1]);
    free(p);free(q);
}

static void applist_case(void) {
    const wos_app *r=&app_applist_ref,*c=&app_applist; start_pair(r,c,20000);
    const uint32_t enter[]={20000,20069,20070,20122,20280,20550,21000};
    for(unsigned i=0;i<sizeof enter/sizeof enter[0];i++) frame("applist","entrance",r,c,enter[i]);
    event_pair(r,c,WOS_EV_TOUCH_DOWN,200,360);
    for(int i=0;i<8;i++){now_ms=21016+i*16;event_pair(r,c,WOS_EV_TOUCH_MOVE,200,330-i*38);frame("applist","drag/focus",r,c,now_ms);}
    event_pair(r,c,WOS_EV_TOUCH_UP,200,64);
    for(int i=0;i<35;i++){event_pair(r,c,WOS_EV_TICK,16,0);frame("applist","inertia/snap",r,c,21160+i*16);}
    start_pair(r,c,23000); frame("applist","reopen",r,c,23700);
}

int main(void) {
    a=(uint16_t*)malloc((size_t)GFX_W*GFX_H*sizeof *a); b=(uint16_t*)malloc((size_t)GFX_W*GFX_H*sizeof *b);
    if(!a||!b) return 2;
    launcher_case(); applist_case(); angular_input_case();
    printf("launcher renders: %u baseline/current pixel+dirty comparisons passed; angular drag/wrap/deadzone/release/tap passed (reference 7b5c1b3)\n",checks);
    free(a);free(b); return 0;
}
