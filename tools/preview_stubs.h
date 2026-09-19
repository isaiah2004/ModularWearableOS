/* preview_stubs.h — shared HAL + wos-registry stubs and a BMP writer for the
 * standalone app/face previewers. Include once per previewer .c (each builds
 * its own exe, so single definitions are fine). Do NOT link core/wos.c or a
 * platform HAL alongside these — the stubs stand in for them. */
#ifndef PREVIEW_STUBS_H
#define PREVIEW_STUBS_H
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../core/gfx.h"
#include "../core/hal.h"
#include "../core/wos.h"
#include "../core/icons.h"
#include "weather_preview.h"

uint32_t g_ms = 0;   /* animation clock; the previewer main advances it */
uint32_t wos_frame_id = 1;   /* faces read this; no wos loop in previews */
int g_brightness=180;

uint32_t hal_millis(void) { return g_ms; }
uint32_t hal_micros(void) { return g_ms * 1000; }
void hal_frame_begin(void) {}
void hal_frame_end(void) {}
void hal_get_datetime(hal_datetime *o) {
    o->year = 2026; o->month = 7; o->day = 12; o->weekday = 0;   /* Sunday */
    o->hour = 10; o->minute = 9; o->second = (g_ms / 1000) % 60; o->millis = g_ms % 1000;
}
int  hal_battery_percent(void) { return 72; }
void hal_power_get(hal_power *out) {*out=(hal_power){3900,72,1,0,1,0};}
void hal_activity_get(hal_activity *out) {*out=(hal_activity){6234,2520,0,1};}
hal_conn hal_status(int feature) {(void)feature;return HAL_CONN_UP;}
void hal_set_backlight(uint8_t l) { (void)l; }
void hal_log(const char *m) { (void)m; }
int  hal_wifi_scan(hal_wifi_ap *o, int m) {
    static const hal_wifi_ap f[] = { {"HomeNet-5G",-48,36},{"TP-Link_A2",-61,6},
        {"makerspace",-67,11},{"SKY9821",-72,1},{"eduroam",-83,44} };
    int n = 5; if (n > m) n = m; for (int i=0;i<n;i++) o[i]=f[i]; return n;
}
int  hal_imu_read(hal_imu *o) {
    float s = g_ms/1000.0f; o->ax=0.2f*sinf(s); o->ay=0.2f*cosf(s*0.8f);
    o->az=1.0f; o->gx=o->gy=o->gz=0; return 1;
}
int  hal_audio_bars(uint8_t *l, int n) {
    float s=g_ms/1000.0f; for(int i=0;i<n;i++){float v=0.5f+0.5f*sinf(s*5+i);l[i]=(uint8_t)(v*255);} return n;
}
int  hal_ble_scan(hal_ble_dev *o, int m) {
    static const hal_ble_dev f[] = { {"Galaxy Buds","5C:3A:45:1B:2E:0F",-52},
        {"Mi Band 7","D8:1F:12:9A:44:70",-61},{"JBL Flip 6","10:94:BB:5C:0A:22",-68},
        {"AirPods Pro","A4:83:E7:2D:66:91",-74} };
    int n = 4; if (n > m) n = m; for (int i=0;i<n;i++) o[i]=f[i]; return n;
}
int  hal_step_count(void) { return 6234; }
void hal_haptic(int ms) { (void)ms; }
int  hal_mic_level(void) { return 30; }
int  hal_mic_pitch(void) { return 0; }
int hal_audio_available(void) { return 0; }
void hal_storage_get(hal_storage *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->message, sizeof out->message, "Preview: no SD card");
}
void *hal_big_alloc(uint32_t b) { return malloc(b); }

/* fake app registry so the launcher / app-list render a realistic set */
static const char *FK_N[8] = { "Clock","Timer","Weather","Music","Wi-Fi","Calc","Maps","Photos" };
static const uint8_t FK_I[8] = { ICON_CLOCK,ICON_TIMER,ICON_WEATHER,ICON_MUSIC,ICON_WIFI,ICON_NONE,ICON_NONE,ICON_NONE };
int         wos_app_count(void)   { return 8; }
const char *wos_app_name(int i)   { return FK_N[i & 7]; }
int         wos_app_icon(int i)   { return FK_I[i & 7]; }
uint16_t    wos_app_accent(int i) {
    static const uint16_t c[8] = { GFX_ACCENT,GFX_ORANGE,GFX_ACCENT,GFX_PURPLE,
                                   GFX_GREEN,GFX_GREY,GFX_RED,GFX_PINK };
    return c[i & 7];
}
int  wos_current_app(void) { return 0; }
void wos_open_app(int i) { (void)i; }
void wos_close_overlay(void) { }

/* round-mask an RGB565 buffer and write it as a 24bpp BMP */
static void pv_save(const char *path, uint16_t *px) {
    int stride = (GFX_W * 3 + 3) & ~3;
    static uint8_t row[GFX_W * 3 + 4];
    float rad = GFX_W / 2.0f;
    for (int y=0;y<GFX_H;y++) for (int x=0;x<GFX_W;x++) {
        float dx=x+0.5f-rad, dy=y+0.5f-rad; if (dx*dx+dy*dy>rad*rad) px[y*GFX_W+x]=0;
    }
    uint8_t fh[14]={0}, ih[40]={0}; int fsz=54+stride*GFX_H;
    fh[0]='B';fh[1]='M';fh[2]=fsz;fh[3]=fsz>>8;fh[4]=fsz>>16;fh[5]=fsz>>24;fh[10]=54;
    ih[0]=40;ih[4]=GFX_W;ih[5]=GFX_W>>8;int nh=-GFX_H;ih[8]=nh;ih[9]=nh>>8;ih[10]=nh>>16;ih[11]=nh>>24;ih[12]=1;ih[14]=24;
    FILE *f=fopen(path,"wb"); if(!f) return; fwrite(fh,14,1,f); fwrite(ih,40,1,f);
    for (int y=0;y<GFX_H;y++){ for(int x=0;x<GFX_W;x++){uint16_t p=px[y*GFX_W+x];
        row[x*3+2]=((p>>11)&0x1F)*255/31; row[x*3+1]=((p>>5)&0x3F)*255/63; row[x*3+0]=(p&0x1F)*255/31;}
        fwrite(row,stride,1,f);}
    fclose(f); printf("wrote %s\n",path);
}
#endif
