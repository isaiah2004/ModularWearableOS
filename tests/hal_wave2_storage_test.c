/* Execute the complete HAL, real checked banks, and the portable RAM owner.
 * Only SDK NVS/mount calls and the card root are redirected into private A: IO. */
#include "hal_wave2_common.inc"
#include "prefs.h"
#include "prefs_disk.h"
#include <errno.h>
#define prefs_disk_load_status disk_load_actual
#define prefs_disk_load disk_load_bool_actual
#define prefs_disk_save disk_save_actual
#include "../nos/prefs_disk.c"
#undef prefs_disk_load_status
#undef prefs_disk_load
#undef prefs_disk_save
static int private_mkdir(const char *path);
#undef mkdir
#define mkdir(path,mode) private_mkdir(path)
#include "../firmware/main/hal_esp32.c"
#undef mkdir
#include "../firmware/main/ota.c"

static workout_snapshot test_workout;
int nos_host_shutdown_pending(void) {return 0;}
const workout_snapshot *workout_get(void) {return &test_workout;}
void workout_storage_get(workout_storage_status *out) {memset(out,0,sizeof *out);out->ready=1;}
void hal_imu_record_get(hal_imu_record *out) {memset(out,0,sizeof *out);}
void hal_imu_file_get(hal_imu_file *out) {memset(out,0,sizeof *out);}
void hal_imu_stream_get(hal_imu_stream *out) {memset(out,0,sizeof *out);}
int imu_worker_pending(void) {return 0;}
int svc_track_active(void) {return 0;}
int svc_track_pending(void) {return 0;}
int svc_track_saving(void) {return 0;}
int hal_rec_active(void) {return 0;}
int hal_audio_playing(void) {return 0;}

static pthread_mutex_t card_mutex=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int card_locked,card_leased;
static int card_absent,card_read_error,card_save_error;
static unsigned card_reads,card_saves,card_acquires,card_releases;
static const char *card_path(const char *path)
{
    if(!strcmp(path,"/sdcard/watch")) return "storage-card";
    assert(!strcmp(path,"/sdcard/watch/wifi"));return "storage-card/wifi";
}
static int private_mkdir(const char *path) {return _mkdir(card_path(path));}
int board_sd_asset_acquire(void)
{
    assert(!card_locked && !card_leased);card_acquires++;
    if(card_absent) return 0;
    card_leased=1;return 1;
}
void board_sd_asset_release(void)
{assert(card_leased && !card_locked);card_leased=0;card_releases++;}
void board_sd_asset_lock(void)
{assert(!card_locked);assert(!pthread_mutex_lock(&card_mutex));card_locked=1;}
void board_sd_asset_unlock(void)
{assert(card_locked);card_locked=0;assert(!pthread_mutex_unlock(&card_mutex));}
prefs_disk_read_status prefs_disk_load_status(const char *path,void *p,size_t n)
{
    assert(card_locked && card_leased);card_reads++;
    return card_read_error?PREFS_DISK_IO:disk_load_actual(card_path(path),p,n);
}
int prefs_disk_save(const char *path,const void *p,size_t n)
{
    assert(card_locked && card_leased);card_saves++;
    return !card_save_error && disk_save_actual(card_path(path),p,n);
}

