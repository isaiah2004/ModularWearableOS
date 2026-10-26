/* Link native regression programs with -D_assert=nos_test_assert and
 * -D_wassert=nos_test_wassert. Failed assertions exit, never invoke WER/CRT UI. */
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#ifdef _WIN32
#include <windows.h>
static void __attribute__((constructor)) no_dialog(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
}
#endif
void nos_test_assert(const char *message,const char *file,unsigned line)
{ fprintf(stderr,"FAIL %s:%u: %s\n",file,line,message); exit(2); }
void nos_test_wassert(const wchar_t *message,const wchar_t *file,unsigned line)
{ fwprintf(stderr,L"FAIL %ls:%u: %ls\n",file,line,message); exit(2); }
