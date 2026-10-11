/* face.h — watch faces. The home screen owns navigation; a face only draws time and
 * complications, ticks its own animations, and reports complication taps.
 *
 * Faces are the most-seen pixels on the watch: they must look perfect at rest and
 * cost ~nothing per frame (damage tracking: only the changing digits/second dot
 * repaint). Each face has an AOD variant: < 10% lit pixels, no fills, no red fills,
 * and drifts by up to 4 px per minute (face_aod_shift) against burn-in. */
#ifndef NOS_FACE_H
#define NOS_FACE_H
#include "gfx.h"
#include "hal.h"

typedef struct {
    hal_datetime dt;        /* latched this frame (dt.millis valid) */
    int  steps, step_goal;
    int  bpm;               /* last heart rate, 0 none */
    int  battery, charging;
    int  unread;            /* unread notifications */
    int  weather_valid; float temp_c; int wx_cond;
    int  next_alarm_min;    /* minutes to next alarm, -1 none */
    float t;                /* nos_t */
    int  aod;               /* draw the always-on variant */
    float enter;            /* 0..1 build-in progress after the face appears/wakes */
} face_ctx;

typedef struct {
    const char *name;                       /* "DOT", "GLYPH", ... shown in the picker */
    void (*enter)(void);                    /* face became visible (build-in restarts) */
    void (*tick)(const face_ctx *c, float dt);
    void (*draw)(gfx_fb *fb, const face_ctx *c);
    /* Return a screen to open for a tap at (x, y), or 0 for none. */
    const struct nos_screen *(*tap)(int x, int y);
} nos_face;

extern const nos_face *const nos_faces[];
extern const int nos_face_count;
void face_aod_shift(int *dx, int *dy);      /* current burn-in drift */
#endif
