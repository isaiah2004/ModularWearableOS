/* Included by ext_audio.c. Only extf may touch these files, SD mutex or leases.
 * There is one bounded path request per stream and fixed producer/consumer rings. */
enum { AIO_OPEN=1,AIO_READ,AIO_WRITE,AIO_SEEK,AIO_FLUSH,AIO_SYNC,AIO_CLOSE,AIO_PUBLISH,AIO_STAT };
static int64_t audio_io_begin(void) { board_sd_asset_lock();return esp_timer_get_time(); }
static void audio_io_end(int64_t began,int op,int error)
{
    uint32_t elapsed=(uint32_t)(esp_timer_get_time()-began);
    board_sd_asset_unlock();
    A->io_calls++;if(elapsed>A->io_max_us)A->io_max_us=elapsed;
    if(error){A->io_errors++;A->last_io_op=op;A->last_errno=error;}
}
static size_t audio_read(void *p,size_t size,size_t count,FILE *f)
{
    int64_t t=audio_io_begin();errno=0;size_t got=fread(p,size,count,f);
    int error=ferror(f)?(errno?errno:EIO):0;audio_io_end(t,AIO_READ,error);return got;
}
static size_t audio_write(const void *p,size_t size,size_t count,FILE *f)
{
    int64_t t=audio_io_begin();errno=0;size_t wrote=fwrite(p,size,count,f);
    audio_io_end(t,AIO_WRITE,wrote==count?0:(errno?errno:EIO));return wrote;
}
static int audio_seek(FILE *f,long offset,int whence)
{
    int64_t t=audio_io_begin();errno=0;int result=fseek(f,offset,whence);
    audio_io_end(t,AIO_SEEK,result?(errno?errno:EIO):0);return result==0;
}
static int audio_close(FILE **file,int *lease)
{
    if(*file) {
        FILE *owned=*file;*file=NULL;
        int64_t t=audio_io_begin();errno=0;int result=fclose(owned);
        int error=result?(errno?errno:EIO):0;audio_io_end(t,AIO_CLOSE,error);
        if(error){A->close_error=error;return 0;}
    }
    if(*lease){board_sd_asset_release();*lease=0;}
    return 1;
}
static int wav_header(FILE *f,uint32_t data_bytes)
{
    uint8_t h[44];uint32_t v;
    memcpy(h,"RIFF",4);v=36+data_bytes;memcpy(h+4,&v,4);
    memcpy(h+8,"WAVEfmt ",8);v=16;memcpy(h+16,&v,4);
    uint16_t fmt[2]={1,1};memcpy(h+20,fmt,4);
    v=AU_FS;memcpy(h+24,&v,4);v=AU_FS*2;memcpy(h+28,&v,4);
    uint16_t ba[2]={2,16};memcpy(h+32,ba,4);
    memcpy(h+36,"data",4);memcpy(h+40,&data_bytes,4);
    return audio_write(h,1,sizeof h,f)==sizeof h;
}
static int playback_open(const char *path)
{
    if(!board_sd_asset_acquire())return 0;
    A->play_lease=1;
    int64_t t=audio_io_begin();errno=0;A->play_f=fopen(path,"rb");
    audio_io_end(t,AIO_OPEN,A->play_f?0:(errno?errno:EIO));
    if(!A->play_f){audio_close(&A->play_f,&A->play_lease);return 0;}
    if(!audio_seek(A->play_f,0,SEEK_END))goto fail;
    t=audio_io_begin();errno=0;long size=ftell(A->play_f);
    audio_io_end(t,AIO_SEEK,size<0?(errno?errno:EIO):0);
    if(size<=0||(uint64_t)size>UINT32_MAX||!audio_seek(A->play_f,0,SEEK_SET))goto fail;
    uint8_t h[12];uint32_t data=(uint32_t)size;
    if(size>=12&&audio_read(h,1,12,A->play_f)==12&&!memcmp(h,"RIFF",4)) {
        if(memcmp(h+8,"WAVE",4))goto fail;
        int format=0,found=0;
        for(unsigned chunks=0;chunks<128;chunks++) {
            uint8_t ch[8];if(audio_read(ch,1,8,A->play_f)!=8)break;
            uint32_t len;memcpy(&len,ch+4,4);
            t=audio_io_begin();long offset=ftell(A->play_f);audio_io_end(t,AIO_SEEK,offset<0?EIO:0);
            if(offset<0||(uint64_t)offset+len+(len&1u)>(uint64_t)size)break;
            if(!memcmp(ch,"fmt ",4)) {
                uint8_t fm[16];if(len<16||audio_read(fm,1,16,A->play_f)!=16)break;
                uint16_t fmt,channels,bits;uint32_t rate;
                memcpy(&fmt,fm,2);memcpy(&channels,fm+2,2);memcpy(&rate,fm+4,4);memcpy(&bits,fm+14,2);
                if(fmt!=1||channels!=1||bits!=16||rate!=AU_FS)break;
                format=1;if(!audio_seek(A->play_f,(long)(len-16+(len&1u)),SEEK_CUR))break;
            } else if(!memcmp(ch,"data",4)) {if(format&&!(len&1u)){data=len;found=1;}break;}
            else if(!audio_seek(A->play_f,(long)(len+(len&1u)),SEEK_CUR))break;
        }
        if(!found)goto fail;
    } else if(!audio_seek(A->play_f,0,SEEK_SET))goto fail;
    if(data&1u)goto fail;
    A->play_left=data;return 1;
fail:
    audio_close(&A->play_f,&A->play_lease);return 0;
}
static int record_open(void)
{
    if(!board_sd_asset_acquire())return HAL_REC_ERROR;
    A->rec_lease=1;
    int64_t t=audio_io_begin();errno=0;struct stat st;
    int exists=stat(A->rec_path,&st)==0;int stat_error=exists?EEXIST:errno;
    audio_io_end(t,AIO_STAT,stat_error==ENOENT?0:stat_error);
    if(exists||stat_error!=ENOENT){audio_close(&A->rec_f,&A->rec_lease);return exists?HAL_REC_EXISTS:HAL_REC_ERROR;}
    snprintf(A->rec_temp,sizeof A->rec_temp,"%s.part",A->rec_path);
    t=audio_io_begin();errno=0;
    int fd=open(A->rec_temp,O_RDWR|O_CREAT|O_EXCL,0666);int error=fd<0?(errno?errno:EIO):0;
    if(fd>=0){A->rec_f=fdopen(fd,"w+b");if(!A->rec_f){error=errno?errno:EIO;close(fd);}}
    audio_io_end(t,AIO_OPEN,error);
    if(!A->rec_f){audio_close(&A->rec_f,&A->rec_lease);return error==EEXIST?HAL_REC_EXISTS:HAL_REC_ERROR;}
    if(!wav_header(A->rec_f,0)){audio_close(&A->rec_f,&A->rec_lease);return HAL_REC_ERROR;}
    return HAL_REC_PENDING;
}
static int record_publish(void)
{
    int64_t t=audio_io_begin();errno=0;
#if defined(__unix__) && !defined(ESP_PLATFORM)
    /* POSIX rename replaces the destination. link is the atomic no-clobber
     * publication on host filesystems; production FatFs/Windows rename refuses
     * an existing destination. Keep the .part on every publication failure. */
    int result=link(A->rec_temp,A->rec_path);
    if(!result)unlink(A->rec_temp);
#else
    int result=rename(A->rec_temp,A->rec_path);
#endif
    int error=result?(errno?errno:EIO):0;audio_io_end(t,AIO_PUBLISH,error);
    return !result?HAL_REC_DONE:error==EEXIST?HAL_REC_EXISTS:HAL_REC_ERROR;
}
static void record_finish(uint32_t now)
{
    if(!ext_due(now,A->file_retry_at))return;
    if(!audio_seek(A->rec_f,0,SEEK_SET)||!wav_header(A->rec_f,A->rec_written*2)) {
        A->file_retry_at=ext_ms()+1000;return;
    }
    int64_t t=audio_io_begin();errno=0;int flushed=fflush(A->rec_f);
    int error=flushed?(errno?errno:EIO):0;audio_io_end(t,AIO_FLUSH,error);
    if(error){clearerr(A->rec_f);A->file_retry_at=ext_ms()+1000;return;}
    t=audio_io_begin();errno=0;int synced=fsync(fileno(A->rec_f));
    error=synced?(errno?errno:EIO):0;audio_io_end(t,AIO_SYNC,error);
    if(error){A->file_retry_at=ext_ms()+1000;return;}
    /* Keep the lease across close + publication. A failed close is ambiguous;
     * retain the lease/error barrier rather than falsely allow unmount/sleep. */
    int lease=A->rec_lease;A->rec_lease=0;
    int closed=audio_close(&A->rec_f,&A->rec_lease);A->rec_lease=lease;
    int result=!closed||A->rec_err?HAL_REC_ERROR:record_publish();
    if(closed&&A->rec_lease){board_sd_asset_release();A->rec_lease=0;}
    portENTER_CRITICAL(&ext_lock);
    A->recording=0;A->rec_stop_req=0;A->rec_result=result;A->rec_status=result==HAL_REC_DONE?3:4;
    portEXIT_CRITICAL(&ext_lock);
}
uint32_t audio_service(uint32_t now)
{
    if(!A)return 1000;
    char play_path[AU_PATH_MAX];int play_request,play_stop,rec_request;
    portENTER_CRITICAL(&ext_lock);
    play_request=A->play_req_flag;play_stop=A->play_stop_req;A->play_req_flag=A->play_stop_req=0;
    if(play_request){memcpy(play_path,A->play_path,sizeof play_path);A->play_opening=1;}
    rec_request=A->rec_pending;A->rec_pending=0;if(rec_request)A->rec_opening=1;
    if(ext_shutdown_requested()){play_stop=1;A->rec_stop_req=1;}
    portEXIT_CRITICAL(&ext_lock);
    if(play_stop||play_request) {
        int closed=audio_close(&A->play_f,&A->play_lease);
        portENTER_CRITICAL(&ext_lock);
        A->pcm_gen++;A->pcm_head=A->pcm_tail=A->play_pos=0;A->playing=0;A->play_eof=1;
        if(!closed)A->play_result=-1;
        portEXIT_CRITICAL(&ext_lock);
    }
    if(play_request) {
        int ok=!play_stop&&!ext_shutdown_requested()&&nos_runtime_recording_allowed()&&!A->close_error&&playback_open(play_path);
        portENTER_CRITICAL(&ext_lock);
        A->play_opening=0;A->play_result=ok?0:-1;A->playing=ok;A->play_eof=!ok;
        portEXIT_CRITICAL(&ext_lock);if(ok)wake();
    }
    if(A->play_f) {
        uint32_t head,tail;portENTER_CRITICAL(&ext_lock);head=A->pcm_head;tail=A->pcm_tail;portEXIT_CRITICAL(&ext_lock);
        if(PCM_RING-(head-tail)>=FILE_CHUNK) {
            uint32_t want=A->play_left/2<FILE_CHUNK?A->play_left/2:FILE_CHUNK;
            size_t got=want?audio_read(A->chunk,2,want,A->play_f):0;
            for(size_t i=0;i<got;i++)A->pcm[(head+i)%PCM_RING]=A->chunk[i];
            A->play_left-=(uint32_t)got*2;
            int eof=!A->play_left||got<want;
            portENTER_CRITICAL(&ext_lock);A->pcm_head=head+(uint32_t)got;
            if(eof){A->play_eof=1;if(got<want)A->play_result=-1;}
            portEXIT_CRITICAL(&ext_lock);
            if(eof&&!audio_close(&A->play_f,&A->play_lease))A->play_result=-1;
        }
    }
    if(rec_request) {
        int result=ext_shutdown_requested()||!nos_runtime_recording_allowed()||A->close_error?HAL_REC_ERROR:record_open();
        portENTER_CRITICAL(&ext_lock);A->rec_opening=0;
        if(result==HAL_REC_PENDING){A->recording=1;A->rec_status=A->rec_stop_req?2:1;}
        else {A->rec_status=4;A->rec_result=result;}
        portEXIT_CRITICAL(&ext_lock);if(result==HAL_REC_PENDING)wake();
    }
    if(A->rec_f) {
        uint32_t head,tail;int producing,stopping;
        portENTER_CRITICAL(&ext_lock);head=A->rec_head;tail=A->rec_tail;producing=A->rec_producing;
        stopping=A->rec_stop_req||A->rec_err||A->rec_samples>=(uint32_t)REC_MAX_MS*(AU_FS/1000);
        if(stopping){A->rec_stop_req=1;A->rec_status=2;}
        portEXIT_CRITICAL(&ext_lock);
        uint32_t avail=head-tail;
        if(A->rec_err) {portENTER_CRITICAL(&ext_lock);A->rec_drops+=avail;A->rec_tail=head;portEXIT_CRITICAL(&ext_lock);avail=0;}
        else if(avail>=FILE_CHUNK||(stopping&&avail)) {
            uint32_t n=avail<FILE_CHUNK?avail:FILE_CHUNK;
            for(uint32_t i=0;i<n;i++)A->chunk[i]=A->rec[(tail+i)%REC_RING];
            size_t written=audio_write(A->chunk,2,n,A->rec_f);A->rec_written+=(uint32_t)written;
            portENTER_CRITICAL(&ext_lock);A->rec_tail=tail+n;
            if(written!=n){A->rec_err=1;A->rec_stop_req=1;A->rec_drops+=n-(uint32_t)written;}
            portEXIT_CRITICAL(&ext_lock);avail-=n;
        }
        if(stopping&&!avail&&!producing)record_finish(ext_ms());
    }
    portENTER_CRITICAL(&ext_lock);
    A->file_idle=!A->play_f&&!A->rec_f&&!A->play_lease&&!A->rec_lease&&!A->close_error&&
                 !A->play_req_flag&&!A->play_opening&&!A->rec_pending&&!A->rec_opening;
    portEXIT_CRITICAL(&ext_lock);
    (void)now;return 20;
}
static void audio_file_task(void *arg)
{
    (void)arg;
    for(;;){uint32_t wait=audio_service(ext_ms());ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(wait));}
}
