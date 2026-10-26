/* Shared device/simulator serial validation. A positive result acknowledges a
 * bounded request, not asynchronous modem/file/hardware completion. */
static int ext_debug_integer(const char *text,int low,int high,int *out)
{
    char *end;errno=0;long value=strtol(text,&end,10);
    if(end==text||errno)return 0;
    while(*end==' ')end++;
    if(*end||value<low||value>high)return 0;
    *out=(int)value;return 1;
}
static void ext_debug_help(void)
{
    printf("EXT cmds: ext | haptic <0-6> | sfx <0-13> | vol <0-100> | play <path> | rec <path> | recstop |"
           " gps on/off | lte on/off | lte at <cmd> | lte sms <num> <text> | hr on/off | gest on/off |"
           " mag cal/done/map <+x+y+z> [<fwd>] | sea <hPa>\n");
}
/* -1 status, 0 rejected, 1 accepted. Backend helpers supply typed acceptance. */
static int ext_debug_dispatch(const char *cmd)
{
    if(!cmd)cmd="";
    while(*cmd==' ')cmd++;
    if(!strncmp(cmd,"ext",3)&&(!cmd[3]||cmd[3]==' '))cmd+=3;
    while(*cmd==' ')cmd++;
    if(!*cmd||!strcmp(cmd,"status"))return -1;
    if(!strcmp(cmd,"help")){ext_debug_help();return 1;}
    /* Stops and status are still useful after shutdown begins. */
    if(!strcmp(cmd,"recstop")){hal_rec_stop();return 1;}
    if(!strcmp(cmd,"gps off")){hal_gps_enable(0);return 1;}
    if(!strcmp(cmd,"lte off")){hal_lte_enable(0);return 1;}
    if(!strcmp(cmd,"hr off")){hal_hr_enable(0);return 1;}
    if(!strcmp(cmd,"gest off")){hal_light_gesture_enable(0);return 1;}
    if(EXT_DEBUG_STOPPING())return 0;
    int value;
    if(!strncmp(cmd,"haptic ",7)){
        if(!ext_debug_integer(cmd+7,0,HAPTIC_HEARTBEAT,&value)||!(hal_ext_present()&EXT_HAPTIC))return 0;
        hal_haptic(value);return 1;
    }
    if(!strncmp(cmd,"sfx ",4)){
        if(!ext_debug_integer(cmd+4,0,SFX_COUNT-1,&value)||!(hal_ext_present()&EXT_SPEAKER))return 0;
        hal_sfx(value);return 1;
    }
    if(!strncmp(cmd,"vol ",4)){
        if(!ext_debug_integer(cmd+4,0,100,&value)||!(hal_ext_present()&EXT_SPEAKER))return 0;
        hal_audio_volume(value);return 1;
    }
    if(!strncmp(cmd,"play ",5)){int accepted=hal_audio_play_file(cmd+5);printf("EXT play %d\n",accepted);return accepted>0;}
    if(!strncmp(cmd,"rec ",4)){int accepted=hal_rec_start_exclusive(cmd+4);printf("EXT rec %d\n",accepted);return accepted==1;}
    if(!strcmp(cmd,"gps on")){hal_gps_enable(1);return 1;}
    if(!strcmp(cmd,"lte on")){hal_lte_enable(1);return 1;}
    if(!strncmp(cmd,"lte ",4))return EXT_DEBUG_LTE(cmd+4);
    if(!strcmp(cmd,"hr on")){if(!(hal_ext_present()&EXT_HR))return 0;hal_hr_enable(1);return 1;}
    if(!strcmp(cmd,"gest on")){if(!(hal_ext_present()&EXT_LIGHT))return 0;hal_light_gesture_enable(1);return 1;}
    if(!strncmp(cmd,"mag ",4))return EXT_DEBUG_MAG(cmd+4);
    if(!strncmp(cmd,"sea ",4)){
        char *end;errno=0;float pressure=strtof(cmd+4,&end);while(*end==' ')end++;
        if(end==cmd+4||*end||errno||!isfinite(pressure)||pressure<800||pressure>1200||!(hal_ext_present()&EXT_ENV))return 0;
        hal_env_set_sea_level(pressure);return 1;
    }
    ext_debug_help();return 0;
}
#undef EXT_DEBUG_STOPPING
#undef EXT_DEBUG_LTE
#undef EXT_DEBUG_MAG
