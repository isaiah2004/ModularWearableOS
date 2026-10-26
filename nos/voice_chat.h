#ifndef NOS_VOICE_CHAT_H
#define NOS_VOICE_CHAT_H
#include <stddef.h>
#include <stdint.h>
#include "voice_history.h"
enum { VOICE_LOW, VOICE_MEDIUM, VOICE_HIGH, VOICE_TIERS };
enum { VOICE_IDLE, VOICE_LISTENING, VOICE_TRANSCRIBING, VOICE_THINKING,
       VOICE_SYNTHESIZING, VOICE_SPEAKING, VOICE_DONE, VOICE_ERROR, VOICE_STARTING };
enum { VOICE_PHASE_NONE, VOICE_PHASE_RECORD, VOICE_PHASE_UPLOAD, VOICE_PHASE_WAIT, VOICE_PHASE_PLAY };
typedef struct {
    const char *name,*model;
    unsigned cents_per_minute,output_tokens,reply_bytes,history_bytes;
    double prompt_per_million,completion_per_million;
} voice_profile;
const voice_profile *voice_profile_get(int tier);
double voice_reserve_usd(int tier,size_t prompt_bytes,unsigned input_ms);
int voice_budget_allows(int tier,double spent,double reserve,unsigned speech_ms);
/* UTF-8-safe truncation; JSON/network responses never overrun their destination. */
size_t voice_text_copy(char *out,size_t capacity,const char *text);
typedef struct {
    int available,configured,tier,state,level,http_status,selftest;
    uint32_t generation,turns,input_ms,output_ms,record_ms,peak,nonzero;
    double cost_usd,last_cost_usd;
    int estimated;
    int busy,volume,last_volume,speak,mic_gain,storage_dirty,storage_error,excerpt;
    uint32_t chat_id,saved_count,text_generation,stage_ms,progress_done,progress_total,record_limit_ms;
    int progress_phase;
    char status[80],input[384],reply[1408];
} voice_chat_snapshot;
void voice_chat_init(void);
void voice_chat_get(voice_chat_snapshot *out);
int voice_chat_start(void);
void voice_chat_send(void);
void voice_chat_cancel(void);
int voice_chat_active(void);
int voice_chat_tier(int tier);
int voice_chat_clear(void);
int voice_chat_volume(int volume);              /* 0..100, live and saved */
int voice_chat_audio(int speak,int mic_gain);    /* mic: 24/30/36 dB */
int voice_chat_history_list(voice_saved_chat *out,unsigned capacity);
int voice_chat_history_select(uint32_t id);
int voice_chat_history_delete(uint32_t id);
int voice_chat_history_pending(void);
void voice_chat_maintenance(void);              /* UI owner, after final DMA fence */
/* Returns 1 success, -1 known/rejected command, 0 not a chat command.
 * Credential commands never echo their argument. NVS writes run on the UI's
 * internal stack; the network/audio worker never writes flash. */
int voice_chat_command(const char *command);
#endif
