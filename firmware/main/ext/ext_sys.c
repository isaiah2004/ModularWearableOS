/* ext_sys.c — strap-module bring-up, the single "ext" service task, presence,
 * side button, memory info and the `ext` serial command.
 *
 * Task model (see docs/MODULES.md):
 *   "ext"   priority 0 (below the UI main task, which runs at 1), no core
 *           affinity, 4 KB stack in PSRAM. Services every slow sensor at its own
 *           rate: each module's *_service() returns how long it can sleep and
 *           the task sleeps for the minimum (capped at 50 ms).
 *   "exta"  (ext_audio.c) the I2S pump; only it touches I2S, it blocks on DMA.
 *   "extf"  (ext_audio.c) bounded audio requests/rings; all audio SD I/O.
 *   Haptics run from an esp_timer one-shot chain (ext_haptic.c), no task.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include "ext_priv.h"
#include "../sleep_grid.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"

portMUX_TYPE ext_lock = portMUX_INITIALIZER_UNLOCKED;
uint32_t ext_present_bits;
static TaskHandle_t s_ext_task;
static uint32_t s_loops;
static int s_display_mode = 2;
static int s_shutdown_requested, s_shutdown_status, s_shutdown_cancel;
static uint32_t s_shutdown_started, s_shutdown_done, s_shutdown_errors, s_shutdown_next;
static uint32_t s_loop_last, s_loop_gap_max, s_loop_work_max;

int ext_shutdown_requested(void) { return __atomic_load_n(&s_shutdown_requested,__ATOMIC_ACQUIRE); }
void hal_ext_shutdown_begin(void)
{
    portENTER_CRITICAL(&ext_lock);
    __atomic_store_n(&s_shutdown_cancel,0,__ATOMIC_RELEASE);
    if(!s_shutdown_requested) {
        s_shutdown_started=ext_ms();s_shutdown_done=s_shutdown_errors=0;s_shutdown_status=0;s_shutdown_next=s_shutdown_started;
        __atomic_store_n(&s_shutdown_requested,1,__ATOMIC_RELEASE);
    }
#if !EXT_BOARD_OK
    s_shutdown_status=1;
#else
    if(!s_ext_task)s_shutdown_status=ext_present_bits?-1:1;
#endif
    portEXIT_CRITICAL(&ext_lock);
    if(s_ext_task)xTaskNotifyGive(s_ext_task);
}
void hal_ext_shutdown_cancel(void)
{
    portENTER_CRITICAL(&ext_lock);
    if(s_shutdown_requested) {
        s_shutdown_status=0;
        if(s_ext_task)__atomic_store_n(&s_shutdown_cancel,1,__ATOMIC_RELEASE);
        else __atomic_store_n(&s_shutdown_requested,0,__ATOMIC_RELEASE);
    }
    portEXIT_CRITICAL(&ext_lock);
    if(s_ext_task)xTaskNotifyGive(s_ext_task);
}
static void shutdown_cancel_service(void)
{
    /* Only ext runs hooks, so no hook can publish an old ACK after this release.
     * Module owners keep pending closes/drains and observed hardware state. */
    portENTER_CRITICAL(&ext_lock);
    if(s_shutdown_cancel) {
        s_shutdown_status=0;s_shutdown_done=s_shutdown_errors=0;
        __atomic_store_n(&s_shutdown_requested,0,__ATOMIC_RELEASE);
        __atomic_store_n(&s_shutdown_cancel,0,__ATOMIC_RELEASE);
    }
    portEXIT_CRITICAL(&ext_lock);
}
int hal_ext_shutdown_poll(void)
{
    int status;
    portENTER_CRITICAL(&ext_lock);status=s_shutdown_status;portEXIT_CRITICAL(&ext_lock);
    return ext_shutdown_requested()?status:0;
}
static void shutdown_service(uint32_t now)
{
    if(__atomic_load_n(&s_shutdown_cancel,__ATOMIC_ACQUIRE))return;
    if(!ext_due(now,s_shutdown_next))return;
    s_shutdown_next=now+100;
    uint32_t errors=0;
#define PARK(bits,fn) do { if((s_shutdown_done&(bits))!=(bits)) { int result=(fn)(ext_ms()); \
    if(__atomic_load_n(&s_shutdown_cancel,__ATOMIC_ACQUIRE))return; \
    if(result>0)s_shutdown_done|=(bits);else if(result<0)errors|=(bits); } }while(0)
    PARK(EXT_HAPTIC,haptic_shutdown_poll);
    PARK(EXT_MIC|EXT_SPEAKER,audio_shutdown_poll);
    PARK(EXT_GPS,gps_shutdown_poll);
    PARK(EXT_LTE,lte_shutdown_poll);
    PARK(EXT_ENV,env_shutdown_poll);
    PARK(EXT_LIGHT,light_shutdown_poll);
    PARK(EXT_MAG,mag_shutdown_poll);
    PARK(EXT_HR,hr_shutdown_poll);
