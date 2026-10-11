/* ext_audio.c — MAX98357A amp + INMP441 mic on ONE full-duplex I2S port.
 *
 *   IO1 BCLK, IO2 WS (shared), IO5 amp DIN, IO3 mic SD (data in), IO6 amp SD.
 *   16 kHz, Philips format, stereo 32-bit slots carrying 16-bit data: the INMP441
 *   needs 64 BCLK per frame and returns the top 16 of its 24 bits; the amp plays
 *   the left slot (SD driven high = left). TX is initialised first and is the
 *   clock master; RX joins as the full-duplex slave on the same pins.
 *
 * Power: the I2S channels (and their ~2 KB of internal DMA RAM) exist only while
 * something needs them: a sound effect, PCM playback, recording, or a mic getter
 * called within NOS_EXT_IDLE_MS. 1.5 s after the last need the channels are
 * deleted; with BCLK stopped the INMP441 powers down. The amp SD pin is low
 * (shutdown) whenever no samples are being played, with a 60 ms tail.
 *
 * Tasks: "exta" (priority 3, PSRAM stack) pumps I2S in 4 ms blocks and is the
 * only code touching I2S. SD-card file I/O for playback/recording happens in the
 * slow "ext" task (audio_service) through PSRAM rings, so a slow card stalls
 * neither the pump nor the UI.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ext_priv.h"
#include "ext_sfx_synth.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"

#define AU_FS          SFX_FS
#define AU_BLOCK       64                  /* frames per pump iteration (4 ms) */
#define AU_KEEPALIVE   1500                /* ms the port stays up after the last need */
#define AMP_TAIL_MS    60
#define PCM_RING       8192                /* samples, 0.5 s */
#define REC_RING       16384               /* samples, 1 s */
#define REC_MAX_MS     (10 * 60 * 1000)    /* safety cap: a forgotten recorder cannot fill the card */
#define FILE_CHUNK     2048                /* samples per SD transfer */
#define MIC_WARMUP_MS  300                 /* INMP441 mutes ~2^18 SCK after clock start */

typedef struct {
    /* rings (PSRAM) */
    int16_t pcm[PCM_RING];
    int16_t rec[REC_RING];
    int16_t chunk[FILE_CHUNK];
    uint32_t pcm_head, pcm_tail, pcm_gen;    /* indices guarded by ext_lock */
    uint32_t rec_head, rec_tail;
    /* sfx */
    sfx_voice voice[2];
    int req[4]; int nreq; int stop_req;
    int volume;
    /* playback (ext task owns the FILE) */
    FILE *play_req, *play_f;
    int play_req_flag, play_stop_req, playing, play_eof;
    uint32_t play_left, play_req_left;       /* bytes of PCM still in the file */
    uint32_t play_pos;                       /* samples consumed by the pump */
    /* recording */
    FILE *rec_f;
    int recording, rec_stop_req, rec_err;
    uint32_t rec_samples, rec_written, rec_drops;
    /* mic */
    uint32_t mic_last_ms, mic_start_ms;
    int mic_slot;                            /* 0 left, 1 right, -1 unknown */
    int mic_ok;                              /* probe result */
    float ms_avg, dc;
    float db;
    uint8_t wave[32];
    int wave_peak, wave_cnt;
    /* stats */
    int running, amp;
    uint32_t amp_until, need_until, blocks, underruns, starts;
    float lp;
} audio_state;

static audio_state *A;
static TaskHandle_t s_task;
static i2s_chan_handle_t s_tx, s_rx;

static void wake(void) { if (s_task) xTaskNotifyGive(s_task); }

