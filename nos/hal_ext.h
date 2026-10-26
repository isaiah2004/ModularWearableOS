/* hal_ext.h — add-on module contract for the Nothing-inspired OS (NOS).
 *
 * The base board (Waveshare ESP32-S3-Touch-AMOLED-1.43-B) is covered by core/hal.h
 * (touch, QMI8658 IMU, PCF85063 RTC, SD, rail ADC, Wi-Fi, BLE). This header adds the
 * strap modules from docs/strap_layout.html, wired to the back headers J1/J3:
 *
 *   Module                        Bus     Pins (ESP32-S3 GPIO)
 *   Vibration motor (coin, via    GPIO    IO16 = motor IN (LEDC PWM, active high)
 *     driver transistor)
 *   INMP441 mic                   I2S RX  IO1 BCLK, IO2 WS, IO3 SD(data in)
 *   MAX98357A amp + 8R speaker    I2S TX  IO1 BCLK, IO2 LRC, IO5 DIN, IO6 SD(shutdown, low=off)
 *   GPS HGLRC M100 Mini (M10)     UART1   IO7 = ESP RX (GPS TX), IO17 = ESP TX (GPS RX)
 *   LTE Quectel EG800AK           UART0   U0RXD IO44 / U0TXD IO43, IO18 = PWRKEY
 *   BME280 env sensor             I2C     shared bus SDA47/SCL48, 0x76 or 0x77
 *   APDS-9960 light/prox/gesture  I2C     0x39
 *   QMC5883L compass (GY-271)     I2C     0x0D  (HMC5883L at 0x1E also accepted)
 *   MAX30102 heart rate / SpO2    I2C     0x57
 *   Cells A + B (2x 420 mAh)      VBAT    measured by the existing rail ADC (core/hal.h)
 *
 * Rules every implementation follows:
 *  - Every module is OPTIONAL. Probe at boot; hal_ext_present() says what answered.
 *    Absent modules return 0 / invalid samples and never block or spam logs.
 *  - Getters are non-blocking reads of a cached sample that a background task
 *    refreshes. UI code calls them every frame, so they must cost ~nothing.
 *  - Streams that cost power (HR LED, GPS, mic, LTE) only run while requested.
 *    *_enable(1) starts, *_enable(0) stops. A stream also auto-stops if its getter
 *    has not been called for NOS_EXT_IDLE_MS, so a forgotten screen cannot drain the cells.
 *  - The simulator (sim/) implements all of this with plausible synthetic data, and
 *    honours `hal_ext_sim_present` so absent-module states can be previewed.
 */
#ifndef NOS_HAL_EXT_H
#define NOS_HAL_EXT_H
#include <stdint.h>

#define NOS_EXT_IDLE_MS 8000

/* called once by the firmware after board init (I2C bus up, NVS ready): probes
 * the modules and starts the background service task. */
void hal_ext_init(void);
/* Idempotent asynchronous shutdown. Poll: 0 pending, 1 acknowledged clean,
 * -1 timeout/error (keep polling; never interpret a timeout as powered off). */
void hal_ext_shutdown_begin(void);
int  hal_ext_shutdown_poll(void);
/* Release quiesce admission after an in-flight shutdown hook returns. Owned
 * file finalization and already-requested hardware power-down still finish. */
void hal_ext_shutdown_cancel(void);
/* Serial hook: `ext` prints module status; `ext help` lists test sub-commands. */
void hal_ext_debug(const char *cmd);
int  hal_ext_debug_command(const char *cmd); /* 1 accepted/status, 0 rejected; async completion is separate */

enum {
    EXT_HAPTIC = 1 << 0,
    EXT_MIC    = 1 << 1,
    EXT_SPEAKER= 1 << 2,
    EXT_GPS    = 1 << 3,
    EXT_LTE    = 1 << 4,
    EXT_ENV    = 1 << 5,   /* BME280 */
    EXT_LIGHT  = 1 << 6,   /* APDS-9960 */
    EXT_MAG    = 1 << 7,   /* QMC5883L / HMC5883L */
    EXT_HR     = 1 << 8,   /* MAX30102 */
};
/* Bitmask of modules that answered their probe (haptic/speaker cannot be probed:
 * they report present when enabled in settings; default on). */
uint32_t hal_ext_present(void);
/* Simulator only: override presence for previewing empty states. */
extern uint32_t hal_ext_sim_present;

/* ---- haptics (vibration motor, IO16) --------------------------------------
 * Patterns are short and synchronous with UI events; the driver plays them on a
 * timer so callers never wait. A new pattern replaces the one playing. */
