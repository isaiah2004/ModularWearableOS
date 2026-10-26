/* Board services for Waveshare 1.43 / 1.43-B, or the 1.43C (CST820 touch, no
 * IMU, RTC or SD). Register settings and wiring follow the official demos;
 * modern IDF I2C master API used throughout. */
#include "board.h"
#include "workout.h"
#include "wrist_detect.h"
#include <stdatomic.h>
#include "board_protocol.h"
#include "pins.h"
#include "../../core/knock.h"
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"
#include "settings.h"
#include "pedometer.h"
#include "sports.h"
#include "tuning.h"
#include "sd_xfer.h"
#include "hal_ext.h"
#include "psram.h"
#include "prefs.h"
#include "freertos/idf_additions.h"
#include "esp_rom_sys.h"
#include "nos_power.h"
#include "sleep_grid.h"

int imu_backend_active(void); /* imu_hal.c: raw recorder/stream owns the sampling cadence */

static atomic_int s_background_persistence = 1;
void board_set_background_persistence(int allowed)
{ atomic_store(&s_background_persistence, allowed != 0); }

#define TAG "board"
#define SD_ROOT "/sdcard"
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_touch, s_imu, s_rtc;
static atomic_int s_touch_ok, s_imu_ok, s_rtc_ok, s_rtc_valid;
/* Handles are allocated once and reused, including after an absent boot probe.
 * A device lock spans a whole configuration sequence or one acquisition. */
static SemaphoreHandle_t s_device_lock[BOARD_DEVICE_COUNT];
static board_device_diag s_devices[BOARD_DEVICE_COUNT];
static int64_t s_device_retry_at[BOARD_DEVICE_COUNT];
static unsigned s_device_backoff[BOARD_DEVICE_COUNT];
static atomic_int s_retry_devices;
static TaskHandle_t s_sensor_task, s_fs_task;
static void fs_kick(void);
static atomic_flag s_worker_create=ATOMIC_FLAG_INIT;
static void start_board_workers(void);
static hal_imu s_sample;
static int64_t s_sample_us;
static time_t s_rtc_pending;
static uint32_t s_rtc_generation;
static int s_rtc_dirty, s_system_trusted;
static unsigned s_rtc_failed_operation;
static esp_err_t s_rtc_last_error;
static uint32_t s_rtc_failures;
static int64_t s_rtc_retry_at;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_sd_lock;
static unsigned s_asset_users;
static SemaphoreHandle_t s_board_stopped, s_fs_stopped;
static atomic_int s_stopping;
static uint32_t s_sleep_generation, s_sensor_ack, s_fs_ack;
static int s_fs_sleep_ok, s_sleep_state;
static int s_sleep_sensor_received, s_sleep_fs_received;
static int64_t s_sleep_deadline;
static pedometer_saved s_pedometer_snapshot, s_pedometer_saved;
static uint32_t s_sensor_max_gap_us, s_fs_max_io_us;
static int64_t s_sensor_last_us, s_fs_io_started;
static sdmmc_card_t *s_card;
static esp_err_t s_sd_teardown_error;
/* Reserve before the UI/radios fragment internal RAM. The SDK uses this for
 * unaligned FAT sector I/O instead of allocating a fresh DMA sector per call.
 * The SD mutex serializes every user; it remains reserved across remounts. */
#if BOARD_HAS_SD
static void *s_sd_dma_sector;
#endif
int hal_sports_sd_lock(void)
{
    if(!s_sd_lock || xSemaphoreTake(s_sd_lock,pdMS_TO_TICKS(50))!=pdTRUE)return 0;
    if(!s_card){xSemaphoreGive(s_sd_lock);return 0;}return 1;
}
void hal_sports_sd_unlock(void) { xSemaphoreGive(s_sd_lock); }
static hal_storage s_storage = { .message = "No card mounted" };
static char s_log_path[96];
static pedometer s_pedometer;
static hal_activity s_activity;
static int s_calendar_timezone_ready, s_calendar_sample_ready;
static uint32_t s_calendar_revision;
static uint32_t s_calendar_closed_generation=1;
static pedometer_saved s_calendar_pending_base;
typedef struct {
    pedometer_saved base;
    uint32_t revision,day,closed_generation;
    int deferred;
    int64_t next_date_us;
} board_calendar_tracker;
/* Sensor-owned calendar state; the host only changes the short-lock gate. */
static board_calendar_tracker s_calendar;
void board_calendar_ready(int ready)
{
    portENTER_CRITICAL(&s_state_lock);
    if (s_calendar_timezone_ready!=(ready!=0)) {
        if (!ready) {
            s_calendar_pending_base=s_pedometer_snapshot;
            s_calendar_closed_generation++;
        }
        s_calendar_timezone_ready=ready!=0; s_calendar_revision++;
    }
    if (!ready) s_calendar_sample_ready=0;
    portEXIT_CRITICAL(&s_state_lock);
}
static uint32_t calendar_hold(void)
{
    if (!s_calendar.deferred) { s_calendar.base=s_pedometer.today; s_calendar.deferred=1; }
    s_calendar.next_date_us=0;
    return 0;
}
static uint32_t calendar_day(int64_t now)
{
    /* A protected restore may hold UI mutation admission, so the host cannot
     * refresh the gate until that transfer returns. This getter is atomic RAM
     * only; no filesystem/NVS work is performed on the sensor stack. */
    if (prefs_loading()) board_calendar_ready(0);
    portENTER_CRITICAL(&s_state_lock);
    int ready=s_calendar_timezone_ready;
    uint32_t revision=s_calendar_revision;
    uint32_t closed_generation=s_calendar_closed_generation;
    pedometer_saved pending_base=s_calendar_pending_base;
    portEXIT_CRITICAL(&s_state_lock);
    /* Observe even a close/open interval shorter than one sensor period. The
     * gate captures the last complete snapshot, so in-flight new samples are
     * retained when the resolved date differs. */
    if (closed_generation!=s_calendar.closed_generation) {
        s_calendar.base=pending_base; s_calendar.deferred=1;
        s_calendar.closed_generation=closed_generation; s_calendar.next_date_us=0;
    }
    if (!ready) return calendar_hold();
    if (revision!=s_calendar.revision || now>=s_calendar.next_date_us) {
        /* UTC may survive a soft reset before the authoritative TZ is loaded.
         * Do not let that early UTC/default-TZ date clear a persisted local day. */
        time_t epoch=time(NULL); struct tm local;
        if (epoch<1577836800 || epoch>=4102444800LL || !localtime_r(&epoch,&local)) return calendar_hold();
        uint32_t day=(uint32_t)((local.tm_year+1900)*10000+(local.tm_mon+1)*100+local.tm_mday);
        portENTER_CRITICAL(&s_state_lock);
        int unchanged=s_calendar_timezone_ready && revision==s_calendar_revision;
        portEXIT_CRITICAL(&s_state_lock);
        if (!unchanged) return calendar_hold();
        if (s_calendar.deferred && s_calendar.base.day && day!=s_calendar.base.day) {
            /* A legitimate new day discards yesterday's seed, but retains the
             * new samples accumulated while their local date was unresolved. */
            uint32_t steps=s_pedometer.today.steps>=s_calendar.base.steps ?
                           s_pedometer.today.steps-s_calendar.base.steps : 0;
            uint64_t active=s_pedometer.today.active_ms>=s_calendar.base.active_ms ?
                            s_pedometer.today.active_ms-s_calendar.base.active_ms : 0;
            s_pedometer.today=(pedometer_saved){.version=1,.day=day,.steps=steps,.active_ms=active};
        }
        s_calendar.deferred=0; s_calendar.day=day; s_calendar.revision=revision;
        s_calendar.next_date_us=now+1000000;
    }
    return s_calendar.day;
}

static atomic_int *const s_device_ok[BOARD_DEVICE_COUNT] = { &s_touch_ok, &s_imu_ok, &s_rtc_ok };
void board_retry_devices(void)
{
    atomic_store(&s_retry_devices,1);
    fs_kick();
    /* Also recover a task-creation allocation failure on explicit retry. */
    if (s_sd_lock) start_board_workers();
}
void board_device_diagnostics(board_device_diag out[BOARD_DEVICE_COUNT])
{
    if (!out) return;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    for (unsigned i=0;i<BOARD_DEVICE_COUNT;i++) {
        out[i]=s_devices[i]; out[i].available=atomic_load(s_device_ok[i]);
        out[i].retry_in_ms=s_device_retry_at[i]>now ? (uint32_t)((s_device_retry_at[i]-now+999)/1000) : 0;
    }
    portEXIT_CRITICAL(&s_state_lock);
}
static uint32_t increment_sat(uint32_t value) { return value==UINT32_MAX ? value : value+1; }
/* Called while the corresponding device mutex is held. Cached diagnostics are
 * short-lock snapshots; neither the UI nor the sensor task waits for reprobe. */
