/* nos_ext_sim.c — nos/hal_ext.h on the PC: plausible, smoothly varying synthetic
 * strap-module data, driven only by hal_millis() / hal_get_datetime(), so scripted
 * runs (virtual clock) are deterministic.
 *
 *   GPS     walking loop around Cubbon Park, Bengaluru; fix ~4 s after enable,
 *           3D at ~5.5 s; metre-level correlated jitter; SNR bars that build up.
 *   Compass slow drift plus wrist wobble; calibration progress ramps over ~8 s.
 *   BME280  daily temperature/humidity cycle from the sim RTC; station pressure at
 *           ~920 m with a semi-diurnal tide and a front that passed ~10 h before boot;
 *           the 24 h history is full from boot so the barometer chart can be previewed.
 *   HR      on-wrist at 0.5 s, locks at ~3 s, ~72 bpm with respiratory HRV, a PPG
 *           trace with systolic peak, dicrotic notch and diastolic wave; SpO2 at ~6 s.
 *   Mic     room tone ~38 dB with speech-like phrases (4 Hz syllables, 2-4 s phrases).
 *   LTE     powers up 2.5 s after enable, registers on "Jio 4G" at 3 bars; 2 unread of
 *           4 sample SMS (inbox known even with the modem off, like the device).
 *   Light   ~300 lux; gestures only via hal_ext_sim_inject_gesture() / `set gesture N`.
 * Streams honour enable + the NOS_EXT_IDLE_MS auto-stop exactly like the device.
 * `hal_ext_sim_present` bits mark modules absent (getters return 0 / invalid).
 *
 * Haptics and SFX are recorded in hal_ext_sim_last_* for the sim UI. In interactive
 * mode (not sim_virtual) SFX are also played through waveOut, synthesised by the very
 * same recipes as the watch (firmware/main/ext/ext_sfx_synth.h), as is PCM playback.
 */
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hal.h"
#include "hal_ext.h"
#include "nos_sim.h"
#include "../firmware/main/ext/ext_sfx_synth.h"

#define PI_F 3.14159265358979f

uint32_t hal_ext_sim_present = 0x1FF;
int      hal_ext_sim_last_haptic = -1;
uint32_t hal_ext_sim_last_haptic_ms;
int      hal_ext_sim_last_sfx = -1;
uint32_t hal_ext_sim_last_sfx_ms;
void     hal_ext_sim_inject_gesture(int g);

static int has(uint32_t bit) { return (hal_ext_sim_present & bit) != 0; }
static uint32_t now_ms(void) { return hal_millis(); }
static int idle(uint32_t now, uint32_t last) { return (uint32_t)(now - last) > NOS_EXT_IDLE_MS; }

