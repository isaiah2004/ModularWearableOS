/* nos_main.c — NOS firmware host for the Waveshare ESP32-S3-Touch-AMOLED-1.43-B.
 *
 * Core 0 runs the UI: input -> nos_frame_run (record, damage, raster into NOS_STRIPE_ROWS
 * internal-RAM stripes). Core 1 runs the flush task: rotate each stripe 90 degrees
 * CCW into one of two DMA buffers (the panel is mounted rotated and the vendor
 * driver cannot swap axes) and send it over LCD_SPI_HZ QSPI while core 0 renders the
 * next stripe. Frames follow NOS_PRESENT_FPS timer deadlines; overruns remain visible in diagnostics.
 *
 * Touch: a small task samples the FT3168 at its report rate (100 Hz polls, every new
 * position timestamped) into a ring; the UI loop hands every sample to the engine right
 * before nos_frame_run, so the velocity fit sees ~6-10 samples per 100 ms and the drag
 * position is the newest one (docs/MOTION30.html R10).
 *
 * Power: ON (NOS_PRESENT_FPS Hz) -> after nos_prefs.sleep_s of no input -> AOD (1 Hz, dim) if
 * enabled, else OFF (no rendering, panel brightness 0). Touch, the button or a
 * wrist raise wakes; the waking touch is swallowed so it never also taps. On wake
 * the first frame renders before the brightness comes back (no stale-frame flash). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "esp_pm.h"
#include "sports.h"
#include "workout.h"
#include "svc_track.h"

#include "pins.h"
#include "board.h"
#include "board_protocol.h"
#include "nos_frame_perf.h"
#include "nos_frame_pacer.h"
#include "frame_cap.h"
#include "nos_frame_priority.h"
#include "nos_blur_capture.h"
#include "nos_cache_capture.h"
#include "apps/scroll_diag.h"
#include "nos_power.h"
#include "sleep_grid.h"
#include "nos_frame_wait.h"
#include "power_policy.h"
#include "diag_log.h"
#include "command_input.h"
#include "host_request.h"
#include <stdarg.h>
#include "imu_backend.h"
#include "nos_mblur_bench.h"
#include "nos_heap_diag.h"
#include "nos_panel_ack.h"
#include "nos_display_te.h"
#include "nos_touch_contact.h"
#include "nos_build_identity.h"
#include "nos_usb_diag.h"
#include "nos_pipeline_probe.h"
#include "nos_list_profile.h"
#include "settings_shutter.h"
#include "native_menu_assets.h"
#include "lcd_probe.h"
#include "ota.h"
#include "../../core/settings.h"
#include "../../core/wake_service.h"
#include "nos.h"
#include "nos_button_pulse.h"
#include "pblur.h"
#include "render_layers.h"
#include "render_history.h"
#include "render_exposure.h"
#include "cost_profile.h"
#include "prefs.h"
#include "ui.h"
extern int blur_cache_packed;
#include "hal_ext.h"
#include "svc.h"
#include "apps/nos_apps.h"
#include "background.h"
#include "voice_chat.h"
#include "apps/sys_launcher_page.h"
#include "faces/face.h"
#include "faces/face_ext.h"
#include "faces/orrery_shade_q16.h"
#include "dot.h"
#include "prog.h"

#define TAG "nos"
#define STRIPE_ROWS NOS_STRIPE_ROWS
#define DMA_ROWS NOS_DMA_ROWS
_Static_assert(DMA_ROWS > 0 && DMA_ROWS <= STRIPE_ROWS, "DMA chunk must fit raw stripe");
#if CONFIG_WATCH_BOARD_143C
#define PANEL_GAP_X 8
#else
#define PANEL_GAP_X (LCD_USE_SH8601 ? 0 : 6)
#endif

void hal_esp32_init_periph(void);
void hal_esp32_start_radios(void);
void hal_esp32_radio_report(void);
int hal_esp32_radios_pending(void);

/* ------------------------------------------------------------------ display */
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_stripe[2];
static uint16_t *s_rotated[2];
static SemaphoreHandle_t s_dma_ready;     /* free rotated buffers; the ISR returns one per DMA */
static SemaphoreHandle_t s_buf_free;      /* free stripe buffers */
static QueueHandle_t s_flush_q;
static SemaphoreHandle_t s_frame_tick;
static SemaphoreHandle_t s_frame_done; /* ordered queue fence, after last DMA submission */
static SemaphoreHandle_t s_command_done,s_command_lock;
static nos_panel_ack s_panel_ack[3]; /* brightness, inversion, sleep; owner lock spans ACK */
static volatile int s_display_fault;
#define DISPLAY_WAIT_MS 1500u
enum { DISPLAY_FAULT_DMA=1, DISPLAY_FAULT_STRIPE, DISPLAY_FAULT_QUEUE,
       DISPLAY_FAULT_FENCE, DISPLAY_FAULT_COMMAND, DISPLAY_FAULT_DRAW };
static void __attribute__((noreturn)) display_fault(int phase,int error)
{
    /* Do not return a DMA/raw credit or free memory on any uncertain completion.
     * RTC first-fault evidence precedes the controlled restart; no SD/printf wait. */
    if(!__atomic_exchange_n(&s_display_fault,1,__ATOMIC_ACQ_REL)) {
        int credits=(s_dma_ready?(int)uxSemaphoreGetCount(s_dma_ready):0) |
                    ((s_buf_free?(int)uxSemaphoreGetCount(s_buf_free):0)<<8);
        diag_log_event(DIAG_DISPLAY_FAULT,phase,error,credits);
        if(phase==DISPLAY_FAULT_COMMAND)for(int channel=0;channel<3;channel++)
            diag_log_event(DIAG_DISPLAY_FAULT,0x100+channel,s_panel_ack[channel].requested,s_panel_ack[channel].applied);
    }
    esp_restart();abort();
}
static void display_take(SemaphoreHandle_t semaphore,int phase)
{
    if(xSemaphoreTake(semaphore,pdMS_TO_TICKS(DISPLAY_WAIT_MS))!=pdTRUE)
        display_fault(phase,ESP_ERR_TIMEOUT);
}
static esp_timer_handle_t s_frame_timer;
static nos_frame_priority s_frame_priority;
static void frame_priority_set(unsigned priority){vTaskPrioritySet(NULL,(UBaseType_t)priority);}
static void frame_priority_report(void){
    printf("NOSSCHED v=1 mode=%u base=%u current=%u core=%d paint=%u mismatches=%u paint_samples=%u\n",s_frame_priority.mode,
        s_frame_priority.base,(unsigned)uxTaskPriorityGet(NULL),xPortGetCoreID(),
        s_frame_priority.paint,s_frame_priority.mismatches,s_frame_priority.paint_samples);
}

static TaskHandle_t s_flush_handle;
/* Metadata has the same ownership lifetime as its raw stripe. It is immutable
 * after queue publication and may be reused only after s_buf_free is returned. */
static nos_exposure_job s_exposure_job[2];
/* The exact worker path remains available for diagnosis. Constant-damage
 * hardware A/B did not establish a win, so retain synchronous exposure by default. */
static int s_exposure_ready[2], s_exposure_worker;
static volatile uint32_t s_worker_history_us;
static nos_frame_capture s_capture;
static nos_frame_pacer s_pacer;
static int s_frame_pacing = 1;
static nos_blur_capture s_blur_capture;
static nos_cache_capture s_cache_capture;
static uint32_t s_band_capture_painted[2]; /* band path / guarded legacy fallback */
static int s_pipeline_enabled; /* opt-in only; unchanged during a live capture */
static DRAM_ATTR nos_pipeline_live s_pipeline;
static DRAM_ATTR nos_pipeline_capture s_pipeline_capture;
static nos_list_profile *s_list_profile; /* allocated in PSRAM only while enabled */
_Static_assert(sizeof s_pipeline + sizeof s_pipeline_capture <= 2048,
               "pipeline probe static storage budget");

typedef struct {
    uint16_t *buf;
    int x0, y0, w, rows, stride, kind;
    const nos_exposure_job *exposure;
    uint32_t command;
} flush_job;
enum { JOB_STRIPE = 0, JOB_BRIGHT = 1, JOB_INVERT = 2, JOB_SLEEP = 3, JOB_FENCE = 4,
       JOB_EXPOSURE_FENCE = 5 };
static void display_queue(const flush_job *job)
{
    if(xQueueSend(s_flush_q,job,pdMS_TO_TICKS(DISPLAY_WAIT_MS))!=pdTRUE)
        display_fault(DISPLAY_FAULT_QUEUE,ESP_ERR_TIMEOUT);
}
static esp_err_t panel_execute(int kind,int value)
{
    if(kind==JOB_INVERT)return esp_lcd_panel_invert_color(s_panel,value!=0);
    if(kind==JOB_BRIGHT) {
        uint8_t level=(uint8_t)value;
        return esp_lcd_panel_io_tx_param(s_io,(0x02ULL<<24)|(0x51<<8),&level,1);
    }
    esp_err_t error;
    if(value) {
        error=esp_lcd_panel_io_tx_param(s_io,(0x02ULL<<24)|(0x28<<8),NULL,0);
        if(error==ESP_OK)error=esp_lcd_panel_io_tx_param(s_io,(0x02ULL<<24)|(0x10<<8),NULL,0);
    } else {
        error=esp_lcd_panel_io_tx_param(s_io,(0x02ULL<<24)|(0x11<<8),NULL,0);
        if(error==ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            error=esp_lcd_panel_io_tx_param(s_io,(0x02ULL<<24)|(0x29<<8),NULL,0);
        }
    }
    if(error==ESP_OK)nos_display_te_set_awake(!value);
    return error;
}
static esp_err_t panel_execute_retry(int kind,int value)
{
    esp_err_t error=ESP_FAIL;
    for(unsigned attempt=0;attempt<3;attempt++) {
        error=panel_execute(kind,value);
        if(error==ESP_OK)break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return error;
}
static void panel_command(int kind,int value)
{
    unsigned channel=(unsigned)(kind-JOB_BRIGHT);
    assert(channel<3);
    if(!s_flush_q) { /* initialization only, before concurrent display ownership */
        uint32_t request=nos_panel_request(&s_panel_ack[channel],value);
        esp_err_t error=s_io?panel_execute_retry(kind,value):ESP_ERR_INVALID_STATE;
        nos_panel_complete(&s_panel_ack[channel],request,error);
        if(error!=ESP_OK)display_fault(DISPLAY_FAULT_COMMAND,error);
        return;
    }
    display_take(s_command_lock,DISPLAY_FAULT_COMMAND);
    while(xSemaphoreTake(s_command_done,0)==pdTRUE) {}
    uint32_t request=nos_panel_request(&s_panel_ack[channel],value);
    flush_job job={.kind=kind,.x0=value,.command=request};display_queue(&job);
    /* The UI-side deadline also detects an SDK SPI call stuck inside the worker.
     * It never attempts to cancel that call or recycle its buffers. */
    display_take(s_command_done,DISPLAY_FAULT_COMMAND);
    if(s_panel_ack[channel].ack!=request || s_panel_ack[channel].error!=ESP_OK)
        display_fault(DISPLAY_FAULT_COMMAND,s_panel_ack[channel].error?s_panel_ack[channel].error:ESP_ERR_INVALID_RESPONSE);
    xSemaphoreGive(s_command_lock);
}

static const sh8601_lcd_init_cmd_t sh8601_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
};
static const sh8601_lcd_init_cmd_t co5300_init[] = {
#if CONFIG_WATCH_BOARD_143C
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x44, (uint8_t[]){0x00, 0x00}, 2, 0}, /* TE high during vertical blank */
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0x00}, 1, 0},
    {0x36, (uint8_t[]){0xC0}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 0},
#else
    {0x11, (uint8_t[]){0x00}, 0, 80},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 1},
    {0x63, (uint8_t[]){0xFF}, 1, 1},
    {0x51, (uint8_t[]){0x00}, 1, 1},
    {0x29, (uint8_t[]){0x00}, 0, 10},
#endif
};

static bool IRAM_ATTR on_dma_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    if (s_pipeline.active) nos_pipeline_done(&s_pipeline, esp_timer_get_time());
    xSemaphoreGiveFromISR(s_dma_ready, &woken);
    return woken == pdTRUE;
}

static volatile uint32_t s_rot_us, s_wait_us;   /* per-frame trace: rotate (core 1), stripe waits (core 0) */
#include "nos_rotation_bench.h"
static void flush_task(void *arg)
{
    flush_job j;
    int slot = 0;
    int first_stripe = 1;
    for (;;) {
        int64_t queue_begin = s_pipeline.active ? esp_timer_get_time() : 0;
        if(xQueueReceive(s_flush_q,&j,portMAX_DELAY)!=pdTRUE)continue; /* no idle poll: it was an ungridded 1 Hz wake */
        if (j.kind == JOB_EXPOSURE_FENCE) {
            /* Prior jobs have finished exposure and submitted their DMA. The
             * normal frame_drain still measures final DMA completion later. */
            xSemaphoreGive(s_frame_done);
            continue;
        }
        if (j.kind == JOB_FENCE) {
            /* All preceding draw calls have submitted their DMA transactions.
             * Free raster buffers alone cannot prove that: the producer releases
             * them before submitting the corresponding draw. */
            display_take(s_dma_ready,DISPLAY_FAULT_DMA);display_take(s_dma_ready,DISPLAY_FAULT_DMA);
            xSemaphoreGive(s_dma_ready); xSemaphoreGive(s_dma_ready);
            first_stripe = 1;
            xSemaphoreGive(s_frame_done);
            continue;
        }
        if(j.kind==JOB_BRIGHT || j.kind==JOB_INVERT || j.kind==JOB_SLEEP) {
            esp_err_t error=panel_execute_retry(j.kind,j.x0);
            nos_panel_complete(&s_panel_ack[j.kind-JOB_BRIGHT],j.command,error);
            xSemaphoreGive(s_command_done);
            continue;
        }
        int probing = s_pipeline.active;
        if (probing) nos_pipeline_raw(&s_pipeline,
            queue_begin && s_pipeline.frame.rawjobs ? (uint32_t)(esp_timer_get_time() - queue_begin) : 0);
        /* Snapshot coverage belongs to this job; the producer may already be
         * drawing another stripe and changing global UI coverage. History has
         * exactly one writer. Finish it before rotation/DMA observes pixels. */
        if (j.exposure) {
            uint32_t start = hal_micros();
            nos_exposure_apply(j.exposure);
            s_worker_history_us += hal_micros() - start;
        }
        /* Keep raster/exposure at the canonical height. Only rotated output is
         * split: raw ownership spans every chunk, rotated ownership each DMA. */
        for (int offset = 0; offset < j.rows; offset += DMA_ROWS) {
            int rows = j.rows - offset;
            if (rows > DMA_ROWS) rows = DMA_ROWS;
            int64_t credit_begin = probing ? esp_timer_get_time() : 0;
            display_take(s_dma_ready,DISPLAY_FAULT_DMA);
            uint16_t *out = s_rotated[slot];
            int64_t r0 = esp_timer_get_time();
            if (probing) nos_pipeline_dmawait(&s_pipeline,(uint32_t)(r0-credit_begin));
#if CONFIG_WATCH_BOARD_143C
            /* The C panel's MADCTL handles orientation. Keep pixels aligned
             * with the CST820's mirrored coordinates; no B-board CCW turn. */
            for(int y=0;y<rows;y++)for(int x=0;x<j.w;x++)
                out[y*j.w+x]=__builtin_bswap16(j.buf[(offset+y)*j.stride+x]);
#else
            board_pixels_ccw_swap(j.buf + offset * j.stride, j.stride,
                                  out, j.w, rows, 1);
#endif
            int64_t r1 = esp_timer_get_time();
            s_rot_us += (uint32_t)(r1-r0);
            if (probing) nos_pipeline_rotation(&s_pipeline,(uint32_t)(r1-r0));
            /* Last raw read is complete. The producer may overwrite it now;
             * the final draw reads only the independently owned rotated slot. */
            if (offset + rows == j.rows) xSemaphoreGive(s_buf_free);
            slot ^= 1;
            /* UI (x,y) -> native (y,465-x); offset changes native x only. */
            /* Gate once per painted frame, after preparing the first chunk.
             * Exposure fences and subsequent stripes must not wait for TE. */
            if(first_stripe) {nos_display_te_wait();first_stripe=0;}
            int64_t api_begin = probing ? esp_timer_get_time() : 0;
            if (probing) nos_pipeline_enter(&s_pipeline,api_begin,(uint32_t)(j.w*rows*2));
            esp_err_t error;
#if CONFIG_WATCH_BOARD_143C
            error=esp_lcd_panel_draw_bitmap(s_panel,j.x0,j.y0+offset,j.x0+j.w,j.y0+offset+rows,out);
#else
            error=esp_lcd_panel_draw_bitmap(s_panel,j.y0+offset,
                GFX_W-j.x0-j.w,j.y0+offset+rows,GFX_W-j.x0,out);
#endif
            if(error!=ESP_OK)display_fault(DISPLAY_FAULT_DRAW,error);
            if (probing) nos_pipeline_return(&s_pipeline,api_begin,esp_timer_get_time());
        }
    }
}

/* Light theme: the panel inverts in hardware while the screen is fully on (AOD and the
 * dim-out into it stay dark). The accents flip with it (ui_theme_inverted) and the next
 * frame repaints everything, queued behind the command so no frame lands half-themed. */
static int s_inverted;
static void theme_apply(int want)
{
    if (want == s_inverted || !s_flush_q) return;
    panel_command(JOB_INVERT,want);
    s_inverted=s_panel_ack[1].applied;
    ui_theme_inverted(s_inverted);
}

void display_set_brightness(uint8_t level)
{
    if(!s_io) {s_panel_ack[0].requested=level;return;}
    if(s_panel_ack[0].applied==level && s_panel_ack[0].request==s_panel_ack[0].ack)return;
    panel_command(JOB_BRIGHT,level);
}

static void lcd_init(void)
{
#ifdef PIN_LCD_EN
    gpio_config_t en = { .pin_bit_mask = 1ULL << PIN_LCD_EN, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&en));
    ESP_ERROR_CHECK(gpio_set_level(PIN_LCD_EN, 1));
    vTaskDelay(pdMS_TO_TICKS(20));
#endif
    uint8_t id = board_lcd_probe();
    ESP_LOGI(TAG, "PANEL id=0x%02x init=%s", id, LCD_USE_SH8601 ? "SH8601" : "CO5300");
    const spi_bus_config_t bus = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_PCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3, GFX_W * DMA_ROWS * 2 + 64);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io_cfg = SH8601_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, on_dma_done, NULL);
    io_cfg.pclk_hz = LCD_SPI_HZ;
    ESP_LOGI(TAG, "Display qspi_requested_hz=%d present_target_hz=%d animation_hz=%d",
             LCD_SPI_HZ, NOS_PRESENT_FPS, NOS_FPS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io));
    sh8601_vendor_config_t vendor = {
        .init_cmds = LCD_USE_SH8601 ? sh8601_init : co5300_init,
        .init_cmds_size = LCD_USE_SH8601 ? sizeof sh8601_init / sizeof sh8601_init[0]
                                         : sizeof co5300_init / sizeof co5300_init[0],
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, PANEL_GAP_X, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    nos_display_te_init();
}

