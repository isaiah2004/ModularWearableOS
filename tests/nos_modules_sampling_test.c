/* The runner inserts the actual modules_tick function at the marker below.
 * Rendering/cache selection is tested separately against the full app source. */
#include "ui.h"
#include <assert.h>
#include <stdio.h>
double nos_t = 10;
static struct { ui_list L; float next; int sample_hold, fb_lv, online, sum; } ML;
static unsigned samples;
#define LIVE_S 1.0f
static void sample(int a,int b) { (void)a;(void)b;samples++; }
static void list_values(void) {}
static void mods_count(void) { ML.online=9; }
static void mmt_set(int *p,const char *s,int init) { (void)p;(void)s;(void)init; }
void ui_list_tick(ui_list *l,float dt) { (void)l;(void)dt; }
static void mband_tick(float dt) { (void)dt; }
#define M_COUNT 16
/* ACTUAL_MODULES_TICK */
int main(void)
{
    ML.fb_lv=1;ML.sample_hold=0;ML.next=0;
    modules_tick(.025f);assert(samples==1 && ML.next==11);
    ML.fb_lv=0;ML.sample_hold=1;ML.next=0;
    modules_tick(.025f);assert(samples==1 && ML.next==0);
    ML.fb_lv=1;ML.sample_hold=0;
    modules_tick(.025f);assert(samples==2 && ML.next==11);
    modules_tick(.025f);assert(samples==2);
    puts("PASS actual Modules tick samples on slow cache motion and holds only on sample_hold");
    return 0;
}
