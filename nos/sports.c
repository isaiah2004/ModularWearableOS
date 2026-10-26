#include "sports.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "psram.h"
#include "power_policy.h"
#ifdef ESP_PLATFORM
#include "diag_log.h"
#endif
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <unistd.h>
#include "sleep_grid.h"
#define ROOT "/sdcard/watch/sports"
#define MKDIR(p) mkdir(p,0777)
#define SPORTS_SYNC(fd) fsync(fd)
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t writer_task;
#define LOCK() portENTER_CRITICAL(&mux)
#define UNLOCK() portEXIT_CRITICAL(&mux)
#else
#include <direct.h>
#include <io.h>
#define ROOT "build/nos_sports"
#define MKDIR(p) _mkdir(p)
#define SPORTS_SYNC(fd) _commit(fd)
#define LOCK() ((void)0)
#define UNLOCK() ((void)0)
int hal_sports_sd_lock(void) { return 1; }
void hal_sports_sd_unlock(void) {}
#endif
#define QN 1024
#define RN 96
#define EN 128
#define DRAIN_BATCH 512
#define IMU_JOURNAL_MS 40u /* raw IMU journal decimation: ~25 Hz of the 112..150 Hz sample stream */
#define RAW_KEEP_DAYS 3    /* hourly *-sensors.spt files older than this many days are pruned */
#define PRUNE_FIRST_MS 60000u
#define PRUNE_EVERY_MS (6u*3600u*1000u)
#ifndef O_BINARY
#define O_BINARY 0
#endif
typedef struct {
    struct { uint32_t boot,seq; } id[DRAIN_BATCH];
    unsigned used,next;
} sports_replay_cache;
static sports_record *queue;
static unsigned qhead,qtail,rhead,rsize;
static int raw_type(int t) { return (t>=SP_IMU && t<=SP_GPS) || (t>=SP_PPG && t<=SP_TOUCH); }
static sports_status status;
/* Raw sensor journal gate (G11): raw types reach SD only during a workout or explicit
 * raw logging, never while the low-battery latch is set. Observers are not gated. */
static int raw_hold,raw_low;
static uint32_t imu_journal_ms;
static int raw_open(void) { return raw_hold && !raw_low; } /* call with the lock held */
static void (*sample_observer)(const sports_record *record);
static uint32_t boot,seq,clock_epoch,clock_day,clock_ms;
static int clock_minute,mode,ready,quiesced;
/* Keep journal history and persistent copies out of scarce internal DMA RAM. */
typedef struct {
    uint32_t token,day,last_intake,retained[EN],started;
    int start,end;
    long position,limit;
    sports_bin bins[24];
} sports_query_job;
typedef struct {
    sports_record history[RN];
    struct { uint32_t version, serial; sports_record entries[EN]; } food;
    struct { uint32_t version, serial; sports_record records[16]; } reports;
    sports_query_job requested,working;
    sports_replay_cache query_replay;
    sports_bin query_result[24];
    uint32_t query_done,query_failed;
} sports_storage;
static void query_step(void);
#ifdef ESP_PLATFORM
static sports_storage *s_journal;
#else
static sports_storage host_storage;
static sports_storage *s_journal=&host_storage;
#endif
#define recent (s_journal->history)
#define ledger (s_journal->food)
#define markers (s_journal->reports)
static uint32_t checksum(const sports_record *r)
{
    const unsigned char *p=(const unsigned char *)r;
    uint32_t h=2166136261u;
    for(unsigned i=0;i<sizeof(*r)-sizeof(r->check);i++) h=(h^p[i])*16777619u;
    return h;
}
static uint32_t monotonic(void)
{
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time()/1000);
#else
    return hal_millis();
