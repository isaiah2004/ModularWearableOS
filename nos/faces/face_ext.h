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

enum { FACE_LIGHT = 1 << 0, FACE_FULL_BLEED = 1 << 1 };
int nos_face_style(const nos_face *f);
const struct nos_screen *nos_face_press_rect(const nos_face *f, int x, int y, int r[4]);
#endif
