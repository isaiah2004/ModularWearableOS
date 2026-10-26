#define SD_XFER_OTA_TEST 1
static int ota_writer,ota_owned,ota_begins,ota_ends;
#define main original_transfer_fixture_main
/* @TRANSFER_FIXTURE@ */
#undef main
#include "prefs.h"
int ota_external_activity_begin(void)
{ota_begins++;assert(!held);if(ota_writer)return 0;assert(!ota_owned);ota_owned=1;return 1;}
void ota_external_activity_end(void)
{assert(ota_owned && !held);ota_owned=0;ota_ends++;}
static int critical_pending;
uint32_t hal_millis(void){return 10000;}
int hal_store_load(const char *key,void *out,int n)
{
    assert(!strcmp(key,"nosprefs"));board_sd_asset_lock();
    prefs_disk_read_status result=prefs_disk_load_status("card/watch",out,(size_t)n);
    board_sd_asset_unlock();critical_pending=result==PREFS_DISK_IO || result==PREFS_DISK_CORRUPT;
    return result==PREFS_DISK_OK;
}
int hal_store_load_pending(const char *key){assert(!strcmp(key,"nosprefs"));return critical_pending;}
int hal_store_save_checked(const char *key,const void *p,int n)
{
    assert(!strcmp(key,"nosprefs"));board_sd_asset_lock();
    int ok=prefs_disk_save("card/watch",p,(size_t)n);board_sd_asset_unlock();return ok;
}
static unsigned char restore_bytes[512];
static void prepare_restore(void)
{
    _mkdir("card");_mkdir("card/watch");_mkdir("card/incoming");
    prefs_load();assert(prefs_flush());nos_prefs_t incoming=nos_prefs;
    for(int i=0;i<5;i++){incoming.brightness=(uint8_t)(20+i);assert(prefs_disk_save("card/incoming",&incoming,sizeof incoming));}
    FILE *f=fopen("card/incoming/settings.0.bin","rb");assert(f);
    input_size=fread(restore_bytes,1,sizeof restore_bytes,f);assert(!fclose(f));
    input_data=restore_bytes;input_offset=0;
}
static void put_restore(void)
{
    char line[80];snprintf(line,sizeof line,"sdput! watch/settings.0.bin %lu",(unsigned long)input_size);
    command(line);
}
static int edit_during_receive(uint8_t *b,int n,int timeout)
{
    int result=receive(b,n,timeout);nos_prefs.volume=31;prefs_save();return result;
}
int main(int argc,char **argv)
{
    assert(argc==2);_mkdir("card");
    if(!strncmp(argv[1],"fixture-",8)) {
        argv[1]+=8;int result=original_transfer_fixture_main(argc,argv);assert(!ota_owned && ota_begins==ota_ends);return result;
    } else if(!strcmp(argv[1],"ota")) {
        write_file("card/original.dat","ORIGINAL",8);ota_writer=1;command("sdput original.dat 64");
        assert(strstr(response,"ERR busy") && !reads && !leases && !ota_owned);
        command("sdget original.dat");assert(!strcmp(response,"SDGET END"));
        ota_writer=0;rename_fail_mask=1<<1;command("sdput original.dat 64");assert(strstr(response,"ERR publish"));
        assert(!ota_owned && exists("card/.sdxfer.txn"));rename_fail_mask=0;int renames=rename_calls;
        ota_writer=1;assert(!sd_xfer_recover() && errno==EBUSY && rename_calls==renames && !ota_owned);
        command("sdget original.dat");assert(!strcmp(response,"SDGET END") && rename_calls==renames);
        command("sdcrc absent.dat");assert(strstr(response,"ERR recovery"));
        ota_writer=0;check_recovered();assert(!ota_owned);
        puts("PASS common transfer/recovery OTA scopes exclude publication, balance failures and preserve readable originals");
    } else if(!strcmp(argv[1],"aliases")) {
        const char *bad[]={"sdput watch/./settings.0.bin 4","sdput watch//settings.0.bin 4",
                           "sdput watch\\settings.0.bin 4","sdput WATCH/SETTINGS.0.BIN 4",
                           "sdput .SDXFER.TXN 4","sdput file.PART 4"};
        for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++) {command(bad[i]);assert(strstr(response,"SDPUT ERR") && !reads);}
        _mkdir("card/watch");write_file("card/watch/settings.0.bin","KEEP",4);
        command("sdrm /watch/settings.0.bin");assert(strstr(response,"ERR protected"));
        check_file("card/watch/settings.0.bin","KEEP",4);
        _mkdir("card/imu");write_file("card/imu/0005.csv","KEEP",4);pending_intent=1;
        command("sdrm imu/0005.csv");assert(strstr(response,"ERR active"));
        command("sdput! imu/0005.csv 4");assert(strstr(response,"ERR active") && !reads);
        command("sdput! watch/wifi/settings.0.bin 4");assert(strstr(response,"ERR owner_busy") && !reads);
        puts("PASS raw checkpoint aliases, protected removals, unknown IMU CSV owner and unsafe Wi-Fi restore are guarded");
    } else {
        prepare_restore();
        if(!strcmp(argv[1],"dirty")) {
            nos_prefs.volume=31;prefs_save();put_restore();
            assert(strstr(response,"ERR owner_busy") && !reads);assert(prefs_flush());
        } else if(!strcmp(argv[1],"late-edit")) {
            char line[80];snprintf(line,sizeof line,"sdput! watch/settings.0.bin %lu",(unsigned long)input_size);
            const xfer_io io={edit_during_receive,capture,2048};assert(sd_xfer_exec(line,&io));
            assert(strstr(response,"ERR publish") && exists("card/.sdxfer.txn"));
            assert(prefs_flush());assert(sd_xfer_recover());assert(prefs_flush());
            assert(nos_prefs.brightness==24 && nos_prefs.volume==60);
        } else {
            put_restore();assert(strstr(response,"SDPUT OK") && prefs_loading() && prefs_pending());
            assert(!hal_store_save_checked("nosprefs",&nos_prefs,sizeof nos_prefs));
            assert(prefs_flush());assert(nos_prefs.brightness==24 && !prefs_loading());
            nos_prefs_t durable;assert(prefs_disk_load("card/watch",&durable,sizeof durable));
            assert(durable.brightness==24);
        }
        assert(!held && !leases && !transfer_active && !ota_owned);
        puts("PASS full transfer/real preferences restore owns publication, rejects dirty/late edits and reloads before stale flush");
    }
    return 0;
}