#endif
}
uint32_t sports_day(int offset)
{
    hal_datetime d; hal_get_datetime(&d);
    struct tm t={0}; t.tm_year=d.year-1900; t.tm_mon=d.month-1;
    t.tm_mday=d.day+offset; t.tm_hour=12; t.tm_isdst=-1;
    if(mktime(&t)==(time_t)-1) return 0;
    return (uint32_t)((t.tm_year+1900)*10000+(t.tm_mon+1)*100+t.tm_mday);
}
static sports_record record(int type,const float *v,int n)
{
    sports_record r={0};
    r.magic=SPORTS_MAGIC; r.type=(uint16_t)type; r.uptime=monotonic();
    LOCK();
    r.boot=boot; r.seq=++seq; r.day=clock_day; r.minute=(uint16_t)clock_minute;
    r.epoch=clock_epoch ? clock_epoch+(r.uptime-clock_ms)/1000u : 0;
    UNLOCK();
    if(n>8)n=8;
    for(int i=0;i<n;i++)r.v[i]=v[i];
    r.check=checksum(&r); return r;
}
static int essential(const sports_record *r)
{
    switch(r->type) {
    case SP_BOOT: case SP_POWER: case SP_REPORT: case SP_LABEL: case SP_TUNE:
    case SP_FOOD: case SP_WATER: case SP_WAKE: case SP_LOCK: return 1;
    case SP_BATTERY: case SP_RAIL: return !r->v[3] || !r->v[4];
    case SP_ROUTINE: return r->v[0]<0 || r->v[2]<0;
    default: return 0;
    }
}
void sports_quiesce(int pause) { LOCK();quiesced=pause!=0;UNLOCK(); }
int sports_quiesced(void) { LOCK();int paused=quiesced;UNLOCK();return paused; }
static void enqueue(const sports_record *r)
{
    LOCK();
    int priority=essential(r);
    if(quiesced && !priority) {status.suppressed++;UNLOCK();return;}
    if(!priority && raw_type(r->type) && !raw_open()) {status.gated++;UNLOCK();return;}
    if(!raw_type(r->type)) { recent[rhead]=*r; rhead=(rhead+1)%RN; if(rsize<RN)rsize++; }
    unsigned limit=priority?QN:raw_type(r->type)?QN-128:QN-32;
    if(queue && qhead-qtail<limit) queue[qhead++%QN]=*r;
    else status.dropped++;
    UNLOCK();
}
static void emit(int type,const float *v,int n,int journal)
{
    LOCK();int initialized=ready;void (*observe)(const sports_record *)=sample_observer;UNLOCK();
    if(!initialized || (!journal && !observe))return;
    sports_record r=record(type,v,n); if(journal)enqueue(&r);
    if(observe)observe(&r);
}
void sports_emit(int type,const float *v,int n) { emit(type,v,n,1); }
void sports_observe(void (*observer)(const sports_record *record))
{ LOCK();sample_observer=observer;UNLOCK(); }
/* Observers get every sample; the journal only gets the gated, decimated stream. */
void sports_imu(const hal_imu *m)
{
    float v[]={m->ax,m->ay,m->az,m->gx,m->gy,m->gz};
    uint32_t t=monotonic();
    LOCK();
    int open=raw_open(),journal=open && (!imu_journal_ms || t-imu_journal_ms>=IMU_JOURNAL_MS);
    if(journal)imu_journal_ms=t;else if(!open)status.gated++;
    UNLOCK();
    emit(SP_IMU,v,6,journal);
}
void sports_raw_set(int source,int on)
{ LOCK();if(on)raw_hold|=source;else raw_hold&=~source;if(!raw_hold)imu_journal_ms=0;UNLOCK(); }
int sports_raw_active(void) { LOCK();int open=raw_open();UNLOCK();return open; }
int sports_raw_sources(void) { LOCK();int h=raw_hold;UNLOCK();return h; }
const char *sports_name(int t)
{
    static const char *const names[]={"?","BOOT","POWER","BATTERY","ACTIVITY","WALK",
        "KNOCK","WAKE","GESTURE","REPORT","LABEL","TUNE","FOOD","WATER","IMU",
        "CLIMATE","LIGHT","COMPASS","HEART","GPS","ROUTINE","PPG","RAIL","LIGHT RAW","MAG RAW","MIC","TOUCH","LOCK"};
    return t>0 && t<(int)(sizeof names/sizeof names[0])?names[t]:"?";
}
int sports_mode(void) { LOCK();int m=mode;UNLOCK();return m; }
const char *sports_field(int t,int f)
{
    static const char *const fields[][8]={
        [SP_POWER]={"mode","CPU MHz","brightness"},
        [SP_BATTERY]={"level %","rail mV","external","level valid","sample valid","mode","age ms"},
        [SP_ACTIVITY]={"steps","burn kcal","active min"},
        [SP_WALK]={"walking","candidates","valid","steps","filtered g","peak g","valley g"},
        [SP_KNOCK]={"reason","Z g","side g","interval ms","gyro dps"},
        [SP_WAKE]={"triggered","stage","reason","view dot","speed","learn result"},
        [SP_REPORT]={"feature","outcome","lookback sec"},
        [SP_LABEL]={"feature","trial active","expected"},
        [SP_TUNE]={"feature","walk g","knock g","entry peaks","quiet ms"},
        [SP_FOOD]={"kcal"},[SP_WATER]={"mL"},[SP_IMU]={"ax g","ay g","az g","gx dps","gy dps","gz dps"},
        [SP_ENV]={"valid","temp C","humidity %","hPa","altitude m"},
        [SP_LIGHT]={"valid","lux","proximity"},[SP_MAG]={"valid","heading","field uT","calibrated","progress"},
        [SP_HEART]={"on wrist","BPM","SpO2","quality"},[SP_PPG]={"red ADC","IR ADC"},
        [SP_GPS]={"fix valid","latitude","longitude","altitude m","speed m/s","HDOP","satellites"},
        [SP_ROUTINE]={"routine","state","result"},[SP_GESTURE]={"direction"}
        ,[SP_RAIL]={"level %","rail mV","external","level valid","sample valid"},
        [SP_LIGHT_RAW]={"clear ADC","red ADC","green ADC","blue ADC"},
        [SP_MAG_RAW]={"x ADC","y ADC","z ADC"},[SP_MIC]={"dB estimate","peak","envelope"},
        [SP_TOUCH]={"x","y","down"},[SP_LOCK]={"locked"}
    };
    if(t<0 || t>=(int)(sizeof fields/sizeof fields[0]) || f<0 || f>=8 || !fields[t][f])return "-";
    return fields[t][f];
}
void sports_power(int m,int mhz,int brightness)
{
    LOCK();mode=m;UNLOCK();float v[]={(float)m,(float)mhz,(float)brightness}; sports_emit(SP_POWER,v,3);
}
int sports_report(int feature,int outcome,int seconds)
{
    if(!ready)return 0;
    float v[]={(float)feature,(float)outcome,(float)seconds};
    sports_record r=record(SP_REPORT,v,3);unsigned slot=markers.serial%16;
    sports_record old=markers.records[slot];markers.records[slot]=r;markers.serial++;
    if(!hal_store_save_checked("sportsmarks",&markers,sizeof markers)) {
        markers.serial--;markers.records[slot]=old;return 0;
    }
    enqueue(&r);return 1;
}
int sports_recent(int offset,sports_record *out)
{
    int ok=0; LOCK();
    if(offset>=0 && (unsigned)offset<rsize) { *out=recent[(rhead+RN-1-(unsigned)offset)%RN]; ok=1; }
    UNLOCK(); return ok;
}
void sports_get_status(sports_status *out)
{ LOCK(); *out=status; out->queued=qhead-qtail; UNLOCK(); }
static void path_for(char *path,int n,uint32_t day,int raw)
{
    if(raw)snprintf(path,(size_t)n,ROOT "/%08lu-%02d-sensors.spt",(unsigned long)day,raw-1);
    else snprintf(path,(size_t)n,ROOT "/%08lu-events.spt",(unsigned long)day);
}
/* Creation is permitted only after confirmed absence. O_EXCL also protects a
 * file created between the read/update open and the create attempt. Append
 * mode cannot be used: the writer must overwrite an incomplete final record. */
