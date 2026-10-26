/* sd_xfer.c — files to and from the SD card, over the USB serial port or Bluetooth
 * (tools/sdsync.py; Bluetooth transport in ble_xfer.c).
 *
 * One command set, two transports. Each transport gives a raw-byte reader (the bytes that follow
 * an sdput line) and a line writer (replies). Commands run in the serial command task, before a
 * line is queued for the main loop, so a transfer never waits on frames. Paths are relative to the
 * card root:
 *   sdls <dir>             SDLS <size> <name> per entry ('/' after a directory), then SDLS END
 *   sdcrc <path>           SDCRC <size> <crc32 hex>, or SDCRC MISSING
 *   sdput <path> <size>    SDPUT READY; then <size> raw bytes in blocks (USB 2 KB, BLE 4 KB), each
 *                          answered SDK <bytes so far> once written; SDPUT OK <size> <crc32>
 *                          (synced/verified .part, recoverable backup publication)
 *   sdput! <path> <size>   explicit raw checkpoint restore; checked structure, no live-file override
 *   sdget <path>           SDGET <size> <crc32>, then SDG <base64> lines (1536 bytes each), then
 *                          SDGET END (or SDGET MISSING / SDGET ERR ...)
 *   sdget2 <path>          SDGET2 <size> <crc32>, SDG2 <offset> <chunk-crc32> <base64>;
 *                          host SGA <next-offset> acknowledges, SGR <offset> retries.
 *                          One frame in flight; SDGET2 END / SDGET2 ERR ...
 *   sdrm <path>            SDRM OK / SDRM ERR
 * Uploads: the USB-Serial-JTAG driver DROPS bytes when its receive ring is full (it never holds
 * the host off; measured: a 1 MB send lost data after ~300 KB while the card was busy), and BLE
 * writes-without-response land in a fixed stream buffer, so the host waits for each block's SDK:
 * one block in flight at most. A stall of 3 s aborts it and retains the partial file for inspection.
 * Downloads are text lines because other tasks print log lines on the same USB port at any time:
 * one printf per line is atomic, so a log line can fall between SDG lines but never inside one;
 * the host keeps only its prefixes and checks the CRC. CRC32 is zlib's. */
#include "psram.h"
#include "sd_xfer.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include "freertos/FreeRTOS.h"
#include "driver/usb_serial_jtag.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "hal.h"
#include "board.h"
#include "prefs_disk.h"
#include "prefs.h"
#include "imu_backend.h"
#ifdef ESP_PLATFORM
#include "ota.h"
#elif defined(SD_XFER_OTA_TEST)
int ota_external_activity_begin(void);
void ota_external_activity_end(void);
#endif
/* Disk-only host fixtures have no RAM owner. Device links the real owners. */
extern int prefs_restore_begin(void) __attribute__((weak));
extern void prefs_restore_end(int changed) __attribute__((weak));
extern int prefs_pending(void) __attribute__((weak));
extern int prefs_loading(void) __attribute__((weak));
extern int imu_checkpoint_restore_begin(void) __attribute__((weak));
extern void imu_checkpoint_restore_end(int changed) __attribute__((weak));

#define XFER_BUF 8192            /* [0, 2048): file data / put block; [2048, 8192): a base64 line */
#define GET_CHUNK 1536           /* bytes per SDG line: 2048 base64 chars */
#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifdef _WIN32
#include <io.h>
#define XFER_SYNC(fd) _commit(fd)
#else
#define XFER_SYNC(fd) fsync(fd)
#endif

