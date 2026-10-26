#include "display_perf.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define HIST_BUCKETS 64
static uint32_t s_frame_deadline_us = 16667u;   /* frame period; 20833 with the 48fps cap */

void display_perf_set_frame_deadline(uint32_t us) { s_frame_deadline_us = us; }

typedef struct {
    uint64_t n, total;
    uint32_t max;
    uint32_t bucket[HIST_BUCKETS];
} dist_acc;

typedef struct {
    uint64_t frames, render_frames, present_frames, deadline_misses;
    uint64_t dirty_pixels, dirty_rects, atomic_frames;
    uint64_t submitted_transactions, submitted_bytes;
    uint64_t completed_transactions, completed_bytes;
    uint64_t cancelled_transactions, cancelled_bytes;
    uint64_t unmatched_completions;
    uint64_t submit_overlaps;
    uint32_t queue_highwater;
    bool queue_depth_valid;
    dist_acc wall, record, raster, present, submit_to_complete, completion_cadence;
    dist_acc stage[DISPLAY_PERF_STAGE_COUNT];
} perf_acc;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static perf_acc s_all;
static perf_acc s_trace_acc;
static uint64_t s_pending_us;
static uint32_t s_pending_bytes;
static bool s_pending;
static uint32_t s_pending_trace_generation;
static uint64_t s_last_complete_us;
static bool s_have_last_complete;
static uint64_t s_trace_last_complete_us;
static bool s_trace_have_last_complete;
static bool s_trace_active;
static uint32_t s_trace_generation;
static char s_trace_name[DISPLAY_PERF_TRACE_NAME];
static uint32_t s_trace_seconds;
static uint64_t s_trace_started, s_trace_deadline;
static display_perf_trace s_ring[DISPLAY_PERF_TRACE_RING];
static uint8_t s_ring_head, s_ring_count;
/* Snapshot/tick are serialized on the UI task. Keeping this scratch static
 * avoids placing the histogram accumulator (~3 KiB) on its 8 KiB stack. */
static perf_acc s_export_scratch;

static unsigned bucket_for(uint32_t us)
{
    if (us <= 1) return 0;
    if (us <= 32000) return (us + 999u) / 1000u;       /* 1 ms through 32 ms */
    if (us <= 160000) return 32u + (us - 32000u + 7999u) / 8000u;
    unsigned b = 49;                                    /* 256 ms, then powers of two */
    uint32_t upper = 256000;
    while (us > upper && b < HIST_BUCKETS - 1) {
        if (upper > UINT32_MAX / 2u) return HIST_BUCKETS - 1;
        upper *= 2u; b++;
    }
    return b;
}

static uint32_t bucket_upper(unsigned b)
{
    if (b == 0) return 1;
    if (b <= 32) return b * 1000u;
    if (b <= 48) return 32000u + (b - 32u) * 8000u;
    uint64_t upper = (uint64_t)256000u << (b - 49u);
    return upper > UINT32_MAX ? UINT32_MAX : (uint32_t)upper;
}

static void dist_add(dist_acc *d, uint32_t us)
{
    if (d->n != UINT64_MAX) d->n++;
    if (UINT64_MAX - d->total < us) d->total = UINT64_MAX;
    else d->total += us;
    if (us > d->max) d->max = us;
    unsigned b = bucket_for(us);
    if (d->bucket[b] != UINT32_MAX) d->bucket[b]++;
}

static uint32_t percentile(const dist_acc *d, unsigned pct)
{
    uint64_t target = (d->n / 100u) * pct +
                      ((d->n % 100u) * pct + 99u) / 100u, seen = 0;
    for (unsigned i = 0; i < HIST_BUCKETS; i++) {
        seen += d->bucket[i];
        if (seen >= target) {
            return bucket_upper(i); /* conservative inclusive bucket upper bound */
        }
    }
    return d->max;
}