static void device_result(unsigned index, esp_err_t err)
{
    int lost=0;
    portENTER_CRITICAL(&s_state_lock);
    board_device_diag *d=&s_devices[index];
    if (err==ESP_OK) d->consecutive=0;
    else {
        d->failures=increment_sat(d->failures); d->consecutive=increment_sat(d->consecutive);
        if (!d->first_error) d->first_error=err;
        d->last_error=err;
        if (d->consecutive>=3 && atomic_exchange(s_device_ok[index],0)) {
            s_device_retry_at[index]=esp_timer_get_time()+1000000; lost=1;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (lost) ESP_LOGW(TAG,"base device %u unavailable (%s); retry scheduled",index,esp_err_to_name(err));
}
static int device_begin(unsigned index)
{
    if (!atomic_load(s_device_ok[index]) || !s_device_lock[index] ||
        xSemaphoreTake(s_device_lock[index],0)!=pdTRUE) return 0;
    if (!atomic_load(s_device_ok[index])) { xSemaphoreGive(s_device_lock[index]); return 0; }
    return 1;
}
static void device_end(unsigned index) { xSemaphoreGive(s_device_lock[index]); }

static esp_err_t rd(i2c_master_dev_handle_t dev, uint8_t reg, void *out, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, out, n, 30);
}
static _Atomic uint32_t s_touch_reads, s_touch_errors, s_touch_injected, s_touch_pending;
static _Atomic int s_touch_last_error;
void board_touch_inject_timeouts(unsigned count)
{
    atomic_store(&s_touch_pending, count > 8 ? 8 : count);
}
board_touch_diag board_touch_diagnostics(void)
{
    return (board_touch_diag) {
        atomic_load(&s_touch_reads), atomic_load(&s_touch_errors),
        atomic_load(&s_touch_injected), atomic_load(&s_touch_pending),
        atomic_load(&s_touch_last_error)
    };
}
static esp_err_t touch_rd(uint8_t reg, void *out, size_t n)
{
    uint32_t pending = atomic_load(&s_touch_pending);
    while (pending && !atomic_compare_exchange_weak(&s_touch_pending, &pending, pending - 1)) {}
    atomic_fetch_add(&s_touch_reads, 1);
    if (pending) atomic_fetch_add(&s_touch_injected, 1);
    /* Exercise the real driver timeout/logging stack on demand. A zero-tick
     * transfer can also succeed or fail on the shared bus mutex; diagnostics
     * must observe actual errors/logs before claiming that path was tested. */
    esp_err_t err = i2c_master_transmit_receive(s_touch, &reg, 1, out, n, pending ? 0 : 30);
    if (err != ESP_OK) {
        atomic_fetch_add(&s_touch_errors, 1);
        atomic_store(&s_touch_last_error, err);
    }
    return err;
}
/* knock to lock: double knocks counted by the board task (hal_knock_count) */
static knock_state s_knock;
static volatile uint32_t s_knocks;
uint32_t hal_knock_count(void) { return s_knocks; }
static volatile int s_walk_log;            /* serial "walk log 1": print the step detector, 50 Hz */
void board_walk_log(int on) { s_walk_log = on; }
static volatile int s_knock_log;           /* serial "knock log 1": print detector events */
void board_knock_log(int on) { s_knock_log = on; }
static void knock_feed_at(const hal_imu *m, uint32_t now_ms)
{
    sports_tuning tc;tuning_get(&tc);s_knock.threshold_g=tc.knock_g;s_knock.quiet_before_ms=(unsigned)tc.quiet_ms;
    static const char *const ev_name[] = { "", "first", "second", "DOUBLE", "not-quiet-before", "bad-gap",
                                           "too-long", "third", "arm-moved", "sideways" };
    float g = sqrtf(m->gx * m->gx + m->gy * m->gy + m->gz * m->gz);
    if (knock_update(&s_knock, m->ax, m->ay, m->az, g, now_ms)) s_knocks++;
    if (s_knock_log) {                         /* once a second: the strongest spike seen */
        static float mz, mxy;
        static int64_t next;
        if (s_knock.hz_now > mz) { mz = s_knock.hz_now; mxy = s_knock.hxy_now; }
        int64_t t = esp_timer_get_time();
        if (t >= next) { if (mz > 0.08f) printf("KN peak z=%.2f side=%.2f\n", mz, mxy); mz = mxy = 0; next = t + 1000000; }
    }
    if (s_knock.event) {
        float v[]={(float)s_knock.event,s_knock.ev_hz,s_knock.ev_hxy,(float)s_knock.ev_ms,g};
        sports_emit(SP_KNOCK,v,5);
        if (s_knock_log && s_knock.event < (int)(sizeof ev_name / sizeof ev_name[0]))
            printf("KN %s z=%.2f side=%.2f ms=%u gyro=%.0f\n", ev_name[s_knock.event], s_knock.ev_hz, s_knock.ev_hxy,
                   (unsigned)s_knock.ev_ms, g);
        s_knock.event = 0;
    }
}
/* The IMU runs at 500 Hz with its accel low-pass opened to ~67 Hz so a knock (a ~10 ms
 * jolt) survives for the knock detector, which takes the raw samples. Everything else
 * (raise-to-wake, steps, Diagnostics) reads s_sample, smoothed here to the ~3 Hz the chip's
 * own filter used to give (2.66 % of 112 Hz), so their thresholds see what they were tuned
 * on. */
static hal_imu s_filt;
static int64_t s_filt_us;
/* now: the sample's own time (the batch reader stamps FIFO samples after the fact). */
static hal_imu imu_take_at(const hal_imu *m, int64_t now)
{
    sports_imu(m);
    workout_motion_sample(m,(uint32_t)(now/1000));
    wrist_detect_feed(m,(uint32_t)(now/1000));
    float dt = s_filt_us ? (float)(now - s_filt_us) * 1e-6f : 0.0f;
    if (dt < 0.0f) dt = 0.0f;                                       /* stamps may trail the last read slightly */
    float k = s_filt_us && dt < 0.2f ? dt / (0.053f + dt) : 1.0f;   /* one pole at ~3 Hz */
    s_filt.ax += (m->ax - s_filt.ax) * k; s_filt.ay += (m->ay - s_filt.ay) * k; s_filt.az += (m->az - s_filt.az) * k;
    s_filt.gx += (m->gx - s_filt.gx) * k; s_filt.gy += (m->gy - s_filt.gy) * k; s_filt.gz += (m->gz - s_filt.gz) * k;
    s_filt_us = now;
    portENTER_CRITICAL(&s_state_lock);
    s_sample = s_filt; s_sample_us = now;
    portEXIT_CRITICAL(&s_state_lock);
    knock_feed_at(m, (uint32_t)(now / 1000));
    return s_filt;
}
static hal_imu imu_take(const hal_imu *m) { return imu_take_at(m, esp_timer_get_time()); }
/* between the 50 Hz cycles: two more reads (a knock lasts a few ms) */
static void knock_poll(void)
{
    hal_imu m;
    if (!board_imu_raw(&m)) return;
    imu_take(&m);
}
static esp_err_t wr(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t b[] = {reg, value};
    return i2c_master_transmit(dev, b, sizeof b, 30);
}
static esp_err_t device(uint8_t addr, i2c_master_dev_handle_t *dev)
{
    i2c_device_config_t cfg = { .device_address = addr, .scl_speed_hz = 400000 };
    if (!*dev) {
        esp_err_t err=i2c_master_bus_add_device(s_bus, &cfg, dev);
        if (err!=ESP_OK) return err;
    }
    return i2c_master_probe(s_bus, addr, 30);
}
#if CONFIG_WATCH_BOARD_143C
/* The CST820 auto-sleeps and NAKs while idle, so a failed boot probe or read
 * never disables touch. After a failure, I2C is retried only once the chip
 * signals activity on TP_INT (falling edge), so an idle bus logs no errors. */
static volatile int s_touch_irq;
static void touch_isr(void *arg) { (void)arg; s_touch_irq = 1; }
int board_touch_read(int *x, int *y)
{
    uint8_t r[5];
    if (!s_touch) return BOARD_TOUCH_INVALID;
    if (!s_touch_ok) {
        if (!s_touch_irq && gpio_get_level(PIN_TP_INT)) return BOARD_TOUCH_INVALID;
        s_touch_irq = 0;
        board_retry_devices();
        return BOARD_TOUCH_INVALID;
    }
    if (!device_begin(BOARD_DEVICE_TOUCH)) return BOARD_TOUCH_INVALID;
    esp_err_t err=touch_rd(0x02,r,sizeof r);
    int result=err==ESP_OK ? board_touch_decode(r,x,y) : BOARD_TOUCH_INVALID;
    device_result(BOARD_DEVICE_TOUCH,err==ESP_OK && result<0 ? ESP_ERR_INVALID_RESPONSE : err);
    device_end(BOARD_DEVICE_TOUCH);
    return result;
}
static void touch_init(void)
{
    gpio_config_t rst = { .pin_bit_mask = 1ULL << PIN_TP_RST, .mode = GPIO_MODE_OUTPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE };
    ESP_ERROR_CHECK(gpio_config(&rst));
    /* Demo reset: high, low, high with 200 ms steps; probe right after it. */
    gpio_set_level(PIN_TP_RST, 1); vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(PIN_TP_RST, 0); vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(PIN_TP_RST, 1); vTaskDelay(pdMS_TO_TICKS(200));
    gpio_config_t irq = { .pin_bit_mask = 1ULL << PIN_TP_INT, .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE };
    ESP_ERROR_CHECK(gpio_config(&irq));
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) gpio_isr_handler_add(PIN_TP_INT, touch_isr, NULL);
    /* FT-style registers 0x02..0x06; no mode write (reg 0x00 is not FT here). */
    /* The common retry path adds/probes the stable handle after GPIO setup. */
}
#else
int board_touch_read(int *x, int *y)
{
    uint8_t r[5];
    if (!device_begin(BOARD_DEVICE_TOUCH)) return BOARD_TOUCH_INVALID;
    esp_err_t err=touch_rd(0x02,r,sizeof r);
    int result=err==ESP_OK ? board_touch_decode(r,x,y) : BOARD_TOUCH_INVALID;
    device_result(BOARD_DEVICE_TOUCH,err==ESP_OK && result<0 ? ESP_ERR_INVALID_RESPONSE : err);
    device_end(BOARD_DEVICE_TOUCH);
    return result;
}
#endif
int board_imu_read(hal_imu *out)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    *out = s_sample;
    int valid = s_sample_us > 0 && now - s_sample_us < 250000;
    portEXIT_CRITICAL(&s_state_lock);
    return valid;
}
int board_imu_available(void) { return s_imu_ok; }
int board_imu_raw(hal_imu *out)
{
    uint8_t raw[12];
    if (!out || !device_begin(BOARD_DEVICE_IMU)) return 0;
    esp_err_t err=rd(s_imu,0x35,raw,sizeof raw);
    device_result(BOARD_DEVICE_IMU,err);
    device_end(BOARD_DEVICE_IMU);
    if (err!=ESP_OK) return 0;
    board_imu_decode(raw,out); return 1;
}
/* ---- QMI8658 batch mode (display not ON) ---------------------------------------------
 * ON keeps the legacy sampling above (three register reads per 20 ms). While the display is
 * OFF/AOD/SENSOR the chip streams accel+gyro at 112.1 Hz (ODR code 6) into its 128-sample FIFO
 * and the board task drains it every ~50 ms, so the CPU can light-sleep in between. Samples are
 * stamped after the fact (board_fifo_stamp) and fed to the same consumers as before. If the FIFO
 * path cannot be made to work the task falls back to one register read per 50 ms (IMU_MODE_POLL)
 * and says so: knock detection is degraded there. docs/LIGHT_SLEEP.md. */
