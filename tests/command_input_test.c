#include "command_input.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int feed(command_input *s,const unsigned char *bytes,size_t n)
{int result=0;for(size_t i=0;i<n;i++){result=command_input_push(s,bytes[i]);if(i+1<n)assert(result==COMMAND_INPUT_NONE);}return result;}
static void metadata(void)
{
    command_input s;
    const unsigned char original[]="req 0123abcd gb {\"name\":\"caf\xc3\xa9\"}\t\r\n";
    unsigned char changed[sizeof original+1];
    size_t size=sizeof original-1;
    /* NUL replacement and insertion at every byte, including UTF-8 and CRLF.
     * Replacing LF needs one final LF to finish the now-invalid frame. */
    for(size_t i=0;i<size;i++) {
        memcpy(changed,original,size);changed[i]=0;
        size_t n=size;if(i==size-1)changed[n++]='\n';
        command_input_reset(&s);assert(feed(&s,changed,n)==COMMAND_INPUT_REJECTED);
        assert(s.nul_count==1 && s.first_nul_offset==i+1 && s.last_nul_offset==i+1);
        assert(s.bytes==n-1 && s.reason==COMMAND_INPUT_BAD_CONTROL);
        assert(s.first_bad_offset==(i==size-1?i:i+1));
        assert(s.first_bad==(i==size-1?'\r':0));
    }
    for(size_t i=0;i<size;i++) {
        memcpy(changed,original,i);changed[i]=0;memcpy(changed+i+1,original+i,size-i);
        command_input_reset(&s);assert(feed(&s,changed,size+1)==COMMAND_INPUT_REJECTED);
        assert(s.nul_count==1 && s.first_nul_offset==i+1 && s.last_nul_offset==i+1);
        assert(s.bytes==size && s.reason==COMMAND_INPUT_BAD_CONTROL);
        assert(s.first_bad_offset==(i==size-1?i:i+1));
    }
    /* The first oversized ordinary byte is an overflow with bad=0, not a NUL.
     * Actual later NULs still update their independent metadata. */
    unsigned char oversized[COMMAND_INPUT_CAP+33];
    memset(oversized,'x',sizeof oversized);oversized[sizeof oversized-1]='\n';
    for(size_t i=0;i<sizeof oversized-1;i++) {
        oversized[i]=0;command_input_reset(&s);
        assert(feed(&s,oversized,sizeof oversized)==COMMAND_INPUT_REJECTED);
        assert(s.nul_count==1 && s.first_nul_offset==i+1 && s.last_nul_offset==i+1);
        assert(s.first_bad_offset==(i<COMMAND_INPUT_CAP?i+1:COMMAND_INPUT_CAP));
        assert(s.reason==(i<COMMAND_INPUT_CAP?COMMAND_INPUT_BAD_CONTROL:COMMAND_INPUT_TOO_LONG));
        oversized[i]='x';
    }
    command_input_reset(&s);assert(feed(&s,oversized,sizeof oversized)==COMMAND_INPUT_REJECTED);
    assert(s.reason==COMMAND_INPUT_TOO_LONG && !s.first_bad && s.first_bad_offset==COMMAND_INPUT_CAP);
    assert(!s.nul_count && !s.first_nul_offset && !s.last_nul_offset);
    const unsigned char multiple[]={'a','\r',0,'b',0,'\r','\n'};
    command_input_reset(&s);assert(feed(&s,multiple,sizeof multiple)==COMMAND_INPUT_REJECTED);
    assert(s.first_bad=='\r' && s.first_bad_offset==2 && s.nul_count==2);
    assert(s.first_nul_offset==3 && s.last_nul_offset==5);
    command_input_reset(&s);assert(feed(&s,(const unsigned char *)"ok\r\n",4)==COMMAND_INPUT_READY);
    assert(!s.reason && !s.first_bad_offset && !s.nul_count);
    /* Retain the CR's actual saturated position, rather than subtracting from
     * an already saturated current-byte counter. */
    command_input_reset(&s);s.bytes=UINT32_MAX-1;
    assert(command_input_push(&s,'\r')==COMMAND_INPUT_NONE);
    assert(command_input_push(&s,'x')==COMMAND_INPUT_NONE);
    assert(s.first_bad=='\r' && s.first_bad_offset==UINT32_MAX && s.bytes==UINT32_MAX);
    command_input_reset(&s);s.bytes=UINT32_MAX-2;
    assert(command_input_push(&s,'\r')==COMMAND_INPUT_NONE);
    assert(command_input_push(&s,0)==COMMAND_INPUT_NONE);
    assert(s.first_bad_offset==UINT32_MAX-1 && s.first_nul_offset==UINT32_MAX);
    s.nul_count=UINT32_MAX-1;
    assert(command_input_push(&s,0)==COMMAND_INPUT_NONE);
    assert(command_input_push(&s,0)==COMMAND_INPUT_NONE);
    assert(s.bytes==UINT32_MAX && s.nul_count==UINT32_MAX && s.last_nul_offset==UINT32_MAX);
    command_input_reset(&s);command_input zero={0};assert(!memcmp(&s,&zero,sizeof s));
    assert(feed(&s,(const unsigned char *)"info\n",5)==COMMAND_INPUT_READY);
    assert(!s.nul_count && !s.first_nul_offset && !s.last_nul_offset && !s.first_bad_offset);
    puts("Numeric rejection offsets/NULs: per-byte UTF8/CRLF/overflow, multiple, saturation and reset PASS");
}
int main(void)
{
    metadata();
    command_input s;command_input_reset(&s);
    assert(feed(&s,(const unsigned char *)"in",2)==COMMAND_INPUT_NONE);
    assert(feed(&s,(const unsigned char *)"fo\r",3)==COMMAND_INPUT_NONE);
    assert(command_input_push(&s,'\n')==COMMAND_INPUT_READY);assert(!strcmp(s.text,"info"));assert(s.bytes==5);
    command_input_reset(&s);
    const char *utf8="gb {\"name\":\"caf\xc3\xa9\"}\t  \n";
    assert(feed(&s,(const unsigned char *)utf8,strlen(utf8))==COMMAND_INPUT_READY);
    assert(!memcmp(s.text,utf8,strlen(utf8)-1));
    command_input_reset(&s);
    const unsigned char nul[]={0,'i','n','f','o','\n'};
    assert(feed(&s,nul,sizeof nul)==COMMAND_INPUT_REJECTED);
    assert(s.reason==COMMAND_INPUT_BAD_CONTROL && s.first_bad==0 && s.bytes==5);
    assert(s.first_bad_offset==1 && s.nul_count==1 && s.first_nul_offset==1 && s.last_nul_offset==1);
    command_input_reset(&s);
    assert(feed(&s,(const unsigned char *)"in\rfo\n",6)==COMMAND_INPUT_REJECTED);
    assert(s.first_bad==13 && s.first_bad_offset==3 && !s.nul_count);
    command_input_reset(&s);
    assert(feed(&s,(const unsigned char *)"info\x7f\n",6)==COMMAND_INPUT_REJECTED);assert(s.first_bad==127);
    assert(s.first_bad_offset==5 && !s.nul_count);
    command_input_reset(&s);
    for(unsigned i=0;i<191;i++)assert(command_input_push(&s,'x')==COMMAND_INPUT_NONE);
    assert(command_input_push(&s,'\r')==COMMAND_INPUT_NONE);
    assert(command_input_push(&s,'\n')==COMMAND_INPUT_READY && strlen(s.text)==191);
    command_input_reset(&s);
    for(unsigned i=0;i<1000000;i++)assert(command_input_push(&s,'x')==COMMAND_INPUT_NONE);
    assert(command_input_push(&s,'\n')==COMMAND_INPUT_REJECTED);
    assert(s.reason==COMMAND_INPUT_TOO_LONG && !s.first_bad && s.bytes==1000000 && s.used==191);
    assert(s.first_bad_offset==192 && !s.nul_count && !s.first_nul_offset && !s.last_nul_offset);
    command_input_reset(&s);
    assert(feed(&s,(const unsigned char *)" \t\r\n",4)==COMMAND_INPUT_EMPTY);
    command_input_reset(&s);assert(command_input_push(&s,'\n')==COMMAND_INPUT_EMPTY);
    puts("Split/CRLF/UTF8/spaces/tabs, NUL/DEL/internalCR, bounded overflow and empty: PASS");
    /* Mirror synchronous cmd_task routing: the SD handler consumes its raw
     * ACK from io->read before the byte-at-a-time command parser resumes. */
    const unsigned char stream[]="sdget2 imu/x.csv\nSGA 1104\r\ninfo\n";
    unsigned commands=0;command_input_reset(&s);
    for(size_t i=0;i<sizeof stream-1;i++) {
        int result=command_input_push(&s,stream[i]);
        if(result==COMMAND_INPUT_NONE)continue;
        assert(result==COMMAND_INPUT_READY);commands++;
        if(!strncmp(s.text,"sdget2 ",7)) {
            size_t start=++i;while(stream[i]!='\n')i++;
            assert(i-start==9 && !memcmp(stream+start,"SGA 1104\r",9));
        } else assert(!strcmp(s.text,"info"));
        command_input_reset(&s);
    }
    assert(commands==2);
    const unsigned char upload[]={'s','d','p','u','t',' ','x',' ','3','\n','A',0,'B','i','n','f','o','\n'};
    commands=0;command_input_reset(&s);
    for(size_t i=0;i<sizeof upload;i++) {
        int result=command_input_push(&s,upload[i]);if(!result)continue;
        assert(result==COMMAND_INPUT_READY);commands++;
        if(!strncmp(s.text,"sdput ",6)){assert(upload[i+2]==0);i+=3;}
        else assert(!strcmp(s.text,"info"));
        command_input_reset(&s);
    }
    assert(commands==2);puts("Raw download ACK/upload NUL bypass preserves following command: PASS");
    return 0;
}