static void dist_export(const dist_acc *in, display_perf_dist *out)
{
    memset(out, 0, sizeof(*out));
    out->samples = in->n;
    out->total_us = in->total;
    out->valid = in->n != 0;
    if (!out->valid) {
        out->p50_us = out->p95_us = out->p99_us = out->max_us = DISPLAY_PERF_MISSING_U32;
        return;
    }
    out->p50_us = percentile(in, 50);
    out->p95_us = percentile(in, 95);
    out->p99_us = percentile(in, 99);
    out->max_us = in->max;
}

static void acc_export(const perf_acc *a, display_perf_snapshot *o)
{
    memset(o, 0, sizeof(*o));
    o->frames = a->frames; o->render_frames = a->render_frames;
    o->present_frames = a->present_frames; o->deadline_misses = a->deadline_misses;
    o->dirty_pixels = a->dirty_pixels; o->dirty_rects = a->dirty_rects;
    o->atomic_frames = a->atomic_frames;
    o->submitted_transactions = a->submitted_transactions;
    o->submitted_bytes = a->submitted_bytes;
    o->completed_transactions = a->completed_transactions;
    o->completed_bytes = a->completed_bytes;
    o->cancelled_transactions = a->cancelled_transactions;
    o->cancelled_bytes = a->cancelled_bytes;
    o->unmatched_completions = a->unmatched_completions;
    o->submit_overlaps = a->submit_overlaps;
    o->queue_highwater = a->queue_highwater;
    o->queue_depth_valid = a->queue_depth_valid;
    dist_export(&a->wall, &o->wall); dist_export(&a->record, &o->record);
    dist_export(&a->raster, &o->raster); dist_export(&a->present, &o->present);
    dist_export(&a->submit_to_complete, &o->submit_to_complete);
    dist_export(&a->completion_cadence, &o->completion_cadence);
    for (int i = 0; i < DISPLAY_PERF_STAGE_COUNT; i++)
        dist_export(&a->stage[i], &o->stage[i]);
}

void display_perf_init(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(&s_all, 0, sizeof(s_all));
    memset(&s_trace_acc, 0, sizeof(s_trace_acc));
    memset(s_ring, 0, sizeof(s_ring));
    s_pending = false; s_have_last_complete = false;
    s_trace_have_last_complete = false; s_trace_active = false;
    s_trace_generation = 0;
    s_ring_head = s_ring_count = 0;
    portEXIT_CRITICAL(&s_lock);
}

void display_perf_reset(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(&s_all, 0, sizeof(s_all));
    s_pending = false;
    s_have_last_complete = false;
    portEXIT_CRITICAL(&s_lock);
}

void display_perf_submit(uint32_t bytes, uint64_t now_us)
{
    portENTER_CRITICAL(&s_lock);
    if (s_pending) {
        s_all.submit_overlaps++;
        if (s_trace_active) s_trace_acc.submit_overlaps++;
    }
    s_all.submitted_transactions++; s_all.submitted_bytes += bytes;
    if (s_trace_active) { s_trace_acc.submitted_transactions++; s_trace_acc.submitted_bytes += bytes; }
    s_pending_us = now_us; s_pending_bytes = bytes; s_pending = true;
    s_pending_trace_generation = s_trace_active ? s_trace_generation : 0;
    portEXIT_CRITICAL(&s_lock);
}

void display_perf_cancel_submit(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_pending) {
        s_all.cancelled_transactions++; s_all.cancelled_bytes += s_pending_bytes;
        if (s_trace_active && s_pending_trace_generation == s_trace_generation) {
            s_trace_acc.cancelled_transactions++;
            s_trace_acc.cancelled_bytes += s_pending_bytes;
        }
        s_pending = false;
    }
    portEXIT_CRITICAL(&s_lock);
}

