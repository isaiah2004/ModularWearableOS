/* ext_sys.c — strap-module bring-up, the single "ext" service task, presence,
 * side button, memory info and the `ext` serial command.
 *
 * Task model (see docs/MODULES.md):
 *   "ext"   priority 0 (below the UI main task, which runs at 1), no core
 *           affinity, 4 KB stack in PSRAM. Services every slow sensor at its own
 *           rate: each module's *_service() returns how long it can sleep and
 *           the task sleeps for the minimum (capped at 50 ms).
 *   "exta"  (ext_audio.c) the I2S pump; only it touches I2S, it blocks on DMA.
 *   Haptics run from an esp_timer one-shot chain (ext_haptic.c), no task.
 */
#include <stdio.h>
#include <string.h>
#include "ext_priv.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"

portMUX_TYPE ext_lock = portMUX_INITIALIZER_UNLOCKED;
uint32_t ext_present_bits;
static TaskHandle_t s_ext_task;
static uint32_t s_loops;
static int s_display_mode = 2;

void *ext_psram_calloc(size_t n)
{
    return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* ---------------------------------------------------------------- I2C */
int ext_i2c_open(uint8_t addr, i2c_master_dev_handle_t *dev)
{
    i2c_master_bus_handle_t bus = board_i2c_bus();
    *dev = NULL;
    if (!bus || i2c_master_probe(bus, addr, EXT_I2C_TMO_MS) != ESP_OK) return 0;
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                .device_address = addr, .scl_speed_hz = EXT_I2C_HZ };
    return i2c_master_bus_add_device(bus, &cfg, dev) == ESP_OK;
}
int ext_rd(i2c_master_dev_handle_t d, uint8_t reg, void *out, size_t n)
{
    return d && i2c_master_transmit_receive(d, &reg, 1, out, n, EXT_I2C_TMO_MS) == ESP_OK;
}
int ext_wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return d && i2c_master_transmit(d, b, 2, EXT_I2C_TMO_MS) == ESP_OK;
}
int ext_wr_buf(i2c_master_dev_handle_t d, uint8_t reg, const uint8_t *v, size_t n)
{
    uint8_t b[8];
    if (!d || n > sizeof b - 1) return 0;
    b[0] = reg; memcpy(b + 1, v, n);
    return i2c_master_transmit(d, b, n + 1, EXT_I2C_TMO_MS) == ESP_OK;
}

/* ---------------------------------------------------------------- task */
static void ext_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t now = ext_ms(), wait = 50, w;
        w = audio_service(now); if (w < wait) wait = w;
        w = gps_service(now);   if (w < wait) wait = w;
        w = lte_service(now);   if (w < wait) wait = w;
        w = env_service(now);   if (w < wait) wait = w;
        w = light_service(now); if (w < wait) wait = w;
        w = mag_service(now);   if (w < wait) wait = w;
        w = hr_service(now);    if (w < wait) wait = w;
        s_loops++;
        uint32_t spent = ext_ms() - now;
        wait = wait > spent ? wait - spent : 1;
        vTaskDelay(pdMS_TO_TICKS(wait ? wait : 1));
    }
}

void hal_ext_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    gpio_config_t btn = { .pin_bit_mask = 1ULL << EXT_PIN_BUTTON, .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&btn);
#if EXT_BOARD_OK
    haptic_init();
    audio_init();
    env_init();
    light_init();
    mag_init();
    hr_init();
    gps_init();     /* UART probes run asynchronously inside the task */
    lte_init();
    if (xTaskCreatePinnedToCoreWithCaps(ext_task, "ext", 4096, NULL, 0, &s_ext_task,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW("ext", "ext task not created; strap modules inactive");
        s_ext_task = NULL;
    }
    ESP_LOGI("ext", "strap modules: present=0x%03x (gps/lte probe in background)",
             (unsigned)ext_present_bits);
#endif
}

uint32_t hal_ext_present(void)
{
    return ext_present_bits;
}

int hal_button_down(void)
{
    return gpio_get_level(EXT_PIN_BUTTON) == 0;
}

