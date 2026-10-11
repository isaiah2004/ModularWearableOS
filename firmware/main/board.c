/* Board services for Waveshare 1.43 / 1.43-B, or the 1.43C (CST820 touch, no
 * IMU, RTC or SD). Register settings and wiring follow the official demos;
 * modern IDF I2C master API used throughout. */
#include "board.h"
#include "board_protocol.h"
#include "pins.h"
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"
#include "settings.h"
#include "pedometer.h"

#define TAG "board"
#define SD_ROOT "/sdcard"
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_touch, s_imu, s_rtc;
static int s_touch_ok, s_imu_ok, s_rtc_ok, s_rtc_valid;
static hal_imu s_sample;
static int64_t s_sample_us;
static time_t s_rtc_pending;
static int s_rtc_dirty;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_sd_lock;
static SemaphoreHandle_t s_board_stopped;
static volatile int s_stopping;
static sdmmc_card_t *s_card;
static hal_storage s_storage = { .message = "No card mounted" };
static char s_log_path[96];
static pedometer s_pedometer;
static hal_activity s_activity;

static esp_err_t rd(i2c_master_dev_handle_t dev, uint8_t reg, void *out, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, out, n, 30);
}
static esp_err_t wr(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value)
{
    uint8_t b[] = {reg, value};
    return i2c_master_transmit(dev, b, sizeof b, 30);
}
static int device(uint8_t addr, i2c_master_dev_handle_t *dev)
{
    i2c_device_config_t cfg = { .device_address = addr, .scl_speed_hz = 400000 };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &cfg, dev));
    return i2c_master_probe(s_bus, addr, 50) == ESP_OK;
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
    if (!s_touch) return 0;
    if (!s_touch_ok) {
        if (!s_touch_irq && gpio_get_level(PIN_TP_INT)) return 0;
        s_touch_irq = 0;
    }
    if (rd(s_touch, 0x02, r, sizeof r) != ESP_OK) { s_touch_ok = 0; return 0; }
    s_touch_ok = 1;
    return board_touch_decode(r, x, y);
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
    s_touch_ok = device(TOUCH_ADDR, &s_touch);
}
#else
int board_touch_read(int *x, int *y)
{
    uint8_t r[5];
    if (!s_touch_ok || rd(s_touch, 0x02, r, sizeof r) != ESP_OK) return 0;
    return board_touch_decode(r, x, y);
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
    portENTER_CRITICAL(&s_state_lock);
    *out=s_activity;
    if(s_sample_us<=0 || now-s_sample_us>=250000) { out->valid=0; out->walking=0; }
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
    if (s_card) { xSemaphoreGive(s_sd_lock); return ESP_OK; }
    /* Same one-bit SDMMC wiring as Waveshare 04_SD_Card. 20 MHz for v1. */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
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
        storage_status("Absent or not FAT32; no format");
        ESP_LOGW(TAG, "SD mount: %s; continuing without card", esp_err_to_name(err));
    }
    xSemaphoreGive(s_sd_lock);
    return err;
