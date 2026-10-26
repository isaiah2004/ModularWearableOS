#include "hal_wave2_common.inc"
#include "../firmware/main/hal_esp32.c"

/* Strictly mocked SDK calls below; production functions above execute intact. */
static int mock_adv,mock_disc,mock_initiating,mock_adv_starts,mock_disc_starts,mock_connects;
static uint16_t mock_terminated[32];static unsigned mock_terminated_n;
static int mock_stop_failure;
static int mock_event_puts, mock_callout_fail,mock_callout_init_fail;
static struct ble_npl_eventq mock_eventq;
struct mock_ble_hs_cfg ble_hs_cfg;
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void) {return &mock_eventq;}
void ble_npl_event_init(struct ble_npl_event *e,void (*fn)(struct ble_npl_event*),void *arg)
{e->fn=fn;e->arg=arg;e->queued=0;}
void ble_npl_eventq_put(struct ble_npl_eventq *q,struct ble_npl_event *e) {mock_event_puts++;e->queued=1;}
int ble_npl_callout_init(struct ble_npl_callout *c,struct ble_npl_eventq *q,void (*fn)(struct ble_npl_event*),void *arg)
{if(mock_callout_init_fail){mock_callout_init_fail--;return BLE_HS_ENOMEM;}ble_npl_event_init(&c->event,fn,arg);return 0;}
int ble_npl_callout_reset(struct ble_npl_callout *c,uint32_t ticks)
{assert(ticks==100);if(mock_callout_fail) {mock_callout_fail--;return 71;}return 0;}
uint32_t ble_npl_time_ms_to_ticks32(uint32_t ms) {return ms;}
static void drain(void)
{
    int limit=32;
    ble_control_tick(NULL); /* One scheduled host-callout delivery. */
    while(s_ble_control_event.queued) {assert(limit--);s_ble_control_event.queued=0;s_ble_control_event.fn(&s_ble_control_event);}
}
int ble_gap_adv_active(void) {return mock_adv;}
int ble_gap_disc_active(void) {return mock_disc;}
int ble_gap_conn_active(void) {return mock_initiating;}
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *f) {return 0;}
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *f) {return 0;}
int ble_gap_adv_start(uint8_t t,const ble_addr_t *a,int32_t ms,const struct ble_gap_adv_params *p,int (*fn)(struct ble_gap_event*,void*),void *arg)
{assert(atomic_load(&s_ble_enabled));mock_adv=1;mock_adv_starts++;return 0;}
int ble_gap_disc(uint8_t t,int32_t ms,const struct ble_gap_disc_params *p,int (*fn)(struct ble_gap_event*,void*),void *arg)
{assert(atomic_load(&s_ble_enabled));mock_disc=1;mock_disc_starts++;return 0;}
int ble_gap_adv_stop(void) {if(mock_stop_failure) {mock_stop_failure--;return 77;}mock_adv=0;return 0;}
int ble_gap_disc_cancel(void) {mock_disc=0;return 0;}
int ble_gap_conn_cancel(void) {mock_initiating=0;return 0;}
int ble_gap_connect(uint8_t t,const ble_addr_t *peer,int32_t ms,const void *p,int (*fn)(struct ble_gap_event*,void*),void *arg)
{assert(atomic_load(&s_ble_enabled));mock_initiating=1;mock_connects++;return 0;}
int ble_gap_terminate(uint16_t h,uint8_t reason)
{assert(mock_terminated_n<32);mock_terminated[mock_terminated_n++]=h;return 0;}
int ble_gap_conn_find(uint16_t h,struct ble_gap_conn_desc *d) {memset(d,0,sizeof *d);return 0;}
int ble_gap_update_params(uint16_t h,const struct ble_gap_upd_params *p) {return 0;}
int ble_gap_unpair(const ble_addr_t *p) {return 0;}
int ble_hs_id_set_rnd(const uint8_t *p) {return 0;}
int ble_hs_util_ensure_addr(int t) {return 0;}
int ble_hs_id_infer_auto(int t,uint8_t *out) {*out=BLE_OWN_ADDR_RANDOM;return 0;}
int ble_hs_id_copy_addr(uint8_t t,uint8_t *out,int *z) {memset(out,0,6);return 0;}
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *f,const uint8_t *data,uint8_t n) {memset(f,0,sizeof *f);return 0;}
int ble_hs_mbuf_to_flat(const struct os_mbuf *m,void *p,uint16_t cap,uint16_t *got)
{if(m->size>cap) return 1;*got=m->size;memcpy(p,m->data,m->size);return 0;}
void ble_xfer_disconnected(void) {}
int board_clock_valid(void) {return 0;}

