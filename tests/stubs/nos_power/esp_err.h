#ifndef TEST_ESP_ERR_H
#define TEST_ESP_ERR_H
#include <assert.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERROR_CHECK(value) assert((value)==ESP_OK)
#endif
