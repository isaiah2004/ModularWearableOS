#ifndef WATCH_BOARD_H
#define WATCH_BOARD_H
#include <time.h>
#include "esp_err.h"
#include "hal.h"
#include "driver/i2c_master.h"
void board_init(void);
void board_restore_clock(void); /* after NVS settings load; before radio time sync */
void board_apply_timezone(void);
int board_clock_valid(void);
void board_clock_changed(time_t epoch); /* schedules RTC write on the board task */
int board_touch_read(int *x, int *y);
int board_imu_read(hal_imu *out);
/* Internal: called only by board_task once per second, after ADC init. */
void hal_esp32_sample_power(void);
void board_print_info(void);
esp_err_t board_sd_mount(void);
esp_err_t board_sd_unmount(void);
esp_err_t board_sd_test(void);
void board_prepare_sleep(void);
/* Shared I2C master bus (SDA47/SCL48) for the strap sensors (ext/). The IDF
 * driver serialises transactions per bus, so no extra mutex is needed. */
i2c_master_bus_handle_t board_i2c_bus(void);
#endif
