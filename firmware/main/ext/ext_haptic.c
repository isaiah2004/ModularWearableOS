/* ext_haptic.c — coin ERM vibration motor on IO16 (LEDC PWM into the driver
 * transistor, active high).
 *
 * Patterns are tables of {duty %, ms} segments in flash. Playback is an
 * esp_timer one-shot chain: each callback applies a segment's duty and arms the
 * timer for its length, so callers never wait and no task is needed. A new
 * pattern replaces the one playing (the request is latched and picked up by the
 * next callback, which is fired immediately).
 *
 * Strength maps 1-100 % onto 40-100 % duty: a coin ERM stalls below ~40 % and an
 * 8 ms tick would not be felt at all. 0 % disables the motor.
 */
#include <stdio.h>
#include "ext_priv.h"
#include "driver/ledc.h"
#include "esp_log.h"

#define HAP_TIMER   LEDC_TIMER_3
#define HAP_CHAN    LEDC_CHANNEL_7
#define HAP_MODE    LEDC_LOW_SPEED_MODE
#define HAP_RES     LEDC_TIMER_10_BIT
#define HAP_FREQ    20000          /* above hearing: the motor must not whine */

typedef struct { uint8_t duty; uint16_t ms; } hap_seg;
typedef struct { const hap_seg *seg; uint8_t n; uint8_t loop; } hap_pat;

static const hap_seg P_TICK[]   = { {100, 8} };
static const hap_seg P_CLICK[]  = { {100, 15} };
static const hap_seg P_DOUBLE[] = { {100, 14}, {0, 70}, {100, 14} };
static const hap_seg P_HEAVY[]  = { {100, 40} };
static const hap_seg P_BUZZ[]   = { {100, 300} };
static const hap_seg P_ALARM[]  = { {100, 120}, {0, 80}, {100, 120}, {0, 80}, {100, 120}, {0, 700} };
static const hap_seg P_HEART[]  = { {100, 30}, {0, 110}, {70, 22} };   /* lub ... dub */
static const hap_pat PATS[] = {
    { P_TICK, 1, 0 }, { P_CLICK, 1, 0 }, { P_DOUBLE, 3, 0 }, { P_HEAVY, 1, 0 },
    { P_BUZZ, 1, 0 }, { P_ALARM, 6, 1 }, { P_HEART, 3, 0 },
};
#define NPATS ((int)(sizeof PATS / sizeof PATS[0]))

static esp_timer_handle_t s_timer;
static volatile int s_req = -1;      /* pattern requested by hal_haptic, -2 = stop */
static int s_pat = -1, s_step;
static volatile int s_strength = 80;
static volatile uint32_t s_busy_until;
static int s_ok;
static uint32_t s_count;
static int s_callback_active;

static int motor_duty(int pct)
{
    int st = s_strength;
    uint32_t d = 0;
    if (pct > 0 && st > 0) {
        int eff = 40 + 60 * (st > 100 ? 100 : st) / 100;
        d = (uint32_t)((1u << HAP_RES) - 1) * (uint32_t)(eff * pct) / 10000u;
    }
    return ledc_set_duty(HAP_MODE, HAP_CHAN, d)==ESP_OK &&
           ledc_update_duty(HAP_MODE, HAP_CHAN)==ESP_OK;
}

static void hap_cb_impl(void *arg)
{
    (void)arg;
    if(ext_shutdown_requested()){s_pat=-1;motor_duty(0);return;}
    portENTER_CRITICAL(&ext_lock);
    int req = s_req; s_req = -1;
    portEXIT_CRITICAL(&ext_lock);
    if (req == -2) { s_pat = -1; motor_duty(0); return; }
    if (req >= 0) { s_pat = req; s_step = 0; }
    if (s_pat < 0) { motor_duty(0); return; }
    const hap_pat *p = &PATS[s_pat];
    if (s_step >= p->n) {
        if (!p->loop) { s_pat = -1; motor_duty(0); return; }
        s_step = 0;
    }
    const hap_seg *sg = &p->seg[s_step++];
    motor_duty(sg->duty);
    if (sg->duty) s_busy_until = ext_ms() + sg->ms + 40;   /* + spin-down */
    esp_timer_start_once(s_timer, (uint64_t)sg->ms * 1000);
}
static void hap_cb(void *arg)
{
    __atomic_store_n(&s_callback_active,1,__ATOMIC_RELEASE);
    hap_cb_impl(arg);
    __atomic_store_n(&s_callback_active,0,__ATOMIC_RELEASE);
}

static void kick(int req)
{
    if (!s_ok || ext_shutdown_requested()) return;
    portENTER_CRITICAL(&ext_lock);
    s_req = req;
    portEXIT_CRITICAL(&ext_lock);
    esp_timer_stop(s_timer);             /* may fail if not armed: fine */
    esp_timer_start_once(s_timer, 0);
}

void haptic_init(void)
{
    ledc_timer_config_t t = { .speed_mode = HAP_MODE, .duty_resolution = HAP_RES,
                              .timer_num = HAP_TIMER, .freq_hz = HAP_FREQ, .clk_cfg = LEDC_AUTO_CLK };
    ledc_channel_config_t c = { .gpio_num = EXT_PIN_MOTOR, .speed_mode = HAP_MODE,
                                .channel = HAP_CHAN, .timer_sel = HAP_TIMER, .duty = 0, .hpoint = 0 };
    const esp_timer_create_args_t ta = { .callback = hap_cb, .name = "haptic" };
    s_ok = ledc_timer_config(&t) == ESP_OK && ledc_channel_config(&c) == ESP_OK &&
           esp_timer_create(&ta, &s_timer) == ESP_OK;
    /* A motor cannot be probed: it is "present" when the driver came up. */
    s_busy_until = ext_ms();
    if (s_ok) ext_present_bits |= EXT_HAPTIC;
}

/* Values above the pattern range are mapped to the closest pattern by length,
 * for legacy core/ apps that pass a duration in ms (15, 40, 400 ...). */
void hal_haptic(int pattern)
{
    if (!s_ok) return;
    s_count++;
    if (pattern >= 0 && pattern < NPATS) { kick(pattern); return; }
    if (pattern >= NPATS) {
        /* legacy duration: closest pattern */
        kick(pattern <= 10 ? HAPTIC_TICK : pattern <= 25 ? HAPTIC_CLICK :
             pattern <= 120 ? HAPTIC_HEAVY : HAPTIC_BUZZ);
    }
}
void hal_haptic_stop(void) { kick(-2); }
void hal_haptic_set_strength(int pct) { s_strength = pct < 0 ? 0 : pct > 100 ? 100 : pct; }
int  haptic_active(void) { return s_ok && !ext_due(ext_ms(), s_busy_until); }
int haptic_shutdown_poll(uint32_t now)
{
    if(!s_ok)return 1;
    esp_timer_stop(s_timer);
    if(__atomic_load_n(&s_callback_active,__ATOMIC_ACQUIRE))return 0;
    s_req=-2;s_pat=-1;s_busy_until=now;
    return motor_duty(0)?1:-1;
}

void haptic_debug(void)
{
    printf("EXT haptic ok=%d strength=%d playing=%d events=%u\n",
           s_ok, s_strength, s_pat, (unsigned)s_count);
}
