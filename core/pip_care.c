#include "pip_care.h"
#include <stddef.h>
#include <string.h>

#define CARE_MAGIC 0x50434132u
#define CARE_VERSION 2u
#define HOUR_MS 3600000u
#define COUNTER_MAX 10000000u

static uint32_t cap32(uint64_t value, uint32_t cap)
{ return value > cap ? cap : (uint32_t)value; }
static uint16_t add_need(uint16_t value, uint32_t amount)
{ return (uint16_t)cap32((uint32_t)value + amount, PIP_CARE_MAX); }
static uint16_t sub_need(uint16_t value, uint32_t amount)
{ return amount > value ? 0 : (uint16_t)(value - amount); }
static int epoch_valid(uint64_t epoch)
{ return epoch >= 946684800ULL && epoch <= 4102444799ULL; }
static int input_clock_valid(const pip_care_input *in)
{ return in->clock_valid && epoch_valid(in->epoch_s); }
static uint32_t hash_value(uint32_t hash, uint64_t value)
{
    for(int i=0;i<8;i++) {hash ^= (uint8_t)value;hash *= 16777619u;value >>= 8;}
    return hash;
}
static uint32_t memory_checksum(const pip_care_memory *p)
{
    uint32_t h=2166136261u;
#define HASH(field) h=hash_value(h,p->field)
    HASH(last_epoch_s);HASH(last_walk_epoch_s);HASH(last_care_epoch_s);HASH(last_greet_epoch_s);
    HASH(magic);HASH(version);HASH(generation);HASH(size);
    HASH(social);HASH(stimulation);HASH(rest);HASH(comfort);
    HASH(request_state);HASH(request_observed_ms);HASH(last_request_day);
    HASH(last_walk_day);HASH(walk_rewards);HASH(care_episodes);HASH(last_greet_day);
    HASH(history_head);HASH(history_count);HASH(clock_valid);
    HASH(awake_start_hour);HASH(awake_end_hour);HASH(reserved);
#undef HASH
    for(int i=0;i<PIP_CARE_HISTORY_COUNT;i++) {
        h=hash_value(h,p->history[i].epoch_s);
        h=hash_value(h,p->history[i].reason);
        h=hash_value(h,p->history[i].event);
    }
    return h;
}
int pip_care_memory_valid(const pip_care_memory *p)
{
    if(!p || p->magic!=CARE_MAGIC || p->version!=CARE_VERSION || p->size!=sizeof *p ||
       p->social>PIP_CARE_MAX || p->stimulation>PIP_CARE_MAX ||
       p->rest>PIP_CARE_MAX || p->comfort>PIP_CARE_MAX || p->request_state>2 ||
       p->request_observed_ms>PIP_CARE_CHARGE_GRACE_MS ||
       p->history_head>=PIP_CARE_HISTORY_COUNT || p->history_count>PIP_CARE_HISTORY_COUNT ||
       p->clock_valid>1 || (p->clock_valid && !epoch_valid(p->last_epoch_s)) ||
       p->awake_start_hour>23 || p->awake_end_hour>23 ||
       p->awake_start_hour==p->awake_end_hour || p->walk_rewards>COUNTER_MAX ||
       p->care_episodes>COUNTER_MAX || p->reserved ||
       (p->last_walk_epoch_s && !epoch_valid(p->last_walk_epoch_s)) ||
       (p->last_care_epoch_s && !epoch_valid(p->last_care_epoch_s)) ||
       (p->last_greet_epoch_s && !epoch_valid(p->last_greet_epoch_s))) return 0;
    for(int i=0;i<PIP_CARE_HISTORY_COUNT;i++) {
        const pip_care_history *h=&p->history[i];
        if(h->reason>=PIP_CARE_REASON_COUNT || h->event>=PIP_CARE_EVENT_COUNT ||
           (h->epoch_s && !epoch_valid(h->epoch_s))) return 0;
    }
    return p->checksum==memory_checksum(p);
}
static int quiet_now(const pip_care_model *m)
{
    int hour=m->input.local_hour;
    if(!input_clock_valid(&m->input) || hour<0 || hour>23) return 0;
    int start=(int)m->memory.awake_start_hour,end=(int)m->memory.awake_end_hour;
    return start<end ? hour<start || hour>=end : hour<start && hour>=end;
}
static int credible_battery(const pip_care_model *m)
{
    return m->input.battery_valid && m->input.battery_percent>=0 &&
        m->input.battery_percent<=100 && m->input.external_power==0;
}
static int priority(pip_care_event_type event)
{
    switch(event) {
    case PIP_CARE_EVENT_ALARM:return 100;
    case PIP_CARE_EVENT_OVERDUE:return 90;
    case PIP_CARE_EVENT_LOW_POWER:return 80;
    case PIP_CARE_EVENT_POWER_RELIEF:return 70;
    case PIP_CARE_EVENT_PETTED:return 60;
    case PIP_CARE_EVENT_WALK:return 50;
    case PIP_CARE_EVENT_PLAY:return 45;
    case PIP_CARE_EVENT_GREETING:return 30;
    case PIP_CARE_EVENT_NOTIFICATION:return 20;
    default:return 0;
    }
}
static uint32_t event_lifetime(pip_care_event_type event)
{
    if(event==PIP_CARE_EVENT_WALK)return 180000u;
    if(event==PIP_CARE_EVENT_LOW_POWER || event==PIP_CARE_EVENT_OVERDUE)return 90000u;
    if(event==PIP_CARE_EVENT_ALARM || event==PIP_CARE_EVENT_POWER_RELIEF)return 30000u;
    return event==PIP_CARE_EVENT_GREETING ? 8000u : 5000u;
}
static void history(pip_care_model *m,pip_care_event_type event,pip_care_reason reason)
{
    pip_care_history *h=&m->memory.history[m->memory.history_head];
    h->epoch_s=input_clock_valid(&m->input)?m->input.epoch_s:0;
    h->reason=(uint32_t)reason;h->event=(uint32_t)event;
    m->memory.history_head=(m->memory.history_head+1)%PIP_CARE_HISTORY_COUNT;
    if(m->memory.history_count<PIP_CARE_HISTORY_COUNT)m->memory.history_count++;
    m->dirty=1;
}
static void enqueue(pip_care_model *m,pip_care_event_type type,
                    pip_care_reason reason,uint32_t count,uint32_t now)
{
    int slot=-1,worst=0;
    for(int i=0;i<4;i++) {
        if(m->pending[i].type && now-m->pending_ms[i]>event_lifetime(m->pending[i].type))
            m->pending[i].type=PIP_CARE_EVENT_NONE;
        if(m->pending[i].type==type) {
            m->pending[i].count=cap32((uint64_t)m->pending[i].count+count,1000);
            m->pending_ms[i]=now;return;
        }
        if(!m->pending[i].type)slot=i;
        if(priority(m->pending[i].type)<priority(m->pending[worst].type))worst=i;
    }
    if(slot<0) {
        if(priority(type)<=priority(m->pending[worst].type))return;
        slot=worst;
    }
    m->pending[slot]=(pip_care_event){type,reason,count};m->pending_ms[slot]=now;
}
int pip_care_take_event(pip_care_model *m,uint32_t now,pip_care_event *out)
{
    int best=-1;
    if(!m || !out)return 0;
    for(int i=0;i<4;i++) {
        if(m->pending[i].type && now-m->pending_ms[i]>event_lifetime(m->pending[i].type))
            m->pending[i].type=PIP_CARE_EVENT_NONE;
        if(m->pending[i].type && (best<0 || priority(m->pending[i].type)>priority(m->pending[best].type)))best=i;
    }
    if(best<0)return 0;
    *out=m->pending[best];m->pending[best].type=PIP_CARE_EVENT_NONE;return 1;
}
static void positive(pip_care_model *m,pip_care_mood mood,pip_care_reason reason,
                     uint32_t duration,uint32_t now)
{
    m->positive_mood=mood;m->positive_reason=reason;
    m->positive_ms=now;m->positive_duration_ms=duration;
}
static void output(pip_care_model *m,uint32_t now)
{
    pip_care_output *o=&m->output;
    o->social=m->memory.social;o->stimulation=m->memory.stimulation;
    o->rest=m->memory.rest;o->comfort=m->memory.comfort;
    o->quiet=quiet_now(m);o->low_power=credible_battery(m)&&m->input.battery_percent<=PIP_CARE_LOW_PERCENT;
    o->charge_requested=m->memory.request_state!=0;o->charge_overdue=m->memory.request_state==2;
    o->energy=m->memory.rest;
    if(credible_battery(m)) {
        uint16_t power=(uint16_t)(250+m->input.battery_percent*750/100);
        if(power<o->energy)o->energy=power;
    }
    o->mood=PIP_CARE_CONTENT;o->reason=PIP_CARE_REASON_NONE;o->intensity=250;
    if(o->quiet || m->memory.rest<300) {
        o->mood=PIP_CARE_SLEEPY;o->reason=PIP_CARE_REASON_REST;
        o->intensity=o->quiet?600:(uint16_t)(1000-m->memory.rest);
    } else if(o->charge_overdue && o->low_power) {
        o->mood=PIP_CARE_GRUMPY;o->reason=PIP_CARE_REASON_POWER;o->intensity=850;
    } else if(m->memory.social<350) {
        o->mood=PIP_CARE_GRUMPY;o->reason=PIP_CARE_REASON_SOCIAL;
        o->intensity=(uint16_t)(1000-m->memory.social);
    } else if(m->memory.social<500) {
        o->mood=PIP_CARE_LONELY;o->reason=PIP_CARE_REASON_SOCIAL;
        o->intensity=(uint16_t)(1000-m->memory.social);
    } else if(o->low_power) {
        o->mood=PIP_CARE_SLEEPY;o->reason=PIP_CARE_REASON_POWER;o->intensity=650;
    } else if(m->memory.stimulation<300) {
        o->mood=PIP_CARE_LONELY;o->reason=PIP_CARE_REASON_STIMULATION;o->intensity=500;
    }
    if(o->mood==PIP_CARE_CONTENT && input_clock_valid(&m->input) &&
       m->memory.last_walk_epoch_s && m->input.epoch_s>=m->memory.last_walk_epoch_s &&
       m->input.epoch_s-m->memory.last_walk_epoch_s<600) {
        o->mood=PIP_CARE_HAPPY;o->reason=PIP_CARE_REASON_WALK;o->intensity=500;
    }
    if(m->positive_duration_ms && now-m->positive_ms<m->positive_duration_ms) {
        o->mood=m->positive_mood;o->reason=m->positive_reason;o->intensity=700;
    }
}
static uint32_t rate_amount(uint32_t ms,uint32_t per_hour,uint32_t *remainder)
{
    uint64_t value=(uint64_t)ms*per_hour+*remainder;
    *remainder=(uint32_t)(value%HOUR_MS);
    return (uint32_t)(value/HOUR_MS);
}
static void advance_needs(pip_care_model *m,uint32_t ms,int quiet)
{
    uint16_t old_social=m->memory.social,old_stimulation=m->memory.stimulation;
    uint16_t old_rest=m->memory.rest,old_comfort=m->memory.comfort;
    if(!quiet) {
        m->memory.social=sub_need(m->memory.social,rate_amount(ms,PIP_CARE_SOCIAL_PER_HOUR,&m->social_fraction));
        m->memory.stimulation=sub_need(m->memory.stimulation,rate_amount(ms,PIP_CARE_STIMULATION_PER_HOUR,&m->stimulation_fraction));
        m->memory.rest=sub_need(m->memory.rest,rate_amount(ms,PIP_CARE_REST_PER_HOUR,&m->rest_fraction));
    } else {
        m->memory.rest=add_need(m->memory.rest,rate_amount(ms,PIP_CARE_REST_RECOVERY_PER_HOUR,&m->rest_fraction));
    }
    uint32_t relax=rate_amount(ms,15,&m->comfort_fraction);
    if(m->memory.comfort<650) {
        m->memory.comfort=add_need(m->memory.comfort,relax);
        if(m->memory.comfort>650)m->memory.comfort=650;
    }
    else if(m->memory.comfort>650) {
        m->memory.comfort=sub_need(m->memory.comfort,relax);
        if(m->memory.comfort<650)m->memory.comfort=650;
    }
    if(old_social!=m->memory.social || old_stimulation!=m->memory.stimulation ||
       old_rest!=m->memory.rest || old_comfort!=m->memory.comfort)m->dirty=1;
}
static void power_step(pip_care_model *m,uint32_t elapsed,uint32_t now)
{
    int valid=credible_battery(m),low=valid&&m->input.battery_percent<=PIP_CARE_LOW_PERCENT;
    int quiet=quiet_now(m);
    if(valid && m->input.battery_percent>=PIP_CARE_RECOVERED_PERCENT)
        m->recovered_ms=cap32((uint64_t)m->recovered_ms+elapsed,PIP_CARE_LOW_DWELL_MS);
    else m->recovered_ms=0;
    if(m->memory.request_state && (m->input.external_power==1 || m->recovered_ms>=PIP_CARE_LOW_DWELL_MS)) {
        m->memory.request_state=0;m->memory.request_observed_ms=0;
        m->low_ms=0;m->memory.comfort=add_need(m->memory.comfort,80);
        /* A resolved episode must not replay an old request on the next visit. */
        for(int i=0;i<4;i++)if(m->pending[i].type==PIP_CARE_EVENT_LOW_POWER ||
            m->pending[i].type==PIP_CARE_EVENT_OVERDUE)m->pending[i].type=PIP_CARE_EVENT_NONE;
        positive(m,PIP_CARE_RELIEVED,PIP_CARE_REASON_POWER,120000,now);
        enqueue(m,PIP_CARE_EVENT_POWER_RELIEF,PIP_CARE_REASON_POWER,1,now);
        history(m,PIP_CARE_EVENT_POWER_RELIEF,PIP_CARE_REASON_POWER);
        return;
    }
    if(!low) {m->low_ms=0;return;}
    m->low_ms=cap32((uint64_t)m->low_ms+elapsed,PIP_CARE_LOW_DWELL_MS);
    if(!m->memory.request_state) {
        if(m->low_ms>=PIP_CARE_LOW_DWELL_MS && !quiet && m->input.visible) {
            m->memory.request_state=1;m->memory.request_observed_ms=0;
            m->memory.last_request_day=m->input.local_day;
            enqueue(m,PIP_CARE_EVENT_LOW_POWER,PIP_CARE_REASON_POWER,1,now);
            history(m,PIP_CARE_EVENT_LOW_POWER,PIP_CARE_REASON_POWER);
        }
    } else if(m->memory.request_state==1 && !quiet) {
        m->memory.request_observed_ms=cap32((uint64_t)m->memory.request_observed_ms+elapsed,PIP_CARE_CHARGE_GRACE_MS);
        m->dirty=1;
        if(m->memory.request_observed_ms>=PIP_CARE_CHARGE_GRACE_MS) {
            m->memory.request_state=2;
            enqueue(m,PIP_CARE_EVENT_OVERDUE,PIP_CARE_REASON_POWER,1,now);
            history(m,PIP_CARE_EVENT_OVERDUE,PIP_CARE_REASON_POWER);
        }
    }
}
static void movement_step(pip_care_model *m,uint32_t elapsed,uint32_t now)
{
    uint32_t slot=now/30000u;
    uint32_t crossed=slot-m->walk_slot;
    if(slot<m->walk_slot || crossed>=6)memset(m->walk_buckets,0,sizeof m->walk_buckets);
    else for(uint32_t n=1;n<=crossed;n++)m->walk_buckets[(m->walk_slot+n)%6]=0;
    m->walk_slot=slot;
    if(!m->input.activity_valid) {
        m->have_activity=0;m->activity_budget_ms=0;
        memset(m->walk_buckets,0,sizeof m->walk_buckets);return;
    }
    if(!m->have_activity) {
        m->active_baseline=m->input.active_seconds;m->steps_baseline=m->input.steps;
        m->have_activity=1;m->activity_budget_ms=0;return;
    }
    uint32_t delta=m->input.active_seconds-m->active_baseline;
    int reset=m->input.active_seconds<m->active_baseline || m->input.steps<m->steps_baseline;
    m->active_baseline=m->input.active_seconds;m->steps_baseline=m->input.steps;
    if(reset || delta>elapsed/1000u+2u || elapsed>30000u) {
        m->activity_budget_ms=0;memset(m->walk_buckets,0,sizeof m->walk_buckets);return;
    }
    if(m->input.walking) {
        m->activity_budget_ms=cap32((uint64_t)m->activity_budget_ms+elapsed,30000u);
        if((uint64_t)delta*1000u>m->activity_budget_ms)return;
        m->activity_budget_ms-=delta*1000u;
        uint32_t value=m->walk_buckets[slot%6]+delta;
        m->walk_buckets[slot%6]=(uint16_t)(value>30?30:value);
    } else m->activity_budget_ms=0;
    uint32_t total=0;for(int n=0;n<6;n++)total+=m->walk_buckets[n];
    if(total<PIP_CARE_WALK_REQUIRED_S || (m->have_walk && now-m->last_walk_ms<PIP_CARE_WALK_COOLDOWN_MS))return;
    m->have_walk=1;m->last_walk_ms=now;memset(m->walk_buckets,0,sizeof m->walk_buckets);
    m->memory.stimulation=add_need(m->memory.stimulation,180);
    m->memory.social=add_need(m->memory.social,80);m->memory.comfort=add_need(m->memory.comfort,40);
    m->memory.last_walk_day=m->input.local_day;
    m->memory.last_walk_epoch_s=input_clock_valid(&m->input)?m->input.epoch_s:0;
    if(m->memory.walk_rewards<COUNTER_MAX)m->memory.walk_rewards++;
    positive(m,PIP_CARE_HAPPY,PIP_CARE_REASON_WALK,600000,now);
    enqueue(m,PIP_CARE_EVENT_WALK,PIP_CARE_REASON_WALK,1,now);
    history(m,PIP_CARE_EVENT_WALK,PIP_CARE_REASON_WALK);
}
static uint64_t interval_overlap(uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{
    uint64_t start=a>c?a:c,end=b<d?b:d;
    return end>start?end-start:0;
}
static void offline_advance(pip_care_model *m,uint64_t delta)
{
    /* Current local wall time anchors the interval, including half-hour offsets.
     * Count complete days arithmetically; at most two days remain to intersect. */
    if(m->input.local_hour<0 || m->input.local_hour>23 ||
       m->input.local_minute<0 || m->input.local_minute>59)return;
    uint32_t start=m->memory.awake_start_hour*3600u,end=m->memory.awake_end_hour*3600u;
    uint32_t daily_awake=start<end?end-start:86400u-start+end;
    uint32_t current=(uint32_t)m->input.local_hour*3600u+
        (uint32_t)m->input.local_minute*60u+(uint32_t)(m->input.epoch_s%60u);
    uint32_t remainder=(uint32_t)(delta%86400u);
    uint32_t beginning=(current+86400u-remainder)%86400u;
    uint64_t awake=delta/86400u*daily_awake;
    for(uint32_t day=0;day<2;day++) {
        uint64_t base=day*86400u;
        if(start<end)awake+=interval_overlap(beginning,(uint64_t)beginning+remainder,base+start,base+end);
        else {
            awake+=interval_overlap(beginning,(uint64_t)beginning+remainder,base,base+end);
            awake+=interval_overlap(beginning,(uint64_t)beginning+remainder,base+start,base+86400u);
        }
    }
    uint32_t awake_capped=cap32(awake,PIP_CARE_OFFLINE_CAP_S);
    uint32_t quiet_capped=cap32(delta-awake,PIP_CARE_OFFLINE_CAP_S);
    m->memory.social=sub_need(m->memory.social,awake_capped*PIP_CARE_SOCIAL_PER_HOUR/(3600u*2u));
    m->memory.stimulation=sub_need(m->memory.stimulation,awake_capped*PIP_CARE_STIMULATION_PER_HOUR/(3600u*2u));
    m->memory.rest=add_need(m->memory.rest,quiet_capped*PIP_CARE_REST_RECOVERY_PER_HOUR/3600u);
    if(awake_capped || quiet_capped)m->dirty=1;
}
void pip_care_init(pip_care_model *m,const pip_care_memory *memory,const pip_care_input *in)
{
    pip_care_memory saved;int valid=pip_care_memory_valid(memory);
    if(valid)saved=*memory;
    memset(m,0,sizeof *m);m->input=*in;m->last_ms=in->now_ms;m->walk_slot=in->now_ms/30000u;
    m->initialized=1;m->dirty=!valid;
    if(valid)m->memory=saved;
    else {
        m->memory.magic=CARE_MAGIC;m->memory.version=CARE_VERSION;m->memory.size=sizeof m->memory;
        m->memory.social=700;m->memory.stimulation=700;m->memory.rest=800;m->memory.comfort=650;
        m->memory.awake_start_hour=7;m->memory.awake_end_hour=22;
    }
    if(valid && memory->clock_valid && input_clock_valid(in) && in->epoch_s>memory->last_epoch_s) {
        uint64_t delta=in->epoch_s-memory->last_epoch_s;
        /* Very large changes are ambiguous clock corrections; preserve the pet. */
        if(delta<=30ULL*86400)offline_advance(m,delta);
    }
    /* Reboot does not forgive observed waiting, nor add unobserved waiting. */
    if(m->memory.walk_rewards) {m->have_walk=1;m->last_walk_ms=in->now_ms;}
    if(input_clock_valid(in) && m->memory.last_walk_epoch_s && in->epoch_s>=m->memory.last_walk_epoch_s) {
        uint64_t age=in->epoch_s-m->memory.last_walk_epoch_s;
        m->have_walk=age<PIP_CARE_WALK_COOLDOWN_MS/1000u;
        if(m->have_walk)m->last_walk_ms=in->now_ms-(uint32_t)age*1000u;
    }
    if(m->memory.care_episodes) {m->have_reward=1;m->last_reward_ms=in->now_ms;}
    if(input_clock_valid(in) && m->memory.last_care_epoch_s && in->epoch_s>=m->memory.last_care_epoch_s) {
        uint64_t age=in->epoch_s-m->memory.last_care_epoch_s;
        m->have_reward=age<PIP_CARE_REWARD_GAP_MS/1000u;
        if(m->have_reward)m->last_reward_ms=in->now_ms-(uint32_t)age*1000u;
    }
    if(m->memory.last_greet_day || m->memory.last_greet_epoch_s) {
        m->have_greeting_reward=1;m->last_greeting_reward_ms=in->now_ms;
    }
    if(input_clock_valid(in) && m->memory.last_greet_epoch_s && in->epoch_s>=m->memory.last_greet_epoch_s) {
        uint64_t age=in->epoch_s-m->memory.last_greet_epoch_s;
        m->have_greeting_reward=age<PIP_CARE_REWARD_GAP_MS/1000u;
        if(m->have_greeting_reward)m->last_greeting_reward_ms=in->now_ms-(uint32_t)age*1000u;
    }
    m->memory.clock_valid=input_clock_valid(in);
    m->memory.last_epoch_s=m->memory.clock_valid?in->epoch_s:0;
    m->last_visible=in->visible;
    if(in->visible)pip_care_greet(m,in->now_ms);
    output(m,in->now_ms);
}
void pip_care_step(pip_care_model *m,const pip_care_input *in)
{
    if(!m->initialized) {pip_care_init(m,NULL,in);return;}
    uint32_t elapsed=in->now_ms-m->last_ms;
    m->last_ms=in->now_ms;m->input=*in;
    /* Continuous care uses monotonic time; UTC jumps only replace the anchor. */
    if(elapsed<=120000u) {
        advance_needs(m,elapsed,quiet_now(m));
        power_step(m,elapsed,in->now_ms);
        movement_step(m,elapsed,in->now_ms);
    } else {
        /* Unobserved running gaps receive capped care advancement, never charge blame. */
        uint32_t capped=elapsed>PIP_CARE_OFFLINE_CAP_S*1000u?PIP_CARE_OFFLINE_CAP_S*1000u:elapsed;
        advance_needs(m,capped/2u,quiet_now(m));
        m->low_ms=m->recovered_ms=0;m->have_activity=0;
        memset(m->walk_buckets,0,sizeof m->walk_buckets);m->walk_slot=in->now_ms/30000u;
    }
    m->memory.clock_valid=input_clock_valid(in);
    m->memory.last_epoch_s=m->memory.clock_valid?in->epoch_s:0;
    if(in->visible && !m->last_visible)pip_care_greet(m,in->now_ms);
    m->last_visible=in->visible;output(m,in->now_ms);
}
void pip_care_snapshot(pip_care_model *m,pip_care_memory *out)
{
    *out=m->memory;
    out->generation=m->memory.generation+1u;
    out->checksum=memory_checksum(out);
}
void pip_care_saved(pip_care_model *m)
{
    m->memory.generation++;
    m->memory.checksum=memory_checksum(&m->memory);
    m->dirty=0;
}
void pip_care_greet(pip_care_model *m,uint32_t now)
{
    if(m->have_greeting && now-m->last_greeting_ms<60000u)return;
    m->have_greeting=1;m->last_greeting_ms=now;
    if(!m->have_greeting_reward || now-m->last_greeting_reward_ms>=PIP_CARE_REWARD_GAP_MS) {
        m->have_greeting_reward=1;m->last_greeting_reward_ms=now;
        m->memory.social=add_need(m->memory.social,30);
        m->memory.last_greet_day=m->input.local_day;
        m->memory.last_greet_epoch_s=input_clock_valid(&m->input)?m->input.epoch_s:0;
    }
    positive(m,m->memory.social<350?PIP_CARE_LONELY:PIP_CARE_HAPPY,PIP_CARE_REASON_REUNION,6000,now);
    enqueue(m,PIP_CARE_EVENT_GREETING,PIP_CARE_REASON_REUNION,1,now);
    history(m,PIP_CARE_EVENT_GREETING,PIP_CARE_REASON_REUNION);output(m,now);
}
static void care(pip_care_model *m,int play,uint32_t now)
{
    if(!m->have_reward || now-m->last_reward_ms>=PIP_CARE_REWARD_GAP_MS) {
        m->have_reward=1;m->last_reward_ms=now;
        m->memory.social=add_need(m->memory.social,play?70:100);
        m->memory.comfort=add_need(m->memory.comfort,play?30:120);
        m->memory.stimulation=add_need(m->memory.stimulation,play?120:20);
        if(m->memory.care_episodes<COUNTER_MAX)m->memory.care_episodes++;
        m->memory.last_care_epoch_s=input_clock_valid(&m->input)?m->input.epoch_s:0;
        history(m,play?PIP_CARE_EVENT_PLAY:PIP_CARE_EVENT_PETTED,PIP_CARE_REASON_TOUCH);
    }
    /* Care comforts immediately; an unresolved need remains visible afterward. */
    uint32_t glow=(m->memory.request_state==2 || m->memory.social<350)?6000u:(play?120000u:60000u);
    positive(m,play?PIP_CARE_EXCITED:PIP_CARE_HAPPY,PIP_CARE_REASON_TOUCH,glow,now);
    enqueue(m,play?PIP_CARE_EVENT_PLAY:PIP_CARE_EVENT_PETTED,PIP_CARE_REASON_TOUCH,1,now);output(m,now);
}
void pip_care_pet(pip_care_model *m,uint32_t now) {care(m,0,now);}
void pip_care_play(pip_care_model *m,uint32_t now) {care(m,1,now);}
void pip_care_notify(pip_care_model *m,uint32_t sequence,uint32_t now)
{
    if(!m->have_notification && !sequence) {m->have_notification=1;return;}
    if(m->have_notification && (int32_t)(sequence-m->notification_sequence)<=0)return;
    uint32_t count=m->have_notification?sequence-m->notification_sequence:1;
    m->notification_sequence=sequence;m->have_notification=1;
    if(quiet_now(m) || !m->input.visible)return;
    enqueue(m,PIP_CARE_EVENT_NOTIFICATION,PIP_CARE_REASON_NOTIFICATION,cap32(count,1000),now);
    m->last_notify_ms=now; /* notifications do not crowd persistent care history */
}
void pip_care_alarm(pip_care_model *m,uint32_t sequence,uint32_t now)
{
    if(!m->have_alarm && !sequence) {m->have_alarm=1;return;}
    if(m->have_alarm && (int32_t)(sequence-m->alarm_sequence)<=0)return;
    m->have_alarm=1;m->alarm_sequence=sequence;
    enqueue(m,PIP_CARE_EVENT_ALARM,PIP_CARE_REASON_ALARM,1,now);
    history(m,PIP_CARE_EVENT_ALARM,PIP_CARE_REASON_ALARM);
}
const char *pip_care_mood_name(pip_care_mood mood)
{
    static const char *const names[]={"content","grumpy","lonely","sleepy","happy","excited","relieved"};
    return mood>=0&&mood<PIP_CARE_MOOD_COUNT?names[mood]:"unknown";
}
const char *pip_care_reason_name(pip_care_reason reason)
{
    static const char *const names[]={"none","social","power","rest","stimulation","touch","walk","notification","alarm","reunion"};
    return reason>=0&&reason<PIP_CARE_REASON_COUNT?names[reason]:"unknown";
}
