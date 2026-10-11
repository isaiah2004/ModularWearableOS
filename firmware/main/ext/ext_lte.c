/* ext_lte.c — Quectel EG800AK (7Semi Pico) over UART0 (IO43 TX / IO44 RX,
 * 115200 8N1), PWRKEY on IO18.
 *
 * UART0 is free: the console is USB-Serial-JTAG (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG,
 * CONFIG_ESP_CONSOLE_UART_NUM=-1). The ROM bootloader still prints its banner on
 * U0TXD at reset; the modem sees one line of garbage and answers ERROR, harmless.
 *
 * PWRKEY polarity is not documented for the 7Semi carrier. Default: open-drain,
 * "press" = pull low, "release" = high-Z (never drives the modem's PWRKEY above its
 * own pull-up, so it is safe if the pin goes straight to the module). If the board
 * buffers PWRKEY with an NPN (MCU high = press), build with
 * EXT_LTE_PWRKEY_ACTIVE_HIGH=1. Before pressing, the driver always tries "AT":
 * a carrier that auto-powers the modem works either way.
 *
 * Non-blocking AT engine: one command in flight, a line parser that recognises
 * responses and URCs anywhere, and a small scheduler in READY state:
 *   power-on: AT x3 -> (PWRKEY 550 ms, wait <=15 s for AT) -> init script
 *   init:     ATE0, CMEE=1, CPIN?, CMGF=1, CSCS="GSM", CNMI=2,1, COPS=3,0
 *   poll:     CSQ / CEREG? / CREG? / COPS? round-robin, one per 1.25 s
 *   inbox:    CMGL="ALL",1 (Quectel: list without marking read) every 60 s and on +CMTI
 *   send:     CMGS="<to>" -> "> " prompt -> text + Ctrl-Z -> +CMGS / OK (<= 60 s)
 *   power-off: AT+QPOWD=1, wait POWERED DOWN (<= 8 s), else PWRKEY 1 s.
 * Boot probe powers the modem once, caches the SIM state and the inbox (so the
 * unread count is known with LTE off), then powers it down again unless enabled.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "ext_priv.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#ifndef EXT_LTE_PWRKEY_ACTIVE_HIGH
#define EXT_LTE_PWRKEY_ACTIVE_HIGH 0
#endif
#define LTE_UART   UART_NUM_0
#define LTE_RXBUF  1024
#define MAX_SMS    12

enum { L_OFF, L_CHECK, L_KEY, L_BOOT, L_INIT, L_READY, L_PWROFF, L_KEYOFF, L_ABSENT };
enum { R_NONE, R_OK, R_ERR, R_TMO, R_PROMPT };
enum { C_NONE, C_GENERIC, C_CPIN, C_CMGL, C_CMGL0, C_CMGS_HDR, C_CMGS_BODY, C_RAW, C_QPOWD };

static const char *const INIT_CMDS[] = {
    "ATE0", "AT+CMEE=1", "AT+CPIN?", "AT+CMGF=1", "AT+CSCS=\"GSM\"", "AT+CNMI=2,1,0,0,0", "AT+COPS=3,0",
};
#define N_INIT ((int)(sizeof INIT_CMDS / sizeof INIT_CMDS[0]))
static const char *const POLL_CMDS[] = { "AT+CSQ", "AT+CEREG?", "AT+CREG?", "AT+COPS?" };

typedef struct {
    int state, enabled, probing, step, tries, installed;
    uint32_t t_state, last_get, next_poll, next_inbox, next_cpin;
    int poll_i;
    /* in-flight command */
    int cmd, result;
    uint32_t deadline;
    char rx[220]; int rxlen;
    /* status (published under ext_lock) */
    hal_lte st;
    int creg, cereg;
    /* inbox */
    hal_sms sms[MAX_SMS]; int nsms;
    hal_sms tmp[MAX_SMS]; int ntmp; int in_text;
    int cmgl_mode1_bad;
    /* outgoing */
    int send_pending; char send_to[24]; char send_text[161]; int sent_ok, sent_fail;
    char raw[96]; int raw_pending;
    int inbox_dirty;
} lte_state;
static lte_state *L;

