/* Adapted from Waveshare Demo V3 read_lcd_id_bsp: single-line 0x03/0xDA
 * read on D0 before handing the pins to the QSPI peripheral. */
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "pins.h"

static void send_byte(uint8_t data)
{
    for (int bit = 7; bit >= 0; bit--) {
        gpio_set_level(PIN_LCD_PCLK, 0);
        gpio_set_level(PIN_LCD_D0, (data >> bit) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_LCD_PCLK, 1);
        esp_rom_delay_us(1);
    }
}
uint8_t board_lcd_probe(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_LCD_CS) | (1ULL << PIN_LCD_PCLK) |
                        (1ULL << PIN_LCD_D0) | (1ULL << PIN_LCD_D1) |
                        (1ULL << PIN_LCD_D2) | (1ULL << PIN_LCD_D3) | (1ULL << PIN_LCD_RST),
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(PIN_LCD_CS, 1);
    gpio_set_level(PIN_LCD_D1, 1); gpio_set_level(PIN_LCD_D2, 1); gpio_set_level(PIN_LCD_D3, 1);
    gpio_set_level(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(PIN_LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));
    gpio_set_level(PIN_LCD_CS, 0);
    send_byte(0x03); send_byte(0x00); send_byte(0xda); send_byte(0x00);
    gpio_set_direction(PIN_LCD_D0, GPIO_MODE_INPUT);
    uint8_t id = 0;
    for (int bit = 0; bit < 8; bit++) {
        gpio_set_level(PIN_LCD_PCLK, 0); esp_rom_delay_us(1);
        id = (uint8_t)((id << 1) | gpio_get_level(PIN_LCD_D0));
        gpio_set_level(PIN_LCD_PCLK, 1); esp_rom_delay_us(1);
    }
    gpio_set_level(PIN_LCD_CS, 1);
    /* Clear GPIO input/output matrix state before spi_bus_initialize(). */
    const int pins[] = {PIN_LCD_CS,PIN_LCD_PCLK,PIN_LCD_D0,PIN_LCD_D1,PIN_LCD_D2,PIN_LCD_D3};
    for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; i++) gpio_reset_pin(pins[i]);
    return id; /* Waveshare selects SH8601 for 0x86, CO5300 otherwise. */
}
