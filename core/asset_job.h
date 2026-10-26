#ifndef ASSET_JOB_H
#define ASSET_JOB_H
#include "asset.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
enum { ASSET_IDLE, ASSET_QUEUED, ASSET_RUNNING, ASSET_DONE };
typedef struct {
    atomic_int state, cancel;
    asset_request request; /* immutable until terminal result consumed */
    void *file;            /* worker exclusively owns this handle */
    unsigned loaded;
    asset_result result;
} asset_job;
typedef struct {
    void *(*open)(const char *);
    size_t (*read)(void *, void *, size_t);
    void (*close)(void *);
    uint32_t (*micros)(void);
} asset_io;
void asset_init(asset_job *);
int asset_submit(asset_job *, const asset_request *);
void asset_cancel(asset_job *);
/* Worker only: one bounded-size fread per invocation; never UI/sensor task. */
int asset_worker_step(asset_job *, const asset_io *);
/* UI only, nonblocking. Success transfers every destination page back to UI.
 * CANCEL/FAIL pages must be freed or reused; OK may be atomically published.
 * Job+all buffers stay alive until this returns1, even after screen leave. */
int asset_take(asset_job *, asset_request *, asset_result *result);
#endif
