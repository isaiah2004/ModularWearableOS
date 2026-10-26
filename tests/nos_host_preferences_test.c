#include "nos.h"
#include "prefs.h"
#include "wake_service.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
nos_prefs_t nos_prefs;
static wake_info current;
static int loading,save_ok=1,ble_value=-1,wifi_value=-1;
static unsigned setter_calls,learn_calls,ble_calls,wifi_calls;
static uint32_t now;
int g_tz_offset_min;
static int calendar_ready,timezone_applied,timezone_calls;
static void board_apply_timezone(void){calendar_ready=0;timezone_applied=g_tz_offset_min;timezone_calls++;}
static void board_calendar_ready(int ready){if(ready)assert(timezone_applied==nos_prefs.tz_min);calendar_ready=ready;}
uint32_t hal_millis(void){return now;}
int prefs_loading(void){return loading;}
uint32_t prefs_wrist_revision(void){return nos_prefs.wrist_revision;}
void wake_service_get(wake_info *out){*out=current;}
int wake_service_set(const char *option,int value)
{assert(!strcmp(option,"wrist") && (value==0 || value==1));setter_calls++;if(!save_ok)return 0;current.wrist=value;current.learning=0;return 1;}
void wake_service_learn(void){learn_calls++;current.learning=1;}
void hal_ble_set_enabled(int on){assert(on==0 || on==1);ble_calls++;ble_value=on;}
void hal_wifi_set_enabled(int on){assert(on==0 || on==1);wifi_calls++;wifi_value=on;}
/* ACTUAL FUNCTIONS */
int main(void)
{
    nos_prefs.wrist_left=1;nos_prefs.wrist_revision=UINT32_MAX;current.wrist=1;
    loading=1;assert(!wake_learn_request() && !setter_calls && !learn_calls);
    loading=0;assert(wake_learn_request() && current.wrist==0 && setter_calls==1 && learn_calls==1);
    assert(wake_wrist_sync(0) && setter_calls==1); /* unchanged revision does no NVS I/O */
    nos_prefs.wrist_revision=1;assert(wake_wrist_sync(0) && setter_calls==2 && !current.learning);
    nos_prefs.wrist_left=0;nos_prefs.wrist_revision=2;save_ok=0;now=10;
    assert(!wake_learn_request() && current.wrist==0 && learn_calls==1);
    save_ok=1;assert(!wake_wrist_sync(0));now=1011;
    assert(wake_wrist_sync(0) && current.wrist==1);
    loading=1;nos_prefs.wrist_left=1;nos_prefs.wrist_revision=3;
    assert(!wake_wrist_sync(0) && current.wrist==1); /* delayed authoritative load remains unknown */
    loading=0;assert(wake_wrist_sync(0) && current.wrist==0);
    assert(wake_learn_request() && learn_calls==2 && current.wrist==0);
    nos_prefs.ble=0;nos_prefs.wifi=0;radio_preferences();assert(ble_calls==1 && ble_value==0 && wifi_calls==1 && wifi_value==0);
    radio_preferences();assert(ble_calls==1 && wifi_calls==1);
    nos_prefs.ble=1;radio_preferences();assert(ble_calls==2 && ble_value==1);
    nos_prefs.ble=0;radio_preferences();assert(ble_calls==3 && !ble_value);
    nos_prefs.wifi=1;radio_preferences();assert(wifi_calls==2 && wifi_value==1 && ble_calls==3);
    nos_prefs.ble=1;radio_preferences();assert(ble_calls==4 && ble_value==1 && wifi_calls==2);
    loading=1;radio_preferences();assert(ble_calls==5 && !ble_value && wifi_calls==3 && !wifi_value);
    radio_preferences();assert(ble_calls==5 && wifi_calls==3); /* unknown defaults cannot restart RF */
    loading=0;radio_preferences();assert(ble_calls==6 && ble_value==1 && wifi_calls==4 && wifi_value==1);
    radio_preferences();assert(ble_calls==6 && wifi_calls==4); /* apply resolved intent once */
    nos_prefs.ble=0;nos_prefs.wifi=0;radio_preferences();assert(ble_calls==7 && !ble_value && wifi_calls==5 && !wifi_value);
    loading=1;nos_prefs.ble=1;nos_prefs.wifi=1;radio_preferences();assert(ble_calls==7 && wifi_calls==5);
    loading=0;nos_prefs.ble=0;radio_preferences();assert(ble_calls==7 && !ble_value && wifi_calls==6 && wifi_value==1);
    loading=1;nos_prefs.tz_min=330;calendar_preferences();
    assert(!calendar_ready && g_tz_offset_min==330 && timezone_applied==330 && timezone_calls==1);
    loading=0;calendar_preferences();assert(calendar_ready && timezone_calls==1);
    nos_prefs.tz_min=-240;calendar_preferences();assert(calendar_ready && timezone_applied==-240 && timezone_calls==2);
    loading=1;calendar_preferences();assert(!calendar_ready);
    puts("PASS actual preferences: boot/delayed/revision wrist sync, left-to-wake mapping, checked retry before learning, BLE/WiFi boot Off and intent changes, unknown RF suppression and resolved reapply");
    return 0;
}