enum { IMU_MODE_LEGACY = 0, IMU_MODE_FIFO = 1, IMU_MODE_POLL = 2 };
#define IMU_FIFO_PERIOD_US 8921          /* ODR code 6 = 112.1 Hz */
#define IMU_FIFO_CTRL_STREAM_128 0x0e    /* size 128 samples, stream mode, FIFO_RD_MODE clear */
#define IMU_BATCH_MAX 64u                /* samples per drain: one session reads what the FIFO holds */
#define IMU_FIFO_SETTLE_US 120000        /* gyro start-up after the sensors are re-enabled */
#define IMU_FIFO_EMPTY_LIMIT 12          /* 12 empty drains (~0.6 s, ~67 samples expected) = a strike */
#define IMU_FIFO_STRIKE_LIMIT 3
static int s_imu_mode;                   /* register set applied to the chip: guarded by the IMU device lock */
static volatile int s_imu_degraded;      /* FIFO gave up: POLL mode until reboot */
static board_fifo_clock s_fifo_clock;
static int64_t s_fifo_settle_until_us;
static unsigned s_fifo_empty, s_fifo_strikes, s_fifo_batches, s_fifo_samples_total, s_fifo_overflows, s_fifo_errors;
int board_imu_batch_state(void) { return s_imu_degraded ? IMU_MODE_POLL : s_imu_mode; }
/* CTRL9 host command with the CmdDone handshake (STATUSINT bit 7). IMU device lock held. */
static esp_err_t imu_ctrl9(uint8_t cmd)
{
    esp_err_t err=wr(s_imu,0x0a,cmd), result=ESP_ERR_TIMEOUT;
    if (err!=ESP_OK) return err;
    for (int i=0;i<30 && result==ESP_ERR_TIMEOUT;i++) {
        uint8_t status=0;
        err=rd(s_imu,0x2d,&status,1);
        if (err!=ESP_OK) {result=err;break;}
        if (status&0x80) result=ESP_OK; else esp_rom_delay_us(100);
    }
    err=wr(s_imu,0x0a,0x00);                 /* acknowledge, even after a timeout */
    for (int i=0;err==ESP_OK && i<10;i++) {  /* CmdDone drops once acknowledged */
        uint8_t status=0;
        if (rd(s_imu,0x2d,&status,1)!=ESP_OK || !(status&0x80)) break;
        esp_rom_delay_us(100);
    }
    return result!=ESP_OK ? result : err;
}
/* Sensors off, new rate/filter/FIFO set, sensors on. IMU device lock held. */
static esp_err_t imu_apply_mode(int fifo)
{
    static const uint8_t legacy[][2]={{0x08,0},{0x14,0x00},{0x03,0x24},{0x04,0x54},{0x06,0x17}};
    static const uint8_t stream[][2]={{0x08,0},{0x14,0x00},{0x03,0x26},{0x04,0x56},{0x06,0x70},{0x13,0x00},
                                      {0x14,IMU_FIFO_CTRL_STREAM_128}};
    const uint8_t (*regs)[2]=fifo?stream:legacy;
    unsigned count=fifo?sizeof stream/sizeof stream[0]:sizeof legacy/sizeof legacy[0];
    esp_err_t err=ESP_OK;
    for (unsigned i=0;err==ESP_OK && i<count;i++) err=wr(s_imu,regs[i][0],regs[i][1]);
    if (err==ESP_OK && fifo) err=imu_ctrl9(0x04);          /* RST_FIFO */
    if (err==ESP_OK) err=wr(s_imu,0x08,3);                  /* accel + gyro */
    if (err==ESP_OK) {
        s_imu_mode=fifo?IMU_MODE_FIFO:IMU_MODE_LEGACY;
        s_fifo_clock.last_us=0;s_fifo_empty=0;
        s_fifo_settle_until_us=esp_timer_get_time()+IMU_FIFO_SETTLE_US;
    }
    return err;
}
/* Back to the legacy register set for the ON display (and for POLL fallback). */
static void imu_leave_fifo(void)
{
    if (s_imu_mode!=IMU_MODE_FIFO || !device_begin(BOARD_DEVICE_IMU)) return;
    esp_err_t err=s_imu_mode==IMU_MODE_FIFO?imu_apply_mode(0):ESP_OK;
    device_result(BOARD_DEVICE_IMU,err);
    device_end(BOARD_DEVICE_IMU);
    if (err==ESP_OK) vTaskDelay(pdMS_TO_TICKS(60)); /* let the gyro settle before the first legacy read */
}
/* Drain the FIFO into raw[] (12 bytes per sample). Returns the sample count, 0 when the FIFO is
 * empty or was just (re)configured, -1 when the sensor is unavailable or the bus failed. */
