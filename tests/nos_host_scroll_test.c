#include "apps/scroll_diag.h"
#include <assert.h>
#include <stdio.h>
typedef struct {int tag;} nos_screen;
static const nos_screen scr_activity={1},scr_heart={2},scr_weather={3},scr_baro={4},scr_nav={5},scr_about={6},scr_launcher={7},scr_settings={8},scr_home={9};
static const nos_screen *top;
static int page,dragging,transition,vertical;
static float offset;
static const nos_screen *nos_top(void){return top;}
static int nos_transitioning(void){return transition;}
static void sys_home_nav_snapshot(int *p,float *o,int *d){*p=page;*o=offset;*d=dragging;}
#define SNAPSHOT(name,id) void name(nos_scroll_snapshot *s){*s=(nos_scroll_snapshot){id,2,0,1,0,12,600};}
SNAPSHOT(fit_activity_scroll_snapshot,1)
SNAPSHOT(fit_heart_scroll_snapshot,2)
SNAPSHOT(out_weather_scroll_snapshot,3)
SNAPSHOT(out_baro_scroll_snapshot,4)
SNAPSHOT(out_nav_scroll_snapshot,5)
SNAPSHOT(set_about_scroll_snapshot,6)
SNAPSHOT(set_settings_scroll_snapshot,8)
void sys_launcher_scroll_snapshot(nos_scroll_snapshot *s){*s=(nos_scroll_snapshot){7,2,0,1,vertical,123,900};}
/* ACTUAL FUNCTIONS */
int main(void)
{
    top=&scr_home;page=1;offset=466;
    nos_scroll_snapshot s=scroll_snapshot();assert(s.app==7 && s.offset==123 && s.limit==900 && s.axis==2);
    dragging=vertical=1;s=scroll_snapshot();assert(s.app==7 && s.dragging==1);
    vertical=0;assert(scroll_snapshot().app==0); /* horizontal drag */
    dragging=0;offset=465.75f;assert(scroll_snapshot().app==0); /* nearest page is not arrival */
    offset=466;transition=1;assert(scroll_snapshot().app==0);
    transition=0;page=0;offset=0;assert(scroll_snapshot().app==0);
    top=&scr_launcher;assert(scroll_snapshot().app==7);
    top=&scr_settings;assert(scroll_snapshot().app==8);
    top=NULL;assert(scroll_snapshot().app==0);
    puts("PASS actual scroll diagnostics: HOME embedded Apps, vertical drag, page/transition/horizontal negatives, real Settings");
    return 0;
}
