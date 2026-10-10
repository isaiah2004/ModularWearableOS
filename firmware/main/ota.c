/* ota.c — see ota.h. One writer at a time (s_busy); the image streams straight
 * into flash in small chunks, so no RAM-sized buffer is needed. esp_ota_end()
 * validates the image header and SHA-256 before the boot slot is switched. */
#include "ota.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OTA_SD_PATH  "/sdcard/update.bin"
#define OTA_SD_DONE  "/sdcard/update.done"
#define OTA_CHUNK    4096

static volatile int s_busy;

static void reboot_soon(void)
{
    printf("OTA: rebooting into the new image\n");
    vTaskDelay(pdMS_TO_TICKS(800));          /* let the HTTP reply / log drain */
    esp_restart();
}

void ota_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        printf("OTA: new image confirmed (%s)\n", run->label);
    }
}

void ota_print_status(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d;
    printf("OTA: running=%s next=%s", run ? run->label : "?", next ? next->label : "none");
    if (run && esp_ota_get_partition_description(run, &d) == ESP_OK)
        printf(" version=%s built=%s %s", d.version, d.date, d.time);
    printf("\n");
}

/* Streaming writer shared by the upload and SD paths. */
typedef struct { esp_ota_handle_t h; const esp_partition_t *part; size_t got; } ota_sink;

static esp_err_t sink_begin(ota_sink *s)
{
    s->part = esp_ota_get_next_update_partition(NULL);
    s->got = 0;
    if (!s->part) return ESP_ERR_NOT_FOUND;   /* single-slot table: no OTA */
    printf("OTA: writing %s @0x%lx\n", s->part->label, (unsigned long)s->part->address);
    return esp_ota_begin(s->part, OTA_WITH_SEQUENTIAL_WRITES, &s->h);
}

static esp_err_t sink_finish(ota_sink *s)
{
    esp_err_t e = esp_ota_end(s->h);          /* checks magic + SHA-256 */
    if (e == ESP_OK) e = esp_ota_set_boot_partition(s->part);
    printf("OTA: %u bytes, %s\n", (unsigned)s->got, e == ESP_OK ? "verified" : esp_err_to_name(e));
    return e;
}

/* ---- HTTP upload: POST /ota with the raw .bin as the body ---- */
static esp_err_t web_ota(httpd_req_t *req)
{
    if (s_busy) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "update already running");
    s_busy = 1;
    char *buf = malloc(OTA_CHUNK);
    ota_sink s;
    esp_err_t e = buf ? sink_begin(&s) : ESP_ERR_NO_MEM;
    int left = req->content_len;
    while (e == ESP_OK && left > 0) {
        int n = httpd_req_recv(req, buf, left < OTA_CHUNK ? left : OTA_CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { e = ESP_FAIL; break; }
        e = esp_ota_write(s.h, buf, n);
        s.got += n; left -= n;
        if ((s.got & 0x7FFFF) < (size_t)n) printf("OTA: %u/%d KB\n", (unsigned)(s.got >> 10), req->content_len >> 10);
    }
    free(buf);
    if (e == ESP_OK) e = sink_finish(&s);
    else if (s.part) esp_ota_abort(s.h);
    s_busy = 0;
    if (e != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(e));
    httpd_resp_sendstr(req, "ok, rebooting");
    reboot_soon();
    return ESP_OK;
}

/* ---- URL pull (also used by POST /ota_url with the URL as the body) ---- */
int ota_from_url(const char *url)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_busy = 1;
    printf("OTA: downloading %s\n", url);
    esp_http_client_config_t http = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000, .keep_alive_enable = true, .buffer_size = 4096,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_err_t e = esp_https_ota(&cfg);       /* follows redirects, verifies, sets boot slot */
    printf("OTA: %s\n", e == ESP_OK ? "verified" : esp_err_to_name(e));
    s_busy = 0;
    if (e == ESP_OK) reboot_soon();
    return e;
}

static void url_task(void *arg)
{
    ota_from_url((const char *)arg);
    free(arg);
    vTaskDelete(NULL);
}

