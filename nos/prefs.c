/* prefs.c — see prefs.h */
#include "prefs.h"
#include "hal.h"
#include "frame_cap.h"
#include <string.h>
#include <stddef.h>

#define PREFS_VERSION 4
#define PREFS_V3_BYTES 42
#define PREFS_V3_PADDED_BYTES 44
#define PREFS_V4_BYTES 52   /* V4 payload before the rev-6 body profile was appended */
_Static_assert(offsetof(nos_prefs_t,wrist_refs)+sizeof(((nos_prefs_t *)0)->wrist_refs)==PREFS_V3_BYTES,
               "preserve complete 42-byte V3 preference payload");
_Static_assert(offsetof(nos_prefs_t,wrist_revision)==PREFS_V3_PADDED_BYTES,"preserve V4 metadata alignment");
_Static_assert(offsetof(nos_prefs_t,fps_cap)==49 && offsetof(nos_prefs_t,body_birth_year)==PREFS_V4_BYTES &&
               sizeof(nos_prefs_t)==56,
               "FPS cap uses a reserved byte; the rev-6 body profile is appended after the 52-byte V4 payload");
_Static_assert(offsetof(nos_prefs_t,display_te)==50,"TE uses the next V4 reserved byte");
nos_prefs_t nos_prefs;
static int s_dirty;
static int s_persist_allowed = 1;
static uint32_t s_dirty_ms;
static uint32_t s_first_dirty_ms, s_attempt_ms;
static int s_retry;
static int s_loading;
static uint32_t s_load_ms;
static nos_prefs_t s_load_base;
static nos_prefs_t s_checkpoint_base;
enum { RESTORE_IDLE, RESTORE_FLUSHING, RESTORE_HELD, RESTORE_RELOAD };
static int s_restore_gate, s_initialized;
/* Optional checked fallback adapter. Disk-only hosts have no NVS overlay. */
extern int hal_prefs_overlay_load(void *data,int size) __attribute__((weak));
extern int hal_prefs_overlay_save_checked(const void *data,int size) __attribute__((weak));
#define EDIT_FIELDS(X) \
    X(haptics) X(haptic_strength) X(sound) X(volume) X(brightness) \
    X(auto_bright) X(aod) X(raise_wake) X(hour24) X(dnd) X(face) X(face_accent) \
    X(sleep_s) X(wifi) X(ble) X(lte) X(gps_auto) X(tz_min) X(step_goal_100) \
    X(move_goal_10) X(units_metric) X(theme) X(aod_bright) X(knock_lock) X(sensor_idle) \
    X(sports_dominant) X(sports_racket) X(sports_batting) X(sports_bowling) \
    X(sports_volleyball) X(sports_mass_kg) X(sports_auto_resume) X(wrist_refs)
typedef struct {size_t offset,size;} edit_field;
#define FIELD_DESC(f) {offsetof(nos_prefs_t,f),sizeof(((nos_prefs_t *)0)->f)},
static const edit_field edit_fields[]={EDIT_FIELDS(FIELD_DESC)};
#undef FIELD_DESC
#define EDIT_COUNT (sizeof edit_fields/sizeof edit_fields[0])
#define WRIST_EDIT (UINT64_C(1)<<EDIT_COUNT)
/* Append overlay bits without moving the existing wrist-provenance bit. */
#define FPS_EDIT (WRIST_EDIT<<1)
#define TE_EDIT (FPS_EDIT<<1)
/* The body profile joins the in-RAM merge of an unresolved boot, but the NVS overlay keeps
 * its original 52-byte value image (an old overlay of another size would read as -1 and
 * wedge preference loading); the full NVS mirror of the preferences carries the profile. */