static void reply(const xfer_io *io, const char *fmt, ...)
{
    char s[192];
    va_list ap; va_start(ap, fmt); vsnprintf(s, sizeof s, fmt, ap); va_end(ap);
    io->line(s);
}
static int full_path(char *out, size_t n, const char *rel)
{
    while (*rel == '/') rel++;
    if (strstr(rel, "..") || strchr(rel,'\\') || strchr(rel,':')) return 0;
    /* A spelling such as watch/./settings.0.bin must not bypass ownership
     * checks while resolving to the protected FAT file. */
    for(const char *p=rel;*p;) {
        const char *end=strchr(p,'/');size_t part=end?(size_t)(end-p):strlen(p);
        if(!part || (part==1 && *p=='.'))return 0;
        if(!end)break;
        p=end+1;
    }
    int length=snprintf(out,n,"%s/%s",hal_sd_root(),rel);
    return length>=0 && (size_t)length<n;
}
static void make_parents(char *path)
{
    for (char *p = path + strlen(hal_sd_root()) + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(path, 0775); *p = '/'; }
}
static int file_crc(FILE *f, uint8_t *buf, long *size, uint32_t *result)
{
    uint32_t crc = 0; long n = 0;
    for(;;) {
        board_sd_asset_lock();
        size_t r=fread(buf,1,XFER_BUF,f);int error=ferror(f)?(errno?errno:EIO):0;
        board_sd_asset_unlock();
        if(error) {errno=error;return 0;}
        if(!r)break;
        if(r>(size_t)(LONG_MAX-n)) {errno=EOVERFLOW;return 0;}
        crc=esp_rom_crc32_le(crc,buf,(uint32_t)r);n+=(long)r;
    }
    *size=n;*result=crc;return 1;
}

/* One publication transaction at a time across USB/BLE and mount recovery.
 * This ownership flag spans transport waits; the shared SD mutex never does. */
static int transfer_active,transfer_mutation_owned;
static int mutation_begin(void)
{
#if defined(ESP_PLATFORM) || defined(SD_XFER_OTA_TEST)
    return ota_external_activity_begin();
#else
    return 1;
#endif
}
static void mutation_end(void)
{
#if defined(ESP_PLATFORM) || defined(SD_XFER_OTA_TEST)
    ota_external_activity_end();
#endif
}
static int transfer_claim(void)
{
    board_sd_asset_lock();int ok=!transfer_active;if(ok)transfer_active=1;board_sd_asset_unlock();return ok;
}
static void transfer_release(void)
{ board_sd_asset_lock();transfer_active=transfer_mutation_owned=0;board_sd_asset_unlock(); }
static int path_state(const char *path)
{
    struct stat st;board_sd_asset_lock();int r=stat(path,&st),error=errno;board_sd_asset_unlock();
    if(!r)return 1;
    errno=error;return error==ENOENT?0:-1;
}
static FILE *read_open(const char *path,int *error)
{
    board_sd_asset_lock();FILE *f=fopen(path,"rb");*error=f?0:errno;board_sd_asset_unlock();return f;
}
static int close_file(FILE *f)
{
    board_sd_asset_lock();int r=fclose(f),error=errno;board_sd_asset_unlock();if(r)errno=error?error:EIO;return r;
}
static int rename_file(const char *from,const char *to)
{
    board_sd_asset_lock();int r=rename(from,to),error=errno;board_sd_asset_unlock();if(r)errno=error?error:EIO;return r;
}
static int remove_file(const char *path)
{
    board_sd_asset_lock();int r=remove(path),error=errno;board_sd_asset_unlock();if(r)errno=error?error:EIO;return r;
}
static int durable_close(FILE *f)
{
    int error=0;
    board_sd_asset_lock();
    if(fflush(f))error=errno?errno:EIO;
    if(!error && XFER_SYNC(fileno(f)))error=errno?errno:EIO;
    if(fclose(f) && !error)error=errno?errno:EIO;
    board_sd_asset_unlock();errno=error;return !error;
}
static FILE *create_file(const char *path)
{
    board_sd_asset_lock();
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_BINARY,0664);
    FILE *f=fd<0?NULL:fdopen(fd,"wb");int error=errno;
    if(!f && fd>=0)close(fd);
    board_sd_asset_unlock();if(!f)errno=error?error:EIO;return f;
}
typedef struct {uint32_t magic,size,crc;char rel[128];uint32_t check;} upload_transaction;
static int live_path(const char *rel);
static int reserved_path(const char *rel);
static int restore_claim(const char *rel);
static void restore_release(int owner,int changed);
static uint32_t transaction_sum(const upload_transaction *t)
{
    const unsigned char *b=(const unsigned char *)t;uint32_t h=2166136261u;
    for(size_t i=0;i<offsetof(upload_transaction,check);i++)h=(h^b[i])*16777619u;
    return h;
}
static int transaction_path(char path[160])
{ return full_path(path,160,".sdxfer.txn"); }
static int matches_file(const char *path,const upload_transaction *t,uint8_t *buf)
{
    int error;FILE *f=read_open(path,&error);if(!f) {errno=error;return 0;}
    long size;uint32_t crc;int ok=file_crc(f,buf,&size,&crc),saved=errno;
    if(close_file(f)) {ok=0;saved=errno;}
    if(!ok) {errno=saved?saved:EIO;return 0;}
    if(size!=(long)t->size || crc!=t->crc) {errno=EINVAL;return 0;}
    return 1;
}
/* A ready marker is synced only after the staged bytes have been synced and
 * verified. Any publication failure retains staged bytes and either the original
 * path or its backup. A reboot can complete exactly this checked transaction. */