typedef struct {const char *key;unsigned char committed[256],pending[256];size_t size,pending_size;int exists,staged;} nvs_slot;
static nvs_slot nvs_slots[3]={{.key="nosprefs"},{.key="wifi_cred"},{.key="prefs_overlay"}};
static int nvs_fail_rw,nvs_fail_ro,nvs_fail_set,nvs_fail_commit,nvs_fail_get,nvs_short,nvs_corrupt;
static int nvs_opened,nvs_mode,nvs_missing_namespace;
static unsigned nvs_sets,nvs_gets,nvs_commits;
static void (*nvs_set_hook)(void);
static nvs_slot *slot_for(const char *key)
{for(unsigned i=0;i<3;i++) if(!strcmp(key,nvs_slots[i].key)) return &nvs_slots[i];abort();}
esp_err_t nvs_open(const char *space,int mode,nvs_handle_t *out)
{
    assert(!strcmp(space,"wos") && !nvs_opened);
    if(nvs_missing_namespace && mode==NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    if((mode==NVS_READWRITE && nvs_fail_rw) || (mode==NVS_READONLY && nvs_fail_ro)) return ESP_FAIL;
    nvs_opened=1;nvs_mode=mode;*out=7;return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *p,size_t n)
{
    assert(h==7 && nvs_opened && nvs_mode==NVS_READWRITE && n<=256);nvs_sets++;
    if(strcmp(key,"prefs_overlay")) assert(card_locked);
    if(nvs_set_hook) nvs_set_hook();
    if(nvs_fail_set) return ESP_FAIL;
    nvs_slot *slot=slot_for(key);memcpy(slot->pending,p,n);slot->pending_size=n;slot->staged=1;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    assert(h==7 && nvs_opened && nvs_mode==NVS_READWRITE);nvs_commits++;
    if(nvs_fail_commit) return ESP_FAIL;
    for(unsigned i=0;i<3;i++) if(nvs_slots[i].staged) {
        nvs_slot *slot=&nvs_slots[i];memcpy(slot->committed,slot->pending,slot->pending_size);
        slot->size=slot->pending_size;slot->exists=1;slot->staged=0;
    }
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *p,size_t *n)
{
    assert(h==7 && nvs_opened && nvs_mode==NVS_READONLY);nvs_gets++;
    nvs_slot *slot=slot_for(key);
    if(nvs_fail_get) return ESP_FAIL;
    if(!slot->exists) return ESP_ERR_NVS_NOT_FOUND;
    if(*n<slot->size) {*n=slot->size;return ESP_FAIL;}
    memcpy(p,slot->committed,slot->size);*n=slot->size;
    if(nvs_short) --*n;
    if(nvs_corrupt && *n) ((unsigned char*)p)[*n-1]^=0x80;
    return ESP_OK;
}
void nvs_close(nvs_handle_t h)
{
    assert(h==7 && nvs_opened);nvs_opened=0;
    for(unsigned i=0;i<3;i++) nvs_slots[i].staged=0;
}

static void faults_clear(void)
{
    nvs_fail_rw=nvs_fail_ro=nvs_fail_set=nvs_fail_commit=nvs_fail_get=nvs_short=nvs_corrupt=0;
    nvs_missing_namespace=0;nvs_set_hook=NULL;card_read_error=card_save_error=card_absent=0;
}
static void reset_store(void)
{
    assert(!card_locked && !card_leased && !nvs_opened);faults_clear();
    for(unsigned i=0;i<3;i++) {nvs_slots[i].exists=nvs_slots[i].staged=0;nvs_slots[i].size=0;}
    atomic_store(&s_preference_read_pending,0);atomic_flag_clear(&s_overlay_owner);
    _mkdir("storage-card");_mkdir("storage-card/wifi");
    remove("storage-card/settings.0.bin");remove("storage-card/settings.1.bin");
    remove("storage-card/wifi/settings.0.bin");remove("storage-card/wifi/settings.1.bin");
}
static void test_overlay_adapter(void)
{
    reset_store();uint8_t input[97],out[97];memset(input,0x35,sizeof input);memset(out,0xA7,sizeof out);
    unsigned before=card_acquires;
    assert(hal_prefs_overlay_load(out,sizeof out)==0 && out[0]==0xA7);
    nvs_missing_namespace=1;assert(hal_prefs_overlay_load(out,sizeof out)==0);nvs_missing_namespace=0;
    atomic_store(&s_preference_read_pending,1);
    for(int fault=0;fault<7;fault++) {
        faults_clear();
        if(fault==0) nvs_fail_rw=1;
        if(fault==1) nvs_fail_set=1;
        if(fault==2) nvs_fail_commit=1;
        if(fault==3) nvs_fail_ro=1;
        if(fault==4) nvs_fail_get=1;
        if(fault==5) nvs_short=1;
        if(fault==6) nvs_corrupt=1;
        assert(!hal_prefs_overlay_save_checked(input,sizeof input));
        assert(hal_store_load_pending("nosprefs") && !atomic_flag_test_and_set(&s_overlay_owner));
        atomic_flag_clear(&s_overlay_owner);
    }
    faults_clear();assert(hal_prefs_overlay_save_checked(input,sizeof input));
    assert(hal_prefs_overlay_load(out,sizeof out)==1 && !memcmp(input,out,sizeof input));
    assert(hal_store_load_pending("nosprefs") && !slot_for("nosprefs")->exists && card_acquires==before);
    for(int fault=0;fault<3;fault++) {
        memset(out,0xA7,sizeof out);faults_clear();
        if(fault==0) nvs_fail_ro=1;
        if(fault==1) nvs_fail_get=1;
        if(fault==2) nvs_short=1;
        assert(hal_prefs_overlay_load(out,sizeof out)==-1);
        for(unsigned i=0;i<sizeof out;i++) assert(out[i]==0xA7);
    }
    faults_clear();assert(hal_prefs_overlay_load(out,1)==-1);
    assert(!hal_prefs_overlay_save_checked(input,257) && hal_prefs_overlay_load(out,257)==-1);
    assert(!hal_prefs_overlay_save_checked(NULL,1) && hal_prefs_overlay_load(NULL,1)==-1);
    atomic_flag_test_and_set(&s_overlay_owner);
    assert(!hal_prefs_overlay_save_checked(input,sizeof input) && hal_prefs_overlay_load(out,sizeof out)==-1);
    atomic_flag_clear(&s_overlay_owner);
    puts("PASS storage: separate checked NVS overlay handles seven commit/readback faults, exact sizes, absent/error distinction, unchanged failure buffer and owner collision; SD gate/mirror unchanged");
}
static void write_raw_bank_sequence(const nos_prefs_t *incoming,uint32_t sequence)
{
    checkpoint raw={.magic=0x4e4f5350,.format=1,.sequence=sequence,.size=sizeof *incoming};
    memcpy(raw.data,incoming,sizeof *incoming);raw.checksum=checksum(&raw);
    FILE *f=fopen("storage-card/settings.0.bin","wb");assert(f);
    assert(fwrite(&raw,1,sizeof raw,f)==sizeof raw && !fflush(f) && !fclose(f));
}
static void write_raw_bank(const nos_prefs_t *incoming) {write_raw_bank_sequence(incoming,100);}
static void test_actual_restore(void)
{
    reset_store();prefs_load();assert(prefs_flush());
    nos_prefs_t incoming=nos_prefs;incoming.brightness=62;incoming.volume=27;
    unsigned before=nvs_sets,saves=card_saves;
    board_sd_asset_lock();assert(prefs_restore_begin());write_raw_bank(&incoming);prefs_restore_end(1);board_sd_asset_unlock();
    assert(!hal_store_save_checked("nosprefs",&nos_prefs,sizeof nos_prefs));
    assert(nvs_sets==before && card_saves==saves && prefs_loading());
    /* Credentials have their own directory and raw restores fail EBUSY. Main
     * preference reload must not disable independent credential durability. */
    wifi_cred_t credentials={.ssid="independent-cache",.pass="private-test-only"};
    assert(wifi_credentials_checkpoint(&credentials));
    nos_prefs.hour24=0;prefs_save();assert(prefs_flush());
    assert(nos_prefs.brightness==62 && nos_prefs.volume==27 && !nos_prefs.hour24);
    assert(!prefs_loading() && !prefs_pending());
    nos_prefs_t from_card;assert(disk_load_actual("storage-card",&from_card,sizeof from_card)==PREFS_DISK_OK);
    assert(!memcmp(&from_card,&nos_prefs,sizeof from_card));
    assert(slot_for("nosprefs")->size==sizeof nos_prefs &&
           !memcmp(slot_for("nosprefs")->committed,&nos_prefs,sizeof nos_prefs));
    puts("PASS storage: actual prefs restore/reload and HAL prevent stale SD and NVS preference mirrors, retain independent credential checkpoint, then merge concurrent field edit and verify paired banks");
}
static void test_unknown_sd(void)
{
    unsigned before=nvs_sets,saves=card_saves;nos_prefs_t initial=nos_prefs;
    card_read_error=1;nos_prefs_t out={0};assert(!hal_store_load("nosprefs",&out,sizeof out));
    assert(hal_store_load_pending("nosprefs"));
    assert(!hal_store_save_checked("nosprefs",&initial,sizeof initial));
    assert(nvs_sets==before && card_saves==saves);
    card_read_error=0;assert(hal_store_load("nosprefs",&out,sizeof out));assert(!hal_store_load_pending("nosprefs"));
    assert(!memcmp(&initial,&out,sizeof out));
    card_save_error=1;assert(!hal_store_save_checked("nosprefs",&initial,sizeof initial));card_save_error=0;
    assert(card_acquires==card_releases && !card_locked && !card_leased);
    puts("PASS storage: unknown SD never becomes absence or a successful checkpoint; checked read resolves gate, failed save remains failure, mount leases/locks balance");
}
static void credentials_owned(void)
{assert(atomic_load(&s_busy)==2 && !ota_activity_begin() && !ota_busy());}
static void test_credential_ota_exclusion(void)
{
    wifi_cred_t credentials={.ssid="durable-network",.pass="private-test-only"};
    ota_session_begin();ota_poll();assert(writer_claim(session_live(NULL)));
    unsigned sets=nvs_sets,acquires=card_acquires;int64_t start=esp_timer_get_time();
    assert(!wifi_credentials_checkpoint(&credentials));
    assert(nvs_sets==sets && card_acquires==acquires && esp_timer_get_time()==start && ota_busy());
    writer_release();nvs_set_hook=credentials_owned;
    assert(wifi_credentials_checkpoint(&credentials));nvs_set_hook=NULL;
    assert(!atomic_load(&s_busy) && !s_runtime_checked && !writer_claim(session_live(NULL)));
    assert(ota_activity_begin());ota_activity_end();assert(writer_claim(session_live(NULL)));writer_release();
    assert(slot_for("wifi_cred")->size==sizeof credentials &&
           !memcmp(slot_for("wifi_cred")->committed,&credentials,sizeof credentials));
    puts("PASS storage/PLAT-002: actual Wi-Fi worker checkpoint shares real OTA ownership before SD/NVS, preserves retry while writer owns flash, and invalidates policy before release");
}
static void test_absent_credential_fallback(void)
{
    card_absent=1;prefs_load();assert(prefs_loading() && prefs_pending());
    wifi_cred_t credentials={.ssid="fallback-network",.pass="private-test-only"};
    unsigned saves=card_saves;
    assert(!wifi_credentials_checkpoint(&credentials)); /* never an SD match */
    assert(card_saves==saves && prefs_loading() && prefs_pending() && hal_store_load_pending("nosprefs"));
    nvs_slot *mirror=slot_for("wifi_cred");assert(mirror->exists && mirror->size==sizeof credentials);
    assert(!memcmp(mirror->committed,&credentials,sizeof credentials));
    assert(!atomic_load(&s_busy) && !card_locked && !card_leased);
    puts("PASS storage: independent card-absent credential fallback persists without resolving unknown SD, claiming SD success, or clearing main preference retry");
}
/* Three independent executable processes model reset. Only the mock NVS
 * committed bytes and real checked-bank files cross the process boundary. */
static void nvs_fixture_save(void)
{
    FILE *f=fopen("storage-nvs.bin","wb");assert(f);
    for(unsigned i=0;i<3;i++) {
        uint32_t header[2]={(uint32_t)nvs_slots[i].exists,(uint32_t)nvs_slots[i].size};
        assert(fwrite(header,1,sizeof header,f)==sizeof header);
        assert(fwrite(nvs_slots[i].committed,1,256,f)==256);
    }
    assert(!fflush(f) && !fclose(f));
}
static void nvs_fixture_load(void)
{
    FILE *f=fopen("storage-nvs.bin","rb");assert(f);
    for(unsigned i=0;i<3;i++) {
        uint32_t header[2];assert(fread(header,1,sizeof header,f)==sizeof header && header[1]<=256);
        nvs_slots[i].exists=(int)header[0];nvs_slots[i].size=header[1];
        assert(fread(nvs_slots[i].committed,1,256,f)==256);
    }
    assert(fgetc(f)==EOF && !fclose(f));
}
static void test_offline_process(const char *phase)
{
    if(!strcmp(phase,"offline-create")) {
        reset_store();prefs_defaults();nos_prefs_t fallback=nos_prefs,incoming=nos_prefs;
        incoming.brightness=62;incoming.volume=11;incoming.tz_min=256;
        write_raw_bank(&incoming);
        nvs_slot *mirror=slot_for("nosprefs");mirror->exists=1;mirror->size=sizeof fallback;
        memcpy(mirror->committed,&fallback,sizeof fallback);
        card_absent=1;prefs_load();assert(prefs_loading());
        nos_prefs.brightness=55;nos_prefs.tz_min=255;prefs_save();
        assert(!prefs_flush() && prefs_loading() && prefs_pending() && hal_store_load_pending("nosprefs"));
        assert(slot_for("prefs_overlay")->exists); /* the original gap fails here */
        assert(!memcmp(mirror->committed,&fallback,sizeof fallback));
        nos_prefs_t unchanged;assert(disk_load_actual("storage-card",&unchanged,sizeof unchanged)==PREFS_DISK_OK);
        assert(!memcmp(&unchanged,&incoming,sizeof incoming));nvs_fixture_save();
        puts("PASS offline reset phase 1: explicit field overlay is durable while unknown SD and original fallback mirror remain unchanged; RAM stays dirty/loading");
    } else if(!strcmp(phase,"offline-unknown")) {
        nvs_fixture_load();card_absent=1;nvs_fail_get=1;
        unsigned char retained[256];memcpy(retained,slot_for("prefs_overlay")->committed,sizeof retained);
        prefs_load();nos_prefs.volume=31;prefs_save();
        assert(!prefs_flush() && prefs_loading() && prefs_pending());
        assert(!nvs_sets && !card_saves && !memcmp(retained,slot_for("prefs_overlay")->committed,sizeof retained));
        nvs_fail_get=0;assert(!prefs_flush());
        assert(nos_prefs.brightness==55 && nos_prefs.tz_min==255 && nos_prefs.volume==31);
        assert(prefs_loading() && prefs_pending() && hal_store_load_pending("nosprefs"));
        /* Independent fault case: leave the reset fixture for offline-return. */
        puts("PASS offline faults: unread NVS overlay cannot be overwritten or treated absent; later checked read merges its durable fields with a new explicit RAM edit while SD stays unknown");
    } else if(!strcmp(phase,"offline-return")) {
        nvs_fixture_load();card_absent=1;prefs_load();
        assert(nos_prefs.brightness==55 && nos_prefs.tz_min==255 && nos_prefs.volume==60);
        assert(prefs_loading() && prefs_pending() && hal_store_load_pending("nosprefs"));
        card_absent=0;assert(prefs_flush());
        assert(nos_prefs.brightness==55 && nos_prefs.tz_min==255 && nos_prefs.volume==11);
        assert(!prefs_pending() && !prefs_loading() && !hal_store_load_pending("nosprefs"));
        nos_prefs_t checked;assert(disk_load_actual("storage-card",&checked,sizeof checked)==PREFS_DISK_OK);
        assert(!memcmp(&checked,&nos_prefs,sizeof checked));nvs_fixture_save();
        puts("PASS offline reset phase 2: fallback reboot retains explicit edits, future SD load merges whole 16-bit timezone and preserves untouched newer SD volume; checked save clears retry");
    } else if(!strcmp(phase,"offline-tombstone")) {
        nvs_fixture_load();prefs_load();assert(prefs_flush());
        nos_prefs.brightness=56;nos_prefs.tz_min=254;prefs_save();card_absent=1;
        assert(!prefs_flush() && prefs_pending());
        unsigned char retained[256];memcpy(retained,slot_for("prefs_overlay")->committed,sizeof retained);
        card_absent=0;nvs_fail_commit=1;
        assert(!prefs_flush() && prefs_pending());
        assert(!memcmp(retained,slot_for("prefs_overlay")->committed,sizeof retained));
        nos_prefs_t checked;assert(disk_load_actual("storage-card",&checked,sizeof checked)==PREFS_DISK_OK);
        assert(checked.brightness==56 && checked.tz_min==254);
        nvs_fail_commit=0;assert(prefs_flush() && !prefs_pending());
        assert(nos_prefs.brightness==56 && nos_prefs.tz_min==254);
        puts("PASS offline faults: verified SD data alone cannot acknowledge a failed overlay tombstone; retry remains dirty until checked NVS acknowledgement succeeds");
    } else {
        assert(!strcmp(phase,"offline-after"));nvs_fixture_load();prefs_defaults();
        nos_prefs_t newer=nos_prefs;newer.brightness=77;newer.tz_min=444;newer.volume=9;
        write_raw_bank_sequence(&newer,1000);prefs_load();
        assert(nos_prefs.brightness==77 && nos_prefs.tz_min==444 && nos_prefs.volume==9);
        assert(prefs_flush() && !prefs_loading() && !prefs_pending());
        puts("PASS offline reset phase 3: acknowledged empty overlay does not replay stale edits over a subsequent newer SD checkpoint");
    }
}
static void test_observational_read_retry(void)
{
    for(int mode=0;mode<3;mode++) {
        reset_store();prefs_defaults();nos_prefs_t incoming=nos_prefs,inspected;
        incoming.brightness=37;incoming.volume=21;write_raw_bank(&incoming);
        prefs_load();assert(prefs_flush() && !prefs_pending() && !prefs_loading());
        card_absent=mode==1;card_read_error=mode!=1;
        int loaded=hal_store_load("nosprefs",&inspected,sizeof inspected);
        assert(loaded==(mode==1));
        assert(hal_store_load_pending("nosprefs") && prefs_loading() && prefs_pending());
        card_absent=card_read_error=0;
        /* A newer untouched field must survive reloading after the observation. */
        incoming.volume=11;write_raw_bank_sequence(&incoming,1000);
        if(mode!=2) {nos_prefs.brightness=62;prefs_save();}
        unsigned reads=card_reads,saves=card_saves;
        for(int attempt=0;attempt<4 && prefs_pending();attempt++) {
            atomic_fetch_add(&mock_us,6000000);prefs_tick();
        }
        assert(!prefs_pending() && !prefs_loading() && !hal_store_load_pending("nosprefs"));
        assert(card_reads>reads && card_saves>saves);
        assert(disk_load_actual("storage-card",&inspected,sizeof inspected)==PREFS_DISK_OK);
        assert(inspected.brightness==(mode==2?37:62) && inspected.volume==11);
        assert(!memcmp(&inspected,&nos_prefs,sizeof inspected));
        assert(!card_leased && !card_locked && !nvs_opened);
    }
    puts("PASS diagnostic read retry: transient query EIO/card absence adopts unresolved state; normal scheduled retries reload the healed card, merge explicit edits, preserve newer untouched fields and acknowledge without another query, including initially clean RAM");
}
int main(int argc,char **argv)
{
    if(argc==2) {test_offline_process(argv[1]);return 0;}
    assert(argc==1);
    test_overlay_adapter();test_actual_restore();test_unknown_sd();test_credential_ota_exclusion();test_absent_credential_fallback();test_observational_read_retry();mock_canaries();
    puts("PASS actual HAL + prefs + checked-bank storage integration");return 0;
}