enum {
    HAPTIC_TICK = 0,   /* 8 ms crisp detent: list item crossing, crown-like steps */
    HAPTIC_CLICK,      /* 15 ms: button/tap confirmation */
    HAPTIC_DOUBLE,     /* click-pause-click: toggle on / success */
    HAPTIC_HEAVY,      /* 40 ms: long-press recognised, limit reached */
    HAPTIC_BUZZ,       /* 300 ms: notification */
    HAPTIC_ALARM,      /* repeating pulse train until hal_haptic_stop() */
    HAPTIC_HEARTBEAT,  /* lub-dub, used by the heart rate app */
};
void hal_haptic(int pattern);
void hal_haptic_stop(void);
void hal_haptic_set_strength(int pct);   /* 0-100, PWM duty scale */

/* ---- audio out (MAX98357A + 8 ohm speaker) --------------------------------
 * A tiny mixer: up to 2 voices of procedural sound (square/triangle/noise with
 * envelopes) for UI sounds, plus one PCM stream for memo playback. The amp's SD
 * pin is held low (off) whenever nothing plays. */
enum {
    SFX_TAP = 0, SFX_TICK, SFX_BACK, SFX_OPEN, SFX_TOGGLE_ON, SFX_TOGGLE_OFF,
    SFX_NOTIFY, SFX_ALARM, SFX_TIMER_DONE, SFX_SUCCESS, SFX_ERROR, SFX_CHARGE,
    SFX_BOOT, SFX_SHUTTER, SFX_COUNT
};
void hal_sfx(int id);
void hal_sfx_stop(void);
void hal_audio_volume(int pct);          /* 0-100 */
/* Play a 16 kHz mono 16-bit PCM file from SD (voice memo). 1 = started. */
int  hal_audio_play_file(const char *path);
int  hal_audio_playing(void);            /* 1 while a PCM stream plays */
int  hal_audio_play_result(void);         /* 0 pending/playing, 1 finished, -1 error */
uint32_t hal_audio_play_pos_ms(void);
void hal_audio_play_stop(void);

/* ---- microphone (INMP441) -------------------------------------------------- */
/* Loudness in dB SPL estimate (A-weighting not applied), smoothed 125 ms.
 * Returns -1 when no mic. Calling it keeps the mic running. */
float hal_mic_db(void);
/* 32 recent envelope samples 0-255, newest last (waveform display). */
int   hal_mic_wave(uint8_t out[32]);
/* Record 16 kHz mono 16-bit WAV to SD. Returns 1 if recording started. */
int   hal_rec_start(const char *path);   /* 1 = bounded request accepted */
int   hal_rec_start_exclusive(const char *path); /* 1 queued, -2 known collision, 0 rejected */
void  hal_rec_stop(void);
int   hal_rec_active(void);
uint32_t hal_rec_ms(void);
/* 0 idle, 1 queued/recording, 2 finalizing, 3 saved, 4 error. Terminal state is
 * latched until another request is accepted. Saved means checked final publish. */
int   hal_rec_status(void);
enum { HAL_REC_PENDING=0, HAL_REC_DONE=1, HAL_REC_ERROR=-1, HAL_REC_EXISTS=-2 };
int   hal_rec_result(void); /* latched result; collision can be discovered asynchronously */

/* ---- GPS (u-blox M10 NMEA) -------------------------------------------------- */
typedef struct {
    int   valid;           /* 1 = 2D/3D fix */
    int   fix3d;
    int   sats_used, sats_view;
    double lat, lon;       /* degrees */
    float alt_m;
    float speed_mps;
    float course_deg;      /* true course over ground, valid when moving */
    float hdop;
    int   utc_h, utc_m, utc_s;
    int   utc_valid;
    uint32_t age_ms;       /* since the last valid fix */
    uint8_t  snr[16];      /* up to 16 satellites in view, dB-Hz (0 = none), strongest first */
    uint8_t  el[16];       /* elevation of the same satellites, degrees 0-90 */
    uint16_t az[16];       /* azimuth, degrees 0-359 */
    int      nsnr;
    uint32_t sentences;    /* NMEA sentences with a good checksum since power-up (data arriving) */
} hal_gps;
void hal_gps_enable(int on);
int  hal_gps_get(hal_gps *out);          /* 1 = module present (fix may still be invalid) */