static int recover_transaction(uint8_t *buf)
{
    char journal[160],path[160],part[176],backup[176];upload_transaction t;
    if(!transaction_path(journal)) {errno=ENAMETOOLONG;return 0;}
    int error;FILE *f=read_open(journal,&error);
    if(!f) {errno=error;return error==ENOENT;}
    board_sd_asset_lock();
    size_t got=fread(&t,1,sizeof t,f);int extra=fgetc(f),read_error=ferror(f)?(errno?errno:EIO):0;
    board_sd_asset_unlock();
    if(close_file(f) && !read_error)read_error=errno;
    if(read_error) {errno=read_error;return 0;}
    if(got!=sizeof t || extra!=EOF || t.magic!=0x53445831u ||
       !memchr(t.rel,0,sizeof t.rel) || t.check!=transaction_sum(&t) ||
       !full_path(path,sizeof path,t.rel) || reserved_path(t.rel) || t.size>LONG_MAX) {errno=EINVAL;return 0;}
    snprintf(part,sizeof part,"%s.part",path);snprintf(backup,sizeof backup,"%s.sdxfer-old",path);
    int staged=path_state(part);if(staged<0)return 0;
    int old=path_state(backup);if(old<0)return 0;
    int published=path_state(path);if(published<0)return 0;
    if(staged && !matches_file(part,&t,buf))return 0;
    if(staged && published && old) {errno=EEXIST;return 0;}
    /* Read-only commands may discover a ready transaction. Its publication is
     * still a mutation, and must not run while OTA owns flash. A PUT/RM already
     * owns this scope for its whole transport lifetime; never claim it twice. */
    int external=!transfer_mutation_owned;
    if(external && !mutation_begin()){errno=EBUSY;return 0;}
    board_sd_asset_lock();
    if(live_path(t.rel)) {board_sd_asset_unlock();if(external)mutation_end();errno=EBUSY;return 0;}
    int owner=restore_claim(t.rel),changed=!staged,ok=0;
    if(owner<0) {board_sd_asset_unlock();if(external)mutation_end();errno=EBUSY;return 0;}
    if(staged) {
        if(published) {
            if(rename(path,backup)) {error=errno;board_sd_asset_unlock();goto finished;}
            old=1;changed=1;
        }
        if(rename(part,path)) {
            error=errno;
            if(old)(void)rename(backup,path);
            board_sd_asset_unlock();goto finished;
        }
        changed=1;
    }
    board_sd_asset_unlock();
    if(!matches_file(path,&t,buf)) {
        error=errno;
        if(old && !path_state(path)) (void)rename_file(backup,path);
        goto finished;
    }
    if(old && remove_file(backup)) {error=errno;goto finished;}
    if(remove_file(journal)) {error=errno;goto finished;}
    ok=1;error=0;
finished:
    board_sd_asset_lock();restore_release(owner,changed);board_sd_asset_unlock();
    if(external)mutation_end();
    errno=error;return ok;
}

int sd_xfer_recover(void)
{
    if(!board_sd_asset_acquire())return 0;
    int ok=0;
    if(transfer_claim()) {
        uint8_t *buf=nos_mem_alloc(XFER_BUF,"sd_xfer.recover");
        if(buf) {ok=recover_transaction(buf);nos_mem_free(buf);}
        transfer_release();
    }
    board_sd_asset_release();return ok;
}

