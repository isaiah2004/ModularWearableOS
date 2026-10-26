/* Run in an isolated working directory: the journal is written below build/. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
static int fail_update_open,fail_journal_close;
static FILE *fault_open(const char *path,const char *mode)
{
    if(fail_update_open && !strcmp(mode,"r+b")) {fail_update_open=0;errno=EIO;return NULL;}
    return fopen(path,mode);
}
static int fault_close(FILE *f)
{
    int result=fclose(f);
    if(fail_journal_close) {fail_journal_close--;errno=EIO;return EOF;}
    return result;
}
#define fopen fault_open
#define fclose fault_close
#include "../nos/sports.c"
#undef fopen
#undef fclose
#include "../nos/tuning.c"
nos_prefs_t nos_prefs;
static uint32_t test_ms=1000;
static hal_datetime test_dt={2026,10,20,2,12,0,0,0};
static int save_ok=1;
static unsigned char saved[sizeof ledger];
static svc_activity_t activity;
uint32_t hal_millis(void){return test_ms;}
uint64_t hal_epoch_seconds(void){return 1792497600u+test_ms/1000;}
void hal_get_datetime(hal_datetime *d){*d=test_dt;}
void *hal_big_alloc(uint32_t n){return calloc(1,n);}
int hal_store_load(const char *k,void *p,int n){(void)k;if(n==(int)sizeof ledger && saved[0]){memcpy(p,saved,n);return 1;}return 0;}
int hal_store_save_checked(const char *k,const void *p,int n){(void)k;if(!save_ok)return 0;if(n==(int)sizeof ledger)memcpy(saved,p,n);return 1;}
static hal_power test_power={4000,75,1,0,1,0};
void hal_power_get(hal_power *p){*p=test_power;}
const svc_activity_t *svc_activity(void){return &activity;}
int hal_env_get(hal_env *v){(void)v;return 0;}
int hal_light_get(hal_light *v){(void)v;return 0;}
int hal_mag_get(hal_mag *v){(void)v;return 0;}
int hal_hr_get(hal_hr *v){(void)v;return 0;}
int hal_gps_get(hal_gps *v){(void)v;return 0;}
unsigned observed_count;
void count_observed(const sports_record *rec){if(rec->type==SP_IMU)observed_count++;}
static float total(int water)
{
    sports_bin b[24];sports_query(20261020,0,1440,b);float sum=0;
    for(int i=0;i<24;i++)sum+=water?b[i].water:b[i].food;
    return sum;
}
int main(void)
{
    _mkdir("build");sports_init();
    assert(sizeof(sports_record)==64);
    assert(!sports_intake(0,0));assert(!sports_intake(1,10001));
    assert(sports_intake(0,500));assert(sports_intake(1,250));
    assert(total(0)==500 && total(1)==250);
    save_ok=0;assert(!sports_intake(0,100));assert(total(0)==500);save_ok=1;
    sports_flush();assert(total(0)==500 && total(1)==250);
    /* Replaying after reboot and re-querying must not double count entries. */
    for(int i=0;i<EN;i++)if(ledger.entries[i].magic)enqueue(&ledger.entries[i]);
    sports_flush();assert(total(0)==500 && total(1)==250);
    assert(sports_undo());assert(!sports_undo());sports_flush();assert(total(1)==0);
    sports_bin b[24];sports_query(20261020,0,720,b);
    for(int i=0;i<24;i++)assert(b[i].food==0);
    sports_query(20261020,720,721,b);assert(b[0].food==500);
    /* Partial write recovery: append over an incomplete tail. */
    char path[100];path_for(path,sizeof path,20261020,0);FILE *f=fopen(path,"ab");assert(f);fwrite("bad",3,1,f);fclose(f);
    assert(sports_intake(0,50));sports_flush();assert(total(0)==550);
    /* Corrupt complete records are ignored. */
    sports_record bad=record(SP_FOOD,0,0);bad.v[0]=999;f=fopen(path,"ab");fwrite(&bad,sizeof bad,1,f);fclose(f);assert(total(0)==550);
    /* Raw sensor gate (G11): closed by default, observers keep seeing every sample. */
    hal_imu imu={0};
    {
        unsigned seen=0,before=qhead;
        sports_observe(count_observed);
        assert(!sports_raw_active() && !sports_raw_sources());
        sports_imu(&imu);sports_imu(&imu);
        float one[]={1,2,3,4};sports_emit(SP_TOUCH,one,4);sports_emit(SP_PPG,one,2);sports_emit(SP_ENV,one,4);
        assert(qhead==before && status.gated==5);
        seen=observed_count;assert(seen==2); /* IMU observed while gated */
        float rail_bad[]={0,0,-1,0,0,SP_ACTIVE};sports_emit(SP_RAIL,rail_bad,6);
        assert(qhead==before+1); /* failed power reading is essential evidence, never gated */
        float rail_ok[]={50,3700,0,1,1,SP_ACTIVE};sports_emit(SP_RAIL,rail_ok,6);assert(qhead==before+1);
        sports_emit(SP_LOCK,one,1);assert(qhead==before+2); /* non-raw events unaffected */
        /* Workout or explicit hold opens it; IMU is decimated to one record per IMU_JOURNAL_MS. */
        sports_raw_set(SPORTS_RAW_WORKOUT,1);assert(sports_raw_active() && sports_raw_sources()==1);
        before=qhead;sports_imu(&imu);test_ms+=IMU_JOURNAL_MS-1;sports_imu(&imu);assert(qhead==before+1);
        test_ms+=1;sports_imu(&imu);assert(qhead==before+2);
        sports_emit(SP_TOUCH,one,4);assert(qhead==before+3);
        sports_raw_set(SPORTS_RAW_MANUAL,1);sports_raw_set(SPORTS_RAW_WORKOUT,0);assert(sports_raw_active() && sports_raw_sources()==2);
        /* Low battery closes the gate for every source and reopens at the policy's clear level. */
        test_power.battery_percent=15;test_power.external_power=0;test_ms+=1000;sports_tick();
        assert(raw_low && !sports_raw_active());before=qhead;test_ms+=100;sports_imu(&imu);assert(qhead==before);
        test_power.battery_percent=19;test_ms+=1000;sports_tick();assert(raw_low);
        test_power.battery_percent=20;test_ms+=1000;sports_tick();assert(!raw_low && sports_raw_active());
        test_power.battery_percent=75;
        sports_raw_set(SPORTS_RAW_MANUAL,0);assert(!sports_raw_active() && !imu_journal_ms);
        sports_observe(NULL);
        while(qhead!=qtail)sports_flush();
    }
    /* Retention removes only hourly sensors files from days before the cutoff. */
    {
        const char *names[]={"20261010-03-sensors.spt","20261018-23-sensors.spt","20261019-00-sensors.spt",
            "20261020-05-sensors.spt","20261010-events.spt"};
        for(int i=0;i<5;i++) {
            char fp[120];snprintf(fp,sizeof fp,ROOT "/%s",names[i]);FILE *pf=fopen(fp,"wb");assert(pf);fputs("x",pf);fclose(pf);
        }
        assert(!sports_prune_raw(0));
        assert(sports_prune_raw(20261019)==2);
        for(int i=0;i<5;i++) {
            char fp[120];snprintf(fp,sizeof fp,ROOT "/%s",names[i]);FILE *pf=fopen(fp,"rb");
            assert((pf!=NULL)==(i>=2));if(pf)fclose(pf);
        }
        assert(sports_prune_raw(20261019)==0);
    }
    /* Raw overload leaves event queue headroom and reports losses. */
    sports_raw_set(SPORTS_RAW_MANUAL,1);
    for(int i=0;i<QN*2;i++)sports_emit(SP_IMU,(float[]){0,0,0,0,0,0},6);
    unsigned before=qhead;sports_report(2,0,30);assert(qhead==before+1);assert(status.dropped>0);
    while(qhead!=qtail)sports_flush();
    sports_record r;assert(sports_archived(20261020,0,&r));assert(r.type==SP_REPORT);
    test_dt.day=21;test_ms+=1000;sports_tick();assert(sports_intake(1,400));assert(total(1)==0);
    /* External power retains measured rail, never creates a valid battery %. */
    float p[]={90,4300,1,0,1,SP_SENSOR,0};sports_emit(SP_BATTERY,p,7);sports_flush();
    sports_query(20261021,0,1440,b);assert(b[12].voltage_samples==1 && b[12].samples==0 && b[12].mv==4300);
    sports_tuning tc;
    tuning_init();assert(tuning_adjust(0,-1));tuning_get(&tc);assert(fabsf(tc.walk_g-.09f)<.0001f);
    save_ok=0;assert(!tuning_adjust(0,-1));tuning_get(&tc);assert(fabsf(tc.walk_g-.09f)<.0001f);save_ok=1;
    for(int i=0;i<30;i++)assert(tuning_adjust(2,-1));
    tuning_get(&tc);assert(tc.walk_peaks==4);
    assert(tuning_adjust(0,0));tuning_get(&tc);assert(tc.walk_peaks==8 && tc.quiet_ms==250);
    sports_imu(&imu);sports_flush();
    test_dt.hour=13;test_ms+=1000;sports_tick();imu.ax=2;sports_imu(&imu);sports_flush();
    assert(sports_raw(20261021,0,&r));assert(r.type==SP_IMU && r.v[0]==2);
    assert(sports_raw(20261021,1,&r));assert(r.type==SP_IMU && r.v[0]==0);
    /* History UI requests never scan files; worker batches a fixed 128 records,
     * publishes atomically, and ignores a cancelled request. */
    uint32_t token=sports_query_request(20261020,0,1440);
    memset(b,0x5a,sizeof b);assert(!sports_query_poll(token,b));
    assert(s_journal->working.token!=token);
    unsigned passes=0;while(!sports_query_poll(token,b)) {query_step();assert(++passes<100);}
    assert(sports_query_poll(token,b)==1);
    float food=0;for(int i=0;i<24;i++)food+=b[i].food;assert(food==550);
    token=sports_query_request(20261020,0,1440);query_step();
    uint32_t newer=sports_query_request(20261021,0,1440);
    assert(sports_query_poll(token,b)==-1);
    passes=0;while(!sports_query_poll(newer,b)) {query_step();assert(++passes<100);}
    assert(sports_query_poll(newer,b)==1 && b[12].mv==4300 && !b[12].samples);
    /* A day with many valid irrelevant events needs multiple bounded batches. */
    for(int i=0;i<400;i++)sports_emit(SP_LOCK,0,0);
    sports_flush();
    token=sports_query_request(20261021,0,1440);query_step();
    assert(sports_query_poll(token,b)==0);
    assert(s_journal->working.position==128*(long)sizeof(sports_record));
    passes=0;while(!sports_query_poll(token,b)) {query_step();assert(++passes<100);}
    assert(sports_query_poll(token,b)==1 && b[12].mv==4300);
    /* An update-open I/O error must not create/truncate an existing journal. */
    sports_record bat={.magic=SPORTS_MAGIC,.boot=123,.seq=10,.day=20261022,.minute=720,.type=SP_BATTERY,
        .v={80,4000,0,1,1,SP_ACTIVE,0}};bat.check=checksum(&bat);
    path_for(path,sizeof path,bat.day,0);f=fopen(path,"wb");assert(f);assert(fwrite(&bat,sizeof bat,1,f)==1);assert(!fclose(f));
    bat.seq=11;bat.v[0]=20;bat.v[1]=3600;bat.check=checksum(&bat);enqueue(&bat);
    unsigned saved_tail=qtail;fail_update_open=1;drain();
    assert(qtail==saved_tail && !status.available);
    f=fopen(path,"rb");assert(f);assert(fread(&r,sizeof r,1,f)==1 && r.seq==10);assert(fgetc(f)==EOF);assert(!fclose(f));
    drain();assert(status.available && qtail==qhead);
    /* A failed close can append a complete sample, then replay its identity. */
    bat.seq=12;bat.v[0]=50;bat.v[1]=3800;bat.check=checksum(&bat);enqueue(&bat);
    saved_tail=qtail;fail_journal_close=1;drain();assert(qtail==saved_tail && !status.available);
    drain();assert(status.available && qtail==qhead);
    /* Independent producers may enqueue a lower sequence after a higher one. */
    bat.seq=9;bat.v[0]=40;bat.v[1]=3700;bat.check=checksum(&bat);enqueue(&bat);drain();
    sports_query(20261022,0,1440,b);
    assert(b[12].samples==4 && b[12].voltage_samples==4 && b[12].battery==47.5f && b[12].mv==3775);
    token=sports_query_request(20261022,0,1440);passes=0;
    while(!sports_query_poll(token,b)) {query_step();assert(++passes<100);}
    assert(sports_query_poll(token,b)==1 && b[12].samples==4 && b[12].battery==47.5f && b[12].mv==3775);
    /* The replay window covers an entire failed batch across worker slices. */
    for(int i=0;i<DRAIN_BATCH;i++) {
        sports_record item=bat;item.day=20261023;item.seq=(uint32_t)i+1;
        item.type=(i==0 || i==DRAIN_BATCH-1)?SP_BATTERY:SP_LOCK;
        item.v[0]=i==0?80:20;item.check=checksum(&item);enqueue(&item);
    }
    fail_journal_close=1;drain();assert(!status.available);
    fail_journal_close=1;drain();assert(!status.available);
    drain();assert(status.available && qtail==qhead);
    sports_query(20261023,0,1440,b);assert(b[12].samples==2 && b[12].battery==50);
    token=sports_query_request(20261023,0,1440);passes=0;
    while(!sports_query_poll(token,b)) {query_step();assert(++passes<100);}
    assert(sports_query_poll(token,b)==1 && b[12].samples==2 && b[12].battery==50);
    puts("PASS sports: checked persistence, undo, replay deduplication, minute slicing, partial/corrupt records, queue loss, day rollover, USB validity");
    return 0;
}
