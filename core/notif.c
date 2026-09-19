#include "notif.h"
#include <string.h>

#define MAX_NOTIF 8

static notif_t s_buf[MAX_NOTIF];
static int s_n;
static uint32_t s_sequence;

int notif_count(void) { return s_n; }
uint32_t notif_sequence(void) { return s_sequence; }

const notif_t *notif_get(int i)
{
    return (i >= 0 && i < s_n) ? &s_buf[i] : 0;
}

static void copy(char *dst, const char *src, int cap)
{
    int i = 0;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

void notif_push(const char *app, const char *title, uint16_t accent)
{
    s_sequence++;
    if (s_n < MAX_NOTIF) {
        /* newest first */
        for (int i = s_n; i > 0; i--) s_buf[i] = s_buf[i - 1];
        s_n++;
    } else {
        for (int i = MAX_NOTIF - 1; i > 0; i--) s_buf[i] = s_buf[i - 1];
    }
    copy(s_buf[0].app, app, sizeof s_buf[0].app);
    copy(s_buf[0].title, title, sizeof s_buf[0].title);
    s_buf[0].accent = accent;
}

void notif_clear(void) { s_n = 0; }
