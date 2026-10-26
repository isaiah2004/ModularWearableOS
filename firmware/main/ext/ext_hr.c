#include "sports.h"
/* ext_hr.c — MAX30102 heart rate / SpO2 at 0x57 (part id 0x15).
 *
 * Acquisition: SpO2 mode (red + IR), 400 sps with 4-sample averaging = 100 Hz
 * into the FIFO, 411 us pulses (18-bit), 4096 nA range, FIFO rollover on. The ext
 * task drains the 32-deep FIFO every 120 ms in one burst. Shutdown (SHDN, ~0.7 uA)
 * whenever neither the user stream nor a background measurement runs.
 *
 * DSP per 100 Hz sample, per channel:
 *   DC   = EMA (0.32 Hz high-pass by subtraction)
 *   AC   = 2nd-order Butterworth low-pass at 4 Hz on (x - DC) -> 0.3-4 Hz band
 *   PPG  = -AC(IR) (blood volume absorbs light, so systole is a dip; inverted for display)
 * Beats: local maxima above 40 % of a decaying envelope, refractory 330 ms
 *   (<= 180 bpm); inter-beat intervals 40-180 bpm go to an 8-deep history;
 *   lock = >= 4 intervals with spread (MAD / median) < 15 %; bpm = 6000 / median.
 * Quality 0-1 = interval regularity x perfusion index (AC/DC of IR).
 * SpO2: ratio of ratios R = (ACrms/DC)red / (ACrms/DC)ir over 4 s windows,
 *   SpO2 = -45.06 R^2 + 30.354 R + 94.845 (Maxim reference curve), reported only
 *   when locked with quality >= 0.5. NOT a medical measurement, especially through
 *   a strap window on the side of the wrist.
 * On-wrist: IR DC above 25000 counts (of 262143) for 0.5 s.
 * LED current auto-ranges once a second to keep IR DC in 50k-200k counts.
 * Background: every 10 min a measurement runs (<= 40 s, ends early after 8 s of
 *   good lock) and appends the bpm (0 = no reading) to a 144-entry history.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "ext_priv.h"
#include "hal.h"

#define FS          100
#define BG_EVERY_MS (10 * 60 * 1000)
#define BG_MAX_MS   40000
#define HIST_N      144
#define WRIST_DC    25000.0f

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } biquad;

typedef struct {
    i2c_master_dev_handle_t dev;
    uint8_t rev;
    int enabled, running, bg, errors, shutdown_pending;
    uint32_t fifo_pointer_errors,fifo_payload_errors,fifo_overflow_reads;
    unsigned owners;
    uint32_t last_get, next, next_bg, bg_t0, good_since, led_t;
    uint8_t pa_red, pa_ir;
    /* DSP */
    uint32_t n;                     /* samples since start */
    float dc_ir, dc_red;
    biquad lp_ir, lp_red;
    float prev, prev2, env;
    uint32_t last_peak, settle_until;
    uint16_t ibi[8]; int nibi, ibi_head;
    float sq_ir, sq_red, sum_dc_ir, sum_dc_red; int win;
    uint32_t wrist_n;
    float spo2_f;
    int wdec;
    hal_hr cur;
    hal_hr_sample hist[HIST_N]; int hn, hhead;
    uint32_t sample_epoch, restart_at;
    uint32_t beats;
} hr_state;
static hr_state *H;

