/* ext_mag.c — GY-271 compass: QMC5883L at 0x0D, or HMC5883L at 0x1E.
 *
 * QMC5883L: continuous 50 Hz, +-8 G (3000 LSB/G = 30 LSB/uT), OSR 512.
 * HMC5883L: continuous 15 Hz, 8-sample average, +-1.9 Ga (820 LSB/Ga).
 * The wide ranges are deliberate: the strap pod sits near a speaker magnet and
 * the motor, which add large static offsets that would saturate +-2 G.
 *
 * Runs only while hal_mag_get() is being called (or calibrating); otherwise the
 * chip is put in standby / idle mode. Samples taken while the vibration motor or
 * the amplifier is active are discarded (their coil currents swamp Earth's field).
 *
 * Frames. Everything is computed in the IMU's (QMI8658 chip) frame:
 *   mag_imu = MAP * (mag_raw - hard_iron) * soft_iron_scale
 * where MAP is a signed axis permutation (default identity) because the module
 * sits in a strap pod, not on the watch PCB. Tilt compensation is vector-based
 * and works at any wrist angle:  D = -accel/|accel| (down), E = norm(D x m),
 * N = E x D, heading = atan2(F.E, F.N) for the watch's 12 o'clock axis F
 * (default IMU +X). MAP and F are set with `ext mag map <+x+y+z> <+x>` and
 * persisted with the calibration (NVS key "ext_mag").
 *
 * Calibration (hal_mag_calibrate(1) ... (0)): per-axis min/max while the user
 * draws figure-8s; hard-iron offset = (max+min)/2, diagonal soft-iron scale =
 * mean range / axis range. Progress = coverage of 26 direction bins around the
 * running centre (20 bins = 100 %). The save happens in the caller's context
 * (UI task, internal stack) because the ext task's stack lives in PSRAM.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "ext_priv.h"
#include "hal.h"

#define MAG_MAGIC 0x4d414731u     /* "MAG1" */

typedef struct {
    uint32_t magic;
    float off[3], scale[3];
    int8_t map[3];                /* imu axis i = sign * raw axis |map|-1 */
    int8_t fwd;                   /* +-1..3: watch 12 o'clock axis in the IMU frame */
    int8_t valid, pad;
} mag_cal;

typedef struct {
    i2c_master_dev_handle_t dev;
    int chip;                     /* 1 QMC5883L, 2 HMC5883L */
    float lsb_ut;
    int running, calibrating, errors;
    uint32_t last_get, next, last_ok;
    float raw[3];
    mag_cal cal;
    float mn[3], mx[3];
    uint32_t bins;
    float hx, hy;                 /* smoothed heading unit vector */
    hal_mag cur;
    uint32_t skipped;
} mag_state;
static mag_state *M;

static void cal_defaults(mag_cal *c)
{
    memset(c, 0, sizeof *c);
    c->magic = MAG_MAGIC;
    c->scale[0] = c->scale[1] = c->scale[2] = 1;
    c->map[0] = 1; c->map[1] = 2; c->map[2] = 3;
    c->fwd = 1;
}

static int chip_run(int on)
{
    if (M->chip == 1) return on ? ext_wr(M->dev, 0x0B, 0x01) && ext_wr(M->dev, 0x09, 0x15) : ext_wr(M->dev, 0x09, 0x00);
    return ext_wr(M->dev, 0x02, on ? 0x00 : 0x03);
}

void mag_init(void)
{
    i2c_master_dev_handle_t d;
    int chip = 0;
    uint8_t id[3] = { 0 };
    if (ext_i2c_open(0x0D, &d)) {
        if (ext_rd(d, 0x0D, id, 1) && id[0] == 0xFF) chip = 1;
        else i2c_master_bus_rm_device(d);
    }
    if (!chip && ext_i2c_open(0x1E, &d)) {
        if (ext_rd(d, 0x0A, id, 3) && id[0] == 'H' && id[1] == '4' && id[2] == '3') chip = 2;
        else i2c_master_bus_rm_device(d);
    }
    if (!chip) return;
    M = ext_psram_calloc(sizeof *M);
    if (!M) { i2c_master_bus_rm_device(d); return; }
    M->dev = d; M->chip = chip;
    M->lsb_ut = chip == 1 ? 30.0f : 8.2f;
    if (chip == 2) { ext_wr(d, 0x00, 0x70); ext_wr(d, 0x01, 0x40); }
    if (!chip_run(0)) { i2c_master_bus_rm_device(d); free(M); M = NULL; return; }
    mag_cal c;
    if (hal_store_load("ext_mag", &c, sizeof c) && c.magic == MAG_MAGIC) M->cal = c;
    else cal_defaults(&M->cal);
    M->last_get = ext_ms() - NOS_EXT_IDLE_MS - 1;
    M->hx = 1;
    ext_present_bits |= EXT_MAG;
}

