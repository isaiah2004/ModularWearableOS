#ifndef DISPLAY_BENCH_H
#define DISPLAY_BENCH_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/semphr.h"

typedef enum {
    DISPLAY_BENCH_SOURCE_SRAM = 0,
    DISPLAY_BENCH_SOURCE_GENERATOR,
    DISPLAY_BENCH_SOURCE_PSRAM,
} display_bench_source;

typedef struct {
    uint64_t elapsed_us;
    uint32_t frames_completed;
    esp_err_t err;
    uint32_t submit_count;
    uint32_t completion_count;
} display_bench_result;

/* Must run on the display's serialized worker while normal UI flushing is
 * suspended. completion_sem is given by the panel color-transfer callback. */
display_bench_result display_bench_run(esp_lcd_panel_io_handle_t io,
                                       esp_lcd_panel_handle_t panel,
                                       uint16_t *dma_buf,
                                       size_t dma_capacity_bytes,
                                       const uint16_t *backbuf_psram,
                                       uint16_t width, uint16_t height,
                                       uint16_t gap_x, uint32_t frames,
                                       display_bench_source source,
                                       SemaphoreHandle_t completion_sem);

#endif