static FILE *open_journal(const char *path)
{
    FILE *f=fopen(path,"r+b");
    if(f || errno!=ENOENT)return f;
    int fd=open(path,O_RDWR|O_CREAT|O_EXCL|O_BINARY,0664);
    if(fd<0)return errno==EEXIST?fopen(path,"r+b"):NULL;
    f=fdopen(fd,"w+b");
    if(!f) {int error=errno;close(fd);errno=error;}
    return f;
}
static int close_journal(FILE *f)
{
    int ok=1;
    if(fflush(f))ok=0;
    if(SPORTS_SYNC(fileno(f)))ok=0;
    if(fclose(f))ok=0;
    return ok;
}
/* Single SD owner at a time, also excludes board mount/eject. Close each batch so
 * media can be removed and a partial last record recovered on the next append. */
static void drain(void)
{
    LOCK();status.last_service_ms=monotonic();int idle=qhead==qtail && status.available;UNLOCK();
    if(idle)return; /* nothing queued: no SD lock, mkdir or FAT access (standby power) */
    if(!hal_sports_sd_lock()) { LOCK();status.available=0;UNLOCK();return; }
    MKDIR(ROOT);
    FILE *files[2]={0}; uint32_t dates[2]={0};unsigned taken=0;
    int failed=0;
    for(int i=0;i<DRAIN_BATCH;i++) {
        sports_record r; int have;
        LOCK(); have=qtail+taken!=qhead; if(have)r=queue[(qtail+taken)%QN]; UNLOCK();
        if(!have)break;
        int raw=raw_type(r.type);
        uint32_t filekey=raw?r.day*24u+r.minute/60u:r.day;
        if(files[raw] && dates[raw]!=filekey) {
            if(!close_journal(files[raw]))failed=1;
            files[raw]=0;
        }
        if(!files[raw] && !failed) {
            char path[100];path_for(path,sizeof path,r.day,raw?(r.minute/60+1):0);
            files[raw]=open_journal(path);
            if(files[raw]) {
                FILE *f=files[raw];dates[raw]=filekey;
                if(fseek(f,0,SEEK_END)!=0)failed=1;
                long size=ftell(f);if(size<0)failed=1;
                if(!failed && size%(long)sizeof r && fseek(f,size-size%(long)sizeof r,SEEK_SET)!=0)failed=1;
            } else failed=1;
        }
        if(failed || fwrite(&r,sizeof r,1,files[raw])!=1) {failed=1;break;}
        taken++;
    }
    for(int i=0;i<2;i++)if(files[i])if(!close_journal(files[i]))failed=1;
    LOCK();
    status.available=!failed;
    /* Failed batches may already be partly written; sequence IDs identify replays. */
    if(!failed) {qtail+=taken;status.written+=taken;if(taken)status.last_write_ms=monotonic();}else status.errors++;
    UNLOCK();
    hal_sports_sd_unlock();
}
/* Raw sensor retention: remove hourly sensors files from days before cutoff (yyyymmdd).
 * Event files are kept. Bounded per pass; the lease excludes mount/eject and the drain. */