/* ---------------------------------------------------------------- I2S */
static int port_up(void)
{
    if (s_tx) return 1;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = AU_BLOCK;
    cc.auto_clear = true;                    /* underrun -> silence, never a stale loop */
    if (i2s_new_channel(&cc, &s_tx, &s_rx) != ESP_OK) { s_tx = s_rx = NULL; return 0; }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AU_FS),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = EXT_PIN_I2S_BCLK, .ws = EXT_PIN_I2S_WS,
                      .dout = EXT_PIN_AMP_DIN, .din = EXT_PIN_MIC_SD },
    };
    sc.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    if (i2s_channel_init_std_mode(s_tx, &sc) != ESP_OK || i2s_channel_init_std_mode(s_rx, &sc) != ESP_OK ||
        i2s_channel_enable(s_tx) != ESP_OK || i2s_channel_enable(s_rx) != ESP_OK) {
        i2s_del_channel(s_tx); i2s_del_channel(s_rx);
        s_tx = s_rx = NULL;
        return 0;
    }
    A->running = 1; A->starts++;
    A->mic_start_ms = ext_ms();
    A->dc = 0; A->ms_avg = 0;
    return 1;
}

static void port_down(void)
{
    if (!s_tx) return;
    gpio_set_level(EXT_PIN_AMP_SD, 0); A->amp = 0;
    i2s_channel_disable(s_tx); i2s_channel_disable(s_rx);
    i2s_del_channel(s_tx); i2s_del_channel(s_rx);
    s_tx = s_rx = NULL;
    A->running = 0;
    /* Park the clocks low: no BCLK -> the mic sleeps. */
    static const int park[2] = { EXT_PIN_I2S_BCLK, EXT_PIN_I2S_WS };
    for (int i = 0; i < 2; i++) {
        gpio_reset_pin(park[i]);                     /* detach from the I2S matrix signal */
        gpio_set_direction(park[i], GPIO_MODE_OUTPUT);
        gpio_set_level(park[i], 0);
    }
}

