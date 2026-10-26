/* Real worker retries unknown intent without overwriting an unread bank. */
#ifdef __MINGW32__
#define __USE_MINGW_ANSI_STDIO 1
#endif
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <io.h>
static int fault_kind,opens;
static FILE *bad_stream;
static FILE *fault_open(const char *path,const char *mode)
{
    int intent_path=strstr(path,"intent.") && !strcmp(mode,"rb");
    if(intent_path)opens++;
    int selected=intent_path && (fault_kind==1 || strstr(path,"intent.1.bin"));
    if(selected && (fault_kind==1 || fault_kind==2)) {errno=EIO;return NULL;}
    FILE *f=fopen(path,mode);if(selected && fault_kind>=3)bad_stream=f;return f;
}
static size_t fault_read(void *p,size_t size,size_t n,FILE *f)
{if(f==bad_stream && fault_kind==3) {errno=EMFILE;return 0;}return fread(p,size,n,f);}
static int fault_error(FILE *f)
{return f==bad_stream && fault_kind==3?1:ferror(f);}
static int fault_close(FILE *f)
{
    int fail=f==bad_stream && fault_kind==4;if(f==bad_stream)bad_stream=NULL;
    int result=fclose(f);if(fail) {errno=EIO;return EOF;}return result;
}
#define fopen fault_open
#define fread fault_read
#define ferror fault_error
#define fclose fault_close
#define IMU_REC_TEST 1
#include "../nos/imu_rec.c"
#undef fopen
#undef fread
#undef ferror
#undef fclose
static uint32_t now;
static unsigned rate;
const char *hal_sd_root(void) {return "card";}
void *nos_mem_alloc(uint32_t n,const char *owner) {(void)owner;return calloc(1,n);}
void nos_mem_free(void *p) {free(p);}
void hal_storage_get(hal_storage *s) {memset(s,0,sizeof *s);s->mounted=1;}
uint32_t imu_backend_ms(void) {return now;}
uint64_t imu_backend_utc_ms(void) {return 1800000000000ULL+now;}
int imu_backend_wake(void) {return 1;}
int imu_backend_start(unsigned hz) {rate=hz;return 1;}
void imu_backend_stop(void) {rate=0;}
int imu_test_sync(FILE *f) {return _commit(_fileno(f));}
void imu_test_before_rollover_stop(void) {}
void diag_log_event(uint16_t code,int32_t a,int32_t b,int32_t c) {(void)code;(void)a;(void)b;(void)c;}
static void write_intent(int slot,int active,unsigned sequence)
{
    intent_record r={.magic=0x494d5532u,.seq=sequence,.active=(uint32_t)active,.hz=100,.file=5};r.sum=intent_sum(&r);
    char path[80];snprintf(path,sizeof path,"card/imu/intent.%d.bin",slot);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(&r,1,sizeof r,f)==sizeof r);assert(!fclose(f));
}
int main(int argc,char **argv)
{
    assert(argc==2);int kind=atoi(argv[1]);_mkdir("card");_mkdir("card/imu");
    if(kind==5) {
        imu_worker_step();assert(intent_loaded && !imu_worker_pending() && !intent_corrupt);
        puts("PASS IMU confirmed missing intent settles inactive");return 0;
    }
    write_intent(0,0,6);write_intent(1,1,7);
    fault_kind=kind==6?2:kind;
    imu_worker_step();
    assert(!intent_loaded && imu_worker_pending() && status.state==HAL_IMU_ERROR);
    assert(strstr(status.message,"READ FAILED / RETRY") && !rate);
    int count_opens=opens;now=500;imu_worker_step();assert(opens==count_opens); /* backoff */
    if(kind==6)assert(hal_imu_record_stop()); /* preserve queued STOP until the newest intent is known */
    fault_kind=0;now=1500;imu_worker_step();
    assert(intent_loaded);
    if(kind!=6) {
        assert(intent.active && rate==100 && status.state==HAL_IMU_RECORDING);
        assert(strstr(status.message,"RESUMED"));
        assert(hal_imu_record_stop());imu_worker_step();
    }
    assert(!intent.active && !intent_dirty && !imu_worker_pending() && status.state==HAL_IMU_IDLE && !rate);
    puts("PASS IMU read/open/close failure remains unknown; worker retries newest active intent and checked STOP");
    return 0;
}