#endif
}
esp_err_t board_sd_unmount(void)
{
    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    esp_err_t err = s_card ? esp_vfs_fat_sdcard_unmount(SD_ROOT, s_card) : ESP_OK;
    if (err == ESP_OK) { s_card = NULL; s_log_path[0] = 0; storage_status("Ejected; safe to remove"); }
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
static void log_sample(void)
{
    xSemaphoreTake(s_sd_lock, portMAX_DELAY);
    if (!s_card) { xSemaphoreGive(s_sd_lock); return; }
    int failed = 0;
    if (!s_log_path[0]) {
        int fd = unique_file(s_log_path, sizeof s_log_path, SD_ROOT "/watch/logs", "csv");
        if (fd < 0) { s_log_path[0] = 0; failed = 1; }
        else {
            const char *hdr = "utc_epoch,uptime_ms,system_mv,imu_valid,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps\n";
            if (write(fd, hdr, strlen(hdr)) != (ssize_t)strlen(hdr)) failed = 1;
            if (close(fd) != 0) failed = 1;
        }
    }
    struct stat st;
    if (!failed && stat(s_log_path, &st) == 0 && st.st_size >= 1024 * 1024) {
        s_log_path[0] = 0; /* rotate next minute */
        xSemaphoreGive(s_sd_lock); return;
    }
    FILE *f = failed ? NULL : fopen(s_log_path, "a");
    if (f) {
        hal_imu m; int valid = board_imu_read(&m);
        if (fprintf(f, "%lld,%llu,%d,%d,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f\n",
            (long long)time(NULL), (unsigned long long)(esp_timer_get_time() / 1000),
            hal_battery_mv(), valid, m.ax, m.ay, m.az, m.gx, m.gy, m.gz) < 0) failed = 1;
        if (fclose(f) != 0) failed = 1;
    } else failed = 1;
    storage_status(failed ? "Log write failed; check card" : "Ready; logs every 60s");
    xSemaphoreGive(s_sd_lock);
}
void board_clock_changed(time_t epoch)
{
    portENTER_CRITICAL(&s_state_lock);
    s_rtc_pending = epoch; s_rtc_dirty = 1;
    portEXIT_CRITICAL(&s_state_lock);
}
static void rtc_save(time_t epoch)
{
    uint8_t data[8] = {0x04};
    if (!s_rtc_ok || !board_rtc_encode((int64_t)epoch, data + 1)) return;
    /* Preserve oscillator capacitance/control bits, select 24h, stop for an
     * atomic calendar write, then restart. A partial write stays untrusted. */
    uint8_t ctrl;
    if (rd(s_rtc, 0x00, &ctrl, 1) != ESP_OK) return;
    ctrl &= (uint8_t)~0x22;
    if (wr(s_rtc, 0, ctrl | 0x20) != ESP_OK) return;
    esp_err_t err = i2c_master_transmit(s_rtc, data, sizeof data, 30);
    if (err != ESP_OK) wr(s_rtc, 4, 0x80); /* invalidate a partial calendar */
    esp_err_t restart = wr(s_rtc, 0, ctrl);
    s_rtc_valid = err == ESP_OK && restart == ESP_OK;
    if (!s_rtc_valid) ESP_LOGW(TAG, "RTC write failed");
}
void board_apply_timezone(void)
{
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
    int64_t epoch;
    if (s_rtc_ok && rd(s_rtc, 0, &ctrl, 1) == ESP_OK && !(ctrl & 0x22) &&
        rd(s_rtc, 4, raw, sizeof raw) == ESP_OK && board_rtc_decode(raw, &epoch)) {
        struct timeval tv = { .tv_sec = (time_t)epoch };
        settimeofday(&tv, NULL); s_rtc_valid = 1;
        hal_log("RTC: restored UTC time");
    } else if (!BOARD_HAS_RTC && time(NULL) >= 1577836800) {
        /* No RTC chip: system time survives a soft reset or deep sleep, not power-off. */
        s_rtc_valid = 1;
        hal_log("RTC: none; kept system time across reset");
    } else {
        /* Explicitly unsynchronised until USB/BLE/NTP sets it; do not seed RTC. */
        struct timeval tv = { .tv_sec = 0 };
        settimeofday(&tv, NULL);
        hal_log("RTC: unset; settime over USB or sync phone/WiFi");
    }
}
static void board_task(void *arg)
{
    (void)arg;
    int64_t log_at = esp_timer_get_time() + 60000000;
    int64_t power_at=0;
    pedometer_saved saved={0};
    hal_store_load("pedometer",&saved,sizeof saved);
    pedometer_init(&s_pedometer,&saved);
    int64_t save_at=esp_timer_get_time()+60000000;
    int64_t date_at=0;
    uint32_t day=0;
    while (!s_stopping) {
        hal_imu sample={0};
        int valid=0;
        if (s_imu_ok) {
            uint8_t raw[12];
            if (rd(s_imu, 0x35, raw, sizeof raw) == ESP_OK) {
                board_imu_decode(raw, &sample); valid=1;
                portENTER_CRITICAL(&s_state_lock);
                s_sample = sample; s_sample_us = esp_timer_get_time();
                portEXIT_CRITICAL(&s_state_lock);
            }
        }
        int64_t sample_us=esp_timer_get_time();
        if(sample_us>=date_at) {
            /* Independent of the UI's frozen frame clock. RTC/SNTP store UTC. */
            time_t epoch=time(NULL);
            struct tm local;
            if(epoch>=1577836800 && localtime_r(&epoch,&local))
                day=(uint32_t)((local.tm_year+1900)*10000+(local.tm_mon+1)*100+local.tm_mday);
            date_at=sample_us+1000000;
        }
        pedometer_update(&s_pedometer,day,(uint32_t)(sample_us/1000),&sample,valid);
        hal_activity activity; pedometer_get(&s_pedometer,&activity);
        portENTER_CRITICAL(&s_state_lock);
        s_activity=activity;
        portEXIT_CRITICAL(&s_state_lock);
        int64_t now=esp_timer_get_time();
        if(now>=save_at) {
            if(memcmp(&saved,&s_pedometer.today,sizeof saved) &&
               hal_store_save_checked("pedometer",&s_pedometer.today,sizeof saved)) saved=s_pedometer.today;
            save_at=now+60000000;
        }
        if(now>=power_at) { hal_esp32_sample_power(); power_at=now+1000000; }
        portENTER_CRITICAL(&s_state_lock);
        int dirty = s_rtc_dirty; time_t epoch = s_rtc_pending; s_rtc_dirty = 0;
        portEXIT_CRITICAL(&s_state_lock);
        if (dirty) rtc_save(epoch);
        if (esp_timer_get_time() >= log_at) { log_sample(); log_at = esp_timer_get_time() + 60000000; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if(memcmp(&saved,&s_pedometer.today,sizeof saved))
        hal_store_save("pedometer",&s_pedometer.today,sizeof saved);
    xSemaphoreGive(s_board_stopped);
    vTaskDelete(NULL);
}
void board_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = 0, .sda_io_num = PIN_TP_SDA, .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_bus));
#if CONFIG_WATCH_BOARD_143C
    touch_init();
    ESP_LOGI(TAG, "I2C touch@%02x=%d (CST820; retried on TP_INT) no rtc/imu on this board",
             TOUCH_ADDR, s_touch_ok);
#else
    s_touch_ok = device(TOUCH_ADDR, &s_touch);
    if (s_touch_ok) s_touch_ok = wr(s_touch, 0x00, 0x00) == ESP_OK;
    s_rtc_ok = device(RTC_ADDR, &s_rtc);
    s_imu_ok = device(IMU_ADDR, &s_imu);
    if (s_imu_ok) {
        uint8_t id = 0;
        s_imu_ok = rd(s_imu, 0, &id, 1) == ESP_OK && id == 0x05;
        /* Auto-increment, sensors off while configuring, +/-8g / +/-512dps,
         * 125 Hz ODR, low-pass filters enabled, then enable accel and gyro. */
        if (s_imu_ok) s_imu_ok = wr(s_imu, 0x02, 0x60) == ESP_OK &&
            wr(s_imu, 0x08, 0x00) == ESP_OK && wr(s_imu, 0x03, 0x26) == ESP_OK &&
            wr(s_imu, 0x04, 0x56) == ESP_OK && wr(s_imu, 0x06, 0x11) == ESP_OK &&
            wr(s_imu, 0x08, 0x03) == ESP_OK;
    }
    ESP_LOGI(TAG, "I2C touch@38=%d rtc@51=%d imu@6b=%d", s_touch_ok, s_rtc_ok, s_imu_ok);
#endif
    s_sd_lock = xSemaphoreCreateMutex(); assert(s_sd_lock);
    s_board_stopped = xSemaphoreCreateBinary(); assert(s_board_stopped);
    board_sd_mount(); /* optional: absent/faulty cards (or no slot) never abort boot */
    BaseType_t ok = xTaskCreate(board_task, "board", 4096, NULL, 2, NULL);
    assert(ok == pdPASS);
}
i2c_master_bus_handle_t board_i2c_bus(void) { return s_bus; }
void board_print_info(void)
{
    hal_storage sd; hal_storage_get(&sd);
    printf("BOARD %s flash=" CONFIG_ESPTOOLPY_FLASHSIZE " psram=%u touch=%d imu=%d rtc=%d rtc_valid=%d audio=0 sd=%d total=%llu free=%llu sys_mv=%d heap_internal=%u heap_largest=%u heap_min=%u\n",
        WATCH_BOARD_NAME, (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        s_touch_ok, s_imu_ok, s_rtc_ok, s_rtc_valid, sd.mounted,
        (unsigned long long)sd.total_bytes, (unsigned long long)sd.free_bytes, hal_battery_mv(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}
void board_prepare_sleep(void)
{
    s_stopping = 1;
    xSemaphoreTake(s_board_stopped, portMAX_DELAY);
    rtc_save(time(NULL));
    board_sd_unmount();
    if (s_imu_ok) wr(s_imu, 0x08, 0x00);
}