static esp_err_t web_ota_url(httpd_req_t *req)
{
    int n = req->content_len;
    if (n <= 0 || n > 512) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body = firmware URL");
    char *url = malloc(n + 1);
    if (!url) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    int got = 0;
    while (got < n) {
        int r = httpd_req_recv(req, url + got, n - got);
        if (r <= 0) { free(url); return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "timeout"); }
        got += r;
    }
    url[n] = 0;
    /* download on its own task so the request returns now */
    if (xTaskCreate(url_task, "ota_url", 8192, url, 5, NULL) != pdPASS) {
        free(url);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no task");
    }
    return httpd_resp_sendstr(req, "downloading; the watch reboots when done");
}

/* ---- SD card: /sdcard/update.bin -> renamed to update.done once applied ---- */
int ota_from_sd(void)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;
    FILE *f = fopen(OTA_SD_PATH, "rb");
    if (!f) { printf("OTA: no %s\n", OTA_SD_PATH); return ESP_ERR_NOT_FOUND; }
    s_busy = 1;
    char *buf = malloc(OTA_CHUNK);
    ota_sink s;
    esp_err_t e = buf ? sink_begin(&s) : ESP_ERR_NO_MEM;
    size_t n;
    while (e == ESP_OK && (n = fread(buf, 1, OTA_CHUNK, f)) > 0) {
        e = esp_ota_write(s.h, buf, n);
        s.got += n;
    }
    fclose(f);
    free(buf);
    if (e == ESP_OK) e = sink_finish(&s);
    else if (s.part) esp_ota_abort(s.h);
    s_busy = 0;
    if (e != ESP_OK) return e;
    remove(OTA_SD_DONE);
    rename(OTA_SD_PATH, OTA_SD_DONE);         /* never re-apply the same file */
    reboot_soon();
    return ESP_OK;
}

/* ---- GET /update: phone-friendly upload page ---- */
static esp_err_t web_update_page(httpd_req_t *req)
{
    static const char *html =
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Watch update</title><style>body{font-family:system-ui,sans-serif;background:#0d0d10;"
        "color:#eee;max-width:440px;margin:2em auto;padding:0 1.2em}h2{color:#5cd}"
        "input,button{font-size:1em;padding:.65em;width:100%;box-sizing:border-box;margin:.35em 0;"
        "border-radius:10px;border:1px solid #333;background:#1a1a20;color:#eee}"
        "button{background:#5cd;color:#000;font-weight:700;border:0}progress{width:100%}</style>"
        "<h2>&#8986; Firmware update</h2>"
        "<p>Upload a firmware .bin</p><input type=file id=f accept='.bin'>"
        "<button onclick=up()>Upload &amp; install</button><progress id=p max=1 value=0></progress>"
        "<p>or install from a URL</p><input id=u placeholder='https://.../watchfw.bin'>"
        "<button onclick=url()>Download &amp; install</button><p id=s></p>"
        "<script>const s=t=>document.getElementById('s').textContent=t;"
        "function up(){const f=document.getElementById('f').files[0];if(!f)return s('pick a file');"
        "const x=new XMLHttpRequest();x.open('POST','/ota');"
        "x.upload.onprogress=e=>document.getElementById('p').value=e.loaded/e.total;"
        "x.onload=()=>s(x.status==200?'Installed - the watch is rebooting':'Failed: '+x.responseText);"
        "x.onerror=()=>s('Connection lost');s('Uploading...');x.send(f)}"
        "function url(){fetch('/ota_url',{method:'POST',body:document.getElementById('u').value})"
        ".then(r=>r.text()).then(s)}</script>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

void ota_register_web(httpd_handle_t h)
{
    httpd_uri_t pg = { .uri = "/update",  .method = HTTP_GET,  .handler = web_update_page };
    httpd_uri_t up = { .uri = "/ota",     .method = HTTP_POST, .handler = web_ota };
    httpd_uri_t ur = { .uri = "/ota_url", .method = HTTP_POST, .handler = web_ota_url };
    httpd_register_uri_handler(h, &pg);
    httpd_register_uri_handler(h, &up);
    httpd_register_uri_handler(h, &ur);
}
