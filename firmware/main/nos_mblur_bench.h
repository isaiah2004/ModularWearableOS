/* Explicit isolated kernel timing; temporary buffers only, no renderer controls. */
#ifndef NOS_MBLUR_BENCH_H
#define NOS_MBLUR_BENCH_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nos_power.h"
#include "mblur.h"
static inline uint16_t nos_mblur_bench_input(int pattern,int x,int y)
{
    if(pattern==0)return 0;
    if(pattern==1)return (x%11<2 && y%13<3)?0xffff:0;
    if(pattern==2)return 0xffff;
    return (uint16_t)(((x*7+y*3)&31)<<11 | ((x*5+y*11)&63)<<5 | ((x*13+y*7)&31));
}
static inline void nos_mblur_bench_run(void)
{
    const unsigned stripe_bytes=4u*GFX_W*sizeof(uint16_t);
    uint16_t *stripe=heap_caps_malloc(stripe_bytes,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    mb_v_columns_state *state=stripe?heap_caps_malloc(sizeof *state,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):0;
    if(!stripe || !state) {
        heap_caps_free(state);heap_caps_free(stripe);
        printf("NOSMBENCH v=1 state=0 cells=0 stripe_bytes=%u state_bytes=%u freed=1\n",stripe_bytes,(unsigned)sizeof *state);
        return;
    }
    int cpu_start=nos_power_get_stats().cpu_mhz;
    for(int pattern=0;pattern<4;pattern++)for(int k=1;k<=4;k++) {
        uint64_t pass_us[2]={0},total=0;uint32_t max_stripe=0,hash[2]={2166136261u,2166136261u};
        for(int pass=0;pass<2;pass++) {
            mb_v_columns_reset(state);
            for(int y=0;y<GFX_H;y+=4) {
                int rows=GFX_H-y<4?GFX_H-y:4;
                for(int r=0;r<rows;r++)for(int x=0;x<GFX_W;x++)
                    stripe[r*GFX_W+x]=nos_mblur_bench_input(pattern,x,y+r);
                gfx_fb fb={0};fb.px=stripe;fb.h=rows;fb.y_base=y;fb.cx1=GFX_W;
                int64_t start=esp_timer_get_time();
                mb_v_columns(&fb,0,GFX_W,0,GFX_H,k,state);
                uint32_t elapsed=(uint32_t)(esp_timer_get_time()-start);
                pass_us[pass]+=elapsed;if(elapsed>max_stripe)max_stripe=elapsed;
                for(int i=0;i<rows*GFX_W;i++)hash[pass]=(hash[pass]^stripe[i])*16777619u;
            }
            total+=pass_us[pass];
        }
        uint64_t pixels=2ull*GFX_W*GFX_H;
        printf("NOSMBENCH_CELL v=1 pattern=%d k=%d passes=2 pixels=%llu total_us=%llu pass0_us=%llu pass1_us=%llu max_stripe_us=%u ns_per_px_q8=%llu checksum0=%u checksum1=%u cpu_mhz=%d\n",
            pattern,k,(unsigned long long)pixels,(unsigned long long)total,
            (unsigned long long)pass_us[0],(unsigned long long)pass_us[1],(unsigned)max_stripe,
            (unsigned long long)((total*256000ull+pixels/2)/pixels),(unsigned)hash[0],(unsigned)hash[1],nos_power_get_stats().cpu_mhz);
    }
    int cpu_end=nos_power_get_stats().cpu_mhz;
    heap_caps_free(state);heap_caps_free(stripe);
    printf("NOSMBENCH v=1 state=1 cells=16 stripe_bytes=%u state_bytes=%u freed=1 cpu_start=%d cpu_end=%d\n",
        stripe_bytes,(unsigned)sizeof *state,cpu_start,cpu_end);
}
#endif