void hal_mem_info(uint32_t *psram_free, uint32_t *internal_free)
{
    if (psram_free) *psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (internal_free) *internal_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

/* The main firmware owns the panel; it overrides this. */
__attribute__((weak)) void hal_display_power(int mode)
{
    s_display_mode = mode;
}

/* ---------------------------------------------------------------- serial */
void hal_ext_debug(const char *cmd)
{
    if (!cmd) cmd = "";
    while (*cmd == ' ') cmd++;
    if (!strncmp(cmd, "ext", 3)) cmd += 3;
    while (*cmd == ' ') cmd++;
    if (!strncmp(cmd, "haptic ", 7)) { hal_haptic(cmd[7] - '0'); return; }
    if (!strncmp(cmd, "sfx ", 4))    { int id = 0; sscanf(cmd + 4, "%d", &id); hal_sfx(id); return; }
    if (!strncmp(cmd, "vol ", 4))    { int v = 0; sscanf(cmd + 4, "%d", &v); hal_audio_volume(v); return; }
    if (!strncmp(cmd, "play ", 5))   { printf("EXT play %d\n", hal_audio_play_file(cmd + 5)); return; }
    if (!strncmp(cmd, "rec ", 4))    { printf("EXT rec %d\n", hal_rec_start(cmd + 4)); return; }
    if (!strcmp(cmd, "recstop"))     { hal_rec_stop(); return; }
    if (!strcmp(cmd, "gps on"))      { hal_gps_enable(1); return; }
    if (!strcmp(cmd, "gps off"))     { hal_gps_enable(0); return; }
    if (!strcmp(cmd, "lte on"))      { hal_lte_enable(1); return; }
    if (!strcmp(cmd, "lte off"))     { hal_lte_enable(0); return; }
    if (!strncmp(cmd, "lte ", 4))    { lte_debug_cmd(cmd + 4); return; }
    if (!strcmp(cmd, "hr on"))       { hal_hr_enable(1); return; }
    if (!strcmp(cmd, "hr off"))      { hal_hr_enable(0); return; }
    if (!strcmp(cmd, "gest on"))     { hal_light_gesture_enable(1); return; }
    if (!strcmp(cmd, "gest off"))    { hal_light_gesture_enable(0); return; }
    if (!strncmp(cmd, "mag ", 4))    { mag_debug_cmd(cmd + 4); return; }
    if (!strncmp(cmd, "sea ", 4))    { float p = 1013.25f; sscanf(cmd + 4, "%f", &p); hal_env_set_sea_level(p); return; }
    if (*cmd && strcmp(cmd, "status")) {
        printf("EXT cmds: ext | haptic <0-6> | sfx <0-13> | vol <pct> | play <path> | rec <path> | recstop |"
               " gps on/off | lte on/off | lte at <cmd> | lte sms <num> <text> | hr on/off | gest on/off |"
               " mag cal/done/map <+x+y+z> <fwd> | sea <hpa>\n");
        return;
    }
    /* Touch the stream getters so `ext ... on` followed by repeated `ext` keeps the
     * stream alive past NOS_EXT_IDLE_MS (exactly as a UI screen would). */
    hal_gps_get(NULL); hal_lte_get(NULL); hal_hr_get(NULL); hal_mag_get(NULL);
    uint32_t ps, in; hal_mem_info(&ps, &in);
    printf("EXT board_ok=%d present=0x%03x loops=%u psram_free=%u internal_free=%u display_mode=%d\n",
           EXT_BOARD_OK, (unsigned)ext_present_bits, (unsigned)s_loops, (unsigned)ps, (unsigned)in,
           s_display_mode);
    printf("EXT stack_free ext=%u exta=%u (bytes)\n",
           s_ext_task ? (unsigned)uxTaskGetStackHighWaterMark(s_ext_task) : 0u,
           (unsigned)audio_stack_free());
    printf("EXT button=%d\n", hal_button_down());
#if EXT_BOARD_OK
    haptic_debug();
    audio_debug();
    gps_debug();
    lte_debug();
    env_debug();
    light_debug();
    mag_debug();
    hr_debug();
#endif
}
