#ifndef NOS_VOICE_AUDIO_H
#define NOS_VOICE_AUDIO_H
#include <stddef.h>
#include <stdint.h>
/* Targets only; the exclusive codec worker applies them during read/write. */
void voice_audio_levels(int volume,int mic_gain);
int voice_audio_open(void);
void voice_audio_close(void);
int voice_audio_read(int16_t *mono,size_t frames,unsigned *peak,unsigned *nonzero);
int voice_audio_write(const int16_t *mono,size_t frames);
void voice_audio_speaker(int on);
#endif
