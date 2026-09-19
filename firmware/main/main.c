/* main.c — device entry point for the ESP32-S3-Touch-AMOLED-1.43.
 *
 * Panel: 466x466 round AMOLED, SH8601 *or* CO5300 controller depending on
 * production batch. Initialization is explicitly selected in pins.h;
 * all-FF probe readback does not establish the controller identity.
 * Init sequences and pin map come from Waveshare's official demo package.
 *
 * Rendering is banded: the OS draws horizontal stripes straight into two
 * small internal-SRAM buffers, and each stripe is byte-swapped, rotated
 * counterclockwise, and DMA'd while the next renders. Tall single-region
 * updates first compose into PSRAM, then present the completed image. */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "../../core/wos.h"
#include "../../core/apps/apps.h"
#include "../../core/settings.h"
#include "../../core/notif.h"
#include "../../core/wake_service.h"
#include "../../core/motion_wake.h"
#include "pins.h"
#include "board.h"
#include "board_protocol.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "display_perf.h"
#include "lcd_probe.h"
#include "display_bench.h"

#define TAG "main"

#define STRIPE_ROWS    16   /* Two render buffers + one rotated DMA buffer: 44 KiB. */
#define PANEL_GAP_X     (LCD_USE_SH8601 ? 0 : 6)
#define PANEL_INIT_NAME (LCD_USE_SH8601 ? "SH8601" : "CO5300")

void hal_esp32_init_periph(void);
void hal_esp32_start_radios(void);

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_stripe[2];             /* render targets, internal DMA RAM */
static uint16_t *s_rotated;
static SemaphoreHandle_t s_dma_ready;    /* rotated buffer reusable after DMA */
static SemaphoreHandle_t s_frame_tick;
static esp_timer_handle_t s_frame_timer;
static void frame_tick(void *arg)
{
    (void)arg;
    xSemaphoreGive(s_frame_tick);
}
static SemaphoreHandle_t s_buf_free;      /* counts free stripe buffers */
typedef struct { uint16_t *buf; int x0, y0, w, rows; int atomic; } flush_job;
static QueueHandle_t s_flush_q;           /* render core (0) -> flush core (1) */

/* Compose large redraws in PSRAM before a burst transfer. The B board has no
 * TE wire; this reduces progressive banding but cannot guarantee no tearing. */
static uint16_t *s_backbuf;               /* PSRAM full-frame, byte-swapped */
static volatile int s_atomic;             /* current frame accumulates into backbuf */
static int s_bx0, s_by0, s_bx1, s_by1;    /* the atomic band's bounds (may be narrow) */
/* Init tables from Waveshare Demo V3, 09_FactoryProgram. */
static const sh8601_lcd_init_cmd_t sh8601_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
};
static const sh8601_lcd_init_cmd_t co5300_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 80},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 1},
    {0x63, (uint8_t[]){0xFF}, 1, 1},
    {0x51, (uint8_t[]){0x00}, 1, 1},
    {0x29, (uint8_t[]){0x00}, 0, 10},
};
static uint8_t s_panel_id;
static volatile int s_panel_brightness=-1; /* Last successful DCS write; not optical readback. */
static SemaphoreHandle_t s_sleep_ready;
static void perf_command(const char *s);
static void perf_benchmark_worker(void);
static SemaphoreHandle_t s_bench_done;
static int s_bench_w, s_bench_h, s_bench_frames, s_bench_source;
static size_t s_bench_capacity = GFX_W*STRIPE_ROWS*2;
static int s_trace_restore_awake, s_trace_restore_face, s_trace_running;
static TaskHandle_t s_flush_handle;
static display_bench_result s_bench_result;

static bool on_stripe_done(esp_lcd_panel_io_handle_t io,
                           esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    display_perf_complete_isr(0, esp_timer_get_time());
    xSemaphoreGiveFromISR(s_dma_ready, &woken);
    return woken == pdTRUE;
}

/* Flush worker on core 1: pack, byte-swap and rotate each stripe. Release the
 * input immediately after copying into the separate DMA buffer; the next
 * stripe can render while this one transfers. Only the ISR releases DMA RAM. */
static void flush_task(void *arg)
{
    (void)arg;
    flush_job j;
    for (;;) {
        xQueueReceive(s_flush_q, &j, portMAX_DELAY);
        if (j.atomic == 4) { perf_benchmark_worker(); continue; }
        if (j.atomic == 3) {
            ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, false));
            ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, (0x02ULL << 24) | (0x10 << 8), NULL, 0));
            vTaskDelay(pdMS_TO_TICKS(120));
            gpio_set_level(PIN_LCD_EN, 0);
            xSemaphoreGive(s_sleep_ready);
            continue;
        }
        if (!j.buf) {
            uint8_t level = (uint8_t)j.x0;
            ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, (0x02ULL << 24) | (0x51 << 8), &level, 1));
            s_panel_brightness=level;
            continue;
        }
        int64_t stage_start = esp_timer_get_time();
        if (j.atomic == 1) {                    /* accumulate into the back-buffer, no DMA */
            uint16_t *b = j.buf;           /* copy just this band's [x0,x0+w) columns,
                                            * byte-swapped, into the back-buffer */
            for (int r = 0; r < j.rows; r++) {
                const uint16_t *src = &b[r * GFX_W + j.x0];
                uint16_t *dst = &s_backbuf[(size_t)(j.y0 + r) * GFX_W + j.x0];
                for (int i = 0; i < j.w; i++) {
                    uint16_t v = src[i];
                    dst[i] = (uint16_t)((v << 8) | (v >> 8));
                }
            }
            display_perf_add_stage(DISPLAY_PERF_COPY, esp_timer_get_time()-stage_start);
            xSemaphoreGive(s_buf_free);    /* stripe buffer reusable immediately */
            continue;
        }
        /* Byte-swap RGB565 to wire order and, for a sub-rectangle,
         * compact its [x0,x0+w) columns to the front of each buffer row so the
         * draw_bitmap gets a tightly-packed w*rows image. In-place is safe: the
         * write index r*w+i never exceeds the read index r*GFX_W+x0+i. */
        uint16_t *b = j.buf;
        if (j.atomic == 2) goto submit; /* already byte-swapped in backbuffer */
        if (j.x0 == 0 && j.w == GFX_W) {                 /* full width — fast 2px swap */
            uint32_t *d32 = (uint32_t *)b;
            int n32 = (GFX_W * j.rows) / 2;
            for (int i = 0; i < n32; i++) {
                uint32_t v = d32[i];
                d32[i] = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
            }
        } else {
            for (int r = 0; r < j.rows; r++) {
                uint16_t *dst = &b[r * j.w];
                const uint16_t *src = &b[r * GFX_W + j.x0];
                for (int i = 0; i < j.w; i++) {
                    uint16_t v = src[i];
                    dst[i] = (uint16_t)((v << 8) | (v >> 8));
                }
            }
        }
        display_perf_add_stage(DISPLAY_PERF_SWAP, esp_timer_get_time()-stage_start);
