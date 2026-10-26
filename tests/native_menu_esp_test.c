/* The ESP/SD branch of nos/native_menu_assets.c on the host: the real loader and the real
 * core/asset_job.c worker protocol, with a fake clock, fake SD mount and zlib behind tinfl.
 * argv: [1] directory acting as the SD root (contains menu/native_menu_sharp.pack), [2] zlib1.dll. */
#include "../nos/native_menu_assets.c"
#include "asset_job.h"
#include <assert.h>
#include <windows.h>
const font_t *UI_F_BODY=&nf_body,*UI_F_LABEL=&nf_label,*UI_F_TITLE=&nf_title;
uint16_t nos_c_red=0xD0C4;
void nos_layers_invalidate(void){}
void nos_history_reset(void){}
void nos_invalidate(void){}
static uint32_t now;
static int mounted=1,unavailable,step_ms,stalled,wedged,steps,allocs;
static char root[400];
uint32_t hal_millis(void){return now;}
const char *hal_sd_root(void){return "/sdcard";}
void vTaskDelay(TickType_t t){now+=t;}
void hal_storage_get(hal_storage *o){memset(o,0,sizeof *o);o->mounted=mounted;}
void *hal_big_alloc(uint32_t n){allocs++;return malloc(n);}
static void *io_open(const char *p){char f[700];snprintf(f,sizeof f,"%s%s",root,p+7);return fopen(f,"rb");}
static size_t io_read(void *f,void *b,size_t n){return fread(b,1,n,f);}
static void io_close(void *f){fclose(f);}
static uint32_t io_us(void){return now*1000u;}
static asset_job job;static const asset_io io={io_open,io_read,io_close,io_us};static int init;
int hal_asset_submit(const asset_request *r){if(unavailable)return -1;if(!init){asset_init(&job);init=1;}return asset_submit(&job,r);}
void hal_asset_cancel(void){asset_cancel(&job);}
int hal_asset_take(asset_request *r,asset_result *o){return init?asset_take(&job,r,o):0;}
void hal_asset_poll(void){if(wedged)return;if(stalled&&!atomic_load(&job.cancel))return;steps++;if(!atomic_load(&job.cancel))now+=step_ms; /* a read in flight finishes; a cancelled one is instant */
    asset_worker_step(&job,&io);}
