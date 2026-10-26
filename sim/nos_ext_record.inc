/* Simulator recording file owner. The UI queues bounded paths and waits for
 * checked .part finalization; synthetic PCM is silence, never a real mic claim. */
static struct {
    SRWLOCK lock;HANDLE event,thread;
    int pending,stop,status,result,owned,close_error;
    uint32_t t0,duration_ms,target;
    char path[256];
} sim_record = {.lock=SRWLOCK_INIT,.result=HAL_REC_DONE};
static int sim_wav_header(FILE *f,uint32_t bytes)
{
    uint8_t h[44]={0};uint32_t value;
    memcpy(h,"RIFF",4);value=36+bytes;memcpy(h+4,&value,4);memcpy(h+8,"WAVEfmt ",8);
    value=16;memcpy(h+16,&value,4);h[20]=h[22]=1;
    value=16000;memcpy(h+24,&value,4);value=32000;memcpy(h+28,&value,4);h[32]=2;h[34]=16;
    memcpy(h+36,"data",4);memcpy(h+40,&bytes,4);return fwrite(h,1,sizeof h,f)==sizeof h;
}
static void sim_record_terminal(int status,int result)
{
    AcquireSRWLockExclusive(&sim_record.lock);
    sim_record.status=status;sim_record.result=result;sim_record.owned=0;
    ReleaseSRWLockExclusive(&sim_record.lock);
}
static DWORD WINAPI sim_record_worker(LPVOID ignored)
{
    (void)ignored;
    for(;;) {
        WaitForSingleObject(sim_record.event,50);
        AcquireSRWLockExclusive(&sim_record.lock);
        int request=sim_record.pending;sim_record.pending=0;
        char path[256];if(request){memcpy(path,sim_record.path,sizeof path);sim_record.owned=1;}
        ReleaseSRWLockExclusive(&sim_record.lock);
        if(!request)continue;
        struct stat info;errno=0;
        if(!stat(path,&info)){sim_record_terminal(4,HAL_REC_EXISTS);continue;}
        if(errno!=ENOENT){sim_record_terminal(4,HAL_REC_ERROR);continue;}
        char part[264];snprintf(part,sizeof part,"%s.part",path);
        int fd=_open(part,_O_RDWR|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE);
        if(fd<0){sim_record_terminal(4,errno==EEXIST?HAL_REC_EXISTS:HAL_REC_ERROR);continue;}
        FILE *file=_fdopen(fd,"w+b");
        if(!file){_close(fd);sim_record_terminal(4,HAL_REC_ERROR);continue;}
        int ok=sim_wav_header(file,0);
        uint32_t samples=0;
        while(ok) {
            AcquireSRWLockExclusive(&sim_record.lock);
            int stop=sim_record.stop;uint32_t target=sim_record.target;
            ReleaseSRWLockExclusive(&sim_record.lock);
            if(stop){samples=target;break;}
            WaitForSingleObject(sim_record.event,50);
        }
        int16_t silence[2048]={0};uint32_t written=0;
        while(ok&&written<samples){uint32_t count=samples-written;if(count>2048)count=2048;
            if(fwrite(silence,2,count,file)!=count){ok=0;break;}written+=count;Sleep(0);}
        if(fseek(file,0,SEEK_SET)||!sim_wav_header(file,written*2)||fflush(file)||_commit(_fileno(file)))ok=0;
        int close_result=fclose(file);
        if(close_result){AcquireSRWLockExclusive(&sim_record.lock);sim_record.close_error=errno?errno:EIO;ReleaseSRWLockExclusive(&sim_record.lock);ok=0;}
        int result=HAL_REC_ERROR;
        if(ok){if(!rename(part,path))result=HAL_REC_DONE;else if(errno==EEXIST)result=HAL_REC_EXISTS;}
        sim_record_terminal(result==HAL_REC_DONE?3:4,result);
    }
    return 0;
}
int hal_rec_start_exclusive(const char *path)
{
    if(!has(EXT_MIC)||!path||!*path||strlen(path)>=250||s_ext_stopping)return 0;
    AcquireSRWLockExclusive(&sim_record.lock);
    if(sim_record.status==1||sim_record.status==2||sim_record.owned||sim_record.close_error||s_ext_stopping){ReleaseSRWLockExclusive(&sim_record.lock);return 0;}
    if(!sim_record.thread) {
        sim_record.event=CreateEvent(NULL,FALSE,FALSE,NULL);
        if(sim_record.event)sim_record.thread=CreateThread(NULL,0,sim_record_worker,NULL,0,NULL);
        if(!sim_record.thread){if(sim_record.event)CloseHandle(sim_record.event);sim_record.event=NULL;ReleaseSRWLockExclusive(&sim_record.lock);return 0;}
    }
    snprintf(sim_record.path,sizeof sim_record.path,"%s",path);
    sim_record.t0=now_ms();sim_record.duration_ms=sim_record.target=0;
    sim_record.pending=1;sim_record.stop=0;sim_record.status=1;sim_record.result=HAL_REC_PENDING;
    S.rec=1;S.rec_t0=sim_record.t0;S.rec_ms=0;
    ReleaseSRWLockExclusive(&sim_record.lock);SetEvent(sim_record.event);return 1;
}
int hal_rec_start(const char *path){return hal_rec_start_exclusive(path)==1;}
void hal_rec_stop(void)
{
    AcquireSRWLockExclusive(&sim_record.lock);
    if(sim_record.status==1){uint32_t elapsed=now_ms()-sim_record.t0;if(elapsed>600000)elapsed=600000;
        sim_record.duration_ms=elapsed;sim_record.target=elapsed*16;sim_record.stop=1;sim_record.status=2;
        S.rec=0;S.rec_ms=elapsed;}
    HANDLE event=sim_record.event;ReleaseSRWLockExclusive(&sim_record.lock);if(event)SetEvent(event);
}
int hal_rec_status(void){AcquireSRWLockShared(&sim_record.lock);int state=sim_record.status;ReleaseSRWLockShared(&sim_record.lock);return state;}
int hal_rec_result(void){AcquireSRWLockShared(&sim_record.lock);int result=sim_record.result;ReleaseSRWLockShared(&sim_record.lock);return result;}
int hal_rec_active(void){int state=hal_rec_status();return state==1||state==2;}
uint32_t hal_rec_ms(void)
{
    AcquireSRWLockShared(&sim_record.lock);uint32_t elapsed=sim_record.status==1?now_ms()-sim_record.t0:sim_record.duration_ms;
    ReleaseSRWLockShared(&sim_record.lock);return elapsed;
}