static int imu_fifo_drain(uint8_t *raw, unsigned max, int *overflow)
{
    *overflow=0;
    if (!device_begin(BOARD_DEVICE_IMU)) return -1;
    esp_err_t err=ESP_OK;
    unsigned n=0;
    if (s_imu_mode!=IMU_MODE_FIFO) {
        err=imu_apply_mode(1);
        if (err!=ESP_OK && ++s_fifo_strikes>=IMU_FIFO_STRIKE_LIMIT) s_imu_degraded=1;
    } else {
        uint8_t status[2];
        err=rd(s_imu,0x15,status,sizeof status);
        if (err==ESP_OK) {
            n=board_fifo_samples(status,overflow);
            if (n>max) n=max;
            if (n) {
                err=imu_ctrl9(0x05);                           /* REQ_FIFO: enter read mode */
                if (err==ESP_OK) err=rd(s_imu,0x17,raw,n*BOARD_FIFO_SAMPLE_BYTES);
                esp_err_t leave=wr(s_imu,0x14,IMU_FIFO_CTRL_STREAM_128); /* clear FIFO_RD_MODE */
                if (err==ESP_OK) err=leave;
            }
        }
    }
    if (err!=ESP_OK) s_imu_mode=IMU_MODE_LEGACY; /* an aborted read session: reset FIFO and read mode next cycle */
    device_result(BOARD_DEVICE_IMU,err);
    device_end(BOARD_DEVICE_IMU);
    if (err!=ESP_OK) {s_fifo_errors++;return -1;}
    return (int)n;
}
static esp_err_t configure_device(unsigned index)
{
    esp_err_t err;
    if (index==BOARD_DEVICE_TOUCH) {
        err=device(TOUCH_ADDR,&s_touch);
#if !CONFIG_WATCH_BOARD_143C
        if (err==ESP_OK) err=wr(s_touch,0,0);
#endif
        return err;
    }
#if BOARD_HAS_IMU
    if (index==BOARD_DEVICE_IMU) {
        err=device(IMU_ADDR,&s_imu);
        uint8_t id=0;
        if (err==ESP_OK) err=rd(s_imu,0,&id,1);
        if (err==ESP_OK && id!=0x05) err=ESP_ERR_INVALID_RESPONSE;
        /* Sensors stay unavailable through the whole configuration sequence. */
        static const uint8_t registers[][2]={{0x02,0x60},{0x08,0},{0x03,0x24},
                                           {0x04,0x54},{0x06,0x17},{0x08,3}};
        for (unsigned i=0;err==ESP_OK && i<sizeof registers/sizeof registers[0];i++)
            err=wr(s_imu,registers[i][0],registers[i][1]);
        s_imu_mode=IMU_MODE_LEGACY; /* the chip is back on the legacy register set, FIFO off */
        return err;
    }
#endif
#if BOARD_HAS_RTC
    if (index==BOARD_DEVICE_RTC) return device(RTC_ADDR,&s_rtc);
#endif
    return ESP_ERR_NOT_SUPPORTED;
}
static void retry_devices(int initial)
{
    int force=atomic_exchange(&s_retry_devices,0);
    int64_t now=esp_timer_get_time();
    for (unsigned i=0;i<BOARD_DEVICE_COUNT;i++) {
        if ((i==BOARD_DEVICE_IMU && !BOARD_HAS_IMU) ||
            (i==BOARD_DEVICE_RTC && !BOARD_HAS_RTC) || atomic_load(s_device_ok[i])) continue;
        portENTER_CRITICAL(&s_state_lock);
        int due=initial || force || now>=s_device_retry_at[i];
        portEXIT_CRITICAL(&s_state_lock);
        if (!due || xSemaphoreTake(s_device_lock[i],0)!=pdTRUE) continue;
#if CONFIG_WATCH_BOARD_143C
        /* CST820 sleeps/NAKs at idle. An IRQ or explicit retry wakes acquisition;
         * do not generate periodic bus errors just to rediscover its sleep. */
        if (i==BOARD_DEVICE_TOUCH && !initial && !force && !s_touch_irq && gpio_get_level(PIN_TP_INT)) {
            portENTER_CRITICAL(&s_state_lock); s_device_retry_at[i]=now+30000000;
            portEXIT_CRITICAL(&s_state_lock); device_end(i); continue;
        }
        if (i==BOARD_DEVICE_TOUCH) s_touch_irq=0;
#endif
        esp_err_t err=configure_device(i);
        portENTER_CRITICAL(&s_state_lock);
        board_device_diag *d=&s_devices[i];
        int first=d->attempts==0;
        d->attempts=increment_sat(d->attempts);
        if (err==ESP_OK) {
            atomic_store(s_device_ok[i],1); d->consecutive=0;
            s_device_backoff[i]=0; s_device_retry_at[i]=0;
        } else {
            d->failures=increment_sat(d->failures); d->consecutive=increment_sat(d->consecutive);
            if (!d->first_error) d->first_error=err;
            d->last_error=err;
            if (force) s_device_backoff[i]=0;
            unsigned delay=s_device_backoff[i]<4 ? 1u<<s_device_backoff[i] : 30u;
#if !CONFIG_WATCH_BOARD_143C
            /* FT3168 has no routed IRQ on the base board. Monitor-mode NAKs
             * must not leave touch acquisition gated off for 8--30 seconds:
             * a physical touch cannot request an earlier retry from the MCU. */
            if(i==BOARD_DEVICE_TOUCH)delay=1;
#endif
            if (s_device_backoff[i]<4) s_device_backoff[i]++;
            s_device_retry_at[i]=now+(int64_t)delay*1000000;
        }
        portEXIT_CRITICAL(&s_state_lock);
        device_end(i);
        if (err==ESP_OK) ESP_LOGI(TAG,"base device %u %s",i,first?"ready":"recovered");
        else if (first) ESP_LOGW(TAG,"base device %u probe %s; bounded retry",i,esp_err_to_name(err));
    }
}
int hal_imu_sample(hal_imu *out,uint32_t *sample_ms)
{
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    *out=s_sample;*sample_ms=(uint32_t)(s_sample_us/1000);
    int valid=s_sample_us>0 && now-s_sample_us<150000;
    portEXIT_CRITICAL(&s_state_lock);
    return valid;
}
void hal_activity_get(hal_activity *out)
{
    if(!out) return;
    int64_t now=esp_timer_get_time();
    int loading=prefs_loading();
    portENTER_CRITICAL(&s_state_lock);
    *out=s_activity;
    if(loading || !s_calendar_timezone_ready || !s_calendar_sample_ready || s_sample_us<=0 || now-s_sample_us>=250000) {
        out->valid=0; out->walking=0;
    }
    portEXIT_CRITICAL(&s_state_lock);
}
static atomic_int s_sd_retry;
void hal_storage_retry(void) { atomic_store(&s_sd_retry,1); fs_kick(); }
static hal_flash_info s_flash;
void hal_flash_info_get(hal_flash_info *out)
{
    portENTER_CRITICAL(&s_state_lock);
    *out = s_flash;
    portEXIT_CRITICAL(&s_state_lock);
}
/* Once, on the INTERNAL-stack filesystem worker (also queries NVS). */
static void flash_info_fill(void)
{
    hal_flash_info f = { 0 };
    esp_flash_get_size(NULL, &f.flash_bytes);
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run) {
        f.app_slot_bytes = run->size;
        esp_partition_pos_t pos = { .offset = run->address, .size = run->size };
        esp_image_metadata_t md;
        if (esp_image_get_metadata(&pos, &md) == ESP_OK) f.app_bytes = md.image_len;
    }
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK) { f.nvs_used = (uint32_t)st.used_entries; f.nvs_total = (uint32_t)st.total_entries; }
    portENTER_CRITICAL(&s_state_lock);
    s_flash = f;
    portEXIT_CRITICAL(&s_state_lock);
}
void hal_storage_get(hal_storage *out)
{
    portENTER_CRITICAL(&s_state_lock);
    *out = s_storage;
    portEXIT_CRITICAL(&s_state_lock);
}
/* Caller owns SD mutex. Publish a complete snapshot for the renderer. */
static void storage_status(const char *msg)
{
    hal_storage state = { .mounted = s_card != NULL };
    if (s_card) esp_vfs_fat_info(SD_ROOT, &state.total_bytes, &state.free_bytes);
    snprintf(state.message, sizeof state.message, "%s", msg);
    portENTER_CRITICAL(&s_state_lock);
    s_storage = state;
    portEXIT_CRITICAL(&s_state_lock);
}
#if BOARD_HAS_SD
static int make_dir(const char *path)
{
    if (mkdir(path, 0775) == 0) return 1;
    struct stat st;
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
#endif
esp_err_t board_sd_mount(void)
{
#if !BOARD_HAS_SD
    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    storage_status("No SD slot on this board");
    xSemaphoreGive(s_sd_lock);
    return ESP_ERR_NOT_SUPPORTED;
#else
    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    if (s_sd_teardown_error) {
        storage_status("SD teardown incomplete; restart required");
        xSemaphoreGive(s_sd_lock); return ESP_ERR_INVALID_STATE;
    }
    if (s_card) { xSemaphoreGive(s_sd_lock); return ESP_OK; }
    /* Same one-bit SDMMC wiring as Waveshare 04_SD_Card. 20 MHz for v1. */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    if (!s_sd_dma_sector)
        s_sd_dma_sector = heap_caps_aligned_alloc(64, 512, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_sd_dma_sector) {
        storage_status("No SD DMA reserve; retry after memory recovery");
        xSemaphoreGive(s_sd_lock);
        return ESP_ERR_NO_MEM;
    }
    host.dma_aligned_buffer = s_sd_dma_sector;
    host.unaligned_multi_block_rw_max_chunk_size = 1;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1; slot.clk = PIN_SD_CLK; slot.cmd = PIN_SD_CMD; slot.d0 = PIN_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false, .max_files = 4, .allocation_unit_size = 16384,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_ROOT, &host, &slot, &cfg, &s_card);
    if (err == ESP_OK) {
        int dirs = make_dir(SD_ROOT "/watch") && make_dir(SD_ROOT "/watch/logs");
        s_log_path[0] = 0;
        storage_status(dirs ? "Ready; logs every 60s" : "Mounted; cannot create logs");
        sdmmc_card_print_info(stdout, s_card);
    } else {
        s_card = NULL;
        /* say which: no card answering vs a card whose filesystem we can't read
         * (ESP_FAIL = no FAT volume, e.g. an exFAT card: reformat it as FAT32) */
        storage_status(err == ESP_FAIL ? "Card not FAT32 (exFAT?)" :
                       err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND ? "No card found" :
                       esp_err_to_name(err));
        ESP_LOGW(TAG, "SD mount: %s; continuing without card", esp_err_to_name(err));
    }
    xSemaphoreGive(s_sd_lock);
    /* Recovery obtains its own mount lease and SD lock. Never run it under the
     * mount mutex: that would deadlock and block all settings/checkpoint users. */
    if (err == ESP_OK && !sd_xfer_recover())
        ESP_LOGW(TAG, "SD transfer recovery pending; preserved files remain readable");
    return err;
#endif
}
int board_sd_asset_acquire(void) {
    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    int ok = s_card != NULL && !s_stopping;
    if (ok)
        s_asset_users++;
    xSemaphoreGive(s_sd_lock);
    return ok;
}
void board_sd_asset_release(void) {
    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    assert(s_asset_users);
    s_asset_users--;
    xSemaphoreGive(s_sd_lock);
}
void board_sd_asset_lock(void) { xSemaphoreTake(s_sd_lock, portMAX_DELAY); }
void board_sd_asset_unlock(void) { xSemaphoreGive(s_sd_lock); }
esp_err_t board_sd_unmount(void) {
    hal_imu_record rec;
    hal_imu_record_get(&rec);
    hal_imu_file file;
    hal_imu_file_get(&file);
    if ((rec.state != HAL_IMU_IDLE && rec.state != HAL_IMU_ERROR) || file.busy)
        return ESP_ERR_INVALID_STATE;
    /* UI eject must never wait for an outstanding SD read. */
    if (xSemaphoreTake(s_sd_lock, 0) != pdTRUE)
        return ESP_ERR_INVALID_STATE;
    if (s_sd_teardown_error) {
        esp_err_t error=s_sd_teardown_error;
        xSemaphoreGive(s_sd_lock); return error;
    }
    if (s_asset_users) {
        xSemaphoreGive(s_sd_lock);
        return ESP_ERR_INVALID_STATE;
    }
    int attempted=s_card!=NULL;
    esp_err_t err = attempted ? esp_vfs_fat_sdcard_unmount(SD_ROOT, s_card) : ESP_OK;
    /* IDF frees the card/host before its final VFS unregister can report an
     * error. Never retain or remount through that potentially dangling pointer. */
    if (attempted) { s_card=NULL; s_log_path[0]=0; }
    if (err == ESP_OK) {
        s_card = NULL;
        s_log_path[0] = 0;
        storage_status("Ejected; safe to remove");
    } else {
        s_sd_teardown_error=err;
        storage_status("SD teardown incomplete; restart required");
    }
    xSemaphoreGive(s_sd_lock);
    return err;
}
/* O_EXCL ensures an existing user file is never overwritten. */
static int unique_file(char *path, size_t size, const char *dir, const char *ext)
{
    for (int i = 0; i < 10000; i++) {
        snprintf(path, size, "%s/%04d.%s", dir, i, ext);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0664);
        if (fd >= 0 || errno != EEXIST) return fd;
    }
    return -1;
}
esp_err_t board_sd_test(void)
{
    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    char path[96];
    if (s_card) {
        int fd = unique_file(path, sizeof path, SD_ROOT "/watch", "tmp");
        if (fd >= 0) {
            uint8_t pattern[256], result[256];
            for (int i = 0; i < 256; i++) pattern[i] = (uint8_t)(i ^ 0xa5);
            int ok = write(fd, pattern, sizeof pattern) == sizeof pattern;
            if (fsync(fd) != 0) ok = 0;
            if (close(fd) != 0) ok = 0;
            fd = open(path, O_RDONLY);
            if (fd < 0) ok = 0;
            else {
                if (read(fd, result, sizeof result) != sizeof result || memcmp(pattern, result, sizeof result)) ok = 0;
                if (close(fd) != 0) ok = 0;
            }
            if (unlink(path) != 0) ok = 0;
            err = ok ? ESP_OK : ESP_FAIL;
        } else err = ESP_FAIL;
    }
    storage_status(err == ESP_OK ? "SD read/write test passed" : "SD test failed / no card");
    xSemaphoreGive(s_sd_lock);
    return err;
}
/* nos_main.c / hal_esp32.c: power context for each minute row, so a discharge can be
 * attributed to screen, radio and supply state instead of guessed. */
