#ifndef EXT_TEST_I2S_H
#define EXT_TEST_I2S_H
#include <stdbool.h>
#include <stddef.h>
#define I2S_NUM_AUTO 0
#define I2S_ROLE_MASTER 0
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_STEREO 2
#define I2S_SLOT_BIT_WIDTH_32BIT 32
#define I2S_GPIO_UNUSED -1
typedef void *i2s_chan_handle_t;
typedef struct {int dma_desc_num,dma_frame_num;bool auto_clear;} i2s_chan_config_t;
typedef struct {int slot_bit_width;} test_i2s_slot;
typedef struct {int clk_cfg;test_i2s_slot slot_cfg;struct {int mclk,bclk,ws,dout,din;} gpio_cfg;} i2s_std_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(a,b) ((i2s_chan_config_t){0})
#define I2S_STD_CLK_DEFAULT_CONFIG(a) (a)
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(a,b) ((test_i2s_slot){0})
int i2s_new_channel(const i2s_chan_config_t*,i2s_chan_handle_t*,i2s_chan_handle_t*);
int i2s_channel_init_std_mode(i2s_chan_handle_t,const i2s_std_config_t*);
int i2s_channel_enable(i2s_chan_handle_t);
int i2s_channel_disable(i2s_chan_handle_t);
int i2s_del_channel(i2s_chan_handle_t);
int i2s_channel_write(i2s_chan_handle_t,const void*,size_t,size_t*,unsigned);
int i2s_channel_read(i2s_chan_handle_t,void*,size_t,size_t*,unsigned);
#endif
