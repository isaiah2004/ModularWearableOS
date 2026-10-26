#include "nos_frame_pacer.h"
#include "nos_frame_perf.h"
#include "../nos/frame_cap.h"
#include <assert.h>
#include <stdio.h>

static nos_frame_perf sequence(int enabled, const int *costs, unsigned n,
                               nos_frame_pacer *p)
{
    nos_frame_perf perf = {0};
    nos_frame_pacer_reset(p);
    int64_t start = 1000;
    for (unsigned i = 0; i < n; ++i) {
        int64_t done = start + costs[i];
        nos_frame_perf_add(&perf, start, done, start + 25000, 1);
        int64_t next = nos_frame_pacer_next(p, enabled, 1, start, done,
                                           start + 25000, 25000);
        start = next > done ? next : done;
    }
    return perf;
}

int main(void)
{
    nos_frame_pacer p;
    int work[80];
    for (unsigned i = 0; i < 80; ++i) work[i] = 20000;
    nos_frame_perf stable = sequence(1, work, 80, &p);
    assert(stable.frames == 80 && stable.painted == 80);
    assert(stable.present_max == 25000 && !stable.floor_misses);
    /* Once the conservative entry reserve expires, steady rendering sleeps. */
    assert(p.advanced_frames == 7 && p.advanced_us == 35000);
    for (unsigned i = 0; i < 80; ++i) work[i] = i & 1 ? 28000 : 12000;
    nos_frame_perf grid = sequence(0, work, 80, &p);
    nos_frame_perf adaptive = sequence(1, work, 80, &p);
    assert(grid.present_max == 41000 && grid.floor_misses == 40);
    assert(adaptive.present_max == 28000 && !adaptive.floor_misses);
    assert(adaptive.frames == grid.frames && adaptive.painted == grid.painted);
    /* The measured cheap-to-expensive failure remains honest: no fence delay,
     * no stale frame discard, and truly expensive frames still fail the floor. */
    const int changing[] = {38460, 4948, 33027, 40000};
    adaptive = sequence(1, changing, 4, &p);
    assert(adaptive.present_max == 40000 && adaptive.floor_misses == 1);
    assert(adaptive.deadline_misses == 3);
    int64_t idle = nos_frame_pacer_next(&p, 1, 0, 100000, 101000, 125000, 25000);
    assert(idle == 125000 && !p.active);
    assert(nos_frame_pacer_next(&p, 1, 1, 125000, 130000, 150000, 25000) == 130000);
    assert(nos_frame_pacer_next(&p, 0, 1, 130000, 135000, 155000, 25000) == 155000);
    assert(nos_frame_pacer_next(&p, 1, 1, 200, 100, 1000, 25000) == 1000);
    /* A hard cap survives adaptive early starts, alternating costs, idle and
     * overrun/rebase. Unlimited adds no scheduler wait. */
    const unsigned caps[]={24,30,40,48,60};
    for(unsigned k=0;k<sizeof caps/sizeof *caps;k++) {
        uint32_t period=nos_frame_cap_period(caps[k]);
        assert((uint64_t)period*caps[k]>=1000000);
        for(int adaptive=0;adaptive<2;adaptive++) {
            int64_t begin=1000,grid=begin;
            nos_frame_pacer_reset(&p);
            for(unsigned i=0;i<80;i++) {
                int64_t done=begin+(i%3==0?period+7000:i%3==1?3000:period-2000);
                int64_t next=nos_frame_pacer_capped(&p,adaptive,i%7!=0,begin,done,grid+period,period,period);
                assert(next>=begin+period);
                grid=next>done?next:done;begin=grid;
            }
            assert(!p.advanced_frames);
        }
    }
    assert(!nos_frame_cap_period(0) && nos_frame_cap_period(25)==16667);
    assert(nos_frame_pacer_capped(&p,1,1,1000,4000,26000,25000,0)==4000 && !p.active);
    puts("PASS completion pacing: stable sleep, changing costs, real overruns, idle and controls");
    return 0;
}