const char *nos_power_state_name(void);
int nos_power_brightness(void), nos_power_cpu_mhz(void);
int hal_wifi_log_state(void), hal_external_power(void);
typedef struct {
    time_t epoch;
    uint64_t uptime_ms;
    int millivolts, valid;
    hal_imu imu;
    const char *screen;
    int brightness, wifi, usb, cpu_mhz;
    uint32_t sleep_ms, wakeups;    /* cumulative light sleep since boot; rows log per-minute deltas */
} board_log_snapshot;
static board_log_snapshot log_snapshot(void)
{
    board_log_snapshot value={.epoch=time(NULL),.uptime_ms=(uint64_t)esp_timer_get_time()/1000,
                              .millivolts=hal_battery_mv(),.screen=nos_power_state_name(),
                              .brightness=nos_power_brightness(),.wifi=hal_wifi_log_state(),
                              .usb=hal_external_power(),.cpu_mhz=nos_power_cpu_mhz()};
    value.valid=board_imu_read(&value.imu);
    nos_power_sleep_stats(&value.sleep_ms,&value.wakeups);
    return value;
}
static int log_sample(const board_log_snapshot *value)
{
    /* Only the filesystem worker opens files. Its immutable acquisition snapshot
     * is captured before taking SD ownership, so delayed I/O cannot redatestamp it. */
    if (xSemaphoreTake(s_sd_lock,0)!=pdTRUE) return 0;
    if (s_asset_users) { xSemaphoreGive(s_sd_lock); return 0; }
    if (!s_card) { xSemaphoreGive(s_sd_lock); return 1; }
    int failed = 0;
    if (!s_log_path[0]) {
        int fd = unique_file(s_log_path, sizeof s_log_path, SD_ROOT "/watch/logs", "csv");
        if (fd < 0) { s_log_path[0] = 0; failed = 1; }
        else {
            const char *hdr = "utc_epoch,uptime_ms,system_mv,imu_valid,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,"
                              "screen,brightness,wifi,usb,cpu_mhz,sleep_ms,wakeups\n";
            if (write(fd, hdr, strlen(hdr)) != (ssize_t)strlen(hdr)) failed = 1;
            if (close(fd) != 0) failed = 1;
        }
    }
    struct stat st;
    if (!failed && stat(s_log_path, &st) == 0 && st.st_size >= 1024 * 1024) {
        s_log_path[0] = 0; /* rotate next minute */
        xSemaphoreGive(s_sd_lock); return 1;
    }
    FILE *f = failed ? NULL : fopen(s_log_path, "a");
    if (f) {
        const hal_imu *m=&value->imu;
        static const char *const wifi[] = { "off", "on", "connected" };
        static uint32_t logged_sleep_ms, logged_wakeups;      /* totals at the last row actually written */
        uint32_t sleep_ms=value->sleep_ms-logged_sleep_ms, wakeups=value->wakeups-logged_wakeups;
        if (fprintf(f, "%lld,%llu,%d,%d,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%s,%d,%s,%s,%d,%u,%u\n",
            (long long)value->epoch, (unsigned long long)value->uptime_ms,
            value->millivolts, value->valid, m->ax, m->ay, m->az, m->gx, m->gy, m->gz,
            value->screen, value->brightness, wifi[value->wifi > 2 || value->wifi < 0 ? 1 : value->wifi],
            value->usb > 0 ? "usb" : value->usb == 0 ? "battery" : "unknown", value->cpu_mhz,
            (unsigned)sleep_ms, (unsigned)wakeups) < 0) failed = 1;
        else {logged_sleep_ms=value->sleep_ms;logged_wakeups=value->wakeups;}
        if (fclose(f) != 0) failed = 1;
    } else failed = 1;
    storage_status(failed ? "Log write failed; check card" : "Ready; logs every 60s");
    xSemaphoreGive(s_sd_lock);
    return 1;
}
void board_clock_changed(time_t epoch)
{
    portENTER_CRITICAL(&s_state_lock);
    s_rtc_pending=epoch; s_rtc_dirty=1; s_rtc_generation++;
    s_system_trusted=epoch>=946684800LL && epoch<4102444800LL;
    s_rtc_retry_at=0;
    portEXIT_CRITICAL(&s_state_lock);
}
#if BOARD_HAS_RTC
static int rtc_failure(unsigned operation, esp_err_t err)
{
    portENTER_CRITICAL(&s_state_lock);
    int report=!s_rtc_failures || operation!=s_rtc_failed_operation || err!=s_rtc_last_error;
    s_rtc_failures=increment_sat(s_rtc_failures);
    s_rtc_failed_operation=operation; s_rtc_last_error=err;
    portEXIT_CRITICAL(&s_state_lock);
    atomic_store(&s_rtc_valid,0);
    if (report) ESP_LOGW(TAG,"RTC operation %u failed: %s; request retained",operation,esp_err_to_name(err));
    return 0;
}
static int rtc_save(time_t epoch)
{
    uint8_t data[8] = {0x04};
    if (!board_rtc_encode((int64_t)epoch,data+1)) return rtc_failure(5,ESP_ERR_INVALID_ARG);
    if (!device_begin(BOARD_DEVICE_RTC)) return 0;
    /* Preserve oscillator capacitance/control bits, select 24h, stop for an
     * atomic calendar write, then restart. A partial write stays untrusted. */
    uint8_t ctrl;
    unsigned operation=1;
    esp_err_t err=rd(s_rtc,0,&ctrl,1);
    if (err!=ESP_OK) goto done;
    ctrl &= (uint8_t)~0x22;
    operation=2; err=wr(s_rtc,0,ctrl|0x20);
    int restart_safe=1;
    if (err==ESP_OK) {
        operation=3; err=i2c_master_transmit(s_rtc,data,sizeof data,30);
        if (err!=ESP_OK) restart_safe=wr(s_rtc,4,0x80)==ESP_OK;
    }
    /* Even a failed stop ACK may have stopped the oscillator; always attempt
     * restart after issuing stop, while preserving the original failure. A
     * partial calendar whose OS invalidation also failed must remain stopped:
     * reboot must not trust a mixture of old/new but individually valid fields. */
    esp_err_t restart=restart_safe ? wr(s_rtc,0,ctrl) : ESP_ERR_INVALID_STATE;
    if (err==ESP_OK && restart!=ESP_OK) { err=restart; operation=4; }
done:
    device_result(BOARD_DEVICE_RTC,err);
    device_end(BOARD_DEVICE_RTC);
    if (err!=ESP_OK) return rtc_failure(operation,err);
    atomic_store(&s_rtc_valid,1);
    return 1;
}
#endif
/* The retry uses current UTC, never the epoch captured several seconds ago.
 * A newer notification cannot be cleared by an older successful transaction. */
