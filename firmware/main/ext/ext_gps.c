/* ext_gps.c — HGLRC M100 Mini (u-blox M10) on UART1: IO7 = ESP RX, IO17 = ESP TX.
 *
 * Boot probe (asynchronous, inside the ext task): install the UART, send a few
 * 0xFF bytes (wakes an M10 left in backup mode by a previous session), then
 * listen for one checksum-valid NMEA sentence at 9600, 38400 and 115200 baud
 * (1.5 s each, two rounds). Found -> EXT_GPS present and the module is put back
 * to sleep. hal_gps_enable(1) on an absent GPS re-runs the probe once.
 *
 * Power: there is no power-gate wire, so "off" is u-blox backup mode via
 * UBX-RXM-PMREQ (duration 0 = until woken, wake source = UART RX activity).
 * The UART driver (and its internal ring buffer) is deleted while off.
 * Enabled streams auto-stop when hal_gps_get() has not been called for
 * NOS_EXT_IDLE_MS.
 *
 * Parser: GGA (fix quality, sats used, HDOP, altitude), RMC (validity, speed,
 * course, UTC), GSA (2D/3D), GSV (sats in view + SNR per satellite, merged
 * across GP/GL/GA/GB/GQ talkers). Any talker prefix is accepted.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ext_priv.h"
#include "driver/uart.h"

#define GPS_UART      UART_NUM_1
#define GPS_RXBUF     512
#define LISTEN_MS     1500
#define STALE_MS      3000

enum { G_PROBE_START, G_PROBE_WAKE, G_PROBE_LISTEN, G_IDLE, G_RUN, G_ABSENT };
static const int BAUDS[3] = { 9600, 38400, 115200 };
#define NTALK 6

typedef struct {
    int state, enabled, baud_i, round, installed, sentences, bad_sum;
    uint32_t t_state, last_get, last_fix, last_rx;
    char line[100]; int len;
    hal_gps fix;                         /* published sample (guarded by ext_lock) */
    uint8_t gsv_snr[NTALK][16]; uint8_t gsv_n[NTALK]; uint8_t gsv_view[NTALK];
    float speed_mps;
} gps_state;
static gps_state *G;