void display_perf_complete_isr(uint32_t bytes, uint64_t now_us)
{
    portENTER_CRITICAL_ISR(&s_lock);
    uint32_t actual = bytes ? bytes : (s_pending ? s_pending_bytes : 0);
    s_all.completed_transactions++; s_all.completed_bytes += actual;
    if (s_trace_active) { s_trace_acc.completed_transactions++; s_trace_acc.completed_bytes += actual; }
    if (!s_pending) {
        s_all.unmatched_completions++;
        if (s_trace_active) s_trace_acc.unmatched_completions++;
    } else if (s_trace_active && s_pending_trace_generation != s_trace_generation) {
        /* The bytes completed inside this trace, but their submit preceded it;
         * keep throughput exact while marking the latency pair unavailable. */
        s_trace_acc.unmatched_completions++;
    }
    if (s_have_last_complete) {
        uint64_t gap = now_us - s_last_complete_us;
        uint32_t cadence = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
        dist_add(&s_all.completion_cadence, cadence);
    }
    if (s_trace_active && s_trace_have_last_complete) {
        uint64_t gap = now_us - s_trace_last_complete_us;
        uint32_t cadence = gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
        dist_add(&s_trace_acc.completion_cadence, cadence);
    }
    if (s_trace_active) { s_trace_last_complete_us = now_us; s_trace_have_last_complete = true; }
    s_last_complete_us = now_us; s_have_last_complete = true;
    if (s_pending) {
        uint64_t delta = now_us - s_pending_us;
        uint32_t us = delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
        dist_add(&s_all.submit_to_complete, us);
        if (s_trace_active && s_pending_trace_generation == s_trace_generation)
            dist_add(&s_trace_acc.submit_to_complete, us);
        s_pending = false;
    }
    portEXIT_CRITICAL_ISR(&s_lock);
}

void display_perf_add_stage(display_perf_stage stage, uint32_t us)
{
    if ((unsigned)stage >= DISPLAY_PERF_STAGE_COUNT || us == DISPLAY_PERF_MISSING_U32) return;
    portENTER_CRITICAL(&s_lock);
    dist_add(&s_all.stage[stage], us);
    if (s_trace_active) dist_add(&s_trace_acc.stage[stage], us);
    portEXIT_CRITICAL(&s_lock);
}

void display_perf_queue_depth(uint32_t depth)
{
    portENTER_CRITICAL(&s_lock);
    s_all.queue_depth_valid = true;
    if (depth > s_all.queue_highwater) s_all.queue_highwater = depth;
    if (s_trace_active) {
        s_trace_acc.queue_depth_valid = true;
        if (depth > s_trace_acc.queue_highwater) s_trace_acc.queue_highwater = depth;
    }
    portEXIT_CRITICAL(&s_lock);
}