static int path_prefix(const char *a,const char *b)
{
    while(*b) {
        unsigned char x=(unsigned char)*a++,y=(unsigned char)*b++;
        if(x>='A' && x<='Z')x=(unsigned char)(x+'a'-'A');
        if(y>='A' && y<='Z')y=(unsigned char)(y+'a'-'A');
        if(x!=y)return 0;
    }
    return 1;
}
static int path_equal(const char *a,const char *b)
{ return strlen(a)==strlen(b) && path_prefix(a,b); }
static int reserved_path(const char *rel)
{
    while(*rel=='/')rel++;
    size_t n=strlen(rel);
    return path_equal(rel,".sdxfer.txn") ||
        (n>=11 && path_equal(rel+n-11,".sdxfer-old")) ||
        (n>=5 && path_equal(rel+n-5,".part"));
}
static int protected_path(const char *rel)
{
    while(*rel=='/')rel++;
    return path_equal(rel,"watch/settings.0.bin") || path_equal(rel,"watch/settings.1.bin") ||
           path_equal(rel,"watch/wifi/settings.0.bin") || path_equal(rel,"watch/wifi/settings.1.bin") ||
           path_equal(rel,"imu/intent.0.bin") || path_equal(rel,"imu/intent.1.bin");
}
static int live_path(const char *rel)
{
    while(*rel=='/')rel++;
    /* An error/unknown intent can still own its previous CSV and linkage, even
     * while the visible recorder is between retry segments. */
    if(path_prefix(rel,"imu/") && imu_worker_pending())return 1;
    hal_imu_record record;hal_imu_record_get(&record);
    if(record.state==HAL_IMU_RECORDING || record.state==HAL_IMU_STOPPING || record.state==HAL_IMU_STARTING) {
        char active[64];snprintf(active,sizeof active,"imu/%s",record.name);
        if(path_equal(rel,active))return 1;
    }
    return 0;
}
static int restore_claim(const char *rel)
{
    while(*rel=='/')rel++;
    if(!protected_path(rel))return 0;
    /* The Wi-Fi worker owns a private durable credential cache. It needs its
     * own reload handshake before a raw publication can safely be accepted. */
    if(path_prefix(rel,"watch/wifi/"))return -1;
    if(path_prefix(rel,"watch/"))return !prefs_restore_begin || prefs_restore_begin()?1:-1;
    return !imu_checkpoint_restore_begin || imu_checkpoint_restore_begin()?2:-1;
}
static void restore_release(int owner,int changed)
{
    if(owner==1 && prefs_restore_end)prefs_restore_end(changed);
    if(owner==2 && imu_checkpoint_restore_end)imu_checkpoint_restore_end(changed);
}
static int validate_restore(const char *rel,const char *part,uint8_t *buf)
{
    if(path_prefix(rel,"watch/")) {
        board_sd_asset_lock();prefs_disk_read_status result=prefs_disk_validate_file(part);board_sd_asset_unlock();
        if(result!=PREFS_DISK_OK) {errno=result==PREFS_DISK_IO?EIO:EINVAL;return 0;}
    } else {
        int error;FILE *f=read_open(part,&error);if(!f) {errno=error;return 0;}
        uint32_t words[6]={0};board_sd_asset_lock();size_t n=fread(words,1,sizeof words,f);int extra=fgetc(f),failed=ferror(f);board_sd_asset_unlock();
        if(close_file(f))failed=1;
        if(failed) {errno=EIO;return 0;}
        memcpy(buf,words,sizeof words);uint32_t sum=2166136261u;
        for(unsigned i=0;i<20;i++)sum=(sum^buf[i])*16777619u;
        if(n!=sizeof words || extra!=EOF || words[0]!=0x494d5532u || words[2]>1 ||
           (words[3]!=25 && words[3]!=50 && words[3]!=100) || words[4]>10000 || words[5]!=sum) {errno=EINVAL;return 0;}
    }
    return 1;
}
static void do_put(const xfer_io *io, const char *rel, long size, uint8_t *buf,int restore)
{
    while(*rel=='/')rel++;
    char path[160], part[176],backup[176],journal[160];
    if (size < 0 || (unsigned long)size>UINT32_MAX || !full_path(path, sizeof path, rel)) { reply(io, "SDPUT ERR path %d",EINVAL); return; }
    if(reserved_path(rel)) {reply(io,"SDPUT ERR reserved %d",EINVAL);return;}
    if(live_path(rel)) {reply(io,"SDPUT ERR active %d",EBUSY);return;}
    int critical=protected_path(rel);
    if(critical && !restore) {reply(io,"SDPUT ERR restore_required %d",EPERM);return;}
    if(critical && (path_prefix(rel,"watch/wifi/") ||
       (path_prefix(rel,"watch/") && prefs_pending && prefs_pending() && !(prefs_loading && prefs_loading())))) {
        reply(io,"SDPUT ERR owner_busy %d",EBUSY);return;
    }
    snprintf(part, sizeof part, "%s.part", path);
    snprintf(backup,sizeof backup,"%s.sdxfer-old",path);
    if(!transaction_path(journal)) {reply(io,"SDPUT ERR path %d",ENAMETOOLONG);return;}
    int old=path_state(backup);
    if(old) {reply(io,"SDPUT ERR staged %d",old<0?errno:EEXIST);return;}
    int staged=path_state(part);
    if(staged) {reply(io,"SDPUT ERR staged %d",staged<0?errno:EEXIST);return;}
    board_sd_asset_lock();
    make_parents(part);
    board_sd_asset_unlock();
    FILE *f = create_file(part);
    if (!f) { reply(io, "SDPUT ERR open %d", errno); return; }
    reply(io, "SDPUT READY");
    uint32_t crc = 0; long got = 0; int ok = 1, block = io->block > 0 && io->block <= 4096 ? io->block : 2048;
    while (ok && got < size) {
        int want = size - got > block ? block : (int)(size - got), have = 0;
        while (have < want) {
            int n = io->read(buf + have, want - have, 3000);
            if (n <= 0) { ok = 0; reply(io, "SDPUT ERR timeout %d offset %ld",ETIMEDOUT,got + have); break; }
            have += n;
        }
        if (!ok) break;
        board_sd_asset_lock();size_t written=fwrite(buf,1,(size_t)have,f);int error=errno;board_sd_asset_unlock();
        if (written != (size_t)have) { ok = 0; reply(io, "SDPUT ERR write %d", error?error:EIO); break; }
        crc = esp_rom_crc32_le(crc, buf, (uint32_t)have);
        got += have;
        reply(io, "SDK %ld", got);             /* the host sends the next block only now */
    }
    if(!ok) {(void)close_file(f);return;}
    if(!durable_close(f)) {reply(io,"SDPUT ERR sync %d",errno);return;}
    upload_transaction t={.magic=0x53445831u,.size=(uint32_t)size,.crc=crc};
    snprintf(t.rel,sizeof t.rel,"%s",rel);t.check=transaction_sum(&t);
    if(!matches_file(part,&t,buf)) {reply(io,"SDPUT ERR verify %d",errno);return;}
    if(critical && !validate_restore(rel,part,buf)) {reply(io,"SDPUT ERR structure %d",errno);return;}
    f=create_file(journal);
    if(!f) {reply(io,"SDPUT ERR transaction %d",errno);return;}
    board_sd_asset_lock();size_t written=fwrite(&t,1,sizeof t,f);board_sd_asset_unlock();
    ok=written==sizeof t;
    if(!durable_close(f))ok=0;
    if(!ok) {reply(io,"SDPUT ERR transaction %d",errno?errno:EIO);return;}
    if(!recover_transaction(buf)) {reply(io,"SDPUT ERR publish %d",errno);return;}
    reply(io, "SDPUT OK %ld %08lx", got, (unsigned long)crc);
}