/* engine stripe hooks */
static void stripe_wait(void)
{
    int64_t t = esp_timer_get_time();
    display_take(s_buf_free,DISPLAY_FAULT_STRIPE);
    s_wait_us += (uint32_t)(esp_timer_get_time() - t);
}
static void stripe_flush(uint16_t *buf, int stride, int x0, int y0, int w, int rows)
{
    int slot = buf - x0 == s_stripe[0] ? 0 : 1;
    assert(buf - x0 == s_stripe[slot]);
    flush_job j = { .buf=buf,.x0=x0,.y0=y0,.w=w,.rows=rows,.stride=stride,.kind=JOB_STRIPE,
                    .exposure=s_exposure_ready[slot]?&s_exposure_job[slot]:NULL };
    int probing = s_pipeline.active;
    int64_t begin = probing ? esp_timer_get_time() : 0;
    display_queue(&j);
    if (probing) nos_pipeline_submit(&s_pipeline,(uint32_t)(esp_timer_get_time()-begin));
}
/* Every stripe submitted and DMA finished: pixels are in panel GRAM, not a vblank fence. */
static void frame_drain(void)
{
    flush_job fence = { .kind = JOB_FENCE };
    display_queue(&fence);
    display_take(s_frame_done,DISPLAY_FAULT_FENCE);
}
static int stripe_exposure_prepare(const gfx_fb *fb, int x0, int y0, int x1, int y1, int slot)
{
    if(NOS_RENDER_SHARP)return 0;
    s_exposure_ready[slot] = s_exposure_worker &&
        nos_exposure_prepare(&s_exposure_job[slot], fb, x0, y0, x1, y1, nos_temporal_blur, nos_aod);
    return s_exposure_ready[slot];
}
static uint32_t stripe_exposure_finish(void)
{
    flush_job fence = { .kind = JOB_EXPOSURE_FENCE };
    display_queue(&fence);
    display_take(s_frame_done,DISPLAY_FAULT_FENCE);
    /* The queue fence establishes completion before this cross-core read/reset.
     * Return only work since the previous fence, including mid-frame fallback. */
    uint32_t us = s_worker_history_us;
    s_worker_history_us = 0;
    return us;
}

/* ------------------------------------------------------------------ button */
#define BTN_DEBOUNCE_MS 25
static portMUX_TYPE s_btn_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_btn_down;
static volatile uint32_t s_btn_edge_ms, s_btn_presses;
static void IRAM_ATTR btn_isr(void *arg)
{
    uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL_ISR(&s_btn_mux);
    int pressed = gpio_get_level(GPIO_NUM_0) == 0;
    if (pressed != s_btn_down && t - s_btn_edge_ms >= BTN_DEBOUNCE_MS) {
        s_btn_down = pressed; s_btn_edge_ms = t;
        if (pressed) s_btn_presses++;
    }
    portEXIT_CRITICAL_ISR(&s_btn_mux);
}
/* Automatic light sleep (docs/LIGHT_SLEEP.md). The button is the only GPIO wake source. It is
 * armed as a level wake for the duration of one sleep only, because gpio_wakeup_enable replaces
 * the pad's ANYEDGE interrupt type; the wake itself consumes the press edge, so the exit hook
 * records it for the main loop. These hooks run in the idle task inside IDF's sleep critical
 * section: no blocking, no logging. */
#if !CONFIG_WATCH_BOARD_143C
static void btn_sleep_before(void)
{
    /* Automatic light sleep isolates every pad (CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND). The
     * panel (RST/EN/QSPI) and the SD card must keep their driven levels, or the AOD frame and
     * the card state are lost. Re-asserted per sleep because drivers may reset the pads. */
    static const uint8_t keep[]={PIN_LCD_CS,PIN_LCD_PCLK,PIN_LCD_D0,PIN_LCD_D1,PIN_LCD_D2,PIN_LCD_D3,
                                 PIN_LCD_RST,PIN_LCD_EN,PIN_SD_CLK,PIN_SD_CMD,PIN_SD_D0};
    for(unsigned i=0;i<sizeof keep;i++) gpio_sleep_sel_dis((gpio_num_t)keep[i]);
    gpio_wakeup_enable(GPIO_NUM_0,GPIO_INTR_LOW_LEVEL);
}
static void btn_sleep_after(int64_t slept_us)
{
    (void)slept_us;
    int woke=(esp_sleep_get_wakeup_causes()&(1u<<ESP_SLEEP_WAKEUP_GPIO))!=0;
    gpio_wakeup_disable(GPIO_NUM_0);
    gpio_set_intr_type(GPIO_NUM_0,GPIO_INTR_ANYEDGE);
    uint32_t t=(uint32_t)(esp_timer_get_time()/1000);
    portENTER_CRITICAL_SAFE(&s_btn_mux);
    if(woke && !s_btn_down) {
        s_btn_presses++;s_btn_edge_ms=t;
        s_btn_down=gpio_get_level(GPIO_NUM_0)==0; /* a press that already ended stays a counted press */
    }
    portEXIT_CRITICAL_SAFE(&s_btn_mux);
}
#endif

static void dotbake_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    nos_power_bake_begin();
    dot_prebake_all();
    unsigned shade_bad = orrery_shade_q16_prebake();
    nos_power_bake_end();
    printf("PREBAKE elapsed_ms=%d min_free_bytes=%u shade_mismatches=%u shade_ready=%d shade_rejection=%d\n",
           (int)((esp_timer_get_time() - t0) / 1000),
           (unsigned)uxTaskGetStackHighWaterMark(NULL), shade_bad,
           orrery_shade_q16_ready(), orrery_shade_q16_rejection());
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ touch
 * The UI runs at NOS_PRESENT_FPS but the controller reports at ~60-100 Hz: reading it once per
 * frame left the velocity fit ~3 samples and a frame of extra drag latency. touch_task
 * polls it every 10 ms (40 ms while the display is not on), timestamps every edge and
 * every new position (and a repeat every 25 ms while still, so stillness is visible), and
 * the UI loop drains the ring right before nos_frame_run (latch the newest before tick). */
#define TOUCH_RING 16
#define TOUCH_POLL_MS 10
typedef struct { int16_t x, y; uint8_t down; uint32_t ms; } touch_smp;
static touch_smp s_tring[TOUCH_RING];            /* 192 B */
static volatile uint32_t s_tw, s_tr;
static portMUX_TYPE s_tmux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_touch_fast = 1;            /* 0: display not on, poll slowly */
static volatile int s_touch_now;                 /* the finger is down (latest poll) */
static volatile int s_touch_any;                 /* a down sample was queued since the last drain */
static int s_touch_task_ok;
/* I2C's rare timeout path invokes full newlib logging. The previous 2560-byte
 * stack overflowed during a device run with I2C errors; ordinary polls alone
 * do not establish the required stack. Report the observed minimum after errors. */
#define TOUCH_STACK_BYTES 4096
static TaskHandle_t s_touch_handle;
typedef struct {
    uint32_t polls,poll_ms,valid_ms,cancels;
    int raw_sample,suppressed;
} touch_health;
static touch_health s_touch_health; /* published with the touch ring lock */
static void touch_push(int x, int y, int down, uint32_t ms)
{
    portENTER_CRITICAL(&s_tmux);
    if (s_tw - s_tr >= TOUCH_RING) s_tr++;       /* full (the UI stalled): drop the oldest */
    touch_smp *t = &s_tring[s_tw % TOUCH_RING];
    t->x = (int16_t)x; t->y = (int16_t)y; t->down = (uint8_t)down; t->ms = ms;
    s_tw++;
    if(down==1)s_touch_any=1;
    portEXIT_CRITICAL(&s_tmux);
}
static int touch_pop(touch_smp *out)
{
    int ok = 0;
    portENTER_CRITICAL(&s_tmux);
    if (s_tr != s_tw) { *out = s_tring[s_tr % TOUCH_RING]; s_tr++; ok = 1; }
    portEXIT_CRITICAL(&s_tmux);
    return ok;
}
/* one poll: push what changed */
static void touch_poll(int *px, int *py, int *was, int *lx, int *ly, uint32_t *lms)
{
    static nos_touch_contact contact;
    int rx = 0, ry = 0;
    int d = board_touch_read(&rx, &ry);
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);
    int raw_sample=d;
    d=nos_touch_contact_sample(&contact,d,ms);
    s_touch_now=contact.down;
    portENTER_CRITICAL(&s_tmux);
    s_touch_health.polls++;s_touch_health.poll_ms=ms;s_touch_health.raw_sample=raw_sample;
    if(raw_sample>=0)s_touch_health.valid_ms=ms;
    if(d==2)s_touch_health.cancels++;
    s_touch_health.suppressed=contact.suppressed;
    portEXIT_CRITICAL(&s_tmux);
    if(d<0)return;
    if(d==2) {
        touch_push(*px,*py,2,ms);*was=0;*lms=ms;return;
    }
    board_pointer_update(px, py, rx, ry, d);     /* rotate; holds the last point on release */
    if (d != *was || (d && (*px != *lx || *py != *ly || ms - *lms >= 25))) {
        touch_push(*px, *py, d, ms);
        *lx = *px; *ly = *py; *lms = ms;
    }
    *was = d;
}
static void touch_task(void *arg)
{
    int px = 0, py = 0, was = 0, lx = -1, ly = -1;
    uint32_t lms = 0;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        touch_poll(&px, &py, &was, &lx, &ly, &lms);
        if (s_touch_fast) vTaskDelayUntil(&last, pdMS_TO_TICKS(TOUCH_POLL_MS));
        else { sleep_grid_delay(50); last = xTaskGetTickCount(); } /* shared light-sleep wake grid */
    }
}
/* Hand the queued samples to the engine (feed = 1) or drop them. The touch that woke the
 * display is swallowed until its release, so it never also taps. */
static int s_swallow;
static void touch_drain(int feed)
{
    touch_smp t;
    s_touch_any = 0;
    while (touch_pop(&t)) {
        /* A lost wake contact ends suppression too. Its later confirmed UP is
         * consumed by the contact filter, so waiting for a queued UP can
         * otherwise swallow the user's next complete gesture. */
        if(t.down==2)s_swallow=0;
        if (s_swallow) { if (!t.down) s_swallow = 0; continue; }
        if (feed) nos_touch_at(t.x, t.y, t.down, t.ms);
    }
}
static void touch_state_report(void)
{
    touch_health health;
    portENTER_CRITICAL(&s_tmux);health=s_touch_health;portEXIT_CRITICAL(&s_tmux);
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
    board_touch_diag counts=board_touch_diagnostics();
    board_device_diag devices[BOARD_DEVICE_COUNT];board_device_diagnostics(devices);
    const board_device_diag *device=&devices[BOARD_DEVICE_TOUCH];
    printf("NOSTOUCHSTATE v=1 polls=%u poll_age_ms=%u valid_age_ms=%u raw=%d suppressed=%d swallowed=%d down=%d cancels=%u task_state=%d reads=%u errors=%u available=%d attempts=%u failures=%u consecutive=%u retry_ms=%u last_error=%d\n",
        (unsigned)health.polls,(unsigned)(health.polls?now-health.poll_ms:UINT32_MAX),
        (unsigned)(health.valid_ms?now-health.valid_ms:UINT32_MAX),health.raw_sample,
        health.suppressed,s_swallow,s_touch_now,(unsigned)health.cancels,
        s_touch_handle?(int)eTaskGetState(s_touch_handle):-1,
        (unsigned)counts.reads,(unsigned)counts.errors,device->available,
        (unsigned)device->attempts,(unsigned)device->failures,(unsigned)device->consecutive,
        (unsigned)device->retry_in_ms,device->last_error);
}

/* ------------------------------------------------------------------ power */
enum { PWR_ON = 0, PWR_AOD, PWR_OFF, PWR_SENSOR };
static int s_pwr = PWR_ON;
static int s_cur_level = -1;          /* last panel brightness sent */
static float s_auto_k = 1.0f;         /* ambient multiplier, smoothed */
static int s_no_sleep;
static int s_aod_lead;                /* AOD entry runs at the active frame rate */
static int s_cpu_mhz=240;
static int s_low_battery;
static power_crit s_crit;                   /* critical-battery latch (power_policy.h, G14) */
static uint32_t s_on_since_ms;              /* hal_millis() when the display last went ON */
static const nos_screen *s_keep_released;   /* KEEPAWAKE screen whose hold the inactivity cap released (G99) */
static void cpu_mode(int mhz)
{
    if(s_cpu_mhz==mhz)return;
    static int64_t retry;
    int64_t now=esp_timer_get_time();
    if(now<retry)return;
    int err=nos_power_set_max_mhz(mhz);
    if(err==ESP_OK) {
        s_cpu_mhz=mhz;retry=0;
        sports_power(s_pwr==PWR_ON?SP_ACTIVE:s_pwr==PWR_SENSOR?SP_SENSOR:s_pwr==PWR_AOD?SP_AOD:SP_DISPLAY_OFF,mhz,s_cur_level);
    }
    else {retry=now+1000000;float v[]={-1,(float)err,(float)mhz};sports_emit(SP_ROUTINE,v,3);}
}

static int user_level(void)
{
    int pct = nos_prefs.brightness;
    if (pct < 1) pct = 1;
    float lvl = 6.0f + pct * 2.49f;               /* 1-100 % -> 8-255 */
    if (nos_prefs.auto_bright) lvl *= s_auto_k;
    if (lvl > 255) lvl = 255;
    if (lvl < 4) lvl = 4;
    return (int)lvl;
}
/* always-on: the user's level (Settings > Always-on brightness; there is no light
 * sensor to follow in a dark room) */
static int aod_level(void) { return prefs_pct_level(nos_prefs.aod_bright); }
static void set_level(int lvl)
{
    if(lvl==s_cur_level && lvl==s_panel_ack[0].applied)return;
    display_set_brightness((uint8_t)lvl);
    s_cur_level=s_panel_ack[0].applied;
    sports_power(s_pwr==PWR_ON?SP_ACTIVE:s_pwr==PWR_SENSOR?SP_SENSOR:s_pwr==PWR_AOD?SP_AOD:SP_DISPLAY_OFF,s_cpu_mhz,s_cur_level);
}
static void auto_brightness_tick(void)
{
    static uint32_t last;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - last < 500) return;
    last = now;
    hal_light l;
    if (!nos_prefs.auto_bright || !hal_light_get(&l) || !l.valid) { s_auto_k = 1.0f; return; }
    /* 0 lux -> 0.35x, 100 lux -> 0.9x, 1000 lux -> 1.15x, 10k+ -> 1.4x (log response) */
    float lg = log10f(l.lux + 1.0f);
    float k = 0.35f + 0.27f * lg;
    if (k > 1.4f) k = 1.4f;
    s_auto_k += (k - s_auto_k) * 0.25f;
}

void hal_battery_set_awake(int awake);         /* hal_esp32.c: battery gauge */
int hal_battery_cal_mv(void);
void hal_battery_cal_set(int mv);
static void panel_sleep(int sleep)
{
    if(sleep==s_panel_ack[2].applied || !s_flush_q)return;
    panel_command(JOB_SLEEP,sleep);
}
static void go_state(int st)
{
    if (st != s_pwr) {nos_frame_capture_abort(&s_capture);ui_fblur_observe_enable(0);}
    if (st == s_pwr) return;
    diag_log_event(DIAG_POWER_STATE,s_pwr,st,s_low_battery);
    if (s_pwr == PWR_OFF) panel_sleep(0);        /* leaving off: wake the panel before the first frame */
    if(st==PWR_OFF) {set_level(0);panel_sleep(1);}
    s_pwr = st;
    nos_power_display_hold(st!=PWR_OFF); /* light sleep only while the panel is off; the main loop refines AOD */
    if(st!=PWR_OFF)cpu_mode(240);
    s_touch_fast = st == PWR_ON;
    hal_battery_set_awake(st == PWR_ON);         /* preserve the gauge's load allowance */
    if (st == PWR_ON) {
        s_on_since_ms = hal_millis(); s_keep_released = NULL;
        prefs_set_persistence_allowed(0); board_set_background_persistence(0);
        nos_aod = 0; nos_invalidate(); nos_keep_awake(); }
    else if (st == PWR_AOD || st == PWR_SENSOR) { nos_aod = 1; nos_home_now(); nos_invalidate(); s_aod_lead = 1; }
    sports_power(st==PWR_ON?SP_ACTIVE:st==PWR_SENSOR?SP_SENSOR:st==PWR_AOD?SP_AOD:SP_DISPLAY_OFF,s_cpu_mhz,s_cur_level);
}

/* hal_ext.h: 0 off, 1 AOD, 2 on (the power sheet's "Sleep" calls this) */
void hal_display_power(int mode) { go_state(mode == 2 ? PWR_ON : mode == 1 ? PWR_AOD : PWR_OFF); }
int hal_display_mode(void) {return s_pwr==PWR_ON?2:s_pwr==PWR_OFF?0:1;}
/* board.c minute log: exact power state, last panel level sent (-1 none yet), CPU cap. */
const char *nos_power_state_name(void) {return s_pwr==PWR_ON?"on":s_pwr==PWR_AOD?"aod":s_pwr==PWR_SENSOR?"sensor":"off";}
int nos_power_brightness(void) {return s_cur_level;}
int nos_power_cpu_mhz(void) {return s_cpu_mhz;}

static int s_shutdown_mode,s_shutdown_phase;
static uint32_t s_shutdown_retry,s_shutdown_report;
int nos_host_shutdown_pending(void) {return s_shutdown_mode!=0;}
static int shutdown_cancel(void)
{
    if(!s_shutdown_mode)return 1;
    int resumed=board_cancel_sleep();hal_ext_shutdown_cancel();
    go_state(PWR_ON);nos_keep_awake();nos_invalidate();
    if(resumed) {
        s_shutdown_mode=s_shutdown_phase=0;s_shutdown_retry=0;sports_quiesce(0);
        nos_toast("SHUTDOWN CANCELLED");
    } else {
        /* A late SDK teardown error cannot be rolled back. Keep admission
         * closed and stop automatic sleep; the UI can export diagnostics. */
        s_shutdown_phase=3;nos_toast("STORAGE RESTART REQUIRED");
    }
    diag_log_event(DIAG_SLEEP_BLOCK,32,resumed,s_shutdown_mode);return resumed;
}
static int shutdown_guard(void)
{
    if(ota_busy())return 1;
    if(voice_chat_active()){voice_chat_cancel();return 4;}
    hal_imu_record rec; hal_imu_record_get(&rec);
    hal_imu_file file; hal_imu_file_get(&file);
    hal_imu_stream stream;hal_imu_stream_get(&stream);
    if((rec.state!=HAL_IMU_IDLE && rec.state!=HAL_IMU_ERROR) || file.busy ||
       imu_worker_pending() || stream.hz || stream.queued)return 2;
    int audio=hal_rec_status();if(hal_rec_active() || audio==1 || audio==2)return 4;
    const workout_snapshot *workout=workout_get();workout_storage_status storage;
    workout_storage_get(&storage);
    if(workout && (workout->state==WORKOUT_RUNNING || workout->state==WORKOUT_PAUSED ||
       (workout->state==WORKOUT_FINISHED && workout->save_state!=WORKOUT_SAVE_SAVED)))return 3;
    if(!storage.ready || storage.busy || storage.dirty || storage.uncertain || svc_track_active() ||
       svc_track_pending() || svc_track_saving())return 3;
    return 0;
}
/* power sheet hook: 0 display sleep, 1 restart, 2 deep sleep. Only request here:
 * peripheral and SD owners acknowledge asynchronously while ordinary UI runs. */
