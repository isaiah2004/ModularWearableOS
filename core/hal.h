/* hal.h — hardware abstraction contract.
 * Implemented by sim/main_win32.c on PC and firmware/main/hal_esp32.c on device.
 * Core code (gfx, wos, apps) may only touch hardware through this interface. */
#ifndef WATCH_HAL_H
#define WATCH_HAL_H

#include <stdint.h>
#include "weather.h"
#include "asset.h"

typedef struct {
    int year, month, day;     /* month 1-12, day 1-31 */
    int weekday;              /* 0 = Sunday */
    int hour, minute, second; /* 24h */
    int millis;               /* 0-999, for smooth animation */
} hal_datetime;

uint32_t hal_millis(void);                /* monotonic ms since boot */
uint32_t hal_micros(void);                /* monotonic us; for profiling (not frozen) */

/* Frame clock. While rendering a frame the OS latches the clock with
 * hal_frame_begin() so every stripe of a banded render sees ONE consistent
 * time — time-based animations advance exactly once per frame and never tear
 * between stripes. hal_frame_end() releases the latch. No-op-safe to nest is
 * not required; the OS calls them in matched pairs. */
void     hal_frame_begin(void);
void     hal_frame_end(void);

void     hal_get_datetime(hal_datetime *out);
void     hal_set_datetime(const hal_datetime *dt);  /* set the wall clock */
int      hal_set_datetime_checked(const hal_datetime *dt); /* validate and report write result */
int      hal_battery_percent(void);       /* 0-100 */
int      hal_battery_mv(void);            /* system-rail millivolts, 0 if unknown */
/* Evidence, not a fuel gauge: USB raises this board's sensed system rail.
 * external_power: -1 unknown, 0 inferred battery, 1 external supply observed.
 * battery_valid is false on external power or uncertain/stale samples. */
typedef struct {
    int rail_mv, battery_percent, battery_valid, external_power, sample_valid;
    uint32_t age_ms;
} hal_power;
void hal_power_get(hal_power *out);
uint64_t hal_epoch_seconds(void);
/* Display timezone, minutes east of UTC (NOS: Gadgetbridge time sync, Settings). */
void hal_set_tz_min(int tz_min);          /* trusted UTC, or 0 if unknown */
/* Battery history logger: `hal_batt_log_tick()` is called every main-loop iteration and self-times
 * a sample; the ring is dumped (delta-compressed) to the companion over BLE for offline analysis
 * (e.g. the "won't charge past 85%" issue). No-op on the sim. */
void     hal_batt_log_tick(void);

/* Settings/Wi-Fi use alternating checked SD checkpoints on device, with NVS
 * migration/card-absent fallback. Other keys use NVS; simulator uses memory/files.
 * Loads return 1 only for exactly `size` bytes. An unresolved SD read must not
 * silently replace durable settings with fallback data. */
void     hal_store_save(const char *key, const void *data, int size);
int      hal_store_save_checked(const char *key, const void *data, int size);
int      hal_store_load(const char *key, void *data, int size);
int      hal_store_load_pending(const char *key); /* unresolved authoritative read */
/* Opaque explicit-edit overlay, separate from the SD-authoritative checkpoint.
 * At most 256 bytes. Save verifies committed NVS readback and never clears SD
 * pending/dirty. Load: 1 exact bytes, 0 absent, -1 unresolved (buffer unchanged).
 * Portable prefs owns schema/field merge and writes an empty-mask tombstone. */
int      hal_prefs_overlay_save_checked(const void *data, int size);
int      hal_prefs_overlay_load(void *data, int size);
void     hal_set_backlight(uint8_t level);/* 0-255 */
void     hal_log(const char *msg);
/* In-watch log ring for the diagnostics log screen. `hal_log_count` = lines kept (newest last);
 * `hal_log_get(i)` returns line i (0 = oldest kept, count-1 = newest). Captures key system events
 * (BLE/time/Wi-Fi/errors). */
int         hal_log_count(void);
const char *hal_log_get(int i);
void        hal_log_clear(void);

/* Play a beep tone through the ES8311 speaker (freq in Hz, duration in ms). No-op where there is
 * no audio path. Also what hal_haptic falls back to (no vibration motor on this board). */
void        hal_beep(int freq_hz, int ms);

