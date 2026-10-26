/* Native menu artwork loader. The pixels live in a pack file written by
 * tools/generate_native_menu_assets.py (format: docs/SD_ASSETS.md):
 *   B watch   /sdcard/menu/native_menu_sharp.pack via the asset worker (level 0 only)
 *   1.43C     the same pack embedded in the image (NOS_MENU_PACK_EMBEDDED, no SD slot)
 *   host/sim  native_menu_full.pack (all blur levels) from a file
 * Any failure leaves compatible()==0, so the consumers' dynamic bake runs. */
#include "native_menu_assets.h"
#include "native_menu_assets_id.h"
#include "ui.h"
#include "nos.h"
#include "fonts/nos_fonts.h"
#include "render_layers.h"
#include "render_history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "miniz.h"
#include "asset.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define NM_ALLOC(n) hal_big_alloc(n) /* tracked (psram.h) when NOS_PSRAM_TRACKING: the arena shows in `mem` and the budget */
#define NM_FREE(p) hal_big_free(p)
#define NM_ARENA_MAX 1500000u
#define NM_WAIT_MS 1500u
#define NM_DRAIN_MS 1000u
#else
#define NM_ALLOC(n) malloc(n)
#define NM_FREE(p) free(p)
#define NM_ARENA_MAX 16000000u
#endif
#define NM_FILE_MAX 4000000u
#define NM_PAL_BYTES (33280u*4+512u)
#define NM_NONE 65535u
#define NM_PL 16640u /* uint16 entries in one 65x256 palette */
typedef struct {uint8_t family,entry,layout,level;native_asset_desc image;} nm_rec;
typedef struct {uint32_t stored,arena,bytes,clen,hash,stride;} nm_body;
typedef struct {
    char magic[4];uint16_t version,flags;
    uint32_t hdr_bytes,file_bytes,payload_crc,n_records,n_bodies,n_apps,reg_str_bytes,meta_off,meta_bytes,pal_off,pal_clen,pal_fnv,
             btab_off,body_off,body_bytes,arena_bytes,l0_bodies,l0_arena,l0_stored;
    uint8_t build[32],blob[32];uint32_t hdr_crc;
} nm_hdr;
typedef char nm_hdr_size_check[sizeof(nm_hdr)==152?1:-1];
typedef char nm_rec_size_check[sizeof(nm_rec)==24?1:-1];
#ifdef NOS_MENU_PACK_EMBEDDED
extern const uint8_t _binary_native_menu_sharp_pack_start[],_binary_native_menu_sharp_pack_end[];
#endif

static struct {
    uint8_t *store,*arena;const nm_rec *rec;const uint16_t *idx_s,*idx_i,*idx_n,*reg_name,*pal;const int16_t *curve,*reg_icon;const char *str;
    unsigned n_rec,n_apps;
} NM;
static native_menu_pack_info nm_info={0,"","",0,0,0,0,0,0};
static char nm_err[64];
static const char *nm_why;

