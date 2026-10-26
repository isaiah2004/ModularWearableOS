#include "voice_chat.h"
#include <math.h>
#include <string.h>
static const voice_profile profiles[VOICE_TIERS]={
    {"LOW","openai/gpt-4o-mini",1,160,640,1800,.20,.80},
    {"MEDIUM","openai/gpt-5.4-mini",5,256,1000,3600,.90,5.0},
    {"HIGH","openai/gpt-5.4",10,384,1280,5600,3.0,18.0},
};
const voice_profile *voice_profile_get(int tier)
{return tier>=0 && tier<VOICE_TIERS?&profiles[tier]:NULL;}
double voice_reserve_usd(int tier,size_t prompt_bytes,unsigned input_ms)
{
    const voice_profile *p=voice_profile_get(tier);if(!p)return INFINITY;
    /* One token per UTF-8 byte is deliberately conservative. Reserve speech at
     * the more expensive published Whisper/Kokoro fallback providers, plus 10%.
     * Provider price ceilings also accompany each text request. */
    return 1.10*((prompt_bytes+700)*p->prompt_per_million/1e6+
        p->output_tokens*p->completion_per_million/1e6+
        input_ms*.0000112/1000+p->reply_bytes*.000004);
}
int voice_budget_allows(int tier,double spent,double reserve,unsigned speech_ms)
{
    const voice_profile *p=voice_profile_get(tier);
    if(!p || !isfinite(spent) || !isfinite(reserve) || spent<0 || reserve<0)return 0;
    double minutes=(speech_ms<60000?60000:speech_ms)/60000.0;
    return spent+reserve<=p->cents_per_minute*.01*minutes;
}
size_t voice_text_copy(char *out,size_t capacity,const char *text)
{
    if(!out || !capacity)return 0;
    if(!text){*out=0;return 0;}
    size_t n=strlen(text);if(n>=capacity)n=capacity-1;
    while(n && ((unsigned char)text[n]&0xc0)==0x80)n--;
    memcpy(out,text,n);out[n]=0;return n;
}