static int rtc_maintain(int force)
{
#if !BOARD_HAS_RTC
    (void)force;
    portENTER_CRITICAL(&s_state_lock); s_rtc_dirty=0; portEXIT_CRITICAL(&s_state_lock);
    return 1;
#else
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    int dirty=s_rtc_dirty, trusted=s_system_trusted;
    uint32_t generation=s_rtc_generation;
    int due=force || now>=s_rtc_retry_at;
    portEXIT_CRITICAL(&s_state_lock);
    if (!dirty) return 1;
    if (!due) return 0;
    int ok=trusted && rtc_save(time(NULL));
    portENTER_CRITICAL(&s_state_lock);
    int recovered=ok && generation==s_rtc_generation && s_rtc_last_error;
    if (ok && generation==s_rtc_generation) {
        s_rtc_dirty=0;
        if (s_rtc_last_error) { s_rtc_last_error=ESP_OK; s_rtc_failed_operation=0; }
    }
    if (generation==s_rtc_generation) s_rtc_retry_at=ok ? 0 : esp_timer_get_time()+5000000;
    int pending=s_rtc_dirty;
    portEXIT_CRITICAL(&s_state_lock);
    if (recovered) ESP_LOGI(TAG,"RTC synchronization recovered");
    return !pending;
#endif
}
void board_apply_timezone(void)
{
    board_calendar_ready(0);
    /* Fixed offset persisted in settings; POSIX sign is west-positive. */
    int off = g_tz_offset_min;
    int magnitude = off < 0 ? -off : off;
    char tz[24];
    snprintf(tz, sizeof tz, "UTC%c%d:%02d", off >= 0 ? '-' : '+', magnitude / 60, magnitude % 60);
    setenv("TZ", tz, 1); tzset();
}
int board_clock_valid(void)
{
#if !BOARD_HAS_RTC
    if (!s_rtc_valid && time(NULL) >= 1577836800) s_rtc_valid = 1; /* set via USB/BLE/SNTP */
#endif
    return s_rtc_valid;
}
void board_restore_clock(void)
{
    board_apply_timezone();
    uint8_t raw[7], ctrl = 0;
    int64_t epoch=0;
    int valid=0;
    if (device_begin(BOARD_DEVICE_RTC)) {
        esp_err_t err=rd(s_rtc,0,&ctrl,1);
        if (err==ESP_OK && !(ctrl&0x22)) {
            err=rd(s_rtc,4,raw,sizeof raw);
            valid=err==ESP_OK && board_rtc_decode(raw,&epoch);
        }
        device_result(BOARD_DEVICE_RTC,err);
        if (err!=ESP_OK) {
            atomic_store(&s_rtc_ok,0);
            portENTER_CRITICAL(&s_state_lock); s_device_retry_at[BOARD_DEVICE_RTC]=esp_timer_get_time()+1000000;
            portEXIT_CRITICAL(&s_state_lock);
        }
        device_end(BOARD_DEVICE_RTC);
    }
    /* Called once before radio time sources start. Later reprobes only recover
     * the device, never overwrite an intervening USB/BLE/SNTP clock update. */
    if (valid) {
        struct timeval tv = { .tv_sec = (time_t)epoch };
        if (settimeofday(&tv,NULL)==0) {
            atomic_store(&s_rtc_valid,1);
            portENTER_CRITICAL(&s_state_lock); s_system_trusted=1; portEXIT_CRITICAL(&s_state_lock);
            hal_log("RTC: restored UTC time");
        } else hal_log("RTC: system clock restore failed");
    } else if (time(NULL)>=1577836800) {
        /* A retained system clock remains useful even if this RTC read failed. */
        portENTER_CRITICAL(&s_state_lock); s_system_trusted=1; portEXIT_CRITICAL(&s_state_lock);
        if (!BOARD_HAS_RTC) atomic_store(&s_rtc_valid,1);
        else board_clock_changed(time(NULL));
        hal_log("RTC: kept retained system UTC; RTC synchronization may be pending");
    } else {
        /* Explicitly unsynchronised until USB/BLE/NTP sets it; do not seed RTC. */
        struct timeval tv = { .tv_sec = 0 };
        settimeofday(&tv, NULL);
        hal_log("RTC: unset; settime over USB or sync phone/WiFi");
    }
}
static void persistence_started(void)
{
    portENTER_CRITICAL(&s_state_lock); s_fs_io_started=esp_timer_get_time(); portEXIT_CRITICAL(&s_state_lock);
}
static void persistence_finished(void)
{
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    uint64_t duration=(uint64_t)(now-s_fs_io_started);
    uint32_t bounded=duration>UINT32_MAX ? UINT32_MAX : (uint32_t)duration;
    if (bounded>s_fs_max_io_us) s_fs_max_io_us=bounded;
    s_fs_io_started=0;
    portEXIT_CRITICAL(&s_state_lock);
}
static int save_pedometer(void)
{
    pedometer_saved snapshot, saved;
    int loading=prefs_loading();
    portENTER_CRITICAL(&s_state_lock);
    snapshot=s_pedometer_snapshot; saved=s_pedometer_saved;
    int calendar_ready=!loading && s_calendar_timezone_ready && s_calendar_sample_ready;
    portEXIT_CRITICAL(&s_state_lock);
    if (!memcmp(&snapshot,&saved,sizeof saved)) return 1;
    /* Keep the last durable known-day count until this window can be assigned
     * to a resolved local day. A failed/pending preference read must not erase it. */
    if (!calendar_ready) return 0;
    if (!hal_store_save_checked("pedometer",&snapshot,sizeof snapshot)) return 0;
    portENTER_CRITICAL(&s_state_lock); s_pedometer_saved=snapshot; portEXIT_CRITICAL(&s_state_lock);
    return 1;
}
/* Filesystem worker cadence: 100 ms while the display is ON; otherwise once a second on the
 * shared light-sleep grid. A request from another task (SD retry, device retry, RTC change)
 * notifies it, so the slow cadence only delays housekeeping, never a user action. */