/* ---------------------------------------------------------------- pins / UART */
static void key(int press)
{
#if EXT_LTE_PWRKEY_ACTIVE_HIGH
    gpio_set_level(EXT_PIN_LTE_KEY, press ? 1 : 0);
#else
    gpio_set_level(EXT_PIN_LTE_KEY, press ? 0 : 1);      /* open-drain: 1 = released (high-Z) */
#endif
}
static int uart_on(void)
{
    if (L->installed) return 1;
    uart_config_t c = { .baud_rate = 115200, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                        .source_clk = UART_SCLK_DEFAULT };
    if (uart_driver_install(LTE_UART, LTE_RXBUF, 0, 0, NULL, 0) != ESP_OK) return 0;
    uart_param_config(LTE_UART, &c);
    uart_set_pin(LTE_UART, EXT_PIN_LTE_TX, EXT_PIN_LTE_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_flush_input(LTE_UART);
    L->installed = 1;
    return 1;
}
static void uart_off(void)
{
    if (!L->installed) return;
    uart_driver_delete(LTE_UART);
    L->installed = 0;
}

static void go(int s) { L->state = s; L->t_state = ext_ms(); L->step = 0; L->tries = 0; }

static void at(const char *s, int kind, uint32_t tmo)
{
    uart_write_bytes(LTE_UART, s, strlen(s));
    uart_write_bytes(LTE_UART, "\r", 1);
    L->cmd = kind; L->result = R_NONE;
    L->deadline = ext_ms() + tmo;
}

/* ---------------------------------------------------------------- parsing */
static int csq_bars(int dbm)
{
    if (!dbm) return 0;
    return dbm >= -73 ? 4 : dbm >= -83 ? 3 : dbm >= -93 ? 2 : dbm >= -105 ? 1 : 0;
}
static void reg_publish(void)
{
    int best = L->cereg == 1 || L->cereg == 5 ? L->cereg : L->creg == 1 || L->creg == 5 ? L->creg :
               L->cereg == 2 || L->creg == 2 ? 2 : 0;
    L->st.registered = best == 1 ? 1 : best == 5 ? 2 : best == 2 ? 3 : 0;
}
/* quoted field n (0-based) of s into out */
static int quoted(const char *s, int n, char *out, int max)
{
    for (int i = 0; i <= n; i++) {
        s = strchr(s, '"'); if (!s) return 0; s++;
        const char *e = strchr(s, '"'); if (!e) return 0;
        if (i == n) {
            int len = (int)(e - s) < max - 1 ? (int)(e - s) : max - 1;
            memcpy(out, s, (size_t)len); out[len] = 0;
            return 1;
        }
        s = e + 1;
    }
    return 0;
}
/* Text-mode bodies of UCS2 messages arrive as hex; decode the ASCII subset. */
static void sms_text(char *dst, const char *src, int max)
{
    int n = (int)strlen(src), hex = n >= 8 && n % 4 == 0;
    for (int i = 0; hex && i < n; i++) if (!isxdigit((unsigned char)src[i])) hex = 0;
    if (hex && !(src[0] == '0' && src[1] == '0')) hex = 0;
    int o = 0;
    if (hex) {
        for (int i = 0; i + 3 < n && o < max - 1; i += 4) {
            unsigned u = 0;
            sscanf(src + i, "%4x", &u);
            dst[o++] = u >= 0x20 && u < 0x7f ? (char)u : u == 0x0a ? ' ' : '?';
        }
    } else {
        for (int i = 0; i < n && o < max - 1; i++) dst[o++] = src[i];
    }
    dst[o] = 0;
}

static void line(char *s)
{
    if (!*s) return;
    if (L->cmd == C_RAW) printf("EXT lte< %s\n", s);
    /* final results */
    if (!strcmp(s, "OK")) { L->result = R_OK; L->in_text = 0; return; }
    if (!strcmp(s, "ERROR") || !strncmp(s, "+CME ERROR", 10) || !strncmp(s, "+CMS ERROR", 10)) {
        if (L->cmd == C_CPIN) { portENTER_CRITICAL(&ext_lock); L->st.sim = 0; portEXIT_CRITICAL(&ext_lock); }
        L->result = R_ERR; L->in_text = 0; return;
    }
    /* SMS body line following +CMGL */
    if (L->in_text && (L->cmd == C_CMGL || L->cmd == C_CMGL0) && strncmp(s, "+CMGL:", 6)) {
        hal_sms *m = &L->tmp[L->ntmp - 1];
        size_t have = strlen(m->text);
        if (have) { if (have < sizeof m->text - 2) { m->text[have++] = ' '; m->text[have] = 0; } }
        sms_text(m->text + have, s, (int)(sizeof m->text - have));
        return;
    }
    portENTER_CRITICAL(&ext_lock);
    if (!strncmp(s, "+CSQ:", 5)) {
        int r = 99; sscanf(s + 5, "%d", &r);
        L->st.rssi_dbm = r >= 0 && r <= 31 ? -113 + 2 * r : 0;
        L->st.bars = csq_bars(L->st.rssi_dbm);
    } else if (!strncmp(s, "+CREG:", 6) || !strncmp(s, "+CEREG:", 7)) {
        const char *p = strchr(s, ':') + 1;
        int a = -1, b = -1, n = sscanf(p, "%d,%d", &a, &b);
        int stat = n >= 2 && L->cmd != C_NONE ? b : a;      /* query: n,stat ; URC: stat */
        if (s[2] == 'E') L->cereg = stat; else L->creg = stat;
        reg_publish();
    } else if (!strncmp(s, "+COPS:", 6)) {
        int act = -1;
        if (quoted(s, 0, L->st.op, sizeof L->st.op)) {
            const char *e = strrchr(s, ',');
            if (e) act = atoi(e + 1);
        } else { L->st.op[0] = 0; }
        const char *t = act == 7 ? "LTE" : act == 0 ? "GSM" : act == 2 ? "3G" : act == 8 ? "CAT-M" : act == 9 ? "NB" : "";
        snprintf(L->st.tech, sizeof L->st.tech, "%s", t);
    } else if (!strncmp(s, "+CPIN:", 6)) {
        L->st.sim = strstr(s, "READY") ? 1 : strstr(s, "PIN") || strstr(s, "PUK") ? 2 : 0;
    } else if (!strncmp(s, "+CMTI:", 6)) {
        L->inbox_dirty = 1;
    } else if (!strncmp(s, "+CMGS:", 6)) {
        L->sent_ok++;
    } else if (!strcmp(s, "POWERED DOWN")) {
        L->st.powered = 0;
    }
    portEXIT_CRITICAL(&ext_lock);
    if (!strncmp(s, "+CMGL:", 6) && (L->cmd == C_CMGL || L->cmd == C_CMGL0)) {
        if (L->ntmp >= MAX_SMS) { L->in_text = 0; return; }
        hal_sms *m = &L->tmp[L->ntmp++];
        memset(m, 0, sizeof *m);
        char stat[16] = "";
        quoted(s, 0, stat, sizeof stat);
        quoted(s, 1, m->from, sizeof m->from);
        /* scts is the last quoted field: "yy/MM/dd,hh:mm:ss+zz" */
        char ts[24] = "";
        for (int q = 4; q >= 2; q--) if (quoted(s, q, ts, sizeof ts) && strchr(ts, '/')) break;
        snprintf(m->when, sizeof m->when, "%.17s", strchr(ts, '/') ? ts : "");
        m->unread = !strcmp(stat, "REC UNREAD");
        L->in_text = 1;
    }
}

static void pump(void)
{
    uint8_t buf[96];
    int n;
    while (L->installed && (n = uart_read_bytes(LTE_UART, buf, sizeof buf, 0)) > 0) {
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n') { L->rx[L->rxlen] = 0; line(L->rx); L->rxlen = 0; continue; }
            if (c == '\r') continue;
            if (L->rxlen < (int)sizeof L->rx - 1) L->rx[L->rxlen++] = c;
            if (L->rxlen == 2 && L->rx[0] == '>' && L->rx[1] == ' ' && L->cmd == C_CMGS_HDR) {
                L->result = R_PROMPT; L->rxlen = 0;
            }
        }
    }
    if (L->cmd != C_NONE && L->result == R_NONE && ext_due(ext_ms(), L->deadline)) L->result = R_TMO;
}

