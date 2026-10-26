/* The Python runner inserts the complete production transfer module below,
 * replacing only hardware includes with these platform/transport mocks. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <direct.h>
#include <sys/stat.h>
#include <unistd.h>
#include "hal.h"
static int held,leases,rename_calls,rename_fail_mask,remove_fail,fault_errno,read_fault,reads,late_live;
static FILE *bad_stream;
static hal_imu_record live_record;
static int pending_intent;
static const unsigned char *input_data;
static size_t input_size,input_offset;
static char response[256];
static char ack_text[64];
static size_t ack_offset;
static int ack_mode,frames,retry_frame,append_on_frame;
static unsigned long download_size;
int board_sd_asset_acquire(void) {leases++;return 1;}
void board_sd_asset_release(void) {assert(leases && !held);leases--;}
void board_sd_asset_lock(void) {assert(!held);held=1;}
void board_sd_asset_unlock(void) {assert(held);held=0;}
const char *hal_sd_root(void) {return "card";}
void hal_imu_record_get(hal_imu_record *r) {*r=live_record;}
int imu_worker_pending(void) {return pending_intent;}
void *nos_mem_alloc(uint32_t n,const char *owner) {(void)owner;return calloc(1,n);}
void nos_mem_free(void *p) {free(p);}
static int64_t esp_timer_get_time(void) {static int64_t ticks;return ++ticks;}
#define pdMS_TO_TICKS(n) (n)
static int usb_serial_jtag_read_bytes(void *p,uint32_t n,int ticks) {(void)p;(void)n;(void)ticks;return 0;}
static uint32_t esp_rom_crc32_le(uint32_t initial,const uint8_t *p,uint32_t n)
{
    uint32_t crc=~initial;
    for(uint32_t i=0;i<n;i++) {crc^=p[i];for(int j=0;j<8;j++)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);}
    return ~crc;
}
static int mbedtls_base64_encode(unsigned char *out,size_t cap,size_t *used,const unsigned char *in,size_t n)
{
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need=(n+2)/3*4;if(cap<=need)return -1;size_t at=0;
    for(size_t i=0;i<n;i+=3) {
        uint32_t value=(uint32_t)in[i]<<16;if(i+1<n)value|=(uint32_t)in[i+1]<<8;if(i+2<n)value|=in[i+2];
        out[at++]=(unsigned char)alphabet[(value>>18)&63];out[at++]=(unsigned char)alphabet[(value>>12)&63];
        out[at++]=i+1<n?(unsigned char)alphabet[(value>>6)&63]:'=';out[at++]=i+2<n?(unsigned char)alphabet[value&63]:'=';
    }
    out[at]=0;*used=at;return 0;
}
static FILE *fault_open(const char *path,const char *mode)
{
    assert(held);
    int selected=!strcmp(path,"card/original.dat") && !strcmp(mode,"rb");
    if(selected && fault_errno) {errno=fault_errno;return NULL;}
    FILE *f=fopen(path,mode);if(selected && read_fault)bad_stream=f;return f;
}
static size_t fault_read(void *p,size_t size,size_t n,FILE *f)
{assert(held);if(f==bad_stream && read_fault) {errno=ENOMEM;return 0;}return fread(p,size,n,f);}
static int fault_error(FILE *f) {assert(held);return f==bad_stream && read_fault?1:ferror(f);}
static int fault_close(FILE *f) {assert(held);if(f==bad_stream)bad_stream=NULL;return fclose(f);}
static int fault_rename(const char *a,const char *b)
{
    assert(held);rename_calls++;
    if(rename_fail_mask&(1<<rename_calls)) {errno=EIO;return -1;}return rename(a,b);
}
static int fault_remove(const char *p)
{assert(held);if(remove_fail && strstr(p,".sdxfer-old")) {remove_fail=0;errno=EIO;return -1;}return remove(p);}
#define fopen fault_open
#define fread fault_read
#define ferror fault_error
#define fclose fault_close
#define rename fault_rename
#define remove fault_remove
#define mkdir(path,mode) _mkdir(path)
/* @SD_XFER_SOURCE@ */
#undef fopen
#undef fread
#undef ferror
#undef fclose
#undef rename
#undef remove
#undef mkdir
static int receive(uint8_t *bytes,int n,int timeout)
{
    (void)timeout;assert(leases && !held);reads++;
    if(ack_mode) {
        size_t available=strlen(ack_text)-ack_offset;if(available>(size_t)n)available=(size_t)n;
        if(!available)return 0;
        memcpy(bytes,ack_text+ack_offset,available);ack_offset+=available;return (int)available;
    }
    if(input_data) {
        size_t count=input_size-input_offset;if(count>(size_t)n)count=(size_t)n;
        if(!count)return 0;
        memcpy(bytes,input_data+input_offset,count);input_offset+=count;n=(int)count;
    } else memset(bytes,'N',(size_t)n);
    if(late_live) {live_record.state=HAL_IMU_RECORDING;strcpy(live_record.name,"0001.csv");}
    return n;
}
static void capture(const char *line)
{
    assert(!held);snprintf(response,sizeof response,"%s",line);
    if(ack_mode && !strncmp(line,"SDG2 ",5)) {
        assert(strlen(line)<1536);unsigned long offset,crc;assert(sscanf(line,"SDG2 %lu %lx",&offset,&crc)==2);(void)crc;
        unsigned long next=offset+GET2_CHUNK;if(next>download_size)next=download_size;
        snprintf(ack_text,sizeof ack_text,retry_frame?"SGR %lu\n":"SGA %lu\n",retry_frame?offset:next);
        retry_frame=0;ack_offset=0;frames++;
        if(append_on_frame) {
            append_on_frame=0;unsigned char suffix[500];memset(suffix,0xcc,sizeof suffix);
            board_sd_asset_lock();FILE *f=fopen("card/original.dat","ab");assert(f);
            assert(fwrite(suffix,1,sizeof suffix,f)==sizeof suffix);assert(!fclose(f));board_sd_asset_unlock();
        }
    }
}
static void command(const char *line)
{
    response[0]=0;const xfer_io io={receive,capture,2048};assert(sd_xfer_exec(line,&io));
    assert(!held && !leases && !transfer_active);
}
static void write_file(const char *p,const void *bytes,size_t n)
{FILE *f=fopen(p,"wb");assert(f);assert(fwrite(bytes,1,n,f)==n);assert(!fclose(f));}
static void check_file(const char *p,const void *expected,size_t n)
{
    unsigned char bytes[8192];assert(n<=sizeof bytes);FILE *f=fopen(p,"rb");assert(f);
    assert(fread(bytes,1,sizeof bytes,f)==n && !memcmp(bytes,expected,n));assert(!fclose(f));
}
static int exists(const char *p) {struct stat st;return stat(p,&st)==0;}
static void check_recovered(void)
{
    unsigned char expected[64];memset(expected,'N',sizeof expected);
    assert(sd_xfer_recover() && !leases && !held && !transfer_active);
    check_file("card/original.dat",expected,sizeof expected);
    assert(!exists("card/original.dat.part") && !exists("card/original.dat.sdxfer-old") && !exists("card/.sdxfer.txn"));
}
int main(int argc,char **argv)
{
    assert(argc==2);int scenario=atoi(argv[1]);_mkdir("card");
    if(scenario!=13)write_file("card/original.dat","ORIGINAL",8);
    if(scenario>=1 && scenario<=4) {
        if(scenario==1)rename_fail_mask=1<<1;
        if(scenario==2)rename_fail_mask=1<<2;
        if(scenario==3)rename_fail_mask=(1<<2)|(1<<3);
        if(scenario==4)remove_fail=1;
        command("sdput original.dat 64");assert(strstr(response,"SDPUT ERR publish"));
        if(scenario<4)assert(exists("card/original.dat.part"));
        if(scenario==3 || scenario==4)check_file("card/original.dat.sdxfer-old","ORIGINAL",8);
        else check_file("card/original.dat","ORIGINAL",8);
        assert(exists("card/.sdxfer.txn"));rename_fail_mask=0;remove_fail=0;check_recovered();
    } else if(scenario==5 || scenario==6) {
        const char *commands[]={"sdget original.dat","sdget2 original.dat","sdcrc original.dat"};
        const char *prefixes[]={"SDGET","SDGET2","SDCRC"};
        int errors[]={EIO,ENOMEM,EMFILE,EACCES};
        for(unsigned e=0;e<(scenario==5?4u:1u);e++)for(unsigned i=0;i<3;i++) {
            fault_errno=scenario==5?errors[e]:0;read_fault=scenario==6;command(commands[i]);
            assert(strstr(response," ERR ") && !strstr(response,"MISSING"));
            char code[32];snprintf(code,sizeof code," %d",scenario==5?errors[e]:ENOMEM);assert(strstr(response,code));
        }
        fault_errno=0;read_fault=0;
        for(unsigned i=0;i<3;i++) {
            char line[64],expected[64];snprintf(line,sizeof line,"%s absent.dat",i==0?"sdget":i==1?"sdget2":"sdcrc");
            snprintf(expected,sizeof expected,"%s MISSING",prefixes[i]);command(line);assert(!strcmp(response,expected));
        }
    } else if(scenario==7) {
        command("sdput /watch/settings.0.bin 4");assert(strstr(response,"restore_required") && !reads);
        pending_intent=1;command("sdput! imu/intent.0.bin 4");assert(strstr(response," ERR active ") && !reads);pending_intent=0;
        live_record.state=HAL_IMU_RECORDING;strcpy(live_record.name,"0001.csv");
        command("sdput! imu/0001.csv 4");assert(strstr(response," ERR active ") && !reads);live_record.state=HAL_IMU_IDLE;
        command("sdput! watch/settings.0.bin 4");assert(strstr(response," ERR structure "));
        assert(exists("card/watch/settings.0.bin.part") && !exists("card/watch/settings.0.bin"));
    } else if(scenario==8) {
        _mkdir("card/checkpoints");char prefs[52]="checked restore";
        assert(prefs_disk_save("card/checkpoints",prefs,sizeof prefs));
        unsigned char bytes[512];FILE *f=fopen("card/checkpoints/settings.0.bin","rb");assert(f);
        input_size=fread(bytes,1,sizeof bytes,f);assert(!fclose(f));input_data=bytes;
        char line[80];snprintf(line,sizeof line,"sdput! watch/settings.0.bin %lu",(unsigned long)input_size);command(line);
        assert(strstr(response,"SDPUT OK"));assert(prefs_disk_validate_file("card/watch/settings.0.bin")==PREFS_DISK_OK);
        check_file("card/watch/settings.0.bin",bytes,input_size);
    } else if(scenario==9) {
        write_file("card/original.dat.part","STAGED",6);command("sdput original.dat 64");
        assert(strstr(response," ERR staged ") && !reads);check_file("card/original.dat","ORIGINAL",8);check_file("card/original.dat.part","STAGED",6);
    } else if(scenario==10) {
        write_file("card/.sdxfer.txn","torn",4);write_file("card/original.dat.part","STAGED",6);
        command("sdget original.dat");assert(!strcmp(response,"SDGET END"));
        command("sdget absent.dat");assert(strstr(response," ERR recovery ") && !strstr(response,"MISSING"));
        command("sdput other.dat 4");assert(strstr(response," ERR recovery "));
        assert(!sd_xfer_recover() && !leases && !held);check_file("card/original.dat","ORIGINAL",8);check_file("card/original.dat.part","STAGED",6);
    } else if(scenario==11) {
        _mkdir("card/imu");write_file("card/imu/0001.csv","ORIGINAL",8);late_live=1;
        command("sdput imu/0001.csv 64");assert(strstr(response," ERR publish "));
        check_file("card/imu/0001.csv","ORIGINAL",8);assert(exists("card/imu/0001.csv.part"));
        live_record.state=HAL_IMU_IDLE;late_live=0;assert(sd_xfer_recover() && !leases && !held);
        unsigned char expected[64];memset(expected,'N',sizeof expected);check_file("card/imu/0001.csv",expected,sizeof expected);
    } else if(scenario==12) {
        rename_fail_mask=(1<<2)|(1<<3);command("sdput original.dat 64");
        assert(strstr(response," ERR publish ") && exists("card/.sdxfer.txn") && exists("card/original.dat.part"));
        check_file("card/original.dat.sdxfer-old","ORIGINAL",8);assert(!exists("card/original.dat"));
    } else if(scenario==13) {
        /* A separate fresh process has no remembered transaction state. */
        assert(exists("card/.sdxfer.txn") && exists("card/original.dat.part"));check_recovered();
    } else if(scenario==14) {
        unsigned char data[2500];for(unsigned i=0;i<sizeof data;i++)data[i]=(uint8_t)i;
        write_file("card/original.dat",data,sizeof data);download_size=sizeof data;
        ack_mode=1;retry_frame=1;append_on_frame=1;command("sdget2 original.dat");
        assert(!strcmp(response,"SDGET2 END") && frames==4); /* one requested retransmission */
        ack_mode=0;command("sdget original.dat");assert(!strcmp(response,"SDGET END"));
        command("sdcrc original.dat");assert(!strncmp(response,"SDCRC 3000 ",11));
    } else assert(0);
    printf("PASS storage transfer fault scenario %d: durable recovery, typed errors, no transport waits under SD lock\n",scenario);
    return 0;
}
