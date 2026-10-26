/* Host test of the pack loader: includes the production loader, stubs only the engine.
 * argv[1] pack path, argv[2] registry file ("entry icon name" lines), prints one
 * RECORD line per descriptor for tests/test_native_asset_packing_budget.py to compare
 * with its independent Python decode, then corrupts copies of the pack and requires
 * every one to be rejected without publishing anything. */
#include "../nos/native_menu_assets.c"
#include <assert.h>
const font_t *UI_F_BODY=&nf_body,*UI_F_LABEL=&nf_label,*UI_F_TITLE=&nf_title;
uint16_t nos_c_red=0xD0C4;
void nos_layers_invalidate(void){}
void nos_history_reset(void){}
void nos_invalidate(void){}
uint32_t hal_millis(void){static uint32_t t;return t+=3;}
const char *hal_sd_root(void){return "no-such-sd-root";}
static uint8_t *slurp(const char *path,size_t *n){
    FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);*n=(size_t)ftell(f);fseek(f,0,SEEK_SET);
    uint8_t *b=malloc(*n);assert(b&&fread(b,1,*n,f)==*n);fclose(f);return b;
}
static void fix_crc(uint8_t *f,size_t n){
    nm_hdr h;memcpy(&h,f,sizeof h);
    h.payload_crc=nm_crc(f+sizeof h,n-sizeof h);memcpy(f,&h,sizeof h);
    h.hdr_crc=nm_crc(f,sizeof h-4);memcpy(f,&h,sizeof h);
}
static void expect_reject(const uint8_t *f,size_t n,const char *what,const char *reason){
    assert(!nm_publish(f,n,"test"));
    printf("REJECT %s: %s\n",what,nm_why);
    assert(!NM.store && !NM.rec && !NM.n_rec);
    assert(!reason || strstr(nm_why,reason));
}
int main(int argc,char **argv){
    assert(argc==3);
    size_t n;uint8_t *good=slurp(argv[1],&n);
    nm_hdr h;memcpy(&h,good,sizeof h);
    /* Corrupt copies first: each must fail with nothing published. */
    uint8_t *bad=malloc(n);
    expect_reject(good,10,"truncated header","short");
    expect_reject(good,n-1,"truncated file","size");
    memcpy(bad,good,n);bad[n-5]^=1;expect_reject(bad,n,"payload bit flip","payload crc");
    memcpy(bad,good,n);bad[1]^=1;expect_reject(bad,n,"magic","magic");
    memcpy(bad,good,n);bad[40]^=1;expect_reject(bad,n,"header crc","header crc");
    memcpy(bad,good,n);bad[offsetof(nm_hdr,build)+3]^=1;
    {nm_hdr t;memcpy(&t,bad,sizeof t);t.hdr_crc=nm_crc(bad,sizeof t-4);memcpy(bad,&t,sizeof t);}
    expect_reject(bad,n,"stale build binding","stale");
    memcpy(bad,good,n);{nm_hdr t;memcpy(&t,bad,sizeof t);t.version=2;t.hdr_crc=nm_crc(bad,sizeof t-4);memcpy(bad,&t,sizeof t);}
    expect_reject(bad,n,"future version","magic");
    /* Valid CRCs but damaged content: the per-layer checks must still catch it. */
    memcpy(bad,good,n);bad[h.body_off+h.body_bytes/2]^=0x55;fix_crc(bad,n);
    assert(!nm_publish(bad,n,"test"));printf("REJECT damaged body: %s\n",nm_why);assert(!NM.store);
    memcpy(bad,good,n);bad[h.pal_off+4]^=0x20;fix_crc(bad,n);expect_reject(bad,n,"damaged palette",NULL);
    memcpy(bad,good,n);{ /* record points at the wrong body size */
        nm_rec r;memcpy(&r,bad+h.meta_off,sizeof r);r.image.bytes+=1;memcpy(bad+h.meta_off,&r,sizeof r);}
    fix_crc(bad,n);expect_reject(bad,n,"record geometry",NULL);
    memcpy(bad,good,n);{ /* index table names the wrong record */
        unsigned o[8];nm_layout(h.n_records,h.n_apps,h.reg_str_bytes,o);
        uint16_t v;unsigned at=h.meta_off+o[1];
        for(unsigned i=0;i<36*18;i++){memcpy(&v,bad+at+i*2,2);if(v!=NM_NONE){v=(uint16_t)((v+1)%h.n_records);memcpy(bad+at+i*2,&v,2);break;}}}
    fix_crc(bad,n);expect_reject(bad,n,"index mismatch","inconsistent");
    memcpy(bad,good,n);{nm_body b;memcpy(&b,bad+h.btab_off+sizeof b,sizeof b);b.hash^=1;memcpy(bad+h.btab_off+sizeof b,&b,sizeof b);}
    fix_crc(bad,n);expect_reject(bad,n,"body hash field","hash");
    memcpy(bad,good,n);{nm_hdr t;memcpy(&t,bad,sizeof t);t.arena_bytes=NM_ARENA_MAX+1;t.hdr_crc=nm_crc(bad,sizeof t-4);memcpy(bad,&t,sizeof t);}
    fix_crc(bad,n);expect_reject(bad,n,"oversized arena","counts");
    free(bad);
    /* Real load through the host search path (NOS_MENU_PACK). */
    char env[600];snprintf(env,sizeof env,"NOS_MENU_PACK=%s",argv[1]);putenv(env);
    assert(native_menu_assets_compatible());
    native_menu_pack_info pi;native_menu_pack_status(&pi);
    assert(pi.state==1 && pi.records==h.n_records && pi.arena_bytes==h.arena_bytes && pi.sharp==(h.flags&1));
    printf("PACK sharp=%d records=%u bodies=%u arena=%u file=%u\n",pi.sharp,pi.records,pi.bodies,(unsigned)pi.arena_bytes,(unsigned)pi.file_bytes);
    for(unsigned f=0;f<3;f++)for(unsigned e=0;e<(f?NM.n_apps:36);e++)for(unsigned l=0;l<(f==0?2u:f==1?3u:1u);l++)for(unsigned v=0;v<9;v++){
        const native_asset_desc *d=native_menu_asset(f,e,l,v);
        if(!d)continue;
        sprite8 s=native_menu_sprite(d);assert(s.px&&s.w==d->w&&s.h==d->h);
        printf("RECORD %u %u %u %u %d %d %d %d %u %08x %08x\n",f,e,l,v,d->w,d->h,d->x,d->y,d->bytes,d->revision,nm_fnv(s.px,d->bytes));
    }
    assert(!native_menu_asset(0,36,0,0) && !native_menu_asset(1,NM.n_apps,0,0) && !native_menu_asset(2,0,1,0) && !native_menu_asset(0,0,0,9));
    assert(native_menu_sprite(NULL).px==NULL);
    native_asset_desc fake={0,1,0,1,1,0,0};fake.offset=pi.arena_bytes;assert(!native_menu_sprite(&fake).px);
    for(int level=0;level<=64;level++)assert(native_menu_settings_palette(0,level)==native_menu_settings_palette(0,0)+level*256);
    {
        const uint8_t *pal=(const uint8_t *)NM.pal;
        printf("PAL %08x %08x %08x %08x %08x\n",nm_fnv(pal,33280),nm_fnv(pal+33280,33280),nm_fnv(pal+2*33280,33280),
               nm_fnv(pal+3*33280,33280),nm_fnv(pal+4*33280,512));
        assert((const uint8_t *)native_menu_settings_palette(0,0)==pal && (const uint8_t *)native_menu_launcher_palette(0,0)==pal+3*33280);
        assert((const uint8_t *)native_menu_launcher_palette(7,1)==pal+4*33280);
        nos_c_red=0x2F3B;assert((const uint8_t *)native_menu_settings_palette(1,5)==pal+2*33280+5*512);
        nos_c_red=0xD0C4;assert((const uint8_t *)native_menu_settings_palette(1,5)==pal+33280+5*512);
        nos_c_red=0x1234;assert(!native_menu_settings_palette(1,5));nos_c_red=0xD0C4;
        assert(!native_menu_settings_palette(0,65) && !native_menu_launcher_palette(-1,0));
    }
    printf("CURVE %08x\n",nm_fnv((const uint8_t *)native_menu_launcher_curve(),1300));
    FILE *r=fopen(argv[2],"r");assert(r);char name[80];unsigned e;int icon;
    while(fscanf(r,"%u %d %79[^\n]",&e,&icon,name)==3){
        assert(native_menu_launcher_matches(e,name,icon));
        assert(!native_menu_launcher_matches(e,"different",icon) && !native_menu_launcher_matches(e,name,icon+1000));
    }
    fclose(r);
    assert(!native_menu_launcher_matches(NM.n_apps,"x",0));
    /* Font identity and the diagnostic switch still gate every accessor. */
    UI_F_BODY=&nf_title;assert(!native_menu_assets_compatible() && !native_menu_asset(2,0,0,0) && !native_menu_launcher_curve());UI_F_BODY=&nf_body;
    nos_native_menu_rom_set(0);assert(!native_menu_assets_compatible() && !native_menu_settings_palette(0,0));nos_native_menu_rom_set(1);
    assert(native_menu_assets_compatible() && native_menu_assets_reload());
    puts("PASS native menu pack loader");
    free(good);return 0;
}
