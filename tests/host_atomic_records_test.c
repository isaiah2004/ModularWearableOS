#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition, label) do { if(!(condition)) { \
    fprintf(stderr,"FAIL %s line=%d calls=%u\n",label,__LINE__,calls);return 1; \
} } while(0)
static const char wifi_log[]="I (532563) wifi:state: init -> auth (0xb0)\n";
static char wire[4096];
static size_t used;
static unsigned calls,reads;
static int s_cmd_ok,read_result;
static uint32_t last_offset,last_count;
static uint32_t raw_crc;
static uint32_t crc32(const uint8_t *bytes,unsigned count)
{
    uint32_t crc=~0u;
    for(unsigned i=0;i<count;i++) {
        crc^=bytes[i];
        for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));
    }
    return ~crc;
}
static int captured_vprintf(const char *format,va_list args)
{
    int n=vsnprintf(wire+used,sizeof wire-used,format,args);
    if(n<0 || (size_t)n>=sizeof wire-used)abort();
    used+=(size_t)n;calls++;
    /* SDK logs may run between complete stdio invocations. This deliberately
     * exposes a record assembled from multiple individually locked calls. */
    size_t count=sizeof wifi_log-1;
    if(count>=sizeof wire-used)abort();
    memcpy(wire+used,wifi_log,count+1);used+=count;
    return n;
}
static void reset(void)
{used=0;wire[0]=0;calls=reads=0;s_cmd_ok=1;read_result=-999;}
#define vprintf captured_vprintf
/* PRODUCTION CMD PRINTF */
#undef vprintf

typedef struct {uint8_t app_elf_sha256[32];char idf_ver[32];} esp_app_desc_t;
static esp_app_desc_t description;
static const esp_app_desc_t *esp_app_get_description(void){return &description;}
typedef struct {int cpu_mhz;} power_stats;
static power_stats nos_power_get_stats(void){return (power_stats){240};}
enum {MALLOC_CAP_SPIRAM=1};
static size_t heap_caps_get_largest_free_block(int caps)
{if(caps!=MALLOC_CAP_SPIRAM)abort();return 2162688;}
#define NOS_SOURCE_TREE_SHA256 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define NOS_SOURCE_BUILD_ID "0123456789abcdef"
#define CONFIG_ESP32S3_DATA_CACHE_SIZE 32768
#define CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE 64
#define STRIPE_ROWS 16
#define DMA_ROWS 8
#define GFX_W 466
#define LCD_SPI_HZ 80000000u
#define NOS_PRESENT_FPS 60
#define NOS_FPS 30
static unsigned char s_pipeline[320],s_pipeline_capture[400];
#define printf cmd_printf
static void emit_build(void)
{
/* PRODUCTION NOSBUILD */
}
#undef printf

typedef void *TaskHandle_t;
static int command_token;
static TaskHandle_t s_command_handle;
static unsigned watermark_queries;
static unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task)
{if(task!=&command_token)abort();watermark_queries++;return 1488;}
static void emit_command_stack(void)
{
#define printf cmd_printf
/* PRODUCTION COMMAND STACK */
#undef printf
}

static void nos_crash_report(void){}
static int nos_crash_clear(void){return 1;}
static int nos_crash_read(uint32_t offset,void *destination,uint32_t count)
{
    reads++;last_offset=offset;last_count=count;
    if(count>256)abort();
    int n=read_result==-999?(int)count:read_result;
    unsigned written=n<0?0u:(unsigned)n<count?(unsigned)n:count;
    for(unsigned i=0;i<written;i++)((uint8_t *)destination)[i]=(uint8_t)(offset+37*i);
    raw_crc=crc32(destination,written); /* Before the production in-place expansion. */
    return n;
}
/* PRODUCTION CRASH */

