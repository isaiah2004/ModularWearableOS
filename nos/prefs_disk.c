#include "prefs_disk.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>
#define sync_file(fd) _commit(fd)
#else
#include <unistd.h>
#define sync_file(fd) fsync(fd)
#endif

#define CAPACITY 256
extern int prefs_disk_write_allowed(const char *directory) __attribute__((weak));
typedef struct {
    uint32_t magic, format, sequence, size;
    unsigned char data[CAPACITY];
    uint32_t checksum;
} checkpoint;

static uint32_t checksum(const checkpoint *p)
{
    const unsigned char *b = (const unsigned char *)p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(checkpoint, checksum); i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}
static int path_for(char *path, size_t size, const char *dir, int slot)
{
    int n = snprintf(path, size, "%s/settings.%d.bin", dir, slot);
    return n > 0 && (size_t)n < size;
}
static prefs_disk_read_status read_checkpoint(const char *path, checkpoint *p)
{
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? PREFS_DISK_MISSING : PREFS_DISK_IO;
    size_t got = fread(p, 1, sizeof *p, f);
    int extra = fgetc(f), failed = ferror(f);
    if (fclose(f)) failed = 1;
    if (failed) return PREFS_DISK_IO;
    return got == sizeof *p && extra == EOF && p->magic == 0x4e4f5350 && p->format == 1 &&
           p->size > 0 && p->size <= CAPACITY && p->checksum == checksum(p) ? PREFS_DISK_OK : PREFS_DISK_CORRUPT;
}
static prefs_disk_read_status read_slot(const char *dir, int slot, checkpoint *p)
{
    char path[256];
    if (!path_for(path, sizeof path, dir, slot)) return PREFS_DISK_IO;
    return read_checkpoint(path,p);
}
prefs_disk_read_status prefs_disk_validate_file(const char *path)
{
    checkpoint p;
    return path ? read_checkpoint(path,&p) : PREFS_DISK_CORRUPT;
}
static int newest(checkpoint p[2], const prefs_disk_read_status states[2])
{
    int a = states[0] == PREFS_DISK_OK, b = states[1] == PREFS_DISK_OK;
    if (!a) return b ? 1 : -1;
    if (!b) return 0;
    return (int32_t)(p[1].sequence - p[0].sequence) > 0 ? 1 : 0;
}
prefs_disk_read_status prefs_disk_load_status(const char *dir, void *data, size_t size)
{
    if (!dir || !data || !size || size > CAPACITY) return PREFS_DISK_CORRUPT;
    checkpoint p[2];
    prefs_disk_read_status states[2] = { read_slot(dir, 0, &p[0]), read_slot(dir, 1, &p[1]) };
    if (states[0] == PREFS_DISK_IO || states[1] == PREFS_DISK_IO) return PREFS_DISK_IO;
    int slot = newest(p, states);
    if (slot < 0) return states[0] == PREFS_DISK_CORRUPT || states[1] == PREFS_DISK_CORRUPT ?
                         PREFS_DISK_CORRUPT : PREFS_DISK_MISSING;
    /* Schema migration must inspect the newest generation, even when an older
     * bank happens to match the requested legacy payload size. */
    if (p[slot].size != size) return PREFS_DISK_CORRUPT;
    memcpy(data, p[slot].data, size);
    return 1;
}
int prefs_disk_load(const char *dir, void *data, size_t size)
{ return prefs_disk_load_status(dir, data, size) == PREFS_DISK_OK; }
int prefs_disk_save(const char *dir, const void *data, size_t size)
{
    if (!dir || !data || !size || size > CAPACITY) return 0;
    if(prefs_disk_write_allowed && !prefs_disk_write_allowed(dir)) {errno=EBUSY;return 0;}
    checkpoint old[2], next = {0}, verified;
    prefs_disk_read_status states[2] = { read_slot(dir, 0, &old[0]), read_slot(dir, 1, &old[1]) };
    /* Never truncate an unreadable bank: it may hold the newest durable data. */
    if (states[0] == PREFS_DISK_IO || states[1] == PREFS_DISK_IO) return 0;
    int latest = newest(old, states);
    /* Avoid flash/card wear when the periodic checkpoint has no changes. */
    if (latest >= 0 && old[latest].size == size && !memcmp(old[latest].data, data, size) &&
        states[1 - latest] == PREFS_DISK_OK) return 1;
    int target = latest < 0 ? 0 : 1 - latest;
    next.magic = 0x4e4f5350;
    next.format = 1;
    next.sequence = latest < 0 ? 1 : old[latest].sequence + 1;
    next.size = (uint32_t)size;
    memcpy(next.data, data, size);
    next.checksum = checksum(&next);
    char path[256];
    if (!path_for(path, sizeof path, dir, target)) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(&next, 1, sizeof next, f) == sizeof next;
    if (fflush(f)) ok = 0;
    if (sync_file(fileno(f))) ok = 0;
    if (fclose(f)) ok = 0;
    return ok && read_slot(dir, target, &verified) == PREFS_DISK_OK &&
           !memcmp(&next, &verified, sizeof next);
}