void nos_host_power(int mode)
{
    if(mode<0) {(void)shutdown_cancel();return;}
    if(mode==0) {if(!s_shutdown_mode)go_state(PWR_OFF);return;}
    if(mode<1 || mode>2 || s_shutdown_mode)return;
    int blocked=shutdown_guard();
    if(blocked) {nos_toast(blocked==1?"UPDATE ACTIVE":blocked==2?"STOP IMU FIRST":"SAVE RECORDING FIRST");return;}
    s_shutdown_mode=mode;s_shutdown_phase=0;s_shutdown_retry=s_shutdown_report=0;
    sports_quiesce(1);diag_log_event(DIAG_POWER_POLICY,0x100,mode,s_pwr);
    go_state(PWR_ON);nos_keep_awake();prefs_save();hal_asset_cancel();
    nos_toast("WAITING FOR SHUTDOWN");
}
static void shutdown_tick(uint32_t now)
{
    if(!s_shutdown_mode)return;
    nos_keep_awake();
    if(s_shutdown_phase==3)return;
    if(s_shutdown_retry && (int32_t)(now-s_shutdown_retry)<0)return;
    int blocked=shutdown_guard();
    if(blocked) {
        (void)shutdown_cancel();nos_toast("SHUTDOWN BLOCKED");
        return; /* ownership cannot be surrendered while a producer is active */
    }
    orrery_card_asset_poll();
    if(hal_asset_busy())return;
    if(!s_shutdown_phase) {
        voice_chat_maintenance();
        if(voice_chat_history_pending() || !prefs_flush()) {s_shutdown_retry=now+1000;nos_toast("PREFERENCES SAVE FAILED");return;}
        hal_ext_shutdown_begin();s_shutdown_phase=1;
    }
    int peripherals=hal_ext_shutdown_poll();
    if(peripherals!=1) {
        if(peripherals<0 && (!s_shutdown_report || now-s_shutdown_report>=5000)) {
            diag_log_event(DIAG_SLEEP_BLOCK,8,peripherals,s_shutdown_mode);
            nos_toast("PERIPHERALS STILL BUSY");s_shutdown_report=now;
        }
        return; /* timeout is not a peripheral power-off acknowledgment */
    }
    /* Preferences may change while the UI is still interactive. Check them
     * again before asking the board to park its filesystem/sampler owners. */
    if(s_shutdown_phase==2 && (prefs_pending() || voice_chat_history_pending())) {
        if(!board_cancel_sleep()) {(void)shutdown_cancel();return;}
        s_shutdown_phase=1; /* fresh edits require a real new filesystem drain */
    }
    voice_chat_maintenance();
    if(voice_chat_history_pending() || !prefs_flush()) {s_shutdown_retry=now+1000;return;}
    int board=board_prepare_sleep();
    if(board!=BOARD_SLEEP_READY) {
        if(board==BOARD_SLEEP_FAILED) {
            (void)shutdown_cancel();
            diag_log_event(DIAG_SLEEP_BLOCK,16,board,s_shutdown_mode);
        } else s_shutdown_phase=2;
        return;
    }
    if(shutdown_guard() || prefs_loading() || prefs_pending() || voice_chat_history_pending()) {(void)shutdown_cancel();return;}
    set_level(0);panel_sleep(1);frame_drain();
    diag_log_event(DIAG_POWER_POLICY,0x101,s_shutdown_mode,PWR_OFF); /* actual parked barrier; retained RTC only */
    vTaskDelay(pdMS_TO_TICKS(150));
    if(s_shutdown_mode==1)esp_restart();
    nos_power_set_sleep_hooks(NULL,NULL);esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep_start();
}

/* G14: critical battery. WARN (qualified <=5 % for 30 s): toast, checkpoint preferences and any workout now,
 * end the optional recorders cleanly. SHUTDOWN (<=2 % for 60 s): finish and save the workout and GPS track,
 * then take the normal orderly deep-sleep path (nos_host_power), retrying until the guard releases. */
static void critical_stop_recorders(void)
{
    hal_imu_record rec;hal_imu_record_get(&rec);
    if(rec.state==HAL_IMU_RECORDING)hal_imu_record_stop();
    hal_imu_stream stream;hal_imu_stream_get(&stream);
    if(stream.hz)hal_imu_stream_stop();
    svc_memo_stop();
}
static void critical_battery_tick(const hal_power *p,uint32_t now)
{
    static uint32_t last,retry;
    int before=s_crit.level,level=power_critical_update(&s_crit,p,last?now-last:0);last=now;
    if(level!=before) {
        diag_log_event(DIAG_POWER_POLICY,0x200+level,p->battery_percent,p->rail_mv);
        printf("NOSCRIT level=%d percent=%d rail=%d\n",level,p->battery_percent,p->rail_mv);
        if(level==NOS_CRIT_WARN) {
            if(s_pwr==PWR_ON)nos_toast("CRITICAL BATTERY / SAVING");
            prefs_save();workout_retry_save();critical_stop_recorders();
        }
    }
    if(level!=NOS_CRIT_SHUTDOWN || s_shutdown_mode || (int32_t)(now-retry)<0)return;
    retry=now+1000;
    const workout_snapshot *w=workout_get();
    if(w->state==WORKOUT_RUNNING || w->state==WORKOUT_PAUSED)workout_finish();
    else if(w->state==WORKOUT_FINISHED && w->save_state==WORKOUT_SAVE_FAILED)workout_retry_save();
    if(svc_track_active() || svc_track_pending())svc_track_stop();
    critical_stop_recorders();
    if(!shutdown_guard())nos_host_power(2);
}

/* ------------------------------------------------------------------ serial */
typedef struct { char text[192]; char request_id[9]; } cmd_line;
static QueueHandle_t s_cmd_q;
static SemaphoreHandle_t s_host_reply_lock;
static TaskHandle_t s_command_handle;
static int s_cmd_ok;
static int cmd_printf(const char *format,...)
{
    if(!strncmp(format,"ERR",3)) s_cmd_ok=0;
    va_list args;va_start(args,format);int n=vprintf(format,args);va_end(args);return n;
}
static int power_restart_request(void)
{
    if(s_shutdown_phase!=3) {cmd_printf("ERR power restart recovery_only\n");return 0;}
    int blocked=shutdown_guard();
    if(blocked) {
        static const char *const reason[]={"none","ota_busy","imu_owner","recording_pending","audio_owner"};
        cmd_printf("ERR power restart %s\n",reason[blocked]);return 0;
    }
    if(prefs_loading()) {cmd_printf("ERR power restart preferences_unknown\n");return 0;}
    if(!prefs_flush()) {cmd_printf("ERR power restart preferences_not_durable\n");return 0;}
    diag_log_event(DIAG_POWER_POLICY,0x102,s_shutdown_mode,s_shutdown_phase);
    esp_restart();return 1;
}
static int s_inj_active, s_inj_x0, s_inj_y0, s_inj_x1, s_inj_y1, s_inj_ms;
static int64_t s_inj_t0;
static nos_button_pulse s_serial_btn;
static int s_perf;
static int s_trace;
extern uint32_t nos_dbg_us[4];                   /* print the next N painted frames (NOSF lines) */

#include "sd_xfer.h"                            /* SD file transfer over USB and BLE (tools/sdsync.py) */
static int s_transfer_ok;
static int host_transfer_read(uint8_t *bytes,int count,int timeout_ms)
{return usb_serial_jtag_read_bytes(bytes,(uint32_t)count,pdMS_TO_TICKS(timeout_ms));}
static void host_transfer_line(const char *line)
{
    if(strstr(line," ERR "))s_transfer_ok=0; /* MISSING is a completed typed absence */
    printf("%s\n",line);fflush(stdout);
}
static const xfer_io s_host_transfer_io={host_transfer_read,host_transfer_line,2048};
static void host_done(const char *id,int ok,const char *code)
{
    if(id[0]) printf("DONE %s ok=%d%s%s\n",id,ok,ok?"":" code=",ok?"":code);
    else printf("DONE\n");
}
static void host_transfer_execute(const char *id,const char *command)
{
    if(id[0])printf("BEGIN %s\n",id);
    s_transfer_ok=1;sd_xfer_exec(command,&s_host_transfer_io);
    if(id[0])host_done(id,s_transfer_ok,"transfer");
}
static void host_input_reject(const command_input *input)
{
    int32_t bytes=input->bytes>INT32_MAX?INT32_MAX:(int32_t)input->bytes;
    diag_log_event(DIAG_COMMAND_INPUT,input->reason,bytes,input->first_bad);
    /* Numeric evidence only. A standalone NUL is the intentional host
     * recovery barrier; neither contents nor a full-line hash are logged. */
    if(input->bytes!=1 || input->nul_count!=1 || input->first_nul_offset!=1)
        diag_log_event(DIAG_COMMAND_INPUT_DETAIL,
            input->first_bad_offset>INT32_MAX?INT32_MAX:(int32_t)input->first_bad_offset,
            input->first_nul_offset>INT32_MAX?INT32_MAX:(int32_t)input->first_nul_offset,
            input->nul_count>INT32_MAX?INT32_MAX:(int32_t)input->nul_count);
    printf("ERR command input reason=%u bytes=%lu bad=%u bad_offset=%lu nul_count=%lu first_nul=%lu last_nul=%lu\nDONE\n",
        (unsigned)input->reason,(unsigned long)input->bytes,(unsigned)input->first_bad,
        (unsigned long)input->first_bad_offset,(unsigned long)input->nul_count,
        (unsigned long)input->first_nul_offset,(unsigned long)input->last_nul_offset);
}
static void cmd_task(void *arg)
{
    command_input input; command_input_reset(&input);
    _Static_assert(sizeof(((cmd_line *)0)->text)==COMMAND_INPUT_CAP,"command queue/parser capacity");
    for (;;) {
        uint8_t ch;
        ble_xfer_poll();                         /* a file command from Bluetooth runs here too */
        uint32_t wait_ms = s_touch_fast ? 50u : sleep_grid_delay_ms(sleep_grid_now_ms(), 50u);
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(wait_ms)) <= 0) continue;
        int result=command_input_push(&input,ch);
        if(result==COMMAND_INPUT_NONE)continue;
        if(input.bytes)nos_usb_diag_usb_line();
        if(result==COMMAND_INPUT_REJECTED) {
            host_input_reject(&input);
        } else if(result==COMMAND_INPUT_READY) {
            /* SD consumes its raw bytes synchronously through io->read. Neither
             * upload data nor download ACKs pass through command validation. */
            char id[9];const char *command;
            int envelope=host_request_parse(input.text,id,&command);
            if(envelope<0) printf("ERR command envelope invalid\nDONE\n");
            else if(host_request_transfer(command)) {
                if(xSemaphoreTake(s_host_reply_lock,pdMS_TO_TICKS(3000))==pdTRUE) {
                    nos_usb_diag_reply_begin();
                    host_transfer_execute(id,command);
                    nos_usb_diag_reply_end();xSemaphoreGive(s_host_reply_lock);
                } else {
                    if(id[0]) printf("BEGIN %s\n",id);
                    printf("ERR command busy\n");host_done(id,0,"busy");
                }
            } else {
                cmd_line line={0};snprintf(line.text,sizeof line.text,"%s",command);
                memcpy(line.request_id,id,sizeof id);
                if(xQueueSend(s_cmd_q,&line,pdMS_TO_TICKS(50))==pdTRUE)nos_usb_diag_queue_accepted();
                else {
                    nos_usb_diag_queue_failed();if(id[0]) printf("BEGIN %s\n",id);
                    printf("ERR command queue full\n");host_done(id,0,"busy");
                }
            }
        }
        command_input_reset(&input);
    }
}

static const nos_screen *screen_by_name(const char *n)
{
    for (int i = 0; i < nos_app_count; i++)
        if (!strcasecmp(nos_app_list[i].name, n)) return nos_app_list[i].scr;
    static const struct { const char *n; const nos_screen *s; } extra[] = {
        { "control", &scr_control }, { "notifs", &scr_notifs }, { "launcher", &scr_launcher },
        { "faces", &scr_faces }, { "power", &scr_power }, { "modules", &scr_modules },
        { "about", &scr_about }, { "activity", &scr_activity }, { "heart", &scr_heart }, { "imu", &scr_imu },
    };
    for (unsigned i = 0; i < sizeof extra / sizeof extra[0]; i++)
        if (!strcasecmp(extra[i].n, n)) return extra[i].s;
    return 0;
}

static uint32_t s_wtrace_until;        /* serial "wake trace S": per-sample detector lines until then */
static int wake_wrist_sync(int force)
{
    static uint32_t revision,retry;static int valid;
    if(prefs_loading())return 0;
    wake_info current;wake_service_get(&current);
    int wrist=nos_prefs.wrist_left?0:1;
    uint32_t now=hal_millis(),wanted=prefs_wrist_revision();
    if(valid && current.wrist==wrist && revision==wanted)return 1;
    if(!force && retry && (int32_t)(now-retry)<0)return 0;
    if(!wake_service_set("wrist",wrist)) {retry=now+1000;return 0;}
    revision=wanted;retry=0;valid=1;return 1;
}
static int wake_learn_request(void)
{if(!wake_wrist_sync(1))return 0;wake_service_learn();return 1;}
static void radio_preferences(void)
{
    static int ble=-1,wifi=-1;
    /* Unknown card state must not turn default radio intents into RF activity.
     * A later authoritative load reapplies both saved intents exactly once. */
    int ready=!prefs_loading(),want_ble=ready && nos_prefs.ble,want_wifi=ready && nos_prefs.wifi;
    if(ble!=want_ble) {hal_ble_set_enabled(want_ble);ble=want_ble;}
    if(wifi!=want_wifi) {hal_wifi_set_enabled(want_wifi);wifi=want_wifi;}
}
static void calendar_preferences(void)
{
    if(g_tz_offset_min!=nos_prefs.tz_min) {g_tz_offset_min=nos_prefs.tz_min;board_apply_timezone();}
    board_calendar_ready(!prefs_loading());
}
int hal_display_te_available(void) {return nos_display_te_snapshot().ready;}
static void display_preferences(void)
{
    static int te=-1;
    if(te!=nos_prefs.display_te) {
        nos_display_te_set_enabled(nos_prefs.display_te);te=nos_prefs.display_te;
    }
}
static void history_path_report(void)
{
    printf("NOSHISTORY v=1 native_copy=%d\n",nos_history_native_copy);
}
static void history_virtual_report(void)
{
    const nos_history_virtual_stats *v=nos_history_virtual_get_stats();
    printf("NOSHISTORYV v=1 enabled=%d virtual_pixels=%llu avoided_bytes=%llu materialize_bytes=%llu classify_pixels=%llu\n",
        nos_history_virtual_get(),(unsigned long long)v->virtual_pixels,(unsigned long long)v->avoided_bytes,
        (unsigned long long)v->materialize_bytes,(unsigned long long)v->classify_pixels);
}
static void native_menu_report(void)
{
    native_menu_pack_info pi;native_menu_pack_status(&pi);
    printf("NOSNATIVE v=1 rom=%d compatible=%d asset_bytes=%u source_sha256=%s pack=%s src=%s sharp=%d pack_bytes=%u records=%u load_ms=%u error=\"%s\"\n",
           nos_native_menu_rom_enabled,native_menu_assets_compatible(),
           (unsigned)native_menu_asset_bytes(),native_menu_source_sha256(),
           pi.state>0?"loaded":pi.state<0?"failed":"untried",pi.source,pi.sharp,(unsigned)pi.file_bytes,
           (unsigned)pi.records,(unsigned)pi.load_ms,pi.error);
    history_path_report();
}
/* A menu pack that failed at boot (card absent or mounted late, transient read error) is retried here with a
 * backoff; on success the dynamic-bake caches are retired exactly as `native_rom 1` does. */
