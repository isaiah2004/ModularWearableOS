/* nos_main.c — NOS firmware host for the Waveshare ESP32-S3-Touch-AMOLED-1.43-B.
 *
 * Core 0 runs the UI: input -> nos_frame_run (record, damage, raster into 16-row
 * internal-RAM stripes). Core 1 runs the flush task: rotate each stripe 90 degrees
 * CCW into one of two DMA buffers (the panel is mounted rotated and the vendor
 * driver cannot swap axes) and send it over 40 MHz QSPI while core 0 renders the
 * next stripe. Frames are paced at exactly 30 Hz by an esp_timer deadline.
 *
 * Power: ON (30 Hz) -> after nos_prefs.sleep_s of no input -> AOD (1 Hz, dim) if
 * enabled, else OFF (no rendering, panel brightness 0). Touch, the button or a
 * wrist raise wakes; the waking touch is swallowed so it never also taps. On wake
 * the first frame renders before the brightness comes back (no stale-frame flash). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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

#include "pins.h"
#include "board.h"
#include "board_protocol.h"
#include "lcd_probe.h"
#include "ota.h"
#include "../../core/settings.h"
#include "../../core/wake_service.h"
#include "nos.h"
#include "prefs.h"
#include "hal_ext.h"
#include "svc.h"
#include "apps/nos_apps.h"
#include "faces/face.h"
#include "dot.h"

#if CONFIG_WATCH_BOARD_143C
#error "nos_main.c targets the 1.43-B; the 1.43C build still uses main.c"
#endif

#define TAG "nos"
#define STRIPE_ROWS 16
#define PANEL_GAP_X (LCD_USE_SH8601 ? 0 : 6)

void hal_esp32_init_periph(void);
void hal_esp32_start_radios(void);

/* ------------------------------------------------------------------ display */
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_stripe[2];
static uint16_t *s_rotated[2];
static SemaphoreHandle_t s_dma_ready;     /* free rotated buffers; the ISR returns one per DMA */
static SemaphoreHandle_t s_buf_free;      /* free stripe buffers */
static QueueHandle_t s_flush_q;
static SemaphoreHandle_t s_frame_tick;
static esp_timer_handle_t s_frame_timer;
static TaskHandle_t s_flush_handle;

typedef struct { uint16_t *buf; int x0, y0, w, rows, stride; int kind; } flush_job;
enum { JOB_STRIPE = 0, JOB_BRIGHT = 1 };

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

static bool on_dma_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_dma_ready, &woken);
    return woken == pdTRUE;
}

static volatile uint32_t s_rot_us, s_wait_us;   /* per-frame trace: rotate (core 1), stripe waits (core 0) */
static void flush_task(void *arg)
{
    flush_job j;
    int slot = 0;
    for (;;) {
        xQueueReceive(s_flush_q, &j, portMAX_DELAY);
        if (j.kind == JOB_BRIGHT) {
            uint8_t level = (uint8_t)j.x0;
            esp_lcd_panel_io_tx_param(s_io, (0x02ULL << 24) | (0x51 << 8), &level, 1);
            continue;
        }
        uint16_t *out = s_rotated[slot];
        xSemaphoreTake(s_dma_ready, portMAX_DELAY);
        /* one pass: read the dirty window, byte-swap to wire order, rotate CCW */
        int64_t r0 = esp_timer_get_time();
        board_pixels_ccw_swap(j.buf, j.stride, out, j.w, j.rows, 1);
        s_rot_us += (uint32_t)(esp_timer_get_time() - r0);
        xSemaphoreGive(s_buf_free);              /* the stripe can be re-rendered now */
        slot ^= 1;
        /* UI (x, y) -> native (y, 465 - x): the window is rows-wide, w-tall */
        esp_lcd_panel_draw_bitmap(s_panel, j.y0, GFX_W - j.x0 - j.w, j.y0 + j.rows, GFX_W - j.x0, out);
    }
}

void display_set_brightness(uint8_t level)
{
    if (s_flush_q) {
        flush_job j = { 0 };
        j.kind = JOB_BRIGHT; j.x0 = level;
        xQueueSend(s_flush_q, &j, portMAX_DELAY);
    } else if (s_io)
        esp_lcd_panel_io_tx_param(s_io, (0x02ULL << 24) | (0x51 << 8), &level, 1);
}

