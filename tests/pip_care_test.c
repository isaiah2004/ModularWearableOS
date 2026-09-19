#include "pip_care.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static pip_care_input sample(void)
{
    pip_care_input in={0};
    in.now_ms=1000;in.epoch_s=1789308000ULL;in.clock_valid=1;
    in.local_hour=14;in.local_day=20260913;
    in.battery_percent=80;in.battery_valid=1;in.external_power=0;
    return in;
}
static void tick(pip_care_model *m,pip_care_input *in,uint32_t ms)
{
    in->epoch_s+=((uint64_t)(in->now_ms%1000u)+ms)/1000u;
    in->now_ms+=ms;pip_care_step(m,in);
}
static void run(pip_care_model *m,pip_care_input *in,uint32_t ms,uint32_t step)
{
    while(ms) {uint32_t dt=ms<step?ms:step;tick(m,in,dt);ms-=dt;}
}
static void drain(pip_care_model *m,uint32_t now)
{pip_care_event e;while(pip_care_take_event(m,now,&e)) {} }
static void check_bounds(const pip_care_model *m)
{
    assert(m->output.social<=1000&&m->output.stimulation<=1000);
    assert(m->output.rest<=1000&&m->output.comfort<=1000);
    assert(m->output.energy<=1000&&m->output.intensity<=1000);
    assert(m->output.mood<PIP_CARE_MOOD_COUNT&&m->output.reason<PIP_CARE_REASON_COUNT);
    assert(m->memory.history_head<PIP_CARE_HISTORY_COUNT);
    assert(m->memory.history_count<=PIP_CARE_HISTORY_COUNT);
}
static void persistence_tests(void)
{
    pip_care_model m;pip_care_input in=sample();pip_care_memory saved,bad;
    pip_care_init(&m,NULL,&in);assert(m.dirty);
    pip_care_pet(&m,in.now_ms);pip_care_snapshot(&m,&saved);
    assert(pip_care_memory_valid(&saved));assert(m.dirty);
    pip_care_saved(&m);assert(!m.dirty);
    bad=saved;bad.social=1001;assert(!pip_care_memory_valid(&bad));
    bad=saved;bad.history[0].reason=PIP_CARE_REASON_COUNT;assert(!pip_care_memory_valid(&bad));
    bad=saved;bad.checksum^=1;assert(!pip_care_memory_valid(&bad));
    bad=saved;bad.version++;assert(!pip_care_memory_valid(&bad));
    bad=saved;bad.size--;assert(!pip_care_memory_valid(&bad));
    pip_care_model restored;pip_care_init(&restored,&saved,&in);
    assert(restored.memory.social==saved.social&&restored.memory.care_episodes==1);
    pip_care_pet(&restored,in.now_ms+10);
    assert(restored.memory.care_episodes==1); /* reboot cannot farm another reward */
    pip_care_init(&restored,&bad,&in);assert(restored.memory.social==700);
    assert(!pip_care_memory_valid(NULL));
    assert(sizeof(pip_care_model)<1024); /* bounded state fits comfortably in SRAM */
}
static void neglect_and_repair_tests(void)
{
    pip_care_model m;pip_care_input in=sample();pip_care_init(&m,NULL,&in);
    run(&m,&in,11u*3600000u,60000);
    assert(m.output.mood==PIP_CARE_GRUMPY&&m.output.reason==PIP_CARE_REASON_SOCIAL);
    uint16_t before=m.memory.social;
    pip_care_pet(&m,in.now_ms);
    assert(m.output.mood==PIP_CARE_HAPPY&&m.memory.social>before);
    uint16_t after=m.memory.social;
    for(int n=0;n<20;n++)pip_care_pet(&m,in.now_ms+(uint32_t)n);
    assert(m.memory.social==after); /* animation is responsive, reward is throttled */
    run(&m,&in,61000,1000);
    assert(m.output.mood==PIP_CARE_LONELY); /* actual need remains partly unfilled */
    run(&m,&in,PIP_CARE_REWARD_GAP_MS,1000);pip_care_pet(&m,in.now_ms);
    assert(m.memory.social>=500);
    run(&m,&in,61000,1000);assert(m.output.reason!=PIP_CARE_REASON_SOCIAL);
    pip_care_play(&m,in.now_ms);assert(m.output.mood==PIP_CARE_EXCITED);
    check_bounds(&m);
}
static void failed_save_retry_test(void)
{
    pip_care_model m;pip_care_input in=sample();pip_care_init(&m,NULL,&in);
    pip_care_memory first,retry;
    uint32_t committed=m.memory.generation;
    pip_care_snapshot(&m,&first);
    assert(first.generation==committed+1u&&pip_care_memory_valid(&first));
    assert(m.memory.generation==committed&&m.dirty);
    /* An unsuccessful NVS write gets no acknowledgement. Every retry must
     * target the same alternating slot, preserving the previous good slot. */
    for(int n=0;n<4;n++) {
        pip_care_snapshot(&m,&retry);
        assert(retry.generation==first.generation);
        assert((retry.generation&1u)==(first.generation&1u));
        assert(retry.checksum==first.checksum&&pip_care_memory_valid(&retry));
        assert(m.memory.generation==committed&&m.dirty);
    }
    pip_care_saved(&m);
    assert(m.memory.generation==first.generation&&!m.dirty);
    assert(pip_care_memory_valid(&m.memory));
    assert(m.memory.checksum==first.checksum);
    run(&m,&in,120000,1000);assert(m.dirty);
    pip_care_snapshot(&m,&retry);
    assert(retry.generation==first.generation+1u);
    assert((retry.generation&1u)!=(first.generation&1u));
    assert(m.memory.generation==first.generation&&m.dirty);
}
static void clock_and_routine_tests(void)
{
    pip_care_model m;pip_care_input in=sample();pip_care_init(&m,NULL,&in);
    run(&m,&in,2u*3600000u,60000);uint16_t social=m.memory.social;
    in.epoch_s+=7u*86400u;tick(&m,&in,1000);
    assert(m.memory.social==social); /* clock jump does not become a week of neglect */
    in.epoch_s-=14u*86400u;tick(&m,&in,1000);assert(m.memory.social==social);
    in.local_hour=23;run(&m,&in,10u*3600000u,60000);
    assert(m.memory.social==social&&m.memory.rest==1000);
    assert(m.output.quiet&&m.output.mood==PIP_CARE_SLEEPY);
    pip_care_memory saved;pip_care_snapshot(&m,&saved);
    in.now_ms=1000;in.epoch_s=saved.last_epoch_s+3u*86400u;in.local_hour=14;
    pip_care_init(&m,&saved,&in);
    assert(m.memory.social>=saved.social-150); /* capped, reduced offline decay */
    assert(m.memory.rest==1000);
    in.epoch_s=saved.last_epoch_s-100;pip_care_init(&m,&saved,&in);
    assert(m.memory.social==saved.social);
    in.epoch_s=saved.last_epoch_s+365u*86400u;pip_care_init(&m,&saved,&in);
    assert(m.memory.social==saved.social); /* ambiguous huge correction */
    in.clock_valid=0;in.epoch_s=0;pip_care_init(&m,&saved,&in);
    assert(m.memory.social==saved.social&&!m.output.quiet);
    in.clock_valid=1;in.epoch_s=1789308000ULL;tick(&m,&in,1000);
    assert(m.memory.social==saved.social); /* first synchronization is harmless */
    in.now_ms=0xfffff000u;pip_care_init(&m,NULL,&in);
    run(&m,&in,20000,20);check_bounds(&m);
}
static void cadence_tests(void)
{
    pip_care_model a,b,c;pip_care_input ia=sample(),ib=ia,ic=ia;
    pip_care_init(&a,NULL,&ia);pip_care_init(&b,NULL,&ib);pip_care_init(&c,NULL,&ic);
    run(&a,&ia,6u*3600000u,1000);
    run(&b,&ib,6u*3600000u,50);
    run(&c,&ic,6u*3600000u,16);
    assert(a.memory.social==b.memory.social&&a.memory.social==c.memory.social);
    assert(a.memory.stimulation==b.memory.stimulation&&a.memory.stimulation==c.memory.stimulation);
    assert(a.memory.rest==b.memory.rest&&a.memory.rest==c.memory.rest);
    assert(a.memory.comfort==b.memory.comfort&&a.memory.comfort==c.memory.comfort);
}
static void offline_quiet_and_greeting_tests(void)
{
    pip_care_model m;pip_care_input in=sample();pip_care_memory saved;
    in.local_hour=22;in.local_minute=30;pip_care_init(&m,NULL,&in);
    pip_care_snapshot(&m,&saved);
    in.epoch_s=saved.last_epoch_s+8u*3600u;in.local_hour=6;in.local_minute=30;
    pip_care_init(&m,&saved,&in);
    assert(m.memory.social==saved.social&&m.memory.stimulation==saved.stimulation);
    assert(m.memory.rest==1000); /* 22:30 to06:30 is entirely quiet, including IST */
    in.epoch_s=saved.last_epoch_s+9u*3600u;in.local_hour=7;in.local_minute=30;
    pip_care_init(&m,&saved,&in);
    assert(m.memory.social==saved.social-8); /* only the final awake half-hour counts */

    in=sample();in.local_hour=11;pip_care_init(&m,NULL,&in);
    m.memory.awake_start_hour=22;m.memory.awake_end_hour=7;
    pip_care_snapshot(&m,&saved);
    in.epoch_s+=6u*3600u;in.local_hour=17;pip_care_init(&m,&saved,&in);
    assert(m.memory.social==saved.social&&m.output.quiet); /* night-shift routine */

    in=sample();pip_care_init(&m,NULL,&in);pip_care_greet(&m,in.now_ms);
    assert(m.memory.social==730);
    tick(&m,&in,60000);pip_care_greet(&m,in.now_ms);
    assert(m.memory.social==730); /* greeting frequency is not the reward frequency */
    run(&m,&in,240000,1000);uint16_t before=m.memory.social;
    pip_care_greet(&m,in.now_ms);assert(m.memory.social==before+30);
    before=m.memory.social;pip_care_pet(&m,in.now_ms);
    assert(m.memory.social==before+100); /* greeting never consumes the pet-care reward */
    pip_care_snapshot(&m,&saved);in.visible=1;
    pip_care_init(&m,&saved,&in);assert(m.memory.social==saved.social);
}
static void power_tests(void)
{
    pip_care_model m;pip_care_input in=sample();in.visible=1;
    in.battery_valid=0;in.external_power=-1;in.battery_percent=0;
    pip_care_init(&m,NULL,&in);run(&m,&in,3600000,1000);
    assert(!m.output.charge_requested&&!m.output.low_power);
    in.battery_valid=1;in.external_power=0;in.battery_percent=10;tick(&m,&in,20);
    in.battery_percent=80;run(&m,&in,60000,1000);assert(!m.output.charge_requested);
    in.battery_percent=20;in.visible=0;run(&m,&in,60000,1000);
    assert(!m.output.charge_requested); /* request needs an observed opportunity */
    in.visible=1;tick(&m,&in,1000);assert(m.output.charge_requested&&!m.output.charge_overdue);
    run(&m,&in,600000,1000);uint32_t observed=m.memory.request_observed_ms;
    in.battery_valid=0;in.external_power=-1;run(&m,&in,3600000,1000);
    assert(m.memory.request_observed_ms==observed&&!m.output.charge_overdue);
    in.battery_valid=1;in.external_power=0;
    pip_care_memory saved;pip_care_snapshot(&m,&saved);
    in.now_ms=1000;in.epoch_s=saved.last_epoch_s+6u*3600u;
    pip_care_init(&m,&saved,&in);
    assert(m.memory.request_observed_ms==observed&&!m.output.charge_overdue);
    run(&m,&in,1200000,1000);
    assert(m.output.charge_overdue&&m.output.mood==PIP_CARE_GRUMPY);
    assert(m.output.reason==PIP_CARE_REASON_POWER);
    pip_care_pet(&m,in.now_ms);assert(m.output.mood==PIP_CARE_HAPPY);
    run(&m,&in,6000,1000);
    assert(m.output.charge_overdue&&m.output.mood==PIP_CARE_GRUMPY);
    assert(m.output.reason==PIP_CARE_REASON_POWER);
    pip_care_play(&m,in.now_ms);assert(m.output.mood==PIP_CARE_EXCITED);
    run(&m,&in,6000,1000);
    assert(m.output.charge_overdue&&m.output.mood==PIP_CARE_GRUMPY);
    in.local_hour=23;tick(&m,&in,1000);
    assert(m.output.mood==PIP_CARE_SLEEPY); /* quiet hours do not nag */
    in.external_power=1;in.battery_valid=0;tick(&m,&in,1000);
    assert(!m.output.charge_requested&&!m.output.charge_overdue);
    assert(m.output.mood==PIP_CARE_RELIEVED);
    pip_care_event event;int relief=0;
    while(pip_care_take_event(&m,in.now_ms,&event)) {
        assert(event.type!=PIP_CARE_EVENT_LOW_POWER&&event.type!=PIP_CARE_EVENT_OVERDUE);
        if(event.type==PIP_CARE_EVENT_POWER_RELIEF)relief++;
    }
    assert(relief==1);run(&m,&in,180000,1000);
    assert(!pip_care_take_event(&m,in.now_ms,&event));
    check_bounds(&m);

    in=sample();in.visible=1;in.battery_percent=10;pip_care_init(&m,NULL,&in);
    run(&m,&in,60000,1000);assert(m.output.charge_requested);
    in.battery_percent=35;run(&m,&in,59000,1000);assert(m.output.charge_requested);
    tick(&m,&in,1000);assert(!m.output.charge_requested);
    in.battery_percent=10;run(&m,&in,60000,1000);assert(m.output.charge_requested);
    uint32_t old=m.memory.request_observed_ms;
    tick(&m,&in,6u*3600000u);
    assert(m.memory.request_observed_ms==old); /* a missed service interval is unobserved */
}
static void walk_seconds(pip_care_model *m,pip_care_input *in,unsigned seconds)
{
    for(unsigned n=0;n<seconds;n++) {
        in->active_seconds++;in->steps+=2;tick(m,in,1000);
    }
}
static void activity_tests(void)
{
    pip_care_model m;pip_care_input in=sample();in.activity_valid=1;in.walking=1;
    pip_care_init(&m,NULL,&in);walk_seconds(&m,&in,130);
    assert(m.memory.walk_rewards==1&&m.output.mood==PIP_CARE_HAPPY);
    assert(m.output.reason==PIP_CARE_REASON_WALK);
    walk_seconds(&m,&in,150);assert(m.memory.walk_rewards==1);
    pip_care_memory saved;pip_care_snapshot(&m,&saved);
    in.now_ms=1000;in.epoch_s=saved.last_epoch_s+1;
    pip_care_init(&m,&saved,&in);
    assert(m.memory.walk_rewards==1&&m.output.reason==PIP_CARE_REASON_WALK);
    walk_seconds(&m,&in,150);assert(m.memory.walk_rewards==1); /* no reboot duplicate */
    in.walking=0;run(&m,&in,1800000,1000);
    in.walking=1;walk_seconds(&m,&in,130);assert(m.memory.walk_rewards==2);

    in=sample();in.activity_valid=1;in.walking=1;pip_care_init(&m,NULL,&in);
    tick(&m,&in,1000);in.active_seconds+=120;tick(&m,&in,1000);
    assert(m.memory.walk_rewards==0); /* large counter jump is not an outing */
    in.walking=0;walk_seconds(&m,&in,300);assert(m.memory.walk_rewards==0);
    in.walking=1;in.activity_valid=0;walk_seconds(&m,&in,300);assert(m.memory.walk_rewards==0);
    in.activity_valid=1;in.active_seconds=0;in.steps=0;tick(&m,&in,1000);
    /* Per-frame injected increments cannot exceed observed walking time. */
    for(int n=0;n<5000;n++){in.active_seconds++;tick(&m,&in,20);}
    assert(m.memory.walk_rewards==0);
    check_bounds(&m);
}
static void event_tests(void)
{
    pip_care_model m;pip_care_input in=sample();in.visible=1;pip_care_init(&m,NULL,&in);
    pip_care_event event;drain(&m,in.now_ms);
    pip_care_notify(&m,0,in.now_ms);pip_care_alarm(&m,0,in.now_ms);
    assert(!pip_care_take_event(&m,in.now_ms,&event)); /* sequence0 means no arrivals */
    uint32_t history_count=m.memory.history_count,history_head=m.memory.history_head;
    for(uint32_t n=1;n<=12;n++)pip_care_notify(&m,n,in.now_ms);
    assert(m.memory.history_count==history_count&&m.memory.history_head==history_head);
    pip_care_notify(&m,12,in.now_ms); /* duplicate */
    pip_care_alarm(&m,1,in.now_ms);
    assert(pip_care_take_event(&m,in.now_ms,&event)&&event.type==PIP_CARE_EVENT_ALARM);
    assert(pip_care_take_event(&m,in.now_ms,&event)&&event.type==PIP_CARE_EVENT_NOTIFICATION&&event.count==12);
    assert(!pip_care_take_event(&m,in.now_ms,&event));
    pip_care_alarm(&m,1,in.now_ms);assert(!pip_care_take_event(&m,in.now_ms,&event));
    pip_care_notify(&m,13,in.now_ms);
    assert(!pip_care_take_event(&m,in.now_ms+6000,&event)); /* expired cosmetic cue */
    in.visible=0;tick(&m,&in,20);pip_care_notify(&m,14,in.now_ms);
    in.visible=1;tick(&m,&in,20);drain(&m,in.now_ms);
    pip_care_notify(&m,14,in.now_ms);assert(!pip_care_take_event(&m,in.now_ms,&event));
    in.local_hour=23;tick(&m,&in,20);pip_care_notify(&m,15,in.now_ms);
    assert(!pip_care_take_event(&m,in.now_ms,&event));
    pip_care_alarm(&m,2,in.now_ms);
    assert(pip_care_take_event(&m,in.now_ms,&event)&&event.type==PIP_CARE_EVENT_ALARM);
    in.local_hour=14;pip_care_init(&m,NULL,&in);drain(&m,in.now_ms);
    pip_care_notify(&m,0xfffffffeu,in.now_ms);pip_care_notify(&m,0,in.now_ms);
    assert(pip_care_take_event(&m,in.now_ms,&event)&&event.count==3);
    for(uint32_t n=0;n<20;n++) {
        tick(&m,&in,30000);pip_care_notify(&m,n+1,in.now_ms);
        pip_care_alarm(&m,n+1,in.now_ms);
    }
    assert(m.memory.history_count==PIP_CARE_HISTORY_COUNT);check_bounds(&m);
    pip_care_memory saved;pip_care_snapshot(&m,&saved);assert(pip_care_memory_valid(&saved));
}
static void unresolved_social_test(void)
{
    pip_care_model m;pip_care_input in=sample();pip_care_init(&m,NULL,&in);
    /* Restore a valid, deeply neglected pet with enough physical rest. */
    m.memory.social=100;pip_care_memory saved;pip_care_snapshot(&m,&saved);
    assert(pip_care_memory_valid(&saved));pip_care_init(&m,&saved,&in);
    assert(m.output.mood==PIP_CARE_GRUMPY);
    pip_care_pet(&m,in.now_ms);assert(m.memory.social==200&&m.output.mood==PIP_CARE_HAPPY);
    run(&m,&in,6000,1000);
    assert(m.output.mood==PIP_CARE_GRUMPY&&m.output.reason==PIP_CARE_REASON_SOCIAL);
    pip_care_play(&m,in.now_ms);assert(m.output.mood==PIP_CARE_EXCITED);
    run(&m,&in,6000,1000);assert(m.output.mood==PIP_CARE_GRUMPY);
}
int main(void)
{
    persistence_tests();failed_save_retry_test();neglect_and_repair_tests();clock_and_routine_tests();cadence_tests();
    offline_quiet_and_greeting_tests();power_tests();activity_tests();event_tests();unresolved_social_test();
    puts("PASS Pip care: persistence/corruption, neglect/repair, six-hour cadence, clock/quiet/offline, observed charging, walking budgets/reboot, event priorities/batching");
    return 0;
}
