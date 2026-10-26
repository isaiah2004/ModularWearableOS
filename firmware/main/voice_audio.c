/* C-board ES8311/ES7210. One worker owns these handles and all blocking I/O.
 * Pin mapping and gain follow Waveshare's 1.43C audio demo. No B strap pins. */
#include "sdkconfig.h"
#include "voice_audio.h"
#if CONFIG_WATCH_BOARD_143C
#include "board.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdatomic.h>
static atomic_int wanted_volume=60,wanted_gain=30;
static int applied_volume=-1,applied_gain=-1;
void voice_audio_levels(int volume,int mic_gain)
{
    if(volume>=0 && volume<=100)atomic_store(&wanted_volume,volume);
    if(mic_gain>=0 && mic_gain<=42)atomic_store(&wanted_gain,mic_gain);
}
static i2s_chan_handle_t tx,rx;
static const audio_codec_data_if_t *data_if;
static const audio_codec_gpio_if_t *gpio_if;
static const audio_codec_ctrl_if_t *dac_ctrl,*adc_ctrl;
static const audio_codec_if_t *dac,*adc;
static esp_codec_dev_handle_t speaker,mic;
static int channel=-1,opened;
static int16_t stereo[256]; /* only the exclusive audio worker touches this */
static int levels_apply(void)
{
    int volume=atomic_load(&wanted_volume),gain=atomic_load(&wanted_gain);
    if(volume!=applied_volume) {
        if(esp_codec_dev_set_out_vol(speaker,volume)!=ESP_CODEC_DEV_OK)return 0;
        applied_volume=volume;
    }
    if(gain!=applied_gain) {
        if(esp_codec_dev_set_in_gain(mic,gain)!=ESP_CODEC_DEV_OK)return 0;
        applied_gain=gain;
    }
    return 1;
}
void voice_audio_speaker(int on){gpio_set_level(GPIO_NUM_46,!!on);}
void voice_audio_close(void)
{
    voice_audio_speaker(0);
    if(mic){esp_codec_dev_close(mic);esp_codec_dev_delete(mic);mic=NULL;}
    if(speaker){esp_codec_dev_close(speaker);esp_codec_dev_delete(speaker);speaker=NULL;}
    if(adc){audio_codec_delete_codec_if(adc);adc=NULL;}
    if(dac){audio_codec_delete_codec_if(dac);dac=NULL;}
    if(adc_ctrl){audio_codec_delete_ctrl_if(adc_ctrl);adc_ctrl=NULL;}
    if(dac_ctrl){audio_codec_delete_ctrl_if(dac_ctrl);dac_ctrl=NULL;}
    if(data_if){audio_codec_delete_data_if(data_if);data_if=NULL;}
    if(gpio_if){audio_codec_delete_gpio_if(gpio_if);gpio_if=NULL;}
    i2s_chan_info_t info;
    if(rx){if(i2s_channel_get_info(rx,&info)==ESP_OK && info.is_enabled)i2s_channel_disable(rx);i2s_del_channel(rx);rx=NULL;}
    if(tx){if(i2s_channel_get_info(tx,&info)==ESP_OK && info.is_enabled)i2s_channel_disable(tx);i2s_del_channel(tx);tx=NULL;}
    gpio_set_level(GPIO_NUM_18,0);opened=0;channel=-1;applied_volume=applied_gain=-1;
}
int voice_audio_open(void)
{
    if(opened)return 1;
    gpio_config_t pins={.pin_bit_mask=(1ULL<<18)|(1ULL<<46),.mode=GPIO_MODE_OUTPUT};
    if(gpio_config(&pins)!=ESP_OK)return 0;
    voice_audio_speaker(0);gpio_set_level(GPIO_NUM_18,1);vTaskDelay(pdMS_TO_TICKS(25));
    if(i2c_master_probe(board_i2c_bus(),0x18,50)!=ESP_OK ||
       i2c_master_probe(board_i2c_bus(),0x40,50)!=ESP_OK)goto fail;
    i2s_chan_config_t cc=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0,I2S_ROLE_MASTER);
    cc.dma_desc_num=4;cc.dma_frame_num=64;cc.auto_clear=true;
    if(i2s_new_channel(&cc,&tx,&rx)!=ESP_OK)goto fail;
    i2s_std_config_t sc={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),
        .gpio_cfg={.mclk=GPIO_NUM_38,.bclk=GPIO_NUM_39,.ws=GPIO_NUM_40,.dout=GPIO_NUM_41,.din=GPIO_NUM_42}};
    if(i2s_channel_init_std_mode(tx,&sc)!=ESP_OK || i2s_channel_init_std_mode(rx,&sc)!=ESP_OK)goto fail;
    audio_codec_i2s_cfg_t dc={.port=0,.rx_handle=rx,.tx_handle=tx};
    data_if=audio_codec_new_i2s_data(&dc);gpio_if=audio_codec_new_gpio();
    audio_codec_i2c_cfg_t ic={.port=0,.addr=ES8311_CODEC_DEFAULT_ADDR,.bus_handle=board_i2c_bus()};
    dac_ctrl=audio_codec_new_i2c_ctrl(&ic);ic.addr=ES7210_CODEC_DEFAULT_ADDR;
    adc_ctrl=audio_codec_new_i2c_ctrl(&ic);
    if(!data_if || !gpio_if || !dac_ctrl || !adc_ctrl)goto fail;
    es8311_codec_cfg_t out={.ctrl_if=dac_ctrl,.gpio_if=gpio_if,.codec_mode=ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin=-1,.use_mclk=true,.hw_gain.pa_gain=6};
    es7210_codec_cfg_t in={.ctrl_if=adc_ctrl,.mic_selected=ES7210_SEL_MIC1|ES7210_SEL_MIC2};
    dac=es8311_codec_new(&out);adc=es7210_codec_new(&in);if(!dac || !adc)goto fail;
    esp_codec_dev_cfg_t dev={.dev_type=ESP_CODEC_DEV_TYPE_OUT,.codec_if=dac,.data_if=data_if};
    speaker=esp_codec_dev_new(&dev);dev.dev_type=ESP_CODEC_DEV_TYPE_IN;dev.codec_if=adc;
    mic=esp_codec_dev_new(&dev);if(!speaker || !mic)goto fail;
    esp_codec_dev_sample_info_t fs={.sample_rate=16000,.channel=2,.bits_per_sample=16};
    if(esp_codec_dev_open(speaker,&fs)!=ESP_CODEC_DEV_OK || esp_codec_dev_open(mic,&fs)!=ESP_CODEC_DEV_OK ||
       !levels_apply())goto fail;
    channel=-1;opened=1;return 1;
