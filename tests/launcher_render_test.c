/* Pixel/damage equivalence for cached launcher and app-list geometry.
 * Reference objects are built by tools/test_launcher.ps1 from git 7b5c1b3. */
#include "wos.h"
#include "icons.h"
#include "pathtext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const wos_app app_launcher_ref, app_launcher;
extern const wos_app app_applist_ref, app_applist;
extern int launcher_label_dbg_renders;

uint32_t wos_frame_id = 1;
static uint32_t now_ms;
uint32_t hal_millis(void) { return now_ms; }
void *hal_big_alloc(uint32_t bytes) { return malloc(bytes); }

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
    for(int i=0;i<ndirty[0];i++) if(memcmp(&dirty[0][i],&dirty[1][i],sizeof(rect_t))) {
        /* A sole top-left launcher rect is the deliberately changed arc-label
         * damage. Geometry damage everywhere else remains baseline exact. */
        rect_t c=dirty[1][i];
        if(!strcmp(which,"launcher") && ndirty[0]==1 && c.x0>=0 && c.y0>=0 && c.x1<=210 && c.y1<=190) continue;
        fail(which,phase,"dirty rect",i);
    }
}
static void raster_one(const wos_app *app,uint16_t *px,int ox,int oy,int ybase,int h,int cx0,int cx1) {
    size_t n=(size_t)GFX_W*h; for(size_t i=0;i<n;i++) px[i]=(uint16_t)(0x39E7u+(i*17u)%997u);
    gfx_fb fb={px,ox,oy,ybase,h,cx0,cx1,0}; app->on_draw(&fb);
}
static void check_pixels(const char *which,const char *phase,const wos_app *ref,const wos_app *cur,
                         int ox,int oy,int ybase,int h,int cx0,int cx1) {
    raster_one(ref,a,ox,oy,ybase,h,cx0,cx1); raster_one(cur,b,ox,oy,ybase,h,cx0,cx1);
    size_t n=(size_t)GFX_W*h; for(size_t i=0;i<n;i++) if(a[i]!=b[i]) {
        int x=(int)(i%GFX_W), y=(int)(i/GFX_W)+ybase-oy;
        /* Launcher intentionally replaced the old bitmap arc label with the
         * Ultra face's vector renderer. Keep comparing every other pixel. */
        if(!strcmp(which,"launcher") && x-ox<210 && y<190) continue;
        fail(which,phase,"pixel",(int)i);
    }
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
    for(unsigned i=0;i<sizeof enter/sizeof enter[0];i++) {
        int renders=launcher_label_dbg_renders;
        frame("launcher","entrance",r,c,enter[i]);
        int expected=i==0 ? 1 : 0;
        if(launcher_label_dbg_renders-renders!=expected)
            fail("launcher","label cache","unexpected vector renders",launcher_label_dbg_renders-renders);
    }
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

static void launcher_label_bounds_case(void) {
    static const char *groups[]={"Time","Health","Media","Connect","Tools"};
    static const char *actual_variants[]={"watchface","stopwatch","Wi-Fi","Calc","Torch","Diagnostics","Pet care"};
    int maxw=0,maxh=0;
    for(unsigned pass=0;pass<3;pass++) {
        unsigned n=pass==0 ? (unsigned)(sizeof groups/sizeof groups[0]) :
                   pass==1 ? 24u : (unsigned)(sizeof actual_variants/sizeof actual_variants[0]);
        for(unsigned i=0;i<n;i++) {
            const char *s=pass==0 ? groups[i] : pass==1 ? names[i] : actual_variants[i]; int x0,y0,x1,y1;
            if(!pathtext_arc_bounds(PATHTEXT_CONDENSED,s,GFX_CX-GFX_S(13),-0.92f,
                                    GFX_S(7),&x0,&y0,&x1,&y1))
                fail("launcher","label bounds","missing bounds",(int)i);
            int w=x1-x0,h=y1-y0; if(w>maxw)maxw=w;if(h>maxh)maxh=h;
            if(w>(int)GFX_S(125)||h>(int)GFX_S(46))
                fail("launcher","label bounds","cache capacity",w>h?w:h);
        }
    }
    printf("launcher label cache: largest registered label %dx%d within %dx%d capacity\n",
           maxw,maxh,(int)GFX_S(125),(int)GFX_S(46));
}

static void launcher_label_cache_case(void) {
    const int cx=(int)GFX_CX,cy=(int)GFX_CY;
    /* A cached full render and the real translated stripe pipeline must agree,
     * including every antialiased label pixel. */
    now_ms=40000; app_launcher.on_start(); now_ms=40700; wos_frame_id++;
    memset(a,0x29,(size_t)GFX_W*GFX_H*sizeof *a);
    memset(b,0x29,(size_t)GFX_W*GFX_H*sizeof *b);
    int renders=launcher_label_dbg_renders;
    gfx_fb full={a,-19,-11,0,GFX_H,0,GFX_W,0}; app_launcher.on_draw(&full);
    for(int y=0;y<GFX_H;y+=37) {
        int h=GFX_H-y<37?GFX_H-y:37;
        gfx_fb stripe={b+(size_t)y*GFX_W,-19,-11,y,h,0,GFX_W,0};
        app_launcher.on_draw(&stripe);
    }
    if(memcmp(a,b,(size_t)GFX_W*GFX_H*sizeof *a)) fail("launcher","label stripes","full/stripe mismatch",0);
    if(launcher_label_dbg_renders-renders!=1) fail("launcher","label stripes","cache miss count",launcher_label_dbg_renders-renders);

    /* Put the longer Connect label in the cache, then focus Time. The one
     * label-only dirty rect must cover both bounds so Connect's ends clear. */
    now_ms=41000; app_launcher.on_start(); current_shot(a,41700);
    event_one(&app_launcher,WOS_EV_TOUCH_DOWN,cx,cy-150);
    now_ms=41716; event_one(&app_launcher,WOS_EV_TOUCH_MOVE,cx+90,cy+120);
    current_shot(a,41716); /* cache focused Connect */
    event_one(&app_launcher,WOS_EV_TOUCH_DOWN,cx+90,cy+120);
    now_ms=41732; event_one(&app_launcher,WOS_EV_TOUCH_MOVE,cx,cy-150);
    gfx_rec rec={0}; gfx_fb record={0,0,0,0,GFX_H,0,GFX_W,&rec};
    ndirty[1]=0;dirty_side=1;wos_frame_id++;app_launcher.on_draw(&record);
    if(ndirty[1]!=1) fail("launcher","long-short cleanup","dirty count",ndirty[1]);
    int ax0,ay0,ax1,ay1,bx0,by0,bx1,by1; rect_t d=dirty[1][0];
    pathtext_arc_bounds(PATHTEXT_CONDENSED,"Connect",GFX_CX-GFX_S(13),-0.92f,GFX_S(7),&ax0,&ay0,&ax1,&ay1);
    pathtext_arc_bounds(PATHTEXT_CONDENSED,"Time",GFX_CX-GFX_S(13),-0.92f,GFX_S(7),&bx0,&by0,&bx1,&by1);
    if(d.x0>ax0||d.y0>ay0||d.x1<ax1||d.y1<ay1||d.x0>bx0||d.y0>by0||d.x1<bx1||d.y1<by1)
        fail("launcher","long-short cleanup","outgoing/incoming not covered",0);
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
    launcher_label_bounds_case(); launcher_label_cache_case(); launcher_case(); applist_case(); angular_input_case();
    printf("launcher renders: %u baseline/current pixel+dirty comparisons passed; angular drag/wrap/deadzone/release/tap passed (reference 7b5c1b3)\n",checks);
    free(a);free(b); return 0;
}
