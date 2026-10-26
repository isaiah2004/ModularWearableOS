#include "prefs.h"
#include "hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t now;
static int attempts, fail, has_saved, saved_size, load_pending;
static nos_prefs_t saved;
uint32_t hal_millis(void) { return now; }
int hal_store_load(const char *key, void *out, int size)
{ assert(!strcmp(key,"nosprefs"));if(!has_saved || size!=saved_size)return 0;memcpy(out,&saved,(size_t)size);return 1; }
int hal_store_load_pending(const char *key) {assert(!strcmp(key,"nosprefs"));return load_pending;}
int hal_store_save_checked(const char *key, const void *data, int size)
{
    assert(!strcmp(key, "nosprefs") && data == &nos_prefs && size == sizeof nos_prefs);
    attempts++; if(fail)return 0;memcpy(&saved,data,sizeof saved);has_saved=1;saved_size=sizeof saved;return 1;
}
int main(void)
{
    prefs_load(); assert(!nos_prefs.display_te);prefs_set_persistence_allowed(0);
    now = 100; prefs_save(); now = 3000; prefs_tick(); assert(!attempts);
    prefs_set_persistence_allowed(1); fail = 1;
    prefs_tick(); assert(attempts == 1 && prefs_pending());
    fail = 0; prefs_tick(); assert(attempts == 1); /* retry is rate limited */
    now = 8000; prefs_tick(); assert(attempts == 2 && !prefs_pending());
    prefs_tick(); assert(attempts == 2); /* success clears pending */
    now = 4000; prefs_save(); now = 5000; prefs_save();
    now = 6001; prefs_tick(); assert(attempts == 2); /* debounce latest change */
    now = 7001; prefs_tick(); assert(attempts == 3);
    now = UINT32_MAX - 1000; prefs_save(); now = 1100;
    prefs_tick(); assert(attempts == 4); /* wrap-safe clock */
    prefs_set_persistence_allowed(0);
    now = 1200; prefs_save(); fail = 1;
    assert(!prefs_flush() && attempts == 5); /* shutdown bypasses window/debounce */
    fail = 0;
    assert(prefs_flush() && attempts == 6); /* failure retains dirty data */
    assert(prefs_flush() && attempts == 6); /* clean checkpoint does not write */
    prefs_tick(); assert(attempts == 6);
    /* Settings/control-center changes survive an awake save and reboot. */
    nos_prefs.brightness=43;nos_prefs.dnd=1;nos_prefs.wifi=1;nos_prefs.face=8;
    nos_prefs.sensor_idle=0;nos_prefs.aod_bright=7;nos_prefs.fps_cap=48;nos_prefs.display_te=1;
    now=20000;prefs_save();prefs_set_persistence_allowed(1);now=22001;prefs_tick();
    nos_prefs_t expected=nos_prefs;memset(&nos_prefs,0,sizeof nos_prefs);prefs_load();
    assert(!memcmp(&nos_prefs,&expected,sizeof expected));
    /* A failed write never clears the pending change or corrupts prior NVS. */
    nos_prefs.brightness=44;prefs_save();fail=1;assert(!prefs_flush() && prefs_pending());
    assert(saved.brightness==43);fail=0;assert(prefs_flush() && !prefs_pending());
    memset(&nos_prefs,0,sizeof nos_prefs);prefs_load();assert(nos_prefs.brightness==44);
    assert(prefs_flush()); /* startup migration checkpoint */
    int before=attempts;
    now=30000;prefs_save();
    for(now=31000;now<40000;now+=1000){prefs_save();prefs_tick();}
    assert(attempts==before);
    prefs_save();prefs_tick();assert(attempts==before+1); /* bounded continuous edits */
    /* Rev 3 reuses the reserved payload, preserving the owner's existing fields. */
    saved.rev=2;saved.wrist_left=0;saved.sensor_idle=0;saved.brightness=37;
    saved.sports_racket=255;saved.sports_auto_resume=0;saved.display_te=255;
    memset(saved.wrist_refs,0xff,sizeof saved.wrist_refs);
    prefs_load();
    assert(nos_prefs.rev==6 && nos_prefs.fps_cap==60 && !nos_prefs.display_te && !nos_prefs.wrist_left && !nos_prefs.sensor_idle && nos_prefs.brightness==37);
    assert(!nos_prefs.sports_racket && nos_prefs.sports_auto_resume==1 && !nos_prefs.wrist_refs[0]);
    nos_prefs.sports_racket=2;nos_prefs.sports_batting=1;nos_prefs.sports_bowling=2;
    nos_prefs.sports_volleyball=1;nos_prefs.sports_mass_kg=73;nos_prefs.sports_auto_resume=0;
    nos_prefs.wrist_refs[0]=0x81;nos_prefs.wrist_refs[1]=90;
    nos_prefs.wrist_refs[2]=0x82;nos_prefs.wrist_refs[3]=88;
    prefs_save();fail=1;assert(!prefs_flush() && prefs_pending());
    fail=0;assert(prefs_flush());expected=nos_prefs;
    memset(&nos_prefs,0,sizeof nos_prefs);prefs_load();
    assert(!memcmp(&expected,&nos_prefs,sizeof expected));
    /* V3 migration preserves the entire legacy prefix, then appends explicit
     * unknown provenance. A newer incompatible bank is tested by prefs_disk. */
    saved.version=3;saved_size=42;saved.brightness=37;saved.sports_racket=2;
    prefs_load();assert(nos_prefs.version==4 && nos_prefs.brightness==37 && nos_prefs.sports_racket==2);
    assert(prefs_wrist_source()==PREFS_WRIST_SOURCE_UNKNOWN && !prefs_wrist_revision());
    assert(prefs_flush() && saved_size==sizeof nos_prefs);
    /* Rev 3 reserved zeros mean the old 60 Hz default, not Unlimited. */
    saved.rev=3;saved.fps_cap=0;prefs_load();assert(nos_prefs.fps_cap==60 && nos_prefs.rev==6);
    assert(prefs_flush());
    for(unsigned i=0;i<6;i++) {
        static const unsigned caps[]={24,30,40,48,60,0};
        nos_prefs.fps_cap=(uint8_t)caps[i];prefs_save();fail=1;
        assert(!prefs_flush() && prefs_pending());fail=0;assert(prefs_flush());
        prefs_load();assert(nos_prefs.fps_cap==caps[i]);assert(prefs_flush());
    }
    saved.fps_cap=255;prefs_load();assert(nos_prefs.fps_cap==60);assert(prefs_flush());
    /* Rev 4's unused byte must migrate off, regardless of its previous contents. */
    saved.rev=4;saved.display_te=1;prefs_load();assert(nos_prefs.rev==6 && !nos_prefs.display_te);
    assert(prefs_flush());
    for(unsigned i=0;i<2;i++) {
        nos_prefs.display_te=(uint8_t)(1-i);prefs_save();fail=1;
        assert(!prefs_flush() && prefs_pending());fail=0;assert(prefs_flush());
        prefs_load();assert(nos_prefs.display_te==1-i);assert(prefs_flush());
    }
    saved.display_te=255;prefs_load();assert(!nos_prefs.display_te);assert(prefs_flush());
    /* Accept explicitly padded V3 blobs too, without treating padding as new metadata. */
    saved.version=3;saved_size=44;prefs_load();
    assert(nos_prefs.version==4 && nos_prefs.brightness==37 && !prefs_loading());
    assert(prefs_wrist_source()==PREFS_WRIST_SOURCE_UNKNOWN && !prefs_wrist_revision());
    assert(prefs_flush() && saved_size==sizeof nos_prefs);
    prefs_set_wrist(0,PREFS_WRIST_SOURCE_MANUAL);assert(prefs_flush());
    uint32_t revision=prefs_wrist_revision();assert(revision);
    prefs_load();assert(!nos_prefs.wrist_left && prefs_wrist_revision()==revision && prefs_wrist_source()==PREFS_WRIST_SOURCE_MANUAL);
    prefs_set_wrist(0,PREFS_WRIST_SOURCE_GUIDED);assert(prefs_wrist_revision()==revision+1);
    assert(prefs_flush());
    /* Unreadable/absent authoritative SD must never be replaced with defaults.
     * Preserve complete user edits while loading other durable fields later. */
    has_saved=0;load_pending=1;prefs_load();assert(prefs_loading());
    nos_prefs.brightness=45;nos_prefs.tz_min=360;nos_prefs.fps_cap=30;nos_prefs.display_te=1;prefs_save();
    before=attempts;assert(!prefs_flush() && attempts==before && prefs_pending());
    saved=nos_prefs;saved.version=4;saved.brightness=62;saved.tz_min=-240;saved.rev=3;saved.fps_cap=0;saved.display_te=0;
    saved.sports_racket=2;saved.sensor_idle=0;saved.wrist_left=0;
    saved.wrist_source=PREFS_WRIST_SOURCE_MANUAL;saved.wrist_revision=30;
    has_saved=1;saved_size=sizeof saved;load_pending=0;
    assert(prefs_flush() && !prefs_loading());
    assert(nos_prefs.brightness==45 && nos_prefs.tz_min==360 && nos_prefs.sports_racket==2 && nos_prefs.fps_cap==30 && nos_prefs.display_te==1);
    assert(!nos_prefs.sensor_idle && !nos_prefs.wrist_left && prefs_wrist_revision()==30);
    /* Rev 6 body profile: the 52-byte V4 payload loads with the profile unset (the zeroed tail),
     * keeps every older field, stamps rev 6 and re-checkpoints at the full size. */
    saved.version=4;saved.rev=5;saved.brightness=41;saved.sports_mass_kg=82;saved.sports_racket=1;
    saved.body_birth_year=1999;saved.body_height_cm=180;saved.body_sex=1; /* beyond 52 bytes: not read */
    saved_size=52;has_saved=1;
    prefs_load();
    assert(nos_prefs.rev==6 && nos_prefs.version==4 && nos_prefs.brightness==41 && nos_prefs.sports_mass_kg==82 && nos_prefs.sports_racket==1);
    assert(!nos_prefs.body_birth_year && !nos_prefs.body_height_cm && !nos_prefs.body_sex && !prefs_loading());
    assert(prefs_flush() && saved_size==sizeof nos_prefs && sizeof nos_prefs==56);
    /* The profile round-trips, including through a failed write that stays dirty. */
    nos_prefs.body_birth_year=1991;nos_prefs.body_height_cm=172;nos_prefs.body_sex=2;nos_prefs.sports_mass_kg=64;
    prefs_save();fail=1;assert(!prefs_flush() && prefs_pending());fail=0;assert(prefs_flush());
    expected=nos_prefs;memset(&nos_prefs,0,sizeof nos_prefs);prefs_load();
    assert(!memcmp(&expected,&nos_prefs,sizeof expected) && nos_prefs.body_birth_year==1991 && nos_prefs.body_height_cm==172 && nos_prefs.body_sex==2);
    /* Out-of-range profile values read as unset instead of skewing every estimate. */
    saved.body_birth_year=1850;saved.body_height_cm=40;saved.body_sex=9;saved.sports_mass_kg=255;saved_size=sizeof saved;
    prefs_load();assert(!nos_prefs.body_birth_year && !nos_prefs.body_height_cm && !nos_prefs.body_sex && !nos_prefs.sports_mass_kg);
    saved.body_birth_year=2100;saved.body_height_cm=230;saved.body_sex=2;saved.sports_mass_kg=250;
    prefs_load();assert(nos_prefs.body_birth_year==2100 && nos_prefs.body_height_cm==230 && nos_prefs.body_sex==2 && nos_prefs.sports_mass_kg==250);
    assert(prefs_flush());
    /* Profile edits made while the card is unresolved merge as whole fields with the later SD load. */
    has_saved=0;load_pending=1;prefs_load();assert(prefs_loading());
    nos_prefs.body_height_cm=168;nos_prefs.body_sex=2;prefs_save();
    saved=nos_prefs;saved.body_height_cm=190;saved.body_birth_year=1980;saved.body_sex=1;saved.brightness=77;
    saved_size=sizeof saved;has_saved=1;load_pending=0;
    assert(prefs_flush() && !prefs_loading());
    assert(nos_prefs.body_height_cm==168 && nos_prefs.body_sex==2 && nos_prefs.brightness==77 && nos_prefs.body_birth_year==1980);
    puts("NOS preferences: debounce, failure retry, awake save and reboot round-trip passed"); return 0;
}
