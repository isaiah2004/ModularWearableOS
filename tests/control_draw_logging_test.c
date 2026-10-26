/* Actual-source regression; wrappers forward into the real diagnostic queue. */
#ifndef CONTROL_SOURCE
#define CONTROL_SOURCE "../nos/apps/sys_control.c"
#endif
#ifndef BASELINE
#define BASELINE 0
#endif
#include CONTROL_SOURCE
#include <assert.h>
#include <windows.h>

extern int sim_program_main(int,char **);
extern uint32_t hal_ext_sim_present;
extern uint32_t __real_diag_action_request(uint16_t,uint16_t,int32_t);
extern void __real_diag_action_context(uint32_t,int32_t,int32_t);
extern void __real_diag_action_result(uint32_t,int32_t,int32_t);
extern void __real_diag_log_event(uint16_t,int32_t,int32_t,int32_t);
static unsigned requests,contexts,results,events;
static uint32_t request_id,context_id,result_id;
static uint16_t request_domain,request_action;
static int32_t request_target,context_source,context_value,result_outcome,result_detail;
uint32_t __wrap_diag_action_request(uint16_t domain,uint16_t action,int32_t target)
{requests++;request_domain=domain;request_action=action;request_target=target;
 return request_id=__real_diag_action_request(domain,action,target);}
void __wrap_diag_action_context(uint32_t request,int32_t source,int32_t value)
{contexts++;context_id=request;context_source=source;context_value=value;__real_diag_action_context(request,source,value);}
void __wrap_diag_action_result(uint32_t request,int32_t result,int32_t detail)
{results++;result_id=request;result_outcome=result;result_detail=detail;__real_diag_action_result(request,result,detail);}
void __wrap_diag_log_event(uint16_t code,int32_t a,int32_t b,int32_t c)
{events++;__real_diag_log_event(code,a,b,c);}
static void reset_counts(void){requests=contexts=results=events=0;}
static diag_log_status status(void){diag_log_status value;diag_log_get(&value);return value;}
static uint16_t full[GFX_W*GFX_H],striped[GFX_W*GFX_H];
static unsigned draw_calls,pixel_cases;

static void raw_draw(gfx_fb *fb,int blur_source)
{
    __typeof__(C) saved=C;__typeof__(P) press=P;
    diag_log_status before=status();unsigned q=requests,c=contexts,r=results,e=events;
    if(blur_source)control_blur_source(fb);else c_draw(fb);
    assert(!memcmp(&saved,&C,sizeof C) && !memcmp(&press,&P,sizeof P));
    if(!BASELINE){
        diag_log_status after=status();
        assert(requests==q && contexts==c && results==r && events==e);
        assert(after.queued==before.queued && after.dropped==before.dropped &&
               after.last_request==before.last_request && after.written==before.written);
    }
    draw_calls++;
}
static void pixels(FILE *file,int blur_source,int ox,int oy)
{
    for(unsigned i=0;i<sizeof full/sizeof *full;i++)full[i]=striped[i]=(uint16_t)(i*29u+123u);
    gfx_fb fb={0};fb.px=full;fb.h=GFX_H;fb.cx1=GFX_W;fb.ox=ox;fb.oy=oy;
    raw_draw(&fb,blur_source);
    for(int y=0;y<GFX_H;y+=16){gfx_fb stripe=fb;stripe.px=striped+y*GFX_W;
        stripe.y_base=y;stripe.h=GFX_H-y<16?GFX_H-y:16;raw_draw(&stripe,blur_source);}
    assert(!memcmp(full,striped,sizeof full));
    assert(fwrite(full,sizeof full,1,file)==1);
    gfx_rec rec={0};fb.rec=&rec;fb.px=NULL;raw_draw(&fb,blur_source);
    assert(rec.n>0);assert(fwrite(&rec.n,sizeof rec.n,1,file)==1);
    assert(fwrite(rec.c,sizeof *rec.c,(size_t)rec.n,file)==(size_t)rec.n);
    pixel_cases++;
}
int main(int argc,char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    assert(!sim_program_main(argc,argv));assert(status().ready);
    nos_t=100;C.t_enter=nos_t-10;C.fill_live=1;C.sl_val=.6f;C.bat=78;
    snprintf(C.time,sizeof C.time,"10:09");P.id=-1;
    nos_prefs.brightness=60;
    for(int i=0;i<NT;i++){C.lv_from[i]=C.lv_to[i]=(short)(state(i)?255:LV_OFF);C.on_t0[i]=nos_t-10;}
    FILE *file=fopen("pixels.bin","wb");assert(file);reset_counts();
    const uint32_t masks[]={0,EXT_SPEAKER,0x1ff};
    for(unsigned mask=0;mask<3;mask++)for(int page=0;page<3;page++)
    for(int pose=0;pose<2;pose++)for(int phase=0;phase<4;phase++){
        hal_ext_sim_present=masks[mask];C.page_x=page*233.0f;C.page=page==2;
        C.t_enter=phase==3?nos_t-10:nos_t-phase/(float)NOS_FPS*3;
        pixels(file,0,pose?-19:0,pose?17:0);
    }
    hal_ext_sim_present=0;C.page=0;C.page_x=0;C.t_enter=nos_t-10;
    pixels(file,1,0,0);pixels(file,1,-19,17);assert(!fclose(file));
    unsigned draw_requests=requests;
    if(BASELINE)assert(draw_requests>100);else assert(!requests && !contexts && !results && !events);
    const int absent[]={T_SOUND,T_HAPTIC,T_LTE,T_GPS};
    for(unsigned n=0;n<sizeof absent/sizeof *absent;n++){
        int tile=absent[n];float x,y;tile_pos(tile,&x,&y);reset_counts();diag_log_service();
        diag_log_status before=status();nos_prefs_t prefs=nos_prefs;
        nos_input input={.type=IN_DOWN,.x=(int)x,.y=(int)y};assert(c_input(&input));assert(!requests);
        input.type=IN_TAP;assert(c_input(&input));input.type=IN_UP;assert(c_input(&input));
        assert(!memcmp(&prefs,&nos_prefs,sizeof prefs));assert(!strncmp(C.note,"NO ",3));
        if(BASELINE)assert(!requests && !contexts && !results);
        else{
            assert(requests==1 && contexts==1 && results==1 && request_id &&
                context_id==request_id && result_id==request_id);
            assert(request_domain==DIAG_DOMAIN_CONTROL && request_action==DIAG_ACT_SET && request_target==tile);
            assert(context_source==(int32_t)diag_log_name_id("CONTROL") && context_value==state(tile));
            assert(result_outcome==DIAG_RESULT_REJECTED && result_detail==(int32_t)k_tile[tile].need);
            diag_log_status after=status();assert(after.queued==before.queued+3 &&
                after.last_request==request_id && after.last_result_request==request_id &&
                after.last_result==DIAG_RESULT_REJECTED && after.last_target==(uint32_t)tile);
            gfx_fb fb={0};fb.px=full;fb.h=GFX_H;fb.cx1=GFX_W;
            for(int i=0;i<20;i++)raw_draw(&fb,0);
            assert(requests==1 && contexts==1 && results==1);
        }
    }
    printf("PASS control draw logging: baseline=%d draw_calls=%u pixel_cases=%u draw_requests=%u disabled_taps=4\n",BASELINE,draw_calls,pixel_cases,draw_requests);
    return 0;
}
