/* prefs.h — persisted user preferences (one blob in hal_store, key "nosprefs"). */
#ifndef NOS_PREFS_H
#define NOS_PREFS_H
#include <stdint.h>

typedef struct {
    uint8_t version;
    uint8_t haptics;        /* 0 off, 1 on */
    uint8_t haptic_strength;/* 0-100 */
    uint8_t sound;          /* UI sounds */
    uint8_t volume;         /* 0-100 */
    uint8_t brightness;     /* 0-100 user level */
    uint8_t auto_bright;    /* follow the APDS-9960 ambient light sensor */
    uint8_t aod;            /* always-on display */
    uint8_t raise_wake;     /* raise-to-wake via the IMU */
    uint8_t wrist_left;
    uint8_t hour24;
    uint8_t dnd;            /* do not disturb: banners silent */
    uint8_t face;           /* watch face index */
    uint8_t face_accent;    /* 0 red, 1 white */
    uint8_t sleep_s;        /* screen timeout, seconds (5-60) */
    uint8_t torch;          /* runtime only, not persisted meaningfully */
    uint8_t wifi, ble, lte, gps_auto;
    int16_t tz_min;         /* display timezone offset */
    uint16_t step_goal_100; /* step goal / 100 */
    uint8_t  move_goal_10;  /* active minutes goal / 10... reserved */
    uint8_t  units_metric;
    uint8_t  reserved[16];
} nos_prefs_t;

extern nos_prefs_t nos_prefs;
void prefs_defaults(void);
void prefs_load(void);
void prefs_save(void);          /* coalesced: writes at most once per 2 s */
void prefs_tick(void);          /* call once per frame to flush a pending save */
#endif
