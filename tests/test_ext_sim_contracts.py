"""Simulator counterparts of the peripheral request/result and expiry contracts.

All files, compiler temporaries and executables stay in the private A: check tree.
The recorder writes synthetic silence; this is not a microphone or modem test.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/alpha05/checks/peripherals/simulator'
SOURCE=r'''
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include "hal.h"
#include "hal_ext.h"
#include "nos_sim.h"
#include "prefs.h"
nos_prefs_t nos_prefs;void prefs_save(void){}
int hal_store_load(const char *key,void *data,int size){(void)key;(void)data;(void)size;return 0;}
int hal_store_save_checked(const char *key,const void *data,int size){(void)key;(void)data;(void)size;return 1;}
int sim_virtual=1,sim_button_down;double sim_vclock_ms;
static volatile LONG64 clock_ms=1000;
uint32_t hal_millis(void){return (uint32_t)InterlockedCompareExchange64(&clock_ms,0,0);}
uint64_t hal_epoch_seconds(void){return 1792569600u+hal_millis()/1000;}
void hal_get_datetime(hal_datetime *p){*p=(hal_datetime){.year=2026,.month=10,.day=21,.hour=12};}
void sim_step_rate_set(float rate){(void)rate;}
extern uint32_t hal_ext_sim_present;
static void advance(uint32_t ms){InterlockedAdd64(&clock_ms,ms);nos_ext_sim_tick(hal_millis());}
static void path_of(char path[256],const char *dir,const char *name){snprintf(path,256,"%s/%s",dir,name);}
static int exists(const char *path){struct stat st;return !stat(path,&st);}
static void create(const char *path){FILE *f=fopen(path,"wb");assert(f);assert(fwrite("KEEP ME",1,7,f)==7);assert(!fclose(f));}
static void wait_record(int status){DWORD began=GetTickCount();while(hal_rec_status()!=status){assert(GetTickCount()-began<3000);Sleep(1);}}
static void wait_path(const char *path){DWORD began=GetTickCount();while(!exists(path)){assert(GetTickCount()-began<3000);Sleep(1);}}
int main(int argc,char **argv){
 assert(argc==2);hal_ext_init();hal_env e;hal_light l;
 assert(!hal_ext_debug_command("sfx nope")&&!hal_ext_debug_command("haptic 98")&&!hal_ext_debug_command("sea nan"));
 assert(!hal_ext_debug_command("play missing-file")&&!hal_ext_debug_command("mag map +x+y+z")&&!hal_ext_debug_command("lte at AT"));
 assert(hal_ext_debug_command("vol 50")&&hal_ext_debug_command("mag cal")&&hal_ext_debug_command("mag done"));
 assert(hal_env_get(&e)&&e.humidity_valid&&e.sample_ms==1000);
 assert(hal_light_get(&l)&&l.prox_valid&&l.sample_ms==1000&&l.prox_ms==1000);
 char mask[24];snprintf(mask,sizeof mask,"%u",EXT_ENV|EXT_LIGHT);assert(nos_ext_sim_set("extfail",mask));
 advance(NOS_LIGHT_MAX_AGE_MS+1);assert(!hal_light_get(&l)&&!l.prox_valid&&l.prox==0&&l.sample_ms==1000);
 advance(NOS_ENV_MAX_AGE_MS);assert(!hal_env_get(&e)&&!e.humidity_valid&&e.sample_ms==1000);
 assert(nos_ext_sim_set("extfail","0"));assert(hal_env_get(&e)&&hal_light_get(&l));
 hal_ext_sim_present&=~(EXT_ENV|EXT_LIGHT);assert(!hal_env_get(&e)&&!hal_light_get(&l)&&!e.humidity_valid&&!l.prox_valid);
 hal_ext_sim_present|=EXT_ENV|EXT_LIGHT;assert(hal_env_get(&e)&&hal_light_get(&l));
 hal_sms messages[4];assert(hal_lte_sms_list(messages,4)==4);uint32_t id=messages[0].id;assert(id);
 hal_lte_sms_mark_read_id(id);assert(hal_lte_sms_list(messages,4)==4&&messages[0].id==id&&!messages[0].unread);
 assert(!hal_lte_sms_send_request("bad","x"));uint32_t send=hal_lte_sms_send_request("+911234567890","test");
 assert(send&&hal_lte_sms_send_status(send)==1&&!hal_lte_sms_send_request("+911234567890","busy"));
 advance(100);assert(hal_lte_sms_send_status(send)==2);advance(3900);assert(hal_lte_sms_send_status(send)==3);
 snprintf(mask,sizeof mask,"%u",EXT_LTE);nos_ext_sim_set("extfail",mask);
 uint32_t fail=hal_lte_sms_send_request("+911234567890","error");assert(fail);advance(4000);assert(hal_lte_sms_send_status(fail)==4);
 nos_ext_sim_set("extfail","0");assert(hal_lte_sms_send_status(send)==3);
 char path[256],part[264];path_of(path,argv[1],"existing.wav");create(path);
 assert(hal_rec_status()==0&&hal_rec_start_exclusive(path)==1);wait_record(4);assert(hal_rec_result()==HAL_REC_EXISTS);
 struct stat info;assert(!stat(path,&info)&&info.st_size==7);
 path_of(path,argv[1],"reserved.wav");snprintf(part,sizeof part,"%s.part",path);create(part);
 assert(hal_rec_start_exclusive(path)==1);wait_record(4);assert(hal_rec_result()==HAL_REC_EXISTS&&!exists(path));
 path_of(path,argv[1],"saved.wav");assert(hal_rec_start_exclusive(path)==1&&hal_rec_status()==1&&!hal_rec_start_exclusive(path));
 advance(1200);hal_rec_stop();wait_record(3);assert(hal_rec_result()==HAL_REC_DONE&&!hal_rec_active()&&hal_rec_ms()==1200);
 assert(!stat(path,&info)&&info.st_size==44+38400);FILE *file=fopen(path,"rb");assert(file);uint8_t header[44];
 assert(fread(header,1,44,file)==44&&!fclose(file));uint32_t bytes;memcpy(&bytes,header+40,4);assert(bytes==38400&&!memcmp(header,"RIFF",4));
 path_of(path,argv[1],"late.wav");snprintf(part,sizeof part,"%s.part",path);assert(hal_rec_start_exclusive(path)==1);wait_path(part);create(path);
 advance(100);hal_rec_stop();wait_record(4);assert(hal_rec_result()==HAL_REC_EXISTS&&!stat(path,&info)&&info.st_size==7);
 path_of(path,argv[1],"shutdown.wav");assert(hal_rec_start_exclusive(path)==1);advance(250);hal_ext_shutdown_begin();hal_ext_shutdown_begin();
 assert(!hal_rec_start_exclusive(path)&&!hal_audio_play_file(path)&&!hal_lte_sms_send_request("+911234567890","blocked"));
 hal_ext_shutdown_cancel();wait_record(3);assert(exists(path)&&!hal_ext_shutdown_poll());
 path_of(path,argv[1],"after-cancel.wav");assert(hal_rec_start_exclusive(path)==1);advance(100);hal_rec_stop();wait_record(3);
 hal_gps_enable(1);advance(4500);hal_gps resumed;hal_gps_get(&resumed);assert(resumed.valid);
 hal_ext_shutdown_begin();assert(!hal_rec_start_exclusive(path));
 hal_gps_enable(1);hal_hr_enable(1);hal_lte_enable(1);hal_light_gesture_enable(1);
 DWORD began=GetTickCount();int status;while((status=hal_ext_shutdown_poll())==0){assert(GetTickCount()-began<3000);Sleep(1);}
 assert(status==1&&hal_rec_status()==3&&exists(path));assert(!hal_env_get(&e)&&!hal_light_get(&l));
 hal_gps gps;hal_hr heart;hal_lte lte;hal_gps_get(&gps);hal_hr_get(&heart);hal_lte_get(&lte);
 assert(!gps.valid&&!heart.bpm&&!heart.quality&&!lte.powered);
 hal_display_power(0);assert(hal_display_mode()==0);hal_display_power(2);assert(hal_display_mode()==2);
 puts("PASS simulator: truthful synthetic sensor ages, absent modules, stable SMS IDs and queued/sending/result states, async exclusive recording/finalization/collisions and shutdown worker drain");
 return 0;
}
'''

@unittest.skipUnless(shutil.which('gcc') and os.name=='nt','Windows GCC required')
class ExternalSimulatorContracts(unittest.TestCase):
    def test_lifecycle(self):
        OUT.mkdir(parents=True,exist_ok=True)
        source=OUT/'lifecycle.c';source.write_text(SOURCE)
        env=os.environ|{key:str(OUT) for key in ('TEMP','TMP','TMPDIR')}
        exe=OUT/'lifecycle.exe'
        command=['gcc','-O2','-std=c99','-Wall','-Wextra','-Werror','-Icore','-Inos','-Isim',
                 str(source),'sim/nos_ext_sim.c','nos/voice_policy.c','nos/voice_history.c','-lwinmm','-lm','-o',str(exe)]
        result=subprocess.run(command,cwd=ROOT,env=env,capture_output=True,text=True,timeout=60)
        (OUT/'compile.log').write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        sd=tempfile.mkdtemp(prefix='sd-',dir=OUT)
        result=subprocess.run([str(exe),sd],cwd=ROOT,env=env,capture_output=True,text=True,timeout=30)
        (OUT/'lifecycle.log').write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        print(result.stdout)

if __name__=='__main__':unittest.main()
