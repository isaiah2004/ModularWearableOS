#include "nos_usb_diag.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#ifdef NOS_USB_DIAG_HOST
#include "nos_usb_diag_host.h"
#else
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL_SAFE(&s_mux)
#define UNLOCK() portEXIT_CRITICAL_SAFE(&s_mux)
#endif
static volatile int s_enabled;
static nos_usb_diag_stats s_stats;
static TaskHandle_t s_reply_task;
extern bool __real_usb_serial_jtag_is_connected(void);
extern int __real_usb_serial_jtag_write_bytes(const void *, size_t, TickType_t);
void nos_usb_diag_start(void) { LOCK(); s_enabled=1; UNLOCK(); }
void nos_usb_diag_reply_begin(void)
{
    if(!s_enabled || xPortInIsrContext())return;
    TaskHandle_t task=xTaskGetCurrentTaskHandle();
    LOCK();s_reply_task=task;UNLOCK();
}
void nos_usb_diag_reply_end(void)
{
    if(!s_enabled || xPortInIsrContext())return;
    TaskHandle_t task=xTaskGetCurrentTaskHandle();
    LOCK();if(s_reply_task==task)s_reply_task=NULL;UNLOCK();
}
void nos_usb_diag_snapshot(nos_usb_diag_stats *out) { LOCK(); *out=s_stats; UNLOCK(); }
void nos_usb_diag_usb_line(void) { if(s_enabled){LOCK();s_stats.usb_lines++;UNLOCK();} }
void nos_usb_diag_queue_accepted(void) { if(s_enabled){LOCK();s_stats.queue_accepted++;UNLOCK();} }
void nos_usb_diag_queue_failed(void) { if(s_enabled){LOCK();s_stats.queue_fail++;UNLOCK();} }
void nos_usb_diag_dispatched(void) { if(s_enabled){LOCK();s_stats.dispatch++;UNLOCK();} }
bool __wrap_usb_serial_jtag_is_connected(void)
{
    bool result=__real_usb_serial_jtag_is_connected();
    if(s_enabled){
        int isr=xPortInIsrContext();
        TaskHandle_t task=isr?NULL:xTaskGetCurrentTaskHandle();
        LOCK();s_stats.connected_checks++;s_stats.connected_false+=!result;
        if(!result && !isr && s_stats.guard_enabled && s_reply_task && task==s_reply_task){s_stats.connected_overrides++;result=true;}
        UNLOCK();
    }
    return result;
}
int __wrap_usb_serial_jtag_write_bytes(const void *src,size_t size,TickType_t ticks)
{
    if(!s_enabled)return __real_usb_serial_jtag_write_bytes(src,size,ticks);
    int64_t begin=esp_timer_get_time();
    int result=__real_usb_serial_jtag_write_bytes(src,size,ticks);
    uint32_t elapsed=(uint32_t)(esp_timer_get_time()-begin);
    LOCK();
    if(ticks){s_stats.wait_calls++;s_stats.wait_fail+=(result<=0 || (size_t)result!=size);s_stats.wait_us+=elapsed;if(elapsed>s_stats.wait_max_us)s_stats.wait_max_us=elapsed;}
    else{s_stats.zero_calls++;s_stats.zero_fail+=(result<=0 || (size_t)result!=size);s_stats.zero_us+=elapsed;if(elapsed>s_stats.zero_max_us)s_stats.zero_max_us=elapsed;}
    UNLOCK();return result;
}
static uint32_t crc32(const unsigned char *p,size_t n)
{
    uint32_t c=~0u;
    while(n--){c^=*p++;for(int i=0;i<8;i++)c=(c>>1)^(0xedb88320u & (0u-(c&1u)));}
    return ~c;
}
int nos_usb_diag_command(const char *command)
{
    int guard_command=!strncmp(command,"usb_guard",9) && (!command[9] || command[9]==' ');
    int diag_command=!strncmp(command,"usb_diag",8) && (!command[8] || command[8]==' ');
    if(!guard_command && !diag_command)return 0;
    const char *p=command+(guard_command?9:8);uint32_t nonce=0;int valid=0,mode=0;
    if(guard_command){
        if(p[0]==' ' && (p[1]=='0' || p[1]=='1') && p[2]==' '){mode=p[1]-'0';p+=2;}
        else p="";
    }
    if(*p++==' ' && *p){valid=1;while(*p){unsigned digit=(unsigned)(*p++-'0');if(digit>9 || nonce>(UINT32_MAX-digit)/10u){valid=0;break;}nonce=nonce*10u+digit;}}
    if(!valid)nonce=0;
    if(valid && guard_command){LOCK();s_stats.guard_enabled=(uint32_t)mode;UNLOCK();}
    nos_usb_diag_stats s;nos_usb_diag_snapshot(&s);
    /* Payload is ASCII bytes between colon and final space before crc=.
     * Nonce is echoed decimal; malformed request uses nonce0 valid0. The
     * snapshot includes this query's RX/dispatch, excludes its own TX. */
    char payload[384],frame[448];
    int n=snprintf(payload,sizeof payload,"v=2 nonce=%lu valid=%d guard=%lu overrides=%lu cc=%lu cf=%lu zc=%lu zf=%lu zu=%lu zm=%lu wc=%lu wf=%lu wu=%lu wm=%lu rx=%lu qa=%lu qf=%lu ui=%lu rc=%lu ri=%lu rq=%lu ra=%lu",
        (unsigned long)nonce,valid,(unsigned long)s.guard_enabled,(unsigned long)s.connected_overrides,(unsigned long)s.connected_checks,(unsigned long)s.connected_false,
        (unsigned long)s.zero_calls,(unsigned long)s.zero_fail,(unsigned long)s.zero_us,(unsigned long)s.zero_max_us,
        (unsigned long)s.wait_calls,(unsigned long)s.wait_fail,(unsigned long)s.wait_us,(unsigned long)s.wait_max_us,
        (unsigned long)s.usb_lines,(unsigned long)s.queue_accepted,(unsigned long)s.queue_fail,(unsigned long)s.dispatch,(unsigned long)s.reply_calls,
        (unsigned long)s.reply_incomplete,(unsigned long)s.reply_last_requested,(unsigned long)s.reply_last_accepted);
    if(n<0 || (size_t)n>=sizeof payload)return 1;
    int len=snprintf(frame,sizeof frame,"USBDIAG %u:%s crc=%08lx\n",(unsigned)n,payload,(unsigned long)crc32((const unsigned char*)payload,(size_t)n));
    if(len<0 || (size_t)len>=sizeof frame)return 1;
    /* One bounded call (50ms per driver wait, potentially two waits)
     * bypasses VFS connected heuristic; real driver return
     * means enqueued bytes, not host receipt. Wrapper still counts this call. */
    int accepted=usb_serial_jtag_write_bytes(frame,(size_t)len,pdMS_TO_TICKS(50));
    LOCK();s_stats.reply_calls++;s_stats.reply_incomplete+=(accepted!=len);
    s_stats.reply_last_requested=(uint32_t)len;s_stats.reply_last_accepted=accepted>0?(uint32_t)accepted:0;UNLOCK();
    return 1;
}
