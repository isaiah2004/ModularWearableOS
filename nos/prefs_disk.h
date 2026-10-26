#ifndef NOS_PREFS_DISK_H
#define NOS_PREFS_DISK_H
#include <stddef.h>
/* Caller serializes access and holds a mounted-card lease.
 * Alternating, checksummed slots retain the previous complete checkpoint. */
typedef enum {
    PREFS_DISK_OK = 1,
    PREFS_DISK_MISSING = 0,
    PREFS_DISK_CORRUPT = -1,
    PREFS_DISK_IO = -2
} prefs_disk_read_status;
/* An unreadable bank may be newer than the readable bank. Leave data unchanged
 * and retry instead of using a fallback that could overwrite that generation. */
prefs_disk_read_status prefs_disk_load_status(const char *directory, void *data, size_t size);
/* Intrinsic format/checksum check for an explicitly staged raw restore. */
prefs_disk_read_status prefs_disk_validate_file(const char *path);
int prefs_disk_load(const char *directory, void *data, size_t size);
int prefs_disk_save(const char *directory, const void *data, size_t size);
/* Optional RAM-owner admission hook. The preferences module blocks its watch
 * directory during raw restore/reload; HAL checks before its NVS mirror too.
 * NULL queries the main preferences owner. Disk-only consumers may omit it. */
int prefs_disk_write_allowed(const char *directory);
#endif