submit:
        /* Mounted panel is 90 degrees clockwise from the UI. The official
         * driver cannot swap axes, so rotate the packed stripe CCW in software.
         * Keep the native controller gap; only logical pixels/rectangles rotate. */
        stage_start = esp_timer_get_time();
        xSemaphoreTake(s_dma_ready, portMAX_DELAY);
        display_perf_add_stage(DISPLAY_PERF_DMA_WAIT, esp_timer_get_time()-stage_start);
        stage_start = esp_timer_get_time();
        board_pixels_ccw(b, s_rotated, j.w, j.rows);
        display_perf_add_stage(DISPLAY_PERF_ROTATE, esp_timer_get_time()-stage_start);
        xSemaphoreGive(s_buf_free); /* DMA reads s_rotated, never b from here on */
        display_perf_submit((uint32_t)j.w*j.rows*2, esp_timer_get_time());
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel,
            j.y0, GFX_W - j.x0 - j.w, j.y0 + j.rows, GFX_W - j.x0, s_rotated));
    }
}

/* AMOLED brightness via DCS 0x51. Once the flush task owns the SPI bus (core 1),
 * route the command through it — issuing it from core 0 concurrently with a
 * stripe DMA corrupts/deadlocks the shared esp_lcd IO. */
void display_set_brightness(uint8_t level)
{
    if (s_flush_q) {
        flush_job j = { NULL, level, 0, 0, 0, 0 };
        xQueueSend(s_flush_q, &j, portMAX_DELAY);
    } else if (s_io) {
        if(esp_lcd_panel_io_tx_param(s_io, (0x02ULL << 24) | (0x51 << 8), &level, 1)==ESP_OK)
            s_panel_brightness=level;
    }
}

static void lcd_init(void)
{
    gpio_config_t en = { .pin_bit_mask = 1ULL << PIN_LCD_EN, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&en));
    ESP_ERROR_CHECK(gpio_set_level(PIN_LCD_EN, 1));
    vTaskDelay(pdMS_TO_TICKS(20));
    s_panel_id = board_lcd_probe();
    ESP_LOGI(TAG, "PANEL id=0x%02x init=%s selection=board_profile identity=unconfirmed gap=%d",
             s_panel_id, PANEL_INIT_NAME, PANEL_GAP_X);
    const spi_bus_config_t bus = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_PCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
        GFX_W * STRIPE_ROWS * 2 + 64);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg =
        SH8601_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, on_stripe_done, NULL);
    io_cfg.pclk_hz = LCD_SPI_HZ;
    ESP_LOGI(TAG, "QSPI requested=%d calculated=%d Hz (80 MHz APB source; not scope measured)",
             LCD_SPI_HZ, spi_get_actual_clock(80000000, LCD_SPI_HZ, 128));
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                             &io_cfg, &s_io));

    sh8601_vendor_config_t vendor = {
        .init_cmds = LCD_USE_SH8601 ? sh8601_init : co5300_init,
        .init_cmds_size = LCD_USE_SH8601 ? sizeof sh8601_init / sizeof sh8601_init[0] : sizeof co5300_init / sizeof co5300_init[0],
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
}

/* FT3168 shares the bus with the RTC and QMI8658. Polling releases the
 * pointer on an I2C fault, so disconnecting touch cannot leave a stuck drag. */
static int touch_poll(void)
{
    static int x, y;
    int raw_x = 0, raw_y = 0;
    int down = board_touch_read(&raw_x, &raw_y);
    board_pointer_update(&x, &y, raw_x, raw_y, down);
    wos_pointer(x, y, down);
    return down;
}

/* The 1.43-B has neither an ES8311 nor an onboard speaker/microphone. */
void hal_beep(int freq, int ms) { (void)freq; (void)ms; }
void hal_play(int sound) { (void)sound; }
int hal_mic_level(void) { return 0; }
int hal_mic_pitch(void) { return 0; }
int hal_audio_available(void) { return 0; }

/* ---- TEST HARNESS: drive the watch over USB serial ------------------------
 * One command per line. Lets an operator (or an automated webcam loop) exercise
 * the whole UI with no finger on the glass:
 *   tap X Y | swipe X0 Y0 X1 Y1 | open N | home | close | bri N | info | help
 *   left | right | top | bottom   (open the edge overlay from that side)
 * Injected pointer events go through the SAME wos_pointer path as real touch;
 * while g_inj_active the main loop feeds these instead of the touch controller. */
static int g_inj_active, g_inj_x, g_inj_y, g_inj_down;
static int s_inj_x0, s_inj_y0, s_inj_x1, s_inj_y1, s_inj_ms;
static int64_t s_inj_start;
static int g_no_sleep = 0, g_pause = 0, g_aod = 0, g_prof = 0;
static int g_screen_state,g_screen_request=-1,g_aod_glance;
static uint32_t g_glance_ms;
typedef struct { char text[192]; } debug_line;
static QueueHandle_t s_cmd_q;
static int s_debug_done;

/* ---- power management ---- */
/* flash a full-screen toast for ~350ms so the user SEES the action before the screen goes dark */
static void power_show(const char *msg)
{
    wos_callout(msg, 1600);
    for (int i = 0; i < 11; i++) { wos_run_frame(); vTaskDelay(pdMS_TO_TICKS(30)); }
}
static void power_deep_sleep(void)
{
    power_show("Sleep");
    printf("PWR: deep sleep (press BOOT to wake)\n");
    fflush(stdout);
    hal_set_backlight(0);
    board_prepare_sleep();
    face_pip_service(1);
    flush_job sleep_job = { .atomic = 3 };
    xQueueSend(s_flush_q, &sleep_job, portMAX_DELAY);
    xSemaphoreTake(s_sleep_ready, portMAX_DELAY);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);  /* BOOT (GPIO0) active-low wakes us */
    esp_deep_sleep_start();                       /* powers down; wake = a fresh boot -> home */
}
static void power_off(void)
{
    power_show("Power Off");
    printf("PWR: power off (press BOOT to turn back on)\n");
    fflush(stdout);
    hal_set_backlight(0);
    board_prepare_sleep();
    face_pip_service(1);
    flush_job sleep_job = { .atomic = 3 };
    xQueueSend(s_flush_q, &sleep_job, portMAX_DELAY);
    xSemaphoreTake(s_sleep_ready, portMAX_DELAY);
    /* Deep sleep, not a physical battery disconnect. Board current needs measurement. */
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep_start();
}
static void power_set_aod(int on)
{
    g_aod = on;
    g_aod_glance=0;
    if (on) { wos_open_app(0); wos_callout("Always-On", 1300); hal_set_backlight(10); printf("PWR: AOD on\n"); }
    else    { wos_callout("AOD Off", 1000); hal_set_backlight((uint8_t)g_brightness); printf("PWR: AOD off\n"); }
}
/* HAL hook so the control-panel AOD button can enter always-on display. */
void hal_set_aod(int on) { power_set_aod(on); }

