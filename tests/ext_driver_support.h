#ifndef EXT_DRIVER_SUPPORT_H
#define EXT_DRIVER_SUPPORT_H
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "../firmware/main/ext/ext_priv.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "hal.h"

static volatile LONG64 test_clock;
static int test_shutdown,test_write_ok=1,test_gpio_ok=1,test_uart_tx_ok=1,test_uart_delete_ok=1;
static int test_ota,test_host_shutdown;
static int test_store_fail;
int ota_busy(void){return test_ota;}
int nos_host_shutdown_pending(void){return test_host_shutdown;}
static unsigned test_writes,test_reads,test_uart_writes;
static int (*test_read_cb)(uint8_t,void*,size_t);
static int (*test_uart_cb)(void*,unsigned);
static int test_i2s_disable_fail,test_i2s_delete_fail;
static jmp_buf test_delay_jmp;
static int test_delay_escape;
static struct {const char *name;void (*fn)(void*);} test_tasks[4];static int test_ntasks;
#ifndef EXT_TEST_SYS
portMUX_TYPE ext_lock=portMUX_INITIALIZER_UNLOCKED;
uint32_t ext_present_bits;
int ext_shutdown_requested(void){return test_shutdown;}
void *ext_psram_calloc_owned(size_t n,const char *owner){(void)owner;return calloc(1,n);}
int ext_i2c_open(uint8_t address,i2c_master_dev_handle_t *out){(void)address;*out=(void*)1;return 1;}
int ext_rd(i2c_master_dev_handle_t dev,uint8_t reg,void *out,size_t n){(void)dev;test_reads++;return test_read_cb?test_read_cb(reg,out,n):0;}
int ext_wr(i2c_master_dev_handle_t dev,uint8_t reg,uint8_t value){(void)dev;(void)reg;(void)value;test_writes++;return test_write_ok;}
int ext_wr_buf(i2c_master_dev_handle_t dev,uint8_t reg,const uint8_t *value,size_t n){(void)value;(void)n;return ext_wr(dev,reg,0);}
#endif
int64_t esp_timer_get_time(void){return InterlockedCompareExchange64(&test_clock,0,0)*1000;}
void vTaskDelay(TickType_t ms){if(test_delay_escape)longjmp(test_delay_jmp,1);InterlockedAdd64(&test_clock,ms);}
unsigned ulTaskNotifyTake(int clear,TickType_t ms){(void)clear;(void)ms;if(test_delay_escape)longjmp(test_delay_jmp,1); /* the ext task idles here */return 0;}
void xTaskNotifyGive(TaskHandle_t handle){(void)handle;}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t handle){(void)handle;return 4096;}
int xTaskCreatePinnedToCoreWithCaps(void (*fn)(void*),const char *name,unsigned stack,void *arg,int priority,TaskHandle_t *out,int core,unsigned caps)
{
    (void)stack;(void)arg;(void)priority;(void)core;(void)caps;
    assert(test_ntasks<4);test_tasks[test_ntasks].name=name;test_tasks[test_ntasks].fn=fn;test_ntasks++;
    *out=(void*)(uintptr_t)test_ntasks;return pdPASS;
}
void *nos_mem_alloc(uint32_t n,const char *owner){(void)owner;return malloc(n);}
void nos_mem_free(void *p){free(p);}
void nos_mem_external(const char *owner,uint32_t size,int ok){(void)owner;(void)size;(void)ok;}
int i2c_master_bus_rm_device(i2c_master_dev_handle_t dev){(void)dev;return 0;}
int i2c_master_probe(i2c_master_bus_handle_t bus,uint8_t addr,unsigned ms){(void)bus;(void)addr;(void)ms;return 0;}
int i2c_master_bus_add_device(i2c_master_bus_handle_t bus,const i2c_device_config_t *cfg,i2c_master_dev_handle_t *out){(void)bus;(void)cfg;*out=(void*)1;return 0;}
int i2c_master_transmit_receive(i2c_master_dev_handle_t dev,const uint8_t *reg,size_t rn,void *out,size_t n,unsigned ms){(void)dev;(void)rn;(void)ms;test_reads++;return test_read_cb&&test_read_cb(*reg,out,n)?0:-1;}
int i2c_master_transmit(i2c_master_dev_handle_t dev,const uint8_t *b,size_t n,unsigned ms){(void)dev;(void)b;(void)n;(void)ms;test_writes++;return test_write_ok?0:-1;}
i2c_master_bus_handle_t board_i2c_bus(void){return (void*)1;}
int gpio_config(const gpio_config_t *cfg){(void)cfg;return test_gpio_ok?0:-1;}
int gpio_set_level(int pin,int level){(void)pin;(void)level;return test_gpio_ok?0:-1;}
int gpio_get_level(int pin){(void)pin;return 1;}
int gpio_reset_pin(int pin){(void)pin;return test_gpio_ok?0:-1;}
int gpio_set_direction(int pin,int mode){(void)pin;(void)mode;return test_gpio_ok?0:-1;}
int uart_driver_install(int port,int rx,int tx,int n,void *queue,int flags){(void)port;(void)rx;(void)tx;(void)n;(void)queue;(void)flags;return 0;}
int uart_driver_delete(int port){(void)port;return test_uart_delete_ok?0:-1;}
int uart_param_config(int port,const uart_config_t *cfg){(void)port;(void)cfg;return 0;}
int uart_set_pin(int port,int tx,int rx,int rts,int cts){(void)port;(void)tx;(void)rx;(void)rts;(void)cts;return 0;}
int uart_flush_input(int port){(void)port;return 0;}
int uart_set_baudrate(int port,int baud){(void)port;(void)baud;return 0;}
int uart_write_bytes(int port,const void *data,size_t n){(void)port;(void)data;test_uart_writes++;return test_uart_tx_ok?(int)n:-1;}
int uart_wait_tx_done(int port,unsigned ms){(void)port;(void)ms;return test_uart_tx_ok?0:-1;}
int uart_read_bytes(int port,void *out,unsigned n,unsigned wait){(void)port;(void)wait;return test_uart_cb?test_uart_cb(out,n):0;}
int i2s_new_channel(const i2s_chan_config_t *cfg,i2s_chan_handle_t *tx,i2s_chan_handle_t *rx){(void)cfg;*tx=(void*)11;*rx=(void*)12;return 0;}
int i2s_channel_init_std_mode(i2s_chan_handle_t channel,const i2s_std_config_t *cfg){(void)channel;(void)cfg;return 0;}
int i2s_channel_enable(i2s_chan_handle_t channel){(void)channel;return 0;}
int i2s_channel_disable(i2s_chan_handle_t channel){(void)channel;if(test_i2s_disable_fail){test_i2s_disable_fail--;return -1;}return 0;}
int i2s_del_channel(i2s_chan_handle_t channel){(void)channel;if(test_i2s_delete_fail){test_i2s_delete_fail--;return -1;}return 0;}
int i2s_channel_write(i2s_chan_handle_t channel,const void *p,size_t n,size_t *out,unsigned ms){(void)channel;(void)p;(void)ms;*out=n;return 0;}
int i2s_channel_read(i2s_chan_handle_t channel,void *p,size_t n,size_t *out,unsigned ms){(void)channel;(void)ms;memset(p,0,n);*out=n;InterlockedAdd64(&test_clock,4);return 0;}
int esp_timer_start_once(esp_timer_handle_t timer,uint64_t us){(void)timer;(void)us;return 0;}
int esp_timer_stop(esp_timer_handle_t timer){(void)timer;return 0;}
int esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *out){(void)args;*out=(void*)1;return 0;}
int ledc_set_duty(int mode,int channel,uint32_t duty){(void)mode;(void)channel;(void)duty;return test_gpio_ok?0:-1;}
int ledc_update_duty(int mode,int channel){(void)mode;(void)channel;return test_gpio_ok?0:-1;}
int ledc_timer_config(const ledc_timer_config_t *cfg){(void)cfg;return 0;}
int ledc_channel_config(const ledc_channel_config_t *cfg){(void)cfg;return 0;}
unsigned heap_caps_get_free_size(unsigned caps){(void)caps;return 1u<<20;}
uint64_t hal_epoch_seconds(void){return 1792530000u;}
int hal_store_load(const char *key,void *p,int n){(void)key;(void)p;(void)n;return 0;}
void hal_store_save(const char *key,const void *p,int n){(void)key;(void)p;(void)n;}
int hal_store_save_checked(const char *key,const void *p,int n){(void)key;(void)p;(void)n;return !test_store_fail;}
int hal_imu_read(hal_imu *out){memset(out,0,sizeof *out);out->az=1;return 1;}
void sports_emit(int type,const float *v,int n){(void)type;(void)v;(void)n;}
#endif