/* ---- LTE (Quectel EG800AK, AT over UART0) ---------------------------------- */
typedef struct {
    int  powered;          /* module on and answering AT */
    int  sim;              /* 0 none, 1 ready, 2 PIN locked */
    int  registered;       /* 0 no, 1 home, 2 roaming, 3 searching */
    int  rssi_dbm;         /* from AT+CSQ, 0 = unknown */
    int  bars;             /* 0-4 */
    char op[20];           /* operator name */
    char tech[8];          /* "LTE", "GSM" ... */
    int  unread_sms;
} hal_lte;
void hal_lte_enable(int on);             /* PWRKEY sequence; takes seconds, non-blocking */
int  hal_lte_get(hal_lte *out);          /* 1 = module present */
typedef struct { char from[24]; char text[160]; char when[20]; int unread; uint32_t id; } hal_sms;
int  hal_lte_sms_list(hal_sms *out, int max);   /* cached inbox, newest first */
int  hal_lte_sms_send(const char *to, const char *text);  /* 1 = queued */
void hal_lte_sms_mark_read(int i);        /* local overlay; prefer stable ID below */
uint32_t hal_lte_sms_send_request(const char *to, const char *text);
int hal_lte_sms_send_status(uint32_t id); /* 0 unknown,1 queued,2 sending,3 accepted by CMGS,4 failed */
void hal_lte_sms_mark_read_id(uint32_t id);

/* ---- environment (BME280) --------------------------------------------------- */
typedef struct {
    int   valid;
    float temp_c, humidity, pressure_hpa;
    float altitude_m;      /* from pressure and the reference below */
    int humidity_valid;    /* valid BME humidity; false for BMP/absent/stale */
    uint32_t sample_ms;    /* last successful acquisition; getters never renew it */
} hal_env;
#define NOS_ENV_MAX_AGE_MS 6000u
int  hal_env_get(hal_env *out);
void hal_env_set_sea_level(float hpa);   /* altimeter calibration */
/* 1 sample per 10 min for 24 h (144), oldest first; returns count. Barometer trend. */
int  hal_env_history(float *hpa_out, int max);

/* ---- light / proximity / gesture (APDS-9960) -------------------------------- */
typedef struct {
    int valid; float lux; int prox; /* 0-255 */
    int prox_valid;
    uint32_t sample_ms, prox_ms; /* independent successful ALS/proximity acquisitions */
} hal_light;
#define NOS_LIGHT_MAX_AGE_MS 2000u
int  hal_light_get(hal_light *out);
enum { GEST_NONE = 0, GEST_UP, GEST_DOWN, GEST_LEFT, GEST_RIGHT, GEST_NEAR, GEST_FAR };
int  hal_light_gesture(void);            /* pops the last air gesture, GEST_NONE if none */
void hal_light_gesture_enable(int on);

/* ---- compass (QMC5883L) ----------------------------------------------------- */
typedef struct {
    int   valid;
    float heading_deg;     /* 0 = magnetic north, clockwise, tilt-compensated with the IMU */
    float field_ut;        /* field strength, microtesla */
    int   calibrated;      /* hard-iron offsets learnt */
    float cal_progress;    /* 0-1 while calibrating (coverage of the sphere) */
} hal_mag;
int  hal_mag_get(hal_mag *out);
void hal_mag_calibrate(int start);       /* 1 = begin figure-8 learning, 0 = finish & save */

/* ---- heart rate / SpO2 (MAX30102) ------------------------------------------ */
typedef struct {
    int   on_wrist;        /* IR level says skin contact */
    int   bpm;             /* 0 until locked */
    int   spo2;            /* % or 0 */
    float quality;         /* 0-1 signal confidence */
    uint8_t wave[48];      /* recent PPG, 0-255, newest last (for the live trace) */
    uint32_t sample_ms;    /* monotonic acquisition time; 0 until a FIFO read succeeds */
} hal_hr;
void hal_hr_enable(int on);
enum { HAL_HR_OWNER_WORKOUT=1u, HAL_HR_OWNER_HEART=2u, HAL_HR_OWNER_BREATHE=4u,
       HAL_HR_OWNER_LEGACY=8u };
/* Each consumer releases only its own demand. Getters renew the shared lease. */
void hal_hr_request(unsigned owner, int on);
int  hal_hr_get(hal_hr *out);            /* 1 = module present */
/* Resting/periodic samples taken in the background, one per 10 min, 0 = none. */
int  hal_hr_history(uint8_t *bpm_out, int max);
typedef struct { uint32_t sample_ms, epoch; uint16_t bpm; } hal_hr_sample;
/* Producer timestamps stay fixed across UI refreshes. epoch=0 means untrusted UTC. */
int  hal_hr_history_samples(hal_hr_sample *out, int max);

/* ---- system ----------------------------------------------------------------- */
/* Side button: the OS polls this each frame. 1 = held down. */
int  hal_button_down(void);
/* Display power: 0 off, 1 AOD (low brightness), 2 on. */
void hal_display_power(int mode);
int  hal_display_mode(void);             /* actual current display power state */
/* Optional: free PSRAM/internal heap, for About/diagnostics. */
void hal_mem_info(uint32_t *psram_free, uint32_t *internal_free);
/* Monotonic counters for diagnostics: frames presented, late frames. */
#endif