struct mock_timer {esp_timer_create_args_t args;int started;};
static struct mock_timer mock_air_timer;
static int fail_timer_create,fail_timer_start,fail_filter,fail_rx_cb,fail_promisc,fail_channel_get,fail_channel_set,fail_cleanup;
static int mock_promisc,mock_wifi_joins,mock_wifi_drops,mock_channel_sets;
static uint8_t mock_channel=6;
static wifi_promiscuous_cb_t mock_rx_cb;
static wifi_ap_record_t mock_aps[8];static uint16_t mock_ap_count;
static wifi_cred_t mock_credentials;
static wifi_config_t mock_wifi_config;
static int take_failure(int *p) {if(*p) {(*p)--;return ESP_FAIL;}return ESP_OK;}
esp_err_t esp_timer_create(const esp_timer_create_args_t *a,esp_timer_handle_t *out)
{if(take_failure(&fail_timer_create)) return ESP_FAIL;mock_air_timer.args=*a;*out=&mock_air_timer;return ESP_OK;}
esp_err_t esp_timer_start_periodic(esp_timer_handle_t h,uint64_t us)
{assert(h && us==AIR_DWELL_MS*1000);if(take_failure(&fail_timer_start)) return ESP_FAIL;h->started=1;return ESP_OK;}
esp_err_t esp_timer_start_once(esp_timer_handle_t h,uint64_t us) {assert(h);h->started=1;return ESP_OK;}
esp_err_t esp_timer_stop(esp_timer_handle_t h) {assert(h);h->started=0;return ESP_OK;}
esp_err_t esp_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *f) {return take_failure(&fail_filter);}
esp_err_t esp_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb_t fn)
{if(take_failure(&fail_rx_cb)) return ESP_FAIL;mock_rx_cb=fn;return ESP_OK;}
esp_err_t esp_wifi_set_promiscuous(bool on)
{if(take_failure(on?&fail_promisc:&fail_cleanup)) return ESP_FAIL;mock_promisc=on;return ESP_OK;}
esp_err_t esp_wifi_get_channel(uint8_t *ch,wifi_second_chan_t *second)
{if(take_failure(&fail_channel_get)) return ESP_FAIL;*ch=mock_channel;*second=0;return ESP_OK;}
esp_err_t esp_wifi_set_channel(uint8_t ch,wifi_second_chan_t second)
{mock_channel_sets++;if(take_failure(&fail_channel_set)) return ESP_FAIL;mock_channel=ch;return ESP_OK;}
esp_err_t esp_wifi_connect(void) {mock_wifi_joins++;return ESP_OK;}
esp_err_t esp_wifi_disconnect(void) {mock_wifi_drops++;return ESP_OK;}
esp_err_t esp_wifi_scan_stop(void) {return ESP_OK;}
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *c,bool wait) {return ESP_OK;}
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *n) {*n=mock_ap_count;return ESP_OK;}
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n,wifi_ap_record_t *out)
{assert(*n<=mock_ap_count);memcpy(out,mock_aps,*n*sizeof *out);return ESP_OK;}
esp_err_t esp_wifi_clear_ap_list(void) {return ESP_OK;}
esp_err_t esp_wifi_set_config(int iface,const wifi_config_t *c) {mock_wifi_config=*c;return ESP_OK;}
int xQueueOverwrite(QueueHandle_t q,const void *c) {assert(q);mock_credentials=*(const wifi_cred_t*)c;return pdTRUE;}