/* ---- find-my-{phone,puck} -------------------------------------------------
 * Both directions ride the EXISTING media characteristics, so no new GATT attribute is added
 * (a new one would be invisible to an already-paired Windows host — see the GATT-cache trap).
 *   watch -> companion : MEDIA_CMD notify code 7  -> companion rings ITSELF (find my phone/PC)
 *   companion -> watch : TIME char, 2-byte write [0xF1][seconds] -> the puck rings (find my puck)
 */
void hal_find_phone(void);          /* ask the paired companion to ring itself, loudly */
/* Milliseconds left on an inbound "ring the puck" request, else 0. The main loop polls this and
 * drives the noise/lights so the BLE callback never blocks. */
int  hal_locate_active(void);

/* Play a short 8-bit / chiptune-style sound sequence (square waves) by id. No-op on the sim. */
enum { SND_TICK = 0, SND_COIN, SND_SUCCESS, SND_ALARM, SND_TIMER, SND_STARTUP, SND_ERROR, SND_SELECT };
void        hal_play(int sound);

/* Passive Wi-Fi scan (read-only; no packet injection). Fills up to `max`
 * access points, returns the count. Real esp_wifi_scan on device, fake list
 * in the simulator. */
typedef struct {
    char ssid[33]; /* complete 32-byte SSID; truncate only its displayed label */
    int  rssi;    /* dBm */
    int  chan;
} hal_wifi_ap;

int hal_wifi_scan(hal_wifi_ap *out, int max);

/* ---- Wi-Fi air analyzer (promiscuous / monitor mode) -----------------------
 * OBSERVE-ONLY passive RF recon: we receive frames and count them, nothing else. No injection,
 * no deauth, no forged frames. This is what hal_wifi_scan CANNOT give you — a scan only hears
 * beacons, while monitor mode hears every frame on the air, so we can measure real per-channel
 * utilisation (the channel analyzer + heat map).
 * Monitor mode has to own the radio and hop channels, which breaks any association, so the STA
 * link is dropped while it runs and restored on stop. 2.4GHz only — the S3 has no 5GHz radio.
 * The analyzer AUTO-RELEASES if hal_wifi_air_get() stops being called (i.e. you navigated away),
 * so a backgrounded app can never strand the radio in monitor mode. */
#define HAL_AIR_NCH  13          /* channels 1..13 */
#define HAL_AIR_HIST 12          /* sweeps retained for the heat map */

typedef struct {
    uint16_t pkts;      /* frames heard per dwell on this channel (smoothed) */
    uint8_t  util;      /* 0-255 airtime utilisation estimate */
    int8_t   rssi_max;  /* strongest frame seen, dBm (0 = nothing heard) */
    uint16_t mgmt, data, ctrl;   /* frame-type mix */
} hal_air_chan;

int  hal_wifi_air_start(void);              /* enter monitor mode; 1 = running */
void hal_wifi_air_stop(void);               /* leave + restore the station link */
int  hal_wifi_air_running(void);
int  hal_wifi_air_channel(void);            /* channel currently being sniffed */
void hal_wifi_air_get(hal_air_chan *out);   /* HAL_AIR_NCH entries, channel 1 first */
/* Heat map history, row-major [channel][sample]; sample 0 = oldest, HIST-1 = newest, 0-255. */
void hal_wifi_air_hist(uint8_t *out);       /* HAL_AIR_NCH * HAL_AIR_HIST bytes */
/* Join a Wi-Fi network (own device). Prints "WIFI_IP: <ip>" over stdout once associated.
 * No-op on the simulator. */
void hal_wifi_connect(const char *ssid, const char *pass);
/* User-requested configuration. Setup stays available while STA joins. */
int hal_setup_set(int enabled);
void hal_setup_info(char *ssid, int sn, char *password, int pn, int *active);
void hal_wifi_info(char *text, int n);
void hal_wifi_set_enabled(int enabled);
/* UI-thread pump for queued phone settings and cached portal statistics. */
void hal_setup_poll(void);

/* 6-axis IMU (QMI8658). Accel in g, gyro in deg/s. Returns 1 on success. */
typedef struct {
    float ax, ay, az;
    float gx, gy, gz;
} hal_imu;

int hal_imu_read(hal_imu *out);
/* Raw, fixed-rate acquisition, independent of UI frames. One consumer (recorder).
 * UTC is Unix milliseconds, zero if the clock is not set. Stop retains unread
 * samples; start resets counters. Only 25/50/100 Hz are supported. */
