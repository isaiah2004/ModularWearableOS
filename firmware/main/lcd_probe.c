/* Pre-SPI panel probe based on Waveshare Demo V3 read_lcd_id_bsp. */
#include "lcd_probe.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pins.h"

#define PROBE_CYCLES 5

typedef struct {
    uint8_t reg04[4], da, db, dc;
    esp_err_t err04, err_da, err_db, err_dc;
} probe_cycle_t;

static probe_cycle_t s_cycles[PROBE_CYCLES];
static esp_err_t s_setup_error = ESP_ERR_INVALID_STATE;
static bool s_cached;
static uint8_t s_compat_id;

static void remember_error(esp_err_t *first, esp_err_t err)
{
    if (*first == ESP_OK && err != ESP_OK) *first = err;
}

static void send_byte(uint8_t data, esp_err_t *err)
{
    for (int bit = 7; bit >= 0; bit--) {
        remember_error(err, gpio_set_level(PIN_LCD_D0, (data >> bit) & 1));
        remember_error(err, gpio_set_level(PIN_LCD_PCLK, 0));
        remember_error(err, gpio_set_level(PIN_LCD_PCLK, 1));
    }
}

/* Preserve Waveshare's SPI_ReadData_Continue timing and per-bit direction. */
static uint8_t read_byte(esp_err_t *err)
{
    uint8_t value = 0;
    for (int bit = 0; bit < 8; bit++) {
        remember_error(err, gpio_set_level(PIN_LCD_PCLK, 0));
        remember_error(err, gpio_set_direction(PIN_LCD_D0, GPIO_MODE_INPUT));
        esp_rom_delay_us(1);
        value = (uint8_t)((value << 1) | (gpio_get_level(PIN_LCD_D0) & 1));
        remember_error(err, gpio_set_direction(PIN_LCD_D0, GPIO_MODE_OUTPUT));
        remember_error(err, gpio_set_level(PIN_LCD_PCLK, 1));
        esp_rom_delay_us(1);
    }
    return value;
}

static esp_err_t read_register(uint8_t reg, uint8_t *data, size_t size)
{
    esp_err_t err = ESP_OK;
    remember_error(&err, gpio_set_level(PIN_LCD_CS, 0));
    send_byte(0x03, &err); send_byte(0x00, &err);
    send_byte(reg, &err);  send_byte(0x00, &err);
    for (size_t i = 0; i < size; i++) data[i] = read_byte(&err);
    remember_error(&err, gpio_set_level(PIN_LCD_CS, 1));
    return err;
}

