#ifndef TEST_ESP_PM_H
#define TEST_ESP_PM_H
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define CONFIG_PM_LIGHT_SLEEP_CALLBACKS 1
struct mock_pm_lock;
typedef struct mock_pm_lock *esp_pm_lock_handle_t;
typedef struct {int max_freq_mhz,min_freq_mhz;bool light_sleep_enable;} esp_pm_config_t;
typedef esp_err_t (*esp_pm_light_sleep_cb_t)(int64_t, void *);
typedef struct {
    esp_pm_light_sleep_cb_t enter_cb,exit_cb;
    void *enter_cb_user_arg,*exit_cb_user_arg;
    uint32_t enter_cb_prior,exit_cb_prior;
} esp_pm_sleep_cbs_register_config_t;
#define ESP_PM_CPU_FREQ_MAX 0
#define ESP_PM_NO_LIGHT_SLEEP 2
esp_err_t esp_pm_configure(const void *config);
esp_err_t esp_pm_lock_create(int type,int arg,const char *name,esp_pm_lock_handle_t *out);
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t handle);
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t handle);
esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t handle);
esp_err_t esp_pm_light_sleep_register_cbs(esp_pm_sleep_cbs_register_config_t *cbs_conf);
#endif