static uint32_t nm_crc(const uint8_t *p,size_t n) {
    static uint32_t t[256];static int ready;
    if(!ready){for(uint32_t i=0;i<256;i++){uint32_t v=i;for(int k=0;k<8;k++)v=v&1?0xedb88320u^(v>>1):v>>1;t[i]=v;}ready=1;}
    uint32_t c=~0u;while(n--)c=t[(c^*p++)&255]^(c>>8);
    return ~c;
}
static uint32_t nm_fnv(const uint8_t *p,size_t n) {
    uint32_t h=2166136261u;
    while(n--)h=(h^*p++)*16777619u;
    return h;
}
#ifdef ESP_PLATFORM
static int nm_inflate(void *state,const uint8_t *src,size_t sn,uint8_t *dst,size_t dn) {
    tinfl_init((tinfl_decompressor *)state);
    size_t s=sn,d=dn;
    tinfl_status r=tinfl_decompress((tinfl_decompressor *)state,src,&s,dst,dst,&d,
        TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return r==TINFL_STATUS_DONE && s==sn && d==dn;
}
#define NM_STATE_BYTES sizeof(tinfl_decompressor)
#else
/* Small zlib inflater (puff-style, bit at a time) for host tools; the device uses the ROM tinfl. */
typedef struct {const uint8_t *s;size_t n,p;uint32_t bb;int bc;uint8_t *d;size_t dn,dp;int err;} nm_inf;
typedef struct {uint16_t cnt[16],sym[288];} nm_huff;
static unsigned nm_bits(nm_inf *z,int n) {
    while(z->bc<n){if(z->p>=z->n){z->err=1;return 0;}z->bb|=(uint32_t)z->s[z->p++]<<z->bc;z->bc+=8;}
    unsigned v=z->bb&((1u<<n)-1);z->bb>>=n;z->bc-=n;return v;
}
static void nm_huff_make(nm_huff *h,const uint8_t *len,int n) {
    uint16_t offs[16];memset(h->cnt,0,sizeof h->cnt);
    for(int i=0;i<n;i++)h->cnt[len[i]]++;
    h->cnt[0]=0;offs[1]=0;
    for(int i=1;i<15;i++)offs[i+1]=(uint16_t)(offs[i]+h->cnt[i]);
    for(int i=0;i<n;i++)if(len[i])h->sym[offs[len[i]]++]=(uint16_t)i;
}
static int nm_huff_dec(nm_inf *z,const nm_huff *h) {
    int code=0,first=0,index=0;
    for(int l=1;l<=15;l++){
        code|=(int)nm_bits(z,1);int c=h->cnt[l];
        if(code-c<first)return h->sym[index+(code-first)];
        index+=c;first+=c;first<<=1;code<<=1;
    }
    return -1;
}
static int nm_codes(nm_inf *z,const nm_huff *lc,const nm_huff *dc) {
    static const uint16_t lb[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint16_t le[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t db[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint16_t de[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for(;;){
        int s=nm_huff_dec(z,lc);
        if(s<0||z->err)return 1;
        if(s<256){if(z->dp>=z->dn)return 1;z->d[z->dp++]=(uint8_t)s;}
        else if(s==256)return 0;
        else{
            s-=257;if(s>=29)return 1;
            unsigned len=lb[s]+nm_bits(z,le[s]);int ds=nm_huff_dec(z,dc);
            if(ds<0||ds>=30)return 1;
            unsigned dist=db[ds]+nm_bits(z,de[ds]);
            if(z->err||dist>z->dp||len>z->dn-z->dp)return 1;
            for(;len--;z->dp++)z->d[z->dp]=z->d[z->dp-dist];
        }
    }
}
static int nm_dynamic(nm_inf *z) {
    static const uint8_t ord[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    unsigned nl=nm_bits(z,5)+257,nd=nm_bits(z,5)+1,nc=nm_bits(z,4)+4;
    uint8_t len[320];nm_huff cl,lit,dist;
    if(nl>286||nd>30)return 1;
    memset(len,0,sizeof len);
    for(unsigned i=0;i<nc;i++)len[ord[i]]=(uint8_t)nm_bits(z,3);
    nm_huff_make(&cl,len,19);
    memset(len,0,sizeof len);
    for(unsigned i=0;i<nl+nd;){
        int sym=nm_huff_dec(z,&cl);unsigned rep;uint8_t v=0;
        if(sym<0||z->err)return 1;
        if(sym<16){len[i++]=(uint8_t)sym;continue;}
        if(sym==16){if(!i)return 1;v=len[i-1];rep=3+nm_bits(z,2);}
        else if(sym==17)rep=3+nm_bits(z,3);
        else rep=11+nm_bits(z,7);
        if(i+rep>nl+nd)return 1;
        while(rep--)len[i++]=v;
    }
    nm_huff_make(&lit,len,(int)nl);nm_huff_make(&dist,len+nl,(int)nd);
    return nm_codes(z,&lit,&dist);
}
static int nm_inflate(void *state,const uint8_t *src,size_t sn,uint8_t *dst,size_t dn) {
    (void)state;
    if(sn<6||(src[0]&15)!=8||((src[0]<<8|src[1])%31)||(src[1]&32))return 0;
    nm_inf z={src,sn-4,2,0,0,dst,dn,0,0};int last;
    do{
        last=(int)nm_bits(&z,1);unsigned t=nm_bits(&z,2);int e;
        if(t==0){
            z.bb=0;z.bc=0;
            if(z.p+4>z.n)return 0;
            unsigned len=src[z.p]|src[z.p+1]<<8,inv=src[z.p+2]|src[z.p+3]<<8;
            z.p+=4;
            if(len!=(~inv&0xffffu)||z.p+len>z.n||len>dn-z.dp)return 0;
            memcpy(dst+z.dp,src+z.p,len);z.p+=len;z.dp+=len;e=0;
        }else if(t==1){
            uint8_t len[320];nm_huff lit,dist;
            for(int i=0;i<288;i++)len[i]=i<144?8:i<256?9:i<280?7:8;
            nm_huff_make(&lit,len,288);
            for(int i=0;i<30;i++)len[i]=5;
            nm_huff_make(&dist,len,30);
            e=nm_codes(&z,&lit,&dist);
        }else if(t==2)e=nm_dynamic(&z);
        else return 0;
        if(e||z.err)return 0;
    }while(!last);
    uint32_t a=1,b=0;
    for(size_t i=0;i<dn;i++){a=(a+dst[i])%65521u;b=(b+a)%65521u;}
    const uint8_t *t=src+sn-4;
    return z.dp==dn && (b<<16|a)==((uint32_t)t[0]<<24|t[1]<<16|t[2]<<8|t[3]);
}
#define NM_STATE_BYTES 1u
#endif

static unsigned nm_layout(unsigned nr,unsigned na,unsigned rs,unsigned o[8]) {
    o[0]=0;o[1]=nr*24;o[2]=o[1]+36*2*9*2;o[3]=o[2]+na*3*9*2;o[4]=o[3]+na*9*2;
    o[5]=o[4]+650*2;o[6]=o[5]+na*2;o[7]=o[6]+na*2;
    return (o[7]+rs+3)&~3u;
}
static int nm_check_tables(unsigned sharp) {
    for(unsigned i=0;i<NM.n_rec;i++){
        const nm_rec *r=&NM.rec[i];
        if(r->family>2||r->level>8||(sharp&&r->level)||r->image.w<=0||r->image.h<=0||
           (uint64_t)(unsigned)r->image.w*(unsigned)r->image.h!=r->image.bytes)return 0;
    }
    for(unsigned i=0;i<36*18;i++){
        unsigned v=NM.idx_s[i];
        if(v!=NM_NONE&&(v>=NM.n_rec||NM.rec[v].family!=0||NM.rec[v].entry!=i/18||NM.rec[v].layout!=(i/9)%2||NM.rec[v].level!=i%9))return 0;
    }
    for(unsigned i=0;i<NM.n_apps*27;i++){
        unsigned v=NM.idx_i[i];
        if(v!=NM_NONE&&(v>=NM.n_rec||NM.rec[v].family!=1||NM.rec[v].entry!=i/27||NM.rec[v].layout!=(i/9)%3||NM.rec[v].level!=i%9))return 0;
    }
    for(unsigned i=0;i<NM.n_apps*9;i++){
        unsigned v=NM.idx_n[i];
        if(v!=NM_NONE&&(v>=NM.n_rec||NM.rec[v].family!=2||NM.rec[v].entry!=i/9||NM.rec[v].layout!=0||NM.rec[v].level!=i%9))return 0;
    }
    return 1;
}
/* Validate every layer (header, CRC, build binding, section bounds, record/index
 * consistency, per-body FNV) then decode into one arena. Nothing is published on failure. */
static int nm_publish(const uint8_t *f,size_t n,const char *source) {
    nm_hdr h;uint8_t *store=NULL;void *state=NULL;unsigned lay[8],meta;
    uint32_t t0=hal_millis();
    nm_why="short pack";
    if(n<sizeof h)goto fail;
    memcpy(&h,f,sizeof h);
    nm_why="bad magic/version";
    if(memcmp(h.magic,"NMAP",4)||h.version!=1||h.hdr_bytes!=sizeof h)goto fail;
    nm_why="header crc";
    if(h.hdr_crc!=nm_crc(f,sizeof h-4))goto fail;
    nm_why="size mismatch";
    if(h.file_bytes!=n||n>NM_FILE_MAX)goto fail;
    nm_why="payload crc";
    if(h.payload_crc!=nm_crc(f+h.hdr_bytes,n-h.hdr_bytes))goto fail;
    nm_why="stale pack: regenerate and resync";
    {char hex[65];for(int i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",h.build[i]);if(strcmp(hex,NATIVE_MENU_BUILD_SHA256))goto fail;}
    nm_why="bad counts";
    if(!h.n_records||h.n_records>4096||!h.n_bodies||h.n_bodies>4096||!h.n_apps||h.n_apps>64||h.reg_str_bytes>4096||
       h.arena_bytes>NM_ARENA_MAX||h.l0_bodies>h.n_bodies||(h.flags&~1u))goto fail;
    meta=nm_layout(h.n_records,h.n_apps,h.reg_str_bytes,lay);
    nm_why="bad sections";
    if(h.meta_off!=h.hdr_bytes||h.meta_bytes!=meta||h.pal_off!=h.meta_off+h.meta_bytes||h.btab_off<h.pal_off||
       h.pal_clen>h.btab_off-h.pal_off||h.body_off!=h.btab_off+h.n_bodies*sizeof(nm_body)||h.body_off+h.body_bytes!=n)goto fail;
    nm_why="PSRAM allocation failed";
    store=NM_ALLOC(meta+NM_PAL_BYTES+h.arena_bytes);
    state=NM_ALLOC(NM_STATE_BYTES);
    if(!store||!state)goto fail;
    memcpy(store,f+h.meta_off,meta);
    NM.store=store;NM.rec=(const nm_rec *)store;NM.n_rec=h.n_records;NM.n_apps=h.n_apps;
    NM.idx_s=(const uint16_t *)(store+lay[1]);NM.idx_i=(const uint16_t *)(store+lay[2]);NM.idx_n=(const uint16_t *)(store+lay[3]);
    NM.curve=(const int16_t *)(store+lay[4]);NM.reg_icon=(const int16_t *)(store+lay[5]);NM.reg_name=(const uint16_t *)(store+lay[6]);
    NM.str=(const char *)(store+lay[7]);NM.pal=(const uint16_t *)(store+meta);NM.arena=store+meta+NM_PAL_BYTES;
    nm_why="inconsistent record/index tables";
    if(!nm_check_tables(h.flags&1))goto fail;
    nm_why="registry strings";
    if(h.reg_str_bytes&&NM.str[h.reg_str_bytes-1])goto fail;
    for(unsigned i=0;i<h.n_apps;i++)if(NM.reg_name[i]>=h.reg_str_bytes)goto fail;
    nm_why="palette";
    {
        uint8_t *pal=store+meta;
        if(!nm_inflate(state,f+h.pal_off,h.pal_clen,pal,NM_PAL_BYTES)||nm_fnv(pal,NM_PAL_BYTES)!=h.pal_fnv)goto fail;
    }
    {
        uint8_t *arena=store+meta+NM_PAL_BYTES;uint32_t pos=0,stored=0;
        for(unsigned i=0;i<h.n_bodies;i++){
            nm_body b;memcpy(&b,f+h.btab_off+i*sizeof b,sizeof b);
            nm_why="body table";
            if(b.arena!=pos||b.stored!=stored||!b.bytes||b.bytes>h.arena_bytes-pos||(b.stride&&(b.stride>b.bytes||b.bytes%b.stride)))goto fail;
            uint32_t span=b.clen?b.clen:b.bytes;
            if(span>h.body_bytes-stored)goto fail;
            const uint8_t *src=f+h.body_off+stored;uint8_t *dst=arena+pos;
            nm_why="body decode";
            if(b.clen){if(!nm_inflate(state,src,b.clen,dst,b.bytes))goto fail;}
            else memcpy(dst,src,b.bytes);
            if(b.stride)for(uint32_t j=b.stride;j<b.bytes;j++)dst[j]=(uint8_t)(dst[j]+dst[j-b.stride]);
            nm_why="body hash";
            if(nm_fnv(dst,b.bytes)!=b.hash)goto fail;
            pos+=b.bytes;stored+=span;
        }
        nm_why="arena size";
        if(pos!=h.arena_bytes)goto fail;
        nm_why="record bodies";
        for(unsigned i=0;i<h.n_records;i++){
            const native_asset_desc *d=&NM.rec[i].image;
            if(d->offset>h.arena_bytes||d->bytes>h.arena_bytes-d->offset)goto fail;
        }
    }
    NM_FREE(state);
    nm_info.state=1;nm_info.source=source;nm_info.error="";nm_info.sharp=h.flags&1;nm_info.file_bytes=(uint32_t)n;
    nm_info.arena_bytes=h.arena_bytes;nm_info.records=h.n_records;nm_info.bodies=h.n_bodies;nm_info.load_ms=hal_millis()-t0;
    return 1;
fail:
    NM_FREE(state);NM_FREE(store);memset(&NM,0,sizeof NM);
    return 0;
}
static void nm_fail(const char *source) {
    snprintf(nm_err,sizeof nm_err,"%s",nm_why?nm_why:"unknown");
    nm_info.state=-1;nm_info.source=source;nm_info.error=nm_err;
    printf("NOSASSETS ERROR %s: %s; dynamic artwork fallback\n",source,nm_err);
}
static void nm_report(uint32_t ms) {
    nm_info.load_ms=ms;
    printf("NOSASSETS loaded=%u ms=%u src=%s sharp=%d records=%u bodies=%u arena=%u hashes=verified\n",(unsigned)nm_info.file_bytes,
           (unsigned)ms,nm_info.source,nm_info.sharp,(unsigned)nm_info.records,(unsigned)nm_info.bodies,(unsigned)nm_info.arena_bytes);
}

#ifdef NOS_MENU_PACK_EMBEDDED
static int nm_load(void) {
    uint32_t t0=hal_millis();
    if(nm_publish(_binary_native_menu_sharp_pack_start,(size_t)(_binary_native_menu_sharp_pack_end-_binary_native_menu_sharp_pack_start),"embedded")){nm_report(hal_millis()-t0);return 1;}
    nm_fail("embedded");return 0;
}
#elif defined(ESP_PLATFORM)
static uint32_t nm_deadline;
static void *nm_orphan; /* buffer of a job the worker still owned after the drain; freed when the job is taken */
/* UI-thread read through the asset worker; bounded by nm_deadline plus a short cancel drain. */
static void *nm_fetch(const char *path,unsigned bytes) {
    void *buf=NM_ALLOC(bytes);
    asset_request rq;asset_result rs;int done=0,accepted;
    if(!buf){nm_why="PSRAM allocation failed";return NULL;}
    memset(&rq,0,sizeof rq);
    snprintf(rq.path,sizeof rq.path,"%s",path);
    rq.bytes=rq.page_bytes=bytes;rq.page_count=1;rq.pages[0]=buf;rq.tag=0x4d4e;
    accepted=hal_asset_submit(&rq);
    if(accepted<=0){NM_FREE(buf);nm_why=accepted<0?"asset worker unavailable":"asset worker busy";return NULL;}
    while(!(done=hal_asset_take(&rq,&rs))){
        if((int32_t)(hal_millis()-nm_deadline)>=0)break;
        hal_asset_poll();vTaskDelay(1);
    }
    if(!done){
        /* The worker checks the cancel flag between slices and may be stalled on the card or the SD lock:
         * give it a bounded drain, and if it still owns the buffer, leak it rather than free memory it may write. */
        uint32_t end=hal_millis()+NM_DRAIN_MS;
        hal_asset_cancel();
        while(!(done=hal_asset_take(&rq,&rs)) && (int32_t)(hal_millis()-end)<0){hal_asset_poll();vTaskDelay(1);}
        if(done)NM_FREE(buf);else nm_orphan=buf;
        nm_why="SD read timed out";return NULL;
    }
    if(rs.code!=ASSET_OK){NM_FREE(buf);nm_why=rs.code==ASSET_FAILED?"pack file missing or unreadable":"SD read cancelled";return NULL;}
    return buf;
}
static int nm_load(void) {
    char path[ASSET_PATH];hal_storage st;uint32_t t0=hal_millis();
    nm_hdr *h;uint8_t *file;
    nm_why=NULL;
    hal_storage_get(&st);
    if(!st.mounted){nm_why="no SD card";nm_fail("sd");return 0;}
    nm_deadline=t0+NM_WAIT_MS;
    snprintf(path,sizeof path,"%s/menu/native_menu_sharp.pack",hal_sd_root());
    h=nm_fetch(path,sizeof *h);
    if(!h){nm_fail("sd");return 0;}
    unsigned size=h->file_bytes;
    int ok=!memcmp(h->magic,"NMAP",4)&&size>sizeof *h&&size<=NM_FILE_MAX;
    NM_FREE(h);
    if(!ok){nm_why="bad pack header";nm_fail("sd");return 0;}
    file=nm_fetch(path,size);
    if(!file){nm_fail("sd");return 0;}
    ok=nm_publish(file,size,"sd");
    NM_FREE(file);
    if(!ok){nm_fail("sd");return 0;}
    nm_report(hal_millis()-t0);
    return 1;
}
#else
static uint8_t *nm_read(const char *path,size_t *n) {
    FILE *f=fopen(path,"rb");uint8_t *b;long size;
    if(!f)return NULL;
    if(fseek(f,0,SEEK_END)||(size=ftell(f))<=0||size>(long)NM_FILE_MAX||fseek(f,0,SEEK_SET)){fclose(f);return NULL;}
    b=malloc((size_t)size);
    if(b&&fread(b,1,(size_t)size,f)!=(size_t)size){free(b);b=NULL;}
    fclose(f);*n=(size_t)size;return b;
}
/* Host: the full pack first (tracked in assets/sd/menu, found from the working
 * directory or this source's location, then the simulated SD), then the sharp pack. */
static int nm_load(void) {
    static const char *const names[2]={"native_menu_full.pack","native_menu_sharp.pack"};
    char dirs[3][400],path[512],here[300];const char *env=getenv("NOS_MENU_PACK");
    snprintf(here,sizeof here,"%s",__FILE__);
    char *cut=here+strlen(here);while(cut>here&&cut[-1]!='/'&&cut[-1]!='\\')cut--;
    *cut=0;
    snprintf(dirs[0],sizeof dirs[0],"assets/sd/menu");
    snprintf(dirs[1],sizeof dirs[1],"%s../assets/sd/menu",here);
    snprintf(dirs[2],sizeof dirs[2],"%s/menu",hal_sd_root());
    nm_why="pack file not found";
    for(int c=env&&*env?-1:0;c<6;c++){
        if(c<0)snprintf(path,sizeof path,"%s",env);
        else snprintf(path,sizeof path,"%s/%s",dirs[c%3],names[c/3]);
        size_t n;uint8_t *b=nm_read(path,&n);
        if(!b)continue;
        int ok=nm_publish(b,n,"file");
        free(b);
        if(ok){nm_report(nm_info.load_ms);return 1;}
    }
    nm_fail("file");return 0;
}
#endif

void native_menu_assets_init(void) {
    if(nm_info.state)return;
    nm_load();
}
int native_menu_assets_reload(void) {
    if(nm_info.state>0)return 1;
    return nm_load();
}
#if defined(ESP_PLATFORM) && !defined(NOS_MENU_PACK_EMBEDDED)
/* B watch: a failed boot load (card absent or mounted late, transient read error) is retried a few times after
 * the card (re)mounts, 1 s then growing; a persistent failure (stale or missing pack) stops after 4 tries. */
int native_menu_assets_retry_due(void) {
    static const uint16_t gap[4]={1000,5000,15000,45000};
    static uint32_t next;static uint8_t tries,known,was;
    hal_storage st;uint32_t now=hal_millis();
    if(nm_orphan){asset_request rq;asset_result rs;if(!hal_asset_take(&rq,&rs))return 0;NM_FREE(nm_orphan);nm_orphan=NULL;}
    if(nm_info.state>=0)return 0;
    hal_storage_get(&st);
    if(!known){known=1;was=st.mounted;next=now+gap[0];}
    else if(st.mounted&&!was){tries=0;next=now+gap[0];}
    was=st.mounted;
    if(!st.mounted||tries>=4||(int32_t)(now-next)<0)return 0;
    next=now+gap[++tries<4?tries:3];
    return 1;
}
#else
int native_menu_assets_retry_due(void) {return 0;}
#endif
void native_menu_pack_status(native_menu_pack_info *out) {*out=nm_info;}
/* Host tools and tests never run nos_init: bind lazily the first time ROM art is wanted. */
static int nm_ready(void) {
#if !defined(ESP_PLATFORM)
    if(!nm_info.state)nm_load();
#endif
    return nm_info.state>0;
}

int nos_native_menu_rom_enabled=1;
void nos_native_menu_rom_set(int enabled)
{
    enabled=!!enabled;
    if(enabled==nos_native_menu_rom_enabled)return;
    nos_native_menu_rom_enabled=enabled;
    nos_layers_invalidate();nos_history_reset();nos_invalidate();
}
int native_menu_assets_compatible(void)
{
    return nos_native_menu_rom_enabled && UI_F_BODY==&nf_body &&
        UI_F_LABEL==&nf_label && UI_F_TITLE==&nf_title && nm_ready();
}
int native_menu_launcher_matches(unsigned entry,const char *name,int icon)
{
    if(!nm_ready()||entry>=NM.n_apps||!name)return 0;
    return icon==NM.reg_icon[entry] && strcmp(name,NM.str+NM.reg_name[entry])==0;
}
const native_asset_desc *native_menu_asset(unsigned family,unsigned entry,unsigned layout,unsigned level)
{
    if(!native_menu_assets_compatible())return NULL;
    unsigned index=NM_NONE;
    if(family==0 && entry<36 && layout<2 && level<9)index=NM.idx_s[(entry*2+layout)*9+level];
    if(family==1 && entry<NM.n_apps && layout<3 && level<9)index=NM.idx_i[(entry*3+layout)*9+level];
    if(family==2 && entry<NM.n_apps && !layout && level<9)index=NM.idx_n[entry*9+level];
    return index<NM.n_rec?&NM.rec[index].image:NULL;
}
sprite8 native_menu_sprite(const native_asset_desc *asset)
{
    if(!asset || !nm_ready() || asset->w<=0 || asset->h<=0 || asset->w>GFX_W || asset->h>GFX_H ||
       (uint64_t)(unsigned)asset->w*(unsigned)asset->h!=asset->bytes ||
       asset->offset>nm_info.arena_bytes || asset->bytes>nm_info.arena_bytes-asset->offset)
        return (sprite8){0};
    return (sprite8){NM.arena+asset->offset,asset->w,asset->h};
}
const uint16_t *native_menu_settings_palette(int red,int level)
{
    if(!native_menu_assets_compatible() || level<0 || level>64)return NULL;
    if(!red)return NM.pal+level*256;
    if(C_RED==0xD0C4)return NM.pal+NM_PL+level*256;
    if(C_RED==0x2F3B)return NM.pal+2*NM_PL+level*256;
    return NULL;
}
const uint16_t *native_menu_launcher_palette(int level,int pressed)
{
    if(!native_menu_assets_compatible() || level<0 || level>64)return NULL;
    return pressed?NM.pal+4*NM_PL:NM.pal+3*NM_PL+level*256;
}
const int16_t *native_menu_launcher_curve(void)
{
    return native_menu_assets_compatible()?NM.curve:NULL;
}
uint32_t native_menu_asset_bytes(void){return nm_info.state>0?nm_info.arena_bytes:0;}
const char *native_menu_source_sha256(void){return NATIVE_MENU_BUILD_SHA256;}
