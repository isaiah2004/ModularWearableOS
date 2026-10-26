/* Real recorder harness; each invocation has fresh RAM and a persistent test SD. */
#include "imu_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <io.h>
static int fail_write, fail_sync;
size_t __real_fwrite(const void *,size_t,size_t,FILE *);
size_t __wrap_fwrite(const void *p,size_t size,size_t n,FILE *f)
{ if(fail_write && size*n>100) { __real_fwrite(p,1,size*n/2,f); errno=ENOSPC; return 0; } return __real_fwrite(p,size,n,f); }
int imu_test_sync(FILE *fp) { if(fail_sync) { errno=EIO; return -1; } return _commit(_fileno(fp)); }
void diag_log_event(uint16_t code,int32_t a,int32_t b,int32_t c) { (void)code; (void)a; (void)b; (void)c; }
static uint32_t now;
static unsigned rate;
static int mounted=1,inject_rollover;
void imu_test_before_rollover_stop(void)
{
    if(!inject_rollover || !rate)return;
    inject_rollover=0;now+=1000/rate;
    hal_imu v={.ax=.1f,.ay=.2f,.az=-1,.gx=3,.gy=4,.gz=5};
    imu_stream_push(&v,now,1800000000000ULL+now);
}
static const char *root;
void *nos_mem_alloc(uint32_t n,const char *owner) { (void)owner; return calloc(1,n); }
void nos_mem_free(void *p) { free(p); }
const char *hal_sd_root(void) { return root; }
void hal_storage_get(hal_storage *s) { memset(s,0,sizeof *s); s->mounted=mounted; }
uint32_t imu_backend_ms(void) { return now; }
uint64_t imu_backend_utc_ms(void) { return 1800000000000ULL+now; }
int imu_backend_wake(void) { return 1; }
int imu_backend_start(unsigned hz) { rate=hz; return 1; }
void imu_backend_stop(void) { rate=0; }
int main(int argc,char **argv)
{
    if(argc!=2) return 2;
    root=argv[1];
    char cmd[64]; unsigned arg;
    while(fgets(cmd,sizeof cmd,stdin)) {
        arg=0; sscanf(cmd,"%*s %u",&arg);
        if(!strncmp(cmd,"injectrollover",14)) inject_rollover=(int)arg;
        if(!strncmp(cmd,"failwrite",9)) fail_write=(int)arg;
        if(!strncmp(cmd,"failsync",8)) fail_sync=(int)arg;
        if(!strncmp(cmd,"start",5)) hal_imu_record_start(arg);
        if(!strncmp(cmd,"stop",4)) hal_imu_record_stop();
        if(!strncmp(cmd,"mounted",7)) mounted=(int)arg;
        if(!strncmp(cmd,"ticks",5)) {
            for(unsigned i=0;i<arg;i++) {
                now+=rate?1000/rate:20;
                if(rate) { hal_imu v={.ax=0.1f,.ay=0.2f,.az=-1,.gx=3,.gy=4,.gz=5}; imu_stream_push(&v,now,imu_backend_utc_ms()); }
                imu_worker_step();
            }
        } else imu_worker_step();
        hal_imu_record s; hal_imu_record_get(&s);
        printf("%d %u %u %llu %s | %s\n",s.state,s.hz,s.samples,(unsigned long long)s.bytes,s.name,s.message); fflush(stdout);
    }
    return 0;
}
