#ifndef EXT_TEST_I2C_H
#define EXT_TEST_I2C_H
#include <stddef.h>
#include <stdint.h>
#define ESP_OK 0
#define I2C_ADDR_BIT_LEN_7 0
typedef void *i2c_master_dev_handle_t;
typedef void *i2c_master_bus_handle_t;
typedef struct {int dev_addr_length,device_address,scl_speed_hz;} i2c_device_config_t;
int i2c_master_bus_rm_device(i2c_master_dev_handle_t);
int i2c_master_probe(i2c_master_bus_handle_t,uint8_t,unsigned);
int i2c_master_bus_add_device(i2c_master_bus_handle_t,const i2c_device_config_t*,i2c_master_dev_handle_t*);
int i2c_master_transmit_receive(i2c_master_dev_handle_t,const uint8_t*,size_t,void*,size_t,unsigned);
int i2c_master_transmit(i2c_master_dev_handle_t,const uint8_t*,size_t,unsigned);
#endif
