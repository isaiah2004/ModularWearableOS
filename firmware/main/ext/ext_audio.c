#include "sports.h"
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
 * only code touching I2S. A separate low-priority "extf" file owner processes
 * bounded path requests and PSRAM rings, so SD latency cannot stall the shared
 * sensor service task or the I2S pump. Files hold a mount lease until close.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "ext_priv.h"
#include "runtime_admission.h"
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
#define AU_PATH_MAX 256

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
    /* File pointers belong exclusively to extf; callers enqueue paths only. */
    FILE *play_f;
    char play_path[AU_PATH_MAX],rec_path[AU_PATH_MAX],rec_temp[AU_PATH_MAX];
    int play_req_flag,play_opening,play_stop_req,playing,play_eof,play_result;
    int play_lease,rec_lease,close_error,file_idle;
    uint32_t play_left;
    uint32_t play_pos;                       /* samples consumed by the pump */
    /* recording */
    FILE *rec_f;
    int recording,rec_pending,rec_opening,rec_producing,rec_stop_req,rec_err,rec_status,rec_result;
    uint32_t file_retry_at,io_calls,io_max_us,io_errors;
    int last_io_op,last_errno;
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
static TaskHandle_t s_file_task;
static i2s_chan_handle_t s_tx, s_rx;
static int s_tx_enabled,s_rx_enabled,s_audio_parked;

static void wake(void) { if (s_task) xTaskNotifyGive(s_task); }
static void file_wake(void) { if(s_file_task)xTaskNotifyGive(s_file_task); }
static int port_down(void);

/* ---------------------------------------------------------------- I2S */
static int port_up(void)
{
    __atomic_store_n(&s_audio_parked,0,__ATOMIC_RELEASE);
    if (A->running && s_tx && s_rx && s_tx_enabled && s_rx_enabled) return 1;
    if((s_tx||s_rx)&&!port_down())return 0;
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
    int ok=i2s_channel_init_std_mode(s_tx,&sc)==ESP_OK&&i2s_channel_init_std_mode(s_rx,&sc)==ESP_OK;
    if(ok){ok=i2s_channel_enable(s_tx)==ESP_OK;s_tx_enabled=ok;}
    if(ok){ok=i2s_channel_enable(s_rx)==ESP_OK;s_rx_enabled=ok;}
    if(!ok){port_down();return 0;}
    A->running = 1; A->starts++;
    A->mic_start_ms = ext_ms();
    A->dc = 0; A->ms_avg = 0;
    return 1;
}

static int port_down(void)
{
    if(gpio_set_level(EXT_PIN_AMP_SD,0)!=ESP_OK)return 0;
    A->amp=0;
    if(s_tx_enabled){if(i2s_channel_disable(s_tx)!=ESP_OK)return 0;s_tx_enabled=0;}
    if(s_rx_enabled){if(i2s_channel_disable(s_rx)!=ESP_OK)return 0;s_rx_enabled=0;}
    if(s_tx){if(i2s_del_channel(s_tx)!=ESP_OK)return 0;s_tx=NULL;}
    if(s_rx){if(i2s_del_channel(s_rx)!=ESP_OK)return 0;s_rx=NULL;}
    /* Park the clocks low: no BCLK -> the mic sleeps. */
    static const int park[2] = { EXT_PIN_I2S_BCLK, EXT_PIN_I2S_WS };
    for (int i = 0; i < 2; i++) {
        if(gpio_reset_pin(park[i])!=ESP_OK || gpio_set_direction(park[i],GPIO_MODE_OUTPUT)!=ESP_OK ||
           gpio_set_level(park[i],0)!=ESP_OK)return 0;
    }
    A->running=0;
    return 1;
}
static int audio_park(void)
{
    A->voice[0].id=A->voice[1].id=-1;
    portENTER_CRITICAL(&ext_lock);A->nreq=0;portEXIT_CRITICAL(&ext_lock);
    int parked=port_down();__atomic_store_n(&s_audio_parked,parked,__ATOMIC_RELEASE);
    return parked;
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
            float reading[]={A->db,(float)A->wave_peak,(float)v};sports_emit(SP_MIC,reading,3);
            A->wave_peak = 0; A->wave_cnt = 0;
        }
    }
    portENTER_CRITICAL(&ext_lock);
    int capture=A->recording&&!A->rec_stop_req&&warm&&!ext_shutdown_requested();
    if(capture)A->rec_producing=1;
    portEXIT_CRITICAL(&ext_lock);
    if (capture) {
        uint32_t head, tail;
        portENTER_CRITICAL(&ext_lock);
        head = A->rec_head; tail = A->rec_tail;
        portEXIT_CRITICAL(&ext_lock);
        if (REC_RING - (head - tail) < (uint32_t)n) {
            portENTER_CRITICAL(&ext_lock);A->rec_drops+=n;A->rec_err=1;A->rec_stop_req=1;portEXIT_CRITICAL(&ext_lock);
        }
        else {
            for (int i = 0; i < n; i++) A->rec[(head + i) % REC_RING] = mono[i];
            portENTER_CRITICAL(&ext_lock);
            A->rec_head = head + n;
            portEXIT_CRITICAL(&ext_lock);
        }
        portENTER_CRITICAL(&ext_lock);A->rec_samples+=n;A->rec_producing=0;portEXIT_CRITICAL(&ext_lock);
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
    while (ext_elapsed(ext_ms(),t0) < 650 && !ext_shutdown_requested()) {
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
        if(ext_shutdown_requested()) {
            audio_park();
            ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100));continue;
        }
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

