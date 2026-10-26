#ifndef WATCH_ASSET_H
#define WATCH_ASSET_H
#include <stddef.h>
#include <stdint.h>
#define ASSET_PAGES 16
#define ASSET_PATH 256
#define ASSET_SLICE 32768u
enum { ASSET_OK, ASSET_FAILED, ASSET_CANCELLED };
typedef struct {
    char path[ASSET_PATH];
    void *pages[ASSET_PAGES];
    unsigned page_bytes, bytes, page_count, tag;
} asset_request;
typedef struct {
    int code;
    uint32_t open_us, read_us, read_max_us, read_calls, close_us;
} asset_result;
/* UI producer.1 transfers private pages;0 busy;-1 unavailable. No device sync I/O. */
int hal_asset_submit(const asset_request *request);
void hal_asset_cancel(void);
int hal_asset_take(asset_request *request, asset_result *result);
int hal_asset_busy(void);
void hal_asset_poll(void);
/* Explicit shutdown only, after panel off: waits for close, not adoption. */
void hal_asset_wait(void);
typedef struct {
    uint32_t state,bytes_owned,stack_min_free,create_failures,admission_skips;
    int worker;
} asset_diagnostics;
void hal_asset_get_diagnostics(asset_diagnostics *out);
#endif