static void inj_swipe(int x0, int y0, int x1, int y1)
{
    s_inj_x0 = x0; s_inj_y0 = y0; s_inj_x1 = x1; s_inj_y1 = y1;
    s_inj_ms = (x0 == x1 && y0 == y1) ? 110 : 240;
    s_inj_start = esp_timer_get_time();
    g_inj_active = 1;
}
static void inj_tap(int x, int y) { inj_swipe(x, y, x, y); }
static void inj_poll(void)
{
    int elapsed = (int)((esp_timer_get_time() - s_inj_start) / 1000);
    int t = elapsed < s_inj_ms ? elapsed : s_inj_ms;
    g_inj_x = s_inj_x0 + (s_inj_x1 - s_inj_x0) * t / s_inj_ms;
    g_inj_y = s_inj_y0 + (s_inj_y1 - s_inj_y0) * t / s_inj_ms;
    g_inj_down = elapsed < s_inj_ms;
    wos_pointer(g_inj_x, g_inj_y, g_inj_down);
    if (elapsed >= s_inj_ms + 50) g_inj_active = 0;
}
/* All diagnostic display writes run on the existing sole LCD owner. */
static void perf_benchmark_worker(void)
{
    s_bench_result=display_bench_run(s_io,s_panel,s_rotated,s_bench_capacity,
        s_backbuf,s_bench_w,s_bench_h,PANEL_GAP_X,
        s_bench_frames,(display_bench_source)s_bench_source,s_dma_ready);
    xSemaphoreGive(s_bench_done);
}

static void perf_info(void)
{
    const esp_app_desc_t *app=esp_app_get_description();
    printf("PERF_INFO,{\"fw\":\"%s\",\"idf\":\"%s\",\"cpu_mhz\":%d,"
        "\"clock_requested_hz\":%d,\"clock_actual_hz\":%d,\"clock_basis\":\"ESP-IDF divider calculation with 80MHz APB; not scope measured\","
        "\"panel_identity\":\"unconfirmed\",\"legacy_id\":%u,\"legacy_gap\":%d,\"te_routed\":false,"
        "\"panel_init\":\"%s\",\"panel_selection\":\"board_profile\",\"panel_gap_x\":%d,"
        "\"width\":466,\"height\":466,\"stripe_rows\":%d,\"dma_buffer_bytes\":%d,"
        "\"internal_free\":%u,\"internal_largest\":%u,\"dma_largest\":%u,\"psram_free\":%u,"
        "\"main_stack_free_bytes\":%u,\"flush_stack_free_bytes\":%u,\"render_core\":0,\"flush_core\":1,"
        "\"queue_capacity\":2,\"lvgl\":false,\"tearing_observed\":null,\"current_ma\":null}\n",
        app->version,app->idf_ver,CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,LCD_SPI_HZ,
        spi_get_actual_clock(80000000, LCD_SPI_HZ, 128),s_panel_id,PANEL_GAP_X,PANEL_INIT_NAME,PANEL_GAP_X,
        STRIPE_ROWS,GFX_W*STRIPE_ROWS*2,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),(unsigned)uxTaskGetStackHighWaterMark(NULL),
        (unsigned)uxTaskGetStackHighWaterMark(s_flush_handle));
}

static void perf_run(int w,int h,int frames,int source,size_t capacity)
{
    if(s_trace_running) { printf("ERR finish app trace before synthetic run\n"); return; }
    if(w<2 || w>GFX_W || h<2 || h>GFX_H || (w&1) || (h&1) || frames<1 || frames>2000 ||
        capacity<(size_t)w*4 || capacity>GFX_W*STRIPE_ROWS*2) { printf("ERR invalid even geometry/count/capacity\n"); return; }
    /* Previous UI frame has drained. The worker will process earlier control jobs first. */
    if(source==DISPLAY_BENCH_SOURCE_PSRAM) {
        if(!s_backbuf) { printf("ERR no PSRAM buffer\n"); return; }
        for(size_t i=0;i<(size_t)w*h;i++) s_backbuf[i]=(uint16_t)(0x1008u ^ (i&0x1fu));
    }
    display_perf_reset();
    s_bench_w=w;s_bench_h=h;s_bench_frames=frames;s_bench_source=source;s_bench_capacity=capacity;
    flush_job job={.atomic=4};
    xQueueSend(s_flush_q,&job,portMAX_DELAY);
    xSemaphoreTake(s_bench_done,portMAX_DELAY);
    /* A timed-out transfer retains DMA ownership; do not resume drawing into it. */
    if(s_bench_result.err==ESP_ERR_TIMEOUT) ESP_ERROR_CHECK(s_bench_result.err);
    display_perf_snapshot snapshot; display_perf_snapshot_get(&snapshot);
    snapshot.elapsed_us=s_bench_result.elapsed_us;
    snprintf(snapshot.trace_name,sizeof snapshot.trace_name,"%s",source==0?"sram":source==1?"generator":"psram");
    printf("PERF_CONFIG,{\"width\":%d,\"height\":%d,\"frames_requested\":%d,\"buffer_bytes\":%u,"
        "\"source\":\"%s\",\"errors\":%d,\"error_name\":\"%s\",\"completed_frames\":%lu,"
        "\"requested_hz\":%d,\"apps_suspended\":true,\"radios_running\":true}\n",
        w,h,frames,(unsigned)capacity,snapshot.trace_name,s_bench_result.err!=ESP_OK,
        esp_err_to_name(s_bench_result.err),(unsigned long)s_bench_result.frames_completed,LCD_SPI_HZ);
    display_perf_print("PERF_RUN",&snapshot);
    wos_invalidate();
}

