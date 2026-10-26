/* Actual display-owner functions are inserted by the private host runner. */
#include "nos.h"
#include "render_exposure.h"
#include "nos_pipeline_probe.h"
#include "nos_panel_ack.h"
#include "diag_log.h"
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int esp_err_t;
enum {ESP_OK=0,ESP_FAIL=-1,ESP_ERR_TIMEOUT=-2,ESP_ERR_INVALID_STATE=-3,ESP_ERR_INVALID_RESPONSE=-4};
enum {pdTRUE=1,pdFALSE=0};
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xffffffffu
#define DMA_ROWS 8
#define DISPLAY_WAIT_MS 1500u
enum {DISPLAY_FAULT_DMA=1,DISPLAY_FAULT_STRIPE,DISPLAY_FAULT_QUEUE,DISPLAY_FAULT_FENCE,DISPLAY_FAULT_COMMAND,DISPLAY_FAULT_DRAW};
typedef struct {int value,id;} semaphore;
typedef semaphore *SemaphoreHandle_t;
static semaphore dma,raw,done,command_done,command_lock;
static SemaphoreHandle_t s_dma_ready=&dma,s_buf_free=&raw,s_frame_done=&done,s_command_done=&command_done,s_command_lock=&command_lock;
static int queue_token;static void *s_flush_q=&queue_token,*s_panel,*s_io;
static nos_panel_ack s_panel_ack[3];static volatile int s_display_fault;
static nos_pipeline_live s_pipeline;
static nos_exposure_job s_exposure_job[2];static int s_exposure_ready[2];
static uint16_t stripe_a[GFX_W*32],stripe_b[GFX_W*32],rot_a[GFX_W*16],rot_b[GFX_W*16];
static uint16_t *s_stripe[2]={stripe_a,stripe_b},*s_rotated[2]={rot_a,rot_b};
static volatile uint32_t s_rot_us,s_wait_us,s_worker_history_us;
typedef struct {uint16_t *buf;int x0,y0,w,rows,stride,kind;const nos_exposure_job *exposure;uint32_t command;} flush_job;
enum {JOB_STRIPE,JOB_BRIGHT,JOB_INVERT,JOB_SLEEP,JOB_FENCE,JOB_EXPOSURE_FENCE};
static flush_job queue[16];static unsigned qread,qwrite;
static unsigned gives[5],rotations,draws,execute_calls,events,restarts;
static int auto_worker,auto_dma,queue_full,draw_error,command_error,fail_dcs;
static int inflight[2],fault_phase,fault_error;
static jmp_buf fault_jump,worker_jump;
static void nos_display_te_wait(void) {}
static void nos_display_te_set_awake(int awake) {(void)awake;}
static void flush_task(void *arg);
static void worker(void) {if(!setjmp(worker_jump))flush_task(NULL);}
static unsigned uxSemaphoreGetCount(SemaphoreHandle_t s) {return (unsigned)s->value;}
static int xSemaphoreTake(SemaphoreHandle_t s,unsigned ms)
{assert(ms<=DISPLAY_WAIT_MS);if(s->value){s->value--;return pdTRUE;}return pdFALSE;}
static int xSemaphoreGive(SemaphoreHandle_t s) {s->value++;gives[s->id]++;return pdTRUE;}
static int xQueueSend(void *q,const void *value,unsigned ms)
{assert(q==s_flush_q && ms<=DISPLAY_WAIT_MS);if(queue_full)return pdFALSE;assert(qwrite<16);queue[qwrite++]=*(const flush_job *)value;if(auto_worker)worker();return pdTRUE;}
static int xQueueReceive(void *q,void *out,unsigned ms)
{assert(q==s_flush_q && ms==portMAX_DELAY);if(qread==qwrite)longjmp(worker_jump,1);*(flush_job *)out=queue[qread++];return pdTRUE;}
static int64_t esp_timer_get_time(void) {static int64_t time;return ++time;}
uint32_t hal_micros(void) {return (uint32_t)esp_timer_get_time();}
static void vTaskDelay(unsigned ms) {assert(ms<=150);}
void diag_log_event(uint16_t code,int32_t a,int32_t b,int32_t c)
{assert(code==DIAG_DISPLAY_FAULT);if(!events){fault_phase=a;fault_error=b;}events++;}
static void esp_restart(void) {restarts++;longjmp(fault_jump,1);}
static esp_err_t esp_lcd_panel_invert_color(void *panel,int want) {execute_calls++;return command_error;}
static esp_err_t esp_lcd_panel_io_tx_param(void *io,uint64_t command,const void *data,unsigned bytes)
{execute_calls++;return command_error || (int)((command>>8)&255)==fail_dcs?ESP_FAIL:ESP_OK;}
static esp_err_t esp_lcd_panel_draw_bitmap(void *panel,int x0,int y0,int x1,int y1,const void *pixels)
{int slot=pixels==rot_a?0:1;assert(!inflight[slot]);draws++;if(draw_error)return ESP_FAIL;inflight[slot]=1;if(auto_dma){inflight[slot]=0;xSemaphoreGive(s_dma_ready);}return ESP_OK;}
void board_pixels_ccw_swap(const uint16_t *input,int stride,uint16_t *out,int w,int rows,int swap)
{assert(out==rot_a || out==rot_b);assert(!inflight[out==rot_a?0:1]);rotations++;out[0]=input[0];}
void nos_exposure_apply(const nos_exposure_job *job) {(void)job;}
/* ACTUAL FUNCTIONS */
static void reset(void)
{
    memset(s_panel_ack,0,sizeof s_panel_ack);memset(gives,0,sizeof gives);memset(inflight,0,sizeof inflight);
    dma=(semaphore){2,0};raw=(semaphore){0,1};done=(semaphore){0,2};command_done=(semaphore){0,3};command_lock=(semaphore){1,4};
    qread=qwrite=rotations=draws=execute_calls=events=restarts=0;
    s_display_fault=queue_full=draw_error=command_error=auto_worker=auto_dma=0;fail_dcs=-1;
}
int main(void)
{
    for(int kind=JOB_BRIGHT;kind<=JOB_SLEEP;kind++) {
        reset();auto_worker=1;panel_command(kind,kind==JOB_BRIGHT?137:1);
        assert(s_panel_ack[kind-JOB_BRIGHT].applied==(kind==JOB_BRIGHT?137:1));
        assert(s_panel_ack[kind-JOB_BRIGHT].ack==s_panel_ack[kind-JOB_BRIGHT].request && !events);
    }
    reset();auto_worker=1;command_error=ESP_FAIL;
    if(!setjmp(fault_jump)){panel_command(JOB_INVERT,1);assert(0);}
    assert(execute_calls==3 && s_panel_ack[1].requested==1 && !s_panel_ack[1].applied && events==4 && restarts==1);
    reset();auto_worker=1;fail_dcs=0x10;
    if(!setjmp(fault_jump)){panel_command(JOB_SLEEP,1);assert(0);}
    assert(execute_calls==6 && !s_panel_ack[2].applied && fault_phase==DISPLAY_FAULT_COMMAND);
    reset();auto_worker=1;s_panel_ack[2].applied=1;panel_command(JOB_SLEEP,0);
    assert(execute_calls==2 && !s_panel_ack[2].applied);
    reset(); /* a worker stuck inside an SDK call cannot deliver an ACK */
    if(!setjmp(fault_jump)){panel_command(JOB_BRIGHT,200);assert(0);}
    assert(qwrite==1 && !s_panel_ack[0].ack && !s_panel_ack[0].applied && fault_error==ESP_ERR_TIMEOUT);

    reset();auto_dma=1;queue[qwrite++]=(flush_job){.buf=stripe_a,.w=4,.rows=16,.stride=GFX_W};worker();
    assert(rotations==2 && draws==2 && dma.value==2 && raw.value==1 && !events);
    reset();dma.value=0;queue[qwrite++]=(flush_job){.buf=stripe_a,.w=4,.rows=16,.stride=GFX_W};
    if(!setjmp(fault_jump)){worker();assert(0);}
    assert(!rotations && !draws && !gives[1] && !gives[0] && fault_phase==DISPLAY_FAULT_DMA);
    reset();dma.value=1;queue[qwrite++]=(flush_job){.kind=JOB_FENCE};
    if(!setjmp(fault_jump)){worker();assert(0);}
    assert(!dma.value && !gives[0] && !gives[2]); /* partial fence credit is not recycled */
    reset();draw_error=1;queue[qwrite++]=(flush_job){.buf=stripe_a,.w=4,.rows=16,.stride=GFX_W};
    if(!setjmp(fault_jump)){worker();assert(0);}
    assert(draws==1 && rotations==1 && raw.value==0 && dma.value==1 && fault_phase==DISPLAY_FAULT_DRAW);
    reset();queue_full=1;
    if(!setjmp(fault_jump)){stripe_flush(stripe_a,GFX_W,0,0,4,16);assert(0);}
    assert(!gives[1] && fault_phase==DISPLAY_FAULT_QUEUE);
    reset();
    if(!setjmp(fault_jump)){stripe_wait();assert(0);}
    assert(!gives[1] && fault_phase==DISPLAY_FAULT_STRIPE);
    reset();
    if(!setjmp(fault_jump)){frame_drain();assert(0);}
    assert(qwrite==1 && queue[0].kind==JOB_FENCE && !gives[0] && fault_phase==DISPLAY_FAULT_FENCE);
    puts("PASS actual display: DMA/raw/queue/fence/SDK-ACK deadlines retain ownership; command failures/retries never advance applied state");
    return 0;
}
