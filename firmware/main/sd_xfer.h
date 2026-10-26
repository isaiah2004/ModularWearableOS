/* sd_xfer.h — SD card file transfer commands (sd_xfer.c) and their transports (USB, ble_xfer.c). */
#pragma once
#include <stdint.h>

typedef struct {
    int (*read)(uint8_t *b, int n, int timeout_ms);  /* raw upload bytes: count read, <= 0 on timeout */
    void (*line)(const char *s);                    /* one reply line (no newline) */
    int block;                                      /* upload block size, acked with SDK */
} xfer_io;

int sd_xfer_exec(const char *line, const xfer_io *io);   /* 1 if it was a transfer command */
int sd_xfer_line(const char *line);                      /* USB: the serial command task */
/* Call after mount has completed and released the SD mutex. Acquires its own
 * mount lease; completes a synced ready publication or preserves all evidence. */
int sd_xfer_recover(void);
void ble_xfer_set_waiter(void *task);                    /* task to notify when a BLE command is queued */
int ble_xfer_poll(void);                                 /* BLE: run a queued command, 1 if one ran */