static void bq_init(biquad *q, float fc)
{
    float K = tanf(3.14159265f * fc / FS), r2 = 1.41421356f;
    float nrm = 1.0f / (1 + r2 * K + K * K);
    q->b0 = K * K * nrm; q->b1 = 2 * q->b0; q->b2 = q->b0;
    q->a1 = 2 * (K * K - 1) * nrm; q->a2 = (1 - r2 * K + K * K) * nrm;
    q->z1 = q->z2 = 0;
}
static float bq(biquad *q, float x)
{
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static int leds(void) { return ext_wr(H->dev, 0x0C, H->pa_red) && ext_wr(H->dev, 0x0D, H->pa_ir); }

static void dsp_reset(void)
{
    H->n = 0; H->dc_ir = H->dc_red = 0;
    bq_init(&H->lp_ir, 4.0f); bq_init(&H->lp_red, 4.0f);
    H->prev = H->prev2 = 0; H->env = 0;
    H->last_peak = 0; H->settle_until = 2 * FS;
    H->nibi = H->ibi_head = 0;
    H->sq_ir = H->sq_red = H->sum_dc_ir = H->sum_dc_red = 0; H->win = 0;
    H->wrist_n = 0; H->spo2_f = 0; H->wdec = 0;
    portENTER_CRITICAL(&ext_lock);
    memset(&H->cur, 0, sizeof H->cur);
    memset(H->cur.wave, 128, sizeof H->cur.wave);
    portEXIT_CRITICAL(&ext_lock);
}

static int start(void)
{
    H->pa_red = H->pa_ir = 0x1F;                        /* ~6.2 mA each */
    int ok = ext_wr(H->dev, 0x09, 0x40);               /* reset */
    vTaskDelay(pdMS_TO_TICKS(2));
    ok = ok && ext_wr(H->dev, 0x02, 0x00) && ext_wr(H->dev, 0x03, 0x00) &&   /* no interrupts */
         ext_wr(H->dev, 0x08, 0x5F) &&                 /* avg 4, rollover, A_FULL 15 */
         ext_wr(H->dev, 0x0A, 0x2F) &&                 /* 4096 nA, 400 sps, 411 us */
         leds() &&
         ext_wr(H->dev, 0x04, 0) && ext_wr(H->dev, 0x05, 0) && ext_wr(H->dev, 0x06, 0) &&
         ext_wr(H->dev, 0x09, 0x03);                   /* SpO2 mode */
    dsp_reset();
    H->running = ok;
    H->led_t = ext_ms();
    return ok;
}
static void stop(void)
{
    ext_wr(H->dev, 0x09, 0x83);                        /* SHDN */
    H->running = 0;
    portENTER_CRITICAL(&ext_lock);
    H->cur.on_wrist = 0; H->cur.bpm = 0; H->cur.spo2 = 0; H->cur.quality = 0;
    portEXIT_CRITICAL(&ext_lock);
}

static int cmp16(const void *a, const void *b) { return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }

static void beat(uint32_t t)
{
    if (H->last_peak) {
        uint32_t d = t - H->last_peak;
        if (d >= FS * 60 / 180 && d <= FS * 60 / 40) {
            H->ibi[H->ibi_head] = (uint16_t)d;
            H->ibi_head = (H->ibi_head + 1) & 7;
            if (H->nibi < 8) H->nibi++;
        } else if (d > FS * 60 / 40) H->nibi = 0;       /* gap: start over */
    }
    H->last_peak = t;
    H->beats++;
}

static void evaluate(hal_hr *o)
{
    o->bpm = 0; o->quality = 0;
    /* A live FIFO does not make old inter-beat intervals a fresh HR estimate. */
    if (!o->on_wrist || H->nibi < 4 || !H->last_peak || H->n - H->last_peak > 3u * FS) return;
    uint16_t s[8], dev[8];
    memcpy(s, H->ibi, sizeof s);
    qsort(s, (size_t)H->nibi, sizeof s[0], cmp16);
    uint16_t med = s[H->nibi / 2];
    for (int i = 0; i < H->nibi; i++) dev[i] = (uint16_t)abs((int)s[i] - (int)med);
    qsort(dev, (size_t)H->nibi, sizeof dev[0], cmp16);
    float spread = (float)dev[H->nibi / 2] / (float)med;
    float pi = H->dc_ir > 1 ? H->env / H->dc_ir : 0;   /* perfusion index proxy */
    float qp = pi < 0.0003f ? 0 : pi > 0.003f ? 1 : (pi - 0.0003f) / 0.0027f;
    float qr = 1.0f - spread * 5.0f;
    if (qr < 0) qr = 0;
    o->quality = qr * (0.5f + 0.5f * qp);
    if (spread < 0.15f) o->bpm = (int)(6000.0f / med + 0.5f);
}

static void sample(uint32_t red, uint32_t ir, hal_hr *o)
{
    uint32_t t = H->n++;
    float fi = (float)ir, fr = (float)red;
    if (t == 0) { H->dc_ir = fi; H->dc_red = fr; }
    H->dc_ir += (fi - H->dc_ir) * 0.02f;
    H->dc_red += (fr - H->dc_red) * 0.02f;
    float ai = bq(&H->lp_ir, fi - H->dc_ir), ar = bq(&H->lp_red, fr - H->dc_red);
    float s = -ai;
    /* on-wrist */
    if (H->dc_ir > WRIST_DC) { if (H->wrist_n < 1000000) H->wrist_n++; } else H->wrist_n = 0;
    o->on_wrist = H->wrist_n > FS / 2;
    /* envelope + peaks */
    float a = s < 0 ? -s : s;
    H->env = a > H->env ? a : H->env * 0.995f;
    if (t > H->settle_until && o->on_wrist) {
        /* prev was a local maximum */
        if (H->prev > H->prev2 && H->prev >= s && H->prev > 0.4f * H->env &&
            (!H->last_peak || t - 1 - H->last_peak >= FS * 60 / 180))
            beat(t - 1);
    }
    H->prev2 = H->prev; H->prev = s;
    /* SpO2 window */
    H->sq_ir += ai * ai; H->sq_red += ar * ar; H->sum_dc_ir += H->dc_ir; H->sum_dc_red += H->dc_red;
    if (++H->win >= 4 * FS) {
        float dci = H->sum_dc_ir / H->win, dcr = H->sum_dc_red / H->win;
        float ri = sqrtf(H->sq_ir / H->win), rr = sqrtf(H->sq_red / H->win);
        if (dci > 1 && dcr > 1 && ri > 0) {
            float R = (rr / dcr) / (ri / dci);
            float sp = -45.060f * R * R + 30.354f * R + 94.845f;
            if (sp >= 70 && sp <= 100.5f) H->spo2_f = H->spo2_f ? H->spo2_f + (sp - H->spo2_f) * 0.3f : sp;
            else H->spo2_f = 0;
        }
        H->sq_ir = H->sq_red = H->sum_dc_ir = H->sum_dc_red = 0; H->win = 0;
    }
    /* live trace: 20 Hz, 48 points = 2.4 s */
    if (++H->wdec >= FS / 20) {
        H->wdec = 0;
        float v = H->env > 1 ? 128.0f + s / H->env * 110.0f : 128.0f;
        memmove(o->wave, o->wave + 1, sizeof o->wave - 1);
        o->wave[sizeof o->wave - 1] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

static void auto_led(uint32_t now)
{
    if ((uint32_t)(now - H->led_t) < 1000) return;
    H->led_t = now;
    int ch = 0;
    if (H->dc_ir > 200000 && H->pa_ir > 4) { H->pa_ir -= 4; ch = 1; }
    else if (H->dc_ir < 50000 && H->dc_ir > 8000 && H->pa_ir < 0x9F) { H->pa_ir += 4; ch = 1; }
    if (H->dc_red > 200000 && H->pa_red > 4) { H->pa_red -= 4; ch = 1; }
    else if (H->dc_red < 50000 && H->dc_red > 8000 && H->pa_red < 0x9F) { H->pa_red += 4; ch = 1; }
    if (ch && leds()) H->settle_until = H->n + FS;      /* step transient: ignore peaks 1 s */
}

static void hist_push(int bpm)
{
    uint32_t now=ext_ms();
    uint32_t epoch=(uint32_t)hal_epoch_seconds();
    portENTER_CRITICAL(&ext_lock);
    if(!H->cur.sample_ms || ext_elapsed(now,H->cur.sample_ms)>2000) bpm=0;
    H->hist[H->hhead] = (hal_hr_sample){bpm?H->cur.sample_ms:now,
        bpm?H->sample_epoch:epoch,(uint16_t)(bpm<0?0:bpm>255?255:bpm)};
    H->hhead = (H->hhead + 1) % HIST_N;
    if (H->hn < HIST_N) H->hn++;
    portEXIT_CRITICAL(&ext_lock);
}

uint32_t hr_service(uint32_t now)
{
    if (!H) return 1000;
    if(H->shutdown_pending) {
        if(!ext_wr(H->dev,0x09,0x83))return 120;
        H->shutdown_pending=0;H->running=0;
    }
    portENTER_CRITICAL(&ext_lock);
    if(H->enabled && ext_idle(now,H->last_get)) {H->owners=0;H->enabled=0;}
    int enabled=H->enabled;
    portEXIT_CRITICAL(&ext_lock);
    /* Deadlines must precede every FIFO/read failure/empty early return. */
    if(H->bg && ext_elapsed(now,H->bg_t0)>=BG_MAX_MS) {
        hist_push(0);H->bg=0;H->good_since=0;
    }
    /* background schedule */
    if (ext_due(now, H->next_bg)) {
        H->next_bg = now + BG_EVERY_MS;
        if (H->running && enabled) hist_push(H->cur.bpm);     /* user stream already live */
        else { H->bg = 1; H->bg_t0 = now; H->good_since = 0; }
    }
    int want = enabled || H->bg;
    if(want && !H->running && H->restart_at && !ext_due(now,H->restart_at)) return 120;
    if (want && !H->running) { if (!start()) { H->errors++; H->bg = 0;H->restart_at=now+5000; return 1000; } H->restart_at=0;H->next = now + 120; }
    if (!want && H->running) stop();
    if (!H->running) return 1000;
    if (!ext_due(now, H->next)) return H->next - now;
    H->next = now + 120;

    uint8_t p[3];
    if (!ext_rd(H->dev, 0x04, p, 3)) {
        H->fifo_pointer_errors++;
        if(++H->errors>10) {H->bg=0;H->good_since=0;stop();H->restart_at=now+5000;}
        return 120;
    }
    int n = (p[0] - p[2]) & 31;
    if (p[1]) {H->fifo_overflow_reads++;n=32;}             /* observed overflow flag, not a sample-loss total */
    if (n) {
        uint8_t buf[32 * 6];
        if (!ext_rd(H->dev, 0x07, buf, (size_t)n * 6)) {
            H->fifo_payload_errors++;
            if(++H->errors>10) {H->bg=0;H->good_since=0;stop();H->restart_at=now+5000;}
            return 120;
        }
        H->errors = 0;
        hal_hr o;
        portENTER_CRITICAL(&ext_lock);
        o = H->cur;
        portEXIT_CRITICAL(&ext_lock);
        for (int i = 0; i < n; i++) {
            const uint8_t *b = buf + 6 * i;
            uint32_t red = ((uint32_t)b[0] << 16 | (uint32_t)b[1] << 8 | b[2]) & 0x3FFFF;
            uint32_t ir  = ((uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | b[5]) & 0x3FFFF;
            sample(red, ir, &o);
            float pv[]={(float)red,(float)ir};sports_emit(SP_PPG,pv,2);
        }
        evaluate(&o);
        now=ext_ms();
        o.sample_ms = now;
        o.spo2 = o.bpm && o.quality >= 0.5f && H->spo2_f > 0 ? (int)(H->spo2_f + 0.5f) : 0;
        if (o.spo2 > 100) o.spo2 = 100;
        uint32_t epoch=(uint32_t)hal_epoch_seconds();
        portENTER_CRITICAL(&ext_lock);
        H->cur = o;
        H->sample_epoch=epoch;
        portEXIT_CRITICAL(&ext_lock);
    {float sv[]={ (float)o.on_wrist,(float)o.bpm,(float)o.spo2,o.quality };sports_emit(SP_HEART,sv,4);}
        auto_led(now);
        if (H->bg) {
            if (o.bpm && o.quality >= 0.6f) { if (!H->good_since) H->good_since = now; }
            else H->good_since = 0;
            int done = (H->good_since && (uint32_t)(now - H->good_since) > 8000) ||
                       (uint32_t)(now - H->bg_t0) > BG_MAX_MS ||
                       ((uint32_t)(now - H->bg_t0) > 5000 && !o.on_wrist);
            if (done) { hist_push(H->good_since ? o.bpm : 0); H->bg = 0; }
        }
    }
    return 120;
}

void hr_init(void)
{
    i2c_master_dev_handle_t d;
    uint8_t id = 0, rev = 0;
    if (!ext_i2c_open(0x57, &d)) return;
    if (!ext_rd(d, 0xFF, &id, 1) || id != 0x15) { i2c_master_bus_rm_device(d); return; }
    ext_rd(d, 0xFE, &rev, 1);
    H = ext_psram_calloc(sizeof *H);
    if (!H) { i2c_master_bus_rm_device(d); return; }
    H->dev = d; H->rev = rev;
    ext_wr(d, 0x09, 0x83);                              /* shutdown until needed */
    H->last_get = ext_ms() - NOS_EXT_IDLE_MS - 1;
    H->next_bg = ext_ms() + 2 * 60 * 1000;              /* first background sample 2 min after boot */
    dsp_reset();
    ext_present_bits |= EXT_HR;
}

void hal_hr_enable(int on)
{ hal_hr_request(HAL_HR_OWNER_LEGACY,on); }

void hal_hr_request(unsigned owner,int on)
{
    if (!H) return;
    if(on && ext_shutdown_requested())return;
    owner&=HAL_HR_OWNER_WORKOUT|HAL_HR_OWNER_HEART|HAL_HR_OWNER_BREATHE|HAL_HR_OWNER_LEGACY;
    if(!owner) return;
    portENTER_CRITICAL(&ext_lock);
    H->last_get = ext_ms();
    if(on) H->owners|=owner;else H->owners&=~owner;
    H->enabled=H->owners!=0;
    portEXIT_CRITICAL(&ext_lock);
}

int hal_hr_get(hal_hr *out)
{
    if (!H) { if (out) memset(out, 0, sizeof *out); return 0; }
    uint32_t now=ext_ms();
    portENTER_CRITICAL(&ext_lock);
    if(!ext_shutdown_requested())H->last_get = now;
    if (out) {
        *out = H->cur;
        if(!out->sample_ms || ext_elapsed(now,out->sample_ms)>2000) {
            out->on_wrist=out->bpm=out->spo2=0;out->quality=0;
        }
    }
    portEXIT_CRITICAL(&ext_lock);
    return 1;
}
int hr_shutdown_poll(uint32_t now)
{
    (void)now;
    if(!H)return 1;
    H->shutdown_pending=1;
    portENTER_CRITICAL(&ext_lock);H->owners=0;H->enabled=H->bg=0;portEXIT_CRITICAL(&ext_lock);
    if(!ext_wr(H->dev,0x09,0x83))return -1;
    H->shutdown_pending=0;
    H->running=0;
    portENTER_CRITICAL(&ext_lock);
    H->cur.on_wrist=H->cur.bpm=H->cur.spo2=0;H->cur.quality=0;
    portEXIT_CRITICAL(&ext_lock);
    return 1;
}

int hal_hr_history(uint8_t *bpm_out, int max)
{
    if (!H || !bpm_out || max <= 0) return 0;
    portENTER_CRITICAL(&ext_lock);
    int n = H->hn < max ? H->hn : max;
    int s = (H->hhead - n + HIST_N) % HIST_N;
    for (int i = 0; i < n; i++) bpm_out[i] = (uint8_t)H->hist[(s + i) % HIST_N].bpm;
    portEXIT_CRITICAL(&ext_lock);
    return n;
}

int hal_hr_history_samples(hal_hr_sample *out,int max)
{
    if(!H || !out || max<=0) return 0;
    portENTER_CRITICAL(&ext_lock);
    int n=H->hn<max?H->hn:max,s=(H->hhead-n+HIST_N)%HIST_N;
    for(int i=0;i<n;i++) out[i]=H->hist[(s+i)%HIST_N];
    portEXIT_CRITICAL(&ext_lock);
    return n;
}

void hr_debug(void)
{
    if (!H) { printf("EXT hr absent (no MAX30102 at 0x57)\n"); return; }
    hal_hr v;
    portENTER_CRITICAL(&ext_lock);
    v = H->cur;
    portEXIT_CRITICAL(&ext_lock);
    printf("EXT hr rev=%u en=%d run=%d bg=%d wrist=%d bpm=%d spo2=%d q=%.2f dc_ir=%.0f dc_red=%.0f pa=%u/%u"
           " beats=%u ibi=%d hist=%d err=%d\n",
           H->rev, H->enabled, H->running, H->bg, v.on_wrist, v.bpm, v.spo2, (double)v.quality,
           (double)H->dc_ir, (double)H->dc_red, H->pa_red, H->pa_ir, (unsigned)H->beats, H->nibi, H->hn, H->errors);
    printf("EXT hr fifo_pointer_errors=%u fifo_payload_errors=%u fifo_overflow_reads=%u\n",
           (unsigned)H->fifo_pointer_errors,(unsigned)H->fifo_payload_errors,(unsigned)H->fifo_overflow_reads);
}