/* ---------------------------------------------------------------- UART */
static int uart_on(int baud)
{
    if (G->installed) { uart_set_baudrate(GPS_UART, baud); uart_flush_input(GPS_UART); return 1; }
    uart_config_t c = { .baud_rate = baud, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                        .source_clk = UART_SCLK_DEFAULT };
    if (uart_driver_install(GPS_UART, GPS_RXBUF, 0, 0, NULL, 0) != ESP_OK) return 0;
    uart_param_config(GPS_UART, &c);
    uart_set_pin(GPS_UART, EXT_PIN_GPS_TX, EXT_PIN_GPS_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    G->installed = 1;
    return 1;
}
static void uart_off(void)
{
    if (!G->installed) return;
    uart_driver_delete(GPS_UART);
    G->installed = 0;
}
static void send_wake(void)
{
    static const uint8_t ff[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uart_write_bytes(GPS_UART, ff, sizeof ff);
}
/* UBX-RXM-PMREQ: backup until UART RX activity. */
static void send_sleep(void)
{
    uint8_t m[8 + 16] = { 0xb5, 0x62, 0x02, 0x41, 16, 0 };
    uint8_t *p = m + 6;
    memset(p, 0, 16);
    p[8] = 0x02;                         /* flags: backup */
    p[12] = 0x08;                        /* wakeupSources: uartrx */
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6 + 16; i++) { a += m[i]; b += a; }
    m[22] = a; m[23] = b;
    uart_write_bytes(GPS_UART, m, sizeof m);
    uart_wait_tx_done(GPS_UART, pdMS_TO_TICKS(60));
}

/* ---------------------------------------------------------------- NMEA */
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static double nmea_deg(const char *v, const char *h)
{
    if (!*v) return 0;
    double x = atof(v);
    int deg = (int)(x / 100);
    double d = deg + (x - deg * 100) / 60.0;
    return (*h == 'S' || *h == 'W') ? -d : d;
}
static int talker_idx(const char *t)
{
    static const char T[NTALK][3] = { "GP", "GL", "GA", "GB", "GQ", "GN" };
    for (int i = 0; i < NTALK; i++) if (t[0] == T[i][0] && t[1] == T[i][1]) return i;
    return NTALK - 1;
}
static void publish_snr(hal_gps *f)
{
    uint8_t all[NTALK * 16]; int n = 0, view = 0;
    for (int t = 0; t < NTALK; t++) {
        view += G->gsv_view[t];
        for (int i = 0; i < G->gsv_n[t]; i++) all[n++] = G->gsv_snr[t][i];
    }
    for (int i = 1; i < n; i++) {                /* insertion sort, strongest first */
        uint8_t v = all[i]; int j = i - 1;
        while (j >= 0 && all[j] < v) { all[j + 1] = all[j]; j--; }
        all[j + 1] = v;
    }
    if (n > 16) n = 16;
    memset(f->snr, 0, sizeof f->snr);
    memcpy(f->snr, all, (size_t)n);
    f->nsnr = n; f->sats_view = view;
}
static void utc_from(hal_gps *f, const char *t)
{
    if (strlen(t) < 6) return;
    f->utc_h = (t[0] - '0') * 10 + t[1] - '0';
    f->utc_m = (t[2] - '0') * 10 + t[3] - '0';
    f->utc_s = (t[4] - '0') * 10 + t[5] - '0';
    f->utc_valid = 1;
}

static void sentence(char *s)
{
    /* s = "GPGGA,....*hh" without the '$' */
    char *star = strchr(s, '*');
    if (!star || hexv(star[1]) < 0 || hexv(star[2]) < 0) { G->bad_sum++; return; }
    uint8_t sum = 0;
    for (char *p = s; p < star; p++) sum ^= (uint8_t)*p;
    if (sum != (uint8_t)(hexv(star[1]) * 16 + hexv(star[2]))) { G->bad_sum++; return; }
    *star = 0;
    char *f[24]; int nf = 0;
    for (char *p = s; nf < 24;) {
        f[nf++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = 0; p = c + 1;
    }
    if (strlen(f[0]) < 5) return;
    G->sentences++;
    G->last_rx = ext_ms();
    const char *type = f[0] + 2;
    uint32_t now = ext_ms();
    portENTER_CRITICAL(&ext_lock);
    hal_gps *x = &G->fix;
    if (!strcmp(type, "GGA") && nf >= 10) {
        int q = atoi(f[6]);
        utc_from(x, f[1]);
        x->sats_used = atoi(f[7]);
        if (*f[8]) x->hdop = (float)atof(f[8]);
        if (q > 0 && *f[2] && *f[4]) {
            x->lat = nmea_deg(f[2], f[3]); x->lon = nmea_deg(f[4], f[5]);
            x->alt_m = (float)atof(f[9]);
            x->valid = 1; G->last_fix = now;
        } else x->valid = 0;
        publish_snr(x);
    } else if (!strcmp(type, "RMC") && nf >= 9) {
        utc_from(x, f[1]);
        if (*f[2] == 'A' && *f[3] && *f[5]) {
            x->lat = nmea_deg(f[3], f[4]); x->lon = nmea_deg(f[5], f[6]);
            x->speed_mps = (float)(atof(f[7]) * 0.514444);
            if (*f[8] && x->speed_mps > 0.5f) x->course_deg = (float)atof(f[8]);
            x->valid = 1; G->last_fix = now;
        }
    } else if (!strcmp(type, "GSA") && nf >= 3) {
        int t = atoi(f[2]);
        x->fix3d = t == 3;
        if (nf >= 17 && *f[16]) x->hdop = (float)atof(f[16]);
    } else if (!strcmp(type, "GSV") && nf >= 4) {
        int ti = talker_idx(f[0]);
        int num = atoi(f[2]);
        if (num == 1) G->gsv_n[ti] = 0;
        G->gsv_view[ti] = (uint8_t)atoi(f[3]);
        for (int i = 4; i + 3 < nf; i += 4) {      /* prn, elev, az, snr; NMEA 4.11 signal id ignored */
            if (!*f[i]) continue;
            if (G->gsv_n[ti] < 16) G->gsv_snr[ti][G->gsv_n[ti]++] = (uint8_t)atoi(f[i + 3]);
        }
    }
    portEXIT_CRITICAL(&ext_lock);
}

static int pump(void)
{
    uint8_t buf[128];
    int total = 0, n;
    while (G->installed && (n = uart_read_bytes(GPS_UART, buf, sizeof buf, 0)) > 0) {
        total += n;
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '$') { G->len = 0; continue; }
            if (c == '\r' || c == '\n') {
                if (G->len > 6) { G->line[G->len] = 0; sentence(G->line); }
                G->len = 0;
                continue;
            }
            if (c < 0x20 || c > 0x7e) { G->len = 0; continue; }
            if (G->len < (int)sizeof G->line - 1) G->line[G->len++] = c;
            else G->len = 0;
        }
    }
    return total;
}