static void do_get(const xfer_io *io, const char *path, uint8_t *buf)
{
    int error;FILE *f=read_open(path,&error);
    if(!f) {if(error==ENOENT)reply(io,"SDGET MISSING");else reply(io,"SDGET ERR open %d",error);return;}
    long size;uint32_t crc;
    if(!file_crc(f,buf,&size,&crc)) {error=errno;close_file(f);reply(io,"SDGET ERR read %d",error);return;}
    board_sd_asset_lock();int seek=fseek(f,0,SEEK_SET);error=errno;board_sd_asset_unlock();
    if(seek) {close_file(f);reply(io,"SDGET ERR seek %d",error?error:EIO);return;}
    reply(io, "SDGET %ld %08lx", size, (unsigned long)crc);
    char *line = (char *)buf + 2048;            /* "SDG " + 2048 base64 chars + NUL */
    for(;;) {
        board_sd_asset_lock();size_t r=fread(buf,1,GET_CHUNK,f);error=ferror(f)?(errno?errno:EIO):0;board_sd_asset_unlock();
        if(error) {close_file(f);reply(io,"SDGET ERR read %d",error);return;}
        if(!r)break;
        size_t olen = 0;
        memcpy(line, "SDG ", 4);
        if (mbedtls_base64_encode((unsigned char *)line + 4, XFER_BUF - 2048 - 4, &olen, buf, r) != 0) {
            reply(io, "SDGET ERR encode");
            close_file(f);
            return;
        }
        line[4 + olen] = 0;
        io->line(line);
    }
    if(close_file(f))reply(io,"SDGET ERR close %d",errno);else reply(io,"SDGET END");
}

