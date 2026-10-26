#if defined(__MINGW32__)
#define __USE_MINGW_ANSI_STDIO 1
#endif
/* Fixed-rate SPSC queue and asynchronous SD worker. No filesystem on UI calls. */
#include "imu_backend.h"
#include "diag_log.h"
#include "runtime_admission.h"
#ifdef ESP_PLATFORM
#include "board.h"
#endif
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include <unistd.h>
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&lock)
#define UNLOCK() portEXIT_CRITICAL(&lock)
#define MKDIR(p) mkdir(p, 0775)
#else
#include <io.h>
#include <direct.h>
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
#define MKDIR(p) _mkdir(p)
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif
#define CAP 512u
#ifndef IMU_SEGMENT_BYTES
#define IMU_SEGMENT_BYTES (4u*1024u*1024u)
#endif
#ifdef IMU_REC_TEST
extern int imu_test_sync(FILE *fp);
#define DURABLE(fp) imu_test_sync(fp)
#elif defined(ESP_PLATFORM)
#define DURABLE(fp) fsync(fileno(fp))
#else
#define DURABLE(fp) _commit(_fileno(fp))
#endif
typedef struct { uint32_t magic, seq, active, hz, file, sum; } intent_record;
static intent_record intent;
static int intent_loaded, intent_dirty, boot_resume, intent_corrupt;
static int intent_slot=-1;
static int checkpoint_restoring;
static uint32_t retry_at, intent_retry_at;
static uint32_t intent_sum(const intent_record *r)
{ const unsigned char *p=(const unsigned char *)r; uint32_t h=2166136261u; for(unsigned i=0;i<sizeof(*r)-4;i++) h=(h^p[i])*16777619u; return h; }
static int intent_valid(const intent_record *r)
{ return r->magic==0x494d5532u && r->sum==intent_sum(r) && r->active<=1 && (r->hz==25||r->hz==50||r->hz==100) && r->file<=10000; }
static void intent_load(void)
{
    char path[160]; intent_record r, latest={.hz=50,.file=10000};
    int corrupt=0, read_error=0, latest_slot=-1;
    for(int i=0;i<2;i++) {
        snprintf(path,sizeof path,"%s/imu/intent.%d.bin",hal_sd_root(),i); FILE *f=fopen(path,"rb");
        if(!f) { if(errno!=ENOENT)read_error=1; continue; }
        size_t got=fread(&r,1,sizeof r,f);
        int extra=fgetc(f), failed=ferror(f);
        if(fclose(f))failed=1;
        if(failed) {read_error=1;continue;}
        int ok=got==sizeof r && extra==EOF && intent_valid(&r);
        if(!ok)corrupt=1;
        if(ok && (!latest.magic || (int32_t)(r.seq-latest.seq)>0)) {latest=r;latest_slot=i;}
    }
    /* Even a readable inactive slot is inconclusive if its sibling might hold
     * a newer active generation. Keep worker work pending and retry the read. */
    if(read_error) {intent_loaded=0;return;}
    intent=latest;intent_slot=latest_slot;intent_corrupt=corrupt && !intent.magic;
    intent_loaded=1; boot_resume=intent.active!=0;
}
static int intent_save(void)
{
    char path[160]; intent_record r=intent, check; r.magic=0x494d5532u; r.seq++; r.sum=intent_sum(&r);
    /* Restored raw generations need not follow sequence parity. Alternate from
     * the actual latest bank so a failed write always preserves that bank. */
    int target=intent_slot<0?0:1-intent_slot;
    snprintf(path,sizeof path,"%s/imu/intent.%d.bin",hal_sd_root(),target);
    FILE *f=fopen(path,"wb"); if(!f) return 0;
    int ok=fwrite(&r,1,sizeof r,f)==sizeof r && !fflush(f) && !DURABLE(f); if(fclose(f)) ok=0;
    if(!ok) return 0;
    f=fopen(path,"rb"); if(!f) return 0;
    ok=fread(&check,1,sizeof check,f)==sizeof check && !memcmp(&r,&check,sizeof r) && fgetc(f)==EOF && !ferror(f); if(fclose(f))ok=0;
    if(ok) { intent=r; intent_slot=target; intent_dirty=0; intent_corrupt=0; } return ok;
}
#define BLOCK 4096
static hal_imu_point *ring;
static unsigned head, tail, count;
static uint32_t start_ms, stop_ms;
static hal_imu_stream stream;
static hal_imu_record status;
static hal_imu_file file_status;
static int command, arg_id, arg_dir;
enum { CMD_START=1, CMD_STOP, CMD_FILES, CMD_DELETE };
typedef struct {
    FILE *fp;
    char data[BLOCK], path[128], line[256];
    unsigned used, rows;
    uint32_t last_flush, last_ms;
    uint64_t bytes;
} writer;
static writer *w;

