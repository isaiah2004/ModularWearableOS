#ifndef NOS_VOICE_HISTORY_H
#define NOS_VOICE_HISTORY_H
#include <stddef.h>
#include <stdint.h>
#define VOICE_SAVED_CHATS 4
#define VOICE_SAVED_TURNS 2
#define VOICE_SAVED_INPUT 128
#define VOICE_SAVED_REPLY 320
typedef struct {
    char input[VOICE_SAVED_INPUT],reply[VOICE_SAVED_REPLY];
    uint8_t excerpt,reserved[3];
} voice_saved_turn;
typedef struct {
    uint32_t id,turns,input_ms,output_ms,cost_micro_usd;
    uint8_t tier,estimated,count,reserved;
    char title[40];
    voice_saved_turn turn[VOICE_SAVED_TURNS];
} voice_saved_chat;
/* Fixed, checksummed text-only checkpoint, bounded below one 4 KiB page.
 * Platform owner serializes edits; a failed verified save stays dirty. */
typedef struct {
    uint32_t magic,version,next_id;
    uint8_t count,speak,mic_gain,last_volume;
    voice_saved_chat chat[VOICE_SAVED_CHATS];
    uint32_t checksum;
} voice_history;
void voice_history_init(voice_history *h);
void voice_history_seal(voice_history *h);
int voice_history_valid(const voice_history *h);
const voice_saved_chat *voice_history_find(const voice_history *h,uint32_t id);
/* New chats reject a full store; existing chats keep their last two turns.
 * Returns the stable chat ID, or zero without mutation on rejection. */
uint32_t voice_history_append(voice_history *h,uint32_t id,const char *input,const char *reply);
int voice_history_delete(voice_history *h,uint32_t id);
void voice_history_metadata(voice_history *h,uint32_t id,unsigned turns,int tier,
                            unsigned input_ms,unsigned output_ms,double cost,int estimated);
#endif
