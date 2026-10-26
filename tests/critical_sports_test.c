/* Independent boundary tests around the complete production journal. */
#include <io.h>
static int critical_sync(int fd);
#define _commit critical_sync
#define main original_sports_test_main
#include "sports_test.c"
#undef main
#undef _commit
static int sync_fails, sync_calls;
static int critical_sync(int fd)
{
    sync_calls++;
    if(sync_fails) {errno=EIO;return -1;}
    return _commit(fd);
}
int main(int argc,char **argv)
{
    assert(argc==2);_mkdir("build");sports_init();sports_flush();
    assert(qhead==qtail);
    if(!strcmp(argv[1],"sync")) {
        float v[]={40,3700,0,1,1,SP_ACTIVE,0};sports_emit(SP_BATTERY,v,7);
        unsigned pending=qhead-qtail;sync_fails=1;sports_flush();
        assert(sync_calls && qhead-qtail==pending && !status.available);
        sync_fails=0;sports_flush();assert(qhead==qtail && status.available);
        sports_bin bins[24];assert(sports_query(20261020,0,1440,bins));
        assert(bins[12].samples==2 && bins[12].battery==57.5f);
        puts("PASS sports failed fsync retains identity and retry deduplicates actual persisted records");
    } else if(!strcmp(argv[1],"rotation")) {
        float v[]={40,3700,0,1,1,SP_ACTIVE,0};sports_emit(SP_BATTERY,v,7);
        clock_day++;sports_emit(SP_BATTERY,v,7);
        unsigned pending=qhead-qtail;sync_fails=1;sports_flush();
        assert(qhead-qtail==pending && !status.available);
        sync_fails=0;sports_flush();assert(qhead==qtail && status.available);
        puts("PASS failed journal rotation fsync retains the complete cross-date batch for replay");
    } else if(!strcmp(argv[1],"reserve")) {
        float routine[]={1,1,0};
        for(int i=0;i<QN*2;i++)sports_emit(SP_ROUTINE,routine,3);
        unsigned before=qhead;float failed[]={-1,0,-1,0,0,SP_ACTIVE,0};
        sports_emit(SP_BATTERY,failed,7);sports_power(SP_DEEP_SLEEP,80,0);
        assert(qhead==before+2 && qhead-qtail<=QN);
        sports_flush();assert(qhead==qtail);
        puts("PASS sports routine saturation reserves battery-failure and shutdown transition evidence");
    } else {
#ifdef SPORTS_QUIESCE_API
        sports_quiesce(1);assert(sports_quiesced());
        unsigned before=qhead;hal_imu raw={0};float routine[]={1,1,0};
        for(int i=0;i<10000;i++){sports_imu(&raw);sports_emit(SP_ROUTINE,routine,3);}
        assert(qhead==before);
        sports_power(SP_DEEP_SLEEP,80,0);assert(sports_intake(0,90));
        sports_flush();assert(qhead==qtail && total(0)==90);
        test_ms+=60000;sports_tick();assert(qhead==qtail);
        sports_quiesce(0);assert(!sports_quiesced());sports_raw_set(SPORTS_RAW_MANUAL,1);sports_imu(&raw);
        assert(qhead==qtail+1);sports_flush();
        puts("PASS sports quiesce bounds routine producers, preserves durable intake/power and resumes on abort");
#else
        (void)critical_sync;
        assert(!"sports quiesce interface missing");
#endif
    }
    return 0;
}