static void native_menu_retry(void)
{
    if(nos_frame_capture_live(&s_capture)||s_inj_active||s_touch_now||s_touch_any||!native_menu_assets_retry_due())return;
    if(!native_menu_assets_reload())return;
    extern void sys_launcher_cache_reset(void),set_settings_cache_release(void);
    set_settings_cache_release();sys_launcher_cache_reset();
    nos_layers_invalidate();nos_history_reset();nos_invalidate();
    native_menu_report();
}
static void panel_state_report(void)
{
    printf("NOSDISPLAY v=1 brightness_requested=%d brightness_applied=%d invert_requested=%d invert_applied=%d sleep_requested=%d sleep_applied=%d errors=%d,%d,%d fault=%d\n",
        s_panel_ack[0].requested,s_panel_ack[0].applied,s_panel_ack[1].requested,s_panel_ack[1].applied,
        s_panel_ack[2].requested,s_panel_ack[2].applied,s_panel_ack[0].error,s_panel_ack[1].error,s_panel_ack[2].error,s_display_fault);
}
static void te_report(void)
{
    nos_display_te_stats te=nos_display_te_snapshot();
    cmd_printf("NOSTE v=1 gpio=%d ready=%d enabled=%d awake=%d error=%d edges=%u period_us=%u age_us=%u waits=%u synced=%u timeouts=%u last_wait_us=%u max_wait_us=%u total_wait_us=%llu phase_us=%u max_phase_us=%u saved=%u\n",
        te.gpio,te.ready,te.enabled,te.awake,te.error,(unsigned)te.edges,(unsigned)te.period_us,
        (unsigned)te.edge_age_us,(unsigned)te.waits,(unsigned)te.synced,(unsigned)te.timeouts,
        (unsigned)te.last_wait_us,(unsigned)te.max_wait_us,(unsigned long long)te.wait_total_us,
        (unsigned)te.last_phase_us,(unsigned)te.max_phase_us,(unsigned)nos_prefs.display_te);
}
static void runtime_state_report(void)
{
    /* Cached status only: do not call sensor/GNSS/media getters that renew a
     * lease or start I/O while an operator checks benchmark admission. */
    hal_imu_record imu;hal_imu_record_get(&imu);
    hal_imu_stream stream;hal_imu_stream_get(&stream);
    hal_imu_file file;hal_imu_file_get(&file);
    const workout_snapshot *workout=workout_get();
    workout_storage_status storage;workout_storage_get(&storage);
    portENTER_CRITICAL(&s_btn_mux);int button=s_btn_down;portEXIT_CRITICAL(&s_btn_mux);
    printf("NOSRUNTIME v=1 ota_busy=%d shutdown_pending=%d prefs_loading=%d prefs_dirty=%d imu_state=%d imu_pending=%d imu_hz=%u imu_queued=%u imu_file_busy=%d workout_ready=%d workout_state=%d workout_save=%d workout_dirty=%d workout_busy=%d workout_uncertain=%d workout_corrupt=%d workout_foreign=%d track_active=%d track_pending=%d track_saving=%d audio_status=%d audio_active=%d touch_down=%d touch_pending=%d button_down=%d injected=%d capture_state=%d asset_busy=%d radio_pending=%d\n",
        ota_busy(),nos_host_shutdown_pending(),prefs_loading(),prefs_pending(),imu.state,imu_worker_pending(),
        stream.hz,stream.queued,file.busy,storage.ready,workout?workout->state:-1,workout?workout->save_state:-1,
        storage.dirty,storage.busy,storage.uncertain,storage.corrupt,storage.foreign,
        svc_track_active(),svc_track_pending(),svc_track_saving(),hal_rec_status(),hal_rec_active(),
        s_touch_now,s_touch_any,button,s_inj_active,s_capture.state,hal_asset_busy(),hal_esp32_radios_pending());
}
static void settings_shutter_report(void)
{
    settings_shutter_stats s=set_settings_shutter_stats();
    printf("NOSSHUTTER enabled=%d ticks=%u moving_ticks=%u painted_pixels=%u filtered_pixels=%u fallback_calls=%u exposure_q8=%d pose_q8=%d\n",
        s.enabled,s.ticks,s.moving_ticks,s.painted_pixels,s.filtered_pixels,s.fallback_calls,
        s.exposure_q8,s.pose_q8);
}
static void pipeline_frame_report(const char *role,const nos_pipeline_frame *f)
{
    static const char *cost_names[]={"scene_other","clear","sprite8","sprite565","text","shape"};
    for(unsigned b=0;b<NOS_COST_COUNT;b++)printf("NOSCOSTFRAME role=%s frame_id=%u bucket=%s inclusive_us=%u exclusive_us=%u calls=%u\n",
        role,(unsigned)f->frame_id,cost_names[b],(unsigned)f->cost.inclusive[b],(unsigned)f->cost.exclusive[b],(unsigned)f->cost.calls[b]);
    printf("NOSPIPEFRAME role=%s frame_id=%u begin_us=%llu rawjobs=%u chunks=%u bytes=%u done=%u first_enter_off_us=%u first_return_off_us=%u last_return_off_us=%u first_done_off_us=%u last_done_off_us=%u api_sum_us=%u api_max_us=%u dmawait_sum_us=%u dmawait_max_us=%u rotation_sum_us=%u rotation_max_us=%u queuewait_sum_us=%u queuewait_max_us=%u done_gap_max_us=%u render_done_off_us=%u drain_enter_off_us=%u completed_off_us=%u e2e_us=%u tick_us=%u history_us=%u raster_us=%u ui_wait_us=%u record_us=%u compare_us=%u layer_record_us=%u layer_raster_us=%u layer_bands=%u layer_reused=%u producer_submit_sum_us=%u producer_submit_max_us=%u\n",
        role,(unsigned)f->frame_id,(unsigned long long)f->begin_us,
        (unsigned)f->rawjobs,(unsigned)f->chunks,(unsigned)f->bytes,(unsigned)f->done,
        (unsigned)f->first_enter,(unsigned)f->first_return,(unsigned)f->last_return,
        (unsigned)f->first_done,(unsigned)f->last_done,(unsigned)f->api_sum,(unsigned)f->api_max,
        (unsigned)f->dmawait_sum,(unsigned)f->dmawait_max,(unsigned)f->rotation_sum,(unsigned)f->rotation_max,
        (unsigned)f->queuewait_sum,(unsigned)f->queuewait_max,(unsigned)f->done_gap_max,
        (unsigned)f->render_done,(unsigned)f->drain_enter,(unsigned)f->completed,(unsigned)f->e2e,
        (unsigned)f->tick,(unsigned)f->history,(unsigned)f->raster,(unsigned)f->ui_wait,
        (unsigned)f->record,(unsigned)f->compare,(unsigned)f->layer_record,(unsigned)f->layer_raster,
        (unsigned)f->layer_bands,(unsigned)f->layer_reused,(unsigned)f->submit_sum,(unsigned)f->submit_max);
}
static void pipeline_capture_report(void)
{
    const nos_pipeline_capture *c=&s_pipeline_capture;
    printf("NOSPIPE enabled=%d capture_state=%d frames=%u painted=%u rawjobs=%u chunks=%u bytes=%u done=%u mismatch=%u api_sum_us=%llu api_max_us=%u dmawait_sum_us=%llu dmawait_max_us=%u rotation_sum_us=%llu rotation_max_us=%u queuewait_sum_us=%llu queuewait_max_us=%u done_gap_max_us=%u worst_pair_valid=%d worst_interval_us=%llu api_includes_prior_dma=1 optical_fence=0 queuewait_is_worker_idle=1\n",
        s_pipeline_enabled,s_capture.state,(unsigned)c->frames,(unsigned)c->painted,
        (unsigned)c->rawjobs,(unsigned)c->chunks,(unsigned)c->bytes,(unsigned)c->done,(unsigned)c->mismatch,
        (unsigned long long)c->api_sum,(unsigned)c->api_max,(unsigned long long)c->dmawait_sum,(unsigned)c->dmawait_max,
        (unsigned long long)c->rotation_sum,(unsigned)c->rotation_max,(unsigned long long)c->queuewait_sum,(unsigned)c->queuewait_max,
        (unsigned)c->done_gap_max,c->worst_valid,(unsigned long long)c->worst_interval);
    if(c->worst_valid) {
        pipeline_frame_report("previous",&c->worst_previous);
        pipeline_frame_report("current",&c->worst_current);
    }
}
/* Read-only static app poses; unsupported screens report app=0. */
static nos_scroll_snapshot scroll_snapshot(void)
{
    nos_scroll_snapshot s={0};const nos_screen *top=nos_top();
    if(top==&scr_activity)fit_activity_scroll_snapshot(&s);
    else if(top==&scr_heart)fit_heart_scroll_snapshot(&s);
    else if(top==&scr_weather)out_weather_scroll_snapshot(&s);
    else if(top==&scr_baro)out_baro_scroll_snapshot(&s);
    else if(top==&scr_nav)out_nav_scroll_snapshot(&s);
    else if(top==&scr_about)set_about_scroll_snapshot(&s);
    else if(top==&scr_launcher)sys_launcher_scroll_snapshot(&s);
    else if(top==&scr_settings)set_settings_scroll_snapshot(&s);
    else if(top==&scr_home && !nos_transitioning()) {
        int page,dragging;float offset;
        sys_home_nav_snapshot(&page,&offset,&dragging);
        nos_scroll_snapshot apps;sys_launcher_scroll_snapshot(&apps);
        /* HOME's drag flag also includes the embedded list's vertical drag.
         * The horizontal page must be exactly settled at its 466 px origin. */
        if(page==1 && offset==466.0f && (!dragging || apps.dragging))s=apps;
    }
    return s;
}
static nos_blur_pose blur_pose(nos_scroll_snapshot s)
{
    nos_blur_pose p={s.app,s.axis,(int32_t)lroundf(s.offset*256.0f)};return p;
}
static void list_profile_report(void)
{
    printf("NOSLISTPROFILE v=1 enabled=%d samples=%u dropped=%u capacity=%u bytes=%u fps_cap=%u\n",
        s_list_profile!=NULL,s_list_profile?(unsigned)s_list_profile->count:0,
        s_list_profile?(unsigned)s_list_profile->dropped:0,NOS_LIST_PROFILE_CAPACITY,
        s_list_profile?(unsigned)sizeof *s_list_profile:0,(unsigned)nos_prefs.fps_cap);
}
static void list_samples_report(unsigned offset,unsigned count)
{
    if(!s_list_profile || nos_frame_capture_live(&s_capture) || !count || count>8 ||
       offset>s_list_profile->count || count>s_list_profile->count-offset) {
        printf("ERR list_samples range_or_busy\n");return;
    }
    for(unsigned i=offset;i<offset+count;i++) {
        const nos_list_sample *s=&s_list_profile->samples[i];
        printf("NOSLISTFRAME index=%u frame=%u begin_us=%llu e2e_us=%u tick_us=%u record_us=%u scene_us=%u raster_us=%u wait_us=%u first_write_us=%u first_done_us=%u last_done_us=%u gap_us=%u rotation_us=%u api_us=%u pixels=%u chunks=%u rawjobs=%u scene_other_us=%u shape_us=%u sprite_us=%u offset_q8=%d app=%u dragging=%u\n",
            i,(unsigned)s->frame,(unsigned long long)s->begin_us,(unsigned)s->e2e_us,
            (unsigned)s->tick_us,(unsigned)s->record_us,(unsigned)s->scene_us,
            (unsigned)s->raster_us,(unsigned)s->wait_us,(unsigned)s->first_write_us,
            (unsigned)s->first_done_us,(unsigned)s->last_done_us,(unsigned)s->gap_us,
            (unsigned)s->rotation_us,(unsigned)s->api_us,(unsigned)s->pixels,
            (unsigned)s->chunks,(unsigned)s->rawjobs,(unsigned)s->scene_other_us,
            (unsigned)s->shape_us,(unsigned)s->sprite_us,(int)s->offset_q8,
            (unsigned)s->app,(unsigned)s->dragging);
    }
}
static nos_blur_counts blur_counts(void)
{
    nos_blur_counts b={nos_mblur_count[0][0],nos_mblur_count[0][1],
        nos_mblur_count[1][0],nos_mblur_count[1][1],nos_mblur_reduced[0],nos_mblur_reduced[1],
        pblur_hits[0],pblur_misses[0],pblur_hits[1],pblur_misses[1]};return b;
}
static void blur_capture_report(void)
{
    const nos_blur_capture *c=&s_blur_capture;
    printf("NOSBLURCAP v=1 state=%d frames=%u painted=%u pose_samples=%u pose_changed=%u drag_frames=%u foreign_frames=%u first_app=%d last_app=%d axis=%d start_q8=%d end_q8=%d min_q8=%d max_q8=%d history_frames=%u blend_frames=%u history_pixels=%llu history_blended=%llu history_uniform_pixels=%llu h_admitted=%llu h_skipped=%llu v_admitted=%llu v_skipped=%llu h_reduced=%llu v_reduced=%llu pblur_h_hits=%llu pblur_h_misses=%llu pblur_v_hits=%llu pblur_v_misses=%llu h_filtered_pixels=%llu v_filtered_pixels=%llu h_filter_frames=%u v_filter_frames=%u allocation_failures=%u\n",
        s_capture.state,(unsigned)c->frames,(unsigned)c->painted,(unsigned)c->pose_samples,
        (unsigned)c->pose_changed,(unsigned)c->drag_frames,(unsigned)c->foreign_frames,
        c->first.app,c->last.app,c->first.axis,(int)c->first.offset_q8,(int)c->last.offset_q8,(int)c->min_q8,(int)c->max_q8,
        (unsigned)c->history_frames,(unsigned)c->blend_frames,
        (unsigned long long)c->history_pixels,(unsigned long long)c->history_blended,(unsigned long long)c->history_uniform_pixels,
        (unsigned long long)c->h_admitted,(unsigned long long)c->h_skipped,(unsigned long long)c->v_admitted,(unsigned long long)c->v_skipped,
        (unsigned long long)c->h_reduced,(unsigned long long)c->v_reduced,
        (unsigned long long)c->pblur_h_hits,(unsigned long long)c->pblur_h_misses,
        (unsigned long long)c->pblur_v_hits,(unsigned long long)c->pblur_v_misses,
        (unsigned long long)c->h_filtered_pixels,(unsigned long long)c->v_filtered_pixels,
        (unsigned)c->h_filter_frames,(unsigned)c->v_filter_frames,(unsigned)c->allocation_failures);
}
static void cache_capture_report(void)
{
    const nos_cache_capture *c=&s_cache_capture;
    printf("NOSCACHECAP v=1 state=%d frames=%u painted=%u requested_frames=%u sharp_frames=%u blurred_frames=%u stale_frames=%u requested_processed_area=%llu sharp_processed_area=%llu blurred_processed_area=%llu unknown_requests=%llu native_frames=%u native_blurred_frames=%u native_processed_area=%llu native_blurred_processed_area=%llu processed_not_unique=1 final_visible=0\n",
        s_capture.state,(unsigned)c->frames,(unsigned)c->painted,(unsigned)c->requested_frames,
        (unsigned)c->sharp_frames,(unsigned)c->blurred_frames,(unsigned)c->stale_frames,
        (unsigned long long)c->requested_processed_area,(unsigned long long)c->sharp_processed_area,
        (unsigned long long)c->blurred_processed_area,(unsigned long long)c->unknown_requests,
        (unsigned)c->native_frames,(unsigned)c->native_blurred_frames,
        (unsigned long long)c->native_processed_area,(unsigned long long)c->native_blurred_processed_area);
}
static int host_crash_command(const char *s)
{
    if(!strcmp(s,"crash")) {nos_crash_report();return 1;}
    if(!strcmp(s,"crash clear")) {
        int ok=nos_crash_clear();
        if(!ok)cmd_printf("ERR crash clear failed\n");
        else cmd_printf("NOSCRASH cleared=1\n");
        return ok;
    }
    if(strncmp(s,"crash read ",11))return -1;
    const char *p=s+11;char *end;errno=0;
    unsigned long offset=strtoul(p,&end,10);
    if(errno || p==end || *p<'0' || *p>'9' || *end!=' ' || offset>UINT32_MAX) {
        cmd_printf("ERR crash read range\n");return 0;
    }
    p=end+1;errno=0;unsigned long count=strtoul(p,&end,10);
    if(errno || p==end || *p<'0' || *p>'9' || *end || !count || count>256 || offset>UINT32_MAX-count) {
        cmd_printf("ERR crash read range\n");return 0;
    }
    char hex[513];int n=nos_crash_read((uint32_t)offset,hex,(uint32_t)count);
    if(n<=0 || n>(int)count) {cmd_printf("ERR crash read %d\n",n);return 0;}
    static const char digits[]="0123456789abcdef";
    /* Expand backwards so the bounded read buffer also owns its hex text.
     * One stdio call keeps SDK logs outside this machine-readable line. */
    for(int i=n-1;i>=0;i--) {
        unsigned byte=(unsigned char)hex[i];
        hex[2*i]=digits[byte>>4];hex[2*i+1]=digits[byte&15];
    }
    hex[2*n]=0;
    cmd_printf("NOSCRASHDATA offset=%lu bytes=%u hex=%s\n",offset,(unsigned)n,hex);return 1;
}
#define printf cmd_printf
static int cmd_exec(char *s)
{
    s_cmd_ok=1;
    /* Command arguments may contain network credentials. Hash the verb only. */
    char verb[32];size_t verb_n=strcspn(s," \t\r\n");
    if(verb_n>=sizeof(verb))verb_n=sizeof(verb)-1;
    memcpy(verb,s,verb_n);verb[verb_n]=0;
    diag_log_event(DIAG_COMMAND,(int32_t)diag_log_name_id(verb),s_pwr,0);
    if(!strcmp(s,"hostcaps")) {printf("NOSHOST v=1 req=1 max_command=191 typed_errors=1\n");return 1;}
    if(!strcmp(s,"runtime_state")) {runtime_state_report();return 1;}
    if(!strcmp(s,"touch_state")) {touch_state_report();return 1;}
    if(!strcmp(s,"board")) {board_print_info();return 1;}
    if(!strcmp(s,"te")) {te_report();return 1;}
    if(!strcmp(s,"jobs")) {
        for(int id=0;id<BG_COUNT;id++) {
            background_status job;
            if(!background_status_get(id,&job)) {printf("ERR background monitor unavailable\n");return 0;}
            int32_t age=(int32_t)(hal_millis()-job.last_ms);if(age<0)age=0;
            printf("JOB id=%d state=%s age_ms=%lu updates=%lu errors=%lu dropped=%lu valid=%u v=%.3f,%.3f,%.3f\n",id,
                background_state_name(job.state),(unsigned long)age,(unsigned long)job.updates,(unsigned long)job.errors,
                (unsigned long)job.dropped,job.valid,(double)job.v[0],(double)job.v[1],(double)job.v[2]);
        }
        return 1;
    }
    if(!strcmp(s,"diag log")) {diag_log_report();return 1;}
    if(!strcmp(s,"board retry")) {board_retry_devices();printf("BOARD_RETRY queued=1\n");return 1;}
    if(!strcmp(s,"ota cancel")) {ota_session_end();printf("OTA cancelled=1\n");return 1;}
    if(!strcmp(s,"power restart"))return power_restart_request();
    if(!strcmp(s,"power cancel")) {
        if(!shutdown_cancel())printf("ERR power storage restart required\n");
        else printf("POWER cancelled=1\n");
        return s_cmd_ok;
    }
    int crash=host_crash_command(s);if(crash>=0)return crash;
    if (nos_usb_diag_command(s)) return 1;
    int a, b, c, d, e;
    if(sscanf(s,"te %d",&a)==1 && (!strcmp(s,"te 0") || !strcmp(s,"te 1"))) {
        if(nos_frame_capture_live(&s_capture))printf("ERR te capture_active\n");
        else if(a && !hal_display_te_available())printf("ERR te unavailable\n");
        else {
            if(nos_prefs.display_te!=a) {nos_prefs.display_te=(uint8_t)a;prefs_save();nos_invalidate();}
            display_preferences();te_report();
        }
    } else if (!strcmp(s, "bench begin")) {
        if (nos_frame_capture_live(&s_capture)) printf("ERR bench already active\n");
        else if (s_trace) printf("ERR bench requires trace 0\n");
        else {
            nos_frame_capture_begin(&s_capture);
            nos_cost_capture_reset();
            nos_list_profile_reset(s_list_profile);
            nos_blur_capture_begin(&s_blur_capture);
            nos_cache_capture_begin(&s_cache_capture);ui_fblur_observe_enable(1);
            memset(s_band_capture_painted,0,sizeof s_band_capture_painted);
            if (s_pipeline_enabled) memset(&s_pipeline_capture,0,sizeof s_pipeline_capture);
            printf("NOSCAPTURE armed=1 bytes=%u\n", (unsigned)sizeof s_capture);
        }
    } else if (!strcmp(s, "bench end")) {
        nos_frame_capture_stop(&s_capture);ui_fblur_observe_enable(0);
        const nos_frame_perf *p = &s_capture.perf;
        printf("NOSCAPTURE state=%d frames=%u painted=%u intervals=%u e2e_max_us=%lld present_max_us=%lld deadline=%u floor=%u p50_ms=%u p95_ms=%u p99_ms=%u start_us=%lld completed_us=%lld\n",
               s_capture.state, (unsigned)p->frames, (unsigned)p->painted, (unsigned)p->intervals,
               (long long)p->e2e_max, (long long)p->present_max,
               (unsigned)p->deadline_misses, (unsigned)p->floor_misses,
               nos_frame_perf_percentile(p,50),nos_frame_perf_percentile(p,95),nos_frame_perf_percentile(p,99),
               (long long)s_capture.start, (long long)s_capture.completed);
        blur_capture_report();cache_capture_report();
        nos_cost_report();
        if (s_pipeline_enabled) pipeline_capture_report();
        list_profile_report();
        printf("NOSDAMAGE state=%d mode=%d painted=%u band_painted=%u fallback_painted=%u\n",
               s_capture.state,nos_damage_bands_get(),(unsigned)p->painted,
               (unsigned)s_band_capture_painted[0],(unsigned)s_band_capture_painted[1]);
    } else if (!strcmp(s,"list_profile")) {
        list_profile_report();
    } else if (!strcmp(s,"list_profile 0") || !strcmp(s,"list_profile 1")) {
        if(nos_frame_capture_live(&s_capture))printf("ERR list_profile capture_active\n");
        else {
            if(s[13]=='1' && !s_list_profile)
                s_list_profile=heap_caps_calloc(1,sizeof *s_list_profile,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(s[13]=='0') {free(s_list_profile);s_list_profile=NULL;}
            if(s[13]=='1' && !s_list_profile)printf("ERR list_profile no_memory\n");
            else list_profile_report();
        }
    } else if (sscanf(s,"list_samples %d %d",&a,&b)==2) {
        list_samples_report((unsigned)a,(unsigned)b);
    } else if (!strcmp(s,"fps_cap")) {
        printf("NOSFPSCAP fps=%u period_us=%u\n",(unsigned)nos_prefs.fps_cap,
               (unsigned)nos_frame_cap_period(nos_prefs.fps_cap));
    } else if (sscanf(s,"fps_cap %d",&a)==1) {
        if(a<0 || !nos_frame_cap_valid((unsigned)a))printf("ERR fps_cap values=24|30|40|48|60|0\n");
        else if(nos_frame_capture_live(&s_capture))printf("ERR fps_cap capture_active\n");
        else {
            if(nos_prefs.fps_cap!=a) {nos_prefs.fps_cap=(uint8_t)a;prefs_save();nos_invalidate();}
            printf("NOSFPSCAP fps=%u period_us=%u\n",(unsigned)nos_prefs.fps_cap,
                   (unsigned)nos_frame_cap_period(nos_prefs.fps_cap));
        }
    } else if (!strcmp(s,"cost_calibrate")) {
        if(nos_frame_capture_live(&s_capture))printf("ERR cost_calibrate capture_active\n");
        else nos_cost_calibrate();
    } else if (!strcmp(s,"rotation_bench")) {
        if(nos_frame_capture_live(&s_capture) || s_inj_active || s_touch_now || s_touch_any)printf("ERR rotation_bench busy\n");
        else nos_rotation_bench_run(s_stripe[0],s_rotated[0]);
    } else if (sscanf(s,"cost_probe %d",&a)==1) {
        if(a<0 || a>1)printf("ERR cost_probe values=0|1\n");
        else if(nos_frame_capture_live(&s_capture))printf("ERR cost_probe capture_active\n");
        else {nos_cost_set(a);printf("NOSDIAG cost_probe=%d\n",nos_cost_enabled());}
    } else if (sscanf(s,"frame_pacing %d",&a)==1) {
        if (a<0 || a>1) printf("ERR frame_pacing values=0|1\n");
        else if (nos_frame_capture_live(&s_capture)) printf("ERR frame_pacing capture_active\n");
        else {
            s_frame_pacing=a; nos_frame_pacer_reset(&s_pacer);
            printf("NOSPACING mode=%d advanced_frames=0 advanced_us=0\n",s_frame_pacing);
        }
    } else if (sscanf(s,"pipeline_probe %d",&a)==1) {
        if (a<0 || a>1) printf("ERR pipeline_probe values=0|1\n");
        else if (nos_frame_capture_live(&s_capture)) printf("ERR pipeline_probe capture_active\n");
        else { s_pipeline_enabled=a; printf("NOSDIAG pipeline_probe=%d\n",s_pipeline_enabled); }
    } else if (sscanf(s,"settings_shutter %d",&a)==1) {
        if (a<0 || a>1) printf("ERR settings_shutter values=0|1\n");
        else if (nos_frame_capture_live(&s_capture)) printf("ERR settings_shutter capture_active\n");
        else { set_settings_shutter_set(a); printf("NOSDIAG settings_shutter=%d\n",set_settings_shutter_get()); }
    } else if (!strncmp(s,"settings_carry",14)) {
        if(strcmp(s,"settings_carry 0") && strcmp(s,"settings_carry 1"))
            printf("ERR settings_carry values=0|1\n");
        else if(nos_frame_capture_live(&s_capture))printf("ERR settings_carry capture_active\n");
        else if(s_inj_active || s_touch_now || s_touch_any)printf("ERR settings_carry touch_active\n");
        else {set_settings_carry_set(s[15]-'0');set_settings_carry_report();}
    } else if (!strncmp(s,"frame_priority",14)) {
        int result=nos_frame_priority_command(&s_frame_priority,s,nos_frame_capture_live(&s_capture),
            s_inj_active || s_touch_now || s_touch_any,frame_priority_set);
        if(result==1)printf("ERR frame_priority values=0|3\n");
        else if(result==2)printf("ERR frame_priority capture_active\n");
        else if(result==3)printf("ERR frame_priority touch_active\n");
        else frame_priority_report();
    } else if (!strncmp(s,"history_virtual",15)) {
        if(strcmp(s,"history_virtual 0") && strcmp(s,"history_virtual 1"))
            printf("ERR history_virtual values=0|1\n");
        else if(nos_frame_capture_live(&s_capture))printf("ERR history_virtual capture_active\n");
        else if(s_inj_active || s_touch_now || s_touch_any)printf("ERR history_virtual touch_active\n");
        else {nos_history_virtual_set(s[16]-'0');history_virtual_report();}
    } else if (!strncmp(s,"history_native_copy",19)) {
        if (strcmp(s,"history_native_copy 0") && strcmp(s,"history_native_copy 1"))
            printf("ERR history_native_copy values=0|1\n");
        else if (nos_frame_capture_live(&s_capture)) printf("ERR history_native_copy capture_active\n");
        else if (s_inj_active || s_touch_now || s_touch_any) printf("ERR history_native_copy touch_active\n");
        else {
            /* The prior frame's history writer is fenced before UI commands.
             * Both paths retain the same sharp pixels; no history reset needed. */
            nos_history_native_copy=s[20]-'0';
            history_path_report();
        }
    } else if (!strncmp(s,"native_rom",10)) {
        if (strcmp(s,"native_rom 0") && strcmp(s,"native_rom 1"))
            printf("ERR native_rom values=0|1\n");
        else if (nos_frame_capture_live(&s_capture)) printf("ERR native_rom capture_active\n");
        else if (s_inj_active || s_touch_now || s_touch_any) printf("ERR native_rom touch_active\n");
        else {
            /* Commands run on the UI thread after the previous DMA fence.
             * Retire mutable views before changing their backing source. */
            extern void sys_launcher_cache_reset(void),set_settings_cache_release(void);
            set_settings_cache_release();sys_launcher_cache_reset();
            /* A failed pack load (card inserted later) is retried here; bounded to 1.5 s. */
            if(s[11]=='1')native_menu_assets_reload();
            nos_native_menu_rom_set(s[11]-'0');
            nos_layers_invalidate();nos_history_reset();nos_invalidate();
            native_menu_report();
        }
    } else if (sscanf(s,"damage_bands %d",&a)==1) {
        if (a<0 || a>1) printf("ERR damage_bands values=0|1\n");
        else if (nos_frame_capture_live(&s_capture)) printf("ERR damage_bands capture_active\n");
        else { nos_damage_bands_set(a); printf("NOSDIAG damage_bands=%d\n",nos_damage_bands_get()); }
    } else if (sscanf(s,"rotation %d",&a)==1) {
        if (a<0 || a>2) printf("ERR rotation values=0|1|2\n");
        else if (nos_frame_capture_live(&s_capture)) printf("ERR rotation capture_active\n");
        else { board_pixels_rotation_mode_set(a); printf("NOSDIAG rotation=%d\n",board_pixels_rotation_mode_get()); }
    } else if (!strncmp(s,"swipe_cold",10)) {
        /* Diagnostic cold-cache gesture: no serial/prewarm gap between reset
         * and motion. Only disposable native pixels are released; navigation,
         * preferences, assets and logical scroll state are retained. The loop's
         * end-to-end timestamp starts before this dispatch and includes reset. */
        const nos_screen *owner=nos_top();
        if(sscanf(s,"swipe_cold %d %d %d %d %d",&a,&b,&c,&d,&e)!=5 ||
           a<0 || a>=GFX_W || c<0 || c>=GFX_W || b<0 || b>=GFX_H ||
           d<0 || d>=GFX_H || e<20 || e>10000)
            printf("ERR swipe_cold coordinates=0..465 duration=20..10000\n");
        else if(owner!=&scr_launcher && owner!=&scr_settings)
            printf("ERR swipe_cold owner=LAUNCHER|SETTINGS\n");
        else if(s_inj_active || s_touch_now || s_touch_any)
            printf("ERR swipe_cold touch_active\n");
        else {
            extern void sys_launcher_cache_reset(void),set_settings_cache_release(void);
            static uint32_t generation;
            int64_t reset_begin=esp_timer_get_time();
            if(owner==&scr_launcher)sys_launcher_cache_reset();
            else set_settings_cache_release();
            nos_layers_invalidate();nos_invalidate();
            uint32_t reset_us=(uint32_t)(esp_timer_get_time()-reset_begin);
            s_inj_x0=a;s_inj_y0=b;s_inj_x1=c;s_inj_y1=d;s_inj_ms=e;
            s_inj_t0=esp_timer_get_time();s_inj_active=1;
            printf("NOSCOLD owner=%s generation=%u reset_us=%u scheduled=1\n",
                   owner->name,(unsigned)++generation,(unsigned)reset_us);
        }
    } else if (sscanf(s, "tap %d %d", &a, &b) == 2) {
        s_inj_x0 = s_inj_x1 = a; s_inj_y0 = s_inj_y1 = b; s_inj_ms = 90;
        s_inj_t0 = esp_timer_get_time(); s_inj_active = 1;
    } else if (sscanf(s, "swipe %d %d %d %d %d", &a, &b, &c, &d, &e) >= 4) {
        s_inj_x0 = a; s_inj_y0 = b; s_inj_x1 = c; s_inj_y1 = d;
        s_inj_ms = (sscanf(s, "swipe %d %d %d %d %d", &a, &b, &c, &d, &e) == 5) ? e : 220;
        s_inj_t0 = esp_timer_get_time(); s_inj_active = 1;
    } else if (!strcmp(s, "btn")) { nos_button_pulse_start(&s_serial_btn, hal_millis()); nos_keep_awake(); }
    else if (sscanf(s, "face %d", &a) == 1) {          /* bench: set the face directly */
        if (a >= 0 && a < nos_face_count) { nos_prefs.face = (uint8_t)a; sys_home_build_in(); nos_invalidate(); }
        printf("FACE %d\n", nos_prefs.face);
    }
    else if (sscanf(s, "layers %d", &a) == 1) {
        nos_layer_cache_enabled = a > 0 ? 1 : 0;
        nos_layers_invalidate(); nos_invalidate();
        printf("NOSDIAG layers=%d history=%d history_uniform=%d\n", nos_layer_cache_enabled, nos_temporal_blur,nos_history_uniform);
    }
    else if (!strncmp(s, "cache_packed", 12)) {
        if (strcmp(s, "cache_packed 0") && strcmp(s, "cache_packed 1"))
            printf("ERR cache_packed values=0|1\n");
        else if (nos_frame_capture_live(&s_capture)) printf("ERR cache_packed capture_active\n");
        else {
            blur_cache_packed = s[13] - '0';
            printf("NOSCACHEMODE v=1 packed=%d\n", blur_cache_packed);
        }
    }
    else if (sscanf(s, "prog_geometry %d", &a) == 1) {
        if (a < 0 || a > 1) printf("ERR prog_geometry values=0|1\n");
        else if (nos_frame_capture_live(&s_capture)) printf("ERR prog_geometry capture_active\n");
        else {
            prog_geometry_enabled = a;
            if (!a) prog_geometry_release();
            nos_invalidate();
            printf("NOSPROG v=1 enabled=%d bytes=%u\n", prog_geometry_enabled,
                   prog_geometry_bytes());
        }
    }
    else if (sscanf(s, "history_cadence %d", &a) == 1) {
        if (a < 0 || a > 1) printf("ERR history_cadence values=0|1\n");
        else if (nos_frame_capture_live(&s_capture)) printf("ERR history_cadence capture_active\n");
        else {
            nos_history_cadence = a;
            printf("NOSHISTORY v=1 cadence=%d interval_us=%u previous_q5=%u denominator=32\n",
                   nos_history_cadence, (unsigned)nos_history_interval_us(),
                   nos_history_previous_weight_q5());
        }
    }
    else if (sscanf(s, "history_uniform %d", &a) == 1) {
        nos_history_uniform=a>0?1:0;
        printf("NOSDIAG history_uniform=%d\n",nos_history_uniform);
    }
    else if (sscanf(s, "history_packed %d", &a) == 1) {
        nos_history_packed = a > 0;
        printf("NOSDIAG history_packed=%d\n", nos_history_packed);
    }
    else if (!strncmp(s,"exposure_worker",15)) {
        if(strcmp(s,"exposure_worker 0") && strcmp(s,"exposure_worker 1"))
            cmd_printf("ERR exposure_worker values=0|1\n");
        else if(nos_frame_capture_live(&s_capture))cmd_printf("ERR exposure_worker capture_active\n");
        else if(s_inj_active || s_touch_now || s_touch_any)cmd_printf("ERR exposure_worker touch_active\n");
        else {
            s_exposure_worker=NOS_RENDER_SHARP?0:s[16]-'0';
            cmd_printf("NOSDIAG exposure_worker=%d\n",s_exposure_worker);
        }
    }
    else if (!strncmp(s,"native_stage",12)) {
        if(strcmp(s,"native_stage 0") && strcmp(s,"native_stage 1") &&
           strcmp(s,"native_stage 2") && strcmp(s,"native_stage 3"))
            cmd_printf("ERR native_stage values=0|1|2|3\n");
        else if(nos_frame_capture_live(&s_capture))cmd_printf("ERR native_stage capture_active\n");
        else if(s_inj_active || s_touch_now || s_touch_any)cmd_printf("ERR native_stage touch_active\n");
        else {
            nos_native_row_stage_mode=s[13]-'0';
            cmd_printf("NOSDIAG native_stage=%d\n",nos_native_row_stage_mode);
        }
    }
    else if (sscanf(s, "card_qz %d", &a) == 1) {
        extern int orrery_card_qz_exact;
        orrery_card_qz_exact = a > 0;
        printf("NOSDIAG card_qz=%d\n", orrery_card_qz_exact);
    }
    else if (!strcmp(s, "card_qz_check")) {
        extern unsigned orrery_card_qz_check(void);
        uint32_t start = hal_micros();
        unsigned mismatches = orrery_card_qz_check();
        printf("CARD_QZ_CHECK cases=65536 mismatches=%u elapsed_us=%u\n", mismatches,
               (unsigned)(hal_micros() - start));
    }
    else if (sscanf(s, "card_light %d", &a) == 1) {
        orrery_card_lighting_cache = a > 0;
        printf("NOSDIAG card_light=%d\n", orrery_card_lighting_cache);
    }
    else if (!strcmp(s, "card_light_check")) {
        uint32_t start = hal_micros();
        unsigned mismatches = orrery_card_lighting_check();
        printf("CARD_LIGHT_CHECK cases=512 mismatches=%u elapsed_us=%u\n", mismatches,
               (unsigned)(hal_micros() - start));
    }
    else if (sscanf(s, "card_shade %d", &a) == 1) {
        orrery_card_shade_q16 = a > 0;
        printf("NOSDIAG card_shade=%d\n", orrery_card_shade_q16);
    }
    else if (sscanf(s, "card_sun %d", &a) == 1) {
        orrery_card_sun_fast = a > 0;
        printf("NOSDIAG card_sun=%d\n", orrery_card_sun_fast);
    }
    else if (sscanf(s, "card_pipeline %d", &a) == 1) {
        orrery_card_progressive = a > 0;
        printf("NOSDIAG card_pipeline=%d\n", orrery_card_progressive);
    }
    else if (sscanf(s, "card_detail %d", &a) == 1) {
        if (a != 2 && a != 4) printf("ERR card_detail values=2|4\n");
        else {
            orrery_card_motion_step = (unsigned)a;
            printf("NOSDIAG card_detail=%u\n", orrery_card_motion_step);
        }
    }
    else if (sscanf(s, "card_proxy %d", &a) == 1) {
        orrery_card_motion_proxy = a > 0;
        printf("NOSDIAG card_proxy=%d\n", orrery_card_motion_proxy);
    }
    else if (sscanf(s, "card_budget %d", &a) == 1) {
        if (a < 250 || a > 8000) printf("ERR card_budget range=250..8000\n");
        else {
            orrery_card_render_budget_us = (unsigned)a;
            printf("NOSDIAG card_budget_us=%u\n", orrery_card_render_budget_us);
        }
    }
    else if (!strcmp(s, "card_work")) orrery_card_render_report();
    else if (!strcmp(s, "card_geometry_check")) {
        uint32_t start = hal_micros();
        unsigned mismatches = orrery_card_geometry_check();
        printf("CARD_GEOMETRY_CHECK available=%u mismatches=%u elapsed_us=%u\n",
               mismatches != ~0u, mismatches, (unsigned)(hal_micros() - start));
    }
    else if (!strcmp(s, "card_shade_check")) {
        uint32_t start = hal_micros();
        unsigned mismatches = orrery_card_shade_q16_check();
        printf("CARD_SHADE_CHECK coefficients=512 mismatches=%u elapsed_us=%u ready=%d rejection=%d\n",
               mismatches, (unsigned)(hal_micros() - start),
               orrery_shade_q16_ready(), orrery_shade_q16_rejection());
    }
    else if (sscanf(s, "card_profile %d", &a) == 1) {
        orrery_card_profile_set(a > 0, 1);
        printf("CARDPROFILE enabled=%d reset=1\n", orrery_card_profile_enabled());
    }
    else if (!strcmp(s, "card_profile report")) orrery_card_profile_report();
    else if (sscanf(s, "history %d", &a) == 1) {
        nos_temporal_blur = !NOS_RENDER_SHARP && a > 0;
        nos_invalidate();
        printf("NOSDIAG layers=%d history=%d history_uniform=%d\n", nos_layer_cache_enabled, nos_temporal_blur,nos_history_uniform);
    }
    else if (sscanf(s, "full %d", &a) == 1) nos_debug_force_full = a;   /* bench: every frame a full repaint */
    else if (!strcmp(s,"network wifi") || !strcmp(s,"network ble")) {
        extern void set_connectivity_open(int bt);
        go_state(PWR_ON);set_connectivity_open(!strcmp(s,"network ble"));
    }
    else if (!strcmp(s,"network status")) {
        char status[96],ble[96];hal_wifi_info(status,sizeof status);hal_ble_device_status(ble,sizeof ble);
        printf("NETWORK wifi=%s ble=%s\n",status,ble);
    }
    else if (!strcmp(s,"network setup")) {
        /* cmd_exec is reached only through the native USB command queue.
         * This temporary secret is already shown on the watch's setup screen. */
        char ssid[33],password[24];int active;
        hal_setup_info(ssid,sizeof ssid,password,sizeof password,&active);
        if(active) printf("SETUP active=1 ssid=%s password=%s\n",ssid,password);
        else printf("SETUP active=0\n");
        memset(password,0,sizeof password);
    }
    else if (!strcmp(s,"network scan")) {
        hal_wifi_ap aps[32];int n=hal_wifi_scan(aps,32);
        printf("NETWORK APS=%d\n",n);
        for(int i=0;i<n;i++) printf("AP %s rssi=%d ch=%d\n",aps[i].ssid,aps[i].rssi,aps[i].chan);
        hal_ble_dev devs[24];n=hal_ble_scan(devs,24);printf("NETWORK BLE=%d\n",n);
        for(int i=0;i<n;i++) printf("BLE %s %s rssi=%d\n",devs[i].name,devs[i].addr,devs[i].rssi);
    }
    else if (!strncmp(s, "open ", 5)) {
        const nos_screen *sc = screen_by_name(s + 5);
        if (sc) { go_state(PWR_ON); nos_open(sc, GFX_W / 2, GFX_H / 2); printf("OPEN %s\n", sc->name); }
        else printf("ERR no screen %s\n", s + 5);
    } else if (!strcmp(s, "home")) nos_home();
    else if (!strcmp(s, "perf")) { s_perf = !s_perf; printf("PERF %d\n", s_perf); }
    else if (sscanf(s, "predict %d", &a) == 1) { nos_touch_predict = a; printf("PREDICT %d\n", a); }   /* A/B drag prediction */
    else if (!strcmp(s, "orrery state")) { extern void orrery_report_state(void); orrery_report_state(); }
    else if (sscanf(s, "touch timeout %d", &a) == 1) {
        if (a < 0 || a > 8) printf("ERR touch timeout range 0..8\n");
        else { board_touch_inject_timeouts((unsigned)a); printf("TOUCH_TIMEOUT queued=%d\n", a); }
    }
    else if (sscanf(s, "orrery prof %d", &a) == 1) { extern void orrery_profile(int); orrery_profile(a); }
    else if (sscanf(s, "trace %d", &a) == 1) {
        if (a > 0) {nos_frame_capture_abort(&s_capture);ui_fblur_observe_enable(0);}
        s_trace = a; printf("TRACE %d\n", a);
    }
    else if (!strcmp(s, "awake")) { s_no_sleep = !s_no_sleep; printf("AWAKE %d\n", s_no_sleep); }
    else if (sscanf(s, "bri %d", &a) == 1) { nos_prefs.brightness = (uint8_t)a; prefs_save(); }
    else if (sscanf(s, "aod %d", &a) == 1) go_state(a ? PWR_AOD : PWR_ON);
    else if (!strncmp(s, "gb ", 3)) svc_phone_inject(s + 3);
    else if(!strncmp(s,"chat",4)) {if(voice_chat_command(s)<1)s_cmd_ok=0;}
    else if(!strncmp(s,"ext",3)) {if(!hal_ext_debug_command(s))s_cmd_ok=0;}
    else if (!strcmp(s, "wake")) {
        wake_info w; wake_service_get(&w);
        printf("WAKE on=%d wrist=%s pickup=%d turn=%d learned=%d valid=%d stage=%d dot=%.2f wakes=%u last=%d bias=(%.1f,%.1f,%.1f)\n",
               nos_prefs.raise_wake, w.wrist ? "right" : "left", w.pickup, w.turn, w.learned, w.valid, w.stage,
               w.view_dot, (unsigned)w.wakes, w.last_reason, w.bias[0], w.bias[1], w.bias[2]);
    } else if (sscanf(s, "wake trace %d", &a) == 1) s_wtrace_until = (uint32_t)(esp_timer_get_time() / 1000) + (uint32_t)a * 1000u;
    else if (!strcmp(s, "wake learn")) {
        if(!wake_learn_request())printf("ERR wake wrist pending\n");
        else printf("WAKE learning: hold your reading pose still\n");
    }
    else if (sscanf(s, "batcal %d", &a) == 1) { hal_battery_cal_set(a); printf("BAT cal %+d mV\n", hal_battery_cal_mv()); }
    else if (!strcmp(s, "bat")) {
        hal_power p; hal_power_get(&p);
        printf("BAT rail=%d mV cal=%+d percent=%d valid=%d external=%d crit=%d\n", p.rail_mv, hal_battery_cal_mv(),
               p.battery_percent, p.battery_valid, p.external_power, s_crit.level);
    }
    else if (!strcmp(s, "imu")) {
        hal_imu m; uint32_t ms; int v = hal_imu_sample(&m, &ms);
        printf("IMU v=%d a=(%.3f,%.3f,%.3f) g g=(%.1f,%.1f,%.1f) dps\n", v, m.ax, m.ay, m.az, m.gx, m.gy, m.gz);
    } else if (!strcmp(s, "off")) go_state(PWR_OFF);
    else if (sscanf(s, "knock log %d", &a) == 1) { extern void board_knock_log(int on); board_knock_log(a); printf("KNOCK log %d\n", a); }
    else if (sscanf(s, "walk log %d", &a) == 1) { extern void board_walk_log(int on); board_walk_log(a); printf("WALK log %d\n", a); }
    else if (!strcmp(s, "steps")) { hal_activity act; hal_activity_get(&act); printf("STEPS today=%u active_s=%u walking=%d valid=%d\n", (unsigned)act.steps, (unsigned)act.active_seconds, act.walking, act.valid); }
    else if (!strcmp(s, "knock")) printf("KNOCK count=%u locked=%d enabled=%d\n", (unsigned)hal_knock_count(), nos_locked, nos_knock_enabled());
    else if (!strcmp(s, "prefs") || !strcmp(s, "prefs flush")) {
        int ok=1;
        if (!strcmp(s,"prefs flush")) {
            prefs_save();ok=voice_chat_active()?0:prefs_flush();voice_chat_maintenance();
            if(voice_chat_history_pending())ok=0;
        }
        nos_prefs_t saved;
        int loaded=hal_store_load("nosprefs",&saved,sizeof saved);
        if(!strcmp(s,"prefs flush") && (!ok || !loaded || memcmp(&saved,&nos_prefs,sizeof saved))) s_cmd_ok=0;
        uint32_t hash=2166136261u,stored=2166136261u;
        const uint8_t *a=(const uint8_t *)&nos_prefs,*b=(const uint8_t *)&saved;
        for(unsigned i=0;i<sizeof saved;i++){hash=(hash^a[i])*16777619u;if(loaded)stored=(stored^b[i])*16777619u;}
        printf("NOSPREFS ok=%d stored=%d match=%d hash=%08x stored_hash=%08x face=%u brightness=%u dnd=%u wifi=%u ble=%u aod=%u sensor_idle=%u fps_cap=%u display_te=%u\n",
            ok,loaded,loaded && !memcmp(&saved,&nos_prefs,sizeof saved),(unsigned)hash,loaded?(unsigned)stored:0,
            nos_prefs.face,nos_prefs.brightness,nos_prefs.dnd,nos_prefs.wifi,nos_prefs.ble,nos_prefs.aod,nos_prefs.sensor_idle,nos_prefs.fps_cap,nos_prefs.display_te);
    }
    else if (!strcmp(s, "sports")) {
        sports_status st;sports_get_status(&st);
        printf("NOSSPORTS mode=%d queued=%u written=%u dropped=%u errors=%u available=%d cpu_max=%d raw=%d raw_src=%d gated=%u\n",
            sports_mode(),(unsigned)st.queued,(unsigned)st.written,(unsigned)st.dropped,(unsigned)st.errors,st.available,nos_power_get_stats().max_mhz,
            sports_raw_active(),sports_raw_sources(),(unsigned)st.gated);
    }
    else if (sscanf(s, "rawlog %d", &a) == 1) { /* explicit raw sensor journaling, session only */
        sports_raw_set(SPORTS_RAW_MANUAL, a != 0);
        printf("RAWLOG manual=%d active=%d sources=%d\n", a != 0, sports_raw_active(), sports_raw_sources());
    }
    else if (!strcmp(s, "mem")) {
        nos_mem_dump(0);
        unsigned used = (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) - heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        unsigned tracked = (unsigned)nos_mem_used();
        printf("MEM heap_used=%u sdk_and_overhead=%u\n", used, used > tracked ? used - tracked : 0);
        printf("MEM psram_free=%u largest=%u minimum=%u\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    }
    else if (!strcmp(s, "mblur_bench")) {
        if(nos_frame_capture_live(&s_capture))printf("ERR mblur_bench capture_active\n");
        else nos_mblur_bench_run();
    }
    else if (!strcmp(s, "scroll_state")) {
        nos_scroll_snapshot p=scroll_snapshot();
        printf("NOSSCROLL v=1 app=%d axis=%d page=%d pages=%d offset_q8=%d limit_q8=%d dragging=%d\n",
            p.app,p.axis,p.page,p.pages,(int)lroundf(p.offset*256.0f),(int)lroundf(p.limit*256.0f),p.dragging);
    }
    else if (!strcmp(s, "nav_state")) {
        int page, dragging;
        float offset;
        sys_home_nav_snapshot(&page, &offset, &dragging);
        printf("NOSNAV page=%d offset_q8=%d dragging=%d scroll_q8=%d settings_scroll_q8=%d\n",
               page, (int)lroundf(offset * 256.0f), dragging,
               (int)lroundf(sys_launcher_page_scroll() * 256.0f),
               (int)lroundf(set_settings_scroll_offset() * 256.0f));
    }
    else if (!strcmp(s, "state")) {
        panel_state_report();
        printf("NOSRENDER v=1 sharp=%d sprite_interpolation=%d history_storage_bytes=%u cost_probe=%d\n",
            NOS_RENDER_SHARP,!NOS_RENDER_SHARP,(unsigned)nos_history_storage_bytes(),nos_cost_enabled());
        /* Compact, read-only bench precondition. A single bounded line avoids
         * repeatedly bursting the full memory/build report through USB. */
        extern int orrery_card_qz_exact;
        printf("NOSSTATE pwr=%d awake=%d perf=%d trace=%d frame=%u top=%.20s face=%d locked=%d mblur=%d tx=%d layers=%d history=%d uniform=%d packed=%d worker=%d qz=%d profile=%d full=%d light=%d shade=%d shade_ready=%d sun=%d pipeline=%d card_budget_us=%u detail=%u proxy=%d pipeline_probe=%d settings_shutter=%d rotation=%d damage_bands=%d cache_packed=%d native_stage=%d\n",
               s_pwr, s_no_sleep, s_perf, s_trace, (unsigned)nos_frame,
               nos_top()?nos_top()->name:"-",nos_prefs.face,nos_locked,nos_mblur,nos_transitioning(),
               nos_layer_cache_enabled, nos_temporal_blur, nos_history_uniform,
               nos_history_packed, s_exposure_worker, orrery_card_qz_exact,
               orrery_card_profile_enabled(), nos_debug_force_full,
               orrery_card_lighting_cache, orrery_card_shade_q16, orrery_shade_q16_ready(), orrery_card_sun_fast,
               orrery_card_progressive, orrery_card_render_budget_us,
               orrery_card_motion_step, orrery_card_motion_proxy,
               s_pipeline_enabled,set_settings_shutter_get(),board_pixels_rotation_mode_get(),nos_damage_bands_get(),blur_cache_packed,nos_native_row_stage_mode);
        settings_shutter_report();
        printf("NOSPACING mode=%d advanced_frames=%u advanced_us=%llu\n",s_frame_pacing,
               (unsigned)s_pacer.advanced_frames,(unsigned long long)s_pacer.advanced_us);
        printf("NOSFPSCAP fps=%u period_us=%u\n",(unsigned)nos_prefs.fps_cap,
               (unsigned)nos_frame_cap_period(nos_prefs.fps_cap));
        list_profile_report();
        native_menu_report();
        if(!nos_frame_capture_live(&s_capture)){frame_priority_report();set_settings_carry_report();history_virtual_report();}
    }
    else if (!strcmp(s, "asset_state")) {
        asset_diagnostics asset;
        hal_asset_get_diagnostics(&asset);
        printf("NOSASSET state=%u bytes_owned=%u worker=%d stack_min_free=%u create_failures=%u admission_skips=%u\n",
               (unsigned)asset.state, (unsigned)asset.bytes_owned, asset.worker,
               (unsigned)asset.stack_min_free, (unsigned)asset.create_failures,
               (unsigned)asset.admission_skips);
    }
    else if (!strcmp(s, "card_state")) orrery_card_report_state();
    else if (!strcmp(s, "info")) {
        panel_state_report();
        te_report();
        hal_esp32_radio_report();
        nos_heap_diag_report();
        nos_mem_dump(1);
        printf("NOS %s board=%s internal_free=%u psram_free=%u depth=%d top=%s\n",
               esp_app_get_description()->version, WATCH_BOARD_NAME,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), nos_depth(),
               nos_top() ? nos_top()->name : "-");
        static const char *const bsn[NOS_BS_N] = { "list", "settings", "notifs", "live" };
        for (int k = 0; k < NOS_BS_N - 1; k++)
            printf("BLUR %-8s moving=%u fast=%u blurred=%u sharp=%u\n", bsn[k], (unsigned)nos_blur_stat[k][0],
                   (unsigned)nos_blur_stat[k][1], (unsigned)nos_blur_stat[k][2], (unsigned)nos_blur_stat[k][3]);
        printf("BLUR live gate first=%u afford=%u\n", (unsigned)nos_mblur_gate[0], (unsigned)nos_mblur_gate[1]);
        printf("MBLUR reduced h=%u v=%u ns/px=%.1f/%.1f\n",
               (unsigned)nos_mblur_reduced[0], (unsigned)nos_mblur_reduced[1], nos_mblur_ns(2), nos_mblur_ns(3));
        printf("PBLUR h=%u/%u v=%u/%u published=%u discarded=%u ready=%02x bytes=%u\n",
               pblur_hits[0], pblur_misses[0], pblur_hits[1], pblur_misses[1], pblur_published, pblur_discarded, pblur_ready_mask(), pblur_bytes());
        printf("PBLUR why switch=%u timeout=%u sig=%u busyskip=%u slices=%u\n",
               pblur_why[0], pblur_why[1], pblur_why[2], pblur_why[3], pblur_why[4]);
        printf("MBLUR h ran=%u skipped=%u  v ran=%u skipped=%u  ns/px h=%.0f v=%.0f\n",
               (unsigned)nos_mblur_count[0][1], (unsigned)nos_mblur_count[0][0],
               (unsigned)nos_mblur_count[1][1], (unsigned)nos_mblur_count[1][0], nos_mblur_ns(0), nos_mblur_ns(1));
        const esp_app_desc_t *app = esp_app_get_description();
        char elf_sha256[65];
        _Static_assert(sizeof app->app_elf_sha256==32,"full ELF SHA256 size");
        static const char digits[]="0123456789abcdef";
        for(unsigned i=0;i<sizeof app->app_elf_sha256;i++) {
            elf_sha256[2*i]=digits[app->app_elf_sha256[i]>>4];
            elf_sha256[2*i+1]=digits[app->app_elf_sha256[i]&15];
        }
        elf_sha256[64]=0;
        /* SDK vprintf shares stdout's lock per call, not across fragments. */
        printf("NOSBUILD source_sha256=%s source_build_id=%s metrics=nos-device-v1 cpu_mhz=%d psram_largest=%u elf_sha256=%s idf=%s dcache_bytes=%u dcache_line=%u stripe_rows=%u dma_rows=%u stripe_dma_bytes=%u pipeline_probe_supported=1 pipeline_probe_bytes=%u qspi_requested_hz=%u present_target_hz=%u animation_hz=%u\n",
               NOS_SOURCE_TREE_SHA256,NOS_SOURCE_BUILD_ID,
               nos_power_get_stats().cpu_mhz,
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),elf_sha256,app->idf_ver,
               (unsigned)CONFIG_ESP32S3_DATA_CACHE_SIZE,
               (unsigned)CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE,
               (unsigned)STRIPE_ROWS, (unsigned)DMA_ROWS,
               (unsigned)(2 * GFX_W * (STRIPE_ROWS + DMA_ROWS) * sizeof(uint16_t)),
               (unsigned)(sizeof s_pipeline + sizeof s_pipeline_capture),
               (unsigned)LCD_SPI_HZ, (unsigned)NOS_PRESENT_FPS, (unsigned)NOS_FPS);
        nos_power_report();
        board_touch_diag td = board_touch_diagnostics();
        printf("NOSUISTACK stack_bytes=%u min_free_bytes=%u\n",
               (unsigned)CONFIG_ESP_MAIN_TASK_STACK_SIZE,
               (unsigned)uxTaskGetStackHighWaterMark(NULL));
        printf("NOSCMDSTACK stack_bytes=6144 min_free_bytes=%u\n",
               s_command_handle?(unsigned)uxTaskGetStackHighWaterMark(s_command_handle):0u);
        printf("NOSTOUCH stack_bytes=%u min_free_bytes=%u reads=%u errors=%u injected=%u pending=%u last_error=%d\n",
               (unsigned)TOUCH_STACK_BYTES, s_touch_handle ? (unsigned)uxTaskGetStackHighWaterMark(s_touch_handle) : 0,
               (unsigned)td.reads, (unsigned)td.errors, (unsigned)td.injected, (unsigned)td.pending, td.last_error);
        extern int orrery_card_qz_exact;
        printf("NOSWORKER exposure_worker=%d history_packed=%d card_qz=%d job_bytes=%u flush_min_free_bytes=%u card_light=%d\n",
               s_exposure_worker, nos_history_packed, orrery_card_qz_exact, (unsigned)sizeof s_exposure_job,
               s_flush_handle ? (unsigned)uxTaskGetStackHighWaterMark(s_flush_handle) : 0, orrery_card_lighting_cache);
        printf("NOSDIAG layers=%d history=%d history_uniform=%d\n", nos_layer_cache_enabled, nos_temporal_blur,nos_history_uniform);
        printf("NOSHISTORY v=1 cadence=%d interval_us=%u previous_q5=%u denominator=32\n",
               nos_history_cadence, (unsigned)nos_history_interval_us(),
               nos_history_previous_weight_q5());
        printf("NOSPROG v=1 enabled=%d bytes=%u\n", prog_geometry_enabled,
               prog_geometry_bytes());
        printf("NOSCACHEMODE v=1 packed=%d\n", blur_cache_packed);
        printf("NOSDIAG pipeline_probe=%d settings_shutter=%d rotation=%d damage_bands=%d\n",
               s_pipeline_enabled,set_settings_shutter_get(),board_pixels_rotation_mode_get(),nos_damage_bands_get());
        settings_shutter_report();
        native_menu_report();
        if(!nos_frame_capture_live(&s_capture)){frame_priority_report();set_settings_carry_report();history_virtual_report();}
        ui_blur_stats blur; ui_fblur_stats(&blur);
        printf("NOSCACHE cache_bytes=%u allocfail=%u blur_bytes=%u blur_hits=%u blur_misses=%u blur_evictions=%u blur_work_pixels=%u\n",
               (unsigned)nos_last.cache_bytes, (unsigned)nos_last.allocation_failures,
               blur.bytes, blur.hits, blur.misses, blur.evictions, blur.work_pixels);
    } else if (!strcmp(s, "sd") || !strcmp(s, "sd mount")) {
        if(s[2])hal_storage_retry(); /* request accepted; board worker reports actual mount later */
        hal_storage st; hal_storage_get(&st);
        hal_flash_info fl; hal_flash_info_get(&fl);
        printf("SD mounted=%d total=%llu free=%llu status=\"%s\"\n", st.mounted,
               (unsigned long long)st.total_bytes, (unsigned long long)st.free_bytes, st.message);
        printf("FLASH size=%u app=%u slot=%u nvs=%u/%u\n", (unsigned)fl.flash_bytes, (unsigned)fl.app_bytes,
               (unsigned)fl.app_slot_bytes, (unsigned)fl.nvs_used, (unsigned)fl.nvs_total);
    } else if (!strcmp(s, "help"))
        printf("tap X Y | swipe X0 Y0 X1 Y1 [MS] | btn | open NAME | home | perf | bench begin|end | pipeline_probe 0|1 | frame_priority 0|3 | settings_carry 0|1 | native_rom 0|1 | settings_shutter 0|1 | damage_bands 0|1 | rotation 0|1|2 | rotation_bench | cost_probe 0|1 | cost_calibrate | trace N | predict 0|1 | layers 0|1 | history 0|1 | history_uniform 0|1 | history_native_copy 0|1 | history_virtual 0|1 | history_packed 0|1 | exposure_worker 0|1 | native_stage 0|1|2|3 | card_qz 0|1 | card_qz_check | card_light 0|1 | card_light_check | card_shade 0|1 | card_shade_check | card_sun 0|1 | card_pipeline 0|1 | card_detail 2|4 | card_proxy 0|1 | card_budget 250..8000 | card_work | card_geometry_check | card_profile 0|1|report | asset_state | card_state | orrery state | touch timeout 0..8 | awake | bri PCT | aod 0|1 | off | gb LINE | ext ... | state | nav_state | scroll_state | mblur_bench | usb_diag NONCE | usb_guard 0|1 NONCE | info | mem | diag log | sd [mount] | wake | wake trace S | wake learn | imu\n");
    else printf("ERR unknown: %s\n", s);
    return s_cmd_ok;
}
#undef printf
static void host_ui_execute(const cmd_line *line)
{
    nos_usb_diag_dispatched();nos_usb_diag_reply_begin();
    if(line->request_id[0])printf("BEGIN %s\n",line->request_id);
    int ok=cmd_exec((char *)line->text);
    /* DONE confirms this dispatch. Semantic navigation logs report arrival. The
     * checked raw USBDIAG legacy frame keeps its original terminal marker. */
    if(line->request_id[0] || (strncmp(line->text,"usb_diag",8) && strncmp(line->text,"usb_guard",9)))
        host_done(line->request_id,ok,"command");
    nos_usb_diag_reply_end();
}
static int host_command_readonly(const char *text)
{
    static const char *const allowed[]={"hostcaps","runtime_state","diag log","state","nav_state","scroll_state",
        "touch_state","board","jobs","te",
        "info","mem","prefs","sports","imu","wake","steps","knock","bat","asset_state",
        "card_state","orrery state","mblur_bench","crash","ext","ext help","ota cancel","power restart"};
    for(unsigned i=0;i<sizeof allowed/sizeof *allowed;i++)if(!strcmp(text,allowed[i]))return 1;
    return !strncmp(text,"crash read ",11) || !strncmp(text,"usb_diag ",9);
}
static void host_ui_dispatch(const cmd_line *line,int mutate)
{
    if(mutate || host_command_readonly(line->text)) {host_ui_execute(line);return;}
    nos_usb_diag_dispatched();nos_usb_diag_reply_begin();
    if(line->request_id[0])printf("BEGIN %s\n",line->request_id);
    printf("ERR update activity busy\n");host_done(line->request_id,0,"busy");
    nos_usb_diag_reply_end();
}

static int inj_poll(int *x, int *y)
{
    int el = (int)((esp_timer_get_time() - s_inj_t0) / 1000);
    int t = el < s_inj_ms ? el : s_inj_ms;
    *x = s_inj_x0 + (s_inj_x1 - s_inj_x0) * t / (s_inj_ms ? s_inj_ms : 1);
    *y = s_inj_y0 + (s_inj_y1 - s_inj_y0) * t / (s_inj_ms ? s_inj_ms : 1);
    if (el >= s_inj_ms + 40) s_inj_active = 0;
    return el < s_inj_ms;
}

/* ------------------------------------------------------------------ main */
static void pace_cb(void *arg) { (void)arg; xSemaphoreGive(s_frame_tick); }
static int64_t pace_now_us(void *context) {(void)context;return esp_timer_get_time();}
static int pace_arm(void *context,uint64_t delay) {(void)context;return esp_timer_start_once(s_frame_timer,delay);}
static void pace_stop(void *context) {(void)context;esp_timer_stop(s_frame_timer);}
static int pace_take(void *context,uint32_t timeout_ms)
{ (void)context;return xSemaphoreTake(s_frame_tick,pdMS_TO_TICKS(timeout_ms))==pdTRUE; }
static void pace_fault(void *context,int64_t requested,int error,int phase)
{
    (void)context;
    static int64_t last;static int last_error,last_phase;
    int64_t now=esp_timer_get_time();
    if(!last || now-last>=1000000 || error!=last_error || phase!=last_phase) {
        int32_t delay=requested>INT32_MAX?INT32_MAX:(int32_t)requested;
        diag_log_event(DIAG_FRAME_PACE,delay,error,phase);
        last=now;last_error=error;last_phase=phase;
    }
}
static const nos_frame_wait_ops s_pace_wait={NULL,pace_now_us,pace_arm,pace_stop,pace_take,pace_fault};
/* Reserve mandatory internal stacks, USB buffers and pacing objects before
 * optional radio allocation. A reserved worker cannot touch unready devices. */
typedef struct { TaskFunction_t entry; } boot_worker;
static boot_worker s_boot_flush={flush_task},s_boot_touch={touch_task},s_boot_command={cmd_task};
static void boot_worker_task(void *arg)
{
    boot_worker *worker=arg;
    (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    worker->entry(NULL);
    vTaskDelete(NULL);
}
static void boot_heap(const char *stage)
{
    ESP_LOGI(TAG,"BOOT stage=%s internal_free=%u internal_largest=%u dma_free=%u",
             stage,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
}
static void boot_resources_init(void)
{
    s_buf_free=xSemaphoreCreateCounting(2,2);
    s_dma_ready=xSemaphoreCreateCounting(2,2);
    s_flush_q=xQueueCreate(3,sizeof(flush_job));
    s_frame_done=xSemaphoreCreateBinary();
    s_command_done=xSemaphoreCreateBinary();s_command_lock=xSemaphoreCreateMutex();
    s_cmd_q=xQueueCreate(4,sizeof(cmd_line));
    s_host_reply_lock=xSemaphoreCreateMutex();
    s_frame_tick=xSemaphoreCreateBinary();
    assert(s_buf_free && s_dma_ready && s_flush_q && s_frame_done && s_command_done &&
           s_command_lock && s_cmd_q && s_host_reply_lock && s_frame_tick);
    usb_serial_jtag_driver_config_t config=USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    config.rx_buffer_size=4096;config.tx_buffer_size=1024;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    const esp_timer_create_args_t timer={.callback=pace_cb,.name="pace"};
    ESP_ERROR_CHECK(esp_timer_create(&timer,&s_frame_timer));
    BaseType_t ok=xTaskCreatePinnedToCore(boot_worker_task,"flush",3072,&s_boot_flush,4,&s_flush_handle,1);
    assert(ok==pdPASS);
    s_touch_task_ok=xTaskCreatePinnedToCore(boot_worker_task,"touch",TOUCH_STACK_BYTES,&s_boot_touch,5,&s_touch_handle,1)==pdPASS;
    assert(s_touch_task_ok);
    ok=xTaskCreate(boot_worker_task,"cmd",6144,&s_boot_command,4,&s_command_handle);
    assert(ok==pdPASS);
    boot_heap("essential-ready");
    /* The LCD and display IPC are ready. UI panel setters may now await the
     * real flush worker during later initialization. */
    xTaskNotifyGive(s_flush_handle);
}
static int s_btn_swallow;
void app_main(void)
{
    nos_frame_priority_init(&s_frame_priority,(unsigned)uxTaskPriorityGet(NULL));
    nos_heap_diag_init();
    prefs_set_persistence_allowed(0);
    board_set_background_persistence(0);
    /* DMA stripe buffers first: the radios eat most of internal RAM */
    for (int i = 0; i < 2; i++) {
        s_stripe[i] = heap_caps_malloc((size_t)GFX_W * STRIPE_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_rotated[i] = heap_caps_malloc((size_t)GFX_W * DMA_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(s_stripe[i] && s_rotated[i]);
    }
    uint16_t *scratch = hal_big_alloc((size_t)GFX_W * STRIPE_ROWS * 2);

    nos_power_init(); /* boot + first paint at fixed maximum; released before pacing */
    hal_esp32_init_periph();
    lcd_init();
    boot_resources_init();
    board_init();
    diag_log_init();
    diag_log_event(DIAG_POWER_STATE,PWR_ON,PWR_ON,0); /* initial observed display policy */
    imu_backend_wake(); /* recover a retained recorder before UI can start another */
    settings_load();
    board_restore_clock();
    prefs_load();
    display_preferences();
    g_tz_offset_min = nos_prefs.tz_min;
    board_apply_timezone();
    board_calendar_ready(!prefs_loading());
    hal_ext_init();
    voice_chat_init();
    radio_preferences();
    boot_heap("before-radios");
    hal_esp32_start_radios();
    boot_heap("after-radios");

    gpio_config_t btn = { .pin_bit_mask = 1ULL << 0, .mode = GPIO_MODE_INPUT, .pull_up_en = 1,
                          .intr_type = GPIO_INTR_ANYEDGE };
    gpio_config(&btn);
    esp_err_t ie = gpio_install_isr_service(0);
    if (ie == ESP_OK || ie == ESP_ERR_INVALID_STATE) gpio_isr_handler_add(GPIO_NUM_0, btn_isr, NULL);
#if !CONFIG_WATCH_BOARD_143C
    if(nos_power_get_stats().light_sleep && esp_sleep_enable_gpio_wakeup()==ESP_OK)
        nos_power_set_sleep_hooks(btn_sleep_before,btn_sleep_after);
#endif

    usb_serial_jtag_vfs_use_driver();

    nos_set_stripes(s_stripe[0], s_stripe[1], STRIPE_ROWS, stripe_wait, stripe_flush);
    nos_set_stripe_exposure(stripe_exposure_prepare, stripe_exposure_finish);
    nos_set_scratch(scratch);
    nos_init(NULL);
    nos_apps_boot();
    /* bake every dot mask on core 1 while the boot animation plays (first-use bakes
     * cost 50-470 ms frames in wakes and builds) */
    if (dot_prebake_begin()) xTaskCreatePinnedToCore(dotbake_task, "dotbake", 3072, NULL, 1, NULL, 1);
    wake_service_init(nos_prefs.raise_wake);
    (void)wake_wrist_sync(1);

    /* Board I2C, the OS and command/SD owners now exist. No input or host
     * command is dispatched before this point. */
    xTaskNotifyGive(s_touch_handle);xTaskNotifyGive(s_command_handle);
    nos_usb_diag_start(); /* Count only post-initialization calls. */
    ESP_LOGI(TAG, "NOS up. internal free %u, psram free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("READY,{\"os\":\"NOS\",\"fw\":\"%s\",\"board\":\"%s\"}\n", esp_app_get_description()->version, WATCH_BOARD_NAME);
    printf("NOSIDENTITY source_build_id=%s source_sha256=%s\n",NOS_SOURCE_BUILD_ID,NOS_SOURCE_TREE_SHA256);

    int64_t period = (1000000 + NOS_PRESENT_FPS - 1) / NOS_PRESENT_FPS;
    unsigned cap_seen=UINT32_MAX;
    int64_t next = esp_timer_get_time();
    int64_t last_run = next;
    uint32_t seen_presses = 0;
    int frames_ok = 0;
    static nos_frame_perf present;
    uint32_t target_slots = 0;
    /* per-second perf */
    int pf_frames = 0, pf_late = 0; int64_t pf_work = 0, pf_max = 0, pf_t0 = next; uint32_t pf_px = 0;
    /* cadence (R1): missed-slot time (hitch), the longest run of missed slots, and the
     * RMS deviation of frame-start intervals from the 33.3 ms grid */
    int64_t pf_hitch = 0, pf_prev = 0, pf_maxint = 0; int pf_run = 0, pf_maxrun = 0, pf_jn = 0; double pf_j2 = 0;
    for (;;) {
        nos_frame_priority_restore(&s_frame_priority,frame_priority_set);
        nos_power_active_begin(); /* includes input/wake/housekeeping and core1 DMA */
        int64_t now = esp_timer_get_time();
        uint32_t nowms = (uint32_t)(now / 1000);
        diag_log_tick();
        nos_heap_diag_tick();
        int activity=ota_activity_begin();int mutate=activity && s_shutdown_phase!=3;
        if(mutate) {
            if(!nos_transitioning() && !s_touch_now && !s_inj_active)voice_chat_maintenance();
            if(!nos_transitioning())native_menu_retry();
            hal_setup_poll();radio_preferences();calendar_preferences();display_preferences();(void)wake_wrist_sync(0);shutdown_tick(nowms);
            mutate=s_shutdown_phase!=3;
        }
        if(!mutate) {s_inj_active=0;if(s_touch_now || s_touch_any)s_swallow=1;}

        static cmd_line line;
        static int command_pending;
        if(!command_pending) command_pending=xQueueReceive(s_cmd_q,&line,0)==pdTRUE;
        if(command_pending && xSemaphoreTake(s_host_reply_lock,0)==pdTRUE) {
            /* read-only queries (wake, imu, off) must not wake the display: they observe it */
            int quiet = !strcmp(line.text,"diag log") || !strncmp(line.text,"prefs",5) || !strcmp(line.text,"sports") || !strcmp(line.text,"mem") || !strncmp(line.text,"history_virtual",15) || !strncmp(line.text,"settings_carry",14) || !strncmp(line.text,"frame_priority",14) || !strncmp(line.text, "wake", 4) || !strcmp(line.text, "imu") || !strcmp(line.text, "off") ||
                        !strncmp(line.text, "knock", 5) || !strncmp(line.text, "walk", 4) || !strcmp(line.text, "steps") || !strcmp(line.text, "info") || !strcmp(line.text, "state") || !strcmp(line.text, "nav_state") || !strcmp(line.text, "scroll_state") || !strcmp(line.text, "mblur_bench") || !strncmp(line.text, "usb_diag", 8) || !strncmp(line.text, "usb_guard", 9) || !strcmp(line.text, "asset_state") || !strcmp(line.text, "card_state") || !strcmp(line.text, "orrery state") || !strncmp(line.text, "touch timeout ", 14) || !strcmp(line.text, "card_qz_check") || !strcmp(line.text, "card_light_check") || !strcmp(line.text, "card_shade_check") || !strcmp(line.text, "card_work") || !strcmp(line.text, "card_geometry_check") || !strcmp(line.text, "bench end") || !strncmp(line.text, "card_profile ", 13);
            quiet|=!strcmp(line.text,"hostcaps") || !strcmp(line.text,"runtime_state") || !strncmp(line.text,"crash",5) || !strcmp(line.text,"board retry") || !strncmp(line.text,"exposure_worker",15) || !strncmp(line.text,"native_stage",12);
            quiet|=!strncmp(line.text,"fps_cap",7) || !strncmp(line.text,"list_profile",12) || !strncmp(line.text,"list_samples",12);
            quiet|=!strcmp(line.text,"touch_state") || !strcmp(line.text,"board") || !strcmp(line.text,"jobs");
            quiet|=!strcmp(line.text,"te");
            if(mutate && s_pwr!=PWR_ON && !quiet)go_state(PWR_ON);
            host_ui_dispatch(&line,mutate);
            xSemaphoreGive(s_host_reply_lock);command_pending=0;
            if(s_shutdown_phase==3)mutate=0;
        }

        /* input: touch samples queue up in the ring (touch_task) and reach the engine
         * right before nos_frame_run; here only "is a finger down" for wake and sleep */
        int x = 0, y = 0, down;
        int inj = s_inj_active;
        if (!s_touch_task_ok) {                      /* no task: one read per loop, as before */
            static int px, py, was, lx = -1, ly = -1; static uint32_t lms;
            touch_poll(&px, &py, &was, &lx, &ly, &lms);
        }
        if (inj) down = inj_poll(&x, &y);
        else down = s_touch_now || s_touch_any;
        portENTER_CRITICAL(&s_btn_mux);
        int bdown = s_btn_down; uint32_t presses = s_btn_presses;
        portEXIT_CRITICAL(&s_btn_mux);
        bdown |= nos_button_pulse_sample(&s_serial_btn, hal_millis());
        int btn_event = presses != seen_presses;
        seen_presses = presses;
        if(mutate && s_shutdown_mode && btn_event) {(void)shutdown_cancel();s_btn_swallow=1;}
        if(s_shutdown_phase==3)mutate=0;
        if(!mutate && btn_event && ota_busy()) {ota_session_end();s_btn_swallow=1;}

        hal_power policy_power;hal_power_get(&policy_power);
        int low_before=s_low_battery;
        if(mutate)s_low_battery=power_low_update(s_low_battery,&policy_power);
        if(low_before!=s_low_battery) {
            diag_log_event(DIAG_POWER_POLICY,s_low_battery,policy_power.battery_percent,policy_power.external_power);
            if(s_pwr==PWR_ON)nos_toast(s_low_battery?"LOW BATTERY / DISPLAY SAVER":"BATTERY SAVER OFF");
            printf("NOSLOW active=%d percent=%d valid=%d external=%d\n",s_low_battery,policy_power.battery_percent,policy_power.battery_valid,policy_power.external_power);
            if(s_low_battery && (s_pwr==PWR_AOD || s_pwr==PWR_SENSOR))go_state(PWR_OFF);
        }
        if(mutate && !s_shutdown_mode)critical_battery_tick(&policy_power,nowms);
        int raised=mutate?wake_service_tick(nowms,s_pwr!=PWR_ON && nos_prefs.raise_wake && !nos_locked && !s_low_battery):0;
        static int wake_stage=-1,learn_result;
        wake_info wi;wake_service_get(&wi);
        if(raised || wi.stage!=wake_stage || wi.learn_result!=learn_result) {
            float v[]={(float)raised,(float)wi.stage,(float)wi.last_reason,wi.view_dot,wi.speed,(float)wi.learn_result};
            sports_emit(SP_WAKE,v,6);wake_stage=wi.stage;learn_result=wi.learn_result;
        }
        /* Step detection and recording keep running with the display asleep.
         * Walking must neither wake the display nor restart its timeout. */
        /* knock to lock from AOD / off: wake first, the engine toggles the lock next frame */
        static uint32_t s_knock_host;
        uint32_t kn = hal_knock_count();
        int knocked=mutate && kn!=s_knock_host && nos_knock_enabled();s_knock_host=kn;
        static int s_lock_seen;
        if (nos_locked != s_lock_seen) { s_lock_seen = nos_locked; printf("LOCK %d\n", nos_locked); }
        if (s_wtrace_until && nowms < s_wtrace_until) {     /* serial "wake trace S" */
            wake_info w; hal_imu m; uint32_t ms;
            wake_service_get(&w);
            int v = hal_imu_sample(&m, &ms);
            printf("WT t=%u pwr=%d elig=%d v=%d a=(%.2f,%.2f,%.2f) g=(%.0f,%.0f,%.0f) stage=%d dot=%.2f spd=%.0f mag=%.2f%s\n",
                   (unsigned)nowms, s_pwr, s_pwr != PWR_ON && nos_prefs.raise_wake, v, m.ax, m.ay, m.az,
                   m.gx, m.gy, m.gz, w.stage, w.view_dot, w.speed, w.magnitude, raised ? " RAISED" : "");
        } else s_wtrace_until = 0;
        if(mutate && s_pwr!=PWR_ON && power_wake_requested(s_low_battery,down,btn_event,raised,knocked)) {
            diag_log_event(DIAG_WAKE,(down?1:0)|(btn_event?2:0)|(raised?4:0)|(knocked?8:0),s_pwr,s_low_battery);
            go_state(PWR_ON);
            if(s_crit.level)nos_toast("CRITICAL BATTERY");
            else if(s_low_battery)nos_toast("LOW BATTERY / DISPLAY SAVER");
            if (nos_locked) nos_lock_hint();          /* a glance while locked: how to unlock */
            if (down) s_swallow = 1;                 /* the waking touch never taps */
            if (btn_event || bdown) s_btn_swallow = 1; /* nor does the waking press */
        }
        if (inj) {                                   /* serial bench gestures replace the panel */
            touch_drain(0);
            if (s_swallow) { if (!down) s_swallow = 0; }
            else if(mutate && s_pwr==PWR_ON)nos_touch(x,y,down);
        }
        if (s_btn_swallow) { if (!bdown) s_btn_swallow = 0; }
        else if(mutate && s_pwr==PWR_ON)nos_button(bdown);

        /* sleep policy */
        if(mutate && s_pwr==PWR_ON && !s_no_sleep) {
            const nos_screen *top = nos_top();
            int keep = top && (top->flags & SCR_KEEPAWAKE);
            if(keep && !(top->flags & (SCR_NOBACK|SCR_SELFCAP)) && !voice_chat_active()) {
                /* G99: a KEEPAWAKE screen (compass, navigate, chat...) holds the display only while the
                 * user is around. Apps call nos_keep_awake() themselves, so release it here directly. */
                uint32_t since_on=hal_millis()-s_on_since_ms,idle=nos_user_idle_ms();
                if(idle>since_on)idle=since_on;
                hal_imu m={0};uint32_t ims;int vs=hal_imu_sample(&m,&ims);
                if(power_keep_expired(idle,power_wrist_down(vs,m.az))) {
                    diag_log_event(DIAG_SLEEP_BLOCK,64,(int32_t)idle,power_wrist_down(vs,m.az));
                    s_keep_released=top;
                    go_state(power_idle_target(s_low_battery,nos_prefs.sensor_idle,nos_prefs.aod));
                    if(activity)ota_activity_end(); /* owner 2 would otherwise leak: every later ota_activity_begin() fails and the UI freezes read-only */
                    continue;
                }
            }
            static int block_seen=-1;
            int block=(s_no_sleep?1:0)|(keep?2:0)|(down?4:0);
            if(block!=block_seen) {diag_log_event(DIAG_SLEEP_BLOCK,block,nos_awake_ms(),nos_prefs.sleep_s);block_seen=block;}
            if (power_timeout_due(s_no_sleep,keep,down,nos_awake_ms(),nos_prefs.sleep_s * 1000u))
                go_state(power_idle_target(s_low_battery,nos_prefs.sensor_idle,nos_prefs.aod));
        }

        /* Light sleep is allowed whenever the panel is off, or in AOD once its entry animation is done. */
        nos_power_display_hold(!(s_pwr==PWR_OFF || ((s_pwr==PWR_AOD || s_pwr==PWR_SENSOR) && !s_aod_lead && !sys_home_aod_entering())));
        /* Reclaim/adopt completed asset ownership even when no UI frame runs. */
        if(mutate) {orrery_card_asset_poll();theme_apply(nos_prefs.theme && s_pwr==PWR_ON);}
        /* screen off / AOD: loop on the shared 50 ms wake grid (motion_wake is dt-based and tolerates 100 ms gaps) */
        if (s_pwr == PWR_OFF) {
            cpu_mode(80);
            static uint32_t last_services;
            if(mutate && nowms-last_services>=1000) {last_services=nowms;nos_frame++;if(nos_bg_tick)nos_bg_tick(1.0f);}
            const nos_screen *pending=nos_top();
            if(mutate && pending && (pending->flags&SCR_KEEPAWAKE) && pending!=s_keep_released) {go_state(PWR_ON);if(activity)ota_activity_end();continue;}
            nos_frame_pacer_reset(&s_pacer);
            if(mutate) {prefs_set_persistence_allowed(!voice_chat_active());board_set_background_persistence(1);prefs_tick();}
            memset(&present, 0, sizeof present); pf_prev = 0; pf_t0 = now; pf_frames = 0;
            pf_late = 0; pf_work = pf_max = 0; pf_px = 0;
            pf_hitch = pf_maxint = 0; pf_maxrun = pf_jn = 0; pf_j2 = 0; target_slots = 0;
            touch_drain(0);if(activity)ota_activity_end();nos_power_active_end();
            sleep_grid_delay(50); next = esp_timer_get_time(); last_run = next; continue; }
        /* AOD entry: target-rate frames while the face dims out and the AOD face comes up
         * (sys_home's ramp), then the 1 Hz loop */
        if ((s_pwr == PWR_AOD || s_pwr == PWR_SENSOR) && !s_aod_lead && !sys_home_aod_entering()) {
            nos_frame_pacer_reset(&s_pacer);
            if(s_pwr==PWR_SENSOR && s_cpu_mhz!=80) {frame_drain();cpu_mode(80);sports_power(SP_SENSOR,s_cpu_mhz,aod_level());}
            static int64_t last_aod;
            if(mutate) {prefs_set_persistence_allowed(!voice_chat_active());board_set_background_persistence(1);prefs_tick();}
            memset(&present, 0, sizeof present); pf_prev = 0; pf_t0 = now; pf_frames = 0;
            pf_late = 0; pf_work = pf_max = 0; pf_px = 0;
            pf_hitch = pf_maxint = 0; pf_maxrun = pf_jn = 0; pf_j2 = 0; target_slots = 0;
            if (now - last_aod >= 1000000) {
                last_aod = now;
                if(mutate)nos_frame_run((float)(now-last_run)/1000.f);
                else nos_frame_run_readonly((float)(now-last_run)/1000.f);
                last_run = now;
                frame_drain();
                const nos_screen *pending=nos_top();
                if(mutate && pending && (pending->flags&SCR_KEEPAWAKE) && pending!=s_keep_released) {go_state(PWR_ON);if(activity)ota_activity_end();continue;}
                set_level(aod_level());
            }
            touch_drain(0);
            if(activity)ota_activity_end();
            nos_power_active_end(); /* optional AOD paint and DMA fence are complete */
            sleep_grid_delay(50);
            next = esp_timer_get_time();
            continue;
        }

        uint32_t cap_period=nos_frame_cap_period(nos_prefs.fps_cap);
        if(cap_seen!=nos_prefs.fps_cap) {
            cap_seen=nos_prefs.fps_cap;
            period=cap_period?cap_period:(1000000+NOS_PRESENT_FPS-1)/NOS_PRESENT_FPS;
            next=now;nos_frame_pacer_reset(&s_pacer);
            nos_frame_capture_abort(&s_capture);memset(&present,0,sizeof present);
            pf_frames=pf_late=0;pf_work=pf_max=0;pf_px=0;pf_t0=now;
            pf_hitch=pf_prev=pf_maxint=0;pf_run=pf_maxrun=pf_jn=0;pf_j2=0;target_slots=0;
        }
        prefs_set_persistence_allowed(0); board_set_background_persistence(0);
        if(mutate)auto_brightness_tick();
        /* radio_preferences is the single saved-intent owner in ON/AOD/OFF. */
        /* Only the ON input/render/DMA window participates; housekeeping stays baseline. */
        nos_frame_priority_begin(&s_frame_priority,s_pwr==PWR_ON,frame_priority_set);
        nos_frame_priority_observe(&s_frame_priority,s_pwr==PWR_ON,(unsigned)uxTaskPriorityGet(NULL));
        int64_t a = esp_timer_get_time();
        float el_ms = (float)(a - last_run) / 1000.0f;
        s_rot_us = 0; s_wait_us = 0; memset(nos_dbg_us, 0, sizeof nos_dbg_us);
        /* Previous iteration/AOD paint already completed its final DMA fence. */
        if (s_pipeline_enabled) nos_pipeline_begin(&s_pipeline,(uint64_t)now);
        if(!inj)touch_drain(mutate && s_pwr==PWR_ON);
        nos_blur_counts blur_before={0};
        if(nos_frame_capture_live(&s_capture))blur_before=blur_counts();
        uint32_t px=mutate?nos_frame_run(el_ms):nos_frame_run_readonly(el_ms);
        last_run = a;
        s_aod_lead = 0;
        int64_t ad = esp_timer_get_time();
        frame_drain();
        /* A preference must not wait indefinitely for display-off/AOD. Debounce
         * writes after input/transition motion, with the DMA queue drained. */
        if(mutate && !voice_chat_active() && !s_touch_now && !s_touch_any && !s_inj_active && !nos_transitioning() && !nos_frame_capture_live(&s_capture)) {
            prefs_set_persistence_allowed(1);
            prefs_tick();
            prefs_set_persistence_allowed(0);
        }
        int64_t completed = esp_timer_get_time();
        nos_frame_priority_restore(&s_frame_priority,frame_priority_set);
        if (s_pipeline_enabled) {
            nos_pipeline_frame f=nos_pipeline_finish(&s_pipeline,(uint32_t)nos_frame,
                (uint64_t)ad,(uint64_t)ad,(uint64_t)completed,
                nos_last.tick_us,nos_last.history_us,nos_last.raster_us,s_wait_us);
            f.record=nos_last.record_us;f.compare=nos_last.compare_us;
            f.layer_record=nos_last.layer_record_us;f.layer_raster=nos_last.layer_raster_us;
            f.layer_bands=nos_last.layer_bands;f.layer_reused=nos_last.layer_reused;
            f.cost=nos_cost_last();
            assert(f.chunks==f.done); /* callback writes precede DMA credit return */
            if (s_capture.state==NOS_CAPTURE_ACTIVE)
                nos_pipeline_capture_add(&s_pipeline_capture,&f,px!=0);
            if(s_list_profile && s_capture.state==NOS_CAPTURE_ACTIVE) {
                nos_scroll_snapshot pose=scroll_snapshot();
                nos_list_profile_add(s_list_profile,&f,pose.app,pose.dragging,
                    (int32_t)lroundf(pose.offset*256.0f));
            }
        }
        int64_t work = completed - a;
        if(s_capture.state==NOS_CAPTURE_ACTIVE)nos_cost_capture_add(px!=0);
        nos_frame_perf_add(&present, now, completed, next + period, px != 0);
        if (s_capture.state == NOS_CAPTURE_ACTIVE && px && nos_damage_bands_get())
            s_band_capture_painted[nos_last.damage_bands_used ? 0 : 1]++;
        if(nos_frame_capture_live(&s_capture)) {
            nos_scroll_snapshot pose=scroll_snapshot();
            if(s_capture.state==NOS_CAPTURE_ARMED)nos_blur_capture_prime(&s_blur_capture,blur_pose(pose));
            else {
                nos_blur_counts blur_after=blur_counts();
                nos_blur_capture_add(&s_blur_capture,&blur_before,&blur_after,blur_pose(pose),pose.dragging,px!=0,
                    nos_last.history_pixels,nos_last.history_blended,nos_last.history_uniform_pixels,nos_last.allocation_failures,
                    nos_last.mblur_h_pixels,nos_last.mblur_v_pixels);
                ui_fblur_observation observation;ui_fblur_observe_get(&observation);
                nos_cache_capture_add(&s_cache_capture,&observation,nos_frame,px!=0);
            }
        }
        nos_frame_capture_add(&s_capture, now, completed, next + period, px != 0);
        if (s_trace > 0 && px) {
            s_trace--;
            printf("NOSF el=%.1f px=%u nr=%u cmds=%u rec=%u ras=%u wait=%u rot=%u drain=%u work=%u top=%s tx=%d u=%u o=%u sel=%u clr=%u e2e=%u deadline_us=%lld begin_us=%lld completed_us=%lld frame_id=%u tick_us=%u history_us=%u history_pixels=%u history_blended=%u history_uniform_pixels=%u history_uniform_enabled=%d history_packed_enabled=%d exposure_worker=%d history_uniform_read_bytes=%u history_uniform_write_bytes=%u layer_record_us=%u layer_raster_us=%u layer_bands=%u layer_reused=%u cache_bytes=%u allocfail=%u\n",
                   el_ms, (unsigned)px, (unsigned)nos_last.rects, (unsigned)nos_last.cmds,
                   (unsigned)nos_last.record_us, (unsigned)nos_last.raster_us, (unsigned)s_wait_us,
                   (unsigned)s_rot_us, (unsigned)(esp_timer_get_time() - ad), (unsigned)work,
                   nos_top() ? nos_top()->name : "-", nos_transitioning(),
                   (unsigned)nos_dbg_us[0], (unsigned)nos_dbg_us[1], (unsigned)nos_dbg_us[2], (unsigned)nos_dbg_us[3],
                   (unsigned)(completed - now), (long long)(completed - (next + period)),
                   (long long)now, (long long)completed, (unsigned)nos_frame,
                   (unsigned)nos_last.tick_us, (unsigned)nos_last.history_us,
                   (unsigned)nos_last.history_pixels, (unsigned)nos_last.history_blended,
                   (unsigned)nos_last.history_uniform_pixels,nos_history_uniform,nos_history_packed,s_exposure_worker,
                   (unsigned)nos_last.history_uniform_read_bytes,(unsigned)nos_last.history_uniform_write_bytes,
                   (unsigned)nos_last.layer_record_us, (unsigned)nos_last.layer_raster_us,
                   (unsigned)nos_last.layer_bands, (unsigned)nos_last.layer_reused,
                   (unsigned)nos_last.cache_bytes, (unsigned)nos_last.allocation_failures);
        }
        /* brightness after the frame is on the panel: never show a stale frame */
        /* AOD entry: the user's level while the face dims out, the AOD level once the
         * AOD face is coming up (one dim step under the nearly black frame) */
        if (s_pwr == PWR_AOD || s_pwr==PWR_SENSOR) set_level(sys_home_aod_dimming() ? user_level() : aod_level());
        else set_level(nos_bright_override >= 0 ? nos_bright_override : user_level());
        if(mutate && ++frames_ok==90)ota_mark_valid();

        pf_frames++; pf_work += work; pf_px += px;
        if (work > pf_max) pf_max = work;
        if (work > period) pf_late++;
        if (pf_prev && a - pf_prev < 500000) {       /* not across an AOD / off stretch */
            int64_t iv = a - pf_prev, dv = iv - period;
            int slots = (int)((iv + period / 2) / period) - 1;   /* missed target-rate slots */
            if (slots > 0) { pf_hitch += dv; pf_run += slots; if (pf_run > pf_maxrun) pf_maxrun = pf_run; }
            else pf_run = 0;
            pf_j2 += (double)dv * (double)dv; pf_jn++;
            if (iv > pf_maxint) pf_maxint = iv;
        }
        pf_prev = a;
        if (s_perf && !nos_frame_capture_live(&s_capture) && a - pf_t0 >= 1000000) {
            double secs = (a - pf_t0) / 1e6;
            /* new fields go last: build/nos-bench/survey.py parses the line up to top= */
            printf("NOSPERF fps=%d avg_ms=%.1f max_ms=%.1f late=%d px=%u depth=%d top=%s hitch=%.1f missrun=%d jit=%.1f maxint=%.1f rate=%.2f painted=%u e2e_max=%.2f deadline=%u floor=%u present_max=%.2f present_p95=%u present_p99=%u target_slots=%u present_intervals=%u\n",
                   pf_frames, pf_work / 1000.0 / pf_frames, pf_max / 1000.0, pf_late,
                   (unsigned)(pf_px / (pf_frames ? pf_frames : 1)), nos_depth(), nos_top() ? nos_top()->name : "-",
                   pf_hitch / 1000.0 / secs, pf_maxrun, pf_jn ? sqrt(pf_j2 / pf_jn) / 1000.0 : 0.0, pf_maxint / 1000.0,
                   present.frames > 1 && present.last_begin > present.first_begin
                       ? (present.frames - 1) * 1e6 / (present.last_begin - present.first_begin) : 0.0,
                   (unsigned)present.painted,
                   present.e2e_max / 1000.0, (unsigned)present.deadline_misses, (unsigned)present.floor_misses,
                   present.present_max / 1000.0, nos_frame_perf_percentile(&present, 95), nos_frame_perf_percentile(&present, 99), (unsigned)target_slots,
                   (unsigned)present.intervals);
            pf_frames = pf_late = 0; pf_work = pf_max = 0; pf_px = 0; pf_t0 = a;
            pf_hitch = pf_maxint = 0; pf_maxrun = pf_jn = 0; pf_j2 = 0;
            nos_frame_perf_reset(&present); target_slots = 0;
        }
        if (!s_perf && a - pf_t0 >= 1000000) {
            pf_frames = pf_late = 0; pf_work = pf_max = 0; pf_px = 0; pf_t0 = a;
            pf_hitch = pf_maxint = 0; pf_maxrun = pf_jn = 0; pf_j2 = 0;
            nos_frame_perf_reset(&present); target_slots = 0;
        }

        if(activity)ota_activity_end();
        nos_power_active_end(); /* frame_drain and post-paint work completed at 240MHz */
        /* Reserve recent measured paint cost before the next completion target.
         * DMA is already complete: metrics retain its real completion timestamp.
         * The timer still yields the CPU whenever the selected start is ahead. */
        int64_t grid = next + period;
        next = nos_frame_pacer_capped(&s_pacer,s_frame_pacing,px!=0,now,completed,
                                     grid,(uint32_t)period,cap_period);
        int64_t pace_now = esp_timer_get_time();
        int64_t left = next - pace_now;
        if (left > 0) {
            if(!nos_frame_wait_until(&s_pace_wait,next,(uint32_t)(period*2+2000))) {
                nos_frame_pacer_reset(&s_pacer);
                vTaskDelay(pdMS_TO_TICKS(1)); /* repeated timer errors cannot busy-spin */
                next=esp_timer_get_time();
            }
        } else if (left < 0) {
            /* Rebase immediately: waiting for another grid slot would turn a
             * 26 ms frame at target 40 Hz into a 50 ms interval. Count missed
             * target slots, but execute and paint the next frame in full. */
            /* Advancing a start deliberately is not a missed target slot. */
            if (pace_now > grid)
                target_slots += (uint32_t)((pace_now - grid + period - 1) / period);
            next = esp_timer_get_time();
        }
        if(!cap_period && !px)vTaskDelay(1); /* Unlimited idle must still yield. */
    }
}