/* ---------------------------------------------------------------- noise helpers */
static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
static float hrand(uint32_t i, uint32_t seed) { return (float)(hash32(i * 0x9E3779B9u ^ seed) & 0xFFFFFF) / 16777215.0f; }
/* smooth value noise in [-1, 1], one feature per unit of x */
static float vnoise(float x, uint32_t seed)
{
    float fl = floorf(x), f = x - fl;
    int32_t i = (int32_t)fl;
    float a = hrand((uint32_t)i, seed) * 2 - 1, b = hrand((uint32_t)(i + 1), seed) * 2 - 1;
    float u = f * f * (3 - 2 * f);
    return a + (b - a) * u;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---------------------------------------------------------------- state */
static struct {
    int gps_on; uint32_t gps_t0, gps_last;
    int lte_on; uint32_t lte_t0, lte_last; int sent;
    int hr_on; uint32_t hr_t0, hr_last; int bpm_override;
    uint32_t mic_last; float db_override;
    int gest_on; uint32_t gest_last; int gq[8]; int gqn;
    int mag_cal; uint32_t mag_cal_t0; int mag_calibrated; float heading_override; int off_wrist;
    float sea; float lux_override;
    int nofix;
    int rec; uint32_t rec_t0, rec_ms;
    int play; uint32_t play_t0, play_len_ms;
    int volume, display_mode;
} S = { .sea = 1013.25f, .volume = 70, .display_mode = 2, .mag_calibrated = 1,
        .heading_override = -1, .lux_override = -1, .db_override = -1 };

void hal_ext_init(void) { }
uint32_t hal_ext_present(void) { return hal_ext_sim_present & 0x1FF; }

/* ================================================================ audio out (waveOut) */
#define WO_BUFS 4
#define WO_LEN  320                          /* 20 ms at 16 kHz */
static CRITICAL_SECTION s_au_cs;
static HWAVEOUT s_wo;
static HANDLE s_wo_evt, s_wo_thread;
static WAVEHDR s_hdr[WO_BUFS];
static int16_t s_buf[WO_BUFS][WO_LEN];
static sfx_voice s_voice[2];
static int16_t *s_pcm; static uint32_t s_pcm_n, s_pcm_pos;
static float s_lp;

static void mix(int16_t *out, int n)
{
    float acc[WO_LEN];
    memset(acc, 0, sizeof acc);
    EnterCriticalSection(&s_au_cs);
    for (int v = 0; v < 2; v++) sfx_voice_render(&s_voice[v], acc, n);
    if (s_pcm) {
        for (int i = 0; i < n && s_pcm_pos < s_pcm_n; i++) acc[i] += s_pcm[s_pcm_pos++] * (1.0f / 32768.0f) * 2.2f;
        if (s_pcm_pos >= s_pcm_n) { free(s_pcm); s_pcm = NULL; }
    }
    float g = (float)S.volume / 100.0f;
    g = g * g * 0.45f * 32767.0f;            /* same output chain as ext_audio.c */
    for (int i = 0; i < n; i++) {
        s_lp += (acc[i] - s_lp) * 0.75f;
        float y = clampf(s_lp * g, -32768, 32767);
        out[i] = (int16_t)y;
    }
    LeaveCriticalSection(&s_au_cs);
}

static DWORD WINAPI wo_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(s_wo_evt, 50);
        for (int i = 0; i < WO_BUFS; i++) {
            if (!(s_hdr[i].dwFlags & WHDR_DONE)) continue;
            mix(s_buf[i], WO_LEN);
            s_hdr[i].dwFlags &= ~(DWORD)WHDR_DONE;
            waveOutWrite(s_wo, &s_hdr[i], sizeof s_hdr[i]);
        }
    }
    return 0;
}

static int audio_up(void)
{
    if (sim_virtual) return 0;               /* scripted runs stay silent + deterministic */
    if (s_wo) return 1;
    static int failed;
    if (failed) return 0;
    InitializeCriticalSection(&s_au_cs);
    s_voice[0].id = s_voice[1].id = -1;
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, SFX_FS, SFX_FS * 2, 2, 16, 0 };
    s_wo_evt = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&s_wo, WAVE_MAPPER, &wf, (DWORD_PTR)s_wo_evt, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        s_wo = NULL; failed = 1; return 0;
    }
    for (int i = 0; i < WO_BUFS; i++) {
        memset(&s_hdr[i], 0, sizeof s_hdr[i]);
        memset(s_buf[i], 0, sizeof s_buf[i]);
        s_hdr[i].lpData = (LPSTR)s_buf[i];
        s_hdr[i].dwBufferLength = sizeof s_buf[i];
        waveOutPrepareHeader(s_wo, &s_hdr[i], sizeof s_hdr[i]);
        waveOutWrite(s_wo, &s_hdr[i], sizeof s_hdr[i]);
    }
    s_wo_thread = CreateThread(NULL, 0, wo_thread, NULL, 0, NULL);
    return 1;
}

/* ================================================================ haptics */
void hal_haptic(int pattern)
{
    if (!has(EXT_HAPTIC)) return;
    hal_ext_sim_last_haptic = pattern;
    hal_ext_sim_last_haptic_ms = now_ms();
}
void hal_haptic_stop(void) { if (hal_ext_sim_last_haptic == HAPTIC_ALARM) hal_ext_sim_last_haptic = -1; }
void hal_haptic_set_strength(int pct) { (void)pct; }

