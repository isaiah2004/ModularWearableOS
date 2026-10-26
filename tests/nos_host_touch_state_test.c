#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef struct {uint32_t polls,poll_ms,valid_ms,cancels;int raw_sample,suppressed;} touch_health;
typedef struct {uint32_t reads,errors,injected,pending;int last_error;} board_touch_diag;
typedef struct {uint32_t attempts,failures,consecutive,retry_in_ms;int available,first_error,last_error;} board_device_diag;
enum {BOARD_DEVICE_TOUCH,BOARD_DEVICE_IMU,BOARD_DEVICE_RTC,BOARD_DEVICE_COUNT};
static touch_health s_touch_health;
static int s_tmux,s_swallow,s_touch_now,critical,reads,devices;
static void *s_touch_handle=(void *)(uintptr_t)1;
static uint64_t clock_us=10000000;
#define portENTER_CRITICAL(p) do {(void)(p);assert(!critical);critical=1;}while(0)
#define portEXIT_CRITICAL(p) do {(void)(p);assert(critical);critical=0;}while(0)
static uint64_t esp_timer_get_time(void){return clock_us;}
static int eTaskGetState(void *task){assert(task==s_touch_handle);return 2;}
static board_touch_diag board_touch_diagnostics(void){reads++;return (board_touch_diag){6903,3,0,0,259};}
static void board_device_diagnostics(board_device_diag out[BOARD_DEVICE_COUNT])
{devices++;memset(out,0,sizeof *out*BOARD_DEVICE_COUNT);out[0]=(board_device_diag){20,22,19,900,0,259,259};}
static char reply[1024];
static int capture_printf(const char *format,...)
{va_list args;va_start(args,format);int n=vsnprintf(reply,sizeof reply,format,args);va_end(args);assert(n>0&&n<(int)sizeof reply);return n;}
#define printf capture_printf
/* ACTUAL FUNCTIONS */
#undef printf
int main(void)
{
    assert(host_command_readonly("touch_state")&&host_command_readonly("board"));
    assert(!host_command_readonly("touch_state reset")&&!host_command_readonly("board retry"));
    s_touch_health=(touch_health){1000,9992,5000,1,-1,1};s_swallow=1;
    touch_state_report();
    assert(strstr(reply,"polls=1000 poll_age_ms=8 valid_age_ms=5000 raw=-1"));
    assert(strstr(reply,"suppressed=1 swallowed=1 down=0 cancels=1 task_state=2"));
    assert(strstr(reply,"reads=6903 errors=3 available=0 attempts=20 failures=22 consecutive=19 retry_ms=900 last_error=259"));
    assert(reads==1&&devices==1&&!critical);
    s_touch_health=(touch_health){0};s_touch_handle=NULL;touch_state_report();
    assert(strstr(reply,"poll_age_ms=4294967295 valid_age_ms=4294967295"));
    assert(strstr(reply,"task_state=-1"));
    puts("PASS actual touch health diagnostic: task heartbeat, controller gate/retries, contact suppression, read-only queries and no I2C or persistence dependency");
}
