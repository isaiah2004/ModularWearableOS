#if defined(__MINGW32__)
#define __USE_MINGW_ANSI_STDIO 1
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <io.h>
static int fail_write,fail_prior,prior_reads;
static FILE *bad_prior,*bad_csv;
static FILE *fault_open(const char *path,const char *mode)
{
    if(!strcmp(path,"card/imu/0005.csv") && !strcmp(mode,"rb")) {
        prior_reads++;if(fail_prior==1){errno=EIO;return NULL;}
        FILE *f=fopen(path,mode);bad_prior=f;return f;
    }
    return fopen(path,mode);
}
static size_t fault_write(const void *p,size_t s,size_t n,FILE *f)
{if(fail_write || f==bad_csv){errno=ENOSPC;return 0;}return fwrite(p,s,n,f);}
static size_t fault_read(void *p,size_t s,size_t n,FILE *f)
{if(f==bad_prior && fail_prior==2){errno=EMFILE;return 0;}return fread(p,s,n,f);}
static int fault_error(FILE *f)
{return f==bad_prior && fail_prior==2?1:ferror(f);}
static int fault_close(FILE *f)
{
    int fail=f==bad_prior && fail_prior==3;
    if(f==bad_prior)bad_prior=NULL;
    int result=fclose(f);if(fail){errno=EIO;return EOF;}return result;
}
#define fopen fault_open
#define fwrite fault_write
#define fread fault_read
#define ferror fault_error
#define fclose fault_close
#define IMU_REC_TEST 1
#include "../nos/imu_rec.c"
#undef fopen
#undef fwrite
#undef fread
#undef ferror
#undef fclose
static uint32_t now;
static unsigned rate;
const char *hal_sd_root(void){return "card";}
void *nos_mem_alloc(uint32_t n,const char *owner){(void)owner;return calloc(1,n);}
void nos_mem_free(void *p){free(p);}
void hal_storage_get(hal_storage *s){memset(s,0,sizeof *s);s->mounted=1;}
uint32_t imu_backend_ms(void){return now;}
uint64_t imu_backend_utc_ms(void){return 1800000000000ULL+now;}
int imu_backend_wake(void){return 1;}
int imu_backend_start(unsigned hz){rate=hz;return 1;}
void imu_backend_stop(void){rate=0;}
int imu_test_sync(FILE *f){return _commit(_fileno(f));}
void imu_test_before_rollover_stop(void){}
void diag_log_event(uint16_t c,int32_t a,int32_t b,int32_t d){(void)c;(void)a;(void)b;(void)d;}
static void write_bank(int slot,int active,uint32_t sequence)
{
    intent_record r={.magic=0x494d5532u,.seq=sequence,.active=(uint32_t)active,.hz=100,.file=5};
    r.sum=intent_sum(&r);char p[80];snprintf(p,sizeof p,"card/imu/intent.%d.bin",slot);
    FILE *f=fopen(p,"wb");assert(f);assert(fwrite(&r,1,sizeof r,f)==sizeof r);assert(!fclose(f));
}
int main(int argc,char **argv)
{
    assert(argc==2);_mkdir("card");_mkdir("card/imu");
    if(!strcmp(argv[1],"bank")) {
        write_bank(0,1,7);write_bank(1,0,6);intent_load();assert(intent.active);
        intent.active=0;intent_dirty=1;fail_write=1;assert(!intent_save());fail_write=0;
        intent_load();assert(intent.active && intent.seq==7);
        puts("PASS displaced IMU generation survives failed alternate-bank STOP checkpoint");
    } else if(!strcmp(argv[1],"pending")) {
        imu_worker_step();assert(!imu_worker_pending());assert(hal_imu_files_request(0,0));
        assert(imu_worker_pending());imu_worker_step();assert(!imu_worker_pending());
        puts("PASS queued IMU file work remains a visible storage owner");
    } else if(!strcmp(argv[1],"restore")) {
        imu_worker_step();assert(!imu_worker_pending());assert(imu_checkpoint_restore_begin());
        assert(imu_worker_pending() && !hal_imu_stream_start(50) && !hal_imu_files_request(0,0));
        write_bank(0,1,7);imu_checkpoint_restore_end(1);assert(!intent_loaded && imu_worker_pending());
        imu_worker_step();assert(rate==100 && intent.active);
        assert(hal_imu_record_stop());imu_worker_step();assert(!imu_worker_pending());
        puts("PASS raw IMU intent restore excludes new owners and invalidates the boot cache before resume");
    } else if(!strcmp(argv[1],"stop-failure")) {
        write_bank(0,1,7);imu_worker_step();assert(rate==100 && w && w->fp);
        hal_imu sample={.az=1};imu_stream_push(&sample,50,1800000000050ULL);
        bad_csv=w->fp;assert(hal_imu_record_stop());imu_worker_step();bad_csv=NULL;
        assert(intent.active && imu_worker_pending() && status.state==HAL_IMU_ERROR);
        now=6000;imu_worker_step();assert(rate==100);
        assert(hal_imu_record_stop());imu_worker_step();assert(!intent.active && !imu_worker_pending());
        intent_loaded=0;imu_worker_step();assert(!rate && !intent.active && !imu_worker_pending());
        puts("PASS failed final CSV close/flush retains active intent; checked STOP suppresses restart across reload");
    } else {
        write_bank(0,0,6);write_bank(1,1,7);
        FILE *f=fopen("card/imu/0005.csv","wb");assert(f);
        char original[6026];memset(original,'x',6000);memcpy(original+6000,"header\n100,complete\n101,torn",26);
        assert(fwrite(original,1,sizeof original,f)==sizeof original);assert(!fclose(f));
        fail_prior=!strcmp(argv[1],"previous-read")?2:!strcmp(argv[1],"previous-close")?3:1;
        imu_worker_step();
        assert(prior_reads && intent.active && intent.file==5 && !rate && status.state==HAL_IMU_ERROR);
        assert(strstr(status.message,"PREVIOUS READ FAILED"));
        fail_prior=0;now=6000;imu_worker_step();assert(rate==100 && intent.file!=5);
        f=fopen("card/imu/0005.csv","rb");assert(f);char retained[sizeof original];
        assert(fread(retained,1,sizeof retained,f)==sizeof retained && !memcmp(original,retained,sizeof original));assert(!fclose(f));
        char link_path[80],metadata[512];snprintf(link_path,sizeof link_path,"card/imu/%04u.link",(unsigned)intent.file);
        f=fopen(link_path,"rb");assert(f);size_t n=fread(metadata,1,sizeof metadata-1,f);metadata[n]=0;assert(!fclose(f));
        assert(strstr(metadata,"previous_bytes=6026\n") && strstr(metadata,"previous_valid_bytes=6020\n"));
        assert(hal_imu_record_stop());imu_worker_step();assert(!imu_worker_pending());
        puts("PASS IMU previous CSV read I/O retains resume identity and retries without false missing metadata");
    }
    return 0;
}