/* ================================================================ sfx / PCM */
void hal_sfx(int id)
{
    if (!has(EXT_SPEAKER) || id < 0 || id >= SFX_COUNT) return;
    hal_ext_sim_last_sfx = id;
    hal_ext_sim_last_sfx_ms = now_ms();
    if (!S.volume || !audio_up()) return;
    EnterCriticalSection(&s_au_cs);
    int v = s_voice[0].id < 0 ? 0 : s_voice[1].id < 0 ? 1 : (s_voice[0].t > s_voice[1].t ? 0 : 1);
    sfx_voice_start(&s_voice[v], id);
    LeaveCriticalSection(&s_au_cs);
}
void hal_sfx_stop(void)
{
    if (!s_wo) return;
    EnterCriticalSection(&s_au_cs);
    s_voice[0].id = s_voice[1].id = -1;
    LeaveCriticalSection(&s_au_cs);
}
void hal_audio_volume(int pct) { S.volume = pct < 0 ? 0 : pct > 100 ? 100 : pct; }

static FILE *open_sd(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (!f && !strncmp(path, "/sdcard/", 8)) {
        char p[300];
        snprintf(p, sizeof p, "build/sdcard/%s", path + 8);
        f = fopen(p, mode);
    }
    return f;
}

int hal_audio_play_file(const char *path)
{
    if (!has(EXT_SPEAKER) || !path) return 0;
    FILE *f = open_sd(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    long off = 0, len = size;
    uint8_t h[12];
    if (size > 44 && fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4)) {
        uint8_t ch[8];
        len = 0;
        while (fread(ch, 1, 8, f) == 8) {
            uint32_t l; memcpy(&l, ch + 4, 4);
            if (!memcmp(ch, "data", 4)) { off = ftell(f); len = (long)l; break; }
            fseek(f, (long)(l + (l & 1)), SEEK_CUR);
        }
    }
    if (len <= 0 || off + len > size) { fclose(f); return 0; }
    S.play = 1; S.play_t0 = now_ms(); S.play_len_ms = (uint32_t)(len / 32);
    if (audio_up()) {
        int16_t *pcm = malloc((size_t)len);
        fseek(f, off, SEEK_SET);
        if (pcm && fread(pcm, 1, (size_t)len, f) == (size_t)len) {
            EnterCriticalSection(&s_au_cs);
            free(s_pcm); s_pcm = pcm; s_pcm_n = (uint32_t)(len / 2); s_pcm_pos = 0;
            LeaveCriticalSection(&s_au_cs);
        } else free(pcm);
    }
    fclose(f);
    return 1;
}
int hal_audio_playing(void)
{
    if (S.play && now_ms() - S.play_t0 >= S.play_len_ms) S.play = 0;
    return S.play;
}
uint32_t hal_audio_play_pos_ms(void)
{
    if (!hal_audio_playing()) return 0;
    return now_ms() - S.play_t0;
}
void hal_audio_play_stop(void)
{
    S.play = 0;
    if (!s_wo) return;
    EnterCriticalSection(&s_au_cs);
    free(s_pcm); s_pcm = NULL;
    LeaveCriticalSection(&s_au_cs);
}

/* ================================================================ microphone */
/* Loudness at time t (s): room tone + phrases of syllables. */
static float mic_level(float t)
{
    if (S.db_override >= 0) return S.db_override;
    float room = 38.0f + 1.5f * vnoise(t * 0.7f, 11);
    /* phrase timeline: 6 s cells, each a phrase of 2-4 s starting 0-1.5 s in */
    uint32_t cell = (uint32_t)(t / 6.0f);
    float ct = t - cell * 6.0f;
    float start = hrand(cell, 21) * 1.5f, len = 2.0f + hrand(cell, 22) * 2.0f;
    float loud = 60.0f + hrand(cell, 23) * 8.0f;
    if (ct < start || ct > start + len) return room;
    float p = ct - start;
    float edge = clampf(p / 0.08f, 0, 1) * clampf((len - p) / 0.15f, 0, 1);
    float syl = 0.5f + 0.5f * sinf(2 * PI_F * (4.0f + 0.8f * vnoise(t * 0.5f, 24)) * p);
    syl = syl * syl;                                       /* sharper syllable envelopes */
    float speech = loud - 18.0f * (1 - syl) + 2.0f * vnoise(t * 9.0f, 25);
    float db = room + (speech - room) * edge;
    return db > room ? db : room;
}

