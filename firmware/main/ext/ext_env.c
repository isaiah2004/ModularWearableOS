#include "sports.h"
/* ext_env.c — BME280 (7Semi Nano) at 0x76 or 0x77. A BMP280 (id 0x58) is
 * accepted too and reports humidity 0.
 *
 * Forced mode every 2 s: humidity x1, temperature x2, pressure x8, IIR filter 4
 * (~28 ms conversion, read 50 ms after the trigger). Between conversions the chip
 * sleeps (~0.1 uA), averaging well under 10 uA.
 * Compensation is Bosch's reference integer code (datasheet rev 1.6, s 4.2.3 and
 * 8.2): 32-bit temperature with t_fine, 64-bit pressure, 32-bit humidity.
 * Altitude: international barometric formula against a sea-level reference
 * (default 1013.25 hPa, persisted under NVS key "ext_sea").
 * History: one pressure sample every 10 min, 144 deep (24 h) in PSRAM; not
 * persisted across reboots (would need a flash write every 10 min).
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "ext_priv.h"
#include "hal.h"

#define PERIOD_MS   2000
#define HIST_N      144
#define HIST_MS     (10 * 60 * 1000)

typedef struct {
    i2c_master_dev_handle_t dev;
    uint8_t addr, chip;
    uint16_t T1, P1; int16_t T2, T3, P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t H1, H3; int16_t H2, H4, H5; int8_t H6;
    int triggered, errors;
    uint32_t t_trig, next, last_error_ms;
    int last_error_op;
    hal_env cur;
    float sea;
    float hist[HIST_N]; int hn, hhead;
    uint32_t next_hist;
} env_state;
static env_state *E;

static int load_cal(void)
{
    uint8_t c[26], h[7];
    if (!ext_rd(E->dev, 0x88, c, 26)) return 0;
#define U16(i) (uint16_t)(c[i] | c[(i) + 1] << 8)
#define S16(i) (int16_t)(c[i] | c[(i) + 1] << 8)
    E->T1 = U16(0); E->T2 = S16(2); E->T3 = S16(4);
    E->P1 = U16(6); E->P2 = S16(8); E->P3 = S16(10); E->P4 = S16(12); E->P5 = S16(14);
    E->P6 = S16(16); E->P7 = S16(18); E->P8 = S16(20); E->P9 = S16(22);
    E->H1 = c[25];
    if (E->chip == 0x60) {
        if (!ext_rd(E->dev, 0xE1, h, 7)) return 0;
        E->H2 = (int16_t)(h[0] | h[1] << 8);
        E->H3 = h[2];
        E->H4 = (int16_t)((int16_t)(int8_t)h[3] * 16 | (h[4] & 0x0F));
        E->H5 = (int16_t)((int16_t)(int8_t)h[5] * 16 | (h[4] >> 4));
        E->H6 = (int8_t)h[6];
    }
    return E->P1 != 0;
}

static void compensate(int32_t adc_P, int32_t adc_T, int32_t adc_H, hal_env *o)
{
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)E->T1 << 1))) * (int32_t)E->T2) >> 11;
    int32_t var2 = (((((adc_T >> 4) - (int32_t)E->T1) * ((adc_T >> 4) - (int32_t)E->T1)) >> 12) *
                    (int32_t)E->T3) >> 14;
    int32_t t_fine = var1 + var2;
    o->temp_c = (float)((t_fine * 5 + 128) >> 8) / 100.0f;

    int64_t v1 = (int64_t)t_fine - 128000;
    int64_t v2 = v1 * v1 * (int64_t)E->P6;
    v2 += (v1 * (int64_t)E->P5) << 17;
    v2 += ((int64_t)E->P4) << 35;
    v1 = ((v1 * v1 * (int64_t)E->P3) >> 8) + ((v1 * (int64_t)E->P2) << 12);
    v1 = ((((int64_t)1) << 47) + v1) * ((int64_t)E->P1) >> 33;
    if (v1 != 0) {
        int64_t p = 1048576 - adc_P;
        p = (((p << 31) - v2) * 3125) / v1;
        v1 = (((int64_t)E->P9) * (p >> 13) * (p >> 13)) >> 25;
        v2 = (((int64_t)E->P8) * p) >> 19;
        p = ((p + v1 + v2) >> 8) + (((int64_t)E->P7) << 4);
        o->pressure_hpa = (float)p / 256.0f / 100.0f;
    }

    if (E->chip == 0x60) {
        int32_t x = t_fine - 76800;
        x = (((((adc_H << 14) - (((int32_t)E->H4) << 20) - (((int32_t)E->H5) * x)) + 16384) >> 15) *
             (((((((x * (int32_t)E->H6) >> 10) * (((x * (int32_t)E->H3) >> 11) + 32768)) >> 10) + 2097152) *
               (int32_t)E->H2 + 8192) >> 14));
        x = x - (((((x >> 15) * (x >> 15)) >> 7) * (int32_t)E->H1) >> 4);
        x = x < 0 ? 0 : x > 419430400 ? 419430400 : x;
        o->humidity = (float)(x >> 12) / 1024.0f;
    } else o->humidity = 0;
    o->altitude_m = 44330.0f * (1.0f - powf(o->pressure_hpa / E->sea, 0.1902949f));
}

void env_init(void)
{
    i2c_master_dev_handle_t d = NULL;
    uint8_t addr = 0, id = 0;
    static const uint8_t addrs[2] = { 0x76, 0x77 };
    for (int i = 0; i < 2 && !addr; i++) {
        if (!ext_i2c_open(addrs[i], &d)) continue;
        if (ext_rd(d, 0xD0, &id, 1) && (id == 0x60 || id == 0x58)) addr = addrs[i];
        else { i2c_master_bus_rm_device(d); d = NULL; }
    }
    if (!addr) return;
    E = ext_psram_calloc(sizeof *E);
    if (!E) { i2c_master_bus_rm_device(d); return; }
    E->dev = d; E->addr = addr; E->chip = id;
    E->sea = 1013.25f;
    float sea;
    if (hal_store_load("ext_sea", &sea, sizeof sea) && sea > 800 && sea < 1200) E->sea = sea;
    if (!load_cal() || !ext_wr(d, 0xF4, 0x00) || !ext_wr(d, 0xF5, 0x08)) {   /* sleep; filter 4 */
        i2c_master_bus_rm_device(d); nos_mem_free(E); E = NULL;
        return;
    }
    E->next = ext_ms();
    E->next_hist = ext_ms();
    ext_present_bits |= EXT_ENV;
}