fail:voice_audio_close();return 0;
}
int voice_audio_read(int16_t *mono,size_t frames,unsigned *peak,unsigned *nonzero)
{
    if(!opened || frames>128 || !levels_apply())return 0;
    size_t bytes=0;
    if(i2s_channel_read(rx,stereo,frames*4,&bytes,100)!=ESP_OK || bytes!=frames*4)return 0;
    if(channel<0){uint64_t e[2]={0};for(size_t i=0;i<frames;i++)for(int c=0;c<2;c++)e[c]+=(int32_t)stereo[2*i+c]*(int32_t)stereo[2*i+c];channel=e[1]>e[0];}
    unsigned p=0,n=0;for(size_t i=0;i<frames;i++){int v=stereo[2*i+channel];mono[i]=(int16_t)v;unsigned a=v<0?-v:v;if(a>p)p=a;if(a>4)n++;}
    if(peak)*peak=p;
    if(nonzero)*nonzero=n;
    return 1;
}
int voice_audio_write(const int16_t *mono,size_t frames)
{
    if(!opened || !levels_apply())return 0;
    while(frames){size_t n=frames>128?128:frames,bytes=0;
        for(size_t i=0;i<n;i++)stereo[2*i]=stereo[2*i+1]=mono[i];
        if(i2s_channel_write(tx,stereo,n*4,&bytes,200)!=ESP_OK || bytes!=n*4)return 0;
        mono+=n;frames-=n;
    }return 1;
}
#else
void voice_audio_levels(int volume,int mic_gain){(void)volume;(void)mic_gain;}
int voice_audio_open(void){return 0;}
void voice_audio_close(void){}
int voice_audio_read(int16_t *m,size_t n,unsigned *p,unsigned *z){(void)m;(void)n;(void)p;(void)z;return 0;}
int voice_audio_write(const int16_t *m,size_t n){(void)m;(void)n;return 0;}
void voice_audio_speaker(int on){(void)on;}
#endif
