"""Execute actual board lease/unmount and transfer dispatch bodies with mutex mocks."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
CHECKS=ROOT/"build/alpha05/checks/platform-board/leases"
def function(source,name):
    match=re.search(r"(?:int|void|esp_err_t|board_log_snapshot) "+name+r"\([^)]*\)\s*\{",source)
    if not match:raise AssertionError(name)
    start=source.index("{",match.start());level=1;end=start+1
    while level:
        level+=(source[end]=="{")-(source[end]=="}");end+=1
    return source[match.start():end]
@unittest.skipUnless(shutil.which("gcc"),"gcc required")
class SdAssetLeaseTests(unittest.TestCase):
    def test_mount_lifetime_and_all_transfer_dispatch_returns(self):
        board=(ROOT/"firmware/main/board.c").read_text()
        transfer=(ROOT/"firmware/main/sd_xfer.c").read_text()
        prefix=r"""
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#define ESP_PLATFORM 1
typedef int esp_err_t;
#define pdTRUE 1
#define portMAX_DELAY 999
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 2
#define HAL_IMU_IDLE 0
#define HAL_IMU_ERROR 1
#define HAL_IMU_RECORDING 2
#define HAL_IMU_STOPPING 3
#define HAL_IMU_STARTING 4
#define SD_ROOT "mock"
typedef struct {int state;char name[48];} hal_imu_record;
typedef struct {int busy;} hal_imu_file;
static int s_sd_lock,held,s_stopping,unmounts,denied,nomem,puts,reads,gets2,transfer_busy,recovery_failed;
static int s_sd_teardown_error,unmount_error,record_state,intent_pending,removes;
static int external_held,external_deny,external_claims,external_ends;
static int transfer_mutation_owned;
static char last_reply[192];
static unsigned s_asset_users;
static void *s_card=(void*)1;
static char s_log_path[96];
static int xSemaphoreTake(int sem,int ticks){(void)sem;if(held){assert(!ticks);return 0;}held=1;return 1;}
static void xSemaphoreGive(int sem){(void)sem;assert(held);held=0;}
static void hal_imu_record_get(hal_imu_record *r){r->state=record_state;snprintf(r->name,sizeof r->name,"0001.csv");}
static void hal_imu_file_get(hal_imu_file *r){r->busy=0;}
static int imu_worker_pending(void){return intent_pending;}
static int esp_vfs_fat_sdcard_unmount(const char *root,void *card){(void)root;assert(card&&held&&!s_asset_users);unmounts++;return unmount_error;}
static void storage_status(const char *s){(void)s;}
int ota_external_activity_begin(void){external_claims++;if(external_deny)return 0;assert(!external_held);external_held=1;return 1;}
void ota_external_activity_end(void){assert(external_held);external_held=0;external_ends++;}
"""
        bodies="\n".join(function(board,n) for n in ("board_sd_asset_acquire","board_sd_asset_release","board_sd_asset_lock","board_sd_asset_unlock","board_sd_unmount"))
        logger_stubs=r"""
typedef struct {float ax,ay,az,gx,gy,gz;} hal_imu;
static int unique_file(char *p,size_t n,const char *d,const char *e){(void)p;(void)n;(void)d;(void)e;return -1;}
static int board_imu_read(hal_imu *m){memset(m,0,sizeof *m);return 0;}
static int hal_battery_mv(void){return 0;}
static long long esp_timer_get_time(void){return 0;}
const char *nos_power_state_name(void){return "off";}
int nos_power_brightness(void){return 0;}
int nos_power_cpu_mhz(void){return 80;}
void nos_power_sleep_stats(uint32_t *sleep_ms,uint32_t *entries){*sleep_ms=0;*entries=0;}
int hal_wifi_log_state(void){return 0;}
int hal_external_power(void){return 0;}
"""
        logger_type=re.search(r"typedef struct \{[^}]+\} board_log_snapshot;",board)[0]
        logger=logger_type+function(board,"log_snapshot")+function(board,"log_sample")
        stubs=r"""
typedef struct {int(*read)(uint8_t*,int,int);void(*line)(const char*);int block;} xfer_io;
static void reply(const xfer_io *io,const char *format,...){(void)io;va_list args;va_start(args,format);vsnprintf(last_reply,sizeof last_reply,format,args);va_end(args);}
static void *nos_mem_alloc(uint32_t bytes,const char *name){(void)name;return nomem?NULL:malloc(bytes);}
static void nos_mem_free(void *p){free(p);}
static const char *hal_sd_root(void){return ".";}
static void do_put(const xfer_io *io,const char *rel,long n,uint8_t *buf,int restore){(void)rel;(void)n;(void)restore;assert(s_asset_users&&!held);puts++;io->read(buf,1,1);}
static void do_get(const xfer_io *io,const char *path,uint8_t *buf){(void)io;(void)path;(void)buf;assert(s_asset_users&&!held);}
static void do_get2(const xfer_io *io,const char *path,uint8_t *buf){(void)io;(void)path;(void)buf;assert(s_asset_users&&!held);gets2++;}
static int file_crc(FILE *file,uint8_t *buf,long *n,uint32_t *crc){(void)file;(void)buf;*n=0;*crc=0;return 1;}
static int transfer_claim(void){assert(s_asset_users&&!held);return !transfer_busy;}
static void transfer_release(void){assert(s_asset_users&&!held);transfer_mutation_owned=0;}
static int recover_transaction(uint8_t *buf){(void)buf;assert(s_asset_users&&!held);return !recovery_failed;}
static int path_state(const char *path){struct stat st;assert(s_asset_users&&!held);return stat(path,&st)==0;}
static FILE *read_open(const char *path,int *error){FILE *f=fopen(path,"rb");*error=f?0:errno;return f;}
static int close_file(FILE *f){return fclose(f);}
static int remove_file(const char *path){assert(s_asset_users&&!held);removes++;return remove(path);}
#define XFER_BUF 8192
static int read_transport(uint8_t *b,int n,int timeout){(void)b;(void)n;(void)timeout;assert(s_asset_users&&!held);reads++;return 0;}
static void line_transport(const char *line){(void)line;}
"""
        main=r"""