float hal_mic_db(void)
{
    if (!has(EXT_MIC)) return -1.0f;
    uint32_t n = now_ms();
    S.mic_last = n;
    /* 125 ms smoothing ~ average of a few points */
    float t = n / 1000.0f, a = 0;
    for (int i = 0; i < 5; i++) a += mic_level(t - i * 0.03f);
    return a / 5;
}
int hal_mic_wave(uint8_t out[32])
{
    if (!has(EXT_MIC)) return 0;
    uint32_t n = now_ms();
    S.mic_last = n;
    float t = n / 1000.0f;
    for (int i = 0; i < 32; i++) {
        float db = mic_level(t - (31 - i) * 0.032f) + 3.0f * vnoise((t - (31 - i) * 0.032f) * 30.0f, 31);
        out[i] = (uint8_t)clampf((db - 40.0f) * (255.0f / 60.0f), 0, 255);
    }
    return 32;
}
int hal_rec_start(const char *path)
{
    if (!has(EXT_MIC) || !path || S.rec) return 0;
    S.rec = 1; S.rec_t0 = now_ms(); S.rec_ms = 0;
    return 1;
}
void hal_rec_stop(void) { if (S.rec) { S.rec_ms = now_ms() - S.rec_t0; S.rec = 0; } }
int  hal_rec_active(void) { return S.rec; }
uint32_t hal_rec_ms(void) { return S.rec ? now_ms() - S.rec_t0 : S.rec_ms; }

/* ================================================================ GPS */
#define PARK_LAT 12.97630
#define PARK_LON 77.59290
#define WALK_MPS 1.35f

void hal_gps_enable(int on)
{
    uint32_t n = now_ms();
    S.gps_last = n;
    if (on && !S.gps_on) S.gps_t0 = n;
    S.gps_on = on ? 1 : 0;
}

/* Position on the loop (metres east/north of the park centre) at walking time w s. */
static void walk_xy(float w, float *x, float *y)
{
    const float a = 210.0f, b = 140.0f;                 /* ~1.1 km loop, ~14 min */
    float th = w * WALK_MPS / 177.0f;                   /* 177 m ~ mean radius */
    float r = 1.0f + 0.07f * sinf(3 * th + 0.6f);
    *x = a * r * cosf(th);
    *y = b * r * sinf(th);
}