static void fs_wait(void)
{
    if (hal_display_mode()==2) ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100));
    else sleep_grid_notify_wait(1000);
}
static void fs_kick(void)
{
    TaskHandle_t task=s_fs_task;
    if (task) xTaskNotifyGive(task);
}
static void board_fs_task(void *arg)
{
    (void)arg;
    int64_t log_at=esp_timer_get_time()+60000000, save_at=log_at;
    persistence_started(); flash_info_fill(); persistence_finished();
    for (;;) {
        if (atomic_load(&s_stopping)) {
            portENTER_CRITICAL(&s_state_lock);
            uint32_t generation=s_sleep_generation;
            int sensor_stopped=s_sensor_ack==generation, acknowledged=s_fs_ack==generation;
            portEXIT_CRITICAL(&s_state_lock);
            if (sensor_stopped && !acknowledged) {
                /* The sensor has published its final immutable count. Keep the
                 * descriptor/lease with this worker through every checked close. */
                persistence_started();
                int ok=save_pedometer();
                if (!rtc_maintain(1)) ok=0;
                sports_flush();
                sports_status journal; sports_get_status(&journal);
                if (journal.queued) ok=0;
                persistence_finished();
                int signal=0;
                portENTER_CRITICAL(&s_state_lock);
                if (atomic_load(&s_stopping) && s_sleep_generation==generation) {
                    s_fs_sleep_ok=ok; s_fs_ack=generation; signal=1;
                }
                portEXIT_CRITICAL(&s_state_lock);
                if (signal) xSemaphoreGive(s_fs_stopped);
            }
            vTaskDelay(pdMS_TO_TICKS(10)); continue;
        }
        persistence_started();
        retry_devices(0);
        if (atomic_exchange(&s_sd_retry,0)) board_sd_mount();
        int64_t now=esp_timer_get_time();
        if (now>=save_at && atomic_load(&s_background_persistence)) {
            int ok=save_pedometer(); save_at=esp_timer_get_time()+(ok?60000000:5000000);
        }
        rtc_maintain(0);
        if (now>=log_at) {
            board_log_snapshot snapshot=log_snapshot();
            int attempted=log_sample(&snapshot);
            log_at=esp_timer_get_time()+(attempted?60000000:1000000);
        }
        persistence_finished();
        fs_wait();
    }
}
/* One detector step for one (filtered) sample: pedometer, walk telemetry, "walk log". */
static uint32_t board_walk_step(const hal_imu *in,int valid,int64_t sample_us)
{
    const hal_imu sample=*in;
    uint32_t day=calendar_day(sample_us);
    pedometer_update(&s_pedometer,day,(uint32_t)(sample_us/1000),&sample,valid);
    sports_tuning tc;tuning_get(&tc);
    s_pedometer.walk.threshold_g=tc.walk_g;s_pedometer.walk.entry_peaks=tc.walk_peaks;
    static int previous_walk,previous_candidates,previous_valid;
    const walk_state *w=&s_pedometer.walk;
    if(w->walking!=previous_walk || w->candidates!=previous_candidates || w->valid!=previous_valid) {
        float v[]={(float)w->walking,(float)w->candidates,(float)w->valid,
            (float)s_pedometer.today.steps,w->filtered,w->peak,w->valley};sports_emit(SP_WALK,v,7);
        previous_walk=w->walking;previous_candidates=w->candidates;previous_valid=w->valid;
    }
    if (s_walk_log) {                    /* serial "walk log 1": the step detector's view, 50 Hz */
        const walk_state *w = &s_pedometer.walk;
        float mag = sqrtf(sample.ax * sample.ax + sample.ay * sample.ay + sample.az * sample.az);
        float rot = sqrtf(sample.gx * sample.gx + sample.gy * sample.gy + sample.gz * sample.gz);
        printf("WK t=%u mag=%.3f rot=%.0f base=%.3f filt=%.3f high=%d cand=%d walking=%d steps=%u valid=%d today=%u\n",
               (unsigned)(sample_us / 1000), mag, rot, w->baseline, w->filtered, w->high, w->candidates,
               w->walking, (unsigned)w->steps, w->valid, (unsigned)s_pedometer.today.steps);
    }
    return day;
}
/* Publish the pedometer state the UI reads, and track the acquisition gap. */
static void board_sensor_publish(uint32_t day,int64_t sample_us)
{
    hal_activity activity; pedometer_get(&s_pedometer,&activity);
    portENTER_CRITICAL(&s_state_lock);
    s_activity=activity;
    s_pedometer_snapshot=s_pedometer.today;
    s_calendar_sample_ready=day!=0;
    if (!s_calendar_sample_ready) s_activity.valid=0;
    if (s_sensor_last_us) {
        uint64_t gap=(uint64_t)(sample_us-s_sensor_last_us);
        uint32_t bounded=gap>UINT32_MAX?UINT32_MAX:(uint32_t)gap;
        if (bounded>s_sensor_max_gap_us) s_sensor_max_gap_us=bounded;
    }
    s_sensor_last_us=sample_us;
    portEXIT_CRITICAL(&s_state_lock);
}
/* Display not ON: one batch cycle. Returns after the wait; the caller loops. */
static void board_batch_cycle(int64_t *power_at)
{
    static uint8_t raw[IMU_BATCH_MAX*BOARD_FIFO_SAMPLE_BYTES];
    static int64_t stamp[IMU_BATCH_MAX];
    static unsigned decimate;
    int n=0,overflow=0;
    if (s_imu_degraded) {                       /* POLL: one register read per cycle */
        imu_leave_fifo();
        hal_imu m;int valid=board_imu_raw(&m);
        int64_t t=esp_timer_get_time();
        hal_imu f={0};
        if (valid) f=imu_take_at(&m,t);
        uint32_t day=board_walk_step(&f,valid,t);
        board_sensor_publish(day,t);
    } else {
        n=imu_fifo_drain(raw,IMU_BATCH_MAX,&overflow);
        int64_t now=esp_timer_get_time();
        if (n<0) {                              /* sensor lost or bus error: same as a failed legacy read */
            hal_imu none={0};
            uint32_t day=board_walk_step(&none,0,now);
            board_sensor_publish(day,now);
        } else if (n>0) {
            s_fifo_batches++;s_fifo_samples_total+=(unsigned)n;s_fifo_empty=0;
            if (overflow) {s_fifo_overflows++;s_fifo_clock.last_us=0;}
            board_fifo_stamp(&s_fifo_clock,now,(unsigned)n,IMU_FIFO_PERIOD_US,stamp);
            if (now<s_fifo_settle_until_us) {n=0;}   /* gyro start-up: discard */
            else {
                hal_imu f={0};uint32_t day=0;
                for (int i=0;i<n;i++) {
                    hal_imu m;
                    board_imu_decode(raw+(unsigned)i*BOARD_FIFO_SAMPLE_BYTES,&m);
                    f=imu_take_at(&m,stamp[i]);
                    /* the step detector was tuned at ~50 Hz: every second 112 Hz sample */
                    if ((++decimate&1u)==0) day=board_walk_step(&f,1,stamp[i]);
                }
                if (!day) day=calendar_day(stamp[n-1]);
                board_sensor_publish(day,stamp[n-1]);
            }
        } else if (s_imu_mode==IMU_MODE_FIFO && now>=s_fifo_settle_until_us && ++s_fifo_empty>=IMU_FIFO_EMPTY_LIMIT) {
            /* The stream stopped although the bus is fine: reconfigure; three strikes give up. */
            s_fifo_empty=0;
            if (++s_fifo_strikes>=IMU_FIFO_STRIKE_LIMIT) s_imu_degraded=1;
            else s_imu_mode=IMU_MODE_LEGACY;
            hal_imu none={0};
            uint32_t day=board_walk_step(&none,0,now);
            board_sensor_publish(day,now);
        }
    }
    int64_t now=esp_timer_get_time();
    if (now>=*power_at) { hal_esp32_sample_power(); *power_at=now+1000000; }
    sleep_grid_delay(50);                       /* shared 50 ms wake grid (nos/sleep_grid.h) */
}
/* Batch (light-sleep friendly) sampling while the display is not ON, unless the raw IMU
 * recorder/stream is running: it paces itself from register reads. */
static int board_batch_wanted(void)
{
    return hal_display_mode()!=2 && !imu_backend_active();
}
static void board_task(void *arg)
{
    (void)arg;
    int64_t power_at=0;
    for (;;) {
        if (atomic_load(&s_stopping)) {
            portENTER_CRITICAL(&s_state_lock);
            uint32_t generation=s_sleep_generation;
            int signal=0;
            s_sensor_last_us=0; /* Deliberate shutdown parking is not an acquisition gap. */
            if (s_sensor_ack!=generation) {
                s_sensor_ack=generation; signal=1;
            }
            portEXIT_CRITICAL(&s_state_lock);
            if (signal) xSemaphoreGive(s_board_stopped);
            vTaskDelay(pdMS_TO_TICKS(10)); continue;
        }
        if (board_batch_wanted()) {
            board_batch_cycle(&power_at);
            continue;
        }
        imu_leave_fifo();                    /* display ON: legacy register set again */
        hal_imu sample={0};
        int valid=board_imu_raw(&sample);
        if (valid) sample=imu_take(&sample);
        int64_t sample_us=esp_timer_get_time();
        uint32_t day=board_walk_step(&sample,valid,sample_us);
        board_sensor_publish(day,sample_us);
        int64_t now=esp_timer_get_time();
        if(now>=power_at) { hal_esp32_sample_power(); power_at=now+1000000; }
        /* 20 ms cycle: the IMU is read again at 8 and 16 ms for the knock detector */
        vTaskDelay(pdMS_TO_TICKS(8)); knock_poll();
        vTaskDelay(pdMS_TO_TICKS(8)); knock_poll();
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}
static void start_board_workers(void)
{
    if (atomic_flag_test_and_set(&s_worker_create)) return;
    if (!s_sensor_task) {
        /* No filesystem, NVS or flash writes on this stack. Calibration and the
         * pedometer seed were loaded on the boot task's INTERNAL stack. */
        StackType_t *stack=nos_mem_alloc(4096,"board.sensor_stack");
        StaticTask_t *tcb=stack ? heap_caps_malloc(sizeof *tcb,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT) : NULL;
        if (tcb) s_sensor_task=xTaskCreateStaticPinnedToCore(board_task,"board-sense",4096,NULL,2,stack,tcb,tskNO_AFFINITY);
        if (!s_sensor_task) {
            free(tcb); nos_mem_free(stack);
            ESP_LOGE(TAG,"sensor worker allocation failed; base sampling unavailable, retry supported");
        }
    }
    if (!s_fs_task && xTaskCreateWithCaps(board_fs_task,"board-fs",4096,NULL,1,&s_fs_task,
                                        MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)!=pdPASS) {
        s_fs_task=NULL;
        ESP_LOGE(TAG,"persistence worker allocation failed; pending state retained, retry supported");
    }
    atomic_flag_clear(&s_worker_create);
}
void board_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = 0, .sda_io_num = PIN_TP_SDA, .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_bus));
    for (unsigned i=0;i<BOARD_DEVICE_COUNT;i++) {
        s_device_lock[i]=xSemaphoreCreateMutex(); assert(s_device_lock[i]);
    }
