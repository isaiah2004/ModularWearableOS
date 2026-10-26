/* ble_xfer.c — the SD file-transfer commands (sd_xfer.c) over Bluetooth LE (tools/sdsync.py --ble).
 *
 * A service of its own beside Gadgetbridge's NUS, so file traffic never mixes with phone lines:
 *   CTRL (write, notify): a command line written in (sdls / sdcrc / sdput / sdget / sdrm, same as
 *        USB); reply lines notified back, '\n'-terminated, cut to the link's MTU.
 *   DATA (write without response): the raw bytes of an sdput, in blocks the host sends only after
 *        the previous block's SDK (4 KB blocks; the stream buffer holds two).
 * Commands run in the serial command task (ble_xfer_poll), like the USB ones: no task of our own
 * (internal RAM is short). The watch takes one BLE connection, so the PC can connect only while
 * the phone isn't. A transfer asks for a fast connection interval (the watch normally asks for a
 * gentle 80-120 ms one to save power and leave airtime for headphones) and gives it back after. */
#include "sd_xfer.h"
#include "psram.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "esp_timer.h"
#include "nos_power.h"

#define SB_BYTES 8192
#define CMD_MAX 160

/* 7f5e0001-5a1d-4c3b-9e0f-6e4f53584652 ("NOS XFR") */
const ble_uuid128_t BLE_XFER_SVC = BLE_UUID128_INIT(0x52, 0x46, 0x58, 0x53, 0x4f, 0x6e, 0x0f, 0x9e,
                                                    0x3b, 0x4c, 0x1d, 0x5a, 0x01, 0x00, 0x5e, 0x7f);
static const ble_uuid128_t CTRL_CHR = BLE_UUID128_INIT(0x52, 0x46, 0x58, 0x53, 0x4f, 0x6e, 0x0f, 0x9e,
                                                       0x3b, 0x4c, 0x1d, 0x5a, 0x02, 0x00, 0x5e, 0x7f);
static const ble_uuid128_t DATA_CHR = BLE_UUID128_INIT(0x52, 0x46, 0x58, 0x53, 0x4f, 0x6e, 0x0f, 0x9e,
                                                       0x3b, 0x4c, 0x1d, 0x5a, 0x03, 0x00, 0x5e, 0x7f);
static uint16_t s_ctrl_h;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static QueueHandle_t s_cmd_q;
static StreamBufferHandle_t s_sb;
static StaticStreamBuffer_t s_sb_ctl;
static volatile TaskHandle_t s_waiter;      /* serial command task: sleeps until a command is queued */
void ble_xfer_set_waiter(void *task) { s_waiter = (TaskHandle_t)task; }

static int ensure(void)
{
    if (!s_cmd_q) s_cmd_q = xQueueCreate(2, CMD_MAX);
    if (!s_sb) {
        uint8_t *mem = nos_mem_alloc(SB_BYTES + 1, "ble_xfer");
        if (mem) s_sb = xStreamBufferCreateStatic(SB_BYTES, 1, mem, &s_sb_ctl);
    }
    return s_cmd_q && s_sb;
}

static int ctrl_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    char line[CMD_MAX] = { 0 };
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, line, CMD_MAX - 1, &n) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    line[n] = 0;
    if (!n || !ensure()) return 0;
    s_conn = conn;
    xQueueSend(s_cmd_q, line, 0);
    TaskHandle_t waiter = s_waiter;
    if (waiter) xTaskNotifyGive(waiter);        /* the command task may be in a long light-sleep wait */
    return 0;
}
static int data_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || !ensure()) return 0;
    uint8_t b[520];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, b, sizeof b, &n) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    xStreamBufferSend(s_sb, b, n, 0);           /* never blocks the host task; the host's block acks bound it */
    return 0;
}
const struct ble_gatt_chr_def BLE_XFER_CHRS[] = {
    { .uuid = &CTRL_CHR.u, .access_cb = ctrl_access, .val_handle = &s_ctrl_h,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY },
    { .uuid = &DATA_CHR.u, .access_cb = data_access,
      .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE },
    { 0 },
};

static int ble_read(uint8_t *b, int n, int timeout_ms)
{
    return (int)xStreamBufferReceive(s_sb, b, (size_t)n, pdMS_TO_TICKS(timeout_ms));
}
static void ble_line(const char *s)
{
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    int mtu = ble_att_mtu(conn), chunk = mtu > 23 ? mtu - 3 : 20, len = (int)strlen(s), total = len + 1;
    static uint8_t buf[256];                    /* only the serial command task writes lines: off its stack */
    if (chunk > (int)sizeof buf) chunk = (int)sizeof buf;
    for (int off = 0; off < total; off += chunk) {
        int k = total - off < chunk ? total - off : chunk;
        for (int i = 0; i < k; i++) buf[i] = (uint8_t)(off + i < len ? s[off + i] : '\n');
        for (int tries = 0;; tries++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)k);
            int rc = om ? ble_gatts_notify_custom(conn, s_ctrl_h, om) : BLE_HS_ENOMEM;   /* consumes om */
            if (rc == 0) break;
            if (rc != BLE_HS_ENOMEM || tries >= 200) return;   /* link gone: the host times out */
            vTaskDelay(1);                       /* the host task is draining the mbuf pool */
        }
    }
}
static const xfer_io BLE_IO = { ble_read, ble_line, 4096 };

static void conn_speed(uint16_t conn, int fast)
{
    struct ble_gap_upd_params p = fast
        ? (struct ble_gap_upd_params){ .itvl_min = 6, .itvl_max = 12, .latency = 0, .supervision_timeout = 400 }
        : (struct ble_gap_upd_params){ .itvl_min = 64, .itvl_max = 96, .latency = 3, .supervision_timeout = 800 };
    ble_gap_update_params(conn, &p);
}

int ble_xfer_poll(void)
{
    static int64_t s_fast_until;
    if (!s_cmd_q) return 0;
    char line[CMD_MAX];
    if (xQueueReceive(s_cmd_q, line, 0) != pdTRUE) {
        if (s_fast_until && esp_timer_get_time() > s_fast_until) {   /* 5 s idle: back to gentle */
            s_fast_until = 0;
            nos_power_xfer_hold(0);              /* transfer over: light sleep may resume */
            if (s_conn != BLE_HS_CONN_HANDLE_NONE) conn_speed(s_conn, 0);
        }
        return 0;
    }
    nos_power_xfer_hold(1);                     /* no light sleep between blocks: SD/BLE latency-bound */
    if (!s_fast_until && s_conn != BLE_HS_CONN_HANDLE_NONE) conn_speed(s_conn, 1);
    xStreamBufferReset(s_sb);                   /* no stale bytes from an aborted upload */
    if (!sd_xfer_exec(line, &BLE_IO)) ble_line("SDXFER ERR unknown");
    s_fast_until = esp_timer_get_time() + 5000000;
    return 1;
}
void ble_xfer_disconnected(void) { s_conn = BLE_HS_CONN_HANDLE_NONE; }