#define BODY_BIRTH_EDIT (TE_EDIT<<1)
#define BODY_HEIGHT_EDIT (BODY_BIRTH_EDIT<<1)
#define BODY_SEX_EDIT (BODY_HEIGHT_EDIT<<1)
#define BODY_EDITS (BODY_BIRTH_EDIT|BODY_HEIGHT_EDIT|BODY_SEX_EDIT)
#define EDIT_MASK ((BODY_SEX_EDIT<<1)-1)
#define OVERLAY_MASK (EDIT_MASK&~BODY_EDITS)
typedef struct {
    uint32_t magic,version;
    uint64_t mask;
    uint32_t wrist_base;
    unsigned char value[PREFS_V4_BYTES]; /* leading bytes of nos_prefs_t, as before rev 6 */
    uint32_t check;
} preference_overlay;
_Static_assert(EDIT_COUNT<61 && sizeof(preference_overlay)==80,"bounded explicit preference overlay; persisted NVS size is fixed");
static preference_overlay s_overlay_saved;
static uint64_t s_edit_mask;
static uint32_t s_wrist_base;
static int s_overlay_known,s_overlay_saved_valid,s_ram_card_known;
static uint32_t overlay_sum(const preference_overlay *p)
{
    const unsigned char *b=(const unsigned char *)p;uint32_t h=2166136261u;
    for(size_t i=0;i<offsetof(preference_overlay,check);i++)h=(h^b[i])*16777619u;
    return h;
}
static int overlay_enabled(void)
{return hal_prefs_overlay_load && hal_prefs_overlay_save_checked;}
static void capture_edits(void)
{
    const unsigned char *value=(const unsigned char *)&nos_prefs,*base=(const unsigned char *)&s_load_base;
    for(size_t i=0;i<EDIT_COUNT;i++)if(memcmp(value+edit_fields[i].offset,base+edit_fields[i].offset,edit_fields[i].size))
        s_edit_mask|=UINT64_C(1)<<i;
    if(nos_prefs.fps_cap!=s_load_base.fps_cap)s_edit_mask|=FPS_EDIT;
    if(nos_prefs.display_te!=s_load_base.display_te)s_edit_mask|=TE_EDIT;
    if(nos_prefs.body_birth_year!=s_load_base.body_birth_year)s_edit_mask|=BODY_BIRTH_EDIT;
    if(nos_prefs.body_height_cm!=s_load_base.body_height_cm)s_edit_mask|=BODY_HEIGHT_EDIT;
    if(nos_prefs.body_sex!=s_load_base.body_sex)s_edit_mask|=BODY_SEX_EDIT;
    if(nos_prefs.wrist_left!=s_load_base.wrist_left || nos_prefs.wrist_source!=s_load_base.wrist_source ||
       nos_prefs.wrist_revision!=s_load_base.wrist_revision) {
        if(!(s_edit_mask&WRIST_EDIT))s_wrist_base=s_load_base.wrist_revision;
        s_edit_mask|=WRIST_EDIT;
    }
}
static void apply_edits(nos_prefs_t *to,const nos_prefs_t *from,uint64_t mask,uint32_t wrist_base)
{
    unsigned char *dest=(unsigned char *)to;const unsigned char *src=(const unsigned char *)from;
    for(size_t i=0;i<EDIT_COUNT;i++)if(mask&(UINT64_C(1)<<i))
        memcpy(dest+edit_fields[i].offset,src+edit_fields[i].offset,edit_fields[i].size);
    if(mask&FPS_EDIT)to->fps_cap=from->fps_cap;
    if(mask&TE_EDIT)to->display_te=from->display_te;
    if(mask&BODY_BIRTH_EDIT)to->body_birth_year=from->body_birth_year;
    if(mask&BODY_HEIGHT_EDIT)to->body_height_cm=from->body_height_cm;
    if(mask&BODY_SEX_EDIT)to->body_sex=from->body_sex;
    if(mask&WRIST_EDIT) {
        /* Retrying an acknowledged SD write after an overlay tombstone failure
         * must not manufacture another placement revision on every reboot. */
        if(to->wrist_left==from->wrist_left && to->wrist_source==from->wrist_source &&
           (int32_t)(to->wrist_revision-from->wrist_revision)>=0)return;
        uint32_t edits=from->wrist_revision-wrist_base;if(!edits)edits=1;
        uint32_t merged=to->wrist_revision+edits;
        if((int32_t)(from->wrist_revision-merged)>0)merged=from->wrist_revision;
        to->wrist_left=from->wrist_left;to->wrist_source=from->wrist_source;
        to->wrist_revision=merged?merged:1;
    }
}
static int overlay_resolve(void)
{
    if(s_overlay_known)return 1;
    if(!overlay_enabled()){s_overlay_known=1;return 1;}
    preference_overlay saved={0};int result=hal_prefs_overlay_load(&saved,sizeof saved);
    if(result<0)return 0;
    if(result==1) {
        if(saved.magic!=0x504f5631u || saved.version!=1 || (saved.mask&~OVERLAY_MASK) ||
           saved.check!=overlay_sum(&saved) || (saved.mask && saved.value[offsetof(nos_prefs_t,version)]!=PREFS_VERSION))return 0;
        uint64_t incoming=saved.mask&~s_edit_mask;
        uint32_t prior=nos_prefs.wrist_revision;
        nos_prefs_t image;memset(&image,0,sizeof image);memcpy(&image,saved.value,sizeof saved.value);
        apply_edits(&nos_prefs,&image,incoming,saved.wrist_base);
        if(incoming&WRIST_EDIT)s_wrist_base=s_ram_card_known?prior:saved.wrist_base;
        s_edit_mask|=saved.mask;s_overlay_saved=saved;s_overlay_saved_valid=1;
    }
    s_overlay_known=1;return 1;
}
static int overlay_checkpoint(int empty)
{
    if(!s_overlay_known)return 0; /* Never erase an unread overlay. */
    if(!overlay_enabled())return 1;
    uint64_t persist=s_edit_mask&OVERLAY_MASK;
    if(empty && !persist && (!s_overlay_saved_valid || !s_overlay_saved.mask))return 1;
    if(!empty && !persist)return 1;
    preference_overlay next={0};next.magic=0x504f5631u;next.version=1;
    if(!empty){next.mask=persist;memcpy(next.value,&nos_prefs,sizeof next.value);next.wrist_base=s_wrist_base;}
    next.check=overlay_sum(&next);
    if(s_overlay_saved_valid && !memcmp(&next,&s_overlay_saved,sizeof next))return 1;
    if(!hal_prefs_overlay_save_checked(&next,sizeof next))return 0;
    s_overlay_saved=next;s_overlay_saved_valid=1;return 1;
}
/* Portable tests/legacy HALs have no deferred SD state; the device overrides. */
__attribute__((weak)) int hal_store_load_pending(const char *key) { (void)key; return 0; }

