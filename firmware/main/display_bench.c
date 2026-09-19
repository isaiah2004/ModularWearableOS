#include "display_bench.h"

#include <stdbool.h>
#include <string.h>
#include "display_perf.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define BENCH_MAX_DIMENSION 466U
#define BENCH_MAX_FRAMES 2000U
#define BENCH_DMA_TIMEOUT_MS 5000U
#define QSPI_PARAM_CMD(reg) ((0x02ULL << 24) | ((uint64_t)(reg) << 8))
#define QSPI_COLOR_CMD(reg) ((0x32ULL << 24) | ((uint64_t)(reg) << 8))

static uint32_t elapsed_u32(uint64_t start, uint64_t end)
{
    uint64_t elapsed = end - start;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static void fill_sram_pattern(uint16_t *buf, size_t pixels)
{
    static const uint16_t colors[] = {0x00f8, 0xe007, 0x1f00, 0xffff};
    for (size_t i = 0; i < pixels; i++) buf[i] = colors[(i / 32U) & 3U];
}

static void generate_pattern(uint16_t *buf, uint16_t width, uint16_t y0,
                             uint16_t rows, uint32_t frame)
{
    for (uint16_t y = 0; y < rows; y++) {
        for (uint16_t x = 0; x < width; x++) {
            uint16_t r = (uint16_t)((x + frame) & 0x1fU);
            uint16_t g = (uint16_t)((y0 + y + frame) & 0x3fU);
            uint16_t b = (uint16_t)((x + y0 + y + frame) & 0x1fU);
            uint16_t rgb565 = (uint16_t)((r << 11) | (g << 5) | b);
            buf[(size_t)y * width + x] = (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
        }
    }
}

static esp_err_t set_window(esp_lcd_panel_io_handle_t io, uint16_t x0,
                            uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t column[] = {(uint8_t)(x0 >> 8), (uint8_t)x0,
                        (uint8_t)(x1 >> 8), (uint8_t)x1};
    uint8_t row[] = {(uint8_t)(y0 >> 8), (uint8_t)y0,
                     (uint8_t)(y1 >> 8), (uint8_t)y1};
    esp_err_t err = esp_lcd_panel_io_tx_param(io, QSPI_PARAM_CMD(0x2a),
                                               column, sizeof(column));
    if (err != ESP_OK) return err;
    return esp_lcd_panel_io_tx_param(io, QSPI_PARAM_CMD(0x2b), row, sizeof(row));
}

display_bench_result display_bench_run(esp_lcd_panel_io_handle_t io,
                                       esp_lcd_panel_handle_t panel,
                                       uint16_t *dma_buf,
                                       size_t dma_capacity_bytes,
                                       const uint16_t *backbuf_psram,
                                       uint16_t width, uint16_t height,
                                       uint16_t gap_x, uint32_t frames,
                                       display_bench_source source,
                                       SemaphoreHandle_t completion_sem)
{
    display_bench_result result = {.err = ESP_ERR_INVALID_ARG};
    (void)panel; /* Kept in the API so the caller documents the owned panel. */
    if (!io || !panel || !dma_buf || !completion_sem || frames == 0 ||
        frames > BENCH_MAX_FRAMES || width < 2 || height < 2 ||
        width > BENCH_MAX_DIMENSION || height > BENCH_MAX_DIMENSION ||
        (width & 1U) || (height & 1U) || gap_x > UINT16_MAX - width ||
        source > DISPLAY_BENCH_SOURCE_PSRAM ||
        (source == DISPLAY_BENCH_SOURCE_PSRAM && !backbuf_psram)) {
        return result;
    }

    size_t row_bytes = (size_t)width * sizeof(*dma_buf);
    size_t chunk_rows_size = dma_capacity_bytes / row_bytes;
    if (chunk_rows_size > height) chunk_rows_size = height;
    chunk_rows_size &= ~(size_t)1; /* Every transfer, including the tail, is even. */
    if (chunk_rows_size < 2) return result;
    uint16_t chunk_rows = (uint16_t)chunk_rows_size;

    /* SRAM source is prepared before the measured region and reused unchanged. */
    if (source == DISPLAY_BENCH_SOURCE_SRAM)
        fill_sram_pattern(dma_buf, (size_t)chunk_rows * width);

    bool pending = false;
    uint64_t run_start = (uint64_t)esp_timer_get_time();
    result.err = ESP_OK;
    for (uint32_t frame = 0; frame < frames; frame++) {
        uint64_t frame_start = (uint64_t)esp_timer_get_time();
        uint16_t rects = 0;
        for (uint16_t y = 0; y < height; ) {
            uint16_t rows = (uint16_t)(height - y);
            if (rows > chunk_rows) rows = chunk_rows;
            if (xSemaphoreTake(completion_sem, pdMS_TO_TICKS(BENCH_DMA_TIMEOUT_MS)) != pdTRUE) {
                result.err = ESP_ERR_TIMEOUT;
                goto done; /* Pending DMA still owns dma_buf; do not give the semaphore. */
            }
            if (pending) {
                result.completion_count++;
                pending = false;
            }

            size_t pixels = (size_t)rows * width;
            size_t bytes = pixels * sizeof(*dma_buf);
            if (source == DISPLAY_BENCH_SOURCE_GENERATOR)
                generate_pattern(dma_buf, width, y, rows, frame);
            else if (source == DISPLAY_BENCH_SOURCE_PSRAM)
                memcpy(dma_buf, backbuf_psram + (size_t)y * width, bytes);

            uint16_t native_x0 = gap_x;
            uint16_t native_x1 = (uint16_t)(gap_x + width - 1U);
            esp_err_t err = set_window(io, native_x0, y, native_x1,
                                       (uint16_t)(y + rows - 1U));
            if (err != ESP_OK) {
                xSemaphoreGive(completion_sem); /* No color completion is expected. */
                result.err = err;
                goto done;
            }
            display_perf_submit((uint32_t)bytes, (uint64_t)esp_timer_get_time());
            err = esp_lcd_panel_io_tx_color(io, QSPI_COLOR_CMD(0x2c), dma_buf, bytes);
            if (err != ESP_OK) {
                display_perf_cancel_submit();
                xSemaphoreGive(completion_sem); /* Submission failed synchronously. */
                result.err = err;
                goto done;
            }
            pending = true;
            result.submit_count++;
            rects++;
            y = (uint16_t)(y + rows);
        }

        if (xSemaphoreTake(completion_sem, pdMS_TO_TICKS(BENCH_DMA_TIMEOUT_MS)) != pdTRUE) {
            result.err = ESP_ERR_TIMEOUT;
            goto done;
        }
        pending = false;
        result.completion_count++;
        xSemaphoreGive(completion_sem);
        uint64_t frame_end = (uint64_t)esp_timer_get_time();
        uint32_t frame_us = elapsed_u32(frame_start, frame_end);
        display_perf_frame((uint32_t)width * height, rects, true,
                           DISPLAY_PERF_MISSING_U32, DISPLAY_PERF_MISSING_U32,
                           frame_us, frame_us);
        result.frames_completed++;
    }

done:
    result.elapsed_us = (uint64_t)esp_timer_get_time() - run_start;
    return result;
}
