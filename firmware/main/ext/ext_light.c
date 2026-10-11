/* ext_light.c — APDS-9960 at 0x39: ambient light (lux), proximity, air gestures.
 *
 * Always-on low-rate cycle: proximity (8 x 16 us pulses @ 100 mA) -> ALS
 * (103 ms integration) -> 206 ms wait, read every 400 ms. ALS gain auto-ranges
 * 1/4/16/64x. Average draw is a few hundred uA.
 *
 * Lux is an estimate (the chip has no official formula): DN40-style IR
 * rejection, IR = (R+G+B-C)/2, lux = (G-IR) * DF * GA / (t_int_ms * gain), with
 * DF = EXT_APDS_LUX_DF and window attenuation GA = EXT_APDS_WINDOW_GA. Expect
 * +-30 % until calibrated against a lux meter through the real lid window.
 *
 * Proximity: raw PDATA minus a baseline that follows the floor (fast down, very
 * slowly up), which cancels the constant crosstalk of the lid window.
 *
 * Gestures (only while enabled; auto-off after NOS_EXT_IDLE_MS without
 * hal_light_gesture()): the chip's gesture engine fills a FIFO of U/D/L/R
 * datasets while something is above it. For each session the first and the
 * last dataset above the noise threshold give ratios ud=(U-D)/(U+D) and
 * lr=(L-R)/(L+R); the change between them gives the swipe direction (sign
 * convention as the SparkFun reference driver). A hover with no swipe for 600 ms
 * emits GEST_NEAR, and its end emits GEST_FAR. EXT_APDS_ROTATE (0-3, quarter
 * turns clockwise) adapts the directions to how the sensor sits in the strap.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ext_priv.h"

#ifndef EXT_APDS_LUX_DF
#define EXT_APDS_LUX_DF     310.0f
#endif
#ifndef EXT_APDS_WINDOW_GA
#define EXT_APDS_WINDOW_GA  1.0f
#endif
#ifndef EXT_APDS_ROTATE
#define EXT_APDS_ROTATE     0
#endif

#define R_ENABLE 0x80
#define R_ATIME  0x81
#define R_WTIME  0x83
#define R_PPULSE 0x8E
#define R_CTRL   0x8F
#define R_CONFIG2 0x90
#define R_ID     0x92
#define R_STATUS 0x93
#define R_CDATA  0x94
#define R_PDATA  0x9C
#define R_GPENTH 0xA0
#define R_GEXTH  0xA1
#define R_GCONF1 0xA2
#define R_GCONF2 0xA3
#define R_GPULSE 0xA6
#define R_GCONF3 0xAA
#define R_GCONF4 0xAB
#define R_GFLVL  0xAE
#define R_GSTATUS 0xAF
#define R_GFIFO  0xFC

#define ATIME_V  0xDB                 /* 37 cycles = 103 ms, max count 37925 */
#define ATIME_MS 102.8f
#define GTHRESH  10
#define GSWIPE   20                   /* ratio delta (percent) for a swipe */

typedef struct {
    i2c_master_dev_handle_t dev;
    uint8_t id;
    int again;                        /* index into GAINS */
    uint32_t next, last_gest_call;
    int gest_on, gest_want, errors;
    hal_light cur;
    float prox_base;
    /* gesture session */
    int sess, have_first, near_sent;
    uint32_t sess_t0, sess_last;
    int ud0, lr0, ud1, lr1;
    int q[4]; int qn;
    uint32_t gestures;
} light_state;
static light_state *Lt;
static const int GAINS[4] = { 1, 4, 16, 64 };

static int set_enable(void)
{
    uint8_t en = 0x01 | 0x02 | 0x04 | 0x08;          /* PON AEN PEN WEN */
    if (Lt->gest_on) en |= 0x40;                     /* GEN */
    return ext_wr(Lt->dev, R_ENABLE, en);
}
static int set_ctrl(void) { return ext_wr(Lt->dev, R_CTRL, (uint8_t)(0x08 | Lt->again)); }  /* LDRIVE 100mA, PGAIN 4x */

