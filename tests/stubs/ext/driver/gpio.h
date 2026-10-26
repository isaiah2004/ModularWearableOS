#ifndef EXT_TEST_GPIO_H
#define EXT_TEST_GPIO_H
#include <stdint.h>
#define GPIO_MODE_INPUT 0
#define GPIO_MODE_OUTPUT 1
#define GPIO_MODE_OUTPUT_OD 2
#define GPIO_PULLUP_ENABLE 1
typedef struct {uint64_t pin_bit_mask;int mode,pull_up_en;} gpio_config_t;
int gpio_config(const gpio_config_t*);
int gpio_set_level(int,int);
int gpio_get_level(int);
int gpio_reset_pin(int);
int gpio_set_direction(int,int);
#endif