/* Dedicated bounded file owner; never called by the sensor task. */
#include "ext_audio_files.inc"

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
    A->file_idle=1;A->rec_result=HAL_REC_DONE;A->play_result=1;
    ext_present_bits |= EXT_SPEAKER;                 /* cannot be probed; default on */
    if (xTaskCreatePinnedToCoreWithCaps(audio_task, "exta", 4096, NULL, 3, &s_task,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = NULL;
        ext_present_bits &= ~(uint32_t)(EXT_SPEAKER | EXT_MIC);
    }
    nos_mem_external("audio_task_stack", 4096, s_task != NULL);
    if(xTaskCreatePinnedToCoreWithCaps(audio_file_task,"extf",4096,NULL,0,&s_file_task,
                                     tskNO_AFFINITY,MALLOC_CAP_SPIRAM)!=pdPASS)s_file_task=NULL;
    nos_mem_external("audio_file_task_stack",4096,s_file_task!=NULL);
}

UBaseType_t audio_stack_free(void) { return s_task ? uxTaskGetStackHighWaterMark(s_task) : 0; }
int audio_amp_on(void) { return A && A->amp; }

void hal_sfx(int id)
{
    if (!A || !s_task || id < 0 || id >= SFX_NRECIPES || !A->volume || ext_shutdown_requested()) return;
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

/* Queue a copied path; validation and opening happen on extf. */
int hal_audio_play_file(const char *path)
{
    if(!A||!s_task||!s_file_task||!path||!*path||strlen(path)>=AU_PATH_MAX||ext_shutdown_requested()||
       !nos_runtime_recording_allowed())return 0;
    portENTER_CRITICAL(&ext_lock);
    if(A->play_req_flag||A->close_error||ext_shutdown_requested()||!nos_runtime_recording_allowed()){portEXIT_CRITICAL(&ext_lock);return 0;}
    snprintf(A->play_path,sizeof A->play_path,"%s",path);A->play_req_flag=1;A->play_result=0;
    portEXIT_CRITICAL(&ext_lock);file_wake();return 1;
}
int hal_audio_playing(void)
{
    if(!A)return 0;
    int playing;
    portENTER_CRITICAL(&ext_lock);playing=A->playing||A->play_req_flag||A->play_opening;portEXIT_CRITICAL(&ext_lock);
    return playing;
}
int hal_audio_play_result(void)
{
    if(!A)return -1;
    portENTER_CRITICAL(&ext_lock);int result=A->play_result;
    if(result==0&&!A->playing&&!A->play_req_flag&&!A->play_opening)result=1;
    portEXIT_CRITICAL(&ext_lock);return result;
}
uint32_t hal_audio_play_pos_ms(void){return A?A->play_pos/(AU_FS/1000):0;}
void hal_audio_play_stop(void)
{
    if(!A)return;
    portENTER_CRITICAL(&ext_lock);A->play_stop_req=1;portEXIT_CRITICAL(&ext_lock);file_wake();
}
float hal_mic_db(void)
{
    if(!A||!A->mic_ok||ext_shutdown_requested())return -1;
    A->mic_last_ms=ext_ms();if(!A->running)wake();return A->db;
}
int hal_mic_wave(uint8_t out[32])
{
    if(!A||!A->mic_ok||!out||ext_shutdown_requested())return 0;
    A->mic_last_ms=ext_ms();if(!A->running)wake();
    portENTER_CRITICAL(&ext_lock);memcpy(out,A->wave,32);portEXIT_CRITICAL(&ext_lock);return 32;
}
int hal_rec_start_exclusive(const char *path)
{
    if(!A||!s_task||!s_file_task||!A->mic_ok||!path||!*path||strlen(path)>=AU_PATH_MAX-6||ext_shutdown_requested()||
       !nos_runtime_recording_allowed())return 0;
    portENTER_CRITICAL(&ext_lock);
    if(A->rec_status==1||A->rec_status==2||A->rec_f||A->close_error||ext_shutdown_requested()||!nos_runtime_recording_allowed()){
        portEXIT_CRITICAL(&ext_lock);return 0;
    }
    snprintf(A->rec_path,sizeof A->rec_path,"%s",path);
    A->rec_head=A->rec_tail=0;A->rec_samples=A->rec_written=A->rec_drops=0;
    A->rec_err=A->rec_stop_req=A->rec_producing=0;A->file_retry_at=ext_ms();
    A->rec_pending=1;A->rec_status=1;A->rec_result=HAL_REC_PENDING;
    portEXIT_CRITICAL(&ext_lock);A->mic_last_ms=ext_ms();file_wake();return 1;
}
int hal_rec_start(const char *path){return hal_rec_start_exclusive(path)==1;}
void hal_rec_stop(void)
{
    if(!A)return;
    portENTER_CRITICAL(&ext_lock);
    if(A->rec_status==1||A->rec_status==2){A->rec_stop_req=1;A->rec_status=2;}
    portEXIT_CRITICAL(&ext_lock);file_wake();
}
int hal_rec_status(void)
{
    if(!A)return 0;
    portENTER_CRITICAL(&ext_lock);int status=A->rec_status;portEXIT_CRITICAL(&ext_lock);return status;
}
int hal_rec_result(void)
{
    if(!A)return HAL_REC_ERROR;
    portENTER_CRITICAL(&ext_lock);int result=A->rec_result;portEXIT_CRITICAL(&ext_lock);return result;
}
int hal_rec_active(void){int status=hal_rec_status();return status==1||status==2;}
uint32_t hal_rec_ms(void){return A?A->rec_samples/(AU_FS/1000):0;}
int audio_shutdown_poll(uint32_t now)
{
    (void)now;
    if(!A)return 1;
    hal_sfx_stop();hal_audio_play_stop();hal_rec_stop();wake();file_wake();
    if(!s_task)audio_park();
    portENTER_CRITICAL(&ext_lock);
    int idle=A->file_idle&&!A->playing&&!A->recording&&!A->play_req_flag&&!A->play_opening&&!A->rec_pending&&!A->rec_opening;
    int error=A->close_error;
    portEXIT_CRITICAL(&ext_lock);
    if(error)return -1;
    return idle&&__atomic_load_n(&s_audio_parked,__ATOMIC_ACQUIRE)?1:0;
}

void audio_debug(void)
{
    if (!A) { printf("EXT audio: no state\n"); return; }
    printf("EXT audio running=%d amp=%d vol=%d starts=%u blocks=%u underruns=%u voices=%d,%d\n",
           A->running, A->amp, A->volume, (unsigned)A->starts, (unsigned)A->blocks,
           (unsigned)A->underruns, A->voice[0].id, A->voice[1].id);
    printf("EXT mic ok=%d slot=%d db=%.1f | play=%d pos=%ums | rec=%d %ums written=%u drops=%u err=%d\n",
           A->mic_ok, A->mic_slot, (double)A->db, A->playing, (unsigned)hal_audio_play_pos_ms(),
           A->recording, (unsigned)hal_rec_ms(), (unsigned)A->rec_written, (unsigned)A->rec_drops, A->rec_err);
    printf("EXT audio_fs task=%d stack_free=%u pending=%d/%d rec_status=%d result=%d leases=%d/%d close_error=%d io_calls=%u io_max_us=%u io_errors=%u last_op=%d errno=%d parked=%d\n",
           s_file_task!=NULL,s_file_task?(unsigned)uxTaskGetStackHighWaterMark(s_file_task):0,
           A->play_req_flag,A->rec_pending,A->rec_status,A->rec_result,A->play_lease,A->rec_lease,A->close_error,
           (unsigned)A->io_calls,(unsigned)A->io_max_us,(unsigned)A->io_errors,A->last_io_op,A->last_errno,
           __atomic_load_n(&s_audio_parked,__ATOMIC_ACQUIRE));
}
