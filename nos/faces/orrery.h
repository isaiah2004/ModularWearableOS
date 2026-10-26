/* orrery.h — shared by the ORRERY face (face_orrery.c), its planet cards (orrery_card.c)
 * and its events list (orrery_events.c). The design and the reference behaviour are the
 * reviewed prototype: docs/SOLAR_FACE.html + docs/assets/solar/watch.js. */
#ifndef ORRERY_H
#define ORRERY_H
#include "nos.h"
#include "ephem.h"
#include "faces/orrery_data.h"

/* card bodies, in the rim dial's order */
enum { OB_SUN, OB_MERCURY, OB_VENUS, OB_EARTH, OB_MOON, OB_MARS, OB_JUPITER, OB_SATURN, OB_URANUS, OB_NEPTUNE, OB_PLUTO, OB_N };
extern const char *const ob_name[OB_N];      /* "Sun", "Mercury", ... (title case, for the card title) */
extern const uint16_t ob_tint[OB_N];         /* the face's planet tints (RGB565) */
int ob_ephem(int ob);                        /* OB_* -> EP_* / EPB_* (ephem.h) */
int ob_of_planet(int ep);                    /* EP_* -> OB_* */

double orrery_jd(void);                      /* the face's time: now + any travel (Julian date) */
double orrery_now(void);                     /* the wall clock as a Julian date */
void   orrery_jump(double jd);               /* fly the face's time to jd (the events list) */
ep_vec orrery_pluto(double jd);              /* Pluto, heliocentric au (from the sky pack) */
const ep_event *orrery_events(int *n);       /* the face's scanned events (sorted, rank >= 2) */
void orrery_events_step(void);              /* UI-owner slice: at most four scan days */
unsigned orrery_events_generation(void);    /* changes on published rows/completion */
int orrery_events_progress(void);           /* 0..100; 100 means the window is complete */

void orrery_open_card(int ob, int x, int y); /* the planet card, iris from (x, y)  (orrery_card.c) */
void orrery_open_events(int x, int y);       /* the events list, iris from (x, y)  (orrery_events.c) */
extern const nos_screen scr_orrery_card, scr_orrery_events;
#endif