int hal_gps_get(hal_gps *out)
{
    hal_gps g;
    memset(&g, 0, sizeof g);
    if (!has(EXT_GPS)) { if (out) *out = g; return 0; }
    uint32_t n = now_ms();
    S.gps_last = n;
    if (!S.gps_on) { g.age_ms = 0xffffffffu; if (out) *out = g; return 1; }
    float on = (n - S.gps_t0) / 1000.0f, t = n / 1000.0f;
    /* sky: satellites appear over ~3 s, SNR builds */
    int view = (int)clampf(on * 4.0f, 0, 14 + 2 * vnoise(t / 40.0f, 41));
    g.sats_view = view;
    g.nsnr = view < 16 ? view : 16;
    float build = clampf(on / 4.0f, 0, 1);
    for (int i = 0; i < g.nsnr; i++) {
        float base = 18.0f + 27.0f * hrand((uint32_t)i, 42);
        g.snr[i] = (uint8_t)clampf((base + 3.0f * vnoise(t / 7.0f + i, 43)) * build, 0, 50);
    }
    for (int i = 0; i < g.nsnr; i++) {              /* slowly wheeling sky positions */
        g.az[i] = (uint16_t)((int)(360.0f * hrand((uint32_t)i, 44) + t * 0.004f) % 360);
        g.el[i] = (uint8_t)(8 + 78 * hrand((uint32_t)i, 45));
    }
    for (int i = 1; i < g.nsnr; i++)               /* strongest first, like the device */
        for (int j = i; j > 0 && g.snr[j] > g.snr[j - 1]; j--) {
            uint8_t s = g.snr[j]; g.snr[j] = g.snr[j - 1]; g.snr[j - 1] = s;
            uint8_t e = g.el[j]; g.el[j] = g.el[j - 1]; g.el[j - 1] = e;
            uint16_t a = g.az[j]; g.az[j] = g.az[j - 1]; g.az[j - 1] = a;
        }
    uint64_t ep = hal_epoch_seconds();
    if (on > 1.5f && ep) {
        uint32_t d = (uint32_t)(ep % 86400);
        g.utc_h = (int)(d / 3600); g.utc_m = (int)(d / 60 % 60); g.utc_s = (int)(d % 60);
        g.utc_valid = 1;
    }
    if (on < 4.0f || S.nofix) { g.hdop = 99.9f; g.age_ms = 0xffffffffu; if (out) *out = g; return 1; }
    float w = on - 4.0f;
    g.valid = 1;
    g.fix3d = on > 5.5f;
    g.sats_used = (int)clampf(view - 3 - (int)(2 * hrand((uint32_t)(t / 20), 44)), 4, 12);
    g.hdop = 0.9f + 1.6f * expf(-w / 6.0f) + 0.15f * vnoise(t / 5.0f, 45);
    /* 1 Hz fixes, like the M10's default rate */
    float wf = floorf(w);
    float x, y, x1, y1;
    walk_xy(wf, &x, &y);
    walk_xy(wf + 1.0f, &x1, &y1);
    float vx = x1 - x, vy = y1 - y;                     /* true track velocity, m/s */
    float jit = 1.2f + 1.5f * g.hdop;                   /* metres, correlated over ~8 s */
    x += jit * vnoise(wf / 8.0f, 46) + 0.4f * vnoise(wf * 1.7f, 47);
    y += jit * vnoise(wf / 8.0f, 48) + 0.4f * vnoise(wf * 1.7f, 49);
    g.lat = PARK_LAT + y / 111320.0;
    g.lon = PARK_LON + x / (111320.0 * cos(PARK_LAT * 3.14159265358979 / 180.0));
    g.alt_m = 921.0f + 3.0f * vnoise(wf / 30.0f, 50) + (g.fix3d ? 0 : 15.0f);
    g.speed_mps = clampf(sqrtf(vx * vx + vy * vy) + 0.12f * vnoise(wf / 3.0f, 51), 0, 5);
    float c = atan2f(vx, vy) * 180.0f / PI_F + 4.0f * vnoise(wf / 2.0f, 52);   /* clockwise from north */
    g.course_deg = c < 0 ? c + 360.0f : c;
    g.age_ms = (uint32_t)((w - wf) * 1000.0f);
    if (out) *out = g;
    return 1;
}

/* ================================================================ LTE */
static hal_sms SMS[4] = {
    { "+919845012345", "Reached the gate near the bandstand. Where are you?", "26/10/11,09:58:12", 1 },
    { "JM-JIOCRE", "Your Jio plan renews on 14-Oct. 1.5 GB/day left today.", "26/10/11,08:15:40", 1 },
    { "+919900554321", "Dinner at 8 still on? I'll book the table.", "26/10/10,19:42:03", 0 },
    { "AX-HDFCBK", "OTP 482913 for your login. Do not share it with anyone.", "26/10/10,11:07:55", 0 },
};

void hal_lte_enable(int on)
{
    uint32_t n = now_ms();
    S.lte_last = n;
    if (on && !S.lte_on) S.lte_t0 = n;
    S.lte_on = on ? 1 : 0;
}
int hal_lte_get(hal_lte *out)
{
    hal_lte l;
    memset(&l, 0, sizeof l);
    if (!has(EXT_LTE)) { if (out) *out = l; return 0; }
    uint32_t n = now_ms();
    S.lte_last = n;
    l.unread_sms = 0;                                   /* inbox cached at boot */
    for (int i = 0; i < 4; i++) l.unread_sms += SMS[i].unread ? 1 : 0;
    l.sim = 1;
    if (S.lte_on) {
        float on = (n - S.lte_t0) / 1000.0f, t = n / 1000.0f;
        l.powered = on > 2.5f;
        if (on > 4.0f) {
            l.registered = 1;
            l.rssi_dbm = -79 + 2 * (int)lroundf(vnoise(t / 15.0f, 61));   /* -81..-77 -> 3 bars */
            l.bars = l.rssi_dbm >= -73 ? 4 : l.rssi_dbm >= -83 ? 3 : 2;
            snprintf(l.op, sizeof l.op, "Jio 4G");
            snprintf(l.tech, sizeof l.tech, "LTE");
        } else if (l.powered) l.registered = 3;         /* searching */
    }
    if (out) *out = l;
    return 1;
}
int hal_lte_sms_list(hal_sms *out, int max)
{
    if (!has(EXT_LTE) || !out || max <= 0) return 0;
    S.lte_last = now_ms();
    int n = max < 4 ? max : 4;
    memcpy(out, SMS, sizeof(hal_sms) * (size_t)n);
    return n;
}
void hal_lte_sms_mark_read(int i)
{
    if (i >= 0 && i < 4 && SMS[i].unread) SMS[i].unread = 0;
}
int hal_lte_sms_send(const char *to, const char *text)
{
    if (!has(EXT_LTE) || !to || !text || !*to) return 0;
    printf("[sim] SMS to %s: %s\n", to, text);
    S.sent++;
    return 1;
}

