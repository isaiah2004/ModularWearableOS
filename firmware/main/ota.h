/* ota.h — over-the-air firmware updates into the inactive ota_0/ota_1 slot.
 * Three sources, all ending in the same verify -> switch boot slot -> reboot:
 *   - HTTP upload: the watch's web page /update (phone browser picks a .bin)
 *   - URL pull:    POST /ota_url; ota_from_url is an internal API, not a NOS command
 *   - SD card:     ota_from_sd API reads /sdcard/update.bin, renamed afterwards
 * Every source requires the existing on-watch Phone setup session. HTTP writes
 * also require its per-session 128-bit token. One writer holds ownership through
 * validation, boot selection and reboot. Revoking/reopening setup invalidates a
 * previously claimed writer. Transfer work has a 180-second deadline, in addition
 * to the 10-minute setup expiry; a blocking SDK call can add its socket timeout.
 * The bootloader rolls back to the previous slot if the new image never calls
 * ota_mark_valid() (crash or boot loop before the UI comes up). */
#ifndef WATCH_OTA_H
#define WATCH_OTA_H

#include "esp_http_server.h"

void ota_mark_valid(void);                 /* call once the UI is running */
void ota_session_begin(void);             /* explicit on-watch Phone setup; 10 minutes */
void ota_session_end(void);
/* UI host calls after servicing preferences/workout/track/IMU each loop. Until
 * its first fresh snapshot, updates fail closed. A new claim needs a snapshot
 * no more than two seconds old. Changes that block updates abort a live writer. */
void ota_poll(void);
int  ota_busy(void);                      /* stop new recording/shutdown while claimed */
/* UI: pair around ALL state-changing service/input/frame/preference work.
 * Begin never waits: 0 means a writer owns OTA or has offered a bounded claim.
 * End publishes fresh ota_poll policy BEFORE owner 2 is released. On failed
 * begin keep cached read-only rendering, without ticks/input/services/prefs.
 * ota_session_end alone may revoke a writer from a physical button. Do not call
 * synchronous OTA APIs within an activity scope: only internal-stack workers
 * may wait up to 200 ms for the current host scope and then write flash. */
int  ota_activity_begin(void);
void ota_activity_end(void);
/* Command/transport scope only: begin offers the current owner 2 up to 200 ms,
 * with fair intent and no UI-object reads or setup-session requirement. An OTA
 * writer rejects immediately. End releases a successful begin, invalidating
 * policy until the next complete UI scope publishes it. Use for the common
 * USB/BLE sdput/sdput!/sdrm lifetime. Never call this waiting begin from UI. */
int  ota_external_activity_begin(void);
void ota_external_activity_end(void);
void ota_register_web(httpd_handle_t h);   /* GET /update, POST /ota, POST /ota_url */
/* API callers must use an internal-stack worker: flash disables PSRAM cache.
 * Do not call these synchronous streaming APIs on the render/UI thread. */
int  ota_from_url(const char *url);        /* 0 on success (reboots), else esp_err_t */
int  ota_from_sd(void);                    /* same, from /sdcard/update.bin */
void ota_print_status(void);

#endif