/* 1104 raw bytes => 1472 base64 characters. Including offset/CRC/prefix and
 * newline, every wire frame fits within 1536 bytes (also for 32-bit offsets). */
#define GET2_CHUNK 1104u
#define GET2_RETRIES 5u
#define GET2_ACK_US 3000000LL
/* Returns 1 ACK, 2 retry/timeout, -1 invalid offset. Partial reads and CRLF
 * stay inside a fixed 64-byte buffer and one absolute deadline. Stale ACKs
 * from retransmitted frames are ignored, never mistaken for the next frame. */
static int get2_ack(const xfer_io *io,unsigned long offset,unsigned long next)
{
    char text[64]; unsigned used=0; int overlong=0;
    int64_t deadline=esp_timer_get_time()+GET2_ACK_US;
    for(;;) {
        int64_t left=deadline-esp_timer_get_time();
        if(left<=0)return 2;
        uint8_t ch;
        int timeout=(int)((left+999)/1000);
        int n=io->read(&ch,1,timeout);
        if(n<=0)return 2;
        if(ch!='\n') {
            if(used+1<sizeof text)text[used++]=(char)ch; else overlong=1;
            continue;
        }
        if(!overlong) {
            if(used && text[used-1]=='\r')used--;
            text[used]=0;
            if(used>4 && (!memcmp(text,"SGA ",4)||!memcmp(text,"SGR ",4))) {
                unsigned i=4;
                while(i<used && text[i]>='0' && text[i]<='9')i++;
                if(i==used) {
                    char *end; errno=0; unsigned long value=strtoul(text+4,&end,10);
                    if(!errno && !*end) {
                        if(text[2]=='A') {
                            if(value==next)return 1;
                            if(value>offset)return -1;
                        } else {
                            if(value==offset)return 2;
                            if(value>offset)return -1;
                        }
                    }
                }
            }
        }
        used=0;overlong=0;
    }
}
static void do_get2(const xfer_io *io,const char *path,uint8_t *buf)
{
    const char *error=NULL;int error_number=EIO;
    board_sd_asset_lock();
    FILE *f=fopen(path,"rb");
    int open_error=f?0:errno;
    long size=-1;
    if(f && !fseek(f,0,SEEK_END))size=ftell(f);
    int seek_failed=f && (size<0 || fseek(f,0,SEEK_SET));
    if(seek_failed)error_number=errno?errno:EIO;
    board_sd_asset_unlock();
    if(!f) {if(open_error==ENOENT)reply(io,"SDGET2 MISSING");else reply(io,"SDGET2 ERR open %d",open_error);return;}
    if(seek_failed) { error="seek";goto done; }
    /* Capture a stable declared prefix. An appending recorder never extends
     * this CRC pass or the subsequent stream beyond the opening file size. */
    uint32_t crc=0;unsigned long remaining=(unsigned long)size;
    while(remaining) {
        size_t want=remaining>XFER_BUF?XFER_BUF:(size_t)remaining;
        board_sd_asset_lock();size_t have=fread(buf,1,want,f);int read_error=ferror(f)?(errno?errno:EIO):EIO;board_sd_asset_unlock();
        if(have!=want) { error="read_crc";error_number=read_error;goto done; }
        crc=esp_rom_crc32_le(crc,buf,(uint32_t)have);remaining-=(unsigned long)have;
    }
    board_sd_asset_lock();int seek=fseek(f,0,SEEK_SET);int seek_error=errno;board_sd_asset_unlock();
    if(seek) { error="seek";error_number=seek_error?seek_error:EIO;goto done; }
    reply(io,"SDGET2 %ld %08lx",size,(unsigned long)crc);
    unsigned long offset=0;uint32_t sent_crc=0;
    char *line=(char *)buf+2048;
    while(offset<(unsigned long)size) {
        remaining=(unsigned long)size-offset;
        size_t want=remaining>GET2_CHUNK?GET2_CHUNK:(size_t)remaining;
        board_sd_asset_lock();size_t have=fread(buf,1,want,f);int read_error=ferror(f)?(errno?errno:EIO):EIO;board_sd_asset_unlock();
        if(have!=want) { error="read_frame";error_number=read_error;goto done; }
        uint32_t chunk_crc=esp_rom_crc32_le(0,buf,(uint32_t)have);
        int prefix=snprintf(line,XFER_BUF-2048,"SDG2 %lu %08lx ",offset,(unsigned long)chunk_crc);
        size_t encoded=0;
        if(prefix<=0 || prefix>48 || mbedtls_base64_encode((unsigned char *)line+prefix,
            XFER_BUF-2048-(size_t)prefix,&encoded,buf,have)) { error="encode";goto done; }
        line[(size_t)prefix+encoded]=0;
        if((size_t)prefix+encoded+1>1536) { error="frame_size";goto done; }
        unsigned retry=0;
        for(;;) {
            /* Neither line transmission nor raw ACK waits hold the SD mutex. */
            io->line(line);
            int ack=get2_ack(io,offset,offset+(unsigned long)have);
            if(ack==1)break;
            if(ack<0) { error="ack_offset";error_number=EINVAL;goto done; }
            if(retry++>=GET2_RETRIES) { error="ack_timeout";error_number=ETIMEDOUT;goto done; }
        }
        sent_crc=esp_rom_crc32_le(sent_crc,buf,(uint32_t)have);
        offset+=(unsigned long)have;
    }
    if(sent_crc!=crc)error="file_changed";
done:
    board_sd_asset_lock();int closed=fclose(f);board_sd_asset_unlock();
    if(error)reply(io,"SDGET2 ERR %s %d",error,error_number);
    else if(closed)reply(io,"SDGET2 ERR close %d",errno?errno:EIO);
    else reply(io,"SDGET2 END");
}