static int newer(const hal_sms *a, const hal_sms *b) { return strcmp(a->when, b->when) > 0; }
static void inbox_publish(void)
{
    for (int i = 1; i < L->ntmp; i++) {               /* newest first */
        hal_sms v = L->tmp[i]; int j = i - 1;
        while (j >= 0 && newer(&v, &L->tmp[j])) { L->tmp[j + 1] = L->tmp[j]; j--; }
        L->tmp[j + 1] = v;
    }
    int unread = 0;
    for (int i = 0; i < L->ntmp; i++) unread += L->tmp[i].unread;
    portENTER_CRITICAL(&ext_lock);
    memcpy(L->sms, L->tmp, sizeof(hal_sms) * (size_t)L->ntmp);
    L->nsms = L->ntmp;
    L->st.unread_sms = unread;
    portEXIT_CRITICAL(&ext_lock);
}

/* ---------------------------------------------------------------- service */
static int want_on(uint32_t now)
{
    if (L->enabled && ext_idle(now, L->last_get) && !L->send_pending) L->enabled = 0;
    return L->enabled || L->probing || L->send_pending;
}

uint32_t lte_service(uint32_t now)
{
    if (!L) return 1000;
    pump();
    int res = L->result;
    if (L->cmd != C_NONE && res == R_NONE) return 20;        /* waiting for a reply */
    int done = L->cmd;
    L->cmd = C_NONE; L->result = R_NONE;

    switch (L->state) {
    case L_OFF:
    case L_ABSENT:
        if (!want_on(now)) { uart_off(); return 1000; }
        if (L->state == L_ABSENT && !L->enabled && !L->send_pending) return 1000;
        if (!uart_on()) return 1000;
        go(L_CHECK);
        return 10;
    case L_CHECK:                         /* already powered? */
        if (done && res == R_OK) { go(L_INIT); return 10; }
        if (L->tries++ >= 3) { key(1); go(L_KEY); return 550; }
        at("AT", C_GENERIC, 300);
        return 20;
    case L_KEY:                           /* PWRKEY held 550 ms (power-on pulse) */
        if ((uint32_t)(now - L->t_state) < 550) return 20;
        key(0);
        go(L_BOOT);
        return 1000;
    case L_BOOT:                          /* wait up to 15 s for the modem to answer */
        if (done && res == R_OK) { go(L_INIT); return 10; }
        if ((uint32_t)(now - L->t_state) > 15000) {
            uart_off();
            if (!(ext_present_bits & EXT_LTE)) { L->probing = 0; L->enabled = 0; L->send_pending = 0; go(L_ABSENT); }
            else go(L_OFF);
            return 1000;
        }
        at("AT", C_GENERIC, 800);
        return 200;
    case L_INIT:
        ext_present_bits |= EXT_LTE;
        portENTER_CRITICAL(&ext_lock);
        L->st.powered = 1;
        portEXIT_CRITICAL(&ext_lock);
        if (L->step < N_INIT) {
            const char *c = INIT_CMDS[L->step++];
            at(c, !strcmp(c, "AT+CPIN?") ? C_CPIN : C_GENERIC, 3000);
            return 20;
        }
        go(L_READY);
        L->next_poll = now; L->next_inbox = now + 2000; L->next_cpin = now + 30000;
        return 10;
    case L_READY:
        if (done == C_CMGL0 || done == C_CMGL) {
            if (res == R_OK) inbox_publish();
            else if (done == C_CMGL && L->st.sim == 1) L->cmgl_mode1_bad = 1;   /* firmware without mode arg */
            L->in_text = 0;
            if (L->probing) { L->probing = 0; }
        }
        if (done == C_CMGS_HDR) {
            if (res == R_PROMPT) {
                uart_write_bytes(LTE_UART, L->send_text, strlen(L->send_text));
                uart_write_bytes(LTE_UART, "\x1a", 1);
                L->cmd = C_CMGS_BODY; L->result = R_NONE; L->deadline = now + 60000;
                return 50;
            }
            uart_write_bytes(LTE_UART, "\x1b", 1);            /* abort the prompt */
            L->sent_fail++; L->send_pending = 0;
        }
        if (done == C_CMGS_BODY) {
            if (res != R_OK) L->sent_fail++;
            L->send_pending = 0;
        }
        if (!want_on(now)) {
            at("AT+QPOWD=1", C_QPOWD, 2000);
            go(L_PWROFF);
            return 50;
        }
        if (L->raw_pending) { L->raw_pending = 0; at(L->raw, C_RAW, 5000); return 20; }
        if (L->send_pending && L->st.sim != 1 && (uint32_t)(now - L->t_state) > 20000) {
            L->send_pending = 0; L->sent_fail++;              /* no usable SIM: give up */
        }
        if (L->send_pending && L->st.sim == 1) {
            char c[48];
            snprintf(c, sizeof c, "AT+CMGS=\"%s\"", L->send_to);
            at(c, C_CMGS_HDR, 5000);
            return 20;
        }
        if (L->inbox_dirty || ext_due(now, L->next_inbox)) {
            L->inbox_dirty = 0; L->next_inbox = now + 60000; L->ntmp = 0;
            if (L->cmgl_mode1_bad) at("AT+CMGL=\"ALL\"", C_CMGL0, 8000);
            else at("AT+CMGL=\"ALL\",1", C_CMGL, 8000);
            return 20;
        }
        if (L->st.sim != 1 && ext_due(now, L->next_cpin)) {
            L->next_cpin = now + 30000;
            at("AT+CPIN?", C_CPIN, 3000);
            return 20;
        }
        if (ext_due(now, L->next_poll)) {
            L->next_poll = now + 1250;
            at(POLL_CMDS[L->poll_i++ & 3], C_GENERIC, 3000);
            return 20;
        }
        return 100;
    case L_PWROFF:                        /* wait for POWERED DOWN */
        if (!L->st.powered) { uart_off(); go(L_OFF); return 1000; }
        if ((uint32_t)(now - L->t_state) > 8000) { key(1); go(L_KEYOFF); return 1000; }
        return 100;
    case L_KEYOFF:                        /* PWRKEY held 1 s (power-off pulse) */
        if ((uint32_t)(now - L->t_state) < 1000) return 20;
        key(0);
        portENTER_CRITICAL(&ext_lock);
        L->st.powered = 0;
        portEXIT_CRITICAL(&ext_lock);
        uart_off();
        go(L_OFF);
        return 1000;
    }
    return 100;
}