static void perf_command(const char *s)
{
    while(*s==' ')s++;
    int w,h,n,used=0; char source[16]="generator",name[24];
    if(!strcmp(s,"info")) perf_info();
    else if(!strcmp(s,"probe")) board_lcd_probe_print();
    else if(!strcmp(s,"status")) {
        display_perf_snapshot snapshot; display_perf_snapshot_get(&snapshot); display_perf_print("PERF_STATUS",&snapshot);
    } else if(!strcmp(s,"export")) {
        display_perf_trace trace;
        for(size_t i=display_perf_trace_count();i>0;i--) if(display_perf_trace_get(i-1,&trace)) display_perf_print("PERF_APP",&trace.perf);
    } else if(sscanf(s,"app %23s %d %n",name,&n,&used)==2 && used>0 && !s[used]) {
        if(n<1 || n>1800 || s_trace_running) { printf("ERR app seconds 1..1800; one active trace\n"); return; }
        int app=-1; s_trace_restore_face=g_saved_face;
        if(!strcmp(name,"current")) app=wos_current_app();
        else if(!strcmp(name,"face0")) { g_saved_face=0; app=0; }
        else if(!strcmp(name,"pip")) { g_saved_face=1; app=0; }
        else if(!strcmp(name,"level")) app=17;
        else if(!strcmp(name,"breathe")) app=8;
        else if(!strcmp(name,"stopwatch")) app=1;
        else { printf("ERR app current|face0|pip|level|breathe|stopwatch\n"); return; }
        wos_open_app(app); s_trace_restore_awake=g_no_sleep;g_no_sleep=1;s_trace_running=1;
        display_perf_trace_start(name,n,esp_timer_get_time());
        printf("PERF_STARTED,{\"name\":\"%s\",\"seconds\":%d}\n",name,n);
    } else if(!strcmp(s,"sweep")) {
        printf("PERF_SWEEP,{\"clock_hz\":%d,\"clock_sweep\":\"unavailable; panel unknown\",\"max_buffer_bytes\":%d}\n",LCD_SPI_HZ,GFX_W*STRIPE_ROWS*2);
        for(int source_id=0;source_id<3;source_id++)
            for(int rows=4;rows<=STRIPE_ROWS;rows*=2) perf_run(466,466,500,source_id,466*rows*2);
    } else if(sscanf(s,"fill %d %15s %n",&n,source,&used)==2 && used>0 && !s[used]) {
        int src=!strcmp(source,"sram")?0:!strcmp(source,"generator")?1:!strcmp(source,"psram")?2:-1;
        if(src<0)printf("ERR source sram|generator|psram\n"); else perf_run(466,466,n,src,GFX_W*STRIPE_ROWS*2);
    } else if(sscanf(s,"rect %d %d %d %15s %n",&w,&h,&n,source,&used)==4 && used>0 && !s[used]) {
        int src=!strcmp(source,"sram")?0:!strcmp(source,"generator")?1:!strcmp(source,"psram")?2:-1;
        if(src<0)printf("ERR source sram|generator|psram\n"); else perf_run(w,h,n,src,GFX_W*STRIPE_ROWS*2);
    } else if(sscanf(s,"rect %d %d %d %n",&w,&h,&n,&used)==3 && used>0 && !s[used]) perf_run(w,h,n,1,GFX_W*STRIPE_ROWS*2);
    else printf("perf display info|probe|status|export|fill N sram|generator|psram|rect W H N [source]|app NAME SEC|sweep\n");
}

