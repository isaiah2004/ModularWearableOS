/* Includes the actual flush_task body extracted by test_nos_dma_chunks.py. */
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../firmware/main/nos_pipeline_probe.h"
#include "../firmware/main/nos_panel_ack.h"
#define GFX_W 466
#define DMA_ROWS 8
#define DISPLAY_WAIT_MS 1500
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) (x)
#define portMAX_DELAY 0xffffffffu
typedef int esp_err_t;
typedef int SemaphoreHandle_t;
enum { ESP_OK=0, ESP_FAIL=-1, ESP_ERR_TIMEOUT=0x107 };
enum { DISPLAY_FAULT_DMA=1, DISPLAY_FAULT_DRAW=6 };
typedef int nos_exposure_job;
typedef struct { uint16_t *buf; int x0,y0,w,rows,stride,kind; const nos_exposure_job *exposure; uint32_t command; } flush_job;
enum { JOB_STRIPE,JOB_BRIGHT,JOB_INVERT,JOB_SLEEP,JOB_FENCE,JOB_EXPOSURE_FENCE };
static uint16_t raw[4][466*16],rot[2][466*8+2];
static uint16_t *s_rotated[2]={rot[0]+1,rot[1]+1};
static int s_dma_ready=1,s_buf_free=2,s_frame_done=3,s_command_done=4,s_flush_q,s_panel,s_io;
static nos_panel_ack s_panel_ack[3];
static uint32_t s_worker_history_us,s_rot_us;
static nos_pipeline_live s_pipeline;
static flush_job jobs[10];
static int current,next_job,credits,pending[4],head,tail,busy[2],reads[4],released[4],exposures[4],draws;
static int pending_job[4],pending_offset[4],pending_rows[4];
static int job_count,idle_receives,command_signals,command_calls[10],command_step,old_applied;
static int fault_mode,expected_fault,caught_fault,caught_error,panel_failures,expected_command_error;
static jmp_buf finished;
static uint64_t clock_us,delayed_us;
static int te_waits,te_awake;
static void nos_display_te_wait(void) {
    /* Exactly the first prepared chunk after a frame fence, never per stripe. */
    assert(current==0 || current==3);assert(draws==(current==0?0:(jobs[0].rows+7)/8+1));
    te_waits++;
}
static void nos_display_te_set_awake(int awake) {te_awake=awake;}
static void __attribute__((noreturn)) display_fault(int phase,esp_err_t error)
{
    assert(expected_fault && phase==expected_fault && !caught_fault);
    caught_fault=phase;caught_error=error;longjmp(finished,2);
}
static uint16_t original(int job,int y,int x) {return (uint16_t)(job*30001+y*467+x+1);}
static void complete(void) {
    assert(head<tail);int q=head++%4,slot=pending[q],job=pending_job[q],off=pending_offset[q],n=pending_rows[q];
    assert(busy[slot]);flush_job *j=&jobs[job];
    for(int x=0;x<j->w;++x)for(int y=0;y<n;++y) {
        uint16_t v=original(job,off+y,j->x0+x);v=(uint16_t)((v<<8)|(v>>8));
        assert(s_rotated[slot][(j->w-1-x)*n+y]==v);
    }
    if(s_pipeline.active)nos_pipeline_done(&s_pipeline,++clock_us);
    busy[slot]=0;++credits;
}
static int xQueueReceive(int q,flush_job *out,unsigned wait) {
    assert(q==s_flush_q && wait==portMAX_DELAY); /* blocking idle wait: no ungridded 1 Hz poll wake; a non-pdTRUE return below still must just retry */
    if(!idle_receives++)return pdFALSE;
    if(next_job==job_count)longjmp(finished,1);
    current=next_job;flush_job *j=&jobs[next_job++];
    if(j->kind>=JOB_BRIGHT && j->kind<=JOB_SLEEP) {
        /* Publish one request only after the prior real ACK and frame fence. */
        assert(head==tail && credits==2 && !busy[0] && !busy[1]);
        for(unsigned c=0;c<3;c++)assert(s_panel_ack[c].request==s_panel_ack[c].ack);
        nos_panel_ack *ack=&s_panel_ack[j->kind-JOB_BRIGHT];old_applied=ack->applied;
        j->command=nos_panel_request(ack,j->x0);command_step=0;
    }
    *out=*j;return pdTRUE;
}
static int xSemaphoreTake(int sem,int wait) {
    assert(sem==s_dma_ready && wait==DISPLAY_WAIT_MS);
    if(!credits && fault_mode==1)return pdFALSE;
    if(!credits)complete();
    assert(credits>0);--credits;return pdTRUE;
}
static void xSemaphoreGive(int sem) {
    if(sem==s_dma_ready) {assert(credits<2 && credits+(tail-head)<2);++credits;}
    else if(sem==s_buf_free) {
        assert(current<4 && current!=2 && !released[current]);assert(reads[current]==jobs[current].rows);
        released[current]=1;memset(raw[current],0xcc,sizeof raw[current]); /* immediate producer reuse */
    } else if(sem==s_command_done) {
        flush_job *j=&jobs[current];assert(j->kind>=JOB_BRIGHT && j->kind<=JOB_SLEEP);
        nos_panel_ack *ack=&s_panel_ack[j->kind-JOB_BRIGHT];
        assert(ack->request==j->command && ack->ack==j->command);
        if(ack->error)assert(expected_command_error && ack->error==ESP_FAIL && ack->applied==old_applied && command_calls[current]==3);
        else assert(!expected_command_error && ack->applied==j->x0);
        command_signals++;
    } else {assert(sem==s_frame_done);assert(credits==2 && head==tail && !busy[0] && !busy[1]);}
}
static uint32_t hal_micros(void) {return 1;}
static int64_t esp_timer_get_time(void) {return (int64_t)++clock_us;}
static void nos_exposure_apply(const nos_exposure_job *p) {assert(p==jobs[current].exposure);assert(!exposures[current]++);}
static void board_pixels_ccw_swap(const uint16_t *p,int stride,uint16_t *out,int w,int rows,int swap) {
    assert(current<4 && current!=2 && !released[current] && rows>0 && rows<=8 && swap);
    int slot=out==s_rotated[0]?0:1;assert(out==s_rotated[slot] && !busy[slot]);
    int off=reads[current];assert(p==jobs[current].buf+off*stride);
    for(int x=0;x<w;++x)for(int y=0;y<rows;++y) {
        assert(p[y*stride+x]==original(current,off+y,jobs[current].x0+x));
        uint16_t v=p[y*stride+x];out[(w-1-x)*rows+y]=(uint16_t)((v<<8)|(v>>8));
    }
    reads[current]+=rows;assert(rot[slot][0]==0x1234 && rot[slot][466*8+1]==0x5678);
}
static int esp_lcd_panel_draw_bitmap(int panel,int x0,int y0,int x1,int y1,const void *data) {
    (void)panel;flush_job *j=&jobs[current];int n=x1-x0,off=reads[current]-n,slot=data==s_rotated[0]?0:1;
    assert(n>0 && n<=8 && y0==466-j->x0-j->w && y1==466-j->x0);
    assert(x0==j->y0+off && x1==j->y0+reads[current]);assert(!busy[slot]);
    assert(released[current]==(reads[current]==j->rows));busy[slot]=1;
    int q=tail++%4;pending[q]=slot;pending_job[q]=current;pending_offset[q]=off;pending_rows[q]=n;++draws;
    if(draws==1 && !fault_mode)complete(); /* callback can precede this API's return */
    return fault_mode==2?ESP_FAIL:ESP_OK;
}
static esp_err_t esp_lcd_panel_invert_color(int panel,int invert) {
    assert(panel==s_panel && jobs[current].kind==JOB_INVERT && invert==(jobs[current].x0!=0));
    command_calls[current]++;return ESP_OK;
}
static esp_err_t esp_lcd_panel_io_tx_param(int io,uint64_t command,const void *data,int bytes) {
    flush_job *j=&jobs[current];assert(io==s_io);
    if(j->kind==JOB_BRIGHT) {
        assert(command==((0x02ULL<<24)|(0x51<<8)) && bytes==1 && data && *(const uint8_t *)data==(uint8_t)j->x0);
        command_calls[current]++;
        if(panel_failures){panel_failures--;return ESP_FAIL;}
    } else {
        assert(j->kind==JOB_SLEEP && !data && !bytes);
        const int sleep_commands[2][2]={{0x11,0x29},{0x28,0x10}};
        assert(command_step<2 && command==((0x02ULL<<24)|((uint64_t)sleep_commands[j->x0!=0][command_step]<<8)));
        command_step++;command_calls[current]++;
    }
    return ESP_OK;
}
static void vTaskDelay(int n) {
    assert((n==100 && jobs[current].kind==JOB_SLEEP && !jobs[current].x0 && command_step==1) ||
           (n==10 && jobs[current].kind==JOB_BRIGHT && command_calls[current]>0));
    clock_us+=(uint64_t)n*1000;delayed_us+=(uint64_t)n*1000;
}
#include "actual_flush_task.h"
static void setup(int height,int width,int probe) {
    current=next_job=head=tail=draws=0;credits=2;job_count=5;idle_receives=0;
    te_waits=0;te_awake=1;
    command_signals=command_step=old_applied=0;
    fault_mode=expected_fault=caught_fault=caught_error=panel_failures=expected_command_error=0;
    memset(busy,0,sizeof busy);memset(reads,0,sizeof reads);memset(released,0,sizeof released);memset(exposures,0,sizeof exposures);
    memset(command_calls,0,sizeof command_calls);memset(s_panel_ack,0,sizeof s_panel_ack);
    s_panel_ack[0].applied=s_panel_ack[0].requested=17;
    for(int i=0;i<4;++i) {
        if(i==2)continue;
        int left=width==466?0:113;
        jobs[i]=(flush_job){.buf=raw[i]+left,.x0=left,.y0=i?464:17,.w=width,.rows=i?2:height,
                           .stride=466,.kind=JOB_STRIPE,.exposure=(const int *)(uintptr_t)(i+1)};
        for(int y=0;y<16;++y)for(int x=0;x<466;++x)raw[i][y*466+x]=original(i,y,x);
        rot[i&1][0]=0x1234;rot[i&1][466*8+1]=0x5678;
    }
    jobs[2]=(flush_job){.kind=JOB_FENCE};jobs[4]=(flush_job){.kind=JOB_FENCE};
    clock_us=1000;delayed_us=0;memset(&s_pipeline,0,sizeof s_pipeline);
    if(probe)nos_pipeline_begin(&s_pipeline,clock_us);
}
static void run_case(int reason_expected) {
    switch(setjmp(finished)) {
    case 0:flush_task(NULL);assert(0);break;
    case 1:assert(reason_expected==1);break;
    case 2:assert(reason_expected==2);break;
    default:assert(0);
    }
}
int main(void) {
    const int heights[]={1,2,7,8,9,15,16},widths[]={1,37,466};
    volatile int cases=0;
    for(volatile int probe=0;probe<2;++probe)
    for(volatile unsigned h=0;h<sizeof heights/sizeof *heights;++h)for(volatile unsigned w=0;w<sizeof widths/sizeof *widths;++w) {
        setup(heights[h],widths[w],probe);run_case(1);
        assert(credits==2 && head==tail && released[0]==1 && released[1]==1 && exposures[0]==1 && exposures[1]==1 && released[3]==1 && exposures[3]==1);
        assert(draws==(heights[h]+7)/8+2);++cases;
        assert(te_waits==2); /* multiple raw stripes and DMA chunks, two frames */
        if(probe) {
            assert(s_pipeline.frame.done==s_pipeline.frame.chunks && s_pipeline.frame.chunks==(uint32_t)draws);
            assert(s_pipeline.frame.rawjobs==3);
            assert(s_pipeline.frame.bytes==(uint32_t)(widths[w]*(heights[h]+4)*2));
            assert(s_pipeline.frame.first_done<s_pipeline.frame.first_return);
        } else assert(clock_us==1000+(uint64_t)draws*2); /* only legacy rotation clocks */
    }
    /* Real request/ACK state, including wake's two commands and required delay. */
    setup(16,37,0);job_count=9;
    jobs[5]=(flush_job){.kind=JOB_BRIGHT,.x0=128};jobs[6]=(flush_job){.kind=JOB_INVERT,.x0=1};
    jobs[7]=(flush_job){.kind=JOB_SLEEP,.x0=1};jobs[8]=(flush_job){.kind=JOB_SLEEP,.x0=0};
    run_case(1);
    assert(command_signals==4 && delayed_us==100000 && credits==2 && head==tail);
    assert(te_waits==2 && te_awake==1);
    assert(s_panel_ack[0].applied==128 && s_panel_ack[1].applied==1 && s_panel_ack[2].applied==0 && s_panel_ack[2].ack==2);
    assert(command_calls[5]==1 && command_calls[6]==1 && command_calls[7]==2 && command_calls[8]==2);
    /* A transient error needs a real third success before applied can advance. */
    setup(16,37,0);job_count=1;panel_failures=2;jobs[0]=(flush_job){.kind=JOB_BRIGHT,.x0=128};
    run_case(1);
    assert(command_signals==1 && command_calls[0]==3 && delayed_us==20000);
    assert(s_panel_ack[0].applied==128 && !s_panel_ack[0].error);
    /* On three failed attempts the worker reports error, never applied=128.
     * The waiting UI owns subsequent fault/restart handling. */
    setup(16,37,0);job_count=1;panel_failures=3;expected_command_error=1;jobs[0]=(flush_job){.kind=JOB_BRIGHT,.x0=128};
    run_case(1);
    assert(command_signals==1 && command_calls[0]==3 && delayed_us==30000);
    assert(s_panel_ack[0].requested==128 && s_panel_ack[0].applied==17 && s_panel_ack[0].error==ESP_FAIL);
    /* Both rotated slots remain owned on a credit timeout. No third rotation,
     * raw release, fence ACK or recycled credit is allowed after the fault. */
    setup(16,37,0);fault_mode=1;expected_fault=DISPLAY_FAULT_DMA;run_case(2);
    assert(caught_fault==DISPLAY_FAULT_DMA && caught_error==ESP_ERR_TIMEOUT);
    assert(draws==2 && credits==0 && head==0 && tail==2 && busy[0] && busy[1]);
    assert(reads[0]==16 && released[0] && !reads[1] && !released[1] && !released[3]);
    complete();complete(); /* only actual late callbacks can return these credits */
    /* A failed SDK draw may still own the submitted descriptor. Never advance
     * to the remaining raw chunk or reclaim the uncertain rotated buffer. */
    setup(9,37,0);fault_mode=2;expected_fault=DISPLAY_FAULT_DRAW;run_case(2);
    assert(caught_fault==DISPLAY_FAULT_DRAW && caught_error==ESP_FAIL);
    assert(draws==1 && credits==1 && head==0 && tail==1 && busy[0] && !busy[1]);
    assert(reads[0]==8 && !released[0] && !reads[1] && !released[1]);
    complete();
    printf("PASS actual flush_task: %d partial/edge cases, two queued raw jobs, exposure once, immediate raw overwrite, delayed DMA completion; checked command ACK/retry and timeout/draw-fault ownership\n",cases);
}
