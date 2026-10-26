/* Offline UI preview. Virtual time drives every phase; no credentials/network. */
#include "voice_chat.h"
#include "prefs.h"
static voice_chat_snapshot chat_sim;
static voice_history chat_saved;
static int chat_initialized,chat_dirty;
static uint32_t chat_stage_started,chat_sim_sequence;
static void chat_phase(int state,const char *status)
{
    chat_sim.state=state;voice_text_copy(chat_sim.status,sizeof chat_sim.status,status);
    chat_sim.generation++;chat_stage_started=hal_millis();chat_sim.progress_done=chat_sim.progress_total=0;
    chat_sim.progress_phase=state==VOICE_LISTENING?VOICE_PHASE_RECORD:state==VOICE_SPEAKING?VOICE_PHASE_PLAY:
        state>=VOICE_TRANSCRIBING && state<=VOICE_SYNTHESIZING?VOICE_PHASE_WAIT:VOICE_PHASE_NONE;
}
void voice_chat_init(void)
{
    if(chat_initialized)return;
    chat_initialized=1;voice_history_init(&chat_saved);
    voice_history loaded;
    if(hal_store_load("voice_hist",&loaded,sizeof loaded) && voice_history_valid(&loaded))chat_saved=loaded;
    memset(&chat_sim,0,sizeof chat_sim);chat_sim.available=chat_sim.configured=1;chat_sim.saved_count=chat_saved.count;
    chat_phase(VOICE_IDLE,"Tap TALK to start");
}
int voice_chat_active(void){return chat_sim.busy;}
int voice_chat_history_pending(void){return chat_dirty;}
void voice_chat_maintenance(void)
{
    if(!chat_initialized || chat_sim.busy || !chat_dirty)return;
    voice_history verify;voice_history_seal(&chat_saved);
    int ok=hal_store_save_checked("voice_hist",&chat_saved,sizeof chat_saved) &&
        hal_store_load("voice_hist",&verify,sizeof verify) && voice_history_valid(&verify) &&
        !memcmp(&verify,&chat_saved,sizeof verify);
    chat_sim.storage_error=!ok;if(ok)chat_dirty=0;
}
static void chat_remember(void)
{
    chat_sim.chat_id=voice_history_append(&chat_saved,chat_sim.chat_id,chat_sim.input,chat_sim.reply);
    chat_sim.turns++;chat_sim.saved_count=chat_saved.count;chat_sim.cost_usd+=.00006;
    voice_history_metadata(&chat_saved,chat_sim.chat_id,chat_sim.turns,chat_sim.tier,
        chat_sim.input_ms,chat_sim.output_ms,chat_sim.cost_usd,1);chat_dirty=1;
}
static void chat_advance(void)
{
    uint32_t elapsed=hal_millis()-chat_stage_started;chat_sim.stage_ms=elapsed;
    if(chat_sim.state==VOICE_LISTENING) {
        chat_sim.record_ms=elapsed;chat_sim.level=20+(int)(35*(1+sinf(elapsed*.007f)));
        chat_sim.progress_done=elapsed;chat_sim.progress_total=20000;
        if(elapsed>=20000)voice_chat_send();
    }else if(chat_sim.state==VOICE_TRANSCRIBING) {
        chat_sim.progress_phase=elapsed<450?VOICE_PHASE_UPLOAD:VOICE_PHASE_WAIT;
        chat_sim.progress_total=elapsed<450?60000:0;chat_sim.progress_done=elapsed<450?elapsed*60000/450:0;
        if(elapsed>=650) {
            snprintf(chat_sim.input,sizeof chat_sim.input,"How can my watch help today? Question %u.",(unsigned)++chat_sim_sequence);
            chat_sim.text_generation++;chat_phase(VOICE_THINKING,"Thinking");
        }
    }else if(chat_sim.state==VOICE_THINKING && elapsed>=850) {
        strcpy(chat_sim.reply,"Keep a conversation in Recents and pick it up later. Open Audio to change the speaker volume or microphone sensitivity. Saved chats contain text only, so they use very little space.");
        chat_sim.text_generation++;chat_remember();
        if(chat_saved.speak && nos_prefs.volume)chat_phase(VOICE_SYNTHESIZING,"Preparing speech");
        else {chat_sim.busy=0;chat_phase(VOICE_DONE,"Reply ready; TALK to continue");}
    }else if(chat_sim.state==VOICE_SYNTHESIZING && elapsed>=600)chat_phase(VOICE_SPEAKING,"Speaking");
    else if(chat_sim.state==VOICE_SPEAKING) {
        chat_sim.progress_done=elapsed>1800?1800:elapsed;chat_sim.progress_total=1800;
        if(elapsed>=1800) {
            chat_sim.output_ms+=1800;chat_sim.busy=0;
            voice_history_metadata(&chat_saved,chat_sim.chat_id,chat_sim.turns,chat_sim.tier,
                chat_sim.input_ms,chat_sim.output_ms,chat_sim.cost_usd,1);
            chat_phase(VOICE_DONE,"Reply ready; TALK to continue");
        }
    }
}
void voice_chat_get(voice_chat_snapshot *out)
{
    voice_chat_init();chat_advance();voice_chat_maintenance();
    chat_sim.volume=nos_prefs.volume;chat_sim.last_volume=chat_saved.last_volume;
    chat_sim.speak=chat_saved.speak;chat_sim.mic_gain=chat_saved.mic_gain;
    chat_sim.storage_dirty=chat_dirty;if(out)*out=chat_sim;
}
int voice_chat_tier(int tier)
{voice_chat_init();if(!voice_profile_get(tier) || chat_sim.busy)return 0;chat_sim.tier=tier;chat_sim.generation++;return 1;}
int voice_chat_start(void)
{
    voice_chat_init();if(chat_sim.busy)return 0;
    if(!chat_sim.chat_id && chat_saved.count==VOICE_SAVED_CHATS){chat_phase(VOICE_ERROR,"Saved chats full; delete one in RECENTS");return 0;}
    chat_sim.input[0]=chat_sim.reply[0]=0;chat_sim.excerpt=0;chat_sim.text_generation++;
    chat_sim.record_ms=0;chat_sim.record_limit_ms=20000;chat_sim.busy=1;chat_phase(VOICE_LISTENING,"Listening; tap SEND when finished");return 1;
}
void voice_chat_send(void)
{if(chat_sim.state==VOICE_LISTENING){chat_sim.input_ms+=chat_sim.record_ms;chat_sim.level=0;chat_phase(VOICE_TRANSCRIBING,"Sending your voice");}}
void voice_chat_cancel(void)
{if(chat_sim.busy){chat_sim.busy=0;chat_sim.level=0;chat_phase(VOICE_IDLE,"Cancelled");}}
int voice_chat_clear(void)
{
    voice_chat_init();if(chat_sim.busy)return 0;
    int tier=chat_sim.tier;uint32_t text_generation=chat_sim.text_generation+1,generation=chat_sim.generation+1;
    memset(&chat_sim,0,sizeof chat_sim);chat_sim.available=chat_sim.configured=1;
    chat_sim.tier=tier;chat_sim.saved_count=chat_saved.count;chat_sim.text_generation=text_generation;chat_sim.generation=generation;
    chat_phase(VOICE_IDLE,"New conversation");return 1;
}
int voice_chat_volume(int volume)
{
    voice_chat_init();if(volume<0 || volume>100)return 0;
    nos_prefs.volume=(uint8_t)volume;prefs_save();if(volume){chat_saved.last_volume=(uint8_t)volume;chat_dirty=1;}return 1;
}
int voice_chat_audio(int speak,int gain)
{
    voice_chat_init();if((speak!=0 && speak!=1) || (gain!=24 && gain!=30 && gain!=36))return 0;
    chat_saved.speak=(uint8_t)speak;chat_saved.mic_gain=(uint8_t)gain;chat_dirty=1;
    if(!speak && chat_sim.state==VOICE_SPEAKING)voice_chat_cancel();
    return 1;
}
int voice_chat_history_list(voice_saved_chat *out,unsigned capacity)
{
    voice_chat_init();if(chat_sim.busy || !out)return -1;
    unsigned count=chat_saved.count;if(count>capacity)count=capacity;memcpy(out,chat_saved.chat,count*sizeof *out);return (int)count;
}
int voice_chat_history_select(uint32_t id)
{
    voice_chat_init();if(chat_sim.busy)return 0;
    const voice_saved_chat *c=voice_history_find(&chat_saved,id);if(!c)return 0;
    voice_chat_clear();chat_sim.chat_id=id;chat_sim.tier=c->tier;chat_sim.turns=c->turns;
    chat_sim.cost_usd=c->cost_micro_usd/1e6;chat_sim.estimated=c->estimated;
    chat_sim.input_ms=c->input_ms;chat_sim.output_ms=c->output_ms;
    strcpy(chat_sim.input,c->turn[c->count-1].input);strcpy(chat_sim.reply,c->turn[c->count-1].reply);
    chat_sim.excerpt=c->turn[c->count-1].excerpt;chat_sim.text_generation++;chat_phase(VOICE_DONE,"Saved chat; TALK to continue");return 1;
}
int voice_chat_history_delete(uint32_t id)
{
    voice_chat_init();if(chat_sim.busy || !voice_history_delete(&chat_saved,id))return 0;
    if(chat_sim.chat_id==id)voice_chat_clear();
    chat_sim.saved_count=chat_saved.count;chat_dirty=1;return 1;
}
int voice_chat_command(const char *command){(void)command;return 0;}