#if CONFIG_WATCH_BOARD_143C
    touch_init();
#endif
    retry_devices(1);
    s_sd_lock = xSemaphoreCreateMutex(); assert(s_sd_lock);
    s_board_stopped = xSemaphoreCreateBinary(); assert(s_board_stopped);
    s_fs_stopped=xSemaphoreCreateBinary(); assert(s_fs_stopped);
    board_sd_mount(); /* optional: absent/faulty cards (or no slot) never abort boot */
    /* Both lazy NVS reads happen before the PSRAM sensor stack starts. */
    hal_store_load("pedometer",&s_pedometer_saved,sizeof s_pedometer_saved);
    pedometer_init(&s_pedometer,&s_pedometer_saved);
    s_pedometer_snapshot=s_pedometer.today;
    s_calendar_pending_base=s_pedometer_snapshot;
    hal_esp32_sample_power();
    start_board_workers();
}
i2c_master_bus_handle_t board_i2c_bus(void) { return s_bus; }
void board_print_info(void)
{
    hal_storage sd; hal_storage_get(&sd);
    uint32_t ext=hal_ext_present();
    printf("BOARD v=2 %s flash=" CONFIG_ESPTOOLPY_FLASHSIZE " psram=%u touch=%d imu=%d rtc=%d rtc_valid=%d base_audio_supported=%d strap_speaker_enabled=%d strap_mic_enabled=%d audio_playing=%d mic_recording=%d display_applied=%d sd=%d total=%llu free=%llu sys_mv=%d heap_internal=%u heap_largest=%u heap_min=%u\n",
        WATCH_BOARD_NAME, (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        atomic_load(&s_touch_ok), atomic_load(&s_imu_ok), atomic_load(&s_rtc_ok), atomic_load(&s_rtc_valid),
        BOARD_HAS_AUDIO,!!(ext&EXT_SPEAKER),!!(ext&EXT_MIC),hal_audio_playing(),hal_rec_active(),hal_display_mode(),sd.mounted,
        (unsigned long long)sd.total_bytes, (unsigned long long)sd.free_bytes, hal_battery_mv(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    /* mode 0 = legacy register reads (display ON), 1 = FIFO batches, 2 = degraded 20 Hz polling
     * (knock detection is degraded there: it needs the 112 Hz stream) */
    printf("BOARD_IMU_BATCH mode=%d degraded=%d strikes=%u batches=%u samples=%u overflows=%u errors=%u\n",
        s_imu_mode,s_imu_degraded?1:0,s_fifo_strikes,s_fifo_batches,s_fifo_samples_total,s_fifo_overflows,s_fifo_errors);
    board_device_diag devices[BOARD_DEVICE_COUNT]; board_device_diagnostics(devices);
    for (unsigned i=0;i<BOARD_DEVICE_COUNT;i++) {
        const board_device_diag *d=&devices[i];
        printf("BOARD_DEVICE id=%u available=%d attempts=%u failures=%u consecutive=%u first_error=%d last_error=%d retry_ms=%u\n",
               i,d->available,(unsigned)d->attempts,(unsigned)d->failures,(unsigned)d->consecutive,
               d->first_error,d->last_error,(unsigned)d->retry_in_ms);
    }
    portENTER_CRITICAL(&s_state_lock);
    int dirty=s_rtc_dirty, trusted=s_system_trusted;
    time_t requested=s_rtc_pending;
    uint32_t generation=s_rtc_generation, failures=s_rtc_failures, gap=s_sensor_max_gap_us, io=s_fs_max_io_us;
    unsigned operation=s_rtc_failed_operation;
    esp_err_t err=s_rtc_last_error;
    int64_t running=s_fs_io_started;
    int pedometer_dirty=memcmp(&s_pedometer_saved,&s_pedometer_snapshot,sizeof s_pedometer_saved)!=0;
    int timezone_ready=s_calendar_timezone_ready, calendar_ready=s_calendar_sample_ready;
    portEXIT_CRITICAL(&s_state_lock);
    printf("BOARD_WORKERS sensor=%d fs=%d sensor_max_gap_us=%u fs_max_io_us=%u fs_current_us=%llu pedometer_pending=%d calendar_timezone_ready=%d calendar_sample_ready=%d rtc_pending=%d rtc_generation=%u rtc_requested_epoch=%lld system_trusted=%d rtc_errors=%u rtc_operation=%u rtc_error=%d\n",
           s_sensor_task!=NULL,s_fs_task!=NULL,(unsigned)gap,(unsigned)io,
           (unsigned long long)(running?esp_timer_get_time()-running:0),pedometer_dirty,timezone_ready,calendar_ready,dirty,
           (unsigned)generation,(long long)requested,trusted,(unsigned)failures,operation,err);
}
static int sleep_failed(void)
{
    /* No descriptor or mount lease is revoked. A still-blocked worker retains
     * its immutable source snapshot and eventually resumes its own cleanup. */
    s_sleep_state=0;
    atomic_store(&s_stopping,0);
    board_retry_devices();
    return BOARD_SLEEP_FAILED;
}
int board_cancel_sleep(void)
{
    if (s_sleep_state==2 || s_sd_teardown_error) return 0;
    sleep_failed();
    return 1;
}
int board_prepare_sleep(void)
{
    if (s_sleep_state==2) return BOARD_SLEEP_READY;
    if (!s_sensor_task || !s_fs_task) return sleep_failed();
    int64_t now=esp_timer_get_time();
    if (!s_sleep_state) {
        while (xSemaphoreTake(s_board_stopped,0)==pdTRUE) {}
        while (xSemaphoreTake(s_fs_stopped,0)==pdTRUE) {}
        portENTER_CRITICAL(&s_state_lock);
        if (!++s_sleep_generation) ++s_sleep_generation;
        s_fs_sleep_ok=0;
        s_sleep_sensor_received=s_sleep_fs_received=0;
        atomic_store(&s_stopping,1);
        portEXIT_CRITICAL(&s_state_lock);
        s_sleep_deadline=now+5000000; s_sleep_state=1;
        hal_asset_cancel(); /* Owner closes asynchronously; the lease is the barrier. */
        return BOARD_SLEEP_PENDING;
    }
    portENTER_CRITICAL(&s_state_lock);
    int sensor_ack=s_sensor_ack==s_sleep_generation, fs_ack=s_fs_ack==s_sleep_generation;
    int fs_ok=s_fs_sleep_ok, rtc_pending=s_rtc_dirty;
    portEXIT_CRITICAL(&s_state_lock);
    if (sensor_ack && xSemaphoreTake(s_board_stopped,0)==pdTRUE) s_sleep_sensor_received=1;
    if (fs_ack && xSemaphoreTake(s_fs_stopped,0)==pdTRUE) s_sleep_fs_received=1;
    if (now>=s_sleep_deadline || (s_sleep_fs_received && !fs_ok)) return sleep_failed();
    if (!s_sleep_sensor_received || !s_sleep_fs_received) return BOARD_SLEEP_PENDING;
    if (rtc_pending) return sleep_failed();
    if (!sports_quiesced()) return sleep_failed();
    sports_status journal; sports_get_status(&journal);
    if (journal.queued) return sleep_failed();
    /* Stop base IMU only after both workers really parked, before unmount. If
     * that transaction fails, reprobe/reconfigure before resumed sensing. */
    if (atomic_load(&s_imu_ok)) {
        if (!device_begin(BOARD_DEVICE_IMU)) return BOARD_SLEEP_PENDING;
        esp_err_t err=wr(s_imu,0x08,0);
        atomic_store(&s_imu_ok,0);
        device_result(BOARD_DEVICE_IMU,err);
        device_end(BOARD_DEVICE_IMU);
        if (err!=ESP_OK) return sleep_failed();
    }
    /* board_sd_unmount tries the mutex without waiting and checks every lease.
     * Only this polling caller can unmount; workers never unmount asynchronously. */
    esp_err_t err=board_sd_unmount();
    if (err==ESP_ERR_INVALID_STATE) return BOARD_SLEEP_PENDING;
    if (err!=ESP_OK) return sleep_failed();
    s_sleep_state=2;
    return BOARD_SLEEP_READY;
}
