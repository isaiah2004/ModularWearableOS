#include "voice_history.h"
#include "voice_chat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static voice_history h,copy;
int main(void)
{
    voice_history_init(&h);assert(voice_history_valid(&h) && sizeof h<=4096 && !h.count);
    uint32_t ids[VOICE_SAVED_CHATS];
    for(unsigned i=0;i<VOICE_SAVED_CHATS;i++) {
        char question[64];snprintf(question,sizeof question,"Question %u",i);
        ids[i]=voice_history_append(&h,0,question,"A useful reply.");assert(ids[i]);
        voice_history_metadata(&h,ids[i],1,(int)(i%3),1000,2000,.000123,1);
        voice_history_seal(&h);assert(voice_history_valid(&h));
    }
    copy=h;assert(!voice_history_append(&h,0,"Full","Never evict an older chat"));assert(!memcmp(&h,&copy,sizeof h));
    assert(voice_history_append(&h,ids[0],"Second turn","Reply two.")==ids[0]);
    assert(h.chat[0].id==ids[0] && h.chat[0].count==2);
    assert(voice_history_append(&h,ids[0],"Third turn","Reply three.")==ids[0]);
    assert(h.chat[0].turns==3 && h.chat[0].count==2 && !strcmp(h.chat[0].turn[0].input,"Second turn"));
    assert(!strcmp(h.chat[0].turn[1].reply,"Reply three."));
    char question[500],reply[1700];memset(question,'a',sizeof question-1);question[sizeof question-1]=0;
    memset(reply,'b',sizeof reply-1);reply[sizeof reply-1]=0;
    assert(voice_history_append(&h,ids[0],question,reply));
    assert(h.chat[0].turn[1].excerpt && strlen(h.chat[0].turn[1].input)==VOICE_SAVED_INPUT-1 &&
           strlen(h.chat[0].turn[1].reply)==VOICE_SAVED_REPLY-1);
    memset(question,0,sizeof question);for(unsigned i=0;i<120;i++)strcat(question,"\xe2\x82\xac");
    assert(voice_history_append(&h,ids[0],question,question));
    assert(strlen(h.chat[0].turn[1].input)%3==0 && strlen(h.chat[0].turn[1].reply)%3==0);
    assert(!voice_history_append(&h,0,"",reply));assert(!voice_history_append(&h,999,"Missing",reply));
    voice_history_seal(&h);assert(voice_history_valid(&h));copy=h;
    assert(!voice_history_delete(&h,999) && !memcmp(&h,&copy,sizeof h));
    assert(voice_history_delete(&h,ids[1]));assert(!voice_history_find(&h,ids[1]));
    assert(voice_history_find(&h,ids[0]) && voice_history_find(&h,ids[2]) && voice_history_find(&h,ids[3]));
    h.next_id=UINT32_MAX;uint32_t wrapped=voice_history_append(&h,0,"Wrap safely","Reply");
    assert(wrapped==UINT32_MAX && h.next_id==1);assert(voice_history_delete(&h,wrapped));
    wrapped=voice_history_append(&h,0,"Avoid colliding IDs","Reply");
    assert(wrapped && wrapped!=ids[0] && wrapped!=ids[2] && wrapped!=ids[3]);
    voice_history_seal(&h);assert(voice_history_valid(&h));copy=h;
    h.chat[0].turn[0].reply[0]^=1;assert(!voice_history_valid(&h));h=copy;
    h.chat[0].id=h.chat[1].id;voice_history_seal(&h);assert(!voice_history_valid(&h));h=copy;
    h.chat[0].count=3;voice_history_seal(&h);assert(!voice_history_valid(&h));h=copy;
    memset(h.chat[0].title,'x',sizeof h.chat[0].title);voice_history_seal(&h);assert(!voice_history_valid(&h));h=copy;
    h.version++;voice_history_seal(&h);assert(!voice_history_valid(&h));
    printf("PASS saved chats: %u-byte bound, stable IDs, no eviction, two turns, UTF-8, deletion and corruption\n",(unsigned)sizeof h);
}
