#define main sports_original_main
#include "sports_test.c"
#undef main
static unsigned observed;
static void capture(const sports_record *r)
{
    sports_status s;sports_get_status(&s); /* callback may safely query journal */
    assert(r->type==SP_IMU);observed++;
}
int main(void)
{
    _mkdir("build");sports_init();sports_flush();sports_observe(capture);
    hal_imu raw={.az=1};
    /* Closed raw gate: observers still see every sample, the journal sees none. */
    for(int i=0;i<QN*2;i++)sports_imu(&raw);
    sports_status s;sports_get_status(&s);
    assert(s.dropped==0&&s.queued==0&&s.gated==QN*2&&observed==QN*2);
    sports_raw_set(SPORTS_RAW_MANUAL,1);observed=0;
    for(int i=0;i<QN*2;i++)sports_emit(SP_IMU,(float[]){0,0,1,0,0,0},6);
    sports_get_status(&s);
    assert(s.dropped>0&&observed==QN*2);
    assert(s.last_service_ms==test_ms);
    test_ms+=5000;sports_flush();sports_get_status(&s);
    assert(s.last_service_ms==test_ms&&s.last_write_ms==test_ms&&s.queued==0);
    sports_observe(NULL);test_ms+=100;sports_imu(&raw);assert(observed==QN*2);
    puts("PASS background acquisition observer survives queue saturation; heartbeat and committed-write times come from actual drain");
    return 0;
}