int hal_imu_stream_start(unsigned hz)
{
    if(!nos_runtime_recording_allowed()) return 0;
    if (hz != 25 && hz != 50 && hz != 100) return 0;
    LOCK(); int running = stream.hz != 0 || checkpoint_restoring; UNLOCK();
    if (running) return 0;
    if (!imu_backend_wake()) return 0;
    if (!ring) ring = nos_mem_alloc(CAP * sizeof *ring, "imu.stream");
    if (!ring) return 0;
    LOCK();
    head = tail = count = 0; memset(&stream, 0, sizeof stream);
    start_ms = imu_backend_ms(); stop_ms = start_ms; stream.hz = hz;
    UNLOCK();
    if (!imu_backend_start(hz)) { LOCK(); stream.hz = 0; UNLOCK(); return 0; }
    return 1;
}
void hal_imu_stream_stop(void)
{
    /* Clear acceptance before stopping the producer; an in-flight read is discarded. */
    LOCK(); if (stream.hz) stop_ms=imu_backend_ms(); stream.hz = 0; UNLOCK();
    imu_backend_stop();
}
void imu_stream_push(const hal_imu *v, uint32_t now, uint64_t utc)
{
    LOCK();
    if (stream.hz) {
        if (count == CAP) stream.dropped++; /* preserve oldest queued samples */
        else {
            ring[head] = (hal_imu_point){ .utc_ms=utc, .ms=now-start_ms, .v=*v };
            head = (head + 1) % CAP; count++;
        }
    }
    UNLOCK();
}
void imu_stream_missed(unsigned n, int error)
{
    LOCK(); if (stream.hz) { if (error) stream.errors += n; else stream.missed += n; } UNLOCK();
}
int hal_imu_stream_read(hal_imu_point *out, int max)
{
    if (!out || max <= 0) return 0;
    /* Small critical sections: never copy a whole ring with interrupts disabled. */
    int n = 0;
    while (n < max) {
        LOCK();
        if (!count) { UNLOCK(); break; }
        out[n++] = ring[tail]; tail = (tail + 1) % CAP; count--;
        UNLOCK();
    }
    return n;
}
void hal_imu_stream_get(hal_imu_stream *out)
{ LOCK(); *out=stream; out->queued=count; UNLOCK(); }
void hal_imu_record_get(hal_imu_record *out)
{ LOCK(); *out=status; UNLOCK(); }
void hal_imu_file_get(hal_imu_file *out)
{ LOCK(); *out=file_status; UNLOCK(); }

static int request(int cmd, int id, int dir)
{
    if (!imu_backend_wake()) return 0;
    LOCK();
    if (checkpoint_restoring || command || file_status.busy || status.state == HAL_IMU_STARTING || status.state == HAL_IMU_STOPPING ||
        (cmd != CMD_STOP && status.state == HAL_IMU_RECORDING)) { UNLOCK(); return 0; }
    if (cmd == CMD_STOP && status.state != HAL_IMU_RECORDING && status.state != HAL_IMU_ERROR) { UNLOCK(); return 0; }
    if (cmd == CMD_START) {
        memset(&status, 0, sizeof status); status.state=HAL_IMU_STARTING; status.hz=(unsigned)id;
    } else if (cmd == CMD_STOP) status.state=HAL_IMU_STOPPING;
    else file_status.busy=1;
    arg_id=id; arg_dir=dir; command=cmd;
    UNLOCK();
    imu_backend_wake();
    return 1;
}
int hal_imu_record_start(unsigned hz)
{ return nos_runtime_recording_allowed() && (hz==25 || hz==50 || hz==100) && request(CMD_START, (int)hz, 0); }
int hal_imu_record_stop(void) { return request(CMD_STOP, 0, 0); }
int hal_imu_files_request(int id, int dir) { return request(CMD_FILES, id, dir); }
int hal_imu_file_delete(int id) { return id>=0 && id<=9999 && request(CMD_DELETE, id, 0); }

