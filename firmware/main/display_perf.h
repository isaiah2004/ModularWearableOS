#ifndef DISPLAY_PERF_H
#define DISPLAY_PERF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_PERF_MISSING_U32 UINT32_MAX
#define DISPLAY_PERF_TRACE_NAME  24
#define DISPLAY_PERF_TRACE_RING  4

typedef enum {
    DISPLAY_PERF_COPY = 0,
    DISPLAY_PERF_SWAP,
    DISPLAY_PERF_ROTATE,
    DISPLAY_PERF_QUEUE_WAIT,
    DISPLAY_PERF_DMA_WAIT,
    DISPLAY_PERF_STAGE_COUNT
} display_perf_stage;

typedef struct {
    uint64_t samples;
    uint64_t total_us;
    uint32_t p50_us;
    uint32_t p95_us;
    uint32_t p99_us;
    uint32_t max_us;
    bool valid;
} display_perf_dist;

typedef struct {
    char trace_name[DISPLAY_PERF_TRACE_NAME]; /* empty for a lifetime snapshot */
    uint32_t trace_seconds;
    uint64_t elapsed_us;
    uint64_t frames;          /* frame evaluations, including frames with no pixels */
    uint64_t render_frames;   /* frames with a measured record or raster stage */
    uint64_t present_frames;  /* frames with dirty rectangles */
    uint64_t deadline_misses; /* measured wall time greater than 16,667 us */
    uint64_t dirty_pixels;
    uint64_t dirty_rects;
    uint64_t atomic_frames;
    uint64_t submitted_transactions;
    uint64_t submitted_bytes;
    uint64_t completed_transactions;
    uint64_t completed_bytes;
    uint64_t cancelled_transactions;
    uint64_t cancelled_bytes;
    uint64_t unmatched_completions;
    uint64_t submit_overlaps;
    uint32_t queue_highwater;
    bool queue_depth_valid;
    display_perf_dist wall;
    display_perf_dist record;
    display_perf_dist raster;
    display_perf_dist present;
    display_perf_dist submit_to_complete;
    display_perf_dist completion_cadence;
    display_perf_dist stage[DISPLAY_PERF_STAGE_COUNT];
} display_perf_snapshot;

typedef struct {
    char name[DISPLAY_PERF_TRACE_NAME];
    uint32_t requested_seconds;
    uint64_t started_us;
    uint64_t ended_us;
    display_perf_snapshot perf;
} display_perf_trace;

void display_perf_init(void);
void display_perf_reset(void);

/* Call immediately before handing one transaction to the LCD driver. The
 * current pipeline permits one outstanding DMA transaction. */
void display_perf_submit(uint32_t bytes, uint64_t now_us);
/* Undo a submit when the LCD driver rejects it synchronously. */
void display_perf_cancel_submit(void);

/* ISR-safe. bytes==0 uses the byte count recorded by display_perf_submit(). */
void display_perf_complete_isr(uint32_t bytes, uint64_t now_us);

/* Task-context stage and frame samples. Pass DISPLAY_PERF_MISSING_U32 for a
 * timing that was not measured; missing measurements remain invalid/null. */
void display_perf_add_stage(display_perf_stage stage, uint32_t us);
void display_perf_queue_depth(uint32_t depth);
/* Wall time above this counts as a deadline miss (the main loop frame period). */
void display_perf_set_frame_deadline(uint32_t us);
void display_perf_frame(uint32_t dirty_pixels, uint16_t rects, bool atomic,
                        uint32_t record_us, uint32_t raster_us,
                        uint32_t present_us, uint32_t wall_us);

void display_perf_snapshot_get(display_perf_snapshot *out);
/* Emits one bounded JSONL record. Intended for command/tick paths, never a
 * measured render or ISR region. prefix should be a short event name. */
void display_perf_print(const char *prefix, const display_perf_snapshot *snapshot);

/* Starts/replaces the active trace. display_perf_tick() finalizes it after the
 * requested wall-clock duration, even if no display frames were rendered. */
void display_perf_trace_start(const char *name, uint32_t seconds, uint64_t now_us);
bool display_perf_tick(uint64_t now_us, display_perf_trace *completed);
bool display_perf_trace_status(char *name, size_t name_size,
                               uint32_t *remaining_ms, uint64_t now_us);
size_t display_perf_trace_count(void);
bool display_perf_trace_get(size_t newest_index, display_perf_trace *out);

#ifdef __cplusplus
}
#endif
#endif