static int read_raw(float v[3])
{
    uint8_t b[6];
    int16_t x, y, z;
    if (M->chip == 1) {
        uint8_t st = 0;
        if (!ext_rd(M->dev, 0x06, &st, 1)) return -1;
        if (!(st & 0x01)) return 0;
        if (!ext_rd(M->dev, 0x00, b, 6)) return -1;
        x = (int16_t)(b[0] | b[1] << 8); y = (int16_t)(b[2] | b[3] << 8); z = (int16_t)(b[4] | b[5] << 8);
        if (st & 0x02) return 0;                       /* overflow */
    } else {
        if (!ext_rd(M->dev, 0x03, b, 6)) return -1;
        x = (int16_t)(b[0] << 8 | b[1]); z = (int16_t)(b[2] << 8 | b[3]); y = (int16_t)(b[4] << 8 | b[5]);
        if (x == -4096 || y == -4096 || z == -4096) return 0;
    }
    v[0] = x; v[1] = y; v[2] = z;
    return 1;
}

static void cal_track(const float r[3])
{
    for (int i = 0; i < 3; i++) {
        if (r[i] < M->mn[i]) M->mn[i] = r[i];
        if (r[i] > M->mx[i]) M->mx[i] = r[i];
    }
    float c[3], n = 0;
    for (int i = 0; i < 3; i++) { c[i] = r[i] - (M->mn[i] + M->mx[i]) * 0.5f; n += c[i] * c[i]; }
    n = sqrtf(n);
    if (n < 1e-3f) return;
    int idx = 0;
    for (int i = 0; i < 3; i++) {
        float u = c[i] / n;
        idx = idx * 3 + (u > 0.4f ? 2 : u < -0.4f ? 0 : 1);
    }
    if (idx != 13) M->bins |= 1u << idx;
}

static float cal_progress(void)
{
    float p = (float)__builtin_popcount(M->bins) / 20.0f;
    return p > 1 ? 1 : p;
}

static void compute(uint32_t now)
{
    const mag_cal *c = &M->cal;
    float r[3], m[3];
    for (int i = 0; i < 3; i++) r[i] = (M->raw[i] - c->off[i]) * c->scale[i];
    for (int i = 0; i < 3; i++) {
        int a = abs(c->map[i]) - 1;
        m[i] = (c->map[i] < 0 ? -1.0f : 1.0f) * r[a < 0 || a > 2 ? i : a];
    }
    hal_imu imu;
    float D[3] = { 0, 0, -1 };                        /* flat if the IMU is unavailable */
    if (hal_imu_read(&imu)) {
        float n = sqrtf(imu.ax * imu.ax + imu.ay * imu.ay + imu.az * imu.az);
        if (n > 0.3f) { D[0] = -imu.ax / n; D[1] = -imu.ay / n; D[2] = -imu.az / n; }
    }
    float E[3] = { D[1] * m[2] - D[2] * m[1], D[2] * m[0] - D[0] * m[2], D[0] * m[1] - D[1] * m[0] };
    float en = sqrtf(E[0] * E[0] + E[1] * E[1] + E[2] * E[2]);
    if (en < 1e-3f) return;
    for (int i = 0; i < 3; i++) E[i] /= en;
    float N[3] = { E[1] * D[2] - E[2] * D[1], E[2] * D[0] - E[0] * D[2], E[0] * D[1] - E[1] * D[0] };
    int fa = abs(c->fwd) - 1;
    if (fa < 0 || fa > 2) fa = 0;
    float fs = c->fwd < 0 ? -1.0f : 1.0f;
    float fe = fs * E[fa], fn = fs * N[fa];
    float hn = sqrtf(fe * fe + fn * fn);
    if (hn > 1e-3f) {
        M->hx += (fn / hn - M->hx) * 0.3f;            /* smooth on the unit circle */
        M->hy += (fe / hn - M->hy) * 0.3f;
    }
    float h = atan2f(M->hy, M->hx) * (180.0f / 3.14159265f);
    if (h < 0) h += 360.0f;
    hal_mag o = {
        .valid = 1, .heading_deg = h,
        .field_ut = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]) / M->lsb_ut,
        .calibrated = c->valid, .cal_progress = M->calibrating ? cal_progress() : (c->valid ? 1.0f : 0.0f),
    };
    portENTER_CRITICAL(&ext_lock);
    M->cur = o;
    portEXIT_CRITICAL(&ext_lock);
    M->last_ok = now;
}