static void publish(const char *error, int state)
{
    hal_imu_stream s; hal_imu_stream_get(&s);
    LOCK();
    int report_error=state==HAL_IMU_ERROR && (status.state!=HAL_IMU_ERROR || (error && strcmp(status.message,error)));
    status.dropped=s.dropped; status.missed=s.missed; status.errors=s.errors;
    if (w) { status.bytes=w->bytes; status.samples=w->rows; }
    if (status.state==HAL_IMU_RECORDING || status.state==HAL_IMU_STOPPING)
        status.elapsed_ms=(stream.hz ? imu_backend_ms() : stop_ms)-start_ms;
    /* Do not overwrite a STOP requested concurrently by the UI. */
    if (state >= 0 && !(state==HAL_IMU_RECORDING && status.state==HAL_IMU_STOPPING)) status.state=state;
    if (error) snprintf(status.message, sizeof status.message, "%s", error);
    UNLOCK();
    if(report_error) diag_log_event(DIAG_IMU_ERROR,errno,(int32_t)intent.file,(int32_t)(w?w->rows:0));
}
static int flush_block(void)
{
    if (!w->used) return 1;
    size_t n=fwrite(w->data, 1, w->used, w->fp);
    w->bytes+=n;
    if (n!=w->used || fflush(w->fp) || DURABLE(w->fp)) return 0;
    w->used=0; w->last_flush=imu_backend_ms(); return 1;
}
static int finish(const char *error)
{
    hal_imu_stream_stop();
    if (w && w->fp) {
        if (!error && !flush_block()) error="SD WRITE FAILED";
        if (fclose(w->fp) && !error) error="SD CLOSE FAILED";
        w->fp=NULL;
    }
    LOCK();int stopping=status.state==HAL_IMU_STOPPING;UNLOCK();
    publish(error ? error : stopping?"SAVING STOP":"SAVED",
        error ? HAL_IMU_ERROR : stopping?HAL_IMU_STOPPING:HAL_IMU_IDLE);
    return error==NULL;
}
static int previous_tail(int previous,uint64_t *bytes,uint64_t *valid,int *missing)
{
    *bytes=*valid=0;*missing=0;
    if(previous>=10000)return 1;
    char path[160];snprintf(path,sizeof path,"%s/imu/%04d.csv",hal_sd_root(),previous);
    FILE *f=fopen(path,"rb");
    if(!f) {if(errno==ENOENT){*missing=1;return 1;}return 0;}
    int ok=!fseek(f,0,SEEK_END);long size=ok?ftell(f):-1;
    if(size<0)ok=0;
    if(ok) {
        long offset=size>BLOCK?size-BLOCK:0;
        if(fseek(f,offset,SEEK_SET))ok=0;
        else {
            size_t expected=(size_t)(size-offset),n=fread(w->data,1,expected,f);
            if(n!=expected || ferror(f))ok=0;
            else {
                *bytes=(uint64_t)size;
                for(size_t j=n;j>0;j--)if(w->data[j-1]=='\n'){*valid=(uint64_t)offset+j;break;}
            }
        }
    }
    if(fclose(f))ok=0;
    return ok;
}
static void begin_record(unsigned hz)
{
    /* Boot/retry must claim STARTING atomically with UI request admission. A
     * request arriving while the prior segment closes is handled next pass. */
    LOCK();
    if(command || (w && w->fp)) { UNLOCK(); return; }
    status.state=HAL_IMU_STARTING; status.hz=hz;
    UNLOCK();
    if (w) memset(w,0,sizeof *w);
    LOCK(); memset(&stream,0,sizeof stream); UNLOCK();
    hal_storage sd; hal_storage_get(&sd);
    if (!sd.mounted) { publish("NO SD CARD", HAL_IMU_ERROR); return; }
    if (!w) w=nos_mem_alloc(sizeof *w, "imu.writer");
    if (!w) { publish("NO PSRAM", HAL_IMU_ERROR); return; }
    memset(w, 0, sizeof *w);
    snprintf(w->path, sizeof w->path, "%s/imu", hal_sd_root());
    if (MKDIR(w->path) && errno!=EEXIST) { publish("SD DIRECTORY FAILED", HAL_IMU_ERROR); return; }
    /* Persist intent before accepting the first sensor sample. Failed writes stay dirty. */
    intent.active=1; intent.hz=hz; intent_dirty=1;
    if(!intent_save()) { publish("RESUME SAVE FAILED / RETRY",HAL_IMU_ERROR); return; }
    int previous=(int)intent.file;
    uint64_t prior_bytes=0, valid_bytes=0;int prior_missing;
    if(!previous_tail(previous,&prior_bytes,&valid_bytes,&prior_missing)) {
        publish("SD PREVIOUS READ FAILED / RETRY",HAL_IMU_ERROR);return;
    }
    int fd=-1, id;
    for (unsigned attempt=0; attempt<10000; attempt++) {
        id=(int)(((previous<10000?(unsigned)previous+1u:0u)+attempt)%10000u);
        snprintf(w->path, sizeof w->path, "%s/imu/%04d.csv", hal_sd_root(), id);
        fd=open(w->path, O_WRONLY|O_CREAT|O_EXCL|O_BINARY, 0664);
        if (fd>=0 || errno!=EEXIST) break;
    }
    if (fd<0) { publish("CANNOT CREATE FILE", HAL_IMU_ERROR); return; }
    w->fp=fdopen(fd, "wb");
    if (!w->fp) { close(fd); publish("CANNOT OPEN FILE", HAL_IMU_ERROR); return; }
    /* Immutable linkage keeps the original CSV (including a torn tail) untouched. */
    snprintf(w->path,sizeof w->path,"%s/imu/%04d.link",hal_sd_root(),id);
    FILE *link=fopen(w->path,"wb");
    int linked=0;
    if(link) { linked=fprintf(link,"previous=%d\nreason=%s\nprevious_bytes=%llu\nprevious_valid_bytes=%llu\nprevious_missing=%d\n",previous,boot_resume?"reset":"start/rollover",(unsigned long long)prior_bytes,(unsigned long long)valid_bytes,prior_missing)>0 && !fflush(link) && !DURABLE(link); if(fclose(link)) linked=0; }
    if(!linked) { finish("SD LINK FAILED"); return; }
    intent.file=(uint32_t)id; intent_dirty=1;
    if(!intent_save()) { finish("RESUME SAVE FAILED / RETRY"); return; }
    int resumed=boot_resume; boot_resume=0;
    setvbuf(w->fp, NULL, _IONBF, 0); /* batch buffer is explicitly budgeted PSRAM */
    LOCK(); snprintf(status.name, sizeof status.name, "%04d.csv", id); UNLOCK();
    const char *header="ms,utc_ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps,a_g\n";
    w->used=(unsigned)strlen(header); memcpy(w->data, header, w->used);
    if (!flush_block()) { finish("SD HEADER FAILED"); return; }
    if (!hal_imu_stream_start(hz)) { finish("IMU / PSRAM UNAVAILABLE"); return; }
    diag_log_event(resumed?DIAG_IMU_RESUME:DIAG_IMU_START,id,previous,(int32_t)hz);
    publish(resumed?"RESUMED / NEW SEGMENT":"RECORDING", HAL_IMU_RECORDING);
}

