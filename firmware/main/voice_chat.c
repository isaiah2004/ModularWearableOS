/* Direct watch -> OpenRouter voice chat. HTTPS and codec I/O stay on one
 * cancellable worker. Audio stays in RAM; compact text history is checked NVS.
 * Configuration is provisioned over USB, never compiled into the firmware. */
#include "voice_chat.h"
#include "voice_audio.h"
#include "sdkconfig.h"
#if CONFIG_WATCH_BOARD_143C
#include "hal.h"
#include "prefs.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <ctype.h>

#define FS 16000u
#define MAX_MS 20000u
#define MAX_SAMPLES (FS*MAX_MS/1000)
#define JSON_MAX 16384u
#define HISTORY 4
typedef struct {uint32_t version;char key[128],stt[64],tts[64],voice[32];} voice_config;
typedef struct {
    voice_chat_snapshot view;
    voice_config config,session;
    char history_in[HISTORY][384],history_out[HISTORY][1408];
    unsigned history_count;
    char request_text[384];
    int request,tier;
    atomic_int busy,send,cancel;
    TaskHandle_t worker;
    int64_t deadline;
    uint8_t *loop_wav;
    unsigned loop_samples,loop_peak;
    voice_history archive;
    atomic_int history_dirty,speak_replies,output_volume;
    int history_ok,history_error;
    uint32_t history_retry,stage_started;
} voice_state;
static voice_state *s;
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static void *memory(size_t bytes){return heap_caps_calloc(1,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
static int active(void){return s && atomic_load(&s->busy);}
int voice_chat_active(void){return active();}
static int cancelled(void){return !s || atomic_load(&s->cancel) || esp_timer_get_time()>s->deadline;}
static void status(int state,const char *text)
{
    portENTER_CRITICAL(&lock);s->view.state=state;voice_text_copy(s->view.status,sizeof s->view.status,text);
    s->stage_started=hal_millis();s->view.progress_done=s->view.progress_total=0;
    s->view.progress_phase=state==VOICE_LISTENING?VOICE_PHASE_RECORD:state==VOICE_SPEAKING?VOICE_PHASE_PLAY:
        (state>=VOICE_TRANSCRIBING && state<=VOICE_SYNTHESIZING) || state==VOICE_STARTING?VOICE_PHASE_WAIT:VOICE_PHASE_NONE;
    s->view.generation++;portEXIT_CRITICAL(&lock);
}
static void progress(int phase,uint32_t done,uint32_t total)
{
    portENTER_CRITICAL(&lock);s->view.progress_phase=phase;s->view.progress_done=done;
    s->view.progress_total=total;portEXIT_CRITICAL(&lock);
}
static int fail(const char *text){if(!atomic_load(&s->cancel))status(VOICE_ERROR,text);return 0;}
static void cost(double value,int estimate)
{
    if(!isfinite(value) || value<0)return;
    portENTER_CRITICAL(&lock);s->view.cost_usd+=value;s->view.last_cost_usd+=value;
    s->view.estimated|=estimate;s->view.generation++;portEXIT_CRITICAL(&lock);
}
static int token(const char *p,size_t capacity)
{
    if(!p || !p[0] || !memchr(p,0,capacity))return 0;
    for(;*p;p++)if(!isalnum((unsigned char)*p) && !strchr("_-/.:+",*p))return 0;
    return 1;
}
static int valid_config(const voice_config *c)
{
    return c->version==1 && memchr(c->key,0,sizeof c->key) &&
        (!c->key[0] || token(c->key,sizeof c->key)) && token(c->stt,sizeof c->stt) &&
        token(c->tts,sizeof c->tts) && token(c->voice,sizeof c->voice) &&
        !strcmp(c->stt,"openai/whisper-large-v3-turbo") && !strcmp(c->tts,"hexgrad/kokoro-82m");
}
void voice_chat_init(void)
{
    if(s)return;
    s=memory(sizeof *s);if(!s)return;
    atomic_init(&s->busy,0);atomic_init(&s->send,0);atomic_init(&s->cancel,0);atomic_init(&s->history_dirty,0);
    voice_history_init(&s->archive);s->history_ok=1;
    /* Distinguish a missing key from an unreadable/incompatible checkpoint.
     * Never replace an unknown saved history with an empty default. */
    nvs_handle_t history_handle;esp_err_t history_result=nvs_open("wos",NVS_READONLY,&history_handle);
    if(history_result==ESP_OK) {
        size_t bytes=sizeof s->archive;
        history_result=nvs_get_blob(history_handle,"voice_hist",&s->archive,&bytes);nvs_close(history_handle);
        if(history_result==ESP_ERR_NVS_NOT_FOUND)voice_history_init(&s->archive);
        else if(history_result!=ESP_OK || bytes!=sizeof s->archive || !voice_history_valid(&s->archive))s->history_ok=0;
    }else if(history_result!=ESP_ERR_NVS_NOT_FOUND)s->history_ok=0;
    if(!s->history_ok){voice_history_init(&s->archive);s->history_error=2;}
    atomic_init(&s->speak_replies,s->archive.speak);atomic_init(&s->output_volume,nos_prefs.volume);
    s->view.saved_count=s->archive.count;
    voice_audio_levels(nos_prefs.volume,s->archive.mic_gain);
    s->config=(voice_config){.version=1,.stt="openai/whisper-large-v3-turbo",.tts="hexgrad/kokoro-82m",.voice="af_heart"};
    voice_config *loaded=memory(sizeof *loaded);
    if(loaded){if(hal_store_load("voice_chat",loaded,sizeof *loaded) && valid_config(loaded))s->config=*loaded;
        memset(loaded,0,sizeof *loaded);free(loaded);}
#if CONFIG_WATCH_BOARD_143C
    s->view.available=1;
#endif
    s->view.configured=!!s->config.key[0];
    status(VOICE_IDLE,s->view.configured?"Tap TALK to start":"Add your OpenRouter key over USB");
}
void voice_chat_get(voice_chat_snapshot *out)
{
    if(!out)return;
    memset(out,0,sizeof *out);
    if(!s){voice_text_copy(out->status,sizeof out->status,"Voice service unavailable");return;}
    portENTER_CRITICAL(&lock);*out=s->view;out->stage_ms=hal_millis()-s->stage_started;portEXIT_CRITICAL(&lock);
    out->busy=active();out->volume=nos_prefs.volume;out->last_volume=s->archive.last_volume;
    out->speak=s->archive.speak;out->mic_gain=s->archive.mic_gain;
    out->storage_dirty=atomic_load(&s->history_dirty);out->storage_error=s->history_error;
}
int voice_chat_tier(int tier)
{
    if(!s || !voice_profile_get(tier) || active())return 0;
    portENTER_CRITICAL(&lock);s->view.tier=tier;s->view.generation++;portEXIT_CRITICAL(&lock);return 1;
}
void voice_chat_send(void){if(s)atomic_store(&s->send,1);}
void voice_chat_cancel(void){if(s && active())atomic_store(&s->cancel,1);}
int voice_chat_clear(void)
{
    if(!s || active())return 0;
    s->history_count=0;memset(s->history_in,0,sizeof s->history_in);memset(s->history_out,0,sizeof s->history_out);
    portENTER_CRITICAL(&lock);int available=s->view.available,configured=s->view.configured,tier=s->view.tier;
    uint32_t generation=s->view.generation+1,text_generation=s->view.text_generation+1;
    memset(&s->view,0,sizeof s->view);s->view.available=available;s->view.configured=configured;s->view.tier=tier;
    s->view.saved_count=s->archive.count;s->view.generation=generation;s->view.text_generation=text_generation;
    portEXIT_CRITICAL(&lock);status(VOICE_IDLE,"New conversation");return 1;
}
int voice_chat_volume(int volume)
{
    if(!s || volume<0 || volume>100)return 0;
    atomic_store(&s->output_volume,volume);
    if(nos_prefs.volume!=volume){nos_prefs.volume=(uint8_t)volume;prefs_save();}
    if(volume && s->archive.last_volume!=volume) {
        s->archive.last_volume=(uint8_t)volume;if(s->history_ok)atomic_store(&s->history_dirty,1);
    }
    voice_audio_levels(volume,s->archive.mic_gain);return 1;
}
int voice_chat_audio(int speak,int mic_gain)
{
    if(!s || (speak!=0 && speak!=1) || (mic_gain!=24 && mic_gain!=30 && mic_gain!=36))return 0;
    if(s->archive.speak!=speak || s->archive.mic_gain!=mic_gain) {
        s->archive.speak=(uint8_t)speak;s->archive.mic_gain=(uint8_t)mic_gain;
        if(s->history_ok)atomic_store(&s->history_dirty,1);
    }
    atomic_store(&s->speak_replies,speak);
    portENTER_CRITICAL(&lock);int speaking=s->view.state==VOICE_SPEAKING;portEXIT_CRITICAL(&lock);
    if(!speak && speaking)voice_chat_cancel();
    voice_audio_levels(nos_prefs.volume,mic_gain);return 1;
}
int voice_chat_history_list(voice_saved_chat *out,unsigned capacity)
{
    if(!s || !s->history_ok || active() || !out)return -1;
    unsigned count=s->archive.count;if(count>capacity)count=capacity;
    memcpy(out,s->archive.chat,count*sizeof *out);return (int)count;
}
int voice_chat_history_select(uint32_t id)
{
    if(!s || active() || !s->history_ok)return 0;
    const voice_saved_chat *c=voice_history_find(&s->archive,id);if(!c)return 0;
    voice_chat_clear();s->history_count=c->count;
    for(unsigned i=0;i<c->count;i++) {
        voice_text_copy(s->history_in[i],sizeof s->history_in[i],c->turn[i].input);
        voice_text_copy(s->history_out[i],sizeof s->history_out[i],c->turn[i].reply);
    }
    portENTER_CRITICAL(&lock);s->view.chat_id=id;s->view.turns=c->turns;s->view.tier=c->tier;
    s->view.input_ms=c->input_ms;s->view.output_ms=c->output_ms;s->view.cost_usd=c->cost_micro_usd/1e6;
    s->view.estimated=c->estimated;
    voice_text_copy(s->view.input,sizeof s->view.input,c->turn[c->count-1].input);
    voice_text_copy(s->view.reply,sizeof s->view.reply,c->turn[c->count-1].reply);
    s->view.excerpt=c->turn[c->count-1].excerpt;s->view.text_generation++;portEXIT_CRITICAL(&lock);
    status(VOICE_DONE,s->view.excerpt?"Saved excerpt; TALK to continue":"Saved chat; TALK to continue");return 1;
}
int voice_chat_history_delete(uint32_t id)
{
    if(!s || active() || !s->history_ok || !voice_history_delete(&s->archive,id))return 0;
    if(s->view.chat_id==id)voice_chat_clear();
    portENTER_CRITICAL(&lock);s->view.saved_count=s->archive.count;s->view.generation++;portEXIT_CRITICAL(&lock);
    atomic_store(&s->history_dirty,1);return 1;
}
int voice_chat_history_pending(void){return s && atomic_load(&s->history_dirty);}
void voice_chat_maintenance(void)
{
    if(!s || active() || !s->history_ok || !atomic_load(&s->history_dirty))return;
    uint32_t now=hal_millis();if(s->history_retry && (int32_t)(now-s->history_retry)<0)return;
    voice_history *verified=memory(sizeof *verified);int ok=0;
    voice_history_seal(&s->archive);
    if(verified)ok=hal_store_save_checked("voice_hist",&s->archive,sizeof s->archive) &&
        hal_store_load("voice_hist",verified,sizeof *verified) && voice_history_valid(verified) &&
        !memcmp(verified,&s->archive,sizeof *verified);
    free(verified);s->history_error=!ok;
    if(ok){atomic_store(&s->history_dirty,0);s->history_retry=0;}
    else s->history_retry=now+5000;
}
typedef struct {char type[100];} response_meta;
static esp_err_t headers(esp_http_client_event_t *e)
{
    response_meta *m=e->user_data;
    if(m && e->event_id==HTTP_EVENT_ON_HEADER && e->header_key && e->header_value && !strcasecmp(e->header_key,"Content-Type"))
        snprintf(m->type,sizeof m->type,"%s",e->header_value);
    return ESP_OK;
}
static esp_http_client_handle_t post(const char *path,const char *body,response_meta *meta)
{
    const char *phase="connect";
    char url[128],auth[160];snprintf(url,sizeof url,"https://openrouter.ai/api/v1/%s",path);
    esp_http_client_config_t cfg={.url=url,.timeout_ms=10000,.crt_bundle_attach=esp_crt_bundle_attach,
        .buffer_size=2048,.buffer_size_tx=1024,.disable_auto_redirect=true,.event_handler=headers,.user_data=meta};
    esp_http_client_handle_t client=esp_http_client_init(&cfg);if(!client){fail("Not enough memory for HTTPS");return NULL;}
    snprintf(auth,sizeof auth,"Bearer %s",s->session.key);
    esp_http_client_set_method(client,HTTP_METHOD_POST);
    esp_http_client_set_header(client,"Authorization",auth);memset(auth,0,sizeof auth);
    esp_http_client_set_header(client,"Content-Type","application/json");
    esp_http_client_set_header(client,"X-Title","NOS Watch");
    s->deadline=esp_timer_get_time()+60000000;
    size_t length=strlen(body),written=0;
    progress(VOICE_PHASE_UPLOAD,0,(uint32_t)length);
    if(esp_http_client_open(client,(int)length)!=ESP_OK)goto error;
    phase="upload";
    while(written<length && !cancelled()){
        size_t n=length-written;if(n>1024)n=1024;
        int sent=esp_http_client_write(client,body+written,(int)n);if(sent<=0)goto error;written+=(unsigned)sent;
        progress(VOICE_PHASE_UPLOAD,(uint32_t)written,(uint32_t)length);
    }
    if(cancelled() || written!=length)goto error;
    phase="response";
    progress(VOICE_PHASE_WAIT,0,0);
    esp_http_client_set_timeout_ms(client,5000);
    int64_t header_result;
    do{
        header_result=esp_http_client_fetch_headers(client);
        /* Continue reading the same response. Never resend a billable request
         * when an upstream speech/model provider needs more time. */
    }while(header_result==-ESP_ERR_HTTP_EAGAIN && !cancelled());
    if(header_result<0 || cancelled())goto error;
    esp_http_client_set_timeout_ms(client,10000);
    int code=esp_http_client_get_status_code(client);
    portENTER_CRITICAL(&lock);s->view.http_status=code;portEXIT_CRITICAL(&lock);
    if(code!=200){
        fail(code==401?"OpenRouter key rejected":code==402?"OpenRouter credits exhausted":
             code==429?"OpenRouter rate limit; try later":"OpenRouter request failed; see HTTP status");
        esp_http_client_cleanup(client);return NULL;
    }
    return client;
error:
    {
        int error=esp_http_client_get_errno(client);char message[80];
        snprintf(message,sizeof message,"Network %s failed (%d)",phase,error);
        printf("NOSCHATNET stage=%s sent=%u expected=%u error=%d\n",phase,(unsigned)written,(unsigned)length,error);
        esp_http_client_cleanup(client);fail(message);return NULL;
    }
}
static cJSON *json_post(const char *path,const char *body)
{
    response_meta m={0};esp_http_client_handle_t client=post(path,body,&m);if(!client)return NULL;
    char *buffer=memory(JSON_MAX+1);size_t used=0;int ok=buffer!=NULL;
    while(ok && !cancelled()){
        int n=esp_http_client_read(client,buffer+used,(int)(JSON_MAX-used));
        if(n<0){ok=0;break;}if(!n)break;used+=(unsigned)n;
        if(used==JSON_MAX){ok=0;break;}
    }
    ok=ok && !cancelled() && esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    cJSON *result=ok?cJSON_ParseWithLength(buffer,used):NULL;free(buffer);
    if(!result)fail("Incomplete or oversized API response");
    return result;
}
static void billed(cJSON *root,double estimate)
{
    cJSON *v=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root,"usage"),"cost");
    if(cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble>=0)cost(v->valuedouble,0);
    else cost(estimate,1);
}
static void le16(uint8_t *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void le32(uint8_t *p,uint32_t v){le16(p,v);le16(p+2,v>>16);}
static void wav_header(uint8_t *p,unsigned samples)
{
    memcpy(p,"RIFF",4);le32(p+4,36+samples*2);memcpy(p+8,"WAVEfmt ",8);le32(p+16,16);
    le16(p+20,1);le16(p+22,1);le32(p+24,FS);le32(p+28,FS*2);le16(p+32,2);le16(p+34,16);
    memcpy(p+36,"data",4);le32(p+40,samples*2);
}
static int loop_capture(unsigned frames)
{
    if(!s->loop_wav)return 1;
    int16_t mono[128];
    while(frames){unsigned n=frames>128?128:frames;
        unsigned peak=0;if(!voice_audio_read(mono,n,&peak,NULL))return 0;
        if(peak>s->loop_peak)s->loop_peak=peak;
        for(unsigned i=0;i<n && s->loop_samples<FS*8;i++,s->loop_samples++)
            le16(s->loop_wav+44+2*s->loop_samples,(unsigned)(uint16_t)mono[i]);
        frames-=n;
    }
    return 1;
}
static unsigned capture(uint8_t *wav,unsigned limit_ms,int local)
{
    status(VOICE_LISTENING,local?"Microphone test; speak now":"Listening; tap SEND when finished");
    portENTER_CRITICAL(&lock);s->view.record_limit_ms=limit_ms;portEXIT_CRITICAL(&lock);
    unsigned count=0,peak=0,nonzero=0;int16_t block[128];float dc=0;
    /* Discard codec/mic start-up transient before measuring speech. */
    for(int i=0;i<25 && !cancelled();i++)if(!voice_audio_read(block,128,NULL,NULL))return fail("Microphone read failed");
    while(count<FS*limit_ms/1000 && !cancelled() && !atomic_load(&s->send)){
        unsigned p=0,z=0;if(!voice_audio_read(block,128,&p,&z))return fail("Microphone read failed");
        p=z=0;
        for(int i=0;i<128;i++){
            dc+=.002f*(block[i]-dc);int v=(int)(block[i]-dc);if(v>32767)v=32767;if(v< -32768)v=-32768;
            le16(wav+44+2*(count+i),(unsigned)(uint16_t)(int16_t)v);unsigned a=v<0?-v:v;if(a>p)p=a;if(a>20)z++;
        }
        count+=128;nonzero+=z;if(p>peak)peak=p;
        progress(VOICE_PHASE_RECORD,count*1000/FS,limit_ms);
        portENTER_CRITICAL(&lock);s->view.record_ms=count*1000/FS;s->view.level=p>8000?100:(int)(p/80);
        s->view.peak=peak;s->view.nonzero=nonzero;portEXIT_CRITICAL(&lock);
    }
    if(cancelled())return 0;
    if(count<FS/4)return fail("Recording too short; speak for a moment");
    if(!local && (peak<180 || nonzero<count/200))return fail("No speech detected; try closer to the mic");
    wav_header(wav,count);
    portENTER_CRITICAL(&lock);s->view.input_ms+=count*1000/FS;portEXIT_CRITICAL(&lock);
    return count;
}
static int transcribe(uint8_t *wav,unsigned samples)
{
    status(VOICE_TRANSCRIBING,"Transcribing with Whisper");
    size_t bytes=44+samples*2,encoded=4*((bytes+2)/3),used=0;
    char *body=memory(encoded+256);if(!body)return fail("Not enough memory for recording upload");
    int prefix=snprintf(body,256,"{\"model\":\"%s\",\"input_audio\":{\"data\":\"",s->session.stt);
    int ok=mbedtls_base64_encode((unsigned char *)body+prefix,encoded+1,&used,wav,bytes)==0;
    if(ok)strcpy(body+prefix+used,"\",\"format\":\"wav\"}}");
    cJSON *root=ok?json_post("audio/transcriptions",body):NULL;free(body);if(!root)return 0;
    billed(root,(samples/(double)FS)*.0000112);
    cJSON *text=cJSON_GetObjectItemCaseSensitive(root,"text");ok=cJSON_IsString(text) && text->valuestring[0];
    if(ok){portENTER_CRITICAL(&lock);voice_text_copy(s->view.input,sizeof s->view.input,text->valuestring);s->view.text_generation++;s->view.generation++;portEXIT_CRITICAL(&lock);}
    cJSON_Delete(root);return ok?1:fail("Whisper returned no speech");
}
static void message(cJSON *array,const char *role,const char *text)
{cJSON *m=cJSON_CreateObject();cJSON_AddStringToObject(m,"role",role);cJSON_AddStringToObject(m,"content",text);cJSON_AddItemToArray(array,m);}
static int think(void)
{
    status(VOICE_THINKING,"Thinking");const voice_profile *p=voice_profile_get(s->tier);
    cJSON *root=cJSON_CreateObject(),*messages=cJSON_CreateArray();if(!root || !messages){cJSON_Delete(root);cJSON_Delete(messages);return fail("Not enough memory for chat");}
    cJSON_AddStringToObject(root,"model",p->model);cJSON_AddNumberToObject(root,"max_tokens",p->output_tokens);
    cJSON_AddBoolToObject(root,"stream",0);cJSON_AddItemToObject(root,"messages",messages);
    message(messages,"system","You are NOS, a friendly smartwatch voice assistant. Reply in plain spoken language, without markdown. Use one to three short sentences unless asked for detail. Reply in the user's language. Do not claim you can operate this watch or read its sensors. Be candid when unsure.");
    unsigned first=s->history_count,chars=0;
    while(first){unsigned bytes=strlen(s->history_in[first-1])+strlen(s->history_out[first-1]);if(chars+bytes>p->history_bytes)break;chars+=bytes;first--;}
    for(unsigned i=first;i<s->history_count;i++){message(messages,"user",s->history_in[i]);message(messages,"assistant",s->history_out[i]);}
    message(messages,"user",s->view.input);
    cJSON *reason=cJSON_AddObjectToObject(root,"reasoning");cJSON_AddStringToObject(reason,"effort","none");
    cJSON *provider=cJSON_AddObjectToObject(root,"provider"),*prices=cJSON_AddObjectToObject(provider,"max_price");
    cJSON_AddNumberToObject(prices,"prompt",p->prompt_per_million);cJSON_AddNumberToObject(prices,"completion",p->completion_per_million);
    char *body=cJSON_PrintUnformatted(root);cJSON_Delete(root);if(!body)return fail("Not enough memory for chat payload");
    root=json_post("chat/completions",body);free(body);if(!root)return 0;
    billed(root,voice_reserve_usd(s->tier,chars+strlen(s->view.input),0));
    cJSON *choice=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root,"choices"),0);
    cJSON *text=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(choice,"message"),"content");
    int ok=cJSON_IsString(text) && text->valuestring[0] && !cancelled();
    if(ok){portENTER_CRITICAL(&lock);voice_text_copy(s->view.reply,p->reply_bytes+1,text->valuestring);s->view.text_generation++;s->view.generation++;portEXIT_CRITICAL(&lock);}
    cJSON_Delete(root);return ok?1:fail("Model returned no answer");
}
static int speak(void)
{
    status(VOICE_SYNTHESIZING,"Preparing speech");
    cJSON *root=cJSON_CreateObject();if(!root)return fail("Not enough memory for speech");
    cJSON_AddStringToObject(root,"model",s->session.tts);cJSON_AddStringToObject(root,"input",s->view.reply);
    cJSON_AddStringToObject(root,"voice",s->session.voice);cJSON_AddStringToObject(root,"response_format","pcm");
    char *body=cJSON_PrintUnformatted(root);cJSON_Delete(root);if(!body)return fail("Not enough memory for speech payload");
    response_meta m={0};esp_http_client_handle_t client=post("audio/speech",body,&m);free(body);if(!client)return 0;
    /* Speech billing has no JSON usage object. Keep a conservative upper
     * estimate (the more expensive published Kokoro provider) and label it. */
    cost(strlen(s->view.reply)*.000004,1);
    const char *rate_text=strstr(m.type,"rate=");unsigned rate=rate_text?(unsigned)strtoul(rate_text+5,NULL,10):0;
    int ok=!strncmp(m.type,"audio/pcm",9) && rate>=8000 && rate<=48000 && !strstr(m.type,"channels=2");
    if(!ok){esp_http_client_cleanup(client);return fail("Unsupported speech audio format");}
    uint8_t raw[1024];int16_t output[256],previous=0;unsigned ready=0,out_count=0;
    unsigned output_base=s->view.output_ms;
    uint64_t index=0,next=0;int pending=-1;
    status(VOICE_SPEAKING,"Speaking");voice_audio_speaker(1);
    int64_t content_bytes=esp_http_client_get_content_length(client);
    uint32_t play_total=content_bytes>0 && content_bytes<=UINT32_MAX?(uint32_t)content_bytes:0;
    progress(VOICE_PHASE_PLAY,0,play_total);
    while(ok && !cancelled()){
        int bytes=esp_http_client_read(client,(char *)raw,sizeof raw);if(bytes<0){ok=0;break;}if(!bytes)break;
        for(int i=0;i<bytes;i++){
            if(pending<0){pending=raw[i];continue;}
            int16_t current=(int16_t)(pending|raw[i]<<8);pending=-1;
            while(next<=index*FS){
                int value=current;
                if(index){uint64_t fraction=next-(index-1)*FS;value=previous+(int)(((int64_t)current-previous)*(int64_t)fraction/FS);}
                output[ready++]=(int16_t)value;out_count++;next+=rate;
                if(ready==256){if(!voice_audio_write(output,ready) || !loop_capture(ready)){ok=0;break;}ready=0;}
            }
            previous=current;index++;if(!ok || out_count>=FS*60){ok=0;break;}
        }
        progress(VOICE_PHASE_PLAY,index>UINT32_MAX/2?UINT32_MAX:(uint32_t)(index*2),play_total);
        portENTER_CRITICAL(&lock);s->view.output_ms=output_base+out_count*1000/FS;s->view.generation++;portEXIT_CRITICAL(&lock);
    }
    if(ok && ready)ok=voice_audio_write(output,ready) && loop_capture(ready);
    ok=ok && pending<0 && index>0 && !cancelled() && esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);vTaskDelay(pdMS_TO_TICKS(80));voice_audio_speaker(0);
    return ok?1:fail("Speech playback interrupted or incomplete");
}
static int allowance(unsigned input_ms)
{
    unsigned chars=strlen(s->view.input);for(unsigned i=0;i<s->history_count;i++)chars+=strlen(s->history_in[i])+strlen(s->history_out[i]);
    unsigned max=voice_profile_get(s->tier)->history_bytes+384;if(chars>max)chars=max;
    return voice_budget_allows(s->tier,s->view.cost_usd,voice_reserve_usd(s->tier,chars,input_ms),
                               s->view.input_ms+s->view.output_ms);
}
static void remember(void)
{
    unsigned n=s->history_count;if(n==HISTORY){memmove(s->history_in,s->history_in+1,sizeof s->history_in[0]*(HISTORY-1));
        memmove(s->history_out,s->history_out+1,sizeof s->history_out[0]*(HISTORY-1));n--;}
    voice_text_copy(s->history_in[n],sizeof s->history_in[n],s->view.input);
    voice_text_copy(s->history_out[n],sizeof s->history_out[n],s->view.reply);s->history_count=n+1;
    uint32_t id=s->view.chat_id;
    if(s->history_ok)id=voice_history_append(&s->archive,id,s->view.input,s->view.reply);
    portENTER_CRITICAL(&lock);s->view.turns++;if(id)s->view.chat_id=id;
    s->view.saved_count=s->archive.count;s->view.generation++;portEXIT_CRITICAL(&lock);
    if(id && s->history_ok)atomic_store(&s->history_dirty,1);
}
static int local_test(uint8_t *wav)
{
    if(!capture(wav,3000,1))return 0;
    status(VOICE_SPEAKING,"Playing microphone test");voice_audio_speaker(1);
    unsigned samples=s->view.record_ms*FS/1000;
    for(unsigned i=0;i<samples && !cancelled();i+=128){unsigned n=samples-i;if(n>128)n=128;
        if(!voice_audio_write((const int16_t *)(wav+44)+i,n))return fail("Speaker write failed");}
    voice_audio_speaker(0);return !cancelled();
}
static void worker(void *arg)
{
    (void)arg;
    for(;;){ulTaskNotifyTake(pdTRUE,portMAX_DELAY);if(!active())continue;
        int ok=0;uint8_t *wav=NULL;s->deadline=esp_timer_get_time()+120000000;
        if(!voice_audio_open()){fail("C-board microphone or speaker unavailable");goto done;}
        if(s->request==3){wav=memory(44+FS*3*2);if(!wav){fail("Not enough recording memory");goto done;}
            ok=local_test(wav);if(ok)status(VOICE_DONE,"Microphone test played through speaker");goto done;}
        if(s->request==4){
            /* A real acoustic loop: generated speech leaves the ES8311 speaker
             * and is sampled through the ES7210 microphone, then Whisper must
             * recognize the reference before the normal model/reply pipeline. */
            if(!allowance(8000)){fail("Tier budget reached; start a new chat");goto done;}
            wav=memory(44+FS*8*2);if(!wav){fail("Not enough self-test memory");goto done;}
            s->loop_wav=wav;s->loop_samples=s->loop_peak=0;
            portENTER_CRITICAL(&lock);strcpy(s->view.reply,"Hello. Voice chat on this watch is ready.");portEXIT_CRITICAL(&lock);
            if(!speak())goto done;
            s->loop_wav=NULL;
            unsigned samples=s->loop_samples;
            if(samples<FS/2 || s->loop_peak<180){fail("Self-test could not hear the speaker");goto done;}
            wav_header(wav,samples);
            portENTER_CRITICAL(&lock);s->view.input_ms+=samples*1000/FS;s->view.record_ms=samples*1000/FS;s->view.peak=s->loop_peak;portEXIT_CRITICAL(&lock);
            if(!transcribe(wav,samples))goto done;
            char reference[384];voice_text_copy(reference,sizeof reference,s->view.input);
            for(char *p=reference;*p;p++)*p=(char)tolower((unsigned char)*p);
            if(!strstr(reference,"voice") || !strstr(reference,"watch")){fail("Self-test transcription did not match");goto done;}
            portENTER_CRITICAL(&lock);s->view.selftest=1;portEXIT_CRITICAL(&lock);
            free(wav);wav=NULL;
        }else if(s->request==1){
            wav=memory(44+MAX_SAMPLES*2);if(!wav){fail("Not enough recording memory");goto done;}
            unsigned samples=capture(wav,MAX_MS,0);if(!samples)goto done;
            if(!allowance(samples*1000/FS)){fail("Tier budget reached; start a new chat");goto done;}
            if(!transcribe(wav,samples))goto done;
            free(wav);wav=NULL;
        }else{
            portENTER_CRITICAL(&lock);voice_text_copy(s->view.input,sizeof s->view.input,s->request_text);portEXIT_CRITICAL(&lock);
            if(!allowance(0)){fail("Tier budget reached; start a new chat");goto done;}
        }
        if(cancelled() || !think())goto done;
        remember();
        if(cancelled())goto done;
        if(atomic_load(&s->speak_replies) && atomic_load(&s->output_volume) && !speak())goto done;
        ok=1;status(VOICE_DONE,"Reply ready; TALK to continue");
done:
        s->loop_wav=NULL;voice_audio_close();free(wav);memset(&s->session,0,sizeof s->session);
        if(atomic_load(&s->cancel))status(VOICE_IDLE,"Cancelled");
        else if(!ok && s->view.state!=VOICE_ERROR)status(VOICE_ERROR,"Voice request did not finish");
        portENTER_CRITICAL(&lock);s->view.level=0;s->view.generation++;portEXIT_CRITICAL(&lock);
        if(s->view.chat_id && s->history_ok) {
            voice_history_metadata(&s->archive,s->view.chat_id,s->view.turns,s->view.tier,
                s->view.input_ms,s->view.output_ms,s->view.cost_usd,s->view.estimated);
            atomic_store(&s->history_dirty,1);
        }
        atomic_store(&s->busy,0);
    }
}
static int request(int kind,const char *text)
{
    if(!s || !s->view.available)return 0;
    int expected=0;if(!atomic_compare_exchange_strong(&s->busy,&expected,1))return 0;
    if(kind!=3 && !s->view.chat_id && s->history_ok && s->archive.count==VOICE_SAVED_CHATS) {
        status(VOICE_ERROR,"Saved chats full; delete one in RECENTS");atomic_store(&s->busy,0);return 0;
    }
    if(kind!=3 && (!s->view.configured || hal_status(HAL_FEAT_WIFI)!=HAL_CONN_UP)){
        status(VOICE_ERROR,s->view.configured?"Connect Wi-Fi in Settings first":"Add your OpenRouter key over USB");atomic_store(&s->busy,0);return 0;}
    atomic_store(&s->cancel,0);atomic_store(&s->send,0);s->request=kind;s->tier=s->view.tier;s->session=s->config;
    voice_text_copy(s->request_text,sizeof s->request_text,text?text:"");
    portENTER_CRITICAL(&lock);s->view.record_ms=0;s->view.peak=s->view.nonzero=0;s->view.last_cost_usd=0;s->view.http_status=0;s->view.selftest=0;
    if(kind!=3){s->view.input[0]=0;s->view.reply[0]=0;s->view.excerpt=0;s->view.text_generation++;}portEXIT_CRITICAL(&lock);
    voice_audio_levels(nos_prefs.volume,s->archive.mic_gain);
    atomic_store(&s->output_volume,nos_prefs.volume);
    status(VOICE_STARTING,kind==1 || kind==3?"Opening microphone":"Getting ready");
    if(!s->worker && xTaskCreateWithCaps(worker,"voice",10240,NULL,3,&s->worker,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){
        s->worker=NULL;memset(&s->session,0,sizeof s->session);atomic_store(&s->busy,0);return fail("Cannot start voice worker");}
    xTaskNotifyGive(s->worker);return 1;
}
int voice_chat_start(void){return request(1,NULL);}
static int configure(const char *field,const char *value)
{
    if(!s || active())return 0;
    voice_config *copy=memory(sizeof *copy*2);if(!copy)return 0;
    *copy=s->config;char *dest=NULL;size_t capacity=0;
    if(!strcmp(field,"key")){dest=copy->key;capacity=sizeof copy->key;}
    if(!strcmp(field,"stt")){dest=copy->stt;capacity=sizeof copy->stt;}
    if(!strcmp(field,"tts")){dest=copy->tts;capacity=sizeof copy->tts;}
    if(!strcmp(field,"voice")){dest=copy->voice;capacity=sizeof copy->voice;}
    int ok=dest && strlen(value)<capacity && token(value,strlen(value)+1);
    if(ok){strcpy(dest,value);ok=valid_config(copy) && hal_store_save_checked("voice_chat",copy,sizeof *copy) &&
        hal_store_load("voice_chat",copy+1,sizeof *copy) && !memcmp(copy,copy+1,sizeof *copy);}
    if(ok){s->config=*copy;portENTER_CRITICAL(&lock);s->view.configured=!!copy->key[0];portEXIT_CRITICAL(&lock);status(VOICE_IDLE,"Voice configuration saved");}
    memset(copy,0,sizeof *copy*2);free(copy);return ok;
}
int voice_chat_command(const char *command)
{
    if(strcmp(command,"chat") && strncmp(command,"chat ",5))return 0;
    if(!s)return -1;
    const char *p=command+4;while(*p==' ')p++;
    int ok=1;
    if(!*p || !strcmp(p,"status")){
        voice_chat_snapshot *v=memory(sizeof *v);if(!v)return -1;voice_chat_get(v);
        printf("NOSCHAT available=%d configured=%d tier=%d state=%d busy=%d turns=%u input_ms=%u output_ms=%u record_ms=%u peak=%u nonzero=%u cost_usd=%.8f last_usd=%.8f estimated=%d http=%d generation=%u stack_free=%u selftest=%d volume=%d speak=%d mic_gain=%d chat_id=%u saved=%u history_dirty=%d history_error=%d phase=%d progress=%u total=%u\n",
            v->available,v->configured,v->tier,v->state,active(),(unsigned)v->turns,
            (unsigned)v->input_ms,(unsigned)v->output_ms,(unsigned)v->record_ms,(unsigned)v->peak,
            (unsigned)v->nonzero,v->cost_usd,v->last_cost_usd,v->estimated,v->http_status,
            (unsigned)v->generation,s->worker?(unsigned)uxTaskGetStackHighWaterMark(s->worker):0,v->selftest,
            v->volume,v->speak,v->mic_gain,(unsigned)v->chat_id,(unsigned)v->saved_count,v->storage_dirty,v->storage_error,
            v->progress_phase,(unsigned)v->progress_done,(unsigned)v->progress_total);
        printf("NOSCHATSTATUS %s\n",v->status);free(v);
    }else if(!strcmp(p,"start"))ok=voice_chat_start();
    else if(!strcmp(p,"history") || !strcmp(p,"history flush")) {
        if(!strcmp(p,"history flush")){s->history_retry=0;voice_chat_maintenance();ok=!voice_chat_history_pending();}
        printf("NOSCHATS count=%u capacity=%u bytes=%u dirty=%d error=%d\n",(unsigned)s->view.saved_count,
            VOICE_SAVED_CHATS,(unsigned)sizeof s->archive,voice_chat_history_pending(),s->history_error);
        if(!active())for(unsigned i=0;i<s->archive.count;i++)printf("NOSCHATITEM index=%u id=%u turns=%u retained=%u tier=%u\n",
            i,(unsigned)s->archive.chat[i].id,(unsigned)s->archive.chat[i].turns,s->archive.chat[i].count,s->archive.chat[i].tier);
    }
    else if(!strncmp(p,"volume ",7)) {char *end;long v=strtol(p+7,&end,10);ok=p[7] && !*end && v>=0 && v<=100 && voice_chat_volume((int)v);}
    else if(!strncmp(p,"audio ",6)) {int speak,gain;char extra;ok=sscanf(p+6,"%d %d %c",&speak,&gain,&extra)==2 && voice_chat_audio(speak,gain);}
    else if(!strncmp(p,"select ",7) || !strncmp(p,"delete ",7)) {
        char *end;unsigned long id=strtoul(p+7,&end,10);ok=p[7] && !*end && id && id<=UINT32_MAX;
        if(ok)ok=p[0]=='s'?voice_chat_history_select((uint32_t)id):voice_chat_history_delete((uint32_t)id);
    }
    else if(!strcmp(p,"send"))voice_chat_send();
    else if(!strcmp(p,"cancel"))voice_chat_cancel();
    else if(!strcmp(p,"clear"))ok=voice_chat_clear();
    else if(!strcmp(p,"test"))ok=request(3,NULL);
    else if(!strcmp(p,"selftest"))ok=request(4,NULL);
    else if(!strncmp(p,"ask ",4))ok=request(2,p+4);
    else if(!strncmp(p,"tier ",5) && strlen(p+5)==1)ok=voice_chat_tier(p[5]-'0');
    else if(!strncmp(p,"wifi ",5)){
        if(active())ok=0;
        else{
            cJSON *j=cJSON_Parse(p+5),*ssid=cJSON_GetObjectItemCaseSensitive(j,"ssid"),*pass=cJSON_GetObjectItemCaseSensitive(j,"password");
            ok=cJSON_IsString(ssid) && cJSON_IsString(pass) && strlen(ssid->valuestring)>0 &&
                strlen(ssid->valuestring)<=32 && (strlen(pass->valuestring)==0 ||
                (strlen(pass->valuestring)>=8 && strlen(pass->valuestring)<=63));
            if(ok){nos_prefs.wifi=1;prefs_save();hal_wifi_set_enabled(1);hal_wifi_connect(ssid->valuestring,pass->valuestring);}
            cJSON_Delete(j);
        }
    }
    else{
        const char *space=strchr(p,' ');char field[12]={0};
        if(!space || (size_t)(space-p)>=sizeof field)ok=0;
        else{memcpy(field,p,(size_t)(space-p));ok=configure(field,space+1);}
    }
    /* No argument or upstream error body is echoed, including rejected keys. */
    if(!ok){printf("ERR chat rejected\n");return -1;}
    printf("NOSCHAT_OK\n");return 1;
}
#else
#include <stdio.h>
#include <string.h>
void voice_chat_init(void){}
void voice_chat_get(voice_chat_snapshot *out){if(out){memset(out,0,sizeof *out);strcpy(out->status,"Requires C-board microphone and speaker");}}
int voice_chat_start(void){return 0;}
void voice_chat_send(void){}
void voice_chat_cancel(void){}
int voice_chat_active(void){return 0;}
int voice_chat_tier(int tier){(void)tier;return 0;}
int voice_chat_clear(void){return 0;}
int voice_chat_volume(int volume){(void)volume;return 0;}
int voice_chat_audio(int speak,int gain){(void)speak;(void)gain;return 0;}
int voice_chat_history_list(voice_saved_chat *out,unsigned n){(void)out;(void)n;return 0;}
int voice_chat_history_select(uint32_t id){(void)id;return 0;}
int voice_chat_history_delete(uint32_t id){(void)id;return 0;}
int voice_chat_history_pending(void){return 0;}
void voice_chat_maintenance(void){}
int voice_chat_command(const char *command){(void)command;printf("ERR chat requires C board\n");return -1;}
#endif