void light_init(void)
{
    i2c_master_dev_handle_t d;
    uint8_t id = 0;
    if (!ext_i2c_open(0x39, &d)) return;
    /* 0xAB genuine; 0xA8 and 0x9C are common clone IDs with the same register map */
    if (!ext_rd(d, R_ID, &id, 1) || (id != 0xAB && id != 0xA8 && id != 0x9C)) { i2c_master_bus_rm_device(d); return; }
    Lt = ext_psram_calloc(sizeof *Lt);
    if (!Lt) { i2c_master_bus_rm_device(d); return; }
    Lt->dev = d; Lt->id = id; Lt->again = 1;
    int ok = ext_wr(d, R_ENABLE, 0x00) && ext_wr(d, R_ATIME, ATIME_V) && ext_wr(d, R_WTIME, 0xB6) &&
             ext_wr(d, R_PPULSE, 0x87) && set_ctrl() && ext_wr(d, R_CONFIG2, 0x01) &&
             ext_wr(d, R_GPENTH, 40) && ext_wr(d, R_GEXTH, 30) && ext_wr(d, R_GCONF1, 0x40) &&
             ext_wr(d, R_GCONF2, 0x41) && ext_wr(d, R_GPULSE, 0xC9) && ext_wr(d, R_GCONF3, 0x00) &&
             ext_wr(d, R_GCONF4, 0x00) && set_enable();
    if (!ok) { i2c_master_bus_rm_device(d); free(Lt); Lt = NULL; return; }
    Lt->prox_base = 255;
    Lt->last_gest_call = ext_ms() - NOS_EXT_IDLE_MS - 1;
    Lt->next = ext_ms() + 400;
    ext_present_bits |= EXT_LIGHT;
}

static void push(int g)
{
    static const int ROT[5] = { GEST_UP, GEST_RIGHT, GEST_DOWN, GEST_LEFT, 0 };
    if (g >= GEST_UP && g <= GEST_RIGHT) {
        int i = g == GEST_UP ? 0 : g == GEST_RIGHT ? 1 : g == GEST_DOWN ? 2 : 3;
        g = ROT[(i + EXT_APDS_ROTATE) & 3];
    }
    portENTER_CRITICAL(&ext_lock);
    if (Lt->qn < 4) Lt->q[Lt->qn++] = g;
    portEXIT_CRITICAL(&ext_lock);
    Lt->gestures++;
}

static void gesture_end(void)
{
    if (Lt->have_first) {
        int dud = Lt->ud1 - Lt->ud0, dlr = Lt->lr1 - Lt->lr0;
        int aud = abs(dud), alr = abs(dlr);
        if (aud > GSWIPE || alr > GSWIPE) {
            if (aud >= alr) push(dud > 0 ? GEST_DOWN : GEST_UP);
            else push(dlr > 0 ? GEST_RIGHT : GEST_LEFT);
        }
    }
    if (Lt->near_sent) push(GEST_FAR);
    Lt->sess = Lt->have_first = Lt->near_sent = 0;
}

static void gesture_poll(uint32_t now)
{
    uint8_t gs = 0, lvl = 0;
    if (!ext_rd(Lt->dev, R_GSTATUS, &gs, 1)) return;
    if (gs & 0x01) {
        if (!ext_rd(Lt->dev, R_GFLVL, &lvl, 1) || !lvl) return;
        if (lvl > 32) lvl = 32;
        uint8_t buf[128];
        if (!ext_rd(Lt->dev, R_GFIFO, buf, (size_t)lvl * 4)) return;
        if (!Lt->sess) { Lt->sess = 1; Lt->sess_t0 = now; }
        Lt->sess_last = now;
        for (int i = 0; i < lvl; i++) {
            int u = buf[4 * i], d = buf[4 * i + 1], l = buf[4 * i + 2], r = buf[4 * i + 3];
            if (u <= GTHRESH || d <= GTHRESH || l <= GTHRESH || r <= GTHRESH) continue;
            int ud = (u - d) * 100 / (u + d), lr = (l - r) * 100 / (l + r);
            if (!Lt->have_first) { Lt->ud0 = ud; Lt->lr0 = lr; Lt->have_first = 1; }
            Lt->ud1 = ud; Lt->lr1 = lr;
        }
        if (!Lt->near_sent && Lt->have_first && (uint32_t)(now - Lt->sess_t0) > 600 &&
            abs(Lt->ud1 - Lt->ud0) <= GSWIPE && abs(Lt->lr1 - Lt->lr0) <= GSWIPE) {
            Lt->near_sent = 1;
            push(GEST_NEAR);
        }
    } else if (Lt->sess && (uint32_t)(now - Lt->sess_last) > 40) {
        uint8_t c4 = 0;
        if (ext_rd(Lt->dev, R_GCONF4, &c4, 1) && !(c4 & 0x01)) gesture_end();   /* engine exited */
    }
}

