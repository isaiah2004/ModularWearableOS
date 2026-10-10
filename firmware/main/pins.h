/* Waveshare ESP32-S3-Touch-AMOLED-1.43 / 1.43-B (metal case), default, or the
 * 1.43C with CONFIG_WATCH_BOARD_143C. Official demos and schematics; see
 * docs/HARDWARE.md and build/c143/hardware_diff.md. */
#ifndef WATCH_PINS_H
#define WATCH_PINS_H
#include "sdkconfig.h"
#if CONFIG_WATCH_BOARD_143C
/* 1.43C: CO5300 on a reshuffled QSPI bus, CST820 touch, no IMU/RTC/SD.
 * VCI_EN is pulled up on the board: there is no LCD enable pin, and GPIO42
 * is the ES7210 data output here, so it must never be driven. */
#define WATCH_BOARD_NAME "ESP32-S3-Touch-AMOLED-1.43C"
#define PIN_LCD_CS     15
#define PIN_LCD_PCLK   14
#define PIN_LCD_D0     9
#define PIN_LCD_D1     10
#define PIN_LCD_D2     11
#define PIN_LCD_D3     12
#define PIN_LCD_RST    13
#define PIN_LCD_TE     8  /* routed; enabled by 35h but not yet used for sync */
#define LCD_SPI_HZ     (40 * 1000 * 1000)
#define LCD_USE_SH8601  0
#define PIN_TP_SDA     47
#define PIN_TP_SCL     48
#define PIN_TP_RST     16
#define PIN_TP_INT     17
#define TOUCH_ADDR     0x15 /* CST820 */
#define BOARD_HAS_IMU  0
#define BOARD_HAS_RTC  0
#define BOARD_HAS_SD   0
#define BAT_ADC_CHANNEL ADC_CHANNEL_3 /* GPIO4, VBAT / 2 (200K/200K) */
#define BAT_DIVIDER    2
#define PIN_CHG_STAT   7  /* ETA6098 STAT, low = charging */
#define PIN_LED        5  /* active low; kept off */
#else
#define WATCH_BOARD_NAME "ESP32-S3-Touch-AMOLED-1.43-B"
#define PIN_LCD_CS     9
#define PIN_LCD_PCLK   10
#define PIN_LCD_D0     11
#define PIN_LCD_D1     12
#define PIN_LCD_D2     13
#define PIN_LCD_D3     14
#define PIN_LCD_RST    21
#define PIN_LCD_EN     42
/* No TE, touch reset or touch interrupt routed on this board. */
#define LCD_SPI_HZ     (40 * 1000 * 1000)
/* Working Demo V3 CO5300 profile. Forced SH8601 produced a black screen.
 * This selects initialization, not a confirmed controller identity. */
#define LCD_USE_SH8601  0
#define PIN_TP_SDA     47
#define PIN_TP_SCL     48
#define TOUCH_ADDR     0x38
#define IMU_ADDR       0x6b
#define RTC_ADDR       0x51
#define PIN_IMU_INT    8
#define PIN_RTC_INT    15
#define PIN_SD_CLK     41
#define PIN_SD_CMD     39
#define PIN_SD_D0      40
#define PIN_SD_D3      38 /* CS in SPI mode; unused in one-bit SDMMC */
#define BOARD_HAS_IMU  1
#define BOARD_HAS_RTC  1
#define BOARD_HAS_SD   1
#define BAT_ADC_CHANNEL ADC_CHANNEL_3 /* GPIO4, system rail / 3 */
#define BAT_DIVIDER    3
#endif
#endif