static void lcd_init(void)
{
    gpio_config_t en = { .pin_bit_mask = 1ULL << PIN_LCD_EN, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&en));
    ESP_ERROR_CHECK(gpio_set_level(PIN_LCD_EN, 1));
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t id = board_lcd_probe();
    ESP_LOGI(TAG, "PANEL id=0x%02x init=%s", id, LCD_USE_SH8601 ? "SH8601" : "CO5300");
    const spi_bus_config_t bus = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_PCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3, GFX_W * STRIPE_ROWS * 2 + 64);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io_cfg = SH8601_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, on_dma_done, NULL);
    io_cfg.pclk_hz = LCD_SPI_HZ;
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
}

/* engine stripe hooks */
static void stripe_wait(void)
{
    int64_t t = esp_timer_get_time();
    xSemaphoreTake(s_buf_free, portMAX_DELAY);
    s_wait_us += (uint32_t)(esp_timer_get_time() - t);
}
static void stripe_flush(uint16_t *buf, int stride, int x0, int y0, int w, int rows)
{
    flush_job j = { buf, x0, y0, w, rows, stride, JOB_STRIPE };
    xQueueSend(s_flush_q, &j, portMAX_DELAY);
}
/* every stripe rendered and every DMA finished: the frame is on the panel */
static void frame_drain(void)
{
    xSemaphoreTake(s_buf_free, portMAX_DELAY); xSemaphoreTake(s_buf_free, portMAX_DELAY);
    xSemaphoreGive(s_buf_free); xSemaphoreGive(s_buf_free);
    xSemaphoreTake(s_dma_ready, portMAX_DELAY); xSemaphoreTake(s_dma_ready, portMAX_DELAY);
    xSemaphoreGive(s_dma_ready); xSemaphoreGive(s_dma_ready);
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

static void dotbake_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    dot_prebake_all();
    printf("DOT masks baked in %d ms\n", (int)((esp_timer_get_time() - t0) / 1000));
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ power */
enum { PWR_ON = 0, PWR_AOD, PWR_OFF };
static int s_pwr = PWR_ON;
static int s_swallow;                 /* the waking touch never reaches the UI */
static int s_cur_level = -1;          /* last panel brightness sent */
static float s_auto_k = 1.0f;         /* ambient multiplier, smoothed */
static int s_no_sleep;
static int s_aod_lead;                /* AOD just began: its first frame runs at 30 Hz */

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
static void set_level(int lvl)
{
    if (lvl == s_cur_level) return;
    s_cur_level = lvl;
    display_set_brightness((uint8_t)lvl);
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

static void go_state(int st)
{
    if (st == s_pwr) return;
    s_pwr = st;
    if (st == PWR_ON) { nos_aod = 0; nos_invalidate(); nos_keep_awake(); }
    else if (st == PWR_AOD) { nos_aod = 1; nos_home_now(); nos_invalidate(); s_aod_lead = 1; }
    else set_level(0);
}

/* hal_ext.h: 0 off, 1 AOD, 2 on (the power sheet's "Sleep" calls this) */
void hal_display_power(int mode) { go_state(mode == 2 ? PWR_ON : mode == 1 ? PWR_AOD : PWR_OFF); }

/* power sheet hook: 0 sleep display, 1 restart, 2 power off (deep sleep, BOOT wakes) */
void nos_host_power(int mode)
{
    if (mode == 0) { go_state(PWR_OFF); return; }
    set_level(0);
    vTaskDelay(pdMS_TO_TICKS(150));
    prefs_save(); prefs_tick();
    if (mode == 1) esp_restart();
    board_prepare_sleep();
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep_start();
}

/* ------------------------------------------------------------------ serial */
typedef struct { char text[192]; } cmd_line;
static QueueHandle_t s_cmd_q;
static int s_inj_active, s_inj_x0, s_inj_y0, s_inj_x1, s_inj_y1, s_inj_ms;
static int64_t s_inj_t0;
static int s_perf;
static int s_trace;
extern uint32_t nos_dbg_us[4];                   /* print the next N painted frames (NOSF lines) */

static void cmd_task(void *arg)
{
    cmd_line line; int len = 0;
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(200)) <= 0 || ch == '\r') continue;
        if (ch == '\n') {
            if (len) { line.text[len] = 0; xQueueSend(s_cmd_q, &line, pdMS_TO_TICKS(50)); }
            len = 0; continue;
        }
        if (len < (int)sizeof line.text - 1) line.text[len++] = (char)ch;
    }
}