static int cover_ble(uint16_t conn,uint32_t offset,size_t n,uint8_t value)
{
    struct os_mbuf m={0};assert(n<=sizeof m.data-4);m.size=(uint16_t)(n+4);
    memcpy(m.data,&offset,4);memset(m.data+4,value,n);
    struct ble_gatt_access_ctxt ctx={.op=BLE_GATT_ACCESS_OP_WRITE_CHR,.om=&m};
    return media_cover_access(conn,0,&ctx,NULL);
}
static void ble_fill(uint16_t conn,uint8_t value)
{
    for(size_t off=0;off<COVER_BYTES;) {
        size_t n=COVER_BYTES-off;if(n>500) n=500;
        assert(!cover_ble(conn,(uint32_t)off,n,value));off+=n;
    }
}
static void assert_image(uint8_t value,unsigned expected)
{
    unsigned gen=0;int px=0;const uint8_t *p=(const uint8_t*)hal_media_cover_acquire(&px,&gen);
    assert(p && px==250 && gen==expected);
    for(size_t i=0;i<COVER_BYTES;i++) assert(p[i]==value);
    hal_media_cover_release((const uint16_t*)p);mock_canaries();
}
static void test_cover(void)
{
    size_t invalid[]={0,1,COVER_BYTES-1,COVER_BYTES+1,INT_MAX,SIZE_MAX};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++) {
        httpd_req_t r={.content_len=invalid[i]};int before=mock_alloc_count;
        web_cover(&r);assert(r.response==400 && !r.recv_calls && mock_alloc_count==before);
    }
    assert(cover_ble(1,UINT32_MAX,0,0)!=0 && s_cover_active==-1);
    assert(cover_ble(1,0,0,0)!=0 && !s_cover_owner);
    mock_alloc_fail=1;assert(cover_ble(1,0,16,1)!=0 && !s_cover_owner);
    httpd_req_t r={.content_len=COVER_BYTES};web_cover(&r);
    assert(r.response==200 && r.max_recv<=4096 && s_cover_active==0);assert_image(0x5C,1);
    mock_alloc_fail=1;r=(httpd_req_t){.content_len=COVER_BYTES};web_cover(&r);
    assert(r.response==500 && !r.recv_calls);assert_image(0x5C,1);
    for(int fault=0;fault<3;fault++) {
        r=(httpd_req_t){.content_len=COVER_BYTES};
        if(fault==0) mock_recv_stop=3000;
        if(fault==1) mock_recv_stall=1;
        if(fault==2) mock_recv_oversize=1;
        web_cover(&r);assert(r.response==408);assert_image(0x5C,1);
        mock_recv_stop=-1;mock_recv_stall=mock_recv_oversize=mock_alloc_fail=0;
    }
    assert(!cover_ble(1,0,12,2));assert(cover_ble(2,12,12,3)!=0);
    assert(cover_ble(1,24,12,2)!=0 && !s_cover_owner);assert_image(0x5C,1);
    assert(!cover_ble(1,0,12,2));atomic_fetch_add(&mock_us,15000000);
    assert(cover_ble(1,12,12,2)!=0 && !s_cover_owner);
    assert(!cover_ble(1,0,12,2));atomic_fetch_add(&mock_us,15000001);
    r=(httpd_req_t){.content_len=COVER_BYTES};web_cover(&r);assert(r.response==200);assert_image(0x5C,2);
    const uint16_t *old=hal_media_cover_acquire(NULL,NULL);
    ble_fill(1,0x31);assert(!cover_ble(1,UINT32_MAX,0,0));assert_image(0x31,3);
    for(size_t i=0;i<COVER_BYTES;i++) assert(((const uint8_t*)old)[i]==0x5C);
    r=(httpd_req_t){.content_len=COVER_BYTES};web_cover(&r);assert(r.response==500 && !r.recv_calls);assert_image(0x31,3);
    hal_media_cover_release(old);r=(httpd_req_t){.content_len=COVER_BYTES};web_cover(&r);assert_image(0x5C,4);
    old=hal_media_cover(NULL,NULL);assert(s_cover_readers[s_cover_active]==1);hal_media_cover_release(old);
    assert(!cover_ble(1,0,500,0x88));assert(cover_ble(1,UINT32_MAX,0,0)!=0);assert_image(0x5C,4);
    ble_fill(1,0x22);assert(cover_ble(1,UINT32_MAX,1,0)!=0);assert_image(0x5C,4);
    assert(xSemaphoreTake(s_cover_writer,0)==pdTRUE);r=(httpd_req_t){.content_len=COVER_BYTES};web_cover(&r);
    assert(r.response==500 && !r.recv_calls && cover_ble(1,0,8,1)!=0);xSemaphoreGive(s_cover_writer);assert_image(0x5C,4);
    puts("PASS PLAT-001: actual HTTP/BLE receive bounds, OOM, truncated/oversized/timeouts, sequential offsets, connection ownership, empty/extra commit rejection, reader pin lifetime and writer exclusion");
}
static void test_ble_off(void)
{
    hal_ble_set_enabled(0);assert(!mock_adv && !s_ble_initialized);
    ble_npl_event_init(&s_ble_control_event,ble_control_tick,NULL);
    ble_npl_callout_init(&s_ble_control_callout,&mock_eventq,ble_control_tick,NULL);s_ble_initialized=1;ble_on_sync();drain();
    assert(!mock_adv_starts && atomic_load(&s_ble_rf_off));
    hal_ble_dev output[2];assert(!hal_ble_scan(output,2));assert(!hal_ble_connect_device("11:22:33:44:55:66"));
    hal_ble_set_enabled(1);assert(!mock_adv);drain();assert(mock_adv);
    s_ble_records=calloc(1,sizeof *s_ble_records);s_ble_mtx=xSemaphoreCreateMutex();assert(s_ble_records);
    strcpy(s_ble_tab[0].addr,"11:22:33:44:55:66");s_ble_count=1;s_ble_seen[0]=millis_raw();
    assert(hal_ble_scan(output,2)==1);drain();assert(mock_disc);
    assert(hal_ble_connect_device(output[0].addr));assert(!mock_initiating);drain();assert(mock_initiating && !mock_disc);
    ble_handle_add(42);ble_handle_add(511);g_ble_conns=2;g_med_conn=511;g_nus_sub=1;
    hal_ble_set_enabled(0);assert(mock_adv);assert(!hal_phone_connected() && hal_status(HAL_FEAT_BLE)==HAL_CONN_OFF);drain();
    assert(!mock_adv && !mock_disc && !mock_initiating && !s_central_pending && mock_terminated_n==2);
    assert(mock_terminated[0]==42 && mock_terminated[1]==511);
    struct ble_gap_event ev={.type=BLE_GAP_EVENT_DISCONNECT};ev.disconnect.conn.conn_handle=42;peri_gap_event(&ev,NULL);
    ev=(struct ble_gap_event){.type=BLE_GAP_EVENT_CONNECT,.connect={.status=0,.conn_handle=53}};
    central_event(&ev,NULL);assert(mock_terminated[mock_terminated_n-1]==53);
    char linked[160];hal_ble_status(linked,sizeof linked);assert(strstr(linked,"conns=2"));
    ev=(struct ble_gap_event){.type=BLE_GAP_EVENT_DISCONNECT};ev.disconnect.conn.conn_handle=53;central_event(&ev,NULL);
    ev.disconnect.conn.conn_handle=511;peri_gap_event(&ev,NULL);drain();
    assert(!s_ble_handles_n && atomic_load(&s_ble_rf_off));int starts=mock_adv_starts,scans=mock_disc_starts;
    ev.type=BLE_GAP_EVENT_ADV_COMPLETE;peri_gap_event(&ev,NULL);ev.type=BLE_GAP_EVENT_DISC_COMPLETE;ble_gap_event(&ev,NULL);
    ble_on_reset(99);ble_on_sync();drain();hal_ble_scan(output,2);drain();
    assert(mock_adv_starts==starts && mock_disc_starts==scans && !mock_disc && !mock_adv);
    hal_ble_set_enabled(1);drain();assert(mock_adv && !mock_disc);
    mock_stop_failure=1;hal_ble_set_enabled(0);drain();assert(mock_adv && !atomic_load(&s_ble_rf_off));ble_control_schedule();drain();assert(!mock_adv && atomic_load(&s_ble_rf_off));
    char text[160];hal_ble_status(text,sizeof text);assert(strstr(text,"radio off") && strstr(text,"stack retained"));
    hal_ble_set_enabled(1);drain();assert(mock_adv);mock_callout_fail=1;drain();assert(!mock_adv && !s_ble_control_clock_ready);
    hal_ble_status(text,sizeof text);assert(strstr(text,"unavailable"));drain();assert(mock_adv && s_ble_control_clock_ready);
    hal_ble_set_enabled(0);drain();assert(mock_event_puts==0); /* UI actions never invoke a potentially blocking NPL put. */
    puts("PASS PLAT-012: asynchronous logical Off stops advertising/discovery/initiating/all actual handles; late connect/reset/scan/advertising callbacks cannot restart RF; cleanup retry and re-enable");
}
static void test_ssid(void)
{
    s_scan_cache=calloc(1,sizeof *s_scan_cache);assert(s_scan_cache);mock_ap_count=4;
    memset(mock_aps,0,sizeof mock_aps);memset(mock_aps[0].ssid,'X',33);mock_aps[1]=mock_aps[0];mock_aps[1].ssid[31]='Y';mock_aps[2]=mock_aps[0];
    wifi_scan_collect();assert(s_ap_n==3 && strlen(s_aps[0].ssid)==32 && strlen(s_aps[1].ssid)==32);
    assert(s_aps[0].ssid[31]=='X' && s_aps[1].ssid[31]=='Y' && s_aps[0].ssid[32]==0 && !strcmp(s_aps[2].ssid,"(hidden)"));
    s_wifi_requests=(void*)1;hal_wifi_connect(s_aps[1].ssid,"12345678");assert(strlen(mock_credentials.ssid)==32 && mock_credentials.ssid[31]=='Y');
    assert(wifi_configure(&mock_credentials)==ESP_OK && !memcmp(mock_wifi_config.sta.ssid,s_aps[1].ssid,32));
    assert(!memcmp(mock_wifi_config.sta.password,"12345678",8) && mock_wifi_config.sta.password[8]==0);
    puts("PASS PLAT-011: actual scan dedup/terminators preserve all 32 bytes including differing final bytes; queued connect and SDK config use the exact selected SSID");
}
static void reset_air(void)
{
    assert(!s_air_reserved);s_air_on=0;s_air_stop_requested=0;s_air_hop_pending=0;
    s_air_timer=NULL;s_wifi_enabled=1;s_wifi_worker_ready=1;s_setup_active=0;s_wifi_want=1;
    fail_timer_create=fail_timer_start=fail_filter=fail_rx_cb=fail_promisc=fail_channel_get=fail_channel_set=fail_cleanup=0;
    mock_promisc=mock_wifi_joins=mock_wifi_drops=mock_channel_sets=0;mock_channel=6;mock_rx_cb=NULL;s_freeze=0;
}
static void test_air(void)
{
    for(int fault=0;fault<7;fault++) {
        reset_air();
        if(fault==0) fail_timer_create=1;
        if(fault==1) fail_timer_start=1;
        if(fault==2) fail_filter=1;
        if(fault==3) fail_rx_cb=1;
        if(fault==4) fail_promisc=1;
        if(fault==5) fail_channel_get=1;
        if(fault==6) fail_channel_set=1;
        assert(!hal_wifi_air_start());assert(!s_air_on && !s_air_reserved && s_wifi_want && !mock_promisc && !mock_rx_cb && mock_channel==6);
        assert(mock_wifi_joins==(fault==0 || fault==5?0:1));
    }
    reset_air();s_wifi_enabled=0;assert(!hal_wifi_air_start() && !mock_wifi_drops && !mock_promisc);
    reset_air();s_wifi_worker_ready=0;assert(!hal_wifi_air_start());
    reset_air();assert(hal_wifi_air_start() && s_air_on && mock_promisc && mock_channel==1 && !s_wifi_want);
    wifi_promiscuous_pkt_t packet={.rx_ctrl={.sig_len=2400,.rssi=-51}};air_rx(&packet,WIFI_PKT_DATA);
    int calls=mock_channel_sets;air_hop(NULL);assert(mock_channel_sets==calls);air_supervise();assert(mock_channel==2 && s_air_view[0].data==1);
    s_freeze=1;s_frozen_ms=1;atomic_fetch_add(&mock_us,3000001);air_supervise();
    assert(!s_air_reserved && !s_air_on && !mock_promisc && s_wifi_want && mock_channel==6);s_freeze=0;
    reset_air();assert(hal_wifi_air_start());fail_cleanup=1;hal_wifi_air_stop();assert(s_air_reserved && s_air_stop_requested);
    air_supervise();assert(!s_air_reserved && !mock_promisc && s_wifi_want);
    reset_air();assert(hal_wifi_air_start());fail_channel_set=1;air_hop(NULL);air_supervise();assert(!s_air_reserved && mock_channel==6);
    reset_air();assert(hal_wifi_air_start());s_wifi_enabled=0;air_supervise();assert(!s_air_reserved && !s_air_on && !mock_promisc && !mock_wifi_joins);
    calls=mock_channel_sets;air_hop(NULL);air_supervise();assert(mock_channel_sets==calls);
    puts("PASS PLAT-016: seven startup faults restore station/promisc/channel; worker independently expires lease with dead timer/frozen UI clock; Wi-Fi Off/unavailable admission, hop failure and cleanup retry");
}
int main(void)
{
    s_cover_writer=xSemaphoreCreateMutexStatic(&s_cover_writer_storage);
    s_air_mutex=xSemaphoreCreateMutexStatic(&s_air_mutex_storage);
    test_cover();test_ble_off();test_ssid();test_air();mock_canaries();
    puts("PASS actual hal_esp32.c fault suite");return 0;
}