typedef struct { uint64_t utc_ms; uint32_t ms; hal_imu v; } hal_imu_point;
typedef struct { unsigned hz, queued; uint32_t dropped, missed, errors; } hal_imu_stream;
int hal_imu_stream_start(unsigned hz);
void hal_imu_stream_stop(void);
int hal_imu_stream_read(hal_imu_point *out, int max);
void hal_imu_stream_get(hal_imu_stream *out);
/* Asynchronous SD recorder and browser. Requests return 0 if busy/unavailable.
 * Snapshot getters never access the card. IDLE/ERROR mean the file is closed. */
enum { HAL_IMU_IDLE, HAL_IMU_STARTING, HAL_IMU_RECORDING, HAL_IMU_STOPPING, HAL_IMU_ERROR };
typedef struct {
    int state; unsigned hz;
    uint32_t elapsed_ms, samples, dropped, missed, errors;
    uint64_t bytes;
    char name[16], message[48];
} hal_imu_record;
typedef struct {
    int busy, valid, id;
    uint32_t duration_ms;
    uint64_t bytes;
    char name[16], message[48];
} hal_imu_file;
int hal_imu_record_start(unsigned hz);
int hal_imu_record_stop(void);
void hal_imu_record_get(hal_imu_record *out);
/* direction: 0 newest, -1 older than id, +1 newer than id. */
int hal_imu_files_request(int id, int direction);
int hal_imu_file_delete(int id);
void hal_imu_file_get(hal_imu_file *out);
/* Fresh cached acquisition time, for motion algorithms that must not count a
 * sample twice. Timestamp uses the same wrapping millisecond clock as uptime. */
int hal_imu_sample(hal_imu *out,uint32_t *sample_ms);
/* Double knocks detected since boot (core/knock.c on the IMU stream): knock to lock. */
uint32_t hal_knock_count(void);

/* Cached hardware status; drawing must not perform filesystem operations. */
typedef struct {
    int mounted;
    uint64_t total_bytes, free_bytes;
    char message[48];
} hal_storage;
void hal_storage_get(hal_storage *out);
/* Try mounting the SD card again (a card inserted after boot). Asynchronous: the board
 * task mounts it and hal_storage_get() shows the result a moment later. */
void hal_storage_retry(void);
/* The watch's own flash, for Settings > Storage (filled once by the board task; zeros
 * until then). nvs_*: settings / activity store entries. */
typedef struct {
    uint32_t flash_bytes, app_bytes, app_slot_bytes;
    uint32_t nvs_used, nvs_total;
} hal_flash_info;
void hal_flash_info_get(hal_flash_info *out);
int hal_audio_available(void);

/* Ambient-audio spectrum for the visualizer: fills up to `n` bar levels
 * (0-255) and returns the count. Returns 0 where there's no microphone
 * (the plain 1.43 board has none — sim provides a demo signal). */
int hal_audio_bars(uint8_t *levels, int n);

/* Ambient microphone loudness envelope, 0-255 (0 = silent / no mic). Polling
 * this keeps the mic path alive; the device stops sampling (and powers the
 * ADC path down) a few seconds after the last call, so only screens that use
 * it pay for it. Sim returns a demo signal with periodic "clap" spikes. */
int hal_mic_level(void);

/* Rough pitch of the ambient sound in Hz (zero-crossing estimate; 0 = too
 * quiet to tell). Only meaningful while hal_mic_level() is being polled. */
int hal_mic_pitch(void);

/* On-demand BLE discovery. Copies a recent snapshot without blocking; polling
 * renews a short scan lease. Friendly names include scan responses. */
typedef struct {
    char name[24];
    char addr[18];   /* "AA:BB:CC:DD:EE:FF" */
    int  rssi;       /* dBm */
} hal_ble_dev;

int hal_ble_scan(hal_ble_dev *out, int max);
/* Logical radio Off: stop all GAP activity/connections asynchronously; stack
 * memory is retained. Calling before radio initialization sets the boot intent. */
void hal_ble_set_enabled(int enabled);
int hal_ble_connect_device(const char *addr);
void hal_ble_disconnect_device(void);
void hal_ble_device_status(char *text, int n);
/* Last BLE pairing outcome as a short string (for the `bleinfo` serial command). */
void hal_ble_status(char *buf, int n);

