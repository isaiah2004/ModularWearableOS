#include "../nos/developer.c"
#include "../nos/apps/set_developer.c"
#include <assert.h>
#include <math.h>
#include <string.h>
nos_stats nos_last;
static diag_log_status fixture;
static const nos_screen *opened;
static int reads,marks;
static uint16_t last_code;
static int32_t last_a;
const font_t *UI_F_BODY,*UI_F_SMALL;
int nos_aod;
nos_prefs_t nos_prefs;
static int saves;
static int te_available=1;
int hal_display_te_available(void){return te_available;}
void prefs_save(void){saves++;}
const nos_screen *nos_top(void){return opened;}
int nos_depth(void){return 1;}
int nos_revealed(void){return 0;}
int nos_leaving_covered(void){return 0;}
void gfx_fill_rect(gfx_fb *fb,int x,int y,int w,int h,uint16_t c){(void)fb;(void)x;(void)y;(void)w;(void)h;(void)c;}
void gfx_ring(gfx_fb *fb,float x,float y,float r,float t,uint16_t c){(void)fb;(void)x;(void)y;(void)r;(void)t;(void)c;}
void gfx_line(gfx_fb *fb,float x0,float y0,float x1,float y1,float t,uint16_t c){(void)fb;(void)x0;(void)y0;(void)x1;(void)y1;(void)t;(void)c;}
void ui_fblur_coverage_claim(const gfx_fb *fb,int x0,int y0,int x1,int y1){(void)fb;(void)x0;(void)y0;(void)x1;(void)y1;}
void nos_invalidate(void){}
void nos_feedback(int k){(void)k;}
void nos_push(const nos_screen *screen,int tx){assert(tx==TX_NONE);opened=screen;if(screen->enter)screen->enter();}
void diag_log_get(diag_log_status *out){reads++;*out=fixture;}
int diag_log_fault_get(unsigned index,diag_fault_summary*out){(void)index;memset(out,0,sizeof *out);return 0;}
uint32_t diag_action_request(uint16_t domain,uint16_t action,int32_t target){assert(domain==DIAG_DOMAIN_DEVELOPER&&action==DIAG_ACT_MARK);last_code=DIAG_ACTION_REQUEST;last_a=target;marks++;return 17;}
void diag_action_result(uint32_t request,int32_t result,int32_t detail){assert(request==17&&result==DIAG_RESULT_SUCCESS&&!detail);}
void diag_log_event(uint16_t code,int32_t a,int32_t b,int32_t c){last_code=code;last_a=a;assert(b==IN_TAP && c==0);marks++;}
uint32_t diag_log_name_id(const char *s){uint32_t h=2166136261u;while(*s)h=(h^(unsigned char)*s++)*16777619u;return h;}
int ui_text(gfx_fb *fb,const font_t *f,int x,int y,const char *s,uint16_t c,int a,int t){(void)fb;(void)f;(void)x;(void)y;(void)s;(void)c;(void)a;(void)t;return 0;}
int ui_text_fit(gfx_fb *fb,const font_t *f,int x,int y,const char *s,uint16_t c,int a,int w){(void)w;return ui_text(fb,f,x,y,s,c,a,0);}
int ui_label(gfx_fb *fb,int x,int y,const char *s,uint16_t c,int a){return ui_text(fb,0,x,y,s,c,a,0);}
void ui_title(gfx_fb *fb,const char *s,float scroll){(void)fb;(void)s;(void)scroll;}
void ui_button(gfx_fb *fb,float x,float y,float w,float h,const char *s,int kind,int pressed){(void)fb;(void)x;(void)y;(void)w;(void)h;(void)s;(void)kind;(void)pressed;}
int main(void){
 for(int i=0;i<20;i++)nos_dev_frame(25);
 assert(fabsf(fps-40)<.01f);
 for(int i=0;i<10;i++)nos_dev_frame(50);
 assert(fabsf(fps-20)<.01f);
 nos_input in={0};in.type=IN_TAP;in.x=233;
 for(int i=0;i<4;i++){in.y=116+39*i;assert(dev_input(&in));assert(nos_dev_overlays==((1u<<(i+1))-1));}
 in.y=380;assert(dev_input(&in));assert(!nos_dev_overlays);
 in.x=0;in.y=150;assert(!dev_input(&in));assert(!nos_dev_overlays);
 nos_dev_touch(42,64,1);assert(tx==42 && ty==64 && down);
 nos_dev_touch(42,64,0);assert(!down);
 nos_prefs.fps_cap=60;in.x=233;in.y=272;assert(dev_input(&in));assert(opened==&scr_frame_cap);
 unsigned overlays=nos_dev_overlays;
 for(unsigned i=0;i<6;i++) {
   in.y=128+(int)i*45;assert(opened->input(&in));assert(nos_prefs.fps_cap==frame_caps[i]);
   assert(nos_dev_overlays==overlays);opened->draw(NULL);
 }
 assert(saves==6);assert(opened->input(&in) && saves==6); /* same choice does not write */
 in.x=79;assert(!opened->input(&in));in.x=233;in.type=IN_DOWN;assert(!opened->input(&in));in.type=IN_TAP;
 in.y=380;assert(dev_input(&in));assert(!nos_dev_overlays && nos_prefs.fps_cap==0); /* overlays only */
 in.y=311;assert(dev_input(&in));assert(nos_prefs.display_te==1 && saves==7);
 assert(!nos_dev_overlays && nos_prefs.fps_cap==0);dev_draw(NULL);
 in.type=IN_DOWN;assert(!dev_input(&in) && nos_prefs.display_te==1 && saves==7);in.type=IN_TAP;
 in.x=59;assert(!dev_input(&in) && saves==7);in.x=233;
 assert(dev_input(&in));assert(!nos_prefs.display_te && saves==8);
 te_available=0;assert(dev_input(&in));assert(!nos_prefs.display_te && saves==8);dev_draw(NULL);
 te_available=1;
 opened=NULL;
 nos_dev_overlays=NOS_DEV_FPS|NOS_DEV_SCREEN;
 fixture=(diag_log_status){.reset_reason=4,.queued=7,.written=123,.dropped=2,.errors=1,.previous_events=31,.crash_boots=3,.available=1,.ready=1};
 in.x=100;in.y=418;assert(!dev_input(&in));assert(!opened);
 in.x=233;assert(dev_input(&in));assert(opened==&scr_system_trace);
 assert(!nos_dev_overlays && reads==1 && trace_status.queued==7);
 int before=reads;opened->draw(NULL);assert(reads==before); /* render uses cached status only */
 fixture.queued=9;opened->tick(.033f);assert(reads==before+1 && trace_status.queued==9);
 assert(!strcmp(reset_name(4),"PANIC") && !strcmp(reset_name(9),"BROWNOUT") && !strcmp(reset_name(99),"UNKNOWN"));
 in.x=106;in.y=382;assert(!opened->input(&in));assert(!marks);
 in.x=233;in.type=IN_DOWN;assert(!opened->input(&in));assert(!marks);
 in.type=IN_TAP;assert(opened->input(&in));assert(marks==1 && last_code==DIAG_ACTION_REQUEST && (uint32_t)last_a==diag_log_name_id("diag.marker"));
 in.y=108;assert(opened->input(&in));assert(trace_page==1);opened->draw(NULL);
 assert(trace_marked);opened->leave();assert(nos_dev_overlays==(NOS_DEV_FPS|NOS_DEV_SCREEN));
 puts("PASS developer controls, cached trace status, mark bounds and overlay restoration");
}