static void dbg_exec(char *s)
{
    int a, b, c, d, e, f;
    char ws[40], wp[68];
    if (!strncmp(s, "perf display", 12) && (s[12]==0 || s[12]==' ')) perf_command(s+12);
    else if (sscanf(s, "tap %d %d", &a, &b) == 2) {
        if (a < 0 || a >= GFX_W || b < 0 || b >= GFX_H) printf("ERR coordinates\n");
        else inj_tap(a, b);
    }
    else if (!strncmp(s,"wifi {",6)) {
        cJSON *root=cJSON_ParseWithOpts(s+5,NULL,1);
        cJSON *ssid=cJSON_GetObjectItemCaseSensitive(root,"ssid");
        cJSON *pass=cJSON_GetObjectItemCaseSensitive(root,"password");
        if(cJSON_IsString(ssid) && cJSON_IsString(pass)) hal_wifi_connect(ssid->valuestring,pass->valuestring);
        else printf("ERR Wi-Fi JSON requires ssid and password strings\n");
        cJSON_Delete(root);
    }
    else if (sscanf(s, "wifi %39s %67s", ws, wp) == 2) hal_wifi_connect(ws, wp);
    else if (!strncmp(s,"wifi ",5)) printf("ERR Wi-Fi credentials\n");
    else if (!strcmp(s, "wifi")) printf("Use wifi SSID PASSWORD or the setup page\n");
    else if (!strcmp(s, "bleinfo")) { char bi[96]; hal_ble_status(bi, sizeof bi); printf("BLEINFO: %s\n", bi); }
    else if (!strcmp(s, "aod"))    power_set_aod(!g_aod);
    else if (!strcmp(s, "hud"))    { extern int wos_debug_hud; wos_debug_hud = !wos_debug_hud; settings_save(); printf("HUD %d (saved)\n", wos_debug_hud); }
    else if (!strcmp(s, "time"))   { hal_datetime dt; hal_get_datetime(&dt); printf("TIME %04d-%02d-%02d %02d:%02d:%02d (tz %+dmin)\n", dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, g_tz_offset_min); }
    else if (sscanf(s, "settime %d %d %d %d %d %d", &a, &b, &c, &d, &e, &f) == 6) {
        /* set the wall clock over USB (no WiFi/BLE needed), local time */
        hal_datetime dt = { .year = a, .month = b, .day = c,
                           .hour = d, .minute = e, .second = f };
        if (!board_datetime_valid(&dt)) { printf("ERR invalid calendar\n"); return; }
        int yy = a - (b <= 2), era = yy / 400, yoe = yy - era * 400;   /* weekday */
        int doy = (153 * (b + (b > 2 ? -3 : 9)) + 2) / 5 + c - 1;
        long days = (long)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
        dt.weekday = (int)((days + 4) % 7);
        hal_set_datetime(&dt);
        printf("TIME set\n");
    }
    else if (sscanf(s, "beep %d %d", &a, &b) == 2) hal_beep(a, b);
    else if (!strcmp(s, "beep"))   hal_beep(1500, 400);
    else if (!strcmp(s, "mic")) printf("MIC unavailable on 1.43-B\n");
    else if (sscanf(s, "face %d", &a) == 1) {
        if (a < 0 || a >= 13) printf("ERR face index\n");
        else { g_saved_face = a; settings_save(); wos_open_app(0); printf("face=%d\n", g_saved_face); }
    }
    else if (!strcmp(s, "face"))   printf("face=%d\n", g_saved_face);
    else if (!strcmp(s, "mute"))   { g_muted = !g_muted; settings_save(); printf("muted=%d\n", g_muted); }
    /* Pip the creature: one-line mood/state dump */
    else if (!strcmp(s, "pip"))    { char pb[768]; printf("PIP %s\n", face_pip_debug(pb, sizeof pb)); }
    else if (!strncmp(s, "pip ", 4)) {
        printf("%s\n", face_pip_command(s + 4) ? "PIP OK" : "ERR pip command/action");
    }
    else if (!strncmp(s, "notify ", 7)) {
        char *separator=strchr(s+7,'|');
        if(separator){*separator=0;printf("%s\n",hal_notification_post(s+7,separator+1)?"NOTIFY queued":"ERR notification payload/full");}
        else printf("ERR use notify APP|TITLE\n");
    }
    else if (!strcmp(s,"power")) {
        hal_power p;hal_power_get(&p);
        printf("POWER rail=%dmV pct=%d valid=%d external=%d sample=%d age=%lums\n",p.rail_mv,p.battery_percent,p.battery_valid,p.external_power,p.sample_valid,(unsigned long)p.age_ms);
    }
    else if (!strcmp(s,"activity")) {
        hal_activity a;hal_activity_get(&a);
        printf("ACTIVITY estimated_steps=%lu active_seconds=%lu walking=%d valid=%d\n",(unsigned long)a.steps,(unsigned long)a.active_seconds,a.walking,a.valid);
    }
    else if (!strcmp(s,"weather")) {
        hal_weather w;hal_weather_get(&w);
        printf("WEATHER valid=%d stale=%d fetching=%d city=%s temp=%dC feels=%dC code=%d age=%lus observed=%lld wifi=%d sync=%d status=%s\n",
            w.valid,w.stale,w.fetching,w.city,w.temp,w.feels,w.code,(unsigned long)w.age_seconds,
            (long long)w.observed_epoch,hal_status(HAL_FEAT_WIFI),hal_status(HAL_FEAT_SYNC),w.status);
    }
    else if (!strcmp(s,"weather refresh")) {hal_weather_refresh();hal_time_resync();printf("WEATHER refresh requested\n");}
    else if (!strncmp(s,"location ",9)) {
        float lat,lon; int used=0;
        if(sscanf(s+9,"%f %f %n",&lat,&lon,&used)==2 && used>0 &&
           hal_weather_set_location(s+9+used,lat,lon)) printf("LOCATION saved\n");
        else printf("ERR use location LAT LON CITY (ASCII, max 31 bytes)\n");
    }
    else if (!strcmp(s, "bledrop")) { extern void ble_drop_all(void); ble_drop_all(); }   /* clear stale BLE links */
    else if (!strcmp(s, "battdump")) { extern void hal_batt_dump_now(void); hal_batt_dump_now(); }
    else if (!strcmp(s, "sleep"))  power_deep_sleep();
    else if (sscanf(s, "swipe %d %d %d %d", &a, &b, &c, &d) == 4) {
        if (a < 0 || a >= GFX_W || c < 0 || c >= GFX_W || b < 0 || b >= GFX_H || d < 0 || d >= GFX_H) printf("ERR coordinates\n");
        else inj_swipe(a, b, c, d);
    }
    else if (sscanf(s, "open %d", &a) == 1) {
        if (a < 0 || a >= wos_app_count()) printf("ERR app index\n");
        else { wos_open_app(a); printf("open %d -> %s\n", a, wos_app_name(a)); }
    }
    else if (!strcmp(s, "home"))   wos_open_app(0);
    else if (!strcmp(s, "close"))  wos_close_overlay();
    else if (!strcmp(s, "right"))  inj_swipe(GFX_W - 6, GFX_H / 2, GFX_W - 320, GFX_H / 2);
    else if (!strcmp(s, "left"))   inj_swipe(6, GFX_H / 2, 320, GFX_H / 2);
    else if (!strcmp(s, "top"))    inj_swipe(GFX_W / 2, 6, GFX_W / 2, 320);
    else if (!strcmp(s, "bottom")) inj_swipe(GFX_W / 2, GFX_H - 6, GFX_W / 2, GFX_H - 320);
    else if (sscanf(s, "bri %d", &a) == 1) {
        if (a < 1) a = 1;
        if (a > 255) a = 255;
        g_brightness = a; hal_set_backlight((uint8_t)a); settings_save(); printf("bri=%d saved\n", a);
    }
    else if (!strcmp(s,"bri")) printf("BRIGHTNESS level=%d percent=%d panel_command=%d\n",g_brightness,(g_brightness*100+127)/255,s_panel_brightness);
    else if (!strcmp(s, "pause"))  g_pause = 1;
    else if (!strcmp(s, "resume")) g_pause = 0;
    else if (sscanf(s,"prof %d",&a)==1) { g_prof=a!=0;printf("prof=%d\n",g_prof); }
    else if (!strcmp(s, "info"))
        printf("app=%d/%d name=%s\n", wos_current_app(), wos_app_count(),
               wos_app_name(wos_current_app()));
    else if (!strcmp(s, "diag touch")) {
        wos_open_app(16);
        app_diagnostics_touch_test();
        printf("DIAG touch\n");
    }
    else if(!strcmp(s,"diag imu")) {
        extern void app_diagnostics_imu_test(void);
        wos_open_app(16);app_diagnostics_imu_test();printf("DIAG imu\n");
    }
    else if(!strcmp(s,"wake")) {
        wake_info i;wake_service_get(&i);
        printf("WAKE wrist=%s pickup=%d turn=%d learned=%d learning=%d result=%d valid=%d stage=%d count=%lu reason=%s norm=%.3f speed=%.2f view=%.3f bias=%.2f,%.2f,%.2f\n",
            i.wrist?"right":"left",i.pickup,i.turn,i.learned,i.learning,i.learn_result,i.valid,i.stage,
            (unsigned long)i.wakes,motion_wake_reason(i.last_reason),i.magnitude,i.speed,i.view_dot,i.bias[0],i.bias[1],i.bias[2]);
    }
    else if(!strcmp(s,"wake left") || !strcmp(s,"wake right"))
        printf("%s\n",wake_service_set("wrist",!strcmp(s,"wake right"))?"WAKE OK":"ERR wake save");
    else if(sscanf(s,"wake pickup %d",&a)==1)
        printf("%s\n",wake_service_set("pickup",a)?"WAKE OK":"ERR wake setting/save");
    else if(sscanf(s,"wake turn %d",&a)==1)
        printf("%s\n",wake_service_set("turn",a)?"WAKE OK":"ERR wake setting/save");
    else if(!strcmp(s,"wake learn")) { wake_service_learn();printf("WAKE hold reading pose still\n"); }
    else if(!strcmp(s,"screen off"))g_screen_request=2;
    else if(!strcmp(s,"screen dim"))g_screen_request=1;
    else if(!strcmp(s,"screen"))printf("SCREEN state=%s aod=%d glance=%d\n",
        g_screen_state==2?"off":g_screen_state==1?"dim":"awake",g_aod,g_aod_glance);
    else if (!strcmp(s, "board")) board_print_info();
    else if (sscanf(s, "tz %d", &a) == 1) {
        if (a < -720 || a > 840) printf("ERR timezone range\n");
        else { g_tz_offset_min = a; board_apply_timezone(); settings_save(); printf("TZ %+d\n", a); }
    }
    else if (!strcmp(s, "imu")) {
        hal_imu m; int ok = hal_imu_read(&m);
        printf("IMU valid=%d accel=%.4f,%.4f,%.4f gyro=%.2f,%.2f,%.2f\n", ok, m.ax, m.ay, m.az, m.gx, m.gy, m.gz);
    }
    else if (!strcmp(s, "sd")) {
        hal_storage sd; hal_storage_get(&sd);
        printf("SD mounted=%d total=%llu free=%llu %s\n", sd.mounted,
            (unsigned long long)sd.total_bytes, (unsigned long long)sd.free_bytes, sd.message);
    }
    else if (!strcmp(s, "sdmount")) printf("SDMOUNT %s\n", esp_err_to_name(board_sd_mount()));
    else if (!strcmp(s, "sdeject")) printf("SDEJECT %s\n", esp_err_to_name(board_sd_unmount()));
    else if (!strcmp(s, "sdtest")) printf("SDTEST %s\n", esp_err_to_name(board_sd_test()));
    else if (sscanf(s, "awake %d", &a) == 1) { g_no_sleep = a != 0; printf("awake=%d\n", g_no_sleep); }
    else if (!strcmp(s, "apps")) {
        for (int i = 0; i < wos_app_count(); i++) printf("APP %d %s\n", i, wos_app_name(i));
    }
    else if (!strcmp(s, "help"))
        printf("cmds: tap X Y | swipe X0 Y0 X1 Y1 | open N | home | close | "
               "left|right|top|bottom | bri 1..255 | info | apps | board | imu | sd | "
               "sdmount | sdeject | sdtest | awake 0|1 | prof 0|1 | time | settime Y M D h m s | "
               "pip [care|hello|pet|play|save|center|live|demo ACTION|routine WAKE REST] | power | activity | notify APP|TITLE | "
               "wake [left|right|pickup 0|1|turn 0|1|learn] | screen [off|dim] | diag imu | weather [refresh] | location LAT LON CITY | wifi {JSON}\n");
    else if (s[0]) printf("? '%s' (try 'help')\n", s);
}
static int command_wakes_display(const char *s)
{
    const char *const queries[]={"wake","screen","imu","board","info","time","activity","power","pip","face","prof 0","prof 1","weather","bri"};
    for(unsigned i=0;i<sizeof queries/sizeof queries[0];i++)if(!strcmp(s,queries[i]))return 0;
    return 1;
}
static void dbg_cmd_task(void *arg)
{
    (void)arg;
    debug_line line;
    int len = 0, overflow = 0;
    for (;;) {
        uint8_t ch;
        int n = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(200));
        if (n <= 0 || ch == '\r') continue;
        if (ch == '\n') {
            if (overflow) printf("ERR command too long\nDONE\n");
            else if (len) {
                line.text[len] = 0;
                if (xQueueSend(s_cmd_q, &line, pdMS_TO_TICKS(100)) != pdTRUE)
                    printf("ERR command queue full\nDONE\n");
            }
            len = 0; overflow = 0; continue;
        }
        if (len < (int)sizeof line.text - 1) line.text[len++] = (char)ch;
        else overflow = 1;
    }
}