void prefs_defaults(void)
{
    memset(&nos_prefs, 0, sizeof nos_prefs);
    nos_prefs.version = PREFS_VERSION;
    nos_prefs.haptics = 1;
    nos_prefs.haptic_strength = 70;
    nos_prefs.sound = 1;
    nos_prefs.volume = 60;
    nos_prefs.brightness = 70;
    nos_prefs.auto_bright = 1;
    nos_prefs.aod = 0;
    nos_prefs.raise_wake = 1;
    nos_prefs.wrist_left = 1;
    nos_prefs.hour24 = 1;
    nos_prefs.face = 0;
    nos_prefs.sleep_s = 15;
    nos_prefs.wifi = 0;              /* off by default: the phone (BLE) brings time and weather; Wi-Fi costs 6-90 mA (power review) */
    nos_prefs.ble = 1;
    nos_prefs.lte = 0;
    nos_prefs.gps_auto = 1;
    nos_prefs.tz_min = 330;          /* IST, matching the existing firmware default */
    nos_prefs.step_goal_100 = 80;    /* 8000 steps */
    nos_prefs.units_metric = 1;
    nos_prefs.aod_bright = PREFS_AOD_BRIGHT_DEF;
    nos_prefs.knock_lock = 1;
    nos_prefs.sensor_idle = 1;
    nos_prefs.sports_auto_resume = 1;
    nos_prefs.fps_cap = NOS_FRAME_CAP_DEFAULT;
    nos_prefs.rev = 6;
}