uint32_t env_service(uint32_t now)
{
    if (!E) return 1000;
    portENTER_CRITICAL(&ext_lock);
    if (E->cur.valid && ext_elapsed(now,E->cur.sample_ms)>NOS_ENV_MAX_AGE_MS)
        E->cur.valid = E->cur.humidity_valid = 0;
    portEXIT_CRITICAL(&ext_lock);
    if (!E->triggered) {
        if (!ext_due(now, E->next)) return E->next - now;
        E->next = now + PERIOD_MS;
        if (ext_wr(E->dev, 0xF2, 0x01) && ext_wr(E->dev, 0xF4, 0x51)) {     /* h x1; t x2, p x8, forced */
            E->triggered = 1; E->t_trig = now;
            return 50;
        }
        E->errors++;
        E->last_error_op=1; E->last_error_ms=ext_ms();
        return PERIOD_MS;
    }
    if (ext_elapsed(now,E->t_trig) < 50) return 50 - ext_elapsed(now,E->t_trig);
    E->triggered = 0;
    uint8_t r[8];
    if (!ext_rd(E->dev, 0xF7, r, 8)) {
        E->errors++;
        E->last_error_op=2; E->last_error_ms=ext_ms();
        if (E->errors > 5) { portENTER_CRITICAL(&ext_lock); E->cur.valid = E->cur.humidity_valid = 0; portEXIT_CRITICAL(&ext_lock); }
        return PERIOD_MS;
    }
    int32_t adc_P = (int32_t)((uint32_t)r[0] << 12 | (uint32_t)r[1] << 4 | r[2] >> 4);
    int32_t adc_T = (int32_t)((uint32_t)r[3] << 12 | (uint32_t)r[4] << 4 | r[5] >> 4);
    int32_t adc_H = (int32_t)((uint32_t)r[6] << 8 | r[7]);
    if (adc_T == 0x80000 || adc_P == 0x80000) {
        E->errors++; E->last_error_op=3; E->last_error_ms=ext_ms();
        return PERIOD_MS;
    }
    now=ext_ms();
    hal_env o = { .valid = 1, .humidity_valid = E->chip==0x60 && adc_H!=0x8000, .sample_ms=now };
    compensate(adc_P, adc_T, adc_H, &o);
    if(!isfinite(o.temp_c)||!isfinite(o.pressure_hpa)||o.pressure_hpa<100||o.pressure_hpa>1300) {
        E->errors++;E->last_error_op=4;E->last_error_ms=now;return PERIOD_MS;
    }
    if(!isfinite(o.humidity)||o.humidity<0||o.humidity>100)o.humidity_valid=0;
    E->errors = 0;
    portENTER_CRITICAL(&ext_lock);
    E->cur = o;
    if (ext_due(now, E->next_hist)) {
        E->next_hist = now + HIST_MS;
        E->hist[E->hhead] = o.pressure_hpa;
        E->hhead = (E->hhead + 1) % HIST_N;
        if (E->hn < HIST_N) E->hn++;
    }
    portEXIT_CRITICAL(&ext_lock);
    {float sv[]={ (float)o.valid,o.temp_c,o.humidity,o.pressure_hpa,o.altitude_m };sports_emit(SP_ENV,sv,5);}
    return PERIOD_MS;
}