void hal_asset_wait(void){int s;while((s=atomic_load(&job.state))!=ASSET_IDLE&&s!=ASSET_DONE)hal_asset_poll();}
/* tinfl over host zlib */
typedef int (__cdecl *uncompress_t)(unsigned char *,unsigned long *,const unsigned char *,unsigned long);
static uncompress_t uncompress_fn;
tinfl_status tinfl_decompress(tinfl_decompressor *d,const uint8_t *in,size_t *in_n,uint8_t *o0,uint8_t *o,size_t *out_n,uint32_t fl){
    (void)d;(void)o0;(void)fl;unsigned long dl=(unsigned long)*out_n;
    if(uncompress_fn(o,&dl,in,(unsigned long)*in_n)!=0)return -1;
    *out_n=dl;return TINFL_STATUS_DONE;
}
static void put(const char *name,const uint8_t *b,size_t n){
    char f[700];snprintf(f,sizeof f,"%s/menu/%s",root,name);FILE *o=fopen(f,"wb");assert(o&&fwrite(b,1,n,o)==n);fclose(o);
}
static void expect_fail(const char *what,const char *why,int max_ms){
    uint32_t t0=now;native_menu_pack_info pi;
    assert(!native_menu_assets_reload());
    native_menu_pack_status(&pi);
    printf("CASE %s: state=%d err=\"%s\" after %u fake ms\n",what,pi.state,pi.error,(unsigned)(now-t0));
    assert(pi.state==-1 && strstr(pi.error,why));
    assert(!native_menu_assets_compatible() && !native_menu_asset(0,0,0,0) && !native_menu_launcher_curve());
    assert(!NM.store && !NM.arena && !NM.rec);
    assert((int)(now-t0)<=max_ms);
}
int main(int argc,char **argv){
    setvbuf(stdout,NULL,_IONBF,0);assert(argc==3);snprintf(root,sizeof root,"%s",argv[1]);
    HMODULE z=LoadLibraryA(argv[2]);assert(z);uncompress_fn=(uncompress_t)(void *)GetProcAddress(z,"uncompress");assert(uncompress_fn);
    char f[700];snprintf(f,sizeof f,"%s/menu/native_menu_sharp.pack",root);
    FILE *in=fopen(f,"rb");assert(in);fseek(in,0,SEEK_END);size_t n=(size_t)ftell(in);fseek(in,0,SEEK_SET);
    uint8_t *good=malloc(n),*bad=malloc(n);assert(fread(good,1,n,in)==n);fclose(in);
    nm_hdr h;memcpy(&h,good,sizeof h);
    /* 1. No SD card: nothing is attempted. */
    mounted=0;expect_fail("no SD","no SD card",0);assert(steps==0 && allocs==0);
    mounted=1;
    /* 2. Worker unavailable and pack missing. */
    unavailable=1;expect_fail("worker unavailable","unavailable",5);unavailable=0;
    remove(f);expect_fail("missing file","missing",100);
    /* 3. Damaged files reach the validator, which refuses each. */
    memcpy(bad,good,n);bad[0]='X';put("native_menu_sharp.pack",bad,n);expect_fail("bad magic","bad pack header",200);
    memcpy(bad,good,n);{nm_hdr t=h;t.file_bytes=5000000;memcpy(bad,&t,sizeof t);}put("native_menu_sharp.pack",bad,n);expect_fail("oversize header","bad pack header",200);
    put("native_menu_sharp.pack",good,n-100);expect_fail("truncated file","missing or unreadable",200);
    memcpy(bad,good,n);bad[n-7]^=1;put("native_menu_sharp.pack",bad,n);expect_fail("payload flip","payload crc",400);
    memcpy(bad,good,n);bad[offsetof(nm_hdr,build)]^=1;{nm_hdr t;memcpy(&t,bad,sizeof t);t.hdr_crc=nm_crc(bad,sizeof t-4);memcpy(bad,&t,sizeof t);}
    put("native_menu_sharp.pack",bad,n);expect_fail("stale build","stale",400);
    /* 4. A slow card hits the 1.5 s bound and is cancelled cleanly (no buffer or job left behind). */
    put("native_menu_sharp.pack",good,n);
    step_ms=900;expect_fail("slow SD","timed out",1500+900);step_ms=0;
    stalled=1;expect_fail("stalled SD","timed out",1500+10);stalled=0;
    assert(atomic_load(&job.state)==ASSET_IDLE);
    /* A worker wedged in fread or on the SD lock never honours the cancel: the drain is bounded and the buffer is
     * parked (not freed under the worker); retry_due releases it once the job is finally taken. */
    wedged=1;expect_fail("wedged worker","timed out",1500+1000+10);
    assert(nm_orphan && atomic_load(&job.state)!=ASSET_IDLE);
    wedged=0;hal_asset_poll();
    assert(!native_menu_assets_retry_due() && !nm_orphan && atomic_load(&job.state)==ASSET_IDLE); /* first call also arms the schedule */
    /* Retry schedule: 1 s, then 5, 15 and 45 s; four tries per mount; a remount re-arms it. */
    now+=999;assert(!native_menu_assets_retry_due());
    now+=1;assert(native_menu_assets_retry_due() && !native_menu_assets_retry_due());
    now+=5000;assert(native_menu_assets_retry_due());
    now+=15000;assert(native_menu_assets_retry_due());
    now+=45000;assert(native_menu_assets_retry_due());
    now+=1000000;assert(!native_menu_assets_retry_due());
    mounted=0;now+=10;assert(!native_menu_assets_retry_due());
    mounted=1;now+=10;assert(!native_menu_assets_retry_due());
    now+=1000;assert(native_menu_assets_retry_due());
    /* 5. Recovery on the same boot: the retry loads, validates and publishes. */
    uint32_t t0=now;assert(native_menu_assets_reload());
    native_menu_pack_info pi;native_menu_pack_status(&pi);
    printf("CASE good: state=%d file=%u arena=%u records=%u load %u fake ms\n",pi.state,(unsigned)pi.file_bytes,(unsigned)pi.arena_bytes,pi.records,(unsigned)(now-t0));
    assert(pi.state==1 && pi.sharp==1 && pi.records==169 && pi.file_bytes==n && !strcmp(pi.source,"sd"));
    assert(native_menu_assets_compatible() && native_menu_asset(1,0,2,0) && !native_menu_asset(1,0,1,1));
    assert(native_menu_asset_bytes()==pi.arena_bytes);
    sprite8 s=native_menu_sprite(native_menu_asset(2,0,0,0));assert(s.px);
    /* Success is sticky: another reload is a no-op. */
    assert(native_menu_assets_reload());
    native_menu_pack_status(&pi);assert(pi.state==1);
    now+=1000000;assert(!native_menu_assets_retry_due());
    puts("PASS native menu pack ESP loader (mocked worker)");
    return 0;
}