/* Scan on the worker. One selected file; no arbitrary list cap. Tail only, even
 * for multi-hour recordings. Incomplete final lines after power loss are ignored. */
static void browse(int id, int direction, int deleting)
{
    hal_imu_file f={0}; f.id=-1;
    hal_storage sd; hal_storage_get(&sd);
    if (!sd.mounted) { snprintf(f.message,sizeof f.message,"NO SD CARD"); goto done; }
    if (!w) { w=nos_mem_alloc(sizeof *w, "imu.writer"); if (w) memset(w,0,sizeof *w); }
    if (!w) { snprintf(f.message,sizeof f.message,"NO PSRAM"); goto done; }
    if (deleting) {
        snprintf(w->path,sizeof w->path,"%s/imu/%04d.csv",hal_sd_root(),id);
        if (remove(w->path)) { snprintf(f.message,sizeof f.message,"DELETE FAILED"); goto done; }
        direction=0;
    }
    snprintf(w->path,sizeof w->path,"%s/imu",hal_sd_root());
    DIR *d=opendir(w->path);
    if (!d) { snprintf(f.message,sizeof f.message, errno==ENOENT ? "NO RECORDINGS" : "SD READ FAILED"); goto done; }
    struct dirent *e; int best=-1;
    while ((e=readdir(d))) {
        const char *n=e->d_name;
        if (strlen(n)!=8 || strcmp(n+4,".csv")) continue;
        int k=0, valid=1;
        for (int j=0;j<4;j++) { if (n[j]<'0'||n[j]>'9') valid=0; k=k*10+n[j]-'0'; }
        if (!valid) continue;
        if ((!direction && k>best) || (direction<0 && k<id && k>best) ||
            (direction>0 && k>id && (best<0 || k<best))) best=k;
    }
    closedir(d);
    if (best<0 && direction) best=id; /* boundary retains the current file */
    if (best<0) { snprintf(f.message,sizeof f.message,"NO RECORDINGS"); goto done; }
    snprintf(w->path,sizeof w->path,"%s/imu/%04d.csv",hal_sd_root(),best);
    struct stat st;
    if (stat(w->path,&st) || !S_ISREG(st.st_mode)) { snprintf(f.message,sizeof f.message,"SD READ FAILED"); goto done; }
    FILE *fp=fopen(w->path,"rb");
    if (!fp) { snprintf(f.message,sizeof f.message,"SD READ FAILED"); goto done; }
    long offset=st.st_size>BLOCK-1 ? (long)(st.st_size-(BLOCK-1)) : 0;
    if (fseek(fp,offset,SEEK_SET)) { fclose(fp); snprintf(f.message,sizeof f.message,"SD SEEK FAILED"); goto done; }
    size_t n=fread(w->data,1,BLOCK-1,fp); int err=ferror(fp); fclose(fp);
    if (err) { snprintf(f.message,sizeof f.message,"SD READ FAILED"); goto done; }
    w->data[n]=0;
    char *p=w->data; if (offset) { p=strchr(p,'\n'); if (p) p++; }
    while (p && *p) {
        char *end=strchr(p,'\n'); if (!end) break;
        unsigned ms; if (sscanf(p,"%u,",&ms)==1) f.duration_ms=ms;
        p=end+1;
    }
    f.valid=1; f.id=best; f.bytes=(uint64_t)st.st_size;
    snprintf(f.name,sizeof f.name,"%04d.csv",best);
    snprintf(f.message,sizeof f.message,deleting ? "DELETED / LATEST" : "ON SD CARD");
done:
    LOCK(); file_status=f; UNLOCK();
}

