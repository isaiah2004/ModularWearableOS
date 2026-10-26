/* Actual Activity source against independent original drawing derivation. */

#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "svc.h"
#include "prefs.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
static unsigned format_calls;
static int counted_snprintf(char *out, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int result = vsnprintf(out, n, fmt, args);
    va_end(args);
    format_calls++;
    return result;
}
#define snprintf counted_snprintf
#include "../nos/apps/fit_activity.c"
#undef snprintf
static svc_activity_t feed;
static hal_datetime wall_clock;
static int revealed, finger;
double nos_t; float nos_dt;
const svc_activity_t *svc_activity(void) { return &feed; }
void hal_get_datetime(hal_datetime *out) { *out = wall_clock; }
int nos_revealed(void) { return revealed; }
int nos_entered_by_finger(void) { return finger; }
double nos_enter_t(void) { return nos_t + 0.15f; }
void nos_feedback(int kind) { (void)kind; }
nos_prefs_t nos_prefs;
uint16_t nos_c_red=0xD0C4,nos_c_red_d=0xB083,nos_c_green=0x4CEB,nos_c_amber=0xD548;
uint32_t nos_frame;
/* Rendering outside Today/Week is deliberately not exercised here. */
int pblur_baking;
void nos_open(const nos_screen *s, int x, int y) { (void)s; (void)x; (void)y; assert(0); }
void prog_ring(gfx_fb *f, int st, float x, float y, float r, float t, int n, float q, uint16_t on, uint16_t off)
{ (void)f; (void)st; (void)x; (void)y; (void)r; (void)t; (void)n; (void)q; (void)on; (void)off; assert(0); }
void dot_ring(gfx_fb *f, float x, float y, float r, int n, float d, float a, float b, uint16_t on, uint16_t off)
{ (void)f; (void)x; (void)y; (void)r; (void)n; (void)d; (void)a; (void)b; (void)on; (void)off; assert(0); }
int dot_text_a(gfx_fb *f, const dot_style *s, float x, float y, const char *t, int a)
{ (void)f; (void)s; (void)x; (void)y; (void)t; (void)a; assert(0); return 0; }
int dot_text_morph(gfx_fb *f, const dot_style *s, float x, float y, const char *a, const char *b, int align, float p)
{ (void)f; (void)s; (void)x; (void)y; (void)a; (void)b; (void)align; (void)p; assert(0); return 0; }
float nos_mb_track_step(nos_mb_track *t, float p) { (void)t; (void)p; assert(0); return 0; }
float nos_mblur_step(float p, int held) { (void)p; (void)held; assert(0); return 0; }
void nos_mblur_v(gfx_fb *f, int a, int b, float s) { (void)f; (void)a; (void)b; (void)s; assert(0); }
void ui_scrollbar(gfx_fb *f, float a, float b, float c, float d)
{ (void)f; (void)a; (void)b; (void)c; (void)d; assert(0); }
static unsigned long long digest;
static void bytes(const void *p,size_t n){const unsigned char *s=p;for(size_t i=0;i<n;i++){digest^=s[i];digest*=1099511628211ull;}}
static void integer(int n){bytes(&n,sizeof n);}
static void number(float n){bytes(&n,sizeof n);}
static void string(const char *s){bytes(s,strlen(s)+1);}
int ui_label(gfx_fb*f,int x,int y,const char*s,uint16_t c,int a){(void)f;integer(1);integer(x);integer(y);string(s);integer(c);integer(a);return 0;}
void dot_icon_anim(gfx_fb*f,int icon,float x,float y,float p,float d,uint16_t c,float g,int mode){(void)f;integer(2);integer(icon);number(x);number(y);number(p);number(d);integer(c);number(g);integer(mode);}
int dot_text_anim(gfx_fb*f,const dot_style*s,float x,float y,const char*t,int a,float p,int mode,unsigned seed){(void)f;integer(3);integer(s->font);number(s->pitch);number(s->dot);integer(s->color);integer(s->ghost);integer(s->gap);integer(s->mono);number(x);number(y);string(t);integer(a);number(p);integer(mode);bytes(&seed,sizeof seed);return 0;}
void dot_columns(gfx_fb*f,float x,float y,int cols,int rows,float pitch,float d,const uint8_t*v,uint16_t on,uint16_t off){(void)f;integer(4);number(x);number(y);integer(cols);integer(rows);number(pitch);number(d);bytes(v,(size_t)cols);integer(on);integer(off);}
void dot_disc(gfx_fb*f,float x,float y,float d,uint16_t c){(void)f;integer(5);number(x);number(y);number(d);integer(c);}
void dot_bar(gfx_fb*f,float x,float y,int n,float pitch,float d,float lit,uint16_t on,uint16_t off){(void)f;integer(6);number(x);number(y);integer(n);number(pitch);number(d);number(lit);integer(on);integer(off);}
#include "nos_activity_reference.inc"
static unsigned rng=7477;
static unsigned random_word(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
static void state(void){
 s_act.steps=(int)(random_word()%500000);s_act.goal=1+(int)(random_word()%100000);
 s_act.km=(random_word()%100001)/1000.f;s_act.kcal=(random_word()%100001)/100.f;s_act.kcal_total=s_act.kcal+(random_word()%200001)/100.f;s_act.kcal_assumed=(uint8_t)(random_word()%4);s_act.active_min=(int)(random_word()%1441);
 for(int i=0;i<24;i++){s_act.hourly[i]=(uint16_t)random_word();A.col_g[i]=(random_word()%1025)/1024.f;}
 for(int i=0;i<7;i++){s_act.week[i]=random_word();A.bar_g[i]=(random_word()%1025)/1024.f;}
 s_act.week_valid=(uint8_t)(random_word()&127u);
 for(int i=0;i<3;i++)A.blk_p[i]=(random_word()%1025)/1024.f;
 A.wk_p=(random_word()%1025)/1024.f;A.now_on=random_word()%2;A.goal_on=random_word()%2;A.hero.over=(int)(random_word()%49);
 s_hour=(int)(random_word()%28)-2;s_wday=(int)(random_word()%15)-4;nos_prefs.units_metric=random_word()%2;
}
static void check(float oy){
 gfx_fb f={0};digest=1469598103934665603ull;draw_today_reference(&f,oy);draw_week_reference(&f,oy);unsigned long long expected=digest;
 digest=1469598103934665603ull;draw_today(&f,oy);draw_week(&f,oy);
 assert(digest==expected);
}
static void edge_cases(void)
{
    static const float distances[] = { 0, .049f, .05f, 9.949f, 9.95f, 9.999f, 10, 16.09345f };
    memset(&s_act, 0, sizeof s_act);
    s_act.goal = 8000;
    memset(&A, 0, sizeof A);
    for (unsigned i = 0; i < sizeof distances / sizeof *distances; i++) {
        for (int metric = 0; metric < 2; metric++) {
            nos_prefs.units_metric = metric;
            s_act.km = distances[i]; s_act.kcal = i & 1 ? .5f : .499f;
            s_act.active_min = i & 1;
            s_hour = i & 1 ? 0 : 23; s_wday = (int)i % 7;
            for (int mode = 0; mode < 3; mode++) {
                s_act.steps = mode == 0 ? 0 : mode == 1 ? s_act.goal : s_act.goal * 3;
                s_act.week[6] = mode == 2 ? 50000 : 0;
                for (int h = 0; h < 24; h++) A.col_g[h] = mode * .5f;
                for (int d = 0; d < 7; d++) A.bar_g[d] = mode * .5f;
                A.goal_on = A.now_on = mode != 0; A.hero.over = mode == 2;
                activity_changed(); check(-465.75f); check(465.75f);
            }
        }
    }
}
static void lifecycle(void)
{
    nos_frame = 70001;
    nos_t = 1;
    nos_dt = 1.0f / M_FPS;
    state(); feed = s_act;
    wall_clock.hour = 12; wall_clock.weekday = 5;
    revealed = finger = 0;
    A.col_g[3] = 0.375f;
    act_enter();
    assert(!prepared_valid && A.col_g[3] == 0.375f);
    assert(!A.pg_started[1] && !A.pg_started[2]);
    unsigned before = format_calls;
    check(0);
    assert(format_calls == before + 5);
    check(-100.75f);
    assert(format_calls == before + 5);

    /* Activity ticks before the underlying HOME widget. Both share read_data,
     * so a widget update in this same frame must invalidate prepared values. */
    A.vp.sp.x = A.vp.sp.target = PAGE_H * 0.5f;
    A.vp.drag = 1;
    act_tick(nos_dt);
    assert(A.pg_started[1] && !prepared_valid);
    check(23.25f);
    feed.steps += 500; feed.hourly[12] = 1234;
    wall_clock.hour = 13; wall_clock.weekday = 6;
    wg_tick(nos_dt, 0);
    assert(!prepared_valid && s_hour == 13 && s_wday == 6);
    before = format_calls; check(-350.5f);
    assert(format_calls == before + 5);
    assert(s_act.steps == feed.steps);

    /* Reveal preserves page animations and pager position. */
    float page_time = A.pg_t[1], growth = A.col_g[3], position = A.vp.sp.x;
    revealed = 1; feed.goal = 0; nos_prefs.step_goal_100 = 123;
    act_enter();
    assert(!prepared_valid && A.pg_t[1] == page_time && A.col_g[3] == growth);
    assert(A.vp.sp.x == position && s_act.goal == 12300);
    check(466.25f);
    before = format_calls;
    nos_prefs.units_metric = !nos_prefs.units_metric;
    check(-465.75f); assert(format_calls == before + 5);
    before = format_calls; nos_frame++;
    check(0); assert(format_calls == before + 5);
    before = format_calls; pages_prepare();
    assert(!prepared_valid); check(0); assert(format_calls == before + 5);

    /* Fresh entries do not eagerly prepare unseen Today/Week metadata. */
    revealed = 0; finger = 1;
    act_enter(); before = format_calls;
    act_tick(nos_dt);
    assert(!prepared_valid && format_calls == before);
    check(1.25f);
    feed.goal = 0; nos_prefs.step_goal_100 = 0;
    read_data(); assert(s_act.goal == 8000 && !prepared_valid);
    check(0);
}
static void known_zero_average(void)
{
    memset(&s_act,0,sizeof s_act);s_act.goal=8000;s_act.week[0]=12000;s_act.week_valid=127;
    activity_changed();activity_prepare();assert(prepared.ndays==7 && !strcmp(prepared.average,"1714"));
    memset(s_act.week,0,sizeof s_act.week);s_act.steps=7000;
    activity_changed();activity_prepare();assert(prepared.ndays==7 && !strcmp(prepared.average,"1000"));
    s_act.week_valid=64;activity_changed();activity_prepare();assert(prepared.ndays==1 && !strcmp(prepared.average,"7000"));
    s_act.steps=0;activity_changed();activity_prepare();assert(prepared.ndays==1 && !strcmp(prepared.average,"0"));
}
int main(void)
{
    for (int frame = 0; frame < 20000; frame++) {
        nos_frame = (unsigned)frame;
        state(); activity_changed();
        unsigned before = format_calls;
        for (int pass = 0; pass < 5; pass++)
            check((int)(random_word() % 1801) - 900 + (random_word() % 4) * .25f);
        assert(format_calls == before + 5);
        if (frame % 5 == 0) {
            state(); activity_changed(); before = format_calls;
            check(0); assert(format_calls == before + 5);
        }
        if (frame % 7 == 0) {
            nos_prefs.units_metric = !nos_prefs.units_metric; before = format_calls;
            check(37.25f); assert(format_calls == before + 5);
        }
    }
    edge_cases();
    known_zero_average();
    lifecycle();
    printf("PASS 20000 ordered drawing states, repeated records/stripes, actual enter/reveal/tick/widget/units lifecycle; metadata %zu bytes\n", sizeof(activity_prepared));
    return 0;
}

uint16_t ui_mix(uint16_t a, uint16_t b, float t)
{
    if (t <= 0) return a;
    if (t >= 1) return b;
    /* lerp in 8-bit channels, then pack: a grey stays a neutral RGB565 grey (lerping
     * the 5- and 6-bit fields separately tints dark greys green or magenta) */
    int ra = (a >> 11) * 255 / 31, ga = ((a >> 5) & 63) * 255 / 63, ba = (a & 31) * 255 / 31;
    int rb = (b >> 11) * 255 / 31, gb = ((b >> 5) & 63) * 255 / 63, bb = (b & 31) * 255 / 31;
    int r = ra + (int)lroundf((rb - ra) * t), g = ga + (int)lroundf((gb - ga) * t), bl = ba + (int)lroundf((bb - ba) * t);
    /* two neutral greys always mix to a neutral grey: one 5-bit level, green the
     * nearest 6-bit match (2x below level 16, 2x + 1 from 16: 2x alone reads pink) */
    int na = (a >> 11) == (a & 31) && (((a >> 5) & 63) >> 1) == (a & 31);
    int nb = (b >> 11) == (b & 31) && (((b >> 5) & 63) >> 1) == (b & 31);
    if (na && nb) {
        int l5 = (r * 31 + 127) / 255;
        return (uint16_t)(l5 << 11 | (l5 * 2 + (l5 >= 16)) << 5 | l5);
    }
    return GFX_RGB(r, g, bl);
}
uint16_t ui_dim(uint16_t c, float k) { return ui_mix(0, c, k); }

