/* Notification store. All access is on the UI thread; device transports
 * enqueue in the HAL. This is a companion protocol, not native ANCS. */
#ifndef WATCH_NOTIF_H
#define WATCH_NOTIF_H

#include <stdint.h>

typedef struct {
    char app[20];
    char title[40];
    uint16_t accent;
} notif_t;

int            notif_count(void);
const notif_t *notif_get(int i);
void           notif_push(const char *app, const char *title, uint16_t accent);
void           notif_clear(void);
uint32_t       notif_sequence(void); /* arrivals, including after the store is full */

#endif
