/* Call collectors for app semantic tests. These deliberately do not rasterize;
 * the existing native tile/blur tests own pixel continuity assertions. */
#ifndef SETTINGS_SEMANTIC_DRAW_STUBS_H
#define SETTINGS_SEMANTIC_DRAW_STUBS_H
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static struct { int absent, dots, texts, pills, circles; char text[4096]; } test_draw;
static const font_t test_font={.ascent=16};
const font_t *UI_F_BODY=&test_font,*UI_F_LABEL=&test_font,*UI_F_SMALL=&test_font;
const font_t *UI_F_TITLE=&test_font,*UI_F_DISPLAY=&test_font,*UI_F_HERO=&test_font;
uint16_t nos_c_red=0xD0C4,nos_c_red_d=0xB083,nos_c_green=0x4CEB,nos_c_amber=0xD548;
static void test_text(const char *s)
{
    size_t n=strlen(test_draw.text);
    snprintf(test_draw.text+n,sizeof test_draw.text-n,"%s\n",s?s:"");
    test_draw.texts++;
}
int ui_text_width(const font_t *f,const char *s,int tracking) { return (int)strlen(s)*(8+tracking); }
int ui_label_width(const char *s) { return (int)strlen(s)*10; }
int ui_text(gfx_fb *f,const font_t *font,int x,int y,const char *s,uint16_t c,int align,int tracking) { test_text(s); return ui_text_width(font,s,tracking); }
int ui_label(gfx_fb *f,int x,int y,const char *s,uint16_t c,int align) { test_text(s); return ui_label_width(s); }
int ui_label_unit(gfx_fb *f,int x,int y,const char *s,const char *u,uint16_t c,int align) { test_text(s); test_text(u); return 0; }
void ui_state(gfx_fb *f,int x,int y,const char *s,uint16_t c) { test_text(s); }
void ui_title(gfx_fb *f,const char *s,float scroll) {}
void ui_absent(gfx_fb *f,int icon,const char *head,const char *body,const char *footer,float p)
{ test_draw.absent++; test_text(head); test_text(body); test_text(footer); }
uint16_t ui_dim(uint16_t c,float k) { return c; }
uint16_t ui_mix(uint16_t a,uint16_t b,float t) { return t<.5f?a:b; }
void ui_rrect(gfx_fb *f,float x,float y,float w,float h,float r,uint16_t c) {}
void ui_rrect_line(gfx_fb *f,float x,float y,float w,float h,float r,float t,uint16_t c) {}
void ui_circle_line(gfx_fb *f,float x,float y,float r,float t,uint16_t c) {}
void ui_button(gfx_fb *f,float x,float y,float w,float h,const char *s,int kind,int pressed) { test_text(s); }
void ui_slider_p(gfx_fb *f,float x,float y,float w,float h,float v,int icon,int pressed) {}
void ui_segments(gfx_fb *f,float x,float y,float w,float h,int n,float v,uint16_t a,uint16_t b) {}
void ui_scrollbar(gfx_fb *f,float o,float c,float v,float a) {}
void ui_row_span(float y,float margin,int *x0,int *x1) { *x0=UI_ROW_X0; *x1=UI_ROW_X1; }
float ui_list_offset(const ui_list *l) { return l->sc.x; }
void ui_list_tick(ui_list *l,float dt) {}
int ui_fblur_draw(gfx_fb *f,const void *id,int i,int y,float fade) { return 0; }
void ui_fblur_tick(const void *id,int held,float step,int i0,int i1,int n,int h,int x0,int x1,int bands,ui_blur_row_fn fn,void *ctx) {}
int ui_fblur_active(const void *id) { return 0; }
void ui_smear_disc(gfx_fb *f,const ui_trail *t,float r,uint16_t c,float step,int ghosts) {}
int dot_text_a(gfx_fb *f,const dot_style *st,float x,float y,const char *s,int align) { test_text(s); return 0; }
int dot_text_anim(gfx_fb *f,const dot_style *st,float x,float y,const char *s,int align,float p,int mode,unsigned seed) { test_text(s); return 0; }
int dot_text_morph(gfx_fb *f,const dot_style *st,float x,float y,const char *from,const char *to,int align,float p) { test_text(from); test_text(to); return 0; }
void dot_disc(gfx_fb *f,float x,float y,float d,uint16_t c) { test_draw.dots++; }
void dot_icon(gfx_fb *f,int icon,float x,float y,float pitch,float dot,uint16_t c) {}
void dot_ring(gfx_fb *f,float x,float y,float r,int n,float d,float l0,float l1,uint16_t on,uint16_t off) {}
void dot_bar(gfx_fb *f,float x,float y,int n,float pitch,float d,float lit,uint16_t on,uint16_t off) {}
void dot_columns(gfx_fb *f,float x,float y,int cols,int rows,float pitch,float d,const uint8_t *v,uint16_t on,uint16_t off) {}
void gfx_fill_circle(gfx_fb *f,float x,float y,float r,uint16_t c) { test_draw.circles++; }
void gfx_fill_pill(gfx_fb *f,float xa,float y,float xb,float r,uint16_t c) { test_draw.pills++; }
float gfx_fast_sqrt(float x) { return sqrtf(x); }
int nos_subview(const gfx_fb *f,gfx_fb *sub,int x0,int y0,int x1,int y1,int dx,int dy) { return 0; }
#endif
