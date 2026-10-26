#include "cost_profile.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
static uint32_t now;
uint32_t hal_micros(void){return now;}
int main(void) {
    nos_cost_set(1);nos_cost_capture_reset();nos_cost_begin();
    now=10;nos_cost_scope scene={0};nos_cost_enter(&scene,NOS_COST_SCENE);
    now=20;nos_cost_scope clear={0};nos_cost_enter(&clear,NOS_COST_CLEAR);
    now=30;nos_cost_leave(&clear);
    now=35;nos_cost_scope sprite={0};nos_cost_enter(&sprite,NOS_COST_SPRITE8);
    now=70;nos_cost_leave(&sprite);
    now=90;nos_cost_leave(&scene);nos_cost_end();
    nos_cost_frame f=nos_cost_last();
    assert(f.inclusive[NOS_COST_SCENE]==80 && f.exclusive[NOS_COST_SCENE]==35);
    assert(f.exclusive[NOS_COST_CLEAR]==10 && f.exclusive[NOS_COST_SPRITE8]==35);
    unsigned sum=0;for(unsigned b=0;b<NOS_COST_COUNT;b++)sum+=f.exclusive[b];assert(sum==80);
    nos_cost_capture_add(1);
    nos_cost_begin();now=0xfffffff0u;nos_cost_scope wrap={0};nos_cost_enter(&wrap,NOS_COST_TEXT);
    now=0x20;nos_cost_leave(&wrap);nos_cost_end();assert(nos_cost_last().exclusive[NOS_COST_TEXT]==48);
    nos_cost_set(0);nos_cost_begin();{NOS_COST_SCOPE(NOS_COST_TEXT,1);}nos_cost_end();
    f=nos_cost_last();for(unsigned b=0;b<NOS_COST_COUNT;b++)assert(!f.calls[b] && !f.exclusive[b]);
    nos_cost_report();puts("PASS nested exclusive timers, clock wrap, disabled probe");return 0;
}