static void go(int st) { G->state = st; G->t_state = ext_ms(); }

/* ---------------------------------------------------------------- service */
uint32_t gps_service(uint32_t now)
{
    if (!G) return 1000;
    switch (G->state) {
    case G_PROBE_START:
        G->sentences = 0;
        if (!uart_on(BAUDS[G->baud_i])) { go(G_ABSENT); return 1000; }
        send_wake();
        go(G_PROBE_WAKE);
        return 100;
    case G_PROBE_WAKE:
        if ((uint32_t)(now - G->t_state) < 600) return 100;
        uart_flush_input(GPS_UART); G->len = 0; G->sentences = 0;
        go(G_PROBE_LISTEN);
        return 100;
    case G_PROBE_LISTEN:
        pump();
        if (G->sentences > 0) {
            ext_present_bits |= EXT_GPS;
            if (G->enabled) go(G_RUN);
            else { send_sleep(); uart_off(); go(G_IDLE); }
            return 100;
        }
        if ((uint32_t)(now - G->t_state) < LISTEN_MS) return 100;
        if (++G->baud_i >= 3) { G->baud_i = 0; G->round++; }
        if (G->round >= 2) { uart_off(); go(G_ABSENT); G->enabled = 0; return 1000; }
        uart_on(BAUDS[G->baud_i]);
        send_wake();
        go(G_PROBE_WAKE);
        return 100;
    case G_RUN:
        pump();
        if (!G->enabled || ext_idle(now, G->last_get)) {
            G->enabled = 0;
            send_sleep(); uart_off();
            portENTER_CRITICAL(&ext_lock);
            G->fix.valid = 0; G->fix.fix3d = 0;
            portEXIT_CRITICAL(&ext_lock);
            go(G_IDLE);
            return 500;
        }
        if (G->fix.valid && (uint32_t)(now - G->last_fix) > STALE_MS) {
            portENTER_CRITICAL(&ext_lock);
            G->fix.valid = 0; G->fix.fix3d = 0;
            portEXIT_CRITICAL(&ext_lock);
        }
        /* 512 B ring at 115200 baud fills in ~44 ms */
        return BAUDS[G->baud_i] > 40000 ? 25 : 80;
    case G_IDLE:
        if (G->enabled) {
            if (!uart_on(BAUDS[G->baud_i])) return 1000;
            send_wake();
            G->last_fix = now - STALE_MS;
            go(G_RUN);
            return 100;
        }
        return 1000;
    default:  /* absent */
        if (G->enabled) { G->round = 0; G->baud_i = 0; go(G_PROBE_START); return 10; }
        return 1000;
    }
}

void gps_init(void)
{
    G = ext_psram_calloc(sizeof *G);
    if (!G) return;
    G->last_get = ext_ms() - NOS_EXT_IDLE_MS - 1;
    go(G_PROBE_START);
}

void hal_gps_enable(int on)
{
    if (!G) return;
    G->last_get = ext_ms();
    G->enabled = on ? 1 : 0;
}

int hal_gps_get(hal_gps *out)
{
    if (!G || !(ext_present_bits & EXT_GPS)) { if (out) memset(out, 0, sizeof *out); return 0; }
    uint32_t now = ext_ms();
    G->last_get = now;
    if (out) {
        portENTER_CRITICAL(&ext_lock);
        *out = G->fix;
        portEXIT_CRITICAL(&ext_lock);
        out->age_ms = G->last_fix ? now - G->last_fix : 0xffffffffu;
    }
    return 1;
}

void gps_debug(void)
{
    if (!G) return;
    hal_gps f;
    portENTER_CRITICAL(&ext_lock);
    f = G->fix;
    portEXIT_CRITICAL(&ext_lock);
    static const char *const S[] = { "probe", "wake", "listen", "sleep", "run", "absent" };
    printf("EXT gps state=%s baud=%d en=%d nmea=%d badsum=%d valid=%d 3d=%d sats=%d/%d hdop=%.1f"
           " lat=%.6f lon=%.6f alt=%.1f spd=%.1f crs=%.0f utc=%02d:%02d:%02d\n",
           S[G->state], BAUDS[G->baud_i], G->enabled, G->sentences, G->bad_sum, f.valid, f.fix3d,
           f.sats_used, f.sats_view, (double)f.hdop, f.lat, f.lon, (double)f.alt_m,
           (double)f.speed_mps, (double)f.course_deg, f.utc_h, f.utc_m, f.utc_s);
}
