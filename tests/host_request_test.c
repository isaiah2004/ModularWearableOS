#include "host_request.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void parse(const char *text,int wanted,const char *wanted_id,const char *wanted_command)
{
    char id[9];const char *command=NULL;
    int result=host_request_parse(text,id,&command);assert(result==wanted);
    if(wanted>=0) {assert(!strcmp(id,wanted_id));assert(!strcmp(command,wanted_command));}
}
int main(void)
{
    parse("req a012bcff prefs flush",1,"a012bcff","prefs flush");
    parse("hostcaps",0,"","hostcaps");parse("",0,"","");
    parse("req",-1,"","");parse("req ABCDEF12 home",-1,"","");
    parse("req 1234567 home",-1,"","");parse("req 12345678 ",-1,"","");
    parse("req 12345678  home",-1,"","");parse("req 12345678 ho\tme",-1,"","");
    parse("req 12345678 home\r",-1,"","");
    char large[194]="req 12345678 ";memset(large+13,'a',178);large[191]=0;
    parse(large,1,"12345678",large+13);large[191]='b';large[192]=0;parse(large,-1,"","");
    for(int i=0;i<256;i++) {
        char b[]="req 12345678 state";b[14]=(char)i;
        char id[9];const char *cmd;int r=host_request_parse(b,id,&cmd);
        if(i<32 || i==127) assert(r==-1 || i==0);
        else assert(r==1);
    }
    const char *yes[]={"sdfeatures","sdls watch","sdget2 imu/0002.csv","sdput! watch/settings.0.bin 52","sdrm old","sdcrc watch/log"};
    for(unsigned i=0;i<sizeof yes/sizeof yes[0];i++) assert(host_request_transfer(yes[i]));
    const char *no[]={"sd mount","sd","sdget20 foo","sdputter foo","sdrmfoo","req 12345678 sdls watch"};
    for(unsigned i=0;i<sizeof no/sizeof no[0];i++) assert(!host_request_transfer(no[i]));
    puts("PASS: correlated envelope boundaries, control rejection and raw transfer dispatch");
    return 0;
}