void lte_init(void)
{
    L = ext_psram_calloc(sizeof *L);
    if (!L) return;
    gpio_config_t k = { .pin_bit_mask = 1ULL << EXT_PIN_LTE_KEY,
                        .mode = EXT_LTE_PWRKEY_ACTIVE_HIGH ? GPIO_MODE_OUTPUT : GPIO_MODE_OUTPUT_OD };
    key(0);
    gpio_config(&k);
    key(0);
    L->last_get = ext_ms() - NOS_EXT_IDLE_MS - 1;
    L->probing = 1;                       /* boot probe: power once, read SIM + inbox */
    go(L_OFF);
}

void hal_lte_enable(int on)
{
    if (!L) return;
    L->last_get = ext_ms();
    L->enabled = on ? 1 : 0;
}

int hal_lte_get(hal_lte *out)
{
    if (!L || !(ext_present_bits & EXT_LTE)) { if (out) memset(out, 0, sizeof *out); return 0; }
    L->last_get = ext_ms();
    if (out) {
        portENTER_CRITICAL(&ext_lock);
        *out = L->st;
        portEXIT_CRITICAL(&ext_lock);
        if (!out->powered) { out->registered = 0; out->rssi_dbm = 0; out->bars = 0; }
    }
    return 1;
}

int hal_lte_sms_list(hal_sms *out, int max)
{
    if (!L || !(ext_present_bits & EXT_LTE) || !out || max <= 0) return 0;
    L->last_get = ext_ms();
    portENTER_CRITICAL(&ext_lock);
    int n = L->nsms < max ? L->nsms : max;
    memcpy(out, L->sms, sizeof(hal_sms) * (size_t)n);
    portEXIT_CRITICAL(&ext_lock);
    return n;
}