/* ---------------------------------------------------------------- mic DSP */
static void mic_block(const int16_t *st, int n, uint32_t now)
{
    int slot = A->mic_slot < 0 ? 0 : A->mic_slot;
    int16_t mono[AU_BLOCK];
    float acc = 0;
    int peak = 0;
    for (int i = 0; i < n; i++) {
        float x = (float)st[2 * i + slot];
        A->dc += (x - A->dc) * 0.002f;            /* ~5 Hz DC/rumble removal */
        float y = x - A->dc;
        acc += y * y;
        int a = (int)(y < 0 ? -y : y);
        if (a > peak) peak = a;
        mono[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
    int warm = (uint32_t)(now - A->mic_start_ms) > MIC_WARMUP_MS;
    if (warm) {
        float ms = acc / (float)n;
        A->ms_avg += (ms - A->ms_avg) * (4.0f / 125.0f);   /* 4 ms block, 125 ms smoothing */
        /* INMP441: -26 dBFS at 94 dB SPL -> SPL = 120 + dBFS (re full-scale amplitude) */
        float db = 120.0f + 10.0f * log10f(A->ms_avg / (32768.0f * 32768.0f) + 1e-12f);
        A->db = db < 30.0f ? 30.0f : db;
        if (peak > A->wave_peak) A->wave_peak = peak;
        if (++A->wave_cnt >= 8) {                          /* one envelope sample per 32 ms */
            float pdb = 120.0f + 20.0f * log10f((float)A->wave_peak / 32768.0f + 1e-6f);
            int v = (int)((pdb - 40.0f) * (255.0f / 60.0f));
            portENTER_CRITICAL(&ext_lock);
            memmove(A->wave, A->wave + 1, 31);
            A->wave[31] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            portEXIT_CRITICAL(&ext_lock);
            A->wave_peak = 0; A->wave_cnt = 0;
        }
    }
    if (A->recording && !A->rec_stop_req && warm) {
        uint32_t head, tail;
        portENTER_CRITICAL(&ext_lock);
        head = A->rec_head; tail = A->rec_tail;
        portEXIT_CRITICAL(&ext_lock);
        if (REC_RING - (head - tail) < (uint32_t)n) { A->rec_drops += n; }
        else {
            for (int i = 0; i < n; i++) A->rec[(head + i) % REC_RING] = mono[i];
            portENTER_CRITICAL(&ext_lock);
            A->rec_head = head + n;
            portEXIT_CRITICAL(&ext_lock);
        }
        A->rec_samples += n;
    }
}

/* Boot probe: run the port ~0.6 s and look for a live data line (the INMP441
 * outputs real noise after its start-up mute; an absent mic reads constant). */
static void mic_probe(void)
{
    int16_t buf[AU_BLOCK * 2];
    int32_t mn[2] = { 32767, 32767 }, mx[2] = { -32768, -32768 };
    if (!port_up()) return;
    uint32_t t0 = ext_ms();
    while ((uint32_t)(ext_ms() - t0) < 650) {
        size_t got = 0;
        memset(buf, 0, sizeof buf);
        i2s_channel_write(s_tx, buf, sizeof buf, &got, 50);
        if (i2s_channel_read(s_rx, buf, sizeof buf, &got, 50) != ESP_OK) continue;
        if ((uint32_t)(ext_ms() - t0) < 400) continue;
        for (int i = 0; i < (int)(got / 4); i++)
            for (int c = 0; c < 2; c++) {
                if (buf[2 * i + c] < mn[c]) mn[c] = buf[2 * i + c];
                if (buf[2 * i + c] > mx[c]) mx[c] = buf[2 * i + c];
            }
    }
    port_down();
    int live0 = mx[0] - mn[0] > 2 && mx[0] - mn[0] < 60000;
    int live1 = mx[1] - mn[1] > 2 && mx[1] - mn[1] < 60000;
    A->mic_slot = live0 ? 0 : live1 ? 1 : -1;
    A->mic_ok = live0 || live1;
    if (A->mic_ok) ext_present_bits |= EXT_MIC;
}

/* ---------------------------------------------------------------- pump */
static int need(uint32_t now)
{
    int n = A->voice[0].id >= 0 || A->voice[1].id >= 0 || A->nreq || A->playing || A->recording ||
            (A->mic_ok && !ext_idle(now, A->mic_last_ms));
    if (n) A->need_until = now + AU_KEEPALIVE;
    return n || !ext_due(now, A->need_until);
}

static void render(int16_t *st, int n, uint32_t now)
{
    float acc[AU_BLOCK];
    memset(acc, 0, sizeof acc);
    int reqs[4], nreq, stop;
    portENTER_CRITICAL(&ext_lock);
    nreq = A->nreq; memcpy(reqs, A->req, sizeof reqs); A->nreq = 0;
    stop = A->stop_req; A->stop_req = 0;
    portEXIT_CRITICAL(&ext_lock);
    if (stop) { A->voice[0].id = A->voice[1].id = -1; }
    for (int r = 0; r < nreq; r++) {
        /* free voice, else steal the one that has played longest */
        int v = A->voice[0].id < 0 ? 0 : A->voice[1].id < 0 ? 1 : (A->voice[0].t > A->voice[1].t ? 0 : 1);
        sfx_voice_start(&A->voice[v], reqs[r]);
    }
    int sfx = 0;
    for (int v = 0; v < 2; v++) sfx |= sfx_voice_render(&A->voice[v], acc, n);
    /* PCM stream */
    int pcm = 0;
    if (A->playing) {
        uint32_t head, tail, gen;
        portENTER_CRITICAL(&ext_lock);
        head = A->pcm_head; tail = A->pcm_tail; gen = A->pcm_gen;
        portEXIT_CRITICAL(&ext_lock);
        uint32_t avail = head - tail, take = avail < (uint32_t)n ? avail : (uint32_t)n;
        for (uint32_t i = 0; i < take; i++) acc[i] += A->pcm[(tail + i) % PCM_RING] * (1.0f / 32768.0f) * 2.2f;
        portENTER_CRITICAL(&ext_lock);
        if (A->pcm_gen == gen) { A->pcm_tail = tail + take; A->play_pos += take; }
        if (!take && A->play_eof) A->playing = 0;
        portEXIT_CRITICAL(&ext_lock);
        if (take < (uint32_t)n && !A->play_eof) A->underruns++;
        pcm = take > 0;
    }
    /* master volume: perceptual (squared) curve; 0.45 keeps squares out of the amp's clip */
    float g = (float)A->volume / 100.0f;
    g = g * g * 0.45f * 32767.0f;
    for (int i = 0; i < n; i++) {
        A->lp += (acc[i] - A->lp) * 0.75f;   /* gentle one-pole: tames naive-square aliasing */
        float y = A->lp * g;
        int16_t s = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
        st[2 * i] = s; st[2 * i + 1] = s;
    }
    if ((sfx || pcm) && A->volume > 0) {
        A->amp_until = now + AMP_TAIL_MS;
        if (!A->amp) { gpio_set_level(EXT_PIN_AMP_SD, 1); A->amp = 1; }
    } else if (A->amp && ext_due(now, A->amp_until)) {
        gpio_set_level(EXT_PIN_AMP_SD, 0); A->amp = 0;
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    int16_t tx[AU_BLOCK * 2], rx[AU_BLOCK * 2];
    mic_probe();
    for (;;) {
        uint32_t now = ext_ms();
        if (!need(now)) {
            port_down();
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!port_up()) {
            /* no internal DMA RAM right now: drop queued blips rather than replay them late */
            portENTER_CRITICAL(&ext_lock);
            A->nreq = 0;
            portEXIT_CRITICAL(&ext_lock);
            A->voice[0].id = A->voice[1].id = -1;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        size_t got = 0;
        render(tx, AU_BLOCK, now);
        i2s_channel_write(s_tx, tx, sizeof tx, &got, 50);
        if (i2s_channel_read(s_rx, rx, sizeof rx, &got, 50) == ESP_OK && A->mic_ok)
            mic_block(rx, (int)(got / 4), now);
        A->blocks++;
    }
}

/* ---------------------------------------------------------------- file I/O (ext task) */
static void wav_header(FILE *f, uint32_t data_bytes)
{
    uint8_t h[44];
    uint32_t v;
    memcpy(h, "RIFF", 4); v = 36 + data_bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8); v = 16; memcpy(h + 16, &v, 4);
    uint16_t fmt[2] = { 1, 1 }; memcpy(h + 20, fmt, 4);                 /* PCM, mono */
    v = AU_FS; memcpy(h + 24, &v, 4); v = AU_FS * 2; memcpy(h + 28, &v, 4);
    uint16_t ba[2] = { 2, 16 }; memcpy(h + 32, ba, 4);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data_bytes, 4);
    fwrite(h, 1, sizeof h, f);
}

static void play_close(void)
{
    if (A->play_f) fclose(A->play_f);
    A->play_f = NULL;
}

uint32_t audio_service(uint32_t now)
{
    if (!A) return 1000;
    uint32_t wait = 200;
    /* ---- playback */
    FILE *req = NULL; int stop;
    portENTER_CRITICAL(&ext_lock);
    if (A->play_req_flag) { req = A->play_req; A->play_req = NULL; A->play_req_flag = 0; }
    stop = A->play_stop_req; A->play_stop_req = 0;
    portEXIT_CRITICAL(&ext_lock);
    if (stop || req) {
        play_close();
        portENTER_CRITICAL(&ext_lock);
        A->pcm_gen++; A->pcm_head = A->pcm_tail = 0; A->play_pos = 0;
        A->playing = 0; A->play_eof = 1;
        portEXIT_CRITICAL(&ext_lock);
    }
    if (req) {
        /* hal_audio_play_file already validated the header and positioned the file
         * at the PCM data; play_left was set there. */
        A->play_f = req;
        A->play_left = A->play_req_left;
        portENTER_CRITICAL(&ext_lock);
        A->play_eof = 0; A->playing = 1;
        portEXIT_CRITICAL(&ext_lock);
        wake();
    }
    if (A->play_f) {
        uint32_t head, tail;
        portENTER_CRITICAL(&ext_lock);
        head = A->pcm_head; tail = A->pcm_tail;
        portEXIT_CRITICAL(&ext_lock);
        uint32_t room = PCM_RING - (head - tail);
        if (room >= FILE_CHUNK) {
            uint32_t want = A->play_left / 2 < FILE_CHUNK ? A->play_left / 2 : FILE_CHUNK;
            size_t got = want ? fread(A->chunk, 2, want, A->play_f) : 0;
            for (size_t i = 0; i < got; i++) A->pcm[(head + i) % PCM_RING] = A->chunk[i];
            A->play_left -= (uint32_t)got * 2;
            portENTER_CRITICAL(&ext_lock);
            A->pcm_head = head + (uint32_t)got;
            if (!got) A->play_eof = 1;
            portEXIT_CRITICAL(&ext_lock);
            if (!got) play_close();
            wait = 20;
        } else wait = 60;
    }
    /* ---- recording */
    if (A->rec_f) {
        uint32_t head, tail;
        portENTER_CRITICAL(&ext_lock);
        head = A->rec_head; tail = A->rec_tail;
        portEXIT_CRITICAL(&ext_lock);
        uint32_t avail = head - tail;
        int stopping = A->rec_stop_req || A->rec_err || A->rec_samples >= (uint32_t)REC_MAX_MS * (AU_FS / 1000);
        while (avail >= FILE_CHUNK || (stopping && avail)) {
            uint32_t n = avail < FILE_CHUNK ? avail : FILE_CHUNK;
            for (uint32_t i = 0; i < n; i++) A->chunk[i] = A->rec[(tail + i) % REC_RING];
            if (fwrite(A->chunk, 2, n, A->rec_f) != n) { A->rec_err = 1; stopping = 1; }
            A->rec_written += n;
            tail += n; avail -= n;
            portENTER_CRITICAL(&ext_lock);
            A->rec_tail = tail;
            portEXIT_CRITICAL(&ext_lock);
            if (A->rec_err) break;
        }
        if (stopping) {
            uint32_t bytes = A->rec_written * 2;
            if (fseek(A->rec_f, 0, SEEK_SET) == 0) wav_header(A->rec_f, bytes);
            fclose(A->rec_f);
            A->rec_f = NULL;
            portENTER_CRITICAL(&ext_lock);
            A->recording = 0; A->rec_stop_req = 0;
            portEXIT_CRITICAL(&ext_lock);
        }
        wait = 50;
    }
    (void)now;
    return wait;
}

/* ---------------------------------------------------------------- API */
void audio_init(void)
{
    gpio_config_t sd = { .pin_bit_mask = 1ULL << EXT_PIN_AMP_SD, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&sd);
    gpio_set_level(EXT_PIN_AMP_SD, 0);               /* amp off until a sound plays */
    A = ext_psram_calloc(sizeof *A);
    if (!A) return;
    A->volume = 70; A->mic_slot = -1; A->db = 30;
    A->voice[0].id = A->voice[1].id = -1;
    A->play_eof = 1;
    A->mic_last_ms = ext_ms() - NOS_EXT_IDLE_MS - 1;
    A->need_until = ext_ms();
    ext_present_bits |= EXT_SPEAKER;                 /* cannot be probed; default on */
    if (xTaskCreatePinnedToCoreWithCaps(audio_task, "exta", 4096, NULL, 3, &s_task,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = NULL;
        ext_present_bits &= ~(uint32_t)(EXT_SPEAKER | EXT_MIC);
    }
}

UBaseType_t audio_stack_free(void) { return s_task ? uxTaskGetStackHighWaterMark(s_task) : 0; }
int audio_amp_on(void) { return A && A->amp; }

void hal_sfx(int id)
{
    if (!A || !s_task || id < 0 || id >= SFX_NRECIPES || !A->volume) return;
    portENTER_CRITICAL(&ext_lock);
    if (A->nreq < 4) A->req[A->nreq++] = id;
    portEXIT_CRITICAL(&ext_lock);
    wake();
}
void hal_sfx_stop(void)
{
    if (!A) return;
    portENTER_CRITICAL(&ext_lock);
    A->stop_req = 1; A->nreq = 0;
    portEXIT_CRITICAL(&ext_lock);
    wake();
}
void hal_audio_volume(int pct) { if (A) A->volume = pct < 0 ? 0 : pct > 100 ? 100 : pct; }

/* Accepts a 16 kHz mono 16-bit WAV (header parsed, "data" chunk located) or a
 * headerless .pcm/.raw file of the same format. */
int hal_audio_play_file(const char *path)
{
    if (!A || !s_task || !path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[12];
    uint32_t data = 0;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size > 12 && fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        uint8_t ch[8];
        int ok = 0;
        while (fread(ch, 1, 8, f) == 8) {
            uint32_t len; memcpy(&len, ch + 4, 4);
            if (!memcmp(ch, "fmt ", 4)) {
                uint8_t fm[16];
                if (len < 16 || fread(fm, 1, 16, f) != 16) break;
                uint16_t fmt, chn, bits; uint32_t rate;
                memcpy(&fmt, fm, 2); memcpy(&chn, fm + 2, 2); memcpy(&rate, fm + 4, 4); memcpy(&bits, fm + 14, 2);
                if (fmt != 1 || chn != 1 || bits != 16 || rate != AU_FS) break;
                fseek(f, (long)(len - 16 + (len & 1)), SEEK_CUR);
            } else if (!memcmp(ch, "data", 4)) { data = len; ok = 1; break; }
            else fseek(f, (long)(len + (len & 1)), SEEK_CUR);
        }
        if (!ok) { fclose(f); return 0; }
    } else {
        fseek(f, 0, SEEK_SET);
        data = (uint32_t)(size > 0 ? size : 0);
    }
    portENTER_CRITICAL(&ext_lock);
    FILE *old = A->play_req_flag ? A->play_req : NULL;
    A->play_req = f; A->play_req_flag = 1; A->play_req_left = data;
    portEXIT_CRITICAL(&ext_lock);
    if (old) fclose(old);
    return 1;
}
int  hal_audio_playing(void) { return A && (A->playing || A->play_req_flag); }
uint32_t hal_audio_play_pos_ms(void) { return A ? A->play_pos / (AU_FS / 1000) : 0; }
void hal_audio_play_stop(void)
{
    if (!A) return;
    portENTER_CRITICAL(&ext_lock);
    A->play_stop_req = 1;
    portEXIT_CRITICAL(&ext_lock);
}

float hal_mic_db(void)
{
    if (!A || !A->mic_ok) return -1.0f;
    A->mic_last_ms = ext_ms();
    if (!A->running) wake();
    return A->db;
}
int hal_mic_wave(uint8_t out[32])
{
    if (!A || !A->mic_ok) return 0;
    A->mic_last_ms = ext_ms();
    if (!A->running) wake();
    portENTER_CRITICAL(&ext_lock);
    memcpy(out, A->wave, 32);
    portEXIT_CRITICAL(&ext_lock);
    return 32;
}
int hal_rec_start(const char *path)
{
    if (!A || !A->mic_ok || !path || A->recording) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    wav_header(f, 0);
    portENTER_CRITICAL(&ext_lock);
    A->rec_head = A->rec_tail = 0;
    A->rec_samples = A->rec_written = A->rec_drops = 0;
    A->rec_err = 0; A->rec_stop_req = 0;
    A->rec_f = f; A->recording = 1;
    portEXIT_CRITICAL(&ext_lock);
    A->mic_last_ms = ext_ms();
    wake();
    return 1;
}
void hal_rec_stop(void)
{
    if (!A) return;
    portENTER_CRITICAL(&ext_lock);
    if (A->recording) A->rec_stop_req = 1;
    portEXIT_CRITICAL(&ext_lock);
}
int hal_rec_active(void) { return A && A->recording && !A->rec_stop_req; }
uint32_t hal_rec_ms(void) { return A ? A->rec_samples / (AU_FS / 1000) : 0; }

void audio_debug(void)
{
    if (!A) { printf("EXT audio: no state\n"); return; }
    printf("EXT audio running=%d amp=%d vol=%d starts=%u blocks=%u underruns=%u voices=%d,%d\n",
           A->running, A->amp, A->volume, (unsigned)A->starts, (unsigned)A->blocks,
           (unsigned)A->underruns, A->voice[0].id, A->voice[1].id);
    printf("EXT mic ok=%d slot=%d db=%.1f | play=%d pos=%ums | rec=%d %ums written=%u drops=%u err=%d\n",
           A->mic_ok, A->mic_slot, (double)A->db, A->playing, (unsigned)hal_audio_play_pos_ms(),
           A->recording, (unsigned)hal_rec_ms(), (unsigned)A->rec_written, (unsigned)A->rec_drops, A->rec_err);
}
