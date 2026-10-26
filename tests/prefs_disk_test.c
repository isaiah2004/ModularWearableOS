#include "prefs_disk.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *dir = argv[1];
    char a[44] = "brightness=43 solar", b[44] = "brightness=59 solar", out[44];
    char path[512];
    assert(!prefs_disk_load(dir, out, sizeof out));
    assert(prefs_disk_save(dir, a, sizeof a));
    assert(prefs_disk_load(dir, out, sizeof out) && !memcmp(a,out,sizeof a));
    assert(prefs_disk_save(dir, b, sizeof b));
    assert(prefs_disk_save(dir, b, sizeof b)); /* unchanged checkpoint is a no-op */
    assert(prefs_disk_load(dir, out, sizeof out) && !memcmp(b,out,sizeof b));
    snprintf(path,sizeof path,"%s/settings.1.bin",dir);
    FILE *f=fopen(path,"r+b");assert(f);assert(!fseek(f,20,SEEK_SET));
    assert(fputc(255,f)!=EOF);assert(!fclose(f));
    assert(prefs_disk_load(dir,out,sizeof out) && !memcmp(a,out,sizeof a));
    assert(prefs_disk_save(dir,b,sizeof b)); /* repairs damaged slot */
    f=fopen(path,"wb");assert(f);assert(fwrite("partial",1,7,f)==7);assert(!fclose(f));
    assert(prefs_disk_load(dir,out,sizeof out) && !memcmp(a,out,sizeof a));
    assert(!prefs_disk_load(dir,out,sizeof out-1)); /* incompatible payload */
    assert(!prefs_disk_save("nonexistent-directory/settings",b,sizeof b));
    assert(prefs_disk_save(dir,b,sizeof b));
    assert(prefs_disk_load(dir,out,sizeof out) && !memcmp(b,out,sizeof b));
    puts("SD preferences: round trip, no-op, corruption, interrupted write and repair passed");
    return 0;
}
