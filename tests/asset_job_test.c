#include "asset_job.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static asset_job job;
static unsigned char a[65536], b[65536];
static int opens, closes, reads, cancel_read, missing, short_read;
static size_t position;
static void *open_file(const char *path) {
    assert(!strcmp(path, "/sdcard/sky/earth.565"));
    opens++;
    position = 0;
    return missing ? 0 : &position;
}
static size_t read_file(void *f, void *dst, size_t n) {
    assert(f == &position);
    assert(n <= ASSET_SLICE);
    reads++;
    asset_request r;
    asset_result result;
    assert(!asset_take(&job, &r, &result));
    if (cancel_read)
        asset_cancel(&job);
    if (short_read)
        n /= 2;
    for (size_t i = 0; i < n; i++)
        ((unsigned char *)dst)[i] = (unsigned char)(position + i);
    position += n;
    return n;
}
static void close_file(void *f) {
    assert(f == &position);
    closes++;
}
static uint32_t micros(void) {
    static uint32_t tick = 0xfffffff0u;
    tick += 13;
    return tick;
}
static asset_io io = {open_file, read_file, close_file, micros};
static asset_request request(void) {
    asset_request r = {0};
    strcpy(r.path, "/sdcard/sky/earth.565");
    r.pages[0] = a;
    r.pages[1] = b;
    r.page_count = 2;
    r.page_bytes = 65536;
    r.bytes = sizeof a + sizeof b;
    return r;
}
int main(void) {
    asset_init(&job);
    asset_request r = request(), out;
    asset_result result;
    assert(asset_submit(&job, &r));
    assert(!asset_submit(&job, &r));
    memset(r.path, 'x', sizeof r.path);
    assert(!asset_take(&job, &out, &result));
    while (asset_worker_step(&job, &io)) {
    }
    assert(asset_take(&job, &out, &result) && result.code == ASSET_OK);
    assert(opens == 1 && closes == 1 && reads == 4);
    assert(result.open_us == 13 && result.read_us == 52 && result.read_max_us == 13 &&
           result.read_calls == 4 && result.close_us == 13);
    for (unsigned i = 0; i < sizeof a; i++) {
        assert(a[i] == (unsigned char)i);
        assert(b[i] == (unsigned char)(i + sizeof a));
    }
    r = request();
    assert(asset_submit(&job, &r));
    asset_cancel(&job);
    assert(!asset_take(&job, &out, &result));
    assert(asset_worker_step(&job, &io));
    assert(asset_take(&job, &out, &result) && result.code == ASSET_CANCELLED);
    assert(opens == 1);
    assert(asset_submit(&job, &r));
    cancel_read = 1;
    assert(asset_worker_step(&job, &io));
    assert(asset_take(&job, &out, &result) && result.code == ASSET_CANCELLED);
    assert(out.pages[0] == a && closes == 2);
    cancel_read = 0;
    assert(asset_submit(&job, &r));
    missing = 1;
    assert(asset_worker_step(&job, &io));
    assert(asset_take(&job, &out, &result) && result.code == ASSET_FAILED);
    missing = 0;
    assert(asset_submit(&job, &r));
    short_read = 1;
    assert(asset_worker_step(&job, &io));
    assert(asset_take(&job, &out, &result) && result.code == ASSET_FAILED);
    short_read = 0;
    assert(asset_submit(&job, &r));
    while (asset_worker_step(&job, &io)) {
    }
    asset_cancel(&job);
    assert(asset_take(&job, &out, &result) && result.code == ASSET_CANCELLED);
    r.bytes = 0;
    assert(!asset_submit(&job, &r));
    printf("PASS immutable path, bounded reads, completion publication, cancellation "
           "before/during/after read, no early reclaim, short/missing asset; job=%zu bytes\n",
           sizeof job);
}