/* Now-playing media relayed from a paired phone/PC over BLE. hal_media_get returns 1
 * when live info is present (fills title/artist/playing), else 0 — apps fall back to a
 * local mockup. hal_media_cmd sends a transport command back to the source:
 * 0=play/pause, 1=next, 2=prev, 3=vol+, 4=vol-. No-op on the simulator. */
int  hal_media_get(char *title, int tn, char *artist, int an, int *playing);
void hal_media_cmd(int cmd);
/* Track time in MILLISECONDS; position is interpolated locally each frame (smooth ring).
 * Returns 1 if known, else 0. */
int  hal_media_time(int *pos_ms, int *dur_ms);
/* Ask the source to seek the current track to pos_sec (bezel scrub). No-op on the sim. */
void hal_media_seek(int pos_sec);
/* Set the host's master output volume, pct 0-100 (volume knob). No-op on the sim. */
void hal_media_vol(int pct);
/* Palette picked from the album art (progress ring, the two name-banner bgs, and their
 * auto-contrast text colours), all RGB565. Returns 1 if a palette is set, else 0. */
int  hal_media_colors(uint16_t *ring, uint16_t *artist_bg, uint16_t *song_bg,
                      uint16_t *artist_txt, uint16_t *song_txt);
/* Album art pushed over Wi-Fi (POST /cover), raw RGB565 of `*px` by `*px` pixels.
 * Both accessors pin an immutable image or return NULL. Release each successful
 * acquisition after pixel reads; the legacy name follows the same pin contract.
 * `*gen` changes only after complete publication. Simulator returns NULL. */
const uint16_t *hal_media_cover(int *px, unsigned *gen);
/* Pin across pixel reads; a published buffer is immutable until released. */
const uint16_t *hal_media_cover_acquire(int *px, unsigned *gen);
void hal_media_cover_release(const uint16_t *pixels);

/* ---- connectivity / feature status (control-panel grid) ------------------
 * Per-feature link state. OFF = feature disabled/not present; DOWN = enabled but
 * disconnected; TRYING = connecting; UP = connected/ready. */
typedef enum { HAL_CONN_OFF = 0, HAL_CONN_DOWN, HAL_CONN_TRYING, HAL_CONN_UP } hal_conn;
enum { HAL_FEAT_BLE = 0, HAL_FEAT_WIFI, HAL_FEAT_NET, HAL_FEAT_SYNC,
       HAL_FEAT_AI, HAL_FEAT_GPS, HAL_FEAT_COUNT };
hal_conn hal_status(int feature);

/* City + coordinates for the watch, derived by the companion from the PC's public IP (there is no
 * GPS chip). Fills `city` (up to n) and lat/lon; returns 1 if a fix is known, else 0. */
int hal_location(char *city, int n, float *lat, float *lon);

/* Enter/leave always-on-display mode (control-panel button / power gesture). */
void hal_set_aod(int on);

/* Force a clock + location re-sync now (control-panel resync button): asks the companion to push
 * time+location again over BLE, and re-kicks NTP if on Wi-Fi. No-op on the sim. */
void hal_time_resync(void);

/* Conservative daily cadence estimates from the QMI8658, persisted on device.
 * Activity continues when another app is visible; rollover uses local date. */
typedef struct {
    uint32_t steps, active_seconds;
    int walking, valid;
} hal_activity;
void hal_activity_get(hal_activity *out);
int hal_step_count(void);

/* Incoming companion notifications: producers enqueue; the UI thread drains.
 * Post rejects oversize/empty/control-character payloads rather than truncating. */
int hal_notification_post(const char *app, const char *title);
int hal_notification_poll(char *app, int an, char *title, int tn);

/* Haptic pulse of `ms` milliseconds (no-op where there's no motor). */
void hal_haptic(int ms);

/* Allocate a large buffer (device: external PSRAM, not scarce internal SRAM;
 * sim: plain malloc). Used for full-screen layer caches. NULL on failure. */
void *(hal_big_alloc)(uint32_t bytes); /* safe if psram.h's call macro was included first */
#include "psram.h"
/* Where the SD card's files live (device: "/sdcard"; sim: "build/sd" under the working dir).
 * Only read files outside draw() (tick, enter): a slice per frame. */
const char *hal_sd_root(void);

#endif