/* The flush worker releases input buffers in FIFO order after rotating them.
 * Within a frame, the next free credit therefore matches the next input slot.
 * frame_end drains both input buffers AND the last DMA before slot 0 restarts. */
int64_t s_prof_wait_us, s_prof_flush_us;   /* profiling accumulators */

/* wos calls this before drawing a band: block until a stripe buffer is free. */
static void stripe_wait(void)
{
    int64_t t = esp_timer_get_time();
    xSemaphoreTake(s_buf_free, portMAX_DELAY);
    s_prof_wait_us += esp_timer_get_time() - t;
    display_perf_add_stage(DISPLAY_PERF_QUEUE_WAIT, esp_timer_get_time()-t);
}

/* wos calls this after drawing a dirty rect's stripe: hand it to the flush core
 * (core 1), which byte-swaps + compacts + DMAs it in parallel with the next
 * stripe's render. */
static void stripe_flush(uint16_t *buf, int x0, int y0, int w, int rows)
{
    flush_job j = { buf, x0, y0, w, rows, s_atomic };
    xQueueSend(s_flush_q, &j, portMAX_DELAY);
    display_perf_queue_depth(uxQueueMessagesWaiting(s_flush_q));
}

/* Wait for all stripes at every frame boundary, including the final DMA. */
static void frame_begin(int atomic, int x0, int y0, int x1, int y1)
{
    s_atomic = atomic && s_backbuf != NULL;
    s_bx0 = x0; s_by0 = y0; s_bx1 = x1; s_by1 = y1;
}

static void frame_end(int atomic)
{
    xSemaphoreTake(s_buf_free, portMAX_DELAY);   /* both stripe buffers free => */
    xSemaphoreTake(s_buf_free, portMAX_DELAY);   /* the back-buffer is fully composed */
    xSemaphoreGive(s_buf_free);
    xSemaphoreGive(s_buf_free);
    if (!atomic || !s_backbuf) {
        xSemaphoreTake(s_dma_ready,portMAX_DELAY);
        xSemaphoreGive(s_dma_ready);
        return;
    }
    int x0 = s_bx0, w = s_bx1 - s_bx0;            /* the band is only [x0,x1) wide */
    for (int y = s_by0; y < s_by1; y += STRIPE_ROWS) {
        int rows = (s_by1 - y) < STRIPE_ROWS ? (s_by1 - y) : STRIPE_ROWS;
        int slot = ((y - s_by0) / STRIPE_ROWS) & 1;
        xSemaphoreTake(s_buf_free, portMAX_DELAY);          /* free bounce buffer */
        for (int r = 0; r < rows; r++)                      /* pack the band's cols */
            memcpy(&s_stripe[slot][r * w],
                   &s_backbuf[(size_t)(y + r) * GFX_W + x0], (size_t)w * 2);
        flush_job job = { s_stripe[slot], x0, y, w, rows, 2 };
        xQueueSend(s_flush_q, &job, portMAX_DELAY);
    }
    xSemaphoreTake(s_buf_free, portMAX_DELAY);   /* drain the last two input buffers */
    xSemaphoreTake(s_buf_free, portMAX_DELAY);
    xSemaphoreGive(s_buf_free);
    xSemaphoreGive(s_buf_free);
    xSemaphoreTake(s_dma_ready,portMAX_DELAY);   /* last rotated output still in flight */
    xSemaphoreGive(s_dma_ready);
}

