#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <direct.h>
#include "../nos/prefs_disk.c"
#include "../nos/prefs.c"
static uint32_t clock_ms;
static uint32_t restore_sequence=50;
static int pending,read_failed,stores;
static unsigned char overlay_blob[256];
static int overlay_exists,overlay_failread,overlay_failsave,overlay_saves;
int hal_prefs_overlay_load(void *p,int n)
{
    assert(n==(int)sizeof(preference_overlay));
    if(overlay_failread)return -1;
    if(!overlay_exists)return 0;
    memcpy(p,overlay_blob,(size_t)n);return 1;
}
int hal_prefs_overlay_save_checked(const void *p,int n)
{
    assert(n==(int)sizeof(preference_overlay));overlay_saves++;
    if(overlay_failsave)return 0;
    memcpy(overlay_blob,p,(size_t)n);overlay_exists=1;return 1;
}
uint32_t hal_millis(void){return clock_ms;}
int hal_store_load(const char *key,void *out,int n)
{
    assert(!strcmp(key,"nosprefs"));
    prefs_disk_read_status result=read_failed?PREFS_DISK_IO:prefs_disk_load_status("card/watch",out,(size_t)n);
    pending=result==PREFS_DISK_IO || result==PREFS_DISK_CORRUPT;
    return result==PREFS_DISK_OK;
}
/* prefs.c's portable weak fallback is replaced at compile time in the runner. */
int critical_load_pending(const char *key){assert(!strcmp(key,"nosprefs"));return pending;}
int hal_store_save_checked(const char *key,const void *p,int n)
{assert(!strcmp(key,"nosprefs"));stores++;return prefs_disk_save("card/watch",p,(size_t)n);}
static void restore_bank(const nos_prefs_t *p,size_t size)
{
    checkpoint bank={.magic=0x4e4f5350,.format=1,.sequence=restore_sequence,.size=(uint32_t)size};
    memcpy(bank.data,p,size);bank.checksum=checksum(&bank);
    FILE *f=fopen("card/watch/settings.0.bin","wb");assert(f);
    assert(fwrite(&bank,1,sizeof bank,f)==sizeof bank);assert(!fclose(f));
}
int main(int argc,char **argv)
{
    assert(argc==2);_mkdir("card");_mkdir("card/watch");prefs_defaults();
    nos_prefs_t incoming=nos_prefs;incoming.wrist_left=0;incoming.wrist_source=PREFS_WRIST_SOURCE_GUIDED;
    incoming.wrist_revision=30;incoming.brightness=20;
    if(!strncmp(argv[1],"overlay-",8)) {
        incoming.volume=11;incoming.tz_min=256;restore_bank(&incoming,sizeof incoming);
        read_failed=1;prefs_load();nos_prefs.brightness=45;nos_prefs.tz_min=255;nos_prefs.fps_cap=40;nos_prefs.display_te=1;prefs_save();
        if(!strcmp(argv[1],"overlay-wrist"))prefs_set_wrist(1,PREFS_WRIST_SOURCE_MANUAL);
        overlay_failsave=1;assert(!prefs_flush() && !overlay_exists && prefs_pending());
        overlay_failsave=0;assert(!prefs_flush() && overlay_exists && prefs_loading());
        unsigned char saved[sizeof overlay_blob];memcpy(saved,overlay_blob,sizeof saved);
        if(!strcmp(argv[1],"overlay-unknown") || !strcmp(argv[1],"overlay-corrupt")) {
            int corrupt=!strcmp(argv[1],"overlay-corrupt");
            if(corrupt)overlay_blob[20]^=0x40;else overlay_failread=1;
            prefs_load();assert(prefs_loading());nos_prefs.volume=31;prefs_save();
            int before=stores,nvs=overlay_saves;
            assert(!prefs_flush() && stores==before && overlay_saves==nvs);
            if(corrupt)assert(overlay_blob[20]==(unsigned char)(saved[20]^0x40));
            else assert(!memcmp(saved,overlay_blob,sizeof saved));
            memcpy(overlay_blob,saved,sizeof saved);overlay_failread=0;read_failed=0;
            assert(prefs_flush() && nos_prefs.brightness==45 && nos_prefs.tz_min==255 && nos_prefs.volume==31 && nos_prefs.fps_cap==40 && nos_prefs.display_te==1);
        } else {
            prefs_load();assert(nos_prefs.brightness==45 && nos_prefs.tz_min==255 && nos_prefs.volume==60 && nos_prefs.fps_cap==40 && nos_prefs.display_te==1);
            read_failed=0;overlay_failsave=1;
            assert(!prefs_flush() && prefs_pending() && !prefs_loading());
            uint32_t wrist=prefs_wrist_revision();
            if(!strcmp(argv[1],"overlay-wrist"))assert(wrist==31 && nos_prefs.wrist_left==1);
            assert(!memcmp(saved,overlay_blob,sizeof saved));
            prefs_load();assert(nos_prefs.brightness==45 && nos_prefs.tz_min==255 && nos_prefs.volume==11 && nos_prefs.fps_cap==40 && nos_prefs.display_te==1);
            assert(prefs_wrist_revision()==wrist);overlay_failsave=0;assert(prefs_flush());
            incoming.brightness=77;incoming.tz_min=444;incoming.volume=9;restore_sequence=1000;restore_bank(&incoming,sizeof incoming);
            prefs_load();assert(nos_prefs.brightness==77 && nos_prefs.tz_min==444 && nos_prefs.volume==9 && nos_prefs.fps_cap==60 && !nos_prefs.display_te);
        }
        puts("PASS checked explicit-edit overlay survives failure/reboot, protects unknown NVS, merges whole fields, and retires idempotently after SD verification");
    } else if(!strcmp(argv[1],"unsupported")) {
        incoming.version=99;restore_bank(&incoming,44);prefs_load();
        assert(prefs_loading() && prefs_pending());int before=stores;
        assert(!prefs_flush() && stores==before);
        puts("PASS unsupported intrinsic legacy preference schema cannot become safe default overwrite");
    } else if(!strcmp(argv[1],"repair-unknown")) {
        nos_prefs_t unsupported=incoming;unsupported.version=99;restore_bank(&unsupported,44);
        prefs_load();assert(prefs_loading());nos_prefs.volume=31;prefs_save();
        assert(prefs_restore_begin());restore_bank(&incoming,sizeof incoming);prefs_restore_end(1);
        assert(prefs_flush() && nos_prefs.brightness==20 && nos_prefs.volume==31 && !prefs_loading());
        puts("PASS explicit checked repair of unknown preferences preserves pending field edits");
    } else if(!strcmp(argv[1],"wrist")) {
        restore_bank(&incoming,sizeof incoming);read_failed=1;prefs_load();
        prefs_set_wrist(1,PREFS_WRIST_SOURCE_MANUAL);read_failed=0;
        assert(prefs_flush());assert(nos_prefs.wrist_left==1 && prefs_wrist_source()==PREFS_WRIST_SOURCE_MANUAL);
        assert(prefs_wrist_revision()==31);
        puts("PASS delayed SD load merges the explicit wrist choice and advances durable provenance");
    } else {
        prefs_load();assert(prefs_flush());
#ifdef PREFS_RESTORE_API
        assert(prefs_restore_begin());assert(!prefs_disk_write_allowed(NULL));
        assert(!prefs_disk_save("card/watch",&nos_prefs,sizeof nos_prefs));
        _mkdir("card/watch/wifi");char credentials[8]="wifi";
        assert(prefs_disk_save("card/watch/wifi",credentials,sizeof credentials));
        if(!strcmp(argv[1],"restore-edit")){nos_prefs.volume=31;prefs_save();}
        restore_bank(&incoming,sizeof incoming);prefs_restore_end(1);
        assert(prefs_loading() && prefs_pending());assert(prefs_flush());
        assert(nos_prefs.brightness==20 && prefs_wrist_revision()==30);
        assert(nos_prefs.volume==(!strcmp(argv[1],"restore-edit")?31:60));
        nos_prefs.brightness=49;prefs_save();assert(!prefs_restore_begin());assert(prefs_flush());
        assert(prefs_restore_begin());prefs_restore_end(0);assert(prefs_disk_write_allowed(NULL));
        puts("PASS explicit restore blocks stale writes, reloads clean RAM, merges concurrent edits and rejects dirty admission");
#else
        restore_bank(&incoming,sizeof incoming);prefs_save();assert(prefs_flush());
        assert(nos_prefs.brightness==20);
#endif
    }
    return 0;
}