static void normalize(nos_prefs_t *p)
{
    p->version=PREFS_VERSION;
    if (p->sleep_s < 5) p->sleep_s = 5;
    if (!p->aod_bright) p->aod_bright = PREFS_AOD_BRIGHT_DEF;   /* saved before it existed */
    if (p->rev < 1) { p->knock_lock = 1; p->rev = 1; }    /* rev 1: knock to lock, on */
    if (p->rev < 2) { p->sensor_idle = 1; p->rev = 2; }
    if (p->rev < 3) {
        p->sports_dominant = p->sports_racket = 0;
        p->sports_batting = p->sports_bowling = 0;
        p->sports_volleyball = p->sports_mass_kg = 0;
        p->sports_auto_resume = 1;
        memset(p->wrist_refs, 0, sizeof p->wrist_refs);
        p->rev = 3;
    }
    if (p->sports_dominant > 2) p->sports_dominant = 0;
    if (p->sports_racket > 3) p->sports_racket = 0;
    if (p->sports_batting > 3) p->sports_batting = 0;
    if (p->sports_bowling > 2) p->sports_bowling = 0;
    if (p->sports_volleyball > 3) p->sports_volleyball = 0;
    p->sports_auto_resume = !!p->sports_auto_resume;
    if(p->wrist_source>PREFS_WRIST_SOURCE_GUIDED) p->wrist_source=PREFS_WRIST_SOURCE_UNKNOWN;
    if(p->rev<4) {p->fps_cap=NOS_FRAME_CAP_DEFAULT;p->rev=4;}
    if(!nos_frame_cap_valid(p->fps_cap))p->fps_cap=NOS_FRAME_CAP_DEFAULT;
    if(p->rev<5) {p->display_te=0;p->rev=5;}
    p->display_te=p->display_te==1;
    if(p->rev<6) {p->body_birth_year=0;p->body_height_cm=0;p->body_sex=0;p->rev=6;}
    /* Body profile: out-of-range values read as unset rather than skewing every estimate. */
    if(p->body_birth_year<1900 || p->body_birth_year>2100)p->body_birth_year=0;
    if(p->body_height_cm<100 || p->body_height_cm>230)p->body_height_cm=0;
    if(p->body_sex>2)p->body_sex=0;
    if(p->sports_mass_kg<20 || p->sports_mass_kg>250)p->sports_mass_kg=0;
}

static int load_current(nos_prefs_t *p)
{
    memset(p,0,sizeof *p);
    if(hal_store_load("nosprefs",p,sizeof *p)) return p->version==PREFS_VERSION?1:-1;
    /* The 52-byte V4 payload predates the rev-6 body profile: it loads with the profile
     * unset (the zeroed tail) and normalize() stamps rev 6. Same version, told apart by size. */
    memset(p,0,sizeof *p);
    if(hal_store_load("nosprefs",p,PREFS_V4_BYTES)) return p->version==PREFS_VERSION?1:-1;
    /* V3's largest member was uint16_t, so its persisted sizeof was 42. The
     * new uint32_t adds two alignment bytes before V4 metadata at offset44;
     * that offset is not the legacy payload size. Also accept padded V3 blobs. */
    const int legacy_sizes[]={PREFS_V3_BYTES,PREFS_V3_PADDED_BYTES};
    for(size_t i=0;i<sizeof legacy_sizes/sizeof *legacy_sizes;i++) {
        memset(p,0,sizeof *p);
        if(hal_store_load("nosprefs",p,legacy_sizes[i])) {
            if(p->version!=3)return -1;
            p->version=PREFS_VERSION; /* appended provenance remains explicitly unknown */
            p->fps_cap=NOS_FRAME_CAP_DEFAULT;
            return 1;
        }
    }
    return 0;
}

/* Preserve complete field edits made during a card-absent/unresolved boot. Byte
 * overlays would corrupt multi-byte settings when only one byte changed. */
static void merge_edits(nos_prefs_t *loaded)
{
    capture_edits();uint32_t prior=loaded->wrist_revision;
    apply_edits(loaded,&nos_prefs,s_edit_mask,s_wrist_base);
    if(s_edit_mask&WRIST_EDIT)s_wrist_base=prior;
}

