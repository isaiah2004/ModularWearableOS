/* Actual prefs + checkpoint + extracted ESP HAL; synthetic/private host I/O. */
#include "hal.h"
#include "prefs.h"
#include "prefs_disk.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef int nvs_handle_t,esp_err_t;
enum { ESP_OK=0, ESP_ERR_NVS_NOT_FOUND=1, NVS_READONLY=0 };
static const char *test_directory;
static int leases,releases,locked,nvs_reads,nvs_writes;
static atomic_int s_preference_read_pending;
static int board_sd_asset_acquire(void){assert(!locked);leases++;return 1;}
static void board_sd_asset_release(void){assert(!locked);releases++;assert(releases==leases);}
static void board_sd_asset_lock(void){assert(leases==releases+1 && !locked);locked=1;}
static void board_sd_asset_unlock(void){assert(locked);locked=0;}
static prefs_disk_read_status mapped_load(const char *directory,void *data,size_t size)
{
    assert(!strcmp(directory,"/sdcard/watch") && locked && leases==releases+1);
    prefs_disk_read_status result=prefs_disk_load_status(test_directory,data,size);
    printf("actual HAL SD load size=%lu status=%d\n",(unsigned long)size,result);fflush(stdout);
    return result;
}
static int mapped_save(const char *directory,const void *data,size_t size)
{assert(!strcmp(directory,"/sdcard/watch") && locked);return prefs_disk_save(test_directory,data,size);}
static int unexpected_mkdir(const char *directory,int mode)
{(void)directory;(void)mode;assert(0 && "unexpected credentials-directory mutation");return -1;}
static int nvs_save(const char *key,const void *data,int size)
{
    assert(!strcmp(key,"nosprefs") && locked && data==&nos_prefs && size==(int)sizeof nos_prefs);
    nvs_writes++;return 1; /* Observe RAM mirror only; never touch real NVS. */
}
static esp_err_t nvs_open(const char *space,int mode,nvs_handle_t *handle)
{(void)space;(void)mode;(void)handle;nvs_reads++;return ESP_ERR_NVS_NOT_FOUND;}
static esp_err_t nvs_get_blob(nvs_handle_t handle,const char *key,void *data,size_t *size)
{(void)handle;(void)key;(void)data;(void)size;assert(0 && "unexpected NVS fallback read");return ESP_ERR_NVS_NOT_FOUND;}
static void nvs_close(nvs_handle_t handle){(void)handle;assert(0);}
uint32_t hal_millis(void){return 1000;}
#define prefs_disk_load_status mapped_load
#define prefs_disk_save mapped_save
#define mkdir unexpected_mkdir
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#include "actual_prefs_hal.h"
#undef prefs_disk_load_status
#undef prefs_disk_save
#undef mkdir

static void read_bank(int slot,unsigned char out[276])
{
    char path[512];assert(snprintf(path,sizeof path,"%s/settings.%d.bin",test_directory,slot)>0);
    FILE *f=fopen(path,"rb");assert(f && fread(out,1,276,f)==276 && fgetc(f)==EOF);assert(!fclose(f));
    assert(prefs_disk_validate_file(path)==PREFS_DISK_OK);
}
int main(int argc,char **argv)
{
    assert(argc==2);test_directory=argv[1];
    unsigned char before[2][276],after[2][276];read_bank(0,before[0]);read_bank(1,before[1]);
    assert(before[0][12]==42 && before[1][12]==42 && before[0][8]==25 && before[1][8]==24);
    assert(before[0][16]==3 && before[0][21]==62 && before[0][28]==8);
    prefs_load();
    assert(!prefs_loading() && prefs_pending());
    assert(sizeof nos_prefs==56 && nos_prefs.version==4 && nos_prefs.brightness==62 && nos_prefs.face==8);
    for(unsigned i=1;i<42;i++)if(i!=offsetof(nos_prefs_t,rev))
        assert(((const unsigned char *)&nos_prefs)[i]==before[0][16+i]);
    assert(nos_prefs.rev==6 && nos_prefs.fps_cap==60 && !nos_prefs.display_te &&
        !nos_prefs.body_birth_year && !nos_prefs.body_height_cm && !nos_prefs.body_sex);
    assert(prefs_wrist_source()==PREFS_WRIST_SOURCE_UNKNOWN && !prefs_wrist_revision());
    assert(!nvs_reads && !nvs_writes && !locked && leases==releases);
    read_bank(0,after[0]);read_bank(1,after[1]);assert(!memcmp(before,after,sizeof before));
    /* Migration preserves seq25 while publishing checked V4 into alternate1. */
    assert(prefs_flush() && !prefs_pending() && !prefs_loading());
    assert(!nvs_reads && nvs_writes==1 && !locked && leases==releases);
    read_bank(0,after[0]);read_bank(1,after[1]);assert(!memcmp(before[0],after[0],276));
    assert(after[1][8]==26 && after[1][12]==56 && after[1][16]==4 && after[1][21]==62 && after[1][28]==8);
    nos_prefs_t loaded;assert(prefs_disk_load_status(test_directory,&loaded,sizeof loaded)==PREFS_DISK_OK);
    assert(!memcmp(&loaded,&nos_prefs,sizeof loaded));
    memset(&nos_prefs,0,sizeof nos_prefs);prefs_load();
    assert(!prefs_loading() && nos_prefs.version==4 && nos_prefs.brightness==62 && nos_prefs.face==8);
    for(unsigned i=1;i<42;i++)if(i!=offsetof(nos_prefs_t,rev))
        assert(((const unsigned char *)&nos_prefs)[i]==before[0][16+i]);
    assert(nos_prefs.rev==6 && nos_prefs.fps_cap==60 && !nos_prefs.display_te &&
        !nos_prefs.body_birth_year && !nos_prefs.body_height_cm && !nos_prefs.body_sex);
    assert(leases==releases && !locked && !nvs_reads);
    puts("PASS actual42-byte V3 physical checkpoint migration: newest25 brightness62 face8, full legacy prefix preserved, balanced leases/lock, alternate26 verified, reboot round-trip");
    return 0;
}