static const nos_screen *screen_by_name(const char *n)
{
    for (int i = 0; i < nos_app_count; i++)
        if (!strcasecmp(nos_app_list[i].name, n)) return nos_app_list[i].scr;
    static const struct { const char *n; const nos_screen *s; } extra[] = {
        { "control", &scr_control }, { "notifs", &scr_notifs }, { "launcher", &scr_launcher },
        { "faces", &scr_faces }, { "power", &scr_power }, { "modules", &scr_modules },
        { "about", &scr_about }, { "activity", &scr_activity }, { "heart", &scr_heart },
    };
    for (unsigned i = 0; i < sizeof extra / sizeof extra[0]; i++)
        if (!strcasecmp(extra[i].n, n)) return extra[i].s;
    return 0;
}

static void cmd_exec(char *s)
{
    int a, b, c, d, e;
    if (sscanf(s, "tap %d %d", &a, &b) == 2) {
        s_inj_x0 = s_inj_x1 = a; s_inj_y0 = s_inj_y1 = b; s_inj_ms = 90;
        s_inj_t0 = esp_timer_get_time(); s_inj_active = 1;
    } else if (sscanf(s, "swipe %d %d %d %d %d", &a, &b, &c, &d, &e) >= 4) {
        s_inj_x0 = a; s_inj_y0 = b; s_inj_x1 = c; s_inj_y1 = d;
        s_inj_ms = (sscanf(s, "swipe %d %d %d %d %d", &a, &b, &c, &d, &e) == 5) ? e : 220;
        s_inj_t0 = esp_timer_get_time(); s_inj_active = 1;
    } else if (!strcmp(s, "btn")) { nos_button(1); vTaskDelay(pdMS_TO_TICKS(80)); nos_button(0); }
    else if (sscanf(s, "face %d", &a) == 1) {          /* bench: set the face directly */
        if (a >= 0 && a < nos_face_count) { nos_prefs.face = (uint8_t)a; sys_home_build_in(); nos_invalidate(); }
        printf("FACE %d\n", nos_prefs.face);
    }
    else if (sscanf(s, "full %d", &a) == 1) nos_debug_force_full = a;   /* bench: every frame a full repaint */
    else if (!strncmp(s, "open ", 5)) {
        const nos_screen *sc = screen_by_name(s + 5);
        if (sc) { go_state(PWR_ON); nos_open(sc, GFX_W / 2, GFX_H / 2); printf("OPEN %s\n", sc->name); }
        else printf("ERR no screen %s\n", s + 5);
    } else if (!strcmp(s, "home")) nos_home();
    else if (!strcmp(s, "perf")) { s_perf = !s_perf; printf("PERF %d\n", s_perf); }
    else if (sscanf(s, "trace %d", &a) == 1) { s_trace = a; printf("TRACE %d\n", a); }
    else if (!strcmp(s, "awake")) { s_no_sleep = !s_no_sleep; printf("AWAKE %d\n", s_no_sleep); }
    else if (sscanf(s, "bri %d", &a) == 1) { nos_prefs.brightness = (uint8_t)a; prefs_save(); }
    else if (sscanf(s, "aod %d", &a) == 1) go_state(a ? PWR_AOD : PWR_ON);
    else if (!strncmp(s, "gb ", 3)) svc_phone_inject(s + 3);
    else if (!strncmp(s, "ext", 3)) hal_ext_debug(s);
    else if (!strcmp(s, "info")) {
        printf("NOS %s board=%s internal_free=%u psram_free=%u depth=%d top=%s\n",
               esp_app_get_description()->version, WATCH_BOARD_NAME,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), nos_depth(),
               nos_top() ? nos_top()->name : "-");
    } else if (!strcmp(s, "help"))
        printf("tap X Y | swipe X0 Y0 X1 Y1 [MS] | btn | open NAME | home | perf | trace N | awake | bri PCT | aod 0|1 | gb LINE | ext ... | info\n");
    else printf("ERR unknown: %s\n", s);
    printf("DONE\n");
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
static int s_btn_swallow;
void app_main(void)
{
    /* DMA stripe buffers first: the radios eat most of internal RAM */
    for (int i = 0; i < 2; i++) {
        s_stripe[i] = heap_caps_malloc((size_t)GFX_W * STRIPE_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_rotated[i] = heap_caps_malloc((size_t)GFX_W * STRIPE_ROWS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(s_stripe[i] && s_rotated[i]);
    }
    uint16_t *scratch = heap_caps_malloc((size_t)GFX_W * STRIPE_ROWS * 2, MALLOC_CAP_SPIRAM);

    hal_esp32_init_periph();
    lcd_init();
    board_init();
    settings_load();
    board_restore_clock();
    prefs_load();
    g_tz_offset_min = nos_prefs.tz_min;
    board_apply_timezone();
    hal_ext_init();
    hal_esp32_start_radios();

    gpio_config_t btn = { .pin_bit_mask = 1ULL << 0, .mode = GPIO_MODE_INPUT, .pull_up_en = 1,
                          .intr_type = GPIO_INTR_ANYEDGE };
    gpio_config(&btn);
    esp_err_t ie = gpio_install_isr_service(0);
    if (ie == ESP_OK || ie == ESP_ERR_INVALID_STATE) gpio_isr_handler_add(GPIO_NUM_0, btn_isr, NULL);

    s_buf_free = xSemaphoreCreateCounting(2, 2);
    s_dma_ready = xSemaphoreCreateCounting(2, 2);
    s_flush_q = xQueueCreate(3, sizeof(flush_job));
    assert(s_buf_free && s_dma_ready && s_flush_q);
    BaseType_t ok = xTaskCreatePinnedToCore(flush_task, "flush", 3072, NULL, configMAX_PRIORITIES - 2, &s_flush_handle, 1);
    assert(ok == pdPASS);

    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&ucfg));
    usb_serial_jtag_vfs_use_driver();
    s_cmd_q = xQueueCreate(4, sizeof(cmd_line));
    ok = xTaskCreate(cmd_task, "cmd", 3072, NULL, 4, NULL);
    assert(ok == pdPASS);

    nos_set_stripes(s_stripe[0], s_stripe[1], STRIPE_ROWS, stripe_wait, stripe_flush);
    nos_set_scratch(scratch);
    nos_init(NULL);
    nos_apps_boot();
    /* bake every dot mask on core 1 while the boot animation plays (first-use bakes
     * cost 50-470 ms frames in wakes and builds) */
    if (dot_prebake_begin()) xTaskCreatePinnedToCore(dotbake_task, "dotbake", 3072, NULL, 1, NULL, 1);
    wake_service_init(nos_prefs.raise_wake);

    s_frame_tick = xSemaphoreCreateBinary();
    const esp_timer_create_args_t targs = { .callback = pace_cb, .name = "pace" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_frame_timer));
    ESP_LOGI(TAG, "NOS up. internal free %u, psram free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("READY,{\"os\":\"NOS\",\"fw\":\"%s\",\"board\":\"%s\"}\n", esp_app_get_description()->version, WATCH_BOARD_NAME);

    const int64_t period = 33333;
    int64_t next = esp_timer_get_time();
    int64_t last_run = next;
    uint32_t seen_presses = 0;
    int frames_ok = 0;
    /* per-second perf */
    int pf_frames = 0, pf_late = 0; int64_t pf_work = 0, pf_max = 0, pf_t0 = next; uint32_t pf_px = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        uint32_t nowms = (uint32_t)(now / 1000);

        cmd_line line;
        if (xQueueReceive(s_cmd_q, &line, 0) == pdTRUE) { if (s_pwr != PWR_ON) go_state(PWR_ON); cmd_exec(line.text); }

        /* input */
        int x = 0, y = 0, down;
        if (s_inj_active) down = inj_poll(&x, &y);
        else {
            static int px, py;
            int rx = 0, ry = 0;
            int d = board_touch_read(&rx, &ry);
            board_pointer_update(&px, &py, rx, ry, d);
            x = px; y = py; down = d;
        }
        portENTER_CRITICAL(&s_btn_mux);
        int bdown = s_btn_down; uint32_t presses = s_btn_presses;
        portEXIT_CRITICAL(&s_btn_mux);
        int btn_event = presses != seen_presses;
        seen_presses = presses;

        int raised = wake_service_tick(nowms, s_pwr != PWR_ON && nos_prefs.raise_wake);
        if (s_pwr != PWR_ON && (down || btn_event || raised)) {
            go_state(PWR_ON);
            if (down) s_swallow = 1;                 /* the waking touch never taps */
            if (btn_event || bdown) s_btn_swallow = 1; /* nor does the waking press */
        }
        if (s_swallow) { if (!down) s_swallow = 0; }
        else if (s_pwr == PWR_ON) nos_touch(x, y, down);
        if (s_btn_swallow) { if (!bdown) s_btn_swallow = 0; }
        else if (s_pwr == PWR_ON) nos_button(bdown);

        /* sleep policy */
        if (s_pwr == PWR_ON && !s_no_sleep) {
            const nos_screen *top = nos_top();
            int keep = top && (top->flags & SCR_KEEPAWAKE);
            if (!keep && !down && nos_awake_ms() > nos_prefs.sleep_s * 1000)
                go_state(nos_prefs.aod ? PWR_AOD : PWR_OFF);
        }

        if (s_pwr == PWR_OFF) { vTaskDelay(pdMS_TO_TICKS(50)); next = esp_timer_get_time(); last_run = next; continue; }
        /* AOD entry: 30 Hz frames while the face dims out and the AOD face comes up
         * (sys_home's ramp), then the 1 Hz loop */
        if (s_pwr == PWR_AOD && !s_aod_lead && !sys_home_aod_entering()) {
            static int64_t last_aod;
            if (now - last_aod >= 1000000) {
                last_aod = now;
                nos_frame_run((float)(now - last_run) / 1000.0f);
                last_run = now;
                frame_drain();
                set_level(10);
            }
            vTaskDelay(pdMS_TO_TICKS(40));
            next = esp_timer_get_time();
            continue;
        }

        auto_brightness_tick();
        /* Wi-Fi tile/setting drives the radio (BLE stays up: it is the phone link) */
        static int s_wifi_applied = -1;
        if (s_wifi_applied != nos_prefs.wifi) {
            if (!nos_prefs.wifi) esp_wifi_stop();                 /* also at boot */
            else if (s_wifi_applied >= 0) { esp_wifi_start(); esp_wifi_connect(); }
            s_wifi_applied = nos_prefs.wifi;
        }
        if (g_tz_offset_min != nos_prefs.tz_min) { g_tz_offset_min = nos_prefs.tz_min; board_apply_timezone(); }
        int64_t a = esp_timer_get_time();
        float el_ms = (float)(a - last_run) / 1000.0f;
        s_rot_us = 0; s_wait_us = 0; memset(nos_dbg_us, 0, sizeof nos_dbg_us);
        uint32_t px = nos_frame_run(el_ms);
        last_run = a;
        s_aod_lead = 0;
        int64_t ad = esp_timer_get_time();
        frame_drain();
        int64_t work = esp_timer_get_time() - a;
        if (s_trace > 0 && px) {
            s_trace--;
            printf("NOSF el=%.1f px=%u nr=%u cmds=%u rec=%u ras=%u wait=%u rot=%u drain=%u work=%u top=%s tx=%d u=%u o=%u sel=%u clr=%u\n",
                   el_ms, (unsigned)px, (unsigned)nos_last.rects, (unsigned)nos_last.cmds,
                   (unsigned)nos_last.record_us, (unsigned)nos_last.raster_us, (unsigned)s_wait_us,
                   (unsigned)s_rot_us, (unsigned)(esp_timer_get_time() - ad), (unsigned)work,
                   nos_top() ? nos_top()->name : "-", nos_transitioning(),
                   (unsigned)nos_dbg_us[0], (unsigned)nos_dbg_us[1], (unsigned)nos_dbg_us[2], (unsigned)nos_dbg_us[3]);
        }
        /* brightness after the frame is on the panel: never show a stale frame */
        /* AOD entry: the user's level while the face dims out, the AOD level once the
         * AOD face is coming up (one dim step under the nearly black frame) */
        if (s_pwr == PWR_AOD) set_level(sys_home_aod_dimming() ? user_level() : 10);
        else set_level(nos_bright_override >= 0 ? nos_bright_override : user_level());
        if (++frames_ok == 90) ota_mark_valid();

        pf_frames++; pf_work += work; pf_px += px;
        if (work > pf_max) pf_max = work;
        if (work > period) pf_late++;
        if (s_perf && a - pf_t0 >= 1000000) {
            printf("NOSPERF fps=%d avg_ms=%.1f max_ms=%.1f late=%d px=%u depth=%d top=%s\n",
                   pf_frames, pf_work / 1000.0 / pf_frames, pf_max / 1000.0, pf_late,
                   (unsigned)(pf_px / (pf_frames ? pf_frames : 1)), nos_depth(), nos_top() ? nos_top()->name : "-");
            pf_frames = pf_late = 0; pf_work = pf_max = 0; pf_px = 0; pf_t0 = a;
        }
        if (!s_perf && a - pf_t0 >= 1000000) { pf_frames = pf_late = 0; pf_work = pf_max = 0; pf_px = 0; pf_t0 = a; }

        /* fixed 30 Hz cadence: sleep to the next deadline; if late, realign */
        next += period;
        int64_t left = next - esp_timer_get_time();
        if (left > 200) {
            esp_timer_start_once(s_frame_timer, (uint64_t)left);
            xSemaphoreTake(s_frame_tick, portMAX_DELAY);
        } else if (left < -period) next = esp_timer_get_time();   /* very late: realign, don't burst */
    }
}