/* ================================================================ BME280 */
/* station pressure (hPa) at x hours since 24 h before sim boot */
static float pressure_at(float x)
{
    float base = 912.0f + 0.08f * x;
    float front = -6.5f * expf(-((x - 14.0f) / 3.5f) * ((x - 14.0f) / 3.5f));
    float tide = 0.9f * sinf(2 * PI_F * (x + 2.0f) / 12.0f);
    return base + front + tide + 0.15f * vnoise(x * 2.0f, 71);
}
static float hours_now(void) { return 24.0f + now_ms() / 3600000.0f; }

int hal_env_get(hal_env *out)
{
    hal_env e;
    memset(&e, 0, sizeof e);
    if (!has(EXT_ENV)) { if (out) *out = e; return 0; }
    hal_datetime dt;
    hal_get_datetime(&dt);
    float h = dt.hour + dt.minute / 60.0f + dt.second / 3600.0f, t = now_ms() / 1000.0f;
    float day = sinf(2 * PI_F * (h - 9.0f) / 24.0f);    /* peaks ~15:00 */
    e.valid = 1;
    e.temp_c = 24.5f + 5.0f * day + 0.15f * vnoise(t / 20.0f, 72);
    e.humidity = clampf(62.0f - 16.0f * day + 1.0f * vnoise(t / 30.0f, 73), 5, 100);
    e.pressure_hpa = pressure_at(hours_now());
    e.altitude_m = 44330.0f * (1.0f - powf(e.pressure_hpa / S.sea, 0.1902949f));
    if (out) *out = e;
    return 1;
}
void hal_env_set_sea_level(float hpa) { if (hpa > 800 && hpa < 1200) S.sea = hpa; }
int hal_env_history(float *hpa_out, int max)
{
    if (!has(EXT_ENV) || !hpa_out || max <= 0) return 0;
    int n = max < 144 ? max : 144;
    float x = hours_now();
    for (int i = 0; i < n; i++) hpa_out[i] = pressure_at(x - (float)(n - 1 - i) / 6.0f);
    return n;
}

/* ================================================================ APDS-9960 */
int hal_light_get(hal_light *out)
{
    hal_light l;
    memset(&l, 0, sizeof l);
    if (!has(EXT_LIGHT)) { if (out) *out = l; return 0; }
    float t = now_ms() / 1000.0f;
    l.valid = 1;
    l.lux = S.lux_override >= 0 ? S.lux_override : 300.0f + 18.0f * vnoise(t / 9.0f, 81) + 4.0f * vnoise(t * 0.8f, 82);
    l.prox = (int)clampf(2.0f + 2.0f * vnoise(t / 3.0f, 83), 0, 255);
    if (out) *out = l;
    return 1;
}
int hal_light_gesture(void)
{
    if (!has(EXT_LIGHT)) return GEST_NONE;
    S.gest_last = now_ms();
    if (!S.gqn) return GEST_NONE;
    int g = S.gq[0];
    memmove(S.gq, S.gq + 1, sizeof(int) * 7);
    S.gqn--;
    return g;
}
void hal_light_gesture_enable(int on) { S.gest_on = on ? 1 : 0; S.gest_last = now_ms(); }
void hal_ext_sim_inject_gesture(int g)
{
    if (g <= GEST_NONE || g > GEST_FAR || S.gqn >= 8) return;
    S.gq[S.gqn++] = g;
}

