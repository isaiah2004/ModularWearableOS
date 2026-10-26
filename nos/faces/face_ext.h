/* face_ext.h — optional extras the home screen can ask of a face (implemented in
 * faces.c). Kept out of face.h so the nos_face contract stays as it is.
 *
 *  - Pressed state for complications: on IN_DOWN call nos_face_press_rect(); if it
 *    returns a screen, fill r[] (screen coordinates, x0 y0 x1 y1) with C_SURF2 at
 *    radius 16 until IN_UP (on a FACE_LIGHT face use #E0E0E0). An all-zero rect
 *    means "no visible target" (e.g. a bezel ring): skip the fill but keep the tap.
 *  - Style flags: FACE_LIGHT = the face is a white disc (draw page dots dark);
 *    FACE_FULL_BLEED = the face has art under the page-dot row (y 430-452 near the
 *    centre): give the dots their black capsule from the first frame of a drag. */
#ifndef NOS_FACE_EXT_H
#define NOS_FACE_EXT_H
#include "faces/face.h"
#include "nos.h"

enum { FACE_LIGHT = 1 << 0, FACE_FULL_BLEED = 1 << 1 };
int nos_face_style(const nos_face *f);
/* A face that takes touch itself (ORRERY: its holds, and every touch while unlocked).
 * Home offers each touch event at the face to it first; 1 = consumed, home skips it. */
int nos_face_input(const nos_face *f, const nos_input *in);
/* The active face owns touch and needs workspace ahead of unrelated list prewarm. */
int nos_face_interactive(const nos_face *f);
/* home's face picker, for a face that opens it from its own gesture (ORRERY: a 3 s hold) */
void home_open_faces(void);
const struct nos_screen *nos_face_press_rect(const nos_face *f, int x, int y, int r[4]);
/* UI thread between frames; default disabled. reset clears cumulative evidence. */
void orrery_card_profile_set(int enabled, int reset);
int orrery_card_profile_enabled(void);
void orrery_card_profile_report(void);
/* UI thread between frames: one dispatch per globe, default enabled when table exists. */
extern int orrery_card_lighting_cache;
/* No allocation: compare all 512 cached coefficients with original expressions.
 * Returns mismatch count; ~0u means card/table unavailable. */
unsigned orrery_card_lighting_check(void);
/* UI-thread A/B; defaults on, falls back while global table is unready/mismatched. */
extern int orrery_card_shade_q16;
/* Independent exact PHOTO Sun specialization, defaults on. */
extern int orrery_card_sun_fast;
/* No allocation: validate all actual card shade channels/gray against all512 multipliers.
 * Returns mismatches, or ~0u while card coefficients/global table are unavailable. */
unsigned orrery_card_shade_q16_check(void);
/* UI-thread bounded rendering controls; baseline remains available with flag0.
 * Work budget is clamped250..8000us; deterministic quantum cap also bounds host replay. */
extern int orrery_card_progressive;
extern int orrery_card_motion_proxy;
extern unsigned orrery_card_render_budget_us;
extern unsigned orrery_card_render_quanta;
/* Moving sampling lattice:2default or4 for target A/B; settled always1. */
extern unsigned orrery_card_motion_step;
void orrery_card_render_report(void);
/* Noallocation; quietdiagnostic full LUT/rings against original scalar expressions.
 * Returns entry mismatches, or~0u while idle fullgeometry is unavailable. */
unsigned orrery_card_geometry_check(void);
/* UI-only nonblocking reaper, also call during OFF/stable AOD. */
void orrery_card_asset_poll(void);
/* Read-only UI-thread diagnostics, including successful asset adoption. */
void orrery_card_report_state(void);
#endif
