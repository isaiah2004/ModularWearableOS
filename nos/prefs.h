/* prefs.h — RAM preferences; firmware checkpoints to paired SD files through
 * hal_store ("nosprefs"), with NVS migration/fallback for card-absent boots. */
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
    uint8_t  theme;         /* 0 dark, 1 light (the panel inverted while awake; AOD stays dark) */
    uint8_t  aod_bright;    /* always-on display brightness, 1-100 % (no light sensor to follow) */
    uint8_t  knock_lock;    /* 1 = knock twice to lock / unlock the screen */
    uint8_t  rev;           /* fields added inside `reserved` get their defaults when rev < theirs */
    uint8_t  sensor_idle;   /* rev 2: 80 MHz sensor-focused idle with 1 Hz display */
    /* Rev 3 occupies the original reserved bytes: persisted blob ABI unchanged.
     * Wrist and playing hand are independent. 0 unknown, 1 left, 2 right. */
    uint8_t  sports_dominant, sports_racket, sports_batting, sports_bowling;
    uint8_t  sports_volleyball, sports_mass_kg, sports_auto_resume;
    uint8_t  wrist_refs[4]; /* bilateral guided-turn calibration; not passive detection */
    /* V3 persisted 42 bytes (uint16_t alignment). V4's uint32_t inserts two
     * padding bytes, then appends metadata at offset44; legacy reads use42. */
    uint32_t wrist_revision;
    uint8_t wrist_source;
    uint8_t fps_cap;        /* rev 4: 24/30/40/48/60 FPS; 0 = unlimited */
    uint8_t display_te;     /* rev 5: wait for panel TE before a frame; default off */
    uint8_t wrist_reserved[1];
    /* Rev 6 appends the body profile (V4 blobs were 52 bytes; prefs.c still reads them).
     * Weight is sports_mass_kg, not duplicated here. 0 = unset in every field. */
    uint16_t body_birth_year;   /* 1900-2100; age is derived from the clock */
    uint8_t  body_height_cm;    /* 100-230 */
    uint8_t  body_sex;          /* 0 unset, 1 male, 2 female */
} nos_prefs_t;

extern nos_prefs_t nos_prefs;
/* Panel level (0-255) for a brightness percentage: the user's Brightness and the always-on
 * level share this curve (1-100 % -> 8-255). */
static inline int prefs_pct_level(int pct) { if (pct < 1) pct = 1; if (pct > 100) pct = 100; return (int)(6.0f + pct * 2.49f); }
#define PREFS_AOD_BRIGHT_DEF 10
void prefs_defaults(void);
void prefs_load(void);
void prefs_save(void);          /* 2 s debounce, 10 s maximum dirty age */
int prefs_pending(void);        /* UI-thread status: waiting for verified SD checkpoint */
/* Firmware permits debounced saves after DMA drains and input motion stops; simulators default to allowed.
 * Deferred data remains dirty until a checked SD write succeeds; retry every 5 s. */
void prefs_set_persistence_allowed(int allowed);
/* Explicit shutdown checkpoint: bypasses debounce/window; failure stays dirty. */
int prefs_flush(void);
void prefs_tick(void);          /* flush only in a host-approved maintenance window */
enum { PREFS_WRIST_SOURCE_UNKNOWN=0, PREFS_WRIST_SOURCE_MANUAL=1, PREFS_WRIST_SOURCE_GUIDED=2 };
void prefs_set_wrist(int left, int source);
int prefs_wrist_source(void);
uint32_t prefs_wrist_revision(void);
int prefs_loading(void);        /* unresolved SD read; do not replace durable settings */
#define PREFS_RESTORE_API 1
/* Nonblocking transfer ownership. Caller holds the shared SD mutex for begin
 * and end, and excludes another raw publisher for the complete transaction.
 * Dirty known RAM rejects restore. An explicitly restored unknown boot state
 * keeps its field edits for the eventual reload. A changed bank blocks stale
 * checkpoints until that UI reload; edits merge as complete fields. */
int prefs_restore_begin(void);
void prefs_restore_end(int changed);
#endif