static int write_point(const hal_imu_point *p)
{
        const hal_imu *v=&p->v;
        float mag=sqrtf(v->ax*v->ax+v->ay*v->ay+v->az*v->az);
        int n=snprintf(w->line,sizeof w->line,"%u,%llu,%.5f,%.5f,%.5f,%.3f,%.3f,%.3f,%.5f\n",
            (unsigned)p->ms,(unsigned long long)p->utc_ms,(double)v->ax,(double)v->ay,(double)v->az,
            (double)v->gx,(double)v->gy,(double)v->gz,(double)mag);
        if (n<0 || n>=(int)sizeof w->line) { finish("SAMPLE FORMAT FAILED"); return 0; }
        if (w->used+(unsigned)n>BLOCK && !flush_block()) { finish("SD WRITE FAILED"); return 0; }
        memcpy(w->data+w->used,w->line,(size_t)n); w->used+=(unsigned)n;
        w->rows++; w->last_ms=p->ms;
    return 1;
}
static int drain_points(unsigned limit)
{
    hal_imu_point point;
    while(limit-- && hal_imu_stream_read(&point,1)) if(!write_point(&point))return 0;
    return 1;
}
static void worker_step(void)
{
    LOCK();int restoring=checkpoint_restoring;UNLOCK();if(restoring)return;
    if(!intent_loaded) {
        uint32_t now=imu_backend_ms();
        if(!intent_retry_at || (int32_t)(now-intent_retry_at)>=0) {
            intent_load();
            intent_retry_at=now+1000;
        }
        if(!intent_loaded) {publish("RESUME READ FAILED / RETRY",HAL_IMU_ERROR);return;}
    }
    /* Commands remain queued while resume intent is unknown; neither START nor
     * STOP may overwrite a generation that could not yet be inspected. */
    LOCK(); int cmd=command, id=arg_id, dir=arg_dir; command=0; UNLOCK();
    if(intent_corrupt && !cmd) { publish("RESUME CHECKPOINT CORRUPT",HAL_IMU_ERROR); return; }
    if(cmd==CMD_STOP) {
        boot_resume=0;hal_imu_stream_stop();
        /* A durable inactive intent is the STOP acknowledgement. A live CSV
         * must finish first, or a failed final write would suppress recovery. */
        if(!w || !w->fp){intent.active=0;intent_dirty=1;}
    }
    int saving_stop=intent_dirty && !intent.active;
    if(intent_dirty && !intent_save()) { if(w && w->fp) finish("RESUME SAVE FAILED / RETRY"); else publish("RESUME SAVE FAILED / RETRY",HAL_IMU_ERROR); return; }
    if(saving_stop && (!w || !w->fp)) publish("STOPPED",HAL_IMU_IDLE);
    if (cmd==CMD_FILES || cmd==CMD_DELETE) { browse(id,dir,cmd==CMD_DELETE); return; }
    if(cmd==CMD_START) { intent.file=10000; begin_record((unsigned)id); }
    else if(cmd!=CMD_STOP && intent.active && (!w || !w->fp) && (int32_t)(imu_backend_ms()-retry_at)>=0) {
        retry_at=imu_backend_ms()+5000; begin_record(intent.hz);
    }
    if(cmd==CMD_STOP && (!w || !w->fp)) { diag_log_event(DIAG_IMU_STOP,(int32_t)intent.file,0,0); publish("STOPPED",HAL_IMU_IDLE); return; }
    if (cmd==CMD_STOP) hal_imu_stream_stop();
    if (!w || !w->fp) return;
    hal_storage sd; hal_storage_get(&sd);
    if (!sd.mounted) { finish("SD REMOVED"); return; }
    if(!drain_points(CAP))return;
    if (cmd==CMD_STOP) {
        if(!finish(NULL))return;
        intent.active=0;intent_dirty=1;
        if(!intent_save()){publish("RESUME SAVE FAILED / RETRY",HAL_IMU_ERROR);return;}
        publish("SAVED",HAL_IMU_IDLE);diag_log_event(DIAG_IMU_STOP,(int32_t)intent.file,0,0);return;
    }
    if(w->bytes+w->used>=IMU_SEGMENT_BYTES) {
        unsigned hz=intent.hz;
#ifdef IMU_REC_TEST
        extern void imu_test_before_rollover_stop(void);
        imu_test_before_rollover_stop();
#endif
        /* Stop acceptance before the final finite drain. Samples arriving
         * between the first drain and this stop still belong to this CSV. */
        hal_imu_stream_stop();
        if(!drain_points(CAP))return;
        finish(NULL); begin_record(hz); return;
    }
    if (imu_backend_ms()-w->last_flush>=1000 && !flush_block()) { finish("SD WRITE FAILED"); return; }
    publish(NULL,-1);
}

