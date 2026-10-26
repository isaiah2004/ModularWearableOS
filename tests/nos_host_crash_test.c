#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static unsigned reports,reads,clears,used;
static int clear_ok=1,read_result=4;
static char reply[2048];
static int cmd_printf(const char *format,...)
{va_list args;va_start(args,format);int n=vsnprintf(reply+used,sizeof reply-used,format,args);va_end(args);assert(n>=0 && (unsigned)n<sizeof reply-used);used+=(unsigned)n;return n;}
static void nos_crash_report(void){reports++;}
static int nos_crash_clear(void){clears++;return clear_ok;}
static int nos_crash_read(uint32_t offset,void *out,uint32_t bytes)
{reads++;assert(bytes<=256);if(read_result>0)memset(out,0xA7,(unsigned)read_result<=bytes?(unsigned)read_result:bytes);return read_result;}
/* ACTUAL FUNCTIONS */
static int run(const char *s){used=0;reply[0]=0;return host_crash_command(s);}
int main(void)
{
    assert(run("crash")==1 && reports==1 && !reads && !clears);
    assert(run("crash read 0 4")==1 && reads==1 && !clears && strstr(reply,"bytes=4 hex=a7a7a7a7\n"));
    assert(run("crash read 3 256")==1 && reads==2 && !clears); /* bounded final partial read */
    const char *bad[]={"crash read 0 0","crash read 0 257","crash read -1 4","crash read 0 -1","crash read 4294967296 1","crash read 4294967295 2","crash read 184467440737095516160 2","crash read 0 4 tail","crash read 1x 4","crash read 0 4 ","crash read 0  4"};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;i++)assert(run(bad[i])==0 && strstr(reply,"ERR") && reads==2 && !clears);
    read_result=-1;assert(!run("crash read 0 4") && reads==3 && !clears);
    clear_ok=0;assert(!run("crash clear") && clears==1);
    clear_ok=1;assert(run("crash clear")==1 && clears==2);
    assert(run("crash clear later")==-1 && clears==2);
    puts("PASS actual crash commands: read-only report,256-byte bound, strict decimal/range/short-read handling, clear only explicit exact operator command");
    return 0;
}
