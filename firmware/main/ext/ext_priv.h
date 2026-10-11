/* ext_priv.h — shared internals of the strap-module drivers (nos/hal_ext.h).
 *
 * Memory rule for this firmware: internal DRAM is nearly exhausted, so every
 * module keeps its state in ONE struct allocated in PSRAM at init (a 4-byte
 * pointer is all that lives in .bss). Task stacks are PSRAM too
 * (xTaskCreateWithCaps). Only DMA buffers that the IDF drivers insist on
 * (I2S, UART ring buffers) come from internal RAM, and only while the stream runs.
 *
 * Consequence of PSRAM stacks: the ext/audio tasks must never write flash (NVS),
 * because flash writes disable the cache that their stack lives behind. NVS saves
 * (magnetometer calibration) therefore happen in the caller's context.
 */
#ifndef NOS_EXT_PRIV_H
#define NOS_EXT_PRIV_H
#include <stdint.h>
#include <stddef.h>
#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_ext.h"

/* The strap is wired to the 1.43-B back headers. The 1.43C reuses IO5/7/16/17
 * for its LED, charger and touch, so the add-on drivers are compiled to "absent". */
#if CONFIG_WATCH_BOARD_143C
#define EXT_BOARD_OK 0
#else
#define EXT_BOARD_OK 1
#endif

/* ---- strap wiring (docs/strap_layout.html, `const pins`) ---- */
#define EXT_PIN_MOTOR     16
#define EXT_PIN_I2S_BCLK   1
#define EXT_PIN_I2S_WS     2
#define EXT_PIN_MIC_SD     3
#define EXT_PIN_AMP_DIN    5
#define EXT_PIN_AMP_SD     6
#define EXT_PIN_GPS_RX     7    /* ESP RX <- GPS TX */
#define EXT_PIN_GPS_TX    17    /* ESP TX -> GPS RX */
#define EXT_PIN_LTE_TX    43    /* U0TXD -> modem RXD */
#define EXT_PIN_LTE_RX    44    /* U0RXD <- modem TXD */
#define EXT_PIN_LTE_KEY   18
#define EXT_PIN_BUTTON     0    /* BOOT, active low, board pull-up */

#define EXT_I2C_HZ      400000
#define EXT_I2C_TMO_MS  20

extern portMUX_TYPE ext_lock;        /* guards every cached sample (tiny copies only) */
extern uint32_t ext_present_bits;    /* EXT_* bits that answered */

static inline uint32_t ext_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
/* wrap-safe "has `t` passed" */
static inline int ext_due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }
static inline int ext_idle(uint32_t now, uint32_t last) { return (uint32_t)(now - last) > NOS_EXT_IDLE_MS; }

void *ext_psram_calloc(size_t n);    /* zeroed PSRAM, falls back to NULL (never internal) */

/* I2C on the board's shared bus (SDA47/SCL48). The IDF master driver serialises
 * transactions per bus internally, so it is safe against board.c's touch/IMU task. */
i2c_master_bus_handle_t board_i2c_bus(void);
int  ext_i2c_open(uint8_t addr, i2c_master_dev_handle_t *dev);   /* probe + add; 1 = answered */
int  ext_rd(i2c_master_dev_handle_t d, uint8_t reg, void *out, size_t n);  /* 1 = ok */
int  ext_wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t v);
int  ext_wr_buf(i2c_master_dev_handle_t d, uint8_t reg, const uint8_t *v, size_t n);

/* ---- per-module entry points (called by ext_sys.c) ----
 * *_init: probe once at boot (fast; no blocking waits beyond short I2C timeouts).
 * *_service: called from the single "ext" task; returns ms until it wants the
 *   next call (the task sleeps for the minimum over all modules).
 * *_debug: print a one-line status for the `ext` serial command. */
void     haptic_init(void);
void     haptic_debug(void);
int      haptic_active(void);        /* motor driven now (compass skips samples) */

void     audio_init(void);
uint32_t audio_service(uint32_t now);    /* SD file I/O for playback / recording */
void     audio_debug(void);
int      audio_amp_on(void);
UBaseType_t audio_stack_free(void);

void     gps_init(void);
uint32_t gps_service(uint32_t now);
void     gps_debug(void);

void     lte_init(void);
uint32_t lte_service(uint32_t now);
void     lte_debug(void);

void     env_init(void);
uint32_t env_service(uint32_t now);
void     env_debug(void);

void     light_init(void);
uint32_t light_service(uint32_t now);
void     light_debug(void);

void     mag_init(void);
uint32_t mag_service(uint32_t now);
void     mag_debug(void);
void     mag_debug_cmd(const char *args);

void     hr_init(void);
uint32_t hr_service(uint32_t now);
void     hr_debug(void);

void     lte_debug_cmd(const char *args);
#endif
