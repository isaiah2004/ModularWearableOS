#include <stdatomic.h>
#include <assert.h>
#include <stdio.h>
#define ESP_CODEC_DEV_OK 0
static atomic_int wanted_volume=60,wanted_gain=30;
static int applied_volume=-1,applied_gain=-1,speaker=1,mic=2;
static int out_calls,in_calls,fail_volume,fail_gain,out_value,in_value;
static int esp_codec_dev_set_out_vol(int handle,int value)
{assert(handle==speaker);out_calls++;out_value=value;return fail_volume?-1:0;}
static int esp_codec_dev_set_in_gain(int handle,int value)
{assert(handle==mic);in_calls++;in_value=value;return fail_gain?-1:0;}
/* ACTUAL FUNCTIONS */
int main(void)
{
    voice_audio_levels(24,36);assert(!out_calls && !in_calls); /* UI never touches codec handles */
    assert(levels_apply() && out_value==24 && in_value==36 && out_calls==1 && in_calls==1);
    assert(levels_apply() && out_calls==1 && in_calls==1);
    voice_audio_levels(0,24);assert(out_calls==1 && in_calls==1);assert(levels_apply() && out_value==0 && in_value==24);
    voice_audio_levels(101,100);assert(levels_apply() && out_value==0 && in_value==24);
    fail_volume=1;voice_audio_levels(50,30);assert(!levels_apply() && applied_volume==0);
    fail_volume=0;fail_gain=1;assert(!levels_apply() && applied_volume==50 && applied_gain==24);
    fail_gain=0;assert(levels_apply() && applied_gain==30);
    puts("PASS live codec targets: validation, mute, worker-only I/O, unchanged fast path and failed apply retry");
}