static int resolve_load(void)
{
    nos_prefs_t p;
    s_load_ms=hal_millis();
    if(!overlay_resolve())return 0;
    int ok=load_current(&p);
    if(ok<0 || hal_store_load_pending("nosprefs")) return 0;
    if(ok) {
        normalize(&p);
        merge_edits(&p);
        nos_prefs=p;
        normalize(&nos_prefs);
        s_ram_card_known=1;
    }
    __atomic_store_n(&s_loading,0,__ATOMIC_RELEASE);
    return 1;
}

void prefs_load(void)
{
    nos_prefs_t p;
    int result=load_current(&p);
    if(result>0) nos_prefs=p;
    else prefs_defaults();
    normalize(&nos_prefs);
    __atomic_store_n(&s_loading,result<0 || hal_store_load_pending("nosprefs"),__ATOMIC_RELEASE);
    s_load_base=nos_prefs;
    s_checkpoint_base=nos_prefs;
    s_edit_mask=0;s_wrist_base=nos_prefs.wrist_revision;
    s_overlay_known=s_overlay_saved_valid=0;memset(&s_overlay_saved,0,sizeof s_overlay_saved);
    s_ram_card_known=result>0 && !hal_store_load_pending("nosprefs");
    if(!overlay_resolve())__atomic_store_n(&s_loading,1,__ATOMIC_RELEASE);
    __atomic_store_n(&s_initialized,1,__ATOMIC_RELEASE);
    s_load_ms=hal_millis();
    __atomic_store_n(&s_dirty,0,__ATOMIC_RELEASE);s_retry=0;
    prefs_save(); /* Migrate legacy NVS and checkpoint normalized fields to SD. */
}

void prefs_save(void) {
    capture_edits();
    s_dirty_ms = hal_millis();
    if (!__atomic_load_n(&s_dirty,__ATOMIC_ACQUIRE)) s_first_dirty_ms = s_dirty_ms;
    __atomic_store_n(&s_dirty,1,__ATOMIC_RELEASE);
}

/* A diagnostic read can discover a card fault after boot resolved successfully.
 * Adopt that HAL uncertainty on the UI owner too, so healing the card triggers
 * a fresh authoritative read and whole-field merge before another checkpoint. */
static void adopt_pending_load(void)
{
    if (__atomic_load_n(&s_loading,__ATOMIC_ACQUIRE) || !hal_store_load_pending("nosprefs")) return;
    s_load_base=s_checkpoint_base;
    __atomic_store_n(&s_loading,1,__ATOMIC_RELEASE);
    s_load_ms=hal_millis();
    prefs_save();
}

void prefs_set_persistence_allowed(int allowed) { s_persist_allowed = allowed != 0; }
int prefs_pending(void) { return __atomic_load_n(&s_dirty,__ATOMIC_ACQUIRE) ||
    hal_store_load_pending("nosprefs") ||
    __atomic_load_n(&s_restore_gate,__ATOMIC_ACQUIRE)>=RESTORE_HELD; }
int prefs_loading(void) { return __atomic_load_n(&s_loading,__ATOMIC_ACQUIRE) ||
    hal_store_load_pending("nosprefs") ||
    __atomic_load_n(&s_restore_gate,__ATOMIC_ACQUIRE)>=RESTORE_HELD; }