/* ================================================================ compass */
int hal_mag_get(hal_mag *out)
{
    hal_mag m;
    memset(&m, 0, sizeof m);
    if (!has(EXT_MAG)) { if (out) *out = m; return 0; }
    uint32_t n = now_ms();
    float t = n / 1000.0f;
    float h = S.heading_override >= 0 ? S.heading_override
            : 35.0f + t * 0.6f + 22.0f * vnoise(t / 25.0f, 91) + 4.0f * sinf(2 * PI_F * 0.2f * t);
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    m.valid = 1;
    m.heading_deg = h;
    m.field_ut = 47.5f + 1.2f * vnoise(t / 6.0f, 92);
    m.calibrated = S.mag_calibrated;
    m.cal_progress = S.mag_cal ? clampf(1.0f - expf(-(float)(n - S.mag_cal_t0) / 3000.0f) * 1.05f, 0, 1)
                               : (S.mag_calibrated ? 1.0f : 0.0f);
    if (S.mag_cal) m.heading_deg = fmodf(h + 40.0f * sinf(t * 2.1f), 360.0f);   /* user waving the arm */
    if (m.heading_deg < 0) m.heading_deg += 360.0f;
    if (out) *out = m;
    return 1;
}
void hal_mag_calibrate(int start)
{
    if (start) { S.mag_cal = 1; S.mag_cal_t0 = now_ms(); return; }
    if (S.mag_cal && now_ms() - S.mag_cal_t0 > 3000) S.mag_calibrated = 1;
    S.mag_cal = 0;
}

/* ================================================================ heart rate */
void hal_hr_enable(int on)
{
    uint32_t n = now_ms();
    S.hr_last = n;
    if (on && !S.hr_on) S.hr_t0 = n;
    S.hr_on = on ? 1 : 0;
}

static float bpm_at(float t)
{
    float base = S.bpm_override > 0 ? (float)S.bpm_override : 72.0f + 3.0f * vnoise(t / 40.0f, 101);
    return base + 2.0f * sinf(2 * PI_F * 0.25f * t);   /* respiratory sinus arrhythmia */
}
/* integrated beat phase: base rate + the closed-form integral of the RSA term */
static float beat_phase(float t)
{
    float base = S.bpm_override > 0 ? (float)S.bpm_override : 72.0f;
    return t * base / 60.0f - (2.0f / 60.0f) * cosf(2 * PI_F * 0.25f * t) / (2 * PI_F * 0.25f);
}
/* one beat of PPG, phase p in [0,1): systolic upstroke + peak, dicrotic notch, diastolic wave */
static float ppg_shape(float p)
{
    float sys = expf(-((p - 0.16f) / 0.065f) * ((p - 0.16f) / 0.065f));
    float notch = -0.18f * expf(-((p - 0.33f) / 0.03f) * ((p - 0.33f) / 0.03f));
    float dia = 0.38f * expf(-((p - 0.43f) / 0.085f) * ((p - 0.43f) / 0.085f));
    float decay = 0.12f * (1.0f - p);
    return sys + notch + dia + decay;
}