int hal_env_get(hal_env *out)
{
    if (!E) { if (out) memset(out, 0, sizeof *out); return 0; }
    hal_env v;
    portENTER_CRITICAL(&ext_lock);
    v = E->cur;
    portEXIT_CRITICAL(&ext_lock);
    if(v.valid && ext_elapsed(ext_ms(),v.sample_ms)>NOS_ENV_MAX_AGE_MS) v.valid=v.humidity_valid=0;
    if (v.valid) v.altitude_m = 44330.0f * (1.0f - powf(v.pressure_hpa / E->sea, 0.1902949f));
    if (out) *out = v;
    return v.valid;
}

/* Called from UI context (internal stack), so the NVS write is safe here. */
void hal_env_set_sea_level(float hpa)
{
    if (!E || hpa < 800 || hpa > 1200) return;
    E->sea = hpa;
    hal_store_save("ext_sea", &hpa, sizeof hpa);
}

int hal_env_history(float *hpa_out, int max)
{
    if (!E || !hpa_out || max <= 0) return 0;
    portENTER_CRITICAL(&ext_lock);
    int n = E->hn < max ? E->hn : max;
    int start = (E->hhead - n + HIST_N) % HIST_N;            /* the newest n, oldest first */
    for (int i = 0; i < n; i++) hpa_out[i] = E->hist[(start + i) % HIST_N];
    portEXIT_CRITICAL(&ext_lock);
    return n;
}

void env_debug(void)
{
    if (!E) { printf("EXT env absent (no BME280 at 0x76/0x77)\n"); return; }
    hal_env v; hal_env_get(&v);
    printf("EXT env chip=0x%02x@0x%02x valid=%d t=%.2fC rh=%.1f%% p=%.2fhPa alt=%.1fm sea=%.2f hist=%d err=%d humidity_valid=%d sample_ms=%u age_ms=%u last_error_op=%d last_error_ms=%u\n",
           E->chip, E->addr, v.valid, (double)v.temp_c, (double)v.humidity, (double)v.pressure_hpa,
           (double)v.altitude_m, (double)E->sea, E->hn, E->errors,v.humidity_valid,
           (unsigned)v.sample_ms,(unsigned)ext_elapsed(ext_ms(),v.sample_ms),E->last_error_op,(unsigned)E->last_error_ms);
}
int env_shutdown_poll(uint32_t now)
{
    (void)now;
    if(!E)return 1;
    E->triggered=0;
    if(!ext_wr(E->dev,0xF4,0))return -1;
    portENTER_CRITICAL(&ext_lock);E->cur.valid=E->cur.humidity_valid=0;portEXIT_CRITICAL(&ext_lock);
    return 1;
}
