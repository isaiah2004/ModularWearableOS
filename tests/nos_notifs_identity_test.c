/* Production screen plus production phone/store operations, fresh engine ABI. */
#include "apps/sys_notifs.c"
#include <assert.h>
extern int sim_program_main(int,char **);

static void post(int id,const char *body)
{
    char line[512];snprintf(line,sizeof line,"GB({\"t\":\"notify\",\"id\":%d,\"src\":\"App\",\"title\":\"Title\",\"body\":\"%s\"})",id,body);
    svc_phone_inject(line);
}
static void reset_list(void)
{
    nos_touch_cancel();svc_notif_clear();post(1,"older body");post(2,"target body");
    nos_set_root(&scr_notifs);nos_t+=3;n_tick(.025f);assert(N.n==2);
}
static void input(int type,int y,float dx,float vx)
{
    nos_input in={.type=(nos_in_type)type,.x=233,.y=y,.x0=233,.y0=y,.axis=AXIS_X,.dx=dx,.vx=vx};
    n_input(&in);
}
static void drag(int y)
{input(IN_DRAG_START,y,-20,0);input(IN_DRAG,y,-160,0);}
static void finish_fly(int target,int survivor)
{
    for(int i=0;i<12;i++) {nos_t+=.025;n_tick(.025f);}
    assert(index_of_id(target)<0 && index_of_id(survivor)>=0 && s_fly && s_fly->id==target);
    diag_log_status status;diag_log_get(&status);
    assert(status.last_result==DIAG_RESULT_SUCCESS && status.last_result_detail==target);
}
int main(int argc,char **argv)
{
    assert(!sim_program_main(argc,argv));nos_prefs.dnd=1;diag_log_init();
    reset_list();input(IN_DOWN,145,0,0);post(3,"arrives before release");n_tick(.025f);
    drag(145);assert(N.sw==1 && N.sw_id==2);input(IN_DRAG_END,145,-160,-900);
    assert(N.out_id==2);finish_fly(2,3);

    reset_list();input(IN_DOWN,145,0,0);drag(145);input(IN_DRAG_END,145,-160,-900);
    post(3,"arrives during fly-out");finish_fly(2,3);

    reset_list();input(IN_DOWN,145,0,0);drag(145);input(IN_DRAG_END,145,-160,-900);
    uint32_t revision=svc_notif_revision();post(1,"same-count update moves older ID to front");
    assert(svc_notif_count()==2 && svc_notif_revision()!=revision);finish_fly(2,1);

    reset_list();float before=card_hi(0);revision=svc_notif_revision();
    post(2,"The replacement body is long enough to wrap over multiple lines while retaining the same first identity and item count.");
    n_tick(.025f);assert(svc_notif_revision()!=revision && card_hi(0)>before);

    reset_list();input(IN_DOWN,145,0,0);drag(145);input(IN_DRAG_END,145,-160,-900);
    svc_notif_dismiss(index_of_id(2));n_tick(.025f);
    assert(N.out_i<0 && N.sw<0 && !N.sw_live);input(IN_DRAG_END,145,-160,-900);
    assert(svc_notif_count()==1 && index_of_id(1)>=0);

    svc_notif_clear();for(int i=0;i<SVC_NOTIF_MAX;i++)post(100+i,"capacity");
    nos_set_root(&scr_notifs);nos_t+=3;n_tick(.025f);
    int last=index_of_id(100);assert(last==SVC_NOTIF_MAX-1);msc_jump(&N.sc,N.sc.max);
    int y=(int)(TOP_Y+card_yi(last)-floorf(N.sc.x+.5f)+card_hi(last)*.5f);assert(y>0 && y<GFX_H);
    input(IN_DOWN,y,0,0);drag(y);assert(N.sw_id==100);
    post(999,"full-capacity replacement");n_tick(.025f);input(IN_DRAG_END,y,-160,-900);
    for(int i=0;i<12;i++) {nos_t+=.025;n_tick(.025f);}
    assert(svc_notif_count()==SVC_NOTIF_MAX && index_of_id(999)>=0 && !N.sw_live && N.out_i<0);

    reset_list();input(IN_DOWN,145,0,0);post(3,"arrival before tap");input(IN_TAP,145,0,0);
    assert(D.id==2 && nos_top()==&scr_notif_detail);

    reset_list();sys_notif_open(0);post(3,"arrival before detail action, without a detail tick");
    nos_input tap={.type=IN_TAP,.x=UI_BTN_PAIR_X1,.y=(int)d_btn_y()};d_input(&tap);
    assert(index_of_id(2)<0 && index_of_id(3)>=0 && index_of_id(1)>=0);

    reset_list();input(IN_DOWN,145,0,0);drag(145);input(IN_CANCEL,145,-160,0);
    for(int i=0;i<12;i++) {nos_t+=.025;n_tick(.025f);}
    assert(svc_notif_count()==2 && index_of_id(2)>=0 && N.sw<0 && N.out_i<0);

    reset_list();input(IN_DOWN,145,0,0);uint32_t old=svc_notif_get(0)->instance;
    svc_notif_dismiss(0);post(2,"reused phone id before drag");
    assert(svc_notif_get(0)->instance!=old);drag(145);input(IN_DRAG_END,145,-160,-900);
    assert(N.sw<0 && N.out_i<0 && svc_notif_count()==2);

    reset_list();input(IN_DOWN,145,0,0);drag(145);input(IN_DRAG_END,145,-160,-900);
    svc_notif_dismiss(0);post(2,"reused phone id during flight");
    for(int i=0;i<12;i++) {nos_t+=.025;n_tick(.025f);}
    assert(N.sw<0 && N.out_i<0 && svc_notif_count()==2 && index_of_id(2)>=0);

    reset_list();sys_notif_open(0);old=D.instance;svc_notif_dismiss(0);post(2,"replacement detail");
    assert(svc_notif_get(0)->instance!=old);d_input(&tap);
    assert(index_of_id(2)>=0 && svc_notif_count()==2 && D.idx<0);
    diag_log_status status;diag_log_get(&status);assert(status.last_result==DIAG_RESULT_STALE && status.last_result_detail==2);
    puts("PASS notification stable IDs: press/flight arrivals, same-count update, body revision, capacity eviction, removal, tap/detail actions, cancellation, reused phone IDs");
    return 0;
}