int sports_prune_raw(uint32_t cutoff)
{
    if(!cutoff || !hal_sports_sd_lock())return 0;
    int removed=0;DIR *dir=opendir(ROOT);
    while(dir && removed<48) {
        struct dirent *e=readdir(dir);if(!e)break;
        unsigned long day;int hour;char tail[16];
        if(sscanf(e->d_name,"%8lu-%2d-%15s",&day,&hour,tail)!=3 || strcmp(tail,"sensors.spt") || day>=cutoff)continue;
        char path[100];snprintf(path,sizeof path,ROOT "/%.31s",e->d_name);
        if(!remove(path))removed++;
    }
    if(dir)closedir(dir);
    hal_sports_sd_unlock();return removed;
}
#ifdef ESP_PLATFORM
static void writer(void *arg)
{
    (void)arg;uint32_t last=0,prune=PRUNE_FIRST_MS;
    for(;;) {
        uint32_t now=monotonic();
        if(!last || now-last>=1000) {drain();last=now;}
        if((int32_t)(now-prune)>=0 && !sports_quiesced()) {
            LOCK();uint32_t today=clock_day;UNLOCK();
            if(today) {prune=now+PRUNE_EVERY_MS;sports_prune_raw(sports_day(-RAW_KEEP_DAYS));}
            else prune=now+10000; /* wall clock not set yet */
        }
        query_step();
        diag_log_service(); /* no sports SD lease is held across this call */
        LOCK();int pending=s_journal->requested.token!=s_journal->query_done;UNLOCK();
        if(pending)ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(20));
        else sleep_grid_notify_wait(1000); /* idle wake rides the shared 50 ms grid */
    }
}
#endif
void sports_flush(void)
{
    for(int i=0;i<3;i++) {drain();sports_status s;sports_get_status(&s);if(!s.available || !s.queued)break;}
}
void sports_init(void)
{
    if(ready)return;
#ifdef ESP_PLATFORM
    if(!s_journal)s_journal=heap_caps_calloc(1,sizeof(*s_journal),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!s_journal){status.errors++;return;}
    queue=heap_caps_calloc(QN,sizeof(*queue),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    boot=esp_random();
#else
    queue=hal_big_alloc(QN*sizeof(*queue)); boot=(uint32_t)time(NULL);
#endif
    if(!hal_store_load("sportsfood",&ledger,sizeof ledger)||ledger.version!=1) {
        memset(&ledger,0,sizeof ledger);ledger.version=1;
    }
    if(!hal_store_load("sportsmarks",&markers,sizeof markers)||markers.version!=1) {
        memset(&markers,0,sizeof markers);markers.version=1;
    }
    LOCK();ready=1;UNLOCK();
    sports_tick(); sports_emit(SP_BOOT,0,0);
    sports_power(SP_ACTIVE,240,nos_prefs.brightness);
    /* Replay retained intake records. Query uses their persistent IDs to dedupe. */
    for(int i=0;i<EN;i++)if(ledger.entries[i].magic==SPORTS_MAGIC)enqueue(&ledger.entries[i]);
    for(int i=0;i<16;i++)if(markers.records[i].magic==SPORTS_MAGIC)enqueue(&markers.records[i]);
#ifdef ESP_PLATFORM
    if(xTaskCreatePinnedToCoreWithCaps(writer,"sports_sd",4096,NULL,1,&writer_task,tskNO_AFFINITY,MALLOC_CAP_SPIRAM)!=pdPASS) { status.errors++; }
#endif
}
static int archived_entry(const sports_record *entry)
{
    if(entry->magic!=SPORTS_MAGIC)return 1;
    if(!hal_sports_sd_lock())return 0;
    char path[100];path_for(path,sizeof path,entry->day,0);FILE *f=fopen(path,"rb");int found=0;
    if(f) {
        sports_record r;
        while(fread(&r,sizeof r,1,f)==1)if(r.boot==entry->boot && r.seq==entry->seq && r.check==checksum(&r)) {found=1;break;}
        fclose(f);
    }
    hal_sports_sd_unlock();return found;
}
int sports_intake(int water,int amount)
{
    if(!ready || amount<=0 || amount>10000)return 0;
    unsigned slot=ledger.serial%EN;
    if(!archived_entry(&ledger.entries[slot]))return 0; /* never overwrite unexported intake */
    sports_record previous=ledger.entries[slot]; uint32_t old=ledger.serial;
    sports_record r=record(water?SP_WATER:SP_FOOD,0,0);
    r.boot=0; r.seq=++ledger.serial; r.v[0]=(float)amount; r.check=checksum(&r);
    ledger.entries[slot]=r;
    if(!hal_store_save_checked("sportsfood",&ledger,sizeof ledger)) {
        ledger.entries[slot]=previous;ledger.serial=old;return 0;
    }
    enqueue(&r);return 1;
}
int sports_undo(void)
{
    if(!ready || !ledger.serial)return 0;
    sports_record last=ledger.entries[(ledger.serial-1)%EN];
    if(last.v[0]<=0)return 0;
    unsigned slot=ledger.serial%EN; sports_record previous=ledger.entries[slot]; uint32_t old=ledger.serial;
    if(!archived_entry(&previous))return 0;
    sports_record r=last; r.seq=++ledger.serial; r.v[0]=-last.v[0];r.check=checksum(&r);
    ledger.entries[slot]=r;
    if(!hal_store_save_checked("sportsfood",&ledger,sizeof ledger)) {
        ledger.entries[slot]=previous;ledger.serial=old;return 0;
    }
    enqueue(&r);return 1;
}
void sports_tick(void)
{
#ifndef ESP_PLATFORM
    query_step();
#endif
    static uint32_t next,minute_at; static int had_minute;
    static int was_available;
    uint32_t now=monotonic();if(!ready || (next && now-next<1000))return;next=now;
    hal_datetime d;hal_get_datetime(&d);
    uint32_t epoch=(uint32_t)hal_epoch_seconds();
    hal_power pw;hal_power_get(&pw);
    LOCK();clock_epoch=epoch;clock_ms=now;clock_day=epoch?(uint32_t)(d.year*10000+d.month*100+d.day):0;
    clock_minute=d.hour*60+d.minute;raw_low=power_low_update(raw_low,&pw);UNLOCK();
    sports_status storage;sports_get_status(&storage);
    if(storage.available && !was_available) {
        for(int i=0;i<EN;i++)if(ledger.entries[i].magic==SPORTS_MAGIC)enqueue(&ledger.entries[i]);
        for(int i=0;i<16;i++)if(markers.records[i].magic==SPORTS_MAGIC)enqueue(&markers.records[i]);
    }
    was_available=storage.available;
    if(!had_minute || now-minute_at>=60000) {
        had_minute=1;minute_at=now;
        hal_power p=pw;
        float v[]={(float)p.battery_percent,(float)p.rail_mv,(float)p.external_power,
            (float)p.battery_valid,(float)p.sample_valid,(float)mode,(float)p.age_ms};
        sports_emit(SP_BATTERY,v,7);
        const svc_activity_t *a=svc_activity();
        float av[]={(float)a->steps,a->kcal,(float)a->active_min};sports_emit(SP_ACTIVITY,av,3);
    }
    /* Device drivers emit at acquisition; only simulate cached readings here. */
#ifndef ESP_PLATFORM
    hal_env e={0}; if(hal_env_get(&e)) { float v[]={(float)e.valid,e.temp_c,e.humidity,e.pressure_hpa,e.altitude_m};sports_emit(SP_ENV,v,5); }
    hal_light l={0};if(hal_light_get(&l)) { float v[]={(float)l.valid,l.lux,(float)l.prox};sports_emit(SP_LIGHT,v,3); }
    hal_mag m={0};if(hal_mag_get(&m)) { float v[]={(float)m.valid,m.heading_deg,m.field_ut,(float)m.calibrated,m.cal_progress};sports_emit(SP_MAG,v,5); }
    hal_hr h={0};if(hal_hr_get(&h)) { float v[]={(float)h.on_wrist,(float)h.bpm,(float)h.spo2,h.quality};sports_emit(SP_HEART,v,4); }
    hal_gps g={0};if(nos_prefs.gps_auto && hal_gps_get(&g)) { float v[]={(float)g.valid,(float)g.lat,(float)g.lon,g.alt_m,g.speed_mps,g.hdop,(float)g.age_ms};sports_emit(SP_GPS,v,7); }
    drain();
#endif
}
static void bin_record(const sports_record *r,int start,int end,sports_bin b[24])
{
    if(r->minute<start||r->minute>=end)return;
    int i=(r->minute-start)*24/(end-start);sports_bin *s=&b[i];
    if(r->type==SP_FOOD)s->food+=r->v[0];
    if(r->type==SP_WATER)s->water+=r->v[0];
    if(r->type==SP_ACTIVITY) { /* daily cumulative estimate: latest point in each bin */
        s->burn=r->v[1];
    }
    if(r->type==SP_BATTERY && r->v[4]>0) {
        if(r->v[3]>0) {s->battery+=r->v[0];s->samples++;}
        s->mv+=r->v[1];s->voltage_samples++;
        if(r->v[2]>0)s->external_samples++;
        if(r->v[5]>=0 && r->v[5]<=SP_AOD)s->modes|=1u<<(int)r->v[5];
    }
}
/* A failed drain retries at most DRAIN_BATCH queue identities before advancing.
 * Remember that many distinct identities, rather than assuming that producers
 * enqueue in sequence order (sensor/UI producers may interleave). Repeated
 * attempts do not evict identities. The older persistent intake replay stream
 * is merged separately below; retained REPORT replays do not affect totals. */
static int query_replay(sports_replay_cache *cache,const sports_record *r)
{
    for(unsigned i=0;i<cache->used;i++)if(cache->id[i].boot==r->boot && cache->id[i].seq==r->seq)return 1;
    cache->id[cache->next].boot=r->boot;cache->id[cache->next].seq=r->seq;
    cache->next=(cache->next+1)%DRAIN_BATCH;
    if(cache->used<DRAIN_BATCH)cache->used++;
    return 0;
}
uint32_t sports_query_request(uint32_t day,int start,int end)
{
    if(!ready || start<0 || end>1440 || start>=end)return 0;
#ifdef ESP_PLATFORM
    if(!writer_task)return 0;
#endif
    /* UI owns intake mutation. Capture its IDs and totals now; the worker never
     * reads that mutable ledger while doing filesystem work. */
    LOCK();
    sports_query_job *q=&s_journal->requested;
    uint32_t token=q->token+1;if(!token)token=1;
    memset(q,0,sizeof(*q));q->token=token;q->day=day;q->start=start;q->end=end;q->limit=-1;q->started=monotonic();
    for(int i=0;i<EN;i++)if(ledger.entries[i].magic==SPORTS_MAGIC) {
        q->retained[i]=ledger.entries[i].seq;
        if(ledger.entries[i].day==day)bin_record(&ledger.entries[i],start,end,q->bins);
    }
    UNLOCK();
#ifdef ESP_PLATFORM
    if(writer_task)xTaskNotifyGive(writer_task);
#endif
    return token;
}
int sports_query_poll(uint32_t token,sports_bin bins[24])
{
    if(!ready || !token)return -1;
    int result=0;LOCK();
    if(token!=s_journal->requested.token)result=-1;
    else if(token==s_journal->query_done) {
        if(token==s_journal->query_failed)result=-1;
        else {memcpy(bins,s_journal->query_result,sizeof(s_journal->query_result));result=1;}
    }
    UNLOCK();return result;
}
static void query_step(void)
{
    if(!ready)return;
    sports_query_job *q=&s_journal->working;
    int changed=0;
    LOCK();
    if(q->token!=s_journal->requested.token) {*q=s_journal->requested;changed=1;}
    int pending=q->token && q->token!=s_journal->query_done;
    UNLOCK();if(!pending)return;
    if(changed)memset(&s_journal->query_replay,0,sizeof(s_journal->query_replay));
    if(!hal_sports_sd_lock()) {
        if(monotonic()-q->started>=3000) {
            LOCK();if(q->token==s_journal->requested.token) {s_journal->query_done=q->token;s_journal->query_failed=q->token;}UNLOCK();
        }
        return;
    }
    char path[100];path_for(path,sizeof path,q->day,0);
    FILE *f=fopen(path,"rb");int done=!f,failed=!f && (errno!=ENOENT || q->position>0);
    if(f) {
        if(q->limit<0) {
            if(fseek(f,0,SEEK_END)!=0)done=failed=1;
            else {q->limit=ftell(f);if(q->limit<0)done=failed=1;}
        }
        if(!done && fseek(f,q->position,SEEK_SET)!=0)done=failed=1;
        for(int i=0;i<128 && !done;i++) {
            sports_record r;
            if(q->position+(long)sizeof(r)>q->limit) {done=1;break;}
            if(fread(&r,sizeof r,1,f)!=1) {done=failed=1;break;}
            q->position+=(long)sizeof r;
            if(r.magic!=SPORTS_MAGIC || r.day!=q->day || r.check!=checksum(&r))continue;
            if(r.type==SP_FOOD || r.type==SP_WATER) {
                int retained=0;for(int j=0;j<EN;j++)if(q->retained[j]==r.seq) {retained=1;break;}
                if(retained || r.seq<=q->last_intake)continue;
                q->last_intake=r.seq;
            } else {
                if(query_replay(&s_journal->query_replay,&r))continue;
            }
            bin_record(&r,q->start,q->end,q->bins);
        }
        if(fclose(f)!=0)done=failed=1;
    }
    hal_sports_sd_unlock();
    if(done) {
        for(int i=0;i<24;i++) {
            if(q->bins[i].samples)q->bins[i].battery/=q->bins[i].samples;
            if(q->bins[i].voltage_samples)q->bins[i].mv/=q->bins[i].voltage_samples;
        }
        LOCK();
        if(q->token==s_journal->requested.token) {
            memcpy(s_journal->query_result,q->bins,sizeof(s_journal->query_result));
            s_journal->query_done=q->token;
            if(failed) {s_journal->query_failed=q->token;status.errors++;}
        }
        UNLOCK();
    }
}
int sports_query(uint32_t day,int start,int end,sports_bin b[24])
{
    memset(b,0,sizeof(*b)*24);if(!ready || start<0||end>1440||end<=start)return 0;
    int count=0; uint32_t last_intake=0; /* records ordered by persisted entry ID */
    sports_replay_cache *replay=hal_big_alloc(sizeof(*replay));
    if(!replay)return 0;
    memset(replay,0,sizeof(*replay));
    if(hal_sports_sd_lock()) {
        char path[100];path_for(path,sizeof path,day,0);FILE *f=fopen(path,"rb");
        if(f) { sports_record r;
            while(fread(&r,sizeof r,1,f)==1) {
                if(r.magic!=SPORTS_MAGIC||r.check!=checksum(&r)||r.day!=day)continue;
                if(r.type==SP_FOOD||r.type==SP_WATER) {
                    /* Replayed records can appear after newer entries. Skip replay,
                     * merge NVS separately, using IDs, without double counting. */
                    int retained=0;for(int j=0;j<EN;j++)if(ledger.entries[j].seq==r.seq && ledger.entries[j].magic==SPORTS_MAGIC) { retained=1;break; }
                    if(retained || r.seq<=last_intake)continue;
                    last_intake=r.seq;
                } else {
                    if(query_replay(replay,&r))continue;
                }
                bin_record(&r,start,end,b);count++;
            }
            fclose(f);
        }hal_sports_sd_unlock();
    }
    hal_big_free(replay);
    for(int j=0;j<EN;j++) {
        sports_record *r=&ledger.entries[j];
        if(r->magic==SPORTS_MAGIC && r->day==day) {bin_record(r,start,end,b);count++;}
    }
    for(int i=0;i<24;i++) {
        if(b[i].samples)b[i].battery/=b[i].samples;
        if(b[i].voltage_samples)b[i].mv/=b[i].voltage_samples;
    }
    return count;
}

static int archived_record(uint32_t day,int offset,sports_record *out,int raw)
{
    if(offset<0 || !hal_sports_sd_lock())return 0;
    int ok=0;
    for(int hour=raw?23:0;hour>=0 && !ok;hour--) {
        char path[100];path_for(path,sizeof path,day,raw?hour+1:0);FILE *f=fopen(path,"rb");
        if(!f)continue;
        if(fseek(f,0,SEEK_END)==0) {
            long pos=ftell(f);
            if(pos>0) {
                pos-=pos%(long)sizeof(*out);
                /* Skip whole hours without scanning high-rate raw records. */
                long records=pos/(long)sizeof(*out);
                if(raw && offset>=records)offset-=(int)records;
                else {
                    if(raw) {pos-=(long)offset*(long)sizeof(*out);offset=0;}
                    while(pos>=(long)sizeof(*out)) {
                        pos-=(long)sizeof(*out);
                        if(fseek(f,pos,SEEK_SET)!=0 || fread(out,sizeof(*out),1,f)!=1)break;
                        if(out->magic!=SPORTS_MAGIC || out->check!=checksum(out))continue;
                        if(!offset--) {ok=1;break;}
                    }
                }
            }
        }
        fclose(f);
    }
    hal_sports_sd_unlock();return ok;
}
int sports_archived(uint32_t day,int offset,sports_record *out) {return archived_record(day,offset,out,0);}
int sports_raw(uint32_t day,int offset,sports_record *out) {return archived_record(day,offset,out,1);}
