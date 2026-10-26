#include "asset_job.h"
#include <string.h>
void asset_init(asset_job *j) {
    memset(j, 0, sizeof *j);
    atomic_init(&j->state, ASSET_IDLE);
    atomic_init(&j->cancel, 0);
}
int asset_submit(asset_job *j, const asset_request *r) {
    if (atomic_load_explicit(&j->state, memory_order_acquire) != ASSET_IDLE)
        return 0;
    if (!r || !r->page_bytes || !r->bytes || !r->page_count || r->page_count > ASSET_PAGES ||
        !memchr(r->path, 0, ASSET_PATH))
        return 0;
    if ((uint64_t)r->page_bytes * r->page_count < r->bytes)
        return 0;
    for (unsigned i = 0; i < r->page_count; i++)
        if (!r->pages[i])
            return 0;
    j->request = *r;
    j->file = 0;
    j->loaded = 0;
    memset(&j->result, 0, sizeof j->result);
    j->result.code = ASSET_FAILED;
    atomic_store_explicit(&j->cancel, 0, memory_order_relaxed);
    atomic_store_explicit(&j->state, ASSET_QUEUED, memory_order_release);
    return 1;
}
void asset_cancel(asset_job *j) { atomic_store_explicit(&j->cancel, 1, memory_order_release); }
static int finish(asset_job *j, const asset_io *io, int result) {
    if (j->file) {
        uint32_t start = io->micros ? io->micros() : 0;
        io->close(j->file);
        if (io->micros)
            j->result.close_us += io->micros() - start;
        j->file = 0;
    }
    j->result.code = result;
    atomic_store_explicit(&j->state, ASSET_DONE, memory_order_release);
    return 1;
}
int asset_worker_step(asset_job *j, const asset_io *io) {
    int expected = ASSET_QUEUED;
    if (!atomic_compare_exchange_strong_explicit(&j->state, &expected, ASSET_RUNNING,
                                                 memory_order_acq_rel, memory_order_acquire))
        return 0;
    if (atomic_load_explicit(&j->cancel, memory_order_acquire))
        return finish(j, io, ASSET_CANCELLED);
    if (!j->file) {
        uint32_t start = io->micros ? io->micros() : 0;
        j->file = io->open(j->request.path);
        if (io->micros)
            j->result.open_us += io->micros() - start;
        if (!j->file)
            return finish(j, io, ASSET_FAILED);
    }
    if (atomic_load_explicit(&j->cancel,memory_order_acquire))
        return finish(j,io,ASSET_CANCELLED);
    unsigned page = j->loaded / j->request.page_bytes, offset = j->loaded % j->request.page_bytes;
    unsigned count = j->request.bytes - j->loaded;
    if (count > ASSET_SLICE)
        count = ASSET_SLICE;
    if (count > j->request.page_bytes - offset)
        count = j->request.page_bytes - offset;
    uint32_t start = io->micros ? io->micros() : 0;
    size_t got = io->read(j->file, (char *)j->request.pages[page] + offset, count);
    if (io->micros) {
        uint32_t us = io->micros() - start;
        j->result.read_us += us;
        if (us > j->result.read_max_us)
            j->result.read_max_us = us;
    }
    j->result.read_calls++;
    j->loaded += (unsigned)got;
    if (atomic_load_explicit(&j->cancel, memory_order_acquire))
        return finish(j, io, ASSET_CANCELLED);
    if (got != count)
        return finish(j, io, ASSET_FAILED);
    if (j->loaded == j->request.bytes)
        return finish(j, io, ASSET_OK);
    atomic_store_explicit(&j->state, ASSET_QUEUED, memory_order_release);
    return 1;
}
int asset_take(asset_job *j, asset_request *out, asset_result *result) {
    if (atomic_load_explicit(&j->state, memory_order_acquire) != ASSET_DONE)
        return 0;
    *out = j->request;
    *result = j->result;
    if (atomic_load_explicit(&j->cancel, memory_order_acquire))
        result->code = ASSET_CANCELLED;
    atomic_store_explicit(&j->state, ASSET_IDLE, memory_order_release);
    return 1;
}