uint32_t mag_service(uint32_t now)
{
    if (!M) return 1000;
    int want = M->calibrating || !ext_idle(now, M->last_get);
    if (want != M->running) {
        if (chip_run(want)) M->running = want;
        if (!want) { portENTER_CRITICAL(&ext_lock); M->cur.valid = 0; portEXIT_CRITICAL(&ext_lock); }
        M->next = now + 30;
    }
    if (!M->running) return 1000;
    if (!ext_due(now, M->next)) return M->next - now;
    M->next = now + 50;                                /* 20 Hz */
    if (haptic_active() || audio_amp_on()) { M->skipped++; return 50; }
    float r[3];
    int ok = read_raw(r);
    if (ok < 0) { if (++M->errors > 10) { portENTER_CRITICAL(&ext_lock); M->cur.valid = 0; portEXIT_CRITICAL(&ext_lock); } return 50; }
    if (!ok) return 50;
    M->errors = 0;
    memcpy(M->raw, r, sizeof r);
    if (M->calibrating) cal_track(r);
    compute(now);
    return 50;
}

int hal_mag_get(hal_mag *out)
{
    if (!M) { if (out) memset(out, 0, sizeof *out); return 0; }
    uint32_t now = ext_ms();
    M->last_get = now;
    hal_mag v;
    portENTER_CRITICAL(&ext_lock);
    v = M->cur;
    portEXIT_CRITICAL(&ext_lock);
    if ((uint32_t)(now - M->last_ok) > 500) v.valid = 0;
    if (out) *out = v;
    return v.valid;
}

void hal_mag_calibrate(int start)
{
    if (!M) return;
    M->last_get = ext_ms();
    if (start) {
        portENTER_CRITICAL(&ext_lock);
        for (int i = 0; i < 3; i++) { M->mn[i] = 1e9f; M->mx[i] = -1e9f; }
        M->bins = 0;
        M->calibrating = 1;
        portEXIT_CRITICAL(&ext_lock);
        return;
    }
    if (!M->calibrating) return;
    M->calibrating = 0;
    float range[3], mean = 0;
    int ok = __builtin_popcount(M->bins) >= 10;
    for (int i = 0; i < 3; i++) {
        range[i] = M->mx[i] - M->mn[i];
        if (range[i] < 10.0f * M->lsb_ut) ok = 0;      /* < 10 uT of swing: not rotated enough */
        mean += range[i] / 3.0f;
    }
    if (!ok) return;                                   /* keep the previous calibration */
    mag_cal c = M->cal;
    for (int i = 0; i < 3; i++) {
        c.off[i] = (M->mx[i] + M->mn[i]) * 0.5f;
        c.scale[i] = mean / range[i];
    }
    c.valid = 1;
    M->cal = c;
    hal_store_save("ext_mag", &c, sizeof c);           /* caller context: internal stack */
}

static int parse_axis(const char *s, int8_t *out)
{
    if ((s[0] != '+' && s[0] != '-') || s[1] < 'x' || s[1] > 'z') return 0;
    *out = (int8_t)((s[0] == '-' ? -1 : 1) * (s[1] - 'x' + 1));
    return 1;
}

void mag_debug_cmd(const char *a)
{
    if (!M) { printf("EXT mag absent\n"); return; }
    if (!strncmp(a, "cal", 3)) { hal_mag_calibrate(1); printf("EXT mag calibrating: draw figure-8s, then `ext mag done`\n"); return; }
    if (!strncmp(a, "done", 4)) { hal_mag_calibrate(0); printf("EXT mag calibrated=%d\n", M->cal.valid); return; }
    if (!strncmp(a, "map ", 4)) {
        mag_cal c = M->cal;
        const char *p = a + 4;
        int ok = strlen(p) >= 6 && parse_axis(p, &c.map[0]) && parse_axis(p + 2, &c.map[1]) && parse_axis(p + 4, &c.map[2]);
        if (ok && p[6] == ' ') ok = parse_axis(p + 7, &c.fwd);
        if (!ok) { printf("EXT mag map: e.g. `ext mag map +x-y-z +x`\n"); return; }
        M->cal = c;
        hal_store_save("ext_mag", &c, sizeof c);
        printf("EXT mag map saved\n");
        return;
    }
    printf("EXT mag cmds: cal | done | map <+x+y+z> [<fwd>]\n");
}

void mag_debug(void)
{
    if (!M) { printf("EXT mag absent (no QMC5883L@0d / HMC5883L@1e)\n"); return; }
    hal_mag v;
    portENTER_CRITICAL(&ext_lock);
    v = M->cur;
    portEXIT_CRITICAL(&ext_lock);
    const mag_cal *c = &M->cal;
    printf("EXT mag chip=%s run=%d valid=%d hdg=%.1f field=%.1fuT cal=%d prog=%.2f calibrating=%d raw=%.0f,%.0f,%.0f"
           " off=%.0f,%.0f,%.0f map=%d,%d,%d fwd=%d skipped=%u\n",
           M->chip == 1 ? "QMC5883L" : "HMC5883L", M->running, v.valid, (double)v.heading_deg, (double)v.field_ut,
           c->valid, (double)v.cal_progress, M->calibrating, (double)M->raw[0], (double)M->raw[1], (double)M->raw[2],
           (double)c->off[0], (double)c->off[1], (double)c->off[2], c->map[0], c->map[1], c->map[2], c->fwd,
           (unsigned)M->skipped);
}