int main(void){
 board_log_snapshot snapshot=log_snapshot();
 board_sd_asset_lock();assert(!log_sample(&snapshot));board_sd_asset_unlock();
 assert(board_sd_asset_acquire());assert(!log_sample(&snapshot));board_sd_asset_release();
 assert(log_sample(&snapshot)); /* real failed attempt consumes ordinary minute cadence */
 assert(board_sd_asset_acquire()&&s_asset_users==1&&!held);
 assert(board_sd_unmount()==ESP_ERR_INVALID_STATE&&!unmounts);
 board_sd_asset_lock();assert(board_sd_unmount()==ESP_ERR_INVALID_STATE&&held);board_sd_asset_unlock();
 board_sd_asset_release();assert(!s_asset_users);assert(board_sd_unmount()==0&&unmounts==1&&!s_card);
 assert(!board_sd_asset_acquire());s_card=(void*)1;s_stopping=1;assert(!board_sd_asset_acquire());s_stopping=0;
 const xfer_io io={read_transport,line_transport,2048};
 assert(!sd_xfer_exec("other",&io)&&!s_asset_users);
 const char *commands[]={"sdput missing 1","sdget missing","sdget2 missing","sdget2","sdget2 ../bad","sdcrc missing","sdls missing","sdrm missing","sdget ../bad"};
 for(unsigned i=0;i<sizeof commands/sizeof *commands;i++){assert(sd_xfer_exec(commands[i],&io));assert(!s_asset_users&&!held&&!external_held);}
 assert(puts==1&&reads==1&&gets2==1);
 transfer_busy=1;assert(sd_xfer_exec("sdget2 missing",&io)&&!s_asset_users&&!held);transfer_busy=0;
 recovery_failed=1;assert(sd_xfer_exec("sdget2 missing",&io)&&!s_asset_users&&!held);recovery_failed=0;
 nomem=1;assert(sd_xfer_exec("sdget2 missing",&io)&&!s_asset_users&&!held);nomem=0;
 int removed=removes;
 assert(sd_xfer_exec("sdrm /WATCH/settings.0.bin",&io)&&strstr(last_reply,"protected")&&removes==removed);
 record_state=HAL_IMU_RECORDING;
 assert(sd_xfer_exec("sdrm IMU/0001.csv",&io)&&strstr(last_reply,"active")&&removes==removed);record_state=0;
 intent_pending=1;assert(sd_xfer_exec("sdrm imu/unknown.csv",&io)&&strstr(last_reply,"active")&&removes==removed);intent_pending=0;
 assert(!s_asset_users&&!held&&!external_held);
#ifdef TEST_EXTERNAL_MUTATION
 external_deny=1;assert(sd_xfer_exec("sdput missing 1",&io)&&strstr(last_reply,"busy")&&!s_asset_users&&!held&&!external_held&&puts==1);
 external_deny=0;assert(external_claims==external_ends+1);
#endif
 s_stopping=1;assert(sd_xfer_exec("sdput missing 1",&io)&&!s_asset_users);assert(puts==1);
 s_stopping=0;unmount_error=3;assert(board_sd_unmount()==3&&!s_card&&s_sd_teardown_error==3);
 int closed=unmounts;assert(board_sd_unmount()==3&&unmounts==closed&&!board_sd_asset_acquire());
 assert(!held&&!s_asset_users&&!external_held);
 printf("PASS current board snapshot/teardown, mount leases, protected/live guards and all transfer return paths\n");
 return denied;}
"""
        # Avoid libc puts name in the deliberately tiny mock translation unit.
        helpers="\n".join(function(transfer,n) for n in ("full_path","path_prefix","path_equal","protected_path","live_path","mutation_begin","mutation_end"))
        dispatch=function(transfer,"sd_xfer_exec")
        if "mutation_begin" in dispatch:prefix+="\n#define TEST_EXTERNAL_MUTATION 1\n"
        text=(prefix+bodies+logger_stubs+logger+stubs+helpers+dispatch+main).replace("puts","put_calls")
        CHECKS.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="nos-lease-",dir=CHECKS) as tmp:
            source=Path(tmp)/"test.c";source.write_text(text)
            exe=Path(tmp)/("test.exe" if os.name=="nt" else "test")
            result=subprocess.run(["gcc","-O2","-std=c11","-Wall","-Wextra","-Werror","-Wno-format-truncation",str(source),"-o",str(exe)],capture_output=True,text=True)
            (CHECKS/"lease.compile.log").write_text(result.stdout+result.stderr)
            (CHECKS/"lease.c").write_text(text)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],cwd=tmp,capture_output=True,text=True)
            (CHECKS/"lease.log").write_text(result.stdout+result.stderr)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
if __name__=="__main__":unittest.main()
