#ifndef WATCH_BOARD_H
#define WATCH_BOARD_H
#include <time.h>
#include "esp_err.h"
#include "hal.h"
#include "driver/i2c_master.h"
void board_init(void);
/* Background NVS writes are deferred while the host animates. */
void board_set_background_persistence(int allowed);
void board_restore_clock(void); /* after NVS settings load; before radio time sync */
void board_apply_timezone(void);
/* Default closed. Host opens only after authoritative prefs/TZ resolve; close
 * while loading/restoring prefs. apply_timezone closes it before changing TZ.
 * Samples still count while closed, but never trigger a destructive rollover. */
void board_calendar_ready(int ready);
int board_clock_valid(void);
void board_clock_changed(time_t epoch); /* checked RTC write on persistence worker */
/* A failed acquisition is never a normal finger release. */
enum { BOARD_TOUCH_INVALID=-1, BOARD_TOUCH_UP=0, BOARD_TOUCH_DOWN=1 };
int board_touch_read(int *x, int *y);
enum { BOARD_DEVICE_TOUCH, BOARD_DEVICE_IMU, BOARD_DEVICE_RTC, BOARD_DEVICE_COUNT };
typedef struct {
    uint32_t attempts, failures, consecutive, retry_in_ms;
    int available, first_error, last_error;
} board_device_diag;
void board_device_diagnostics(board_device_diag out[BOARD_DEVICE_COUNT]);
/* Recheck unavailable base devices; never reset/reconfigure a live device. */
void board_retry_devices(void);
typedef struct {
    uint32_t reads, errors, injected, pending;
    int last_error;
} board_touch_diag;
board_touch_diag board_touch_diagnostics(void);
/* Bench only: at most eight subsequent reads use a zero-tick driver timeout.
 * Zero cancels pending injections. Normal polling retains its 30 ms timeout. */
void board_touch_inject_timeouts(unsigned count);
int board_imu_read(hal_imu *out);
int board_imu_available(void);
int board_imu_raw(hal_imu *out); /* sampler task only; does not touch wake/knock filters */
int board_imu_batch_state(void); /* 0 legacy reads (display ON), 1 FIFO batches, 2 degraded 20 Hz polling */
/* Boot preloads calibration on an internal stack; subsequent sensor calls do no NVS I/O. */
void hal_esp32_sample_power(void);
void board_print_info(void);
esp_err_t board_sd_mount(void);
esp_err_t board_sd_unmount(void);
esp_err_t board_sd_test(void);
/* Asset worker only: lease spans open..close, mutex held for one I/O operation. */
int board_sd_asset_acquire(void);
void board_sd_asset_release(void);
void board_sd_asset_lock(void);
void board_sd_asset_unlock(void);
enum { BOARD_SLEEP_PENDING=-1, BOARD_SLEEP_FAILED=0, BOARD_SLEEP_READY=1 };
/* Nonblocking polling; READY requires both workers parked and an actual unmount.
 * Failure resumes workers without surrendering any open file or mount lease. */
int board_prepare_sleep(void);
/* 1 request accepted; 0 already unmounted or teardown failed. Never revokes I/O ownership. */
int board_cancel_sleep(void);
/* Shared I2C master bus (SDA47/SCL48) for the strap sensors (ext/). The IDF
 * driver serialises transactions per bus, so no extra mutex is needed. */
i2c_master_bus_handle_t board_i2c_bus(void);
#endif
