#include "voice_chat.h"
#include "prefs.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define HISTORY 4
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static int lock;
static struct {
    voice_chat_snapshot view;
    voice_history archive;
    char history_in[HISTORY][384],history_out[HISTORY][1408];
    unsigned history_count;
    atomic_int busy,cancel,history_dirty,speak_replies,output_volume;
    int history_ok,history_error;
    uint32_t history_retry,stage_started;
} fixture,*s=&fixture;
nos_prefs_t nos_prefs;
static uint32_t now;
static int fail_alloc,fail_save,bad_read,save_calls,read_calls,pref_calls,levels_calls;
static int last_volume,last_gain;
static voice_history durable;
uint32_t hal_millis(void){return now;}
static void *memory(size_t n){return fail_alloc?NULL:calloc(1,n);}
void prefs_save(void){pref_calls++;}
void voice_audio_levels(int volume,int gain){last_volume=volume;last_gain=gain;levels_calls++;}
int hal_store_save_checked(const char *key,const void *data,int size)
{assert(!strcmp(key,"voice_hist") && size==(int)sizeof durable);save_calls++;if(fail_save)return 0;memcpy(&durable,data,sizeof durable);return 1;}
int hal_store_load(const char *key,void *data,int size)
{assert(!strcmp(key,"voice_hist") && size==(int)sizeof durable);read_calls++;memcpy(data,&durable,sizeof durable);if(bad_read)((voice_history *)data)->version++;return 1;}
/* ACTUAL FUNCTIONS */
int main(void)
{
    s->history_ok=1;voice_history_init(&s->archive);
    s->view.available=s->view.configured=1;nos_prefs.volume=60;
    uint32_t id=voice_history_append(&s->archive,0,"A question","A useful answer");
    voice_history_metadata(&s->archive,id,1,2,1200,3400,.00012,1);
    assert(voice_chat_history_select(id));assert(s->view.chat_id==id && s->history_count==1 && s->view.turns==1);
    assert(s->view.tier==2 && !strcmp(s->history_in[0],"A question") && !strcmp(s->view.reply,"A useful answer"));
    assert(voice_chat_clear() && !s->view.chat_id && s->archive.count==1 && s->view.saved_count==1);
    voice_saved_chat out[VOICE_SAVED_CHATS];assert(voice_chat_history_list(out,VOICE_SAVED_CHATS)==1 && out[0].id==id);
    assert(!voice_chat_history_select(999));
    atomic_store(&s->busy,1);voice_history before=s->archive;
    assert(!voice_chat_history_select(id) && !voice_chat_history_delete(id) && !voice_chat_clear());
    assert(voice_chat_history_list(out,VOICE_SAVED_CHATS)==-1 && !memcmp(&before,&s->archive,sizeof before));
    assert(!voice_chat_volume(-1) && !voice_chat_volume(101) && !levels_calls && !pref_calls);
    assert(voice_chat_volume(27) && nos_prefs.volume==27 && last_volume==27 && atomic_load(&s->output_volume)==27);
    assert(voice_chat_volume(0) && s->archive.last_volume==27 && last_volume==0);
    assert(!voice_chat_audio(1,100) && !voice_chat_audio(2,30));
    s->view.state=VOICE_SPEAKING;assert(voice_chat_audio(0,36) && last_gain==36 && atomic_load(&s->cancel));
    assert(!atomic_load(&s->speak_replies) && !save_calls);voice_chat_maintenance();assert(!save_calls);
    atomic_store(&s->busy,0);fail_alloc=1;voice_chat_maintenance();
    assert(voice_chat_history_pending() && s->history_error && !save_calls);
    fail_alloc=0;fail_save=1;now=5001;voice_chat_maintenance();assert(save_calls==1 && voice_chat_history_pending());
    fail_save=0;voice_chat_maintenance();assert(save_calls==1); /* bounded retry */
    bad_read=1;now=10002;voice_chat_maintenance();assert(save_calls==2 && read_calls==1 && voice_chat_history_pending());
    bad_read=0;now=15003;voice_chat_maintenance();assert(save_calls==3 && !voice_chat_history_pending() && !s->history_error);
    assert(voice_history_valid(&durable) && !memcmp(&durable,&s->archive,sizeof durable));
    voice_chat_maintenance();assert(save_calls==3);
    assert(voice_chat_history_select(id) && voice_chat_history_delete(id));
    assert(!s->view.chat_id && !s->archive.count && voice_chat_history_pending());
    voice_chat_maintenance();assert(!voice_chat_history_pending() && !durable.count);
    atomic_store(&s->history_dirty,1);s->history_ok=0;before=durable;voice_chat_maintenance();
    assert(voice_chat_history_pending() && !memcmp(&durable,&before,sizeof before));
    puts("PASS production voice state: context restore, new/delete, active guards, live levels, verified save, retries and unknown-store protection");
}