int hal_hr_get(hal_hr *out)
{
    hal_hr r;
    memset(&r, 0, sizeof r);
    memset(r.wave, 128, sizeof r.wave);
    if (!has(EXT_HR)) { if (out) *out = r; return 0; }
    uint32_t n = now_ms();
    S.hr_last = n;
    if (!S.hr_on) { if (out) *out = r; return 1; }
    float on = (n - S.hr_t0) / 1000.0f, t = n / 1000.0f;
    r.on_wrist = on > 0.5f && !S.off_wrist;
    float amp = clampf((on - 0.5f) / 2.0f, 0, 1);
    for (int i = 0; i < 48; i++) {
        float ts = t - (47 - i) * 0.05f;
        float ph = beat_phase(ts);
        float v = ppg_shape(ph - floorf(ph)) - 0.25f;
        float noise = (1.2f - amp) * 0.25f * vnoise(ts * 7.0f, 102) + 0.04f * vnoise(ts * 23.0f, 103);
        float wv = r.on_wrist ? v * amp + noise : 0.05f * vnoise(ts * 5.0f, 104);
        r.wave[i] = (uint8_t)clampf(128.0f + wv * 150.0f, 0, 255);
    }
    if (on > 3.0f) {
        r.bpm = (int)lroundf(bpm_at(t));
        r.quality = clampf(0.55f + (on - 3.0f) * 0.1f, 0, 0.92f) + 0.04f * vnoise(t / 4.0f, 105);
    } else r.quality = clampf((on - 0.5f) * 0.15f, 0, 0.4f);
    if (on > 6.0f) r.spo2 = 97 + (vnoise(t / 20.0f, 106) > 0.2f ? 1 : 0);
    if (out) *out = r;
    return 1;
}
int hal_hr_history(uint8_t *bpm_out, int max)
{
    if (!has(EXT_HR) || !bpm_out || max <= 0) return 0;
    int n = max < 144 ? max : 144;
    hal_datetime dt;
    hal_get_datetime(&dt);
    float hnow = dt.hour + dt.minute / 60.0f;
    for (int i = 0; i < n; i++) {
        float hrs = hnow - (float)(n - 1 - i) / 6.0f;
        float hd = fmodf(hrs + 48.0f, 24.0f);
        int sleep = hd < 6.5f || hd > 23.5f;
        float v = (sleep ? 58.0f : 70.0f) + 6.0f * vnoise((float)i / 5.0f, 107);
        if (!sleep && hd > 17.5f && hd < 18.3f) v += 45.0f;          /* evening walk */
        int gap = hrand((uint32_t)i, 108) < 0.06f;                    /* off-wrist / no lock */
        bpm_out[i] = gap ? 0 : (uint8_t)v;
    }
    return n;
}

/* ================================================================ system */
int hal_button_down(void) { return sim_button_down; }
void hal_display_power(int mode) { S.display_mode = mode; }
void hal_mem_info(uint32_t *psram_free, uint32_t *internal_free)
{
    if (psram_free) *psram_free = 6u << 20;
    if (internal_free) *internal_free = 7u << 10;      /* the device runs this tight */
}

/* ================================================================ sim hooks */
void nos_ext_sim_tick(uint32_t n)
{
    /* the device's NOS_EXT_IDLE_MS auto-stop */
    if (S.gps_on && idle(n, S.gps_last)) S.gps_on = 0;
    if (S.lte_on && idle(n, S.lte_last)) S.lte_on = 0;
    if (S.hr_on && idle(n, S.hr_last)) S.hr_on = 0;
    if (S.gest_on && idle(n, S.gest_last)) S.gest_on = 0;
}

int nos_ext_sim_set(const char *key, const char *value)
{
    double v = value ? atof(value) : 0;
    if (!_stricmp(key, "gesture")) { hal_ext_sim_inject_gesture((int)v); return 1; }
    if (!_stricmp(key, "bpm"))     { S.bpm_override = (int)v; return 1; }
    if (!_stricmp(key, "lux"))     { S.lux_override = (float)v; return 1; }
    if (!_stricmp(key, "heading")) { S.heading_override = (float)v; return 1; }
    if (!_stricmp(key, "db"))      { S.db_override = (float)v; return 1; }
    if (!_stricmp(key, "nofix"))   { S.nofix = v != 0; return 1; }
    if (!_stricmp(key, "magcal"))  { S.mag_calibrated = v != 0; return 1; }
    if (!_stricmp(key, "wrist"))   { S.off_wrist = v == 0; return 1; }   /* set wrist 0 = no skin contact */
    return 0;
}

void hal_ext_debug(const char *cmd)
{
    (void)cmd;
    hal_gps g; hal_gps_get(&g);
    hal_hr r; hal_hr_get(&r);
    hal_env e; hal_env_get(&e);
    printf("[sim] ext present=0x%03x gps=%d valid=%d sats=%d/%d hr=%d bpm=%d env=%.1fC %.1fhPa"
           " haptic=%d@%u sfx=%d@%u sms_sent=%d\n",
           (unsigned)hal_ext_present(), S.gps_on, g.valid, g.sats_used, g.sats_view, S.hr_on, r.bpm,
           (double)e.temp_c, (double)e.pressure_hpa, hal_ext_sim_last_haptic, (unsigned)hal_ext_sim_last_haptic_ms,
           hal_ext_sim_last_sfx, (unsigned)hal_ext_sim_last_sfx_ms, S.sent);
}