#undef PARK
    int status=(s_shutdown_done&0x1ffu)==0x1ffu?1:
               errors||ext_elapsed(ext_ms(),s_shutdown_started)>30000?-1:0;
    portENTER_CRITICAL(&ext_lock);
    if(s_shutdown_cancel){portEXIT_CRITICAL(&ext_lock);return;}
    int changed=status!=s_shutdown_status||errors!=s_shutdown_errors;
    s_shutdown_status=status;s_shutdown_errors=errors;
    portEXIT_CRITICAL(&ext_lock);
    if(changed)ESP_LOGI("ext","shutdown status=%d done=0x%03x pending=0x%03x errors=0x%03x elapsed_ms=%u",
                       status,(unsigned)s_shutdown_done,(unsigned)(0x1ffu&~s_shutdown_done),
                       (unsigned)errors,(unsigned)ext_elapsed(ext_ms(),s_shutdown_started));
}

void *ext_psram_calloc_owned(size_t n, const char *owner)
{
    void *p = nos_mem_alloc((uint32_t)n, owner);
    if (p) memset(p, 0, n);
    return p;
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
/* Display not ON: nothing here is interactive, so idle modules may sleep up to a second
 * (every service still returns its own earlier deadline) and the wake lands on the shared
 * light-sleep grid. hal_ext_kick() ends the wait when the display turns ON. */
void hal_ext_kick(void)
{
    if(s_ext_task)xTaskNotifyGive(s_ext_task);
}
static void ext_task(void *arg)
{
    (void)arg;
    for (;;) {
        int interactive = hal_display_mode() == 2;
        uint32_t now = ext_ms(), wait = interactive ? 50 : 1000, w;
        uint32_t gap=s_loop_last?ext_elapsed(now,s_loop_last):0;s_loop_last=now;
        if(gap>s_loop_gap_max)s_loop_gap_max=gap;
        if(__atomic_load_n(&s_shutdown_cancel,__ATOMIC_ACQUIRE))shutdown_cancel_service();
        else if(ext_shutdown_requested())shutdown_service(now);
        else {
            w = gps_service(ext_ms());   if (w < wait) wait = w;
            w = lte_service(ext_ms());   if (w < wait) wait = w;
            w = env_service(ext_ms());   if (w < wait) wait = w;
            w = light_service(ext_ms()); if (w < wait) wait = w;
            w = mag_service(ext_ms());   if (w < wait) wait = w;
            w = hr_service(ext_ms());    if (w < wait) wait = w;
        }
        s_loops++;
        uint32_t spent = ext_elapsed(ext_ms(),now);
        if(spent>s_loop_work_max)s_loop_work_max=spent;
        wait = wait > spent ? wait - spent : 1;
        if (!wait) wait = 1;
        if (!interactive) wait = sleep_grid_delay_ms(sleep_grid_now_ms(), wait);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));
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
    nos_mem_external("ext_task_stack", 4096, s_ext_task != NULL);
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
__attribute__((weak)) int hal_display_mode(void) { return s_display_mode; }

/* ---------------------------------------------------------------- serial */
#define EXT_DEBUG_STOPPING() ext_shutdown_requested()
#define EXT_DEBUG_LTE(args) lte_debug_request(args)
#define EXT_DEBUG_MAG(args) mag_debug_request(args)
#include "ext_debug_commands.inc"
int hal_ext_debug_command(const char *cmd)
{
    int accepted=ext_debug_dispatch(cmd);if(accepted>=0)return accepted;
    /* Touch the stream getters so `ext ... on` followed by repeated `ext` keeps the
     * stream alive past NOS_EXT_IDLE_MS (exactly as a UI screen would). */
    hal_gps_get(NULL); hal_lte_get(NULL); hal_hr_get(NULL); hal_mag_get(NULL);
    uint32_t ps, in; hal_mem_info(&ps, &in);
    printf("EXT board_ok=%d present=0x%03x loops=%u psram_free=%u internal_free=%u display_mode=%d\n",
           EXT_BOARD_OK, (unsigned)ext_present_bits, (unsigned)s_loops, (unsigned)ps, (unsigned)in,
           hal_display_mode());
    printf("EXT scheduling loop_gap_max_ms=%u loop_work_max_ms=%u shutdown=%d done=0x%03x errors=0x%03x\n",
           (unsigned)s_loop_gap_max,(unsigned)s_loop_work_max,hal_ext_shutdown_poll(),
           (unsigned)s_shutdown_done,(unsigned)s_shutdown_errors);
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
    return 1;
}
void hal_ext_debug(const char *cmd) { (void)hal_ext_debug_command(cmd); }
