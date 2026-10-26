#include "sd_xfer.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static char reply[4096];static unsigned used,begins,ends,dispatches;
static int capture_vprintf(const char *format,va_list args)
{int n=vsnprintf(reply+used,sizeof reply-used,format,args);assert(n>=0 && (unsigned)n<sizeof reply-used);used+=(unsigned)n;return n;}
static int capture_printf(const char *format,...)
{va_list args;va_start(args,format);int n=capture_vprintf(format,args);va_end(args);return n;}
static int s_cmd_ok,s_transfer_ok;
typedef struct {char text[192],request_id[9];} cmd_line;
static int cmd_exec(char *text);
static void host_transfer_line(const char *line);
static const xfer_io s_host_transfer_io={NULL,host_transfer_line,2048};
static void nos_usb_diag_dispatched(void) {dispatches++;}
static void nos_usb_diag_reply_begin(void) {begins++;}
static void nos_usb_diag_reply_end(void) {ends++;}
#define printf capture_printf
#define vprintf capture_vprintf
#define fflush(stream) ((void)(stream))
/* ACTUAL FUNCTIONS */
static int cmd_exec(char *text)
{
    s_cmd_ok=1;
    if(!strcmp(text,"bad"))cmd_printf("ERR invalid test command\n");
    else if(!strncmp(text,"usb_diag",8) || !strncmp(text,"usb_guard",9))printf("USBDIAG_RAW_FRAME\n");
    else cmd_printf("PAYLOAD accepted=1\n");
    return s_cmd_ok;
}
int sd_xfer_exec(const char *command,const xfer_io *io)
{
    if(!strcmp(command,"missing"))io->line("SDCRC MISSING");
    else if(!strcmp(command,"failed"))io->line("SDCRC ERR read 5");
    else {io->line("SDGET2 DATA 0 4 c3RhZw==");io->line("SDGET2 END 4 12345678");}
    return 1;
}
#undef printf
#undef vprintf
#undef fflush
static void reset(void){used=begins=ends=dispatches=0;reply[0]=0;}
int main(void)
{
    cmd_line line={.text="status"};reset();host_ui_execute(&line);
    assert(!strcmp(reply,"PAYLOAD accepted=1\nDONE\n") && begins==1 && ends==1 && dispatches==1);
    strcpy(line.request_id,"1a2b3c4d");reset();host_ui_execute(&line);
    assert(!strcmp(reply,"BEGIN 1a2b3c4d\nPAYLOAD accepted=1\nDONE 1a2b3c4d ok=1\n"));
    strcpy(line.text,"bad");reset();host_ui_execute(&line);
    assert(!strcmp(reply,"BEGIN 1a2b3c4d\nERR invalid test command\nDONE 1a2b3c4d ok=0 code=command\n"));
    for(int i=0;i<2;i++) {
        strcpy(line.text,i?"usb_guard 0 1234":"usb_diag 1234");line.request_id[0]=0;
        reset();host_ui_execute(&line);assert(!strcmp(reply,"USBDIAG_RAW_FRAME\n"));
        strcpy(line.request_id,"1a2b3c4d");reset();host_ui_execute(&line);
        assert(!strcmp(reply,"BEGIN 1a2b3c4d\nUSBDIAG_RAW_FRAME\nDONE 1a2b3c4d ok=1\n"));
    }
    reset();host_transfer_execute("1a2b3c4d","missing");
    assert(!strcmp(reply,"BEGIN 1a2b3c4d\nSDCRC MISSING\nDONE 1a2b3c4d ok=1\n"));
    reset();host_transfer_execute("1a2b3c4d","failed");
    assert(!strcmp(reply,"BEGIN 1a2b3c4d\nSDCRC ERR read 5\nDONE 1a2b3c4d ok=0 code=transfer\n"));
    reset();host_transfer_execute("","okay");
    assert(!strcmp(reply,"SDGET2 DATA 0 4 c3RhZw==\nSDGET2 END 4 12345678\n"));
    reset();host_transfer_execute("1a2b3c4d","okay");
    assert(!strcmp(reply,"BEGIN 1a2b3c4d\nSDGET2 DATA 0 4 c3RhZw==\nSDGET2 END 4 12345678\nDONE 1a2b3c4d ok=1\n"));
    const char *mutations[]={"prefs flush","open timer","gb notification","ext rec start path","crash clear","board retry","wake learn"};
    for(unsigned i=0;i<sizeof mutations/sizeof *mutations;i++) {
        assert(!host_command_readonly(mutations[i]));strcpy(line.text,mutations[i]);reset();host_ui_dispatch(&line,0);
        assert(!strcmp(reply,"BEGIN 1a2b3c4d\nERR update activity busy\nDONE 1a2b3c4d ok=0 code=busy\n"));
        assert(begins==1 && ends==1 && dispatches==1);
    }
    assert(host_command_readonly("prefs") && host_command_readonly("ota cancel") && host_command_readonly("crash read 0 4"));
    assert(host_command_readonly("runtime_state"));
    strcpy(line.text,"runtime_state");reset();host_ui_dispatch(&line,0);
    assert(!strcmp(reply,"BEGIN 1a2b3c4d\nPAYLOAD accepted=1\nDONE 1a2b3c4d ok=1\n"));
    strcpy(line.text,"hostcaps");reset();host_ui_dispatch(&line,0);
    assert(!strcmp(reply,"BEGIN 1a2b3c4d\nPAYLOAD accepted=1\nDONE 1a2b3c4d ok=1\n"));
    strcpy(line.text,"open timer");reset();host_ui_dispatch(&line,1);assert(strstr(reply,"accepted=1") && !strstr(reply,"ERR"));
    puts("PASS actual host wrappers: one BEGIN/DONE, UI rejection, legacy raw frame, SD payload, MISSING success, I/O failure");
    return 0;
}
