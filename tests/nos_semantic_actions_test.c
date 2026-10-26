#include "nos.h"
#include "diag_log.h"
#include <assert.h>
#include <stdio.h>
extern int sim_program_main(int,char **);
static const nos_screen first={.name="SEMANTIC FIRST"},second={.name="SEMANTIC SECOND"};
static int root_input(const nos_input *in)
{if(in->type==IN_DRAG_START)nos_pull(&first,TX_RISE,in);return 1;}
static const nos_screen root={.name="SEMANTIC ROOT",.input=root_input,.flags=SCR_HDRAG};
static void settle(void) {for(int i=0;i<80;i++)nos_frame_run(25);}
static diag_log_status status(void) {diag_log_status s;diag_log_get(&s);return s;}
int main(int argc,char **argv)
{
    assert(!sim_program_main(argc,argv));diag_log_init();assert(status().ready);
    nos_set_root(&root);diag_log_status s=status();
    assert(s.last_request && s.last_result_request==s.last_request && s.last_result==DIAG_RESULT_SUCCESS);

    nos_push(&first,TX_PUSH);s=status();uint32_t request=s.last_request;
    assert(s.last_target==diag_log_name_id(first.name) && s.last_result_request!=request);
    settle();s=status();assert(s.last_result_request==request && s.last_result==DIAG_RESULT_SUCCESS && nos_top()==&first);

    nos_pop();request=status().last_request;settle();s=status();
    assert(s.last_result_request==request && s.last_result==DIAG_RESULT_SUCCESS && nos_top()==&root);

    nos_touch(233,400,1);nos_frame_run(25);nos_touch(233,300,1);nos_frame_run(25);request=status().last_request;
    nos_touch_cancel();settle();s=status();
    assert(s.last_result_request==request && s.last_result==DIAG_RESULT_CANCELLED && nos_top()==&root);

    nos_push(&first,TX_PUSH);request=status().last_request;
    assert(nos_retarget(&second));s=status();
    assert(s.last_result_request==request && s.last_result==DIAG_RESULT_CANCELLED);
    request=s.last_request;settle();s=status();
    assert(s.last_result_request==request && s.last_result==DIAG_RESULT_SUCCESS && nos_top()==&second);

    nos_home_now();for(int i=1;i<8;i++)nos_push(&first,TX_NONE);
    nos_push(&second,TX_PUSH);s=status();
    assert(s.last_result_request==s.last_request && s.last_result==DIAG_RESULT_REJECTED && nos_top()==&first);
    puts("PASS semantic navigation: request versus arrival, dismissal, cancelled pull, retarget cancellation, rejected stack capacity");
    return 0;
}
