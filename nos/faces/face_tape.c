/* face_tape.c — STUB; the faces team replaces this file. */
#include "faces/face.h"
#include "ui.h"
#include "dot.h"
static void draw(gfx_fb *fb, const face_ctx *c)
{
    char b[8];
    ui_fmt_time(b, sizeof b, c->dt.hour, c->dt.minute);
    dot_style st = { DOT_FONT_TALL, 14, 11, C_WHITE, 0, 1, 1 };
    dot_text_a(fb, &st, UI_CX, 170, b, 1);
    ui_label(fb, UI_CX, 330, "TAPE", C_SEC, UI_CENTER);
}
const nos_face face_tape = { "TAPE", 0, 0, draw, 0 };
