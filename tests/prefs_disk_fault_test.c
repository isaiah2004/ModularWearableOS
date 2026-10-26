/* Fault injection uses the actual alternating checkpoint implementation. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <direct.h>
static const char *read_path;
static int read_failure,write_failure,write_calls;
static FILE *bad_stream;
static FILE *fault_open(const char *path,const char *mode)
{
    int selected=read_path && !strcmp(path,read_path) && !strcmp(mode,"rb");
    if(selected && read_failure==1) {errno=EIO;return NULL;}
    FILE *f=fopen(path,mode);
    if(selected && read_failure>1)bad_stream=f;
    return f;
}
static size_t fault_read(void *p,size_t size,size_t n,FILE *f)
{if(f==bad_stream && read_failure==2) {errno=ENOMEM;return 0;}return fread(p,size,n,f);}
static int fault_error(FILE *f)
{return f==bad_stream && read_failure==2?1:ferror(f);}
static int fault_close(FILE *f)
{
    int fail=f==bad_stream && read_failure==3;
    if(f==bad_stream)bad_stream=NULL;
    int result=fclose(f);if(fail) {errno=EIO;return EOF;}return result;
}
static size_t fault_write(const void *p,size_t size,size_t n,FILE *f)
{write_calls++;if(write_failure) {write_failure=0;errno=ENOSPC;return 0;}return fwrite(p,size,n,f);}
#define fopen fault_open
#define fread fault_read
#define ferror fault_error
#define fclose fault_close
#define fwrite fault_write
#include "../nos/prefs_disk.c"
#undef fopen
#undef fread
#undef ferror
#undef fclose
#undef fwrite
static size_t snapshot(const char *path,unsigned char *bytes)
{FILE *f=fopen(path,"rb");assert(f);size_t n=fread(bytes,1,512,f);assert(!fclose(f));return n;}
int main(void)
{
    _mkdir("card");
    char old[44]="legacy old",newer[52]="migrated newest",next[52]="next checkpoint",out[52];
    memset(out,0x5a,sizeof out);
    assert(prefs_disk_load_status("card",out,sizeof out)==PREFS_DISK_MISSING);
    assert(out[0]==0x5a);
    assert(prefs_disk_save("card",old,sizeof old));
    assert(prefs_disk_save("card",old,sizeof old));
    assert(prefs_disk_save("card",newer,sizeof newer)); /* newest: slot 0, sequence 3 */
    assert(prefs_disk_load_status("card",out,sizeof old)==PREFS_DISK_CORRUPT);
    assert(prefs_disk_load_status("card",out,sizeof out)==PREFS_DISK_OK);
    assert(!memcmp(out,newer,sizeof out));
    unsigned char before[512],after[512];size_t saved=snapshot("card/settings.0.bin",before);
    read_path="card/settings.0.bin";
    for(read_failure=1;read_failure<=3;read_failure++) {
        memset(out,0x5a,sizeof out);write_calls=0;write_failure=1;
        assert(prefs_disk_load_status("card",out,sizeof out)==PREFS_DISK_IO && out[0]==0x5a);
        assert(!prefs_disk_save("card",next,sizeof next));
        assert(!write_calls && write_failure);
        assert(snapshot(read_path,after)==saved && !memcmp(before,after,saved));
    }
    read_failure=0;read_path=NULL;write_failure=1;
    assert(!prefs_disk_save("card",next,sizeof next)); /* failed older-bank write */
    assert(prefs_disk_load("card",out,sizeof out) && !memcmp(out,newer,sizeof out));
    assert(snapshot("card/settings.0.bin",after)==saved && !memcmp(before,after,saved));
    assert(prefs_disk_save("card",next,sizeof next));
    assert(prefs_disk_load("card",out,sizeof out) && !memcmp(out,next,sizeof out));
    assert(prefs_disk_validate_file("card/settings.1.bin")==PREFS_DISK_OK);
    remove("card/settings.0.bin");remove("card/settings.1.bin");
    FILE *f=fopen("card/settings.0.bin","wb");assert(f);assert(fwrite("torn",1,4,f)==4);assert(!fclose(f));
    assert(prefs_disk_load_status("card",out,sizeof out)==PREFS_DISK_CORRUPT);
    puts("PASS preferences: missing/corrupt/I/O, ambiguous read refusal, failed write preserves newest, schema migration");
    return 0;
}