uint8_t board_lcd_probe(void)
{
    memset(s_cycles, 0, sizeof(s_cycles));
    s_cached = false;
    s_compat_id = 0;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_LCD_CS) | (1ULL << PIN_LCD_PCLK) |
                        (1ULL << PIN_LCD_D0) | (1ULL << PIN_LCD_D1) |
                        (1ULL << PIN_LCD_D2) | (1ULL << PIN_LCD_D3) |
                        (1ULL << PIN_LCD_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    s_setup_error = gpio_config(&cfg);
    if (s_setup_error == ESP_OK) {
        remember_error(&s_setup_error, gpio_set_level(PIN_LCD_CS, 1));
        remember_error(&s_setup_error, gpio_set_level(PIN_LCD_D1, 1));
        remember_error(&s_setup_error, gpio_set_level(PIN_LCD_D2, 1));
        remember_error(&s_setup_error, gpio_set_level(PIN_LCD_D3, 1));
        remember_error(&s_setup_error, gpio_set_level(PIN_LCD_RST, 1));
        vTaskDelay(pdMS_TO_TICKS(120));
        remember_error(&s_setup_error, gpio_set_level(PIN_LCD_RST, 0));
        vTaskDelay(pdMS_TO_TICKS(120));
        remember_error(&s_setup_error, gpio_set_level(PIN_LCD_RST, 1));
        vTaskDelay(pdMS_TO_TICKS(120));
        for (int i = 0; i < PROBE_CYCLES; i++) {
            probe_cycle_t *c = &s_cycles[i];
            c->err04 = read_register(0x04, c->reg04, sizeof(c->reg04));
            c->err_da = read_register(0xda, &c->da, 1);
            c->err_db = read_register(0xdb, &c->db, 1);
            c->err_dc = read_register(0xdc, &c->dc, 1);
        }
        s_compat_id = s_cycles[0].da; /* Preserve the existing selection ABI. */
    }
    const int pins[] = {PIN_LCD_CS, PIN_LCD_PCLK, PIN_LCD_D0,
                        PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        remember_error(&s_setup_error, gpio_reset_pin(pins[i]));
    s_cached = true;
    return s_compat_id;
}

static bool all_bytes_are(uint8_t expected)
{
    for (int i = 0; i < PROBE_CYCLES; i++) {
        const probe_cycle_t *c = &s_cycles[i];
        for (size_t j = 0; j < sizeof(c->reg04); j++)
            if (c->reg04[j] != expected) return false;
        if (c->da != expected || c->db != expected || c->dc != expected) return false;
    }
    return true;
}

void board_lcd_probe_print(void)
{
    const char *classification = "unknown";
    const char *evidence = "mixed_or_unverified_readback";
    const char *confidence = "unknown";
    bool da_all_86 = s_cached;
    bool da_all_same = s_cached;
    for (int i = 0; i < PROBE_CYCLES; i++)
        da_all_86 = da_all_86 && s_cycles[i].err_da == ESP_OK && s_cycles[i].da == 0x86;
    for (int i = 0; i < PROBE_CYCLES; i++)
        da_all_same = da_all_same && s_cycles[i].err_da == ESP_OK &&
                      s_cycles[i].da == s_cycles[0].da;
    if (!s_cached) evidence = "probe_not_run";
    else if (s_setup_error != ESP_OK) evidence = "gpio_error";
    else if (all_bytes_are(0x00)) evidence = "all_raw_bytes_00";
    else if (all_bytes_are(0xff)) evidence = "all_raw_bytes_ff";
    else if (da_all_86) {
        classification = "SH8601";
        evidence = "five_DA_reads_match_Waveshare_0x86_selector";
        confidence = "selector_consistent_identity_unverified";
    } else if (da_all_same)
        evidence = "five_DA_reads_stable_but_non86_is_only_Waveshare_fallback";

    printf("PANEL_PROBE,{\"type\":\"lcd_probe\",\"cached\":%s,\"setup_error\":%ld,"
           "\"compat_id\":\"%02X\",\"classification\":\"%s\","
           "\"evidence\":\"%s\",\"identity_confidence\":\"%s\","
           "\"source_limit\":\"Waveshare_documents_only_DA_86_vs_fallback_selection\","
           "\"wire\":{\"opcode\":\"03\",\"dummy_cycles\":0,"
           "\"dummy_cycles_verified\":false},\"cycles\":[",
           s_cached ? "true" : "false", (long)s_setup_error, s_compat_id,
           classification, evidence, confidence);
    for (int i = 0; i < PROBE_CYCLES; i++) {
        const probe_cycle_t *c = &s_cycles[i];
        printf("%s{\"n\":%d,\"04\":[\"%02X\",\"%02X\",\"%02X\",\"%02X\"],"
               "\"DA\":\"%02X\",\"DB\":\"%02X\",\"DC\":\"%02X\","
               "\"errors\":{\"04\":%ld,\"DA\":%ld,\"DB\":%ld,\"DC\":%ld}}",
               i ? "," : "", i + 1, c->reg04[0], c->reg04[1], c->reg04[2],
               c->reg04[3], c->da, c->db, c->dc, (long)c->err04,
               (long)c->err_da, (long)c->err_db, (long)c->err_dc);
    }
    printf("],\"post_init_status\":{\"supported\":false,"
           "\"reason\":\"no_verified_non_disruptive_read_path_on_owned_QSPI_bus\"}}\n");
}