static int complete_record(const char *expected,const char *label)
{
    CHECK(calls==1,label);
    size_t size=strlen(expected);
    CHECK(!memcmp(wire,expected,size),label);
    CHECK(!strcmp(wire+size,wifi_log),label);
    CHECK(s_cmd_ok,label);
    return 0;
}
static int build_cases(void)
{
    strcpy(description.idf_ver,"897cf407");
    for(unsigned pattern=0;pattern<3;pattern++) {
        reset();char hex[65],expected[1024];
        for(unsigned i=0;i<32;i++) {
            description.app_elf_sha256[i]=(uint8_t)(pattern==0?0:pattern==1?255:i*17);
            snprintf(hex+2*i,sizeof hex-2*i,"%02x",description.app_elf_sha256[i]);
        }
        snprintf(expected,sizeof expected,
            "NOSBUILD source_sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef source_build_id=0123456789abcdef metrics=nos-device-v1 cpu_mhz=240 psram_largest=2162688 elf_sha256=%s idf=897cf407 dcache_bytes=32768 dcache_line=64 stripe_rows=16 dma_rows=8 stripe_dma_bytes=44736 pipeline_probe_supported=1 pipeline_probe_bytes=720 qspi_requested_hz=80000000 present_target_hz=60 animation_hz=30\n",hex);
        emit_build();
        if(complete_record(expected,"BUILD_RECORD_CALLS_AND_EXACT_FIELDS"))return 1;
    }
    puts("PASS actual NOSBUILD:3 full64-hex identities, exact fields/order/values,1 complete stdio call, concurrent Wi-Fi log stays on separate line");
    return 0;
}
static int crash_cases(void)
{
    for(unsigned count=1;count<=256;count++) {
        reset();unsigned offset=count+17;char command[80],expected[1024];
        snprintf(command,sizeof command,"crash read %u %u",offset,count);
        int pos=snprintf(expected,sizeof expected,"NOSCRASHDATA offset=%u bytes=%u hex=",offset,count);
        for(unsigned i=0;i<count;i++)pos+=snprintf(expected+pos,sizeof expected-(size_t)pos,"%02x",(uint8_t)(offset+37*i));
        strcpy(expected+pos,"\n");
        CHECK(host_crash_command(command)==1,"CRASH_SUCCESS");
        CHECK(reads==1 && last_offset==offset && last_count==count,"CRASH_READ_BOUND");
        if(complete_record(expected,"CRASH_RECORD_CALLS_AND_EXACT_BYTES"))return 1;
        uint8_t decoded[256];const char *payload=strstr(wire," hex=")+5;
        for(unsigned i=0;i<count;i++) {
            char byte[3]={payload[2*i],payload[2*i+1],0};
            decoded[i]=(uint8_t)strtoul(byte,NULL,16);
        }
        CHECK(crc32(decoded,count)==raw_crc,"CRASH_RAW_CRC_BEFORE_EXPANSION");
    }
    reset();read_result=3;
    CHECK(host_crash_command("crash read 0 256")==1,"CRASH_PARTIAL_SUCCESS");
    if(complete_record("NOSCRASHDATA offset=0 bytes=3 hex=00254a\n","CRASH_PARTIAL_RECORD"))return 1;
    for(int result=-1;result<=5;result++) {
        if(result>=1 && result<=4)continue;
        reset();read_result=result;
        CHECK(!host_crash_command("crash read 0 4") && !s_cmd_ok && calls==1 && reads==1,"CRASH_READ_FAILURE");
        CHECK(!strncmp(wire,"ERR crash read ",15),"CRASH_TYPED_ERROR");
    }
    const char *invalid[]={"crash read 0 0","crash read 0 257","crash read 4294967295 2","crash read 0 256 tail"};
    for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++) {
        reset();CHECK(!host_crash_command(invalid[i]) && !reads && !s_cmd_ok && calls==1,"CRASH_RANGE");
    }
    puts("PASS actual NOSCRASHDATA:1..256 byte payloads, every byte value, raw CRC preserved, bounded partial/error/range cases,1 complete stdio call, concurrent Wi-Fi log stays on separate line");
    return 0;
}
static int command_stack_cases(void)
{
    reset();watermark_queries=0;s_command_handle=&command_token;
    emit_command_stack();
    CHECK(watermark_queries==1,"CMD_STACK_OWNER");
    if(complete_record("NOSCMDSTACK stack_bytes=6144 min_free_bytes=1488\n","CMD_STACK_BYTE_UNITS"))return 1;
    reset();watermark_queries=0;s_command_handle=NULL;
    emit_command_stack();
    CHECK(!watermark_queries,"CMD_STACK_UNKNOWN_OWNER");
    if(complete_record("NOSCMDSTACK stack_bytes=6144 min_free_bytes=0\n","CMD_STACK_UNKNOWN_VALUE"))return 1;
    puts("PASS actual NOSCMDSTACK: reserved command-task handle sampled in byte units, no fallback to UI task,1 complete stdio call");
    return 0;
}
int main(int argc,char **argv)
{
    if(argc!=2)return 2;
    if(!strcmp(argv[1],"build"))return build_cases();
    if(!strcmp(argv[1],"crash"))return crash_cases();
    if(!strcmp(argv[1],"command-stack"))return command_stack_cases();
    return 2;
}
