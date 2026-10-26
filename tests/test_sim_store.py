"""Fault-inject the actual simulator store section; do not build the shared sim."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#define CreateDirectoryA(p,unused) mkdir(p,0700)
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_WRITE_THROUGH 2
#define _commit fsync
static int MoveFileExA(const char *from,const char *to,int flags) {
    (void)flags; return rename(from,to)==0;
}
#endif
static int fault;
static void *checked_malloc(size_t n) { return fault==1 ? NULL : malloc(n); }
static FILE *checked_open(const char *p,const char *m) { return fault==2 ? NULL : fopen(p,m); }
static size_t checked_write(const void *p,size_t s,size_t n,FILE *f) {
    return fault==3 ? 0 : fwrite(p,s,n,f);
}
static int checked_flush(FILE *f) { return fault==4 ? -1 : fflush(f); }
static int checked_commit(int fd) { return fault==5 ? -1 : _commit(fd); }
static int checked_close(FILE *f) { int r=fclose(f); return fault==6 ? -1 : r; }
static int checked_move(const char *from,const char *to,int flags) {
    return fault==7 ? 0 : MoveFileExA(from,to,flags);
}
#define malloc checked_malloc
#define fopen checked_open
#define fwrite checked_write
#define fflush checked_flush
#define _commit checked_commit
#define fclose checked_close
#define MoveFileExA checked_move
int sim_store_disabled;
char sim_store_dir[260]="store";
'''
SUFFIX = r'''
#undef malloc
#undef fopen
#undef fwrite
#undef fflush
#undef _commit
#undef fclose
#undef MoveFileExA
int main(void) {
    int old=42,next=73,loaded=0;
    assert(hal_store_save_checked("existing",&old,sizeof old));
    for(int f=1;f<=7;f++) {
        fault=f;
        assert(!hal_store_save_checked("existing",&next,sizeof next));
        fault=0;
        assert(s_nstore==1);
        assert(hal_store_load("existing",&loaded,sizeof loaded) && loaded==old);
        FILE *disk=fopen("store/store_existing.dat","rb");
        assert(disk && fread(&loaded,sizeof loaded,1,disk)==1 && loaded==old);
        assert(!fclose(disk));
    }
    fault=1;
    assert(!hal_store_save_checked("new",&next,sizeof next));
    assert(s_nstore==1 && store_find("new")==-1);
    fault=0;
    assert(hal_store_save_checked("existing",&next,sizeof next));
    assert(hal_store_load("existing",&loaded,sizeof loaded) && loaded==next);
    sim_store_disabled=1;fault=1;
    assert(!hal_store_save_checked("existing",&old,sizeof old));
    fault=0;
    assert(hal_store_load("existing",&loaded,sizeof loaded) && loaded==next);
    puts("PASS allocation/open/write/flush/commit/close/replace failures preserve old RAM and disk");
    return 0;
}
'''

class SimulatorStoreTests(unittest.TestCase):
    def test_prior_ram_and_disk_survive_every_fallible_stage(self):
        compiler = shutil.which('gcc')
        if not compiler: self.skipTest('Host GCC required')
        source = (ROOT / 'sim/nos_hal_sim.c').read_text(encoding='utf-8')
        section = source[source.index('#define STORE_MAX'):source.index('/* ---- display / log')]
        with tempfile.TemporaryDirectory(prefix='sim-store-') as temporary:
            directory = Path(temporary); harness = directory / 'store_test.c'; executable = directory / 'store_test.exe'
            harness.write_text(PREFIX + section + SUFFIX, encoding='utf-8')
            compilation = subprocess.run([compiler, '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                                          str(harness), '-o', str(executable)], capture_output=True, text=True)
            self.assertEqual(compilation.returncode, 0, compilation.stdout + compilation.stderr)
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__': unittest.main()