void display_perf_frame(uint32_t dirty_pixels, uint16_t rects, bool atomic,
                        uint32_t record_us, uint32_t raster_us,
                        uint32_t present_us, uint32_t wall_us)
{
    portENTER_CRITICAL(&s_lock);
    perf_acc *set[2] = { &s_all, s_trace_active ? &s_trace_acc : NULL };
    for (int i = 0; i < 2 && set[i]; i++) {
        perf_acc *a = set[i];
        a->frames++; a->dirty_pixels += dirty_pixels; a->dirty_rects += rects;
        if (rects) a->present_frames++;
        if (atomic && rects) a->atomic_frames++;
        if (record_us != DISPLAY_PERF_MISSING_U32 || raster_us != DISPLAY_PERF_MISSING_U32)
            a->render_frames++;
        if (record_us != DISPLAY_PERF_MISSING_U32) dist_add(&a->record, record_us);
        if (raster_us != DISPLAY_PERF_MISSING_U32) dist_add(&a->raster, raster_us);
        /* A frame evaluation with no dirty rectangles did not present pixels;
         * keep its bookkeeping separate from actual presentation latency. */
        if (rects && present_us != DISPLAY_PERF_MISSING_U32) dist_add(&a->present, present_us);
        if (wall_us != DISPLAY_PERF_MISSING_U32) {
            dist_add(&a->wall, wall_us);
            if (wall_us > s_frame_deadline_us) a->deadline_misses++;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

void display_perf_snapshot_get(display_perf_snapshot *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock); s_export_scratch = s_all; portEXIT_CRITICAL(&s_lock);
    acc_export(&s_export_scratch, out);
}

static void json_string(const char *s)
{
    putchar('"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else if (c >= 0x20 && c < 0x7f) putchar(c);
        else printf("\\u%04x", c);
    }
    putchar('"');
}

static void print_dist(const char *key, const display_perf_dist *d)
{
    printf(",\"%s\":{\"n\":%llu,\"total_us\":%llu,\"p50_us\":", key,
           (unsigned long long)d->samples, (unsigned long long)d->total_us);
    if (d->valid) printf("%lu,\"p95_us\":%lu,\"p99_us\":%lu,\"max_us\":%lu}",
                         (unsigned long)d->p50_us, (unsigned long)d->p95_us,
                         (unsigned long)d->p99_us, (unsigned long)d->max_us);
    else printf("null,\"p95_us\":null,\"p99_us\":null,\"max_us\":null}");
}

void display_perf_print(const char *prefix, const display_perf_snapshot *s)
{
    if (!s) return;
    /* Formatting runs outside measured regions. Floating point avoids relying
     * on non-portable 128-bit integers and cannot overflow for uint64 inputs. */
    uint64_t bps = s->elapsed_us ?
        (uint64_t)((double)s->completed_bytes * 1000000.0 / (double)s->elapsed_us) : 0;
    uint64_t terminal_tx = s->completed_transactions + s->cancelled_transactions;
    uint64_t terminal_bytes = s->completed_bytes + s->cancelled_bytes;
    uint64_t tx_gap = s->submitted_transactions > terminal_tx ? s->submitted_transactions - terminal_tx : 0;
    uint64_t byte_gap = s->submitted_bytes > terminal_bytes ? s->submitted_bytes - terminal_bytes : 0;
    printf("%s,{\"trace\":", prefix ? prefix : "PERF"); json_string(s->trace_name);
    printf(",\"seconds\":%u,\"elapsed_us\":%llu,\"frames\":%llu,\"render_frames\":%llu"
           ",\"present_frames\":%llu,\"deadline_misses\":%llu,\"dirty_pixels\":%llu"
           ",\"dirty_rects\":%llu,\"atomic_frames\":%llu,\"submitted_tx\":%llu"
           ",\"submitted_bytes\":%llu,\"completed_tx\":%llu,\"completed_bytes\":%llu"
           ",\"cancelled_tx\":%llu,\"cancelled_bytes\":%llu"
           ",\"throughput_Bps\":",
           (unsigned)s->trace_seconds, (unsigned long long)s->elapsed_us,
           (unsigned long long)s->frames, (unsigned long long)s->render_frames,
           (unsigned long long)s->present_frames, (unsigned long long)s->deadline_misses,
           (unsigned long long)s->dirty_pixels, (unsigned long long)s->dirty_rects,
           (unsigned long long)s->atomic_frames, (unsigned long long)s->submitted_transactions,
           (unsigned long long)s->submitted_bytes, (unsigned long long)s->completed_transactions,
           (unsigned long long)s->completed_bytes,
           (unsigned long long)s->cancelled_transactions, (unsigned long long)s->cancelled_bytes);
    if (s->elapsed_us) printf("%llu", (unsigned long long)bps); else printf("null");
    printf(",\"pending_tx\":%llu,\"pending_bytes\":%llu"
           ",\"unmatched_completions\":%llu,\"submit_overlaps\":%llu,\"queue_highwater\":",
           (unsigned long long)tx_gap, (unsigned long long)byte_gap,
           (unsigned long long)s->unmatched_completions,
           (unsigned long long)s->submit_overlaps);
    if (s->queue_depth_valid) printf("%lu", (unsigned long)s->queue_highwater); else printf("null");
    print_dist("frame_work", &s->wall); print_dist("record", &s->record);
    print_dist("raster", &s->raster); print_dist("present", &s->present);
    print_dist("submit_to_complete", &s->submit_to_complete);
    print_dist("completion_cadence", &s->completion_cadence);
    static const char *const names[] = { "copy", "swap", "rotate", "queue_wait", "dma_wait" };
    for (int i = 0; i < DISPLAY_PERF_STAGE_COUNT; i++) print_dist(names[i], &s->stage[i]);
    printf("}\n");
}

void display_perf_trace_start(const char *name, uint32_t seconds, uint64_t now_us)
{
    if (!seconds) seconds = 1;
    portENTER_CRITICAL(&s_lock);
    memset(&s_trace_acc, 0, sizeof(s_trace_acc));
    memset(s_trace_name, 0, sizeof(s_trace_name));
    if (name) strncpy(s_trace_name, name, sizeof(s_trace_name) - 1);
    s_trace_seconds = seconds; s_trace_started = now_us;
    s_trace_have_last_complete = false;
    s_trace_generation++;
    if (!s_trace_generation) s_trace_generation++;
    uint64_t span = (uint64_t)seconds * 1000000u;
    s_trace_deadline = UINT64_MAX - now_us < span ? UINT64_MAX : now_us + span;
    s_trace_active = true;
    portEXIT_CRITICAL(&s_lock);
}

bool display_perf_tick(uint64_t now_us, display_perf_trace *completed)
{
    display_perf_trace result;
    bool done = false;
    portENTER_CRITICAL(&s_lock);
    if (s_trace_active && now_us >= s_trace_deadline) {
        memset(&result, 0, sizeof(result));
        memcpy(result.name, s_trace_name, sizeof(result.name));
        result.requested_seconds = s_trace_seconds;
        result.started_us = s_trace_started; result.ended_us = now_us;
        s_export_scratch = s_trace_acc;
        s_trace_active = false;
        done = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!done) return false;
    acc_export(&s_export_scratch, &result.perf);
    memcpy(result.perf.trace_name, result.name, sizeof(result.perf.trace_name));
    result.perf.trace_seconds = result.requested_seconds;
    result.perf.elapsed_us = result.ended_us - result.started_us;
    portENTER_CRITICAL(&s_lock);
    s_ring[s_ring_head] = result;
    s_ring_head = (uint8_t)((s_ring_head + 1) % DISPLAY_PERF_TRACE_RING);
    if (s_ring_count < DISPLAY_PERF_TRACE_RING) s_ring_count++;
    portEXIT_CRITICAL(&s_lock);
    if (completed) *completed = result;
    return true;
}

bool display_perf_trace_status(char *name, size_t name_size,
                               uint32_t *remaining_ms, uint64_t now_us)
{
    bool active;
    portENTER_CRITICAL(&s_lock);
    active = s_trace_active;
    if (name && name_size) {
        size_t n = 0;
        if (active) while (n + 1 < name_size && s_trace_name[n]) n++;
        if (active && n) memcpy(name, s_trace_name, n);
        name[n] = 0;
    }
    if (remaining_ms) {
        uint64_t left = active && now_us < s_trace_deadline ? s_trace_deadline - now_us : 0;
        *remaining_ms = left / 1000u > UINT32_MAX ? UINT32_MAX : (uint32_t)(left / 1000u);
    }
    portEXIT_CRITICAL(&s_lock);
    return active;
}

size_t display_perf_trace_count(void)
{
    portENTER_CRITICAL(&s_lock); size_t n = s_ring_count; portEXIT_CRITICAL(&s_lock);
    return n;
}

bool display_perf_trace_get(size_t newest_index, display_perf_trace *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_lock);
    if (newest_index >= s_ring_count) { portEXIT_CRITICAL(&s_lock); return false; }
    unsigned pos = (s_ring_head + DISPLAY_PERF_TRACE_RING - 1u - newest_index) % DISPLAY_PERF_TRACE_RING;
    *out = s_ring[pos];
    portEXIT_CRITICAL(&s_lock);
    return true;
}
