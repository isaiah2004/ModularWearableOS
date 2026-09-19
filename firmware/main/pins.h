/* Waveshare ESP32-S3-Touch-AMOLED-1.43 / 1.43-B (metal case).
 * Official Demo V3 and docs; see docs/HARDWARE.md. NOT the 1.43C. */
#ifndef WATCH_PINS_H
#define WATCH_PINS_H
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
/* Owner-requested 80 MHz trial; physical display acceptance still required. */
#define LCD_SPI_HZ     (80 * 1000 * 1000)
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
#define BAT_ADC_CHANNEL ADC_CHANNEL_3 /* GPIO4, system rail / 3 */
#define BAT_DIVIDER    3
#endif
