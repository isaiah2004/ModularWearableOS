#ifndef EXT_TEST_LOG_H
#define EXT_TEST_LOG_H
#include <stdio.h>
#define ESP_LOGI(tag,...) do{printf(__VA_ARGS__);putchar('\n');}while(0)
#define ESP_LOGW(tag,...) ESP_LOGI(tag,__VA_ARGS__)
#endif
