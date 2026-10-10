/* ota.h — over-the-air firmware updates into the inactive ota_0/ota_1 slot.
 * Three sources, all ending in the same verify -> switch boot slot -> reboot:
 *   - HTTP upload: the watch's web page /update (phone browser picks a .bin)
 *   - URL pull:    `ota URL` over serial or /ota_url (watch downloads it itself)
 *   - SD card:     `ota sd` flashes /sdcard/update.bin, renamed afterwards
 * The bootloader rolls back to the previous slot if the new image never calls
 * ota_mark_valid() (crash or boot loop before the UI comes up). */
#ifndef WATCH_OTA_H
#define WATCH_OTA_H

#include "esp_http_server.h"

void ota_mark_valid(void);                 /* call once the UI is running */
void ota_register_web(httpd_handle_t h);   /* GET /update, POST /ota, POST /ota_url */
int  ota_from_url(const char *url);        /* 0 on success (reboots), else esp_err_t */
int  ota_from_sd(void);                    /* same, from /sdcard/update.bin */
void ota_print_status(void);

#endif
