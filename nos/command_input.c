#include "command_input.h"
#include <string.h>
#include <stdint.h>
void command_input_reset(command_input *s) { memset(s,0,sizeof *s); }
static void bad(command_input *s,unsigned reason,unsigned byte,uint32_t offset)
{ if(!s->reason) {s->reason=(uint8_t)reason;s->first_bad=(uint8_t)byte;s->first_bad_offset=offset;} }
int command_input_push(command_input *s,uint8_t byte)
{
    if(byte=='\n') {
        s->text[s->used]=0;
        return s->reason?COMMAND_INPUT_REJECTED:s->nonspace?COMMAND_INPUT_READY:COMMAND_INPUT_EMPTY;
    }
    if(s->bytes<UINT32_MAX)s->bytes++;
    if(s->pending_cr) {
        bad(s,COMMAND_INPUT_BAD_CONTROL,'\r',s->pending_cr_offset);
        s->pending_cr=0;s->pending_cr_offset=0;
    }
    if(byte=='\r') {s->pending_cr=1;s->pending_cr_offset=s->bytes;return COMMAND_INPUT_NONE;}
    if(!byte) {
        if(s->nul_count<UINT32_MAX)s->nul_count++;
        if(!s->first_nul_offset)s->first_nul_offset=s->bytes;
        s->last_nul_offset=s->bytes;
    }
    if((byte<32 && byte!='\t') || byte==127) {bad(s,COMMAND_INPUT_BAD_CONTROL,byte,s->bytes);return COMMAND_INPUT_NONE;}
    if(s->used>=COMMAND_INPUT_CAP-1)bad(s,COMMAND_INPUT_TOO_LONG,0,s->bytes);
    else if(!s->reason) {s->text[s->used++]=(char)byte;if(byte!=' ' && byte!='\t')s->nonspace=1;}
    return COMMAND_INPUT_NONE;
}
