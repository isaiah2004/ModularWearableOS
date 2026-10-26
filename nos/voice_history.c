#include "voice_history.h"
#include "voice_chat.h"
#include <string.h>
#include <math.h>
_Static_assert(sizeof(voice_history)<=4096,"bounded saved text; never persist audio");
static uint32_t sum(const voice_history *h)
{
    const unsigned char *p=(const unsigned char *)h;uint32_t value=2166136261u;
    for(size_t i=0;i<offsetof(voice_history,checksum);i++)value=(value^p[i])*16777619u;
    return value;
}
void voice_history_seal(voice_history *h){if(h)h->checksum=sum(h);}
void voice_history_init(voice_history *h)
{
    memset(h,0,sizeof *h);h->magic=0x56434831u;h->version=1;h->next_id=1;
    h->speak=1;h->mic_gain=30;h->last_volume=60;voice_history_seal(h);
}
int voice_history_valid(const voice_history *h)
{
    if(!h || h->magic!=0x56434831u || h->version!=1 || h->checksum!=sum(h) ||
       h->count>VOICE_SAVED_CHATS || !h->next_id || h->speak>1 ||
       (h->mic_gain!=24 && h->mic_gain!=30 && h->mic_gain!=36) ||
       !h->last_volume || h->last_volume>100)return 0;
    for(unsigned i=0;i<h->count;i++) {
        const voice_saved_chat *c=&h->chat[i];
        if(!c->id || !c->count || c->count>VOICE_SAVED_TURNS || c->turns<c->count ||
           c->tier>=VOICE_TIERS || c->estimated>1 || !memchr(c->title,0,sizeof c->title))return 0;
        for(unsigned j=0;j<i;j++)if(c->id==h->chat[j].id)return 0;
        for(unsigned j=0;j<c->count;j++)if(c->turn[j].excerpt>1 ||
            !memchr(c->turn[j].input,0,sizeof c->turn[j].input) ||
            !memchr(c->turn[j].reply,0,sizeof c->turn[j].reply))return 0;
    }
    return 1;
}
const voice_saved_chat *voice_history_find(const voice_history *h,uint32_t id)
{
    if(h && id)for(unsigned i=0;i<h->count;i++)if(h->chat[i].id==id)return &h->chat[i];
    return NULL;
}
uint32_t voice_history_append(voice_history *h,uint32_t id,const char *input,const char *reply)
{
    if(!h || !input || !*input || !reply || !*reply || h->count>VOICE_SAVED_CHATS)return 0;
    unsigned index=0;
    if(id) {
        for(;index<h->count && h->chat[index].id!=id;index++){}
        if(index==h->count)return 0;
    }else {
        if(h->count==VOICE_SAVED_CHATS)return 0;
        index=h->count++;
        do{id=h->next_id++;if(!h->next_id)h->next_id=1;}while(!id || voice_history_find(h,id));
        memset(&h->chat[index],0,sizeof h->chat[index]);h->chat[index].id=id;
        voice_text_copy(h->chat[index].title,sizeof h->chat[index].title,input);
        for(char *p=h->chat[index].title;*p;p++)if((unsigned char)*p<32)*p=' ';
    }
    /* Small temporary, not a full history copy on the UI's internal stack. */
    voice_saved_chat moved=h->chat[index];
    memmove(h->chat+1,h->chat,index*sizeof h->chat[0]);h->chat[0]=moved;
    voice_saved_chat *c=&h->chat[0];unsigned n=c->count;
    if(n==VOICE_SAVED_TURNS){memmove(c->turn,c->turn+1,(VOICE_SAVED_TURNS-1)*sizeof c->turn[0]);n--;}
    voice_saved_turn *t=&c->turn[n];memset(t,0,sizeof *t);
    t->excerpt=strlen(input)>=sizeof t->input || strlen(reply)>=sizeof t->reply;
    voice_text_copy(t->input,sizeof t->input,input);voice_text_copy(t->reply,sizeof t->reply,reply);
    c->count=(uint8_t)(n+1);c->turns++;return id;
}
int voice_history_delete(voice_history *h,uint32_t id)
{
    if(!h || !id || h->count>VOICE_SAVED_CHATS)return 0;
    for(unsigned i=0;i<h->count;i++)if(h->chat[i].id==id) {
        memmove(h->chat+i,h->chat+i+1,(h->count-i-1)*sizeof h->chat[0]);
        memset(&h->chat[--h->count],0,sizeof h->chat[0]);return 1;
    }
    return 0;
}
void voice_history_metadata(voice_history *h,uint32_t id,unsigned turns,int tier,
                            unsigned input_ms,unsigned output_ms,double cost,int estimated)
{
    voice_saved_chat *c=(voice_saved_chat *)voice_history_find(h,id);if(!c)return;
    c->turns=turns;c->tier=(uint8_t)tier;c->input_ms=input_ms;c->output_ms=output_ms;
    double micros=isfinite(cost) && cost>0?cost*1e6:0;
    c->cost_micro_usd=micros>UINT32_MAX?UINT32_MAX:(uint32_t)(micros+.5);c->estimated=!!estimated;
}