int prefs_disk_write_allowed(const char *directory)
{
    if(directory) {
        size_t n=strlen(directory);
        while(n && directory[n-1]=='/')n--;
        size_t start=n;while(start && directory[start-1]!='/')start--;
        if(n-start!=5)return 1;
        const char *watch="watch";
        for(size_t i=0;i<5;i++) {
            unsigned char c=(unsigned char)directory[start+i];
            if(c>='A' && c<='Z')c=(unsigned char)(c+'a'-'A');
            if(c!=(unsigned char)watch[i])return 1;
        }
    }
    return !__atomic_load_n(&s_loading,__ATOMIC_ACQUIRE) &&
        __atomic_load_n(&s_restore_gate,__ATOMIC_ACQUIRE)<RESTORE_HELD;
}
int prefs_restore_begin(void)
{
    int expected=RESTORE_IDLE;
    if(!__atomic_compare_exchange_n(&s_restore_gate,&expected,RESTORE_HELD,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 0;
    if(__atomic_load_n(&s_dirty,__ATOMIC_ACQUIRE) && !__atomic_load_n(&s_loading,__ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_restore_gate,RESTORE_IDLE,__ATOMIC_RELEASE);return 0;
    }
    return 1;
}
void prefs_restore_end(int changed)
{
    int next=changed && __atomic_load_n(&s_initialized,__ATOMIC_ACQUIRE)?RESTORE_RELOAD:RESTORE_IDLE;
    __atomic_store_n(&s_restore_gate,next,__ATOMIC_RELEASE);
}

void prefs_set_wrist(int left,int source)
{
    left=left!=0;
    if(source<PREFS_WRIST_SOURCE_UNKNOWN || source>PREFS_WRIST_SOURCE_GUIDED) source=PREFS_WRIST_SOURCE_UNKNOWN;
    if(nos_prefs.wrist_left==left && nos_prefs.wrist_source==source) return;
    nos_prefs.wrist_left=(uint8_t)left;
    nos_prefs.wrist_source=(uint8_t)source;
    if(++nos_prefs.wrist_revision==0) nos_prefs.wrist_revision=1;
    prefs_save();
}
int prefs_wrist_source(void) { return nos_prefs.wrist_source; }
uint32_t prefs_wrist_revision(void) { return nos_prefs.wrist_revision; }

int prefs_flush(void)
{
    int expected=RESTORE_IDLE,reloading=0;
    if(!__atomic_compare_exchange_n(&s_restore_gate,&expected,RESTORE_FLUSHING,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        if(expected!=RESTORE_RELOAD)return 0;
        expected=RESTORE_RELOAD;
        if(!__atomic_compare_exchange_n(&s_restore_gate,&expected,RESTORE_FLUSHING,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 0;
        reloading=1;s_load_base=s_checkpoint_base;
        __atomic_store_n(&s_loading,1,__ATOMIC_RELEASE);prefs_save();
    }
    adopt_pending_load();
    if (!prefs_pending()) {__atomic_store_n(&s_restore_gate,RESTORE_IDLE,__ATOMIC_RELEASE);return 1;}
    if(s_loading && !resolve_load()) {
        (void)overlay_checkpoint(0);
        s_retry=1;s_attempt_ms=hal_millis();
        __atomic_store_n(&s_restore_gate,reloading?RESTORE_RELOAD:RESTORE_IDLE,__ATOMIC_RELEASE);return 0;
    }
    s_attempt_ms = hal_millis();
    if (!hal_store_save_checked("nosprefs", &nos_prefs, sizeof nos_prefs)) {
        (void)overlay_checkpoint(0);
        s_retry = 1;
        __atomic_store_n(&s_restore_gate,RESTORE_IDLE,__ATOMIC_RELEASE);
        return 0;
    }
    s_checkpoint_base=nos_prefs;
    s_load_base=nos_prefs;s_ram_card_known=1;
    if(!overlay_checkpoint(1)) {
        s_retry=1;__atomic_store_n(&s_restore_gate,RESTORE_IDLE,__ATOMIC_RELEASE);return 0;
    }
    s_edit_mask=0;s_wrist_base=nos_prefs.wrist_revision;s_retry=0;
    __atomic_store_n(&s_dirty,0,__ATOMIC_RELEASE);
    __atomic_store_n(&s_restore_gate,RESTORE_IDLE,__ATOMIC_RELEASE);
    return 1;
}

void prefs_tick(void)
{
    uint32_t now = hal_millis();
    int gate=__atomic_load_n(&s_restore_gate,__ATOMIC_ACQUIRE);
    if(gate==RESTORE_HELD)return;
    if(gate==RESTORE_RELOAD) {
        if(s_persist_allowed && (!s_retry || now-s_attempt_ms>=5000))(void)prefs_flush();
        return;
    }
    adopt_pending_load();
    if(s_persist_allowed && s_loading && (uint32_t)(now-s_load_ms)>=5000) {
        (void)prefs_flush();return;
    }
    if (s_persist_allowed && s_dirty &&
        (!s_retry || now - s_attempt_ms >= 5000) &&
        (now - s_dirty_ms > 2000 || now - s_first_dirty_ms >= 10000)) {
        (void)prefs_flush();
    }
}
