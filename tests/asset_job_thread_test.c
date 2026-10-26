#include "asset_job.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define YIELD() SwitchToThread()
#else
#include <pthread.h>
#include <sched.h>
#define YIELD() sched_yield()
#endif
static asset_job job;
static atomic_int entered, allowed;
static void *open_file(const char *path) {
    (void)path;
    return &job;
}
static size_t read_file(void *file, void *dest, size_t count) {
    assert(file == &job);
    atomic_store(&entered, 1);
    while (!atomic_load(&allowed))
        YIELD();
    memset(dest, 0x5a, count);
    return count;
}
static void close_file(void *file) { assert(file == &job); }
static asset_io io = {open_file, read_file, close_file, NULL};
#ifdef _WIN32
static DWORD WINAPI worker(void *arg)
#else
static void *worker(void *arg)
#endif
{
    (void)arg;
    assert(asset_worker_step(&job, &io));
    return 0;
}
int main(void) {
    for (unsigned i = 0; i < 200; i++) {
        asset_init(&job);
        atomic_store(&entered, 0);
        atomic_store(&allowed, 0);
        asset_request request = {0}, out;
        asset_result result;
        strcpy(request.path, "immutable");
        request.pages[0] = malloc(64);
        assert(request.pages[0]);
        request.page_bytes = request.bytes = 64;
        request.page_count = 1;
        request.tag = i;
        assert(asset_submit(&job, &request));
#ifdef _WIN32
        HANDLE task = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        assert(task);
#else
        pthread_t task;
        assert(!pthread_create(&task, NULL, worker, NULL));
#endif
        while (!atomic_load(&entered))
            YIELD();
        asset_cancel(&job);
        assert(!asset_take(&job, &out, &result));
        atomic_store(&allowed, 1);
#ifdef _WIN32
        assert(WaitForSingleObject(task, INFINITE) == WAIT_OBJECT_0);
        CloseHandle(task);
#else
        assert(!pthread_join(task, NULL));
#endif
        assert(asset_take(&job, &out, &result) && result.code == ASSET_CANCELLED && out.tag == i);
        for (unsigned k = 0; k < 64; k++)
            assert(((unsigned char *)out.pages[0])[k] == 0x5a);
        free(out.pages[0]);
    }
    puts("PASS200 real-thread cancellation during owned read, release/acquire terminal "
         "publication, exact bytes and no early reclaim");
}