int imu_worker_pending(void)
{
    LOCK();int pending=checkpoint_restoring || !intent_loaded || intent_dirty || intent.active || command ||
        file_status.busy || stream.hz || count;UNLOCK();return pending;
}
/* Transfer calls under the SD mutex; the worker uses that same mutex for its
 * complete step. Invalidate cached intent before the next worker may proceed. */
int imu_checkpoint_restore_begin(void)
{
    LOCK();
    int ok=!checkpoint_restoring && intent_loaded && !intent_dirty && !intent.active &&
        !command && !file_status.busy && !stream.hz && !count;
    if(ok)checkpoint_restoring=1;
    UNLOCK();return ok;
}
void imu_checkpoint_restore_end(int changed)
{
    LOCK();
    if(changed){intent_loaded=0;intent_retry_at=0;}
    checkpoint_restoring=0;
    UNLOCK();
}
void imu_worker_step(void)
{
#ifndef ESP_PLATFORM
    hal_storage sd; hal_storage_get(&sd);
    if(!sd.mounted) { if(w && w->fp) finish("SD REMOVED / RESUME WAIT"); else publish("NO SD CARD / RESUME WAIT",HAL_IMU_ERROR); return; }
#endif
#ifdef ESP_PLATFORM
    static int leased;
    if(!leased) { if(!board_sd_asset_acquire()) { publish("NO SD CARD / RESUME WAIT",HAL_IMU_ERROR); return; } leased=1; }
    board_sd_asset_lock();
#endif
    worker_step();
#ifdef ESP_PLATFORM
    board_sd_asset_unlock();
    if(!w || !w->fp) { board_sd_asset_release(); leased=0; }
#endif
}