void app_main(void)
{
    display_perf_init();
    /* Reserve the display's internal DMA stripe buffers FIRST. The radios
     * (Wi-Fi + BT, brought up in hal_esp32_start_radios) consume most of the
     * internal RAM, so grabbing these before them guarantees the panel always
     * has its DMA buffers (otherwise this malloc fails and the UI never starts). */
    s_stripe[0] = heap_caps_malloc((size_t)GFX_W * STRIPE_ROWS * 2,
                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_stripe[1] = heap_caps_malloc((size_t)GFX_W * STRIPE_ROWS * 2,
                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_rotated = heap_caps_malloc((size_t)GFX_W * STRIPE_ROWS * 2,
                                   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(s_stripe[0] && s_stripe[1] && s_rotated);

    hal_esp32_init_periph();
    lcd_init();
    board_init();
    settings_load();       /* restore brightness / mute / face BEFORE any sound or draw */
    face_pip_service(0);
    board_restore_clock();
    hal_esp32_start_radios();
    hal_play(SND_STARTUP); /* little 8-bit power-on jingle (silent while muted) */

    /* BOOT button (GPIO0): input w/ pull-up (active low). Polled in the main loop. */
    gpio_config_t btn = { .pin_bit_mask = 1ULL << 0, .mode = GPIO_MODE_INPUT, .pull_up_en = 1 };
    gpio_config(&btn);

    s_buf_free = xSemaphoreCreateCounting(2, 2);    /* both buffers start free */
    s_dma_ready = xSemaphoreCreateBinary();
    s_flush_q = xQueueCreate(2, sizeof(flush_job));
    s_sleep_ready = xSemaphoreCreateBinary();
    s_bench_done = xSemaphoreCreateBinary();
    assert(s_bench_done);
    assert(s_buf_free && s_dma_ready && s_flush_q && s_sleep_ready);
    xSemaphoreGive(s_dma_ready);
    s_backbuf = heap_caps_malloc((size_t)GFX_W * GFX_H * 2, MALLOC_CAP_SPIRAM);
    if (!s_backbuf)
        ESP_LOGW(TAG, "no PSRAM back-buffer -> atomic present disabled");
    BaseType_t task_ok = xTaskCreatePinnedToCore(flush_task, "flush", 3072, NULL,
                            configMAX_PRIORITIES - 2, &s_flush_handle, 1);   /* core 1 */
    assert(task_ok == pdPASS);
    ESP_LOGI(TAG, "free internal DRAM: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    static const wos_app *const apps[] = {
        &app_watchface, &app_stopwatch, &app_timer, &app_alarm,
        &app_worldclock, &app_calendar, &app_weather, &app_activity,
        &app_breathe, &app_music, &app_recorder, &app_voice, &app_findphone,
        &app_ble, &app_netmon, &app_wifi, &app_sensors, &app_level,
        &app_calc, &app_2048, &app_flashlight, &app_battery, &app_pomodoro, &app_settings, &app_storage, &app_pet_care,
    };
    wos_init(NULL, apps, sizeof apps / sizeof apps[0]);   /* stripe render: no full fb */
    wos_set_stripes(s_stripe[0], s_stripe[1], STRIPE_ROWS, stripe_wait, stripe_flush);
    /* No TE wire on this board; loop pacing below limits rendering to 60 Hz. */
    wos_set_frame_hooks(frame_begin, frame_end);   /* atomic present for full redraws */
    wos_register_overlay(WOS_EDGE_LEFT,   &app_launcher,      WOS_OV_PANNABLE);
    wos_register_overlay(WOS_EDGE_RIGHT,  &app_applist,       0);
    wos_register_overlay(WOS_EDGE_TOP,    &app_notifications, 0);
    wos_register_overlay(WOS_EDGE_BOTTOM, &app_control,       0);

    /* Test harness: USB-serial command interface (see dbg_exec). Route stdio
     * through the driver so ESP_LOG (PROF) and the command reader share it. */
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&ucfg));
    usb_serial_jtag_vfs_use_driver();
    s_cmd_q = xQueueCreate(4, sizeof(debug_line)); assert(s_cmd_q);
    task_ok = xTaskCreate(dbg_cmd_task, "dbg", 4096, NULL, 4, NULL);
    assert(task_ok == pdPASS);

    ESP_LOGI(TAG, "watch os running (%dx%d)", GFX_W, GFX_H);
    printf("READY,{\"fw\":\"%s\",\"commands\":\"perf display help\",\"panel\":\"unconfirmed\",\"clock_hz\":%d}\n",esp_app_get_description()->version,LCD_SPI_HZ);
    s_frame_tick=xSemaphoreCreateBinary();assert(s_frame_tick);
    const esp_timer_create_args_t pace_timer={.callback=frame_tick,.name="frame_pace"};
    ESP_ERROR_CHECK(esp_timer_create(&pace_timer,&s_frame_timer));
    int64_t facc = 0, tacc = 0, last = esp_timer_get_time();
    int64_t record_acc=0,raster_acc=0,present_acc=0,previous_present=0;
    uint16_t intervals[101]={0};int interval_count=0,interval_max=0;
    int frames = 0;

    /* Screen power management: full brightness while active, dim after 30s of no
     * touch, off after 2min. Any touch wakes it. While off we stop rendering to
     * save power; time-based apps catch up (big dt) on the next wake. */
    uint32_t s_activity = (uint32_t)(esp_timer_get_time() / 1000);
    int scr = 0;   /* 0 = on, 1 = dim, 2 = off */
    for (;;) {
        display_perf_trace finished;
        if(display_perf_tick(esp_timer_get_time(), &finished)) {
            display_perf_print("PERF_APP", &finished.perf);
            if(s_trace_running){g_no_sleep=s_trace_restore_awake;g_saved_face=s_trace_restore_face;s_trace_running=0;wos_invalidate();}
        }
        int64_t a = esp_timer_get_time();
        int command_activity = 0;
        if (!g_inj_active && !s_debug_done) {
            debug_line line;
            if (xQueueReceive(s_cmd_q, &line, 0) == pdTRUE) {
                dbg_exec(line.text); s_debug_done = 1; command_activity = command_wakes_display(line.text);
            }
        }
        int touched;
        if (g_inj_active) {                    /* test harness drives the pointer */
            inj_poll();
            touched = g_inj_down;
        } else {
            touched = touch_poll();
        }
        uint32_t nowms = (uint32_t)(esp_timer_get_time() / 1000);
        if(g_screen_request>=0) {
            scr=g_screen_request;g_screen_request=-1;g_aod=g_aod_glance=0;g_no_sleep=0;
            s_activity=nowms;command_activity=touched=0;
            hal_set_backlight(scr==2?0:(uint8_t)(g_brightness/5+4));
        }
        if(g_aod_glance && nowms-g_glance_ms>=10000) {
            g_aod_glance=0;if(g_aod)hal_set_backlight(10);
        }
        int motion_wake=wake_service_tick(nowms,!g_no_sleep && (scr!=0 || (g_aod&&!g_aod_glance)));
        if(motion_wake || (g_aod && touched)) {
            scr=0;s_activity=nowms;command_activity=1;
            if(g_aod){g_aod_glance=1;g_glance_ms=nowms;}
            hal_set_backlight((uint8_t)g_brightness);
            if(motion_wake && wos_app_visible(0) && g_saved_face==1)face_pip_command("hello");
        }
        g_screen_state=scr;
        hal_batt_log_tick();          /* sample battery ~1/min + push log when the companion subscribes */
        alarm_service_tick();         /* fire enabled alarms (self-throttled; rings in any app) */
        face_pip_display(scr==0 && (!g_aod||g_aod_glance));
        face_pip_service(0);

        /* find-my-puck: companion pinged us -> make noise + light up until the deadline passes */
        {
            static uint32_t s_loc_beep;
            if (hal_locate_active()) {
                if (nowms - s_loc_beep >= 2600) {
                    s_loc_beep = nowms;
                    command_activity = 1;
                    hal_set_backlight(255);
                    wos_callout("FIND MY PUCK", 2500);
                    hal_play(SND_ALARM);
                }
            }
        }

        /* BOOT button (GPIO0) gestures, each with an on-screen callout:
         *   SHORT   = home / exit AOD          DOUBLE  = toggle AOD (Always-On)
         *   HOLD >=1.5s = Sleep (deep sleep)   HOLD >=3s = Power Off
         * While held, a live toast previews which action a release will trigger, so you can keep
         * holding from Sleep into Power Off. (Second physical button TBD — give me its GPIO and I
         * can split these across the two.) */
        {
            static int bt_prev = 1, bt_pending = 0;
            static uint32_t bt_down = 0, bt_up = 0;
            int lv = gpio_get_level(GPIO_NUM_0);
            if (lv == 0 && bt_prev == 1) bt_down = nowms;             /* pressed */
            else if (lv == 0) {                                       /* held: preview the action */
                uint32_t held = nowms - bt_down;
                if (held >= 3000)      wos_callout("Power Off", 400);
                else if (held >= 1500) wos_callout("Sleep", 400);
            }
            else if (lv == 1 && bt_prev == 0) {                       /* released */
                uint32_t held = nowms - bt_down;
                s_activity = nowms;
                command_activity = 1;
                if (held >= 3000)      power_off();
                else if (held >= 1500) power_deep_sleep();
                else if (bt_pending && nowms - bt_up < 400) { bt_pending = 0; power_set_aod(!g_aod); }
                else { bt_pending = 1; bt_up = nowms; }
            }
            if (bt_pending && nowms - bt_up > 400) {                  /* settle single click */
                bt_pending = 0;
                if (g_aod) power_set_aod(0); else wos_open_app(0);
            }
            bt_prev = lv;
        }

        if (s_debug_done && !g_inj_active) {
            printf("DONE\n"); fflush(stdout); s_debug_done = 0;
        }

        /* AOD: keep the watchface up, dim, refreshed at ~1 Hz to sip power */
        if (g_aod && !g_aod_glance) {
            static uint32_t last_aod_draw;
            if (nowms - last_aod_draw >= 1000 || touched || command_activity || g_inj_active) {
                wos_run_frame(); last_aod_draw = nowms;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (touched || command_activity) {
            s_activity = nowms;
            if (scr != 0) { scr = 0; hal_set_backlight((uint8_t)g_brightness); }
        }
        uint32_t idle = nowms - s_activity;
        if (!g_no_sleep && scr == 0 && idle > 30000u) {   /* dim */
            scr = 1;
            hal_set_backlight((uint8_t)(g_brightness / 5 + 4));
        }
        if (!g_no_sleep && scr <= 1 && idle > 120000u) {  /* off */
            scr = 2;
            hal_set_backlight(0);
        }
        if (scr == 2) { vTaskDelay(pdMS_TO_TICKS(40)); continue; }   /* asleep: no render */
        if (g_pause) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }    /* frozen (hw-scroll probe) */

        /* POWER: a STATIC screen (control centre, notifications, a face with no moving hand) has
         * nothing to redraw, so instead of spinning the record+damage pass at ~1kHz we drop to a
         * ~1Hz heartbeat and just poll the touch panel at ~50Hz. Any touch, or any frame that
         * actually drew (an animation / transition in flight), ramps straight back to full speed. */
        static uint32_t s_touch_ms = 0, s_render_active = 0, s_last_render = 0;
        if (touched) s_touch_ms = nowms;
        int recent_touch = (nowms - s_touch_ms)     < 600u;   /* catches the tap's action + fade */
        int animating    = (nowms - s_render_active) < 350u;  /* something moved recently */
        int heartbeat    = (nowms - s_last_render)  >= 1000u; /* keep clock/battery fresh at 1Hz */
        if (!recent_touch && !animating && !heartbeat && !g_inj_active && !command_activity) {
            vTaskDelay(pdMS_TO_TICKS(20));      /* idle: skip the render, keep touch responsive */
            continue;
        }

        int64_t b = esp_timer_get_time();
        int drew = wos_run_frame();      /* renders + flushes only the dirty band */
        int64_t c = esp_timer_get_time();
        extern uint32_t wos_dbg_record_us,wos_dbg_raster_us,wos_dbg_present_us;
        extern int wos_dbg_nr;
        extern uint32_t wos_dbg_dirty_pixels;
        display_perf_frame(wos_dbg_dirty_pixels,wos_dbg_nr,s_atomic && drew,wos_dbg_record_us,wos_dbg_raster_us,wos_dbg_present_us,c-b);
        record_acc+=wos_dbg_record_us;raster_acc+=wos_dbg_raster_us;present_acc+=wos_dbg_present_us;
        if(previous_present && c-previous_present<100000) {
            int us=(int)(c-previous_present),bucket=(us+999)/1000;
            intervals[bucket]++;interval_count++;
            if(us>interval_max)interval_max=us;
        }
        g_screen_state=scr;
        previous_present=c;
        s_last_render = nowms;
        if (drew) s_render_active = nowms;
        /* Microsecond deadline instead of truncating the wait to scheduler
         * ticks. Yield the CPU while waiting; slow frames never catch up in
         * bursts. The panel has no TE signal, so this is software 60 Hz pacing. */
        int64_t frame_left = 16667 - (esp_timer_get_time() - a);
        if (frame_left > 0) {
            ESP_ERROR_CHECK(esp_timer_start_once(s_frame_timer,(uint64_t)frame_left));
            xSemaphoreTake(s_frame_tick,portMAX_DELAY);
        }
        tacc += b - a; facc += c - b; frames++;
        if (frames > 0 && c - last >= 1000000) {
            extern int wos_dbg_n, wos_dbg_bands, wos_dbg_rows, layer_dbg;
            double f = frames;
            int p95=0,cumulative=0;
            for(int i=0;i<=100;i++){cumulative+=intervals[i];if(cumulative*100>=interval_count*95){p95=i;break;}}
            /* USB writes may block if a host stops reading. Profiling is an
             * explicit diagnostic mode, off on boot and after the test tool. */
            if(g_prof)ESP_LOGI(TAG, "PROF fps=%d render=%.1f swap=%.1f | cmds=%d bands=%d rows=%d layer/s=%d | bat=%dmV(%d%%) | record=%.2f raster=%.2f present=%.2f wait=%.2f p95_ms=%d max_ms=%.2f",
                     frames,
                     (facc - s_prof_flush_us - s_prof_wait_us) / 1000.0 / f,
                     s_prof_flush_us / 1000.0 / f,
                     wos_dbg_n, wos_dbg_bands, wos_dbg_rows, layer_dbg,
                     hal_battery_mv(), hal_battery_percent(),
                     record_acc/1000.0/f,raster_acc/1000.0/f,present_acc/1000.0/f,
                     s_prof_wait_us/1000.0/f,p95,interval_max/1000.0);
            record_acc=raster_acc=present_acc=0;
            memset(intervals,0,sizeof intervals);interval_count=interval_max=0;
            facc = tacc = 0; frames = 0; last = c;
            s_prof_flush_us = s_prof_wait_us = 0;
            layer_dbg = 0;
        }
    }
}
