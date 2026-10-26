/* Included after the Wi-Fi scan cache. HTTP work never mutates UI preferences. */
#include "setup_protocol.h"
#include "esp_random.h"
static portMUX_TYPE s_setup_lock=portMUX_INITIALIZER_UNLOCKED;
static volatile int s_setup_active, s_credentials_dirty;
static char s_setup_ssid[33]="Watch-Setup", s_setup_pass[24];
static char s_wifi_message[96]="Not connected";
typedef struct { int brightness,auto_bright,dnd,aod; } setup_settings;
static setup_settings s_setup_pending, s_setup_current;
static int s_setup_have_pending;
static int s_setup_sd, s_setup_settings_pending;
static void wifi_message(const char *text)
{
    portENTER_CRITICAL(&s_setup_lock);
    snprintf(s_wifi_message,sizeof s_wifi_message,"%s",text);
    portEXIT_CRITICAL(&s_setup_lock);
}
void hal_wifi_info(char *text,int n)
{
    portENTER_CRITICAL(&s_setup_lock);
    if(s_wifi_enabled && !s_wifi_initialized)
        snprintf(text,n,"Wi-Fi deferred / stage %d error %d / retries %u",atomic_load(&s_wifi_startup.stage),
                 atomic_load(&s_wifi_startup.error),atomic_load(&s_wifi_startup.failures));
    else snprintf(text,n,"%s",s_wifi_message);
    portEXIT_CRITICAL(&s_setup_lock);
}
void hal_setup_info(char *ssid,int sn,char *password,int pn,int *active)
{
    portENTER_CRITICAL(&s_setup_lock);
    snprintf(ssid,sn,"%s",s_setup_ssid);snprintf(password,pn,"%s",s_setup_pass);*active=s_setup_active;
    portEXIT_CRITICAL(&s_setup_lock);
}
void hal_setup_poll(void)
{
    radio_poll(); /* UI INTERNAL stack owns optional initialization/retries. */
    setup_settings patch; int apply;
    portENTER_CRITICAL(&s_setup_lock);
    apply=s_setup_have_pending;patch=s_setup_pending;s_setup_have_pending=0;
    portEXIT_CRITICAL(&s_setup_lock);
    if(apply) {
        nos_prefs.brightness=(uint8_t)patch.brightness;
        nos_prefs.auto_bright=(uint8_t)patch.auto_bright;
        nos_prefs.dnd=(uint8_t)patch.dnd;nos_prefs.aod=(uint8_t)patch.aod;
        prefs_save();
    }
    hal_storage storage;hal_storage_get(&storage);
    portENTER_CRITICAL(&s_setup_lock);
    s_setup_current=(setup_settings){nos_prefs.brightness,nos_prefs.auto_bright,nos_prefs.dnd,nos_prefs.aod};
    s_setup_sd=storage.mounted;
    s_setup_settings_pending=prefs_pending();
    portEXIT_CRITICAL(&s_setup_lock);
}
static esp_err_t setup_json(httpd_req_t *req,cJSON *json)
{
    if(!json) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Out of memory");
    char *text=cJSON_PrintUnformatted(json);cJSON_Delete(json);
    if(!text) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Out of memory");
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    esp_err_t rc=httpd_resp_sendstr(req,text);free(text);return rc;
}
static int setup_authorized(httpd_req_t *req)
{
    if(!s_setup_active) { httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Start Phone setup on the watch");return 0; }
    /* Browser mutations require JSON: cross-origin forms cannot submit it and
     * the server grants no CORS/preflight permissions. */
    if(req->method==HTTP_POST) {
        char type[64];
        if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof type)!=ESP_OK ||
           strcmp(type,"application/json")) {
            httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Expected application/json");return 0;
        }
    }
    return 1;
}
static cJSON *setup_body(httpd_req_t *req)
{
    char body[512];
    if(req->content_len<=0 || req->content_len>=sizeof body) return NULL;
    size_t used=0;
    while(used<req->content_len) {
        int n=httpd_req_recv(req,body+used,req->content_len-used);
        if(n<=0) return NULL;
        used+=n;
    }
    body[used]=0;
    cJSON *json=cJSON_ParseWithLengthOpts(body,(size_t)used+1,NULL,1);
    memset(body,0,sizeof body);
    return json;
}
static esp_err_t web_root(httpd_req_t *req)
{
    if(!setup_authorized(req)) return ESP_OK;
    extern const char page[] asm("_binary_watch_setup_html_start");
    httpd_resp_set_type(req,"text/html");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    return httpd_resp_sendstr(req,page);
}
static esp_err_t web_wifi(httpd_req_t *req)
{
    if(!setup_authorized(req)) return ESP_OK;
    cJSON *j=setup_body(req);
    const cJSON *ssid=cJSON_GetObjectItemCaseSensitive(j,"ssid"),*pass=cJSON_GetObjectItemCaseSensitive(j,"pass");
    if(!cJSON_IsString(ssid)||!cJSON_IsString(pass)||!setup_credentials_valid(ssid->valuestring,pass->valuestring)) {
        cJSON_Delete(j);return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"SSID: 1-32 bytes. Password: 8-63 bytes, or empty for open Wi-Fi.");
    }
    hal_wifi_connect(ssid->valuestring,pass->valuestring);
    memset(pass->valuestring,0,strlen(pass->valuestring));cJSON_Delete(j);
    httpd_resp_set_type(req,"application/json");
    return httpd_resp_sendstr(req,"{\"accepted\":true}");
}
static esp_err_t web_networks(httpd_req_t *req)
{
    if(!setup_authorized(req)) return ESP_OK;
    hal_wifi_ap aps[32];int n=hal_wifi_scan(aps,32);
    cJSON *j=cJSON_CreateObject(),*list=cJSON_AddArrayToObject(j,"networks");
    cJSON_AddBoolToObject(j,"scanning",s_scan_busy);
    for(int i=0;i<n;i++) {
        cJSON *a=cJSON_CreateObject();cJSON_AddStringToObject(a,"ssid",aps[i].ssid);
        cJSON_AddNumberToObject(a,"rssi",aps[i].rssi);cJSON_AddItemToArray(list,a);
    }
    return setup_json(req,j);
}
static esp_err_t web_status(httpd_req_t *req)
{
    if(!setup_authorized(req)) return ESP_OK;
    setup_settings s;int sd,pending;char wifi[96];hal_wifi_info(wifi,sizeof wifi);
    portENTER_CRITICAL(&s_setup_lock);s=s_setup_current;sd=s_setup_sd;pending=s_setup_settings_pending;portEXIT_CRITICAL(&s_setup_lock);
    hal_power p;hal_power_get(&p);
    cJSON *j=cJSON_CreateObject();
    cJSON_AddStringToObject(j,"wifi",wifi);
    cJSON_AddNumberToObject(j,"battery",p.battery_percent);
    cJSON_AddNumberToObject(j,"external_power",p.external_power);
    cJSON_AddNumberToObject(j,"uptime",millis_raw()/1000);
    cJSON_AddNumberToObject(j,"psram_free",heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddBoolToObject(j,"phone",g_ble_conns>0);
    cJSON_AddBoolToObject(j,"sd",sd);
    cJSON_AddBoolToObject(j,"credentials_pending",s_credentials_dirty);
    cJSON_AddBoolToObject(j,"settings_pending",pending);
    cJSON_AddNumberToObject(j,"brightness",s.brightness);
    cJSON_AddNumberToObject(j,"auto_bright",s.auto_bright);
    cJSON_AddNumberToObject(j,"dnd",s.dnd);cJSON_AddNumberToObject(j,"aod",s.aod);
    return setup_json(req,j);
}
static esp_err_t web_settings(httpd_req_t *req)
{
    if(!setup_authorized(req)) return ESP_OK;
    cJSON *j=setup_body(req),*b=cJSON_GetObjectItemCaseSensitive(j,"brightness"),
          *a=cJSON_GetObjectItemCaseSensitive(j,"auto_bright"),
          *d=cJSON_GetObjectItemCaseSensitive(j,"dnd"),*o=cJSON_GetObjectItemCaseSensitive(j,"aod");
    if(!cJSON_IsNumber(b)||b->valuedouble<1||b->valuedouble>100||b->valuedouble!=b->valueint||
       !cJSON_IsBool(a)||!cJSON_IsBool(d)||!cJSON_IsBool(o)) {
        cJSON_Delete(j);return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid settings");
    }
    setup_settings patch={b->valueint,cJSON_IsTrue(a),cJSON_IsTrue(d),cJSON_IsTrue(o)};
    portENTER_CRITICAL(&s_setup_lock);
    s_setup_pending=patch;s_setup_have_pending=1;
    portEXIT_CRITICAL(&s_setup_lock);
    cJSON_Delete(j);httpd_resp_set_type(req,"application/json");
    return httpd_resp_sendstr(req,"{\"queued\":true}");
}
