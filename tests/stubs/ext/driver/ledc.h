#ifndef EXT_TEST_LEDC_H
#define EXT_TEST_LEDC_H
#include <stdint.h>
#define LEDC_TIMER_3 3
#define LEDC_CHANNEL_7 7
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_10_BIT 10
#define LEDC_AUTO_CLK 0
typedef struct {int speed_mode,duty_resolution,timer_num,freq_hz,clk_cfg;} ledc_timer_config_t;
typedef struct {int gpio_num,speed_mode,channel,timer_sel,duty,hpoint;} ledc_channel_config_t;
int ledc_set_duty(int,int,uint32_t);
int ledc_update_duty(int,int);
int ledc_timer_config(const ledc_timer_config_t*);
int ledc_channel_config(const ledc_channel_config_t*);
#endif