uint32_t light_service(uint32_t now)
{
    if (!Lt) return 1000;
    if (Lt->gest_want && ext_idle(now, Lt->last_gest_call)) Lt->gest_want = 0;
    if (Lt->gest_want != Lt->gest_on) {
        Lt->gest_on = Lt->gest_want;
        ext_wr(Lt->dev, R_WTIME, Lt->gest_on ? 0xFF : 0xB6);   /* 2.8 ms wait: faster gesture entry */
        set_enable();
        Lt->sess = Lt->have_first = Lt->near_sent = 0;
    }
    if (Lt->gest_on) gesture_poll(now);
    if (!ext_due(now, Lt->next)) {
        uint32_t w = Lt->next - now;
        return Lt->gest_on && w > 15 ? 15 : w;
    }
    Lt->next = now + 400;
    uint8_t st = 0, c[8], p = 0;
    if (!ext_rd(Lt->dev, R_STATUS, &st, 1)) {
        if (++Lt->errors > 5) { portENTER_CRITICAL(&ext_lock); Lt->cur.valid = 0; portEXIT_CRITICAL(&ext_lock); }
        return 400;
    }
    Lt->errors = 0;
    hal_light v;
    portENTER_CRITICAL(&ext_lock);
    v = Lt->cur;
    portEXIT_CRITICAL(&ext_lock);
    if ((st & 0x01) && ext_rd(Lt->dev, R_CDATA, c, 8)) {
        float C = (float)(c[0] | c[1] << 8), R = (float)(c[2] | c[3] << 8);
        float Gc = (float)(c[4] | c[5] << 8), B = (float)(c[6] | c[7] << 8);
        float ir = (R + Gc + B - C) * 0.5f;
        if (ir < 0) ir = 0;
        float g = Gc - ir;
        if (g < 0) g = 0;
        v.lux = g * EXT_APDS_LUX_DF * EXT_APDS_WINDOW_GA / (ATIME_MS * (float)GAINS[Lt->again]);
        v.valid = 1;
        int ng = Lt->again;
        if (C > 37925 * 0.9f && ng > 0) ng--;
        else if (C < 37925 * 0.04f && ng < 3) ng++;
        if (ng != Lt->again) { Lt->again = ng; set_ctrl(); }
    }
    if ((st & 0x02) && ext_rd(Lt->dev, R_PDATA, &p, 1)) {
        if (p < Lt->prox_base) Lt->prox_base += ((float)p - Lt->prox_base) * 0.5f;
        else Lt->prox_base += 0.04f;                        /* ~1 count / 10 s upward drift */
        int pr = (int)((float)p - Lt->prox_base);
        v.prox = pr < 0 ? 0 : pr > 255 ? 255 : pr;
    }
    portENTER_CRITICAL(&ext_lock);
    Lt->cur = v;
    portEXIT_CRITICAL(&ext_lock);
    return Lt->gest_on ? 15 : 400;
}

int hal_light_get(hal_light *out)
{
    if (!Lt) { if (out) memset(out, 0, sizeof *out); return 0; }
    hal_light v;
    portENTER_CRITICAL(&ext_lock);
    v = Lt->cur;
    portEXIT_CRITICAL(&ext_lock);
    if (out) *out = v;
    return v.valid;
}

int hal_light_gesture(void)
{
    if (!Lt) return GEST_NONE;
    Lt->last_gest_call = ext_ms();
    int g = GEST_NONE;
    portENTER_CRITICAL(&ext_lock);
    if (Lt->qn) { g = Lt->q[0]; memmove(Lt->q, Lt->q + 1, sizeof(int) * 3); Lt->qn--; }
    portEXIT_CRITICAL(&ext_lock);
    return g;
}

/* Register writes happen on the ext task; this only flips the request. */
void hal_light_gesture_enable(int on)
{
    if (!Lt) return;
    Lt->last_gest_call = ext_ms();
    Lt->gest_want = on ? 1 : 0;
}

void light_debug(void)
{
    if (!Lt) { printf("EXT light absent (no APDS-9960 at 0x39)\n"); return; }
    hal_light v; hal_light_get(&v);
    printf("EXT light id=0x%02x valid=%d lux=%.1f prox=%d base=%.1f again=%dx gest_on=%d gestures=%u queued=%d\n",
           Lt->id, v.valid, (double)v.lux, v.prox, (double)Lt->prox_base, GAINS[Lt->again], Lt->gest_on,
           (unsigned)Lt->gestures, Lt->qn);
}