int sd_xfer_exec(const char *s, const xfer_io *io)
{
    if(!strcmp(s,"sdfeatures")) { reply(io,"SDXFER v=2 paced_get=1 typed_errors=1 safe_put=1 restore_put=1");return 1; }
    char rel[128]; long size;
    int restore=!strncmp(s,"sdput! ",7);
    int put = sscanf(s, restore?"sdput! %127s %ld":"sdput %127s %ld", rel, &size) == 2;
    int get2=!put && (!strcmp(s,"sdget2") || !strncmp(s,"sdget2 ",7));
    if(get2 && sscanf(s,"sdget2 %127s",rel)!=1) { reply(io,"SDGET2 ERR path %d",EINVAL);return 1; }
    int get = !put && !get2 && sscanf(s, "sdget %127s", rel) == 1;
    int crc = !put && !get2 && !get && sscanf(s, "sdcrc %127s", rel) == 1;
    int ls = !put && !get2 && !get && !crc && (sscanf(s, "sdls %127s", rel) == 1 || !strcmp(s, "sdls"));
    int rm = !put && !get2 && !get && !crc && !ls && sscanf(s, "sdrm %127s", rel) == 1;
    if (!put && !get2 && !get && !crc && !ls && !rm) return 0;
    const char *prefix=put?"SDPUT":get2?"SDGET2":get?"SDGET":crc?"SDCRC":ls?"SDLS":"SDRM";
    int mutating=put || rm;
    if(mutating && !mutation_begin()){reply(io,"%s ERR busy %d",prefix,EBUSY);return 1;}
    /* Mount lifetime only: no SD mutex is held while waiting on USB/BLE. */
    if (!board_sd_asset_acquire()) {if(mutating)mutation_end();reply(io,"%s ERR unavailable %d",prefix,ENODEV);return 1;}
    if (ls && !strcmp(s, "sdls")) strcpy(rel, "");
    uint8_t *buf = nos_mem_alloc(XFER_BUF, "sd_xfer");
    if (!buf) {board_sd_asset_release();if(mutating)mutation_end();reply(io,"%s ERR nomem %d",prefix,ENOMEM);return 1;}
    if(!transfer_claim()) {nos_mem_free(buf);board_sd_asset_release();if(mutating)mutation_end();reply(io,"%s ERR busy %d",prefix,EBUSY);return 1;}
    transfer_mutation_owned=mutating;
    char path[160];
    if(!put && !full_path(path,sizeof path,rel)) {reply(io,"%s ERR path %d",prefix,EINVAL);goto complete;}
    if(!recover_transaction(buf)) {
        int error=errno;
        /* Preserve read-only access to existing evidence while an unreadable or
         * corrupt transaction blocks publication. An absent target may be in
         * its backup phase, so never report it as MISSING in that state. */
        if(!ls && !((get || get2 || crc) && path_state(path)==1)) {
            reply(io,"%s ERR recovery %d",prefix,error);goto complete;
        }
    }
    if (put) do_put(io, rel, size, buf,restore);
    else if(get2)do_get2(io,path,buf);
    else if (get) do_get(io, path, buf);
    else if (crc) {
        int error;FILE *f=read_open(path,&error);
        if(!f) {if(error==ENOENT)reply(io,"SDCRC MISSING");else reply(io,"SDCRC ERR open %d",error);}
        else {
            long n;uint32_t c;int ok=file_crc(f,buf,&n,&c);error=errno;
            if(close_file(f)) {ok=0;error=errno;}
            if(ok)reply(io,"SDCRC %ld %08lx",n,(unsigned long)c);else reply(io,"SDCRC ERR read %d",error?error:EIO);
        }
    } else if (ls) {
        board_sd_asset_lock();DIR *d=opendir(path);int error=d?0:errno;board_sd_asset_unlock();
        if (d) {
            struct dirent *e;
            for(;;) {
                board_sd_asset_lock();errno=0;e=readdir(d);int entry_error=errno;board_sd_asset_unlock();
                if(!e) {if(entry_error)error=entry_error;break;}
                static char p2[300]; struct stat st;   /* off the command task's stack (BLE notify is deep) */
                snprintf(p2, sizeof p2, "%s/%s", path, e->d_name);
                board_sd_asset_lock();int found=stat(p2,&st)==0,stat_error=errno;board_sd_asset_unlock();
                if(found)reply(io,"SDLS %ld %s%s",(long)st.st_size,e->d_name,S_ISDIR(st.st_mode)?"/":"");
                else if(stat_error!=ENOENT) {error=stat_error;break;}
            }
            board_sd_asset_lock();if(closedir(d) && !error)error=errno?errno:EIO;board_sd_asset_unlock();
        }
        if(error && error!=ENOENT)reply(io,"SDLS ERR read %d",error);else reply(io,"SDLS END");
    } else {
        if(protected_path(rel))reply(io,"SDRM ERR protected %d",EPERM);
        else if(live_path(rel))reply(io,"SDRM ERR active %d",EBUSY);
        else if(!remove_file(path))reply(io,"SDRM OK");else reply(io,"SDRM ERR remove %d",errno);
    }
complete:
    transfer_release();
    nos_mem_free(buf);
    board_sd_asset_release();
    if(mutating)mutation_end();
    return 1;
}

/* ---- the USB transport ---- */
static int usb_read(uint8_t *b, int n, int timeout_ms)
{
    return usb_serial_jtag_read_bytes(b, (uint32_t)n, pdMS_TO_TICKS(timeout_ms));
}
static void usb_line(const char *s)
{
    printf("%s\n", s);                          /* one call: atomic against other tasks' lines */
    fflush(stdout);
}
static const xfer_io USB_IO = { usb_read, usb_line, 2048 };   /* 2 KB: the driver's rx ring is 4 KB */
int sd_xfer_line(const char *s) { return sd_xfer_exec(s, &USB_IO); }