int hal_lte_sms_send(const char *to, const char *text)
{
    if (!L || !(ext_present_bits & EXT_LTE) || !to || !text || L->send_pending) return 0;
    int n = 0;
    for (const char *p = to; *p; p++) {
        if (!(isdigit((unsigned char)*p) || (*p == '+' && p == to))) return 0;
        n++;
    }
    if (n < 3 || n >= (int)sizeof L->send_to) return 0;
    snprintf(L->send_to, sizeof L->send_to, "%s", to);
    int o = 0;
    for (const char *p = text; *p && o < 160; p++)
        L->send_text[o++] = (*p == 0x1a || *p == 0x1b) ? ' ' : *p;   /* Ctrl-Z/ESC would end/abort */
    L->send_text[o] = 0;
    L->last_get = ext_ms();
    L->send_pending = 1;                  /* powers the modem if needed; stays on until sent */
    return 1;
}

void lte_debug_cmd(const char *args)
{
    if (!L) return;
    if (!strncmp(args, "at ", 3)) {
        snprintf(L->raw, sizeof L->raw, "%s", args + 3);
        L->raw_pending = 1;
        hal_lte_enable(1);
        printf("EXT lte raw queued (modem must be READY)\n");
    } else if (!strncmp(args, "sms ", 4)) {
        char to[24] = ""; int off = 0;
        if (sscanf(args + 4, "%23s %n", to, &off) >= 1)
            printf("EXT lte sms queued=%d\n", hal_lte_sms_send(to, args + 4 + off));
    }
}

void lte_debug(void)
{
    if (!L) return;
    static const char *const S[] = { "off", "check", "key", "boot", "init", "ready", "pwroff", "keyoff", "absent" };
    hal_lte s;
    portENTER_CRITICAL(&ext_lock);
    s = L->st;
    portEXIT_CRITICAL(&ext_lock);
    printf("EXT lte state=%s en=%d probing=%d powered=%d sim=%d reg=%d rssi=%d bars=%d op=\"%s\" tech=%s"
           " unread=%d inbox=%d sent=%d/%d key_active_high=%d\n",
           S[L->state], L->enabled, L->probing, s.powered, s.sim, s.registered, s.rssi_dbm, s.bars, s.op,
           s.tech, s.unread_sms, L->nsms, L->sent_ok, L->sent_fail, EXT_LTE_PWRKEY_ACTIVE_HIGH);
}
