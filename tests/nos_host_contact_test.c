#include "nos_touch_contact.h"
#include "nos_panel_ack.h"
#include <assert.h>
#include <stdio.h>
/* ACTUAL FUNCTIONS */
int main(void)
{
    nos_touch_contact t={0};assert(nos_touch_contact_sample(&t,-1,0)==-1 && !t.down);
    assert(nos_touch_contact_sample(&t,1,1)==1);
    assert(nos_touch_contact_sample(&t,-1,20)==-1 && t.down);
    assert(nos_touch_contact_sample(&t,-1,169)==-1 && t.down);
    assert(nos_touch_contact_sample(&t,-1,170)==2 && !t.down && t.suppressed);
    assert(nos_touch_contact_sample(&t,-1,200)==-1);
    assert(nos_touch_contact_sample(&t,1,220)==-1 && !t.down);
    assert(nos_touch_contact_sample(&t,0,230)==-1 && !t.suppressed);
    assert(nos_touch_contact_sample(&t,1,240)==1);
    assert(nos_touch_contact_sample(&t,-1,250)==-1 && t.down);
    assert(nos_touch_contact_sample(&t,0,260)==0); /* only an actual read can release */
    t=(nos_touch_contact){0};assert(nos_touch_contact_sample(&t,1,UINT32_MAX-70)==1);
    assert(nos_touch_contact_sample(&t,-1,UINT32_MAX-49)==-1);
    assert(nos_touch_contact_sample(&t,-1,100)==2);
    nos_panel_ack s={.applied=17};uint32_t old=nos_panel_request(&s,93);
    assert(!nos_panel_complete(&s,old+1,0) && s.applied==17 && !s.ack);
    assert(nos_panel_complete(&s,old,-7) && s.applied==17 && s.error==-7);
    assert(!nos_panel_complete(&s,old,0) && s.applied==17); /* late callback cannot repair an ambiguous operation */
    uint32_t fresh=nos_panel_request(&s,41);assert(fresh!=old);
    assert(!nos_panel_complete(&s,old,0) && s.applied==17);
    assert(nos_panel_complete(&s,fresh,0) && s.applied==41);
    s.request=UINT32_MAX;assert(nos_panel_request(&s,8)==1);
    puts("PASS touch: retain brief unknown, cancel once, suppress until real release, wrap; panel stale/failed ACK preserves applied state");
    return 0;
}
