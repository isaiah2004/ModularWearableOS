"""Mandatory boot resources survive optional heap pressure; workers start gated."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "host GCC is required")
class BootResources(unittest.TestCase):
    def test_actual_reservation_and_worker_dependency_gates(self):
        source = (ROOT / "firmware/main/nos_main.c").read_text()
        start = source.index("typedef struct { TaskFunction_t entry; } boot_worker;")
        end = source.index("static int s_btn_swallow;", start)
        actual = source[start:end]
        boot = source[source.index("void app_main(void)"):]
        reserve = boot.index("boot_resources_init();")
        self.assertLess(boot.index("lcd_init();"), reserve)
        for operation in ("board_init();", "hal_ext_init();", "hal_esp32_start_radios();"):
            self.assertLess(reserve, boot.index(operation))
        self.assertGreater(boot.index("xTaskNotifyGive(s_touch_handle)"), boot.index("nos_apps_boot();"))
        self.assertGreater(boot.index("xTaskNotifyGive(s_command_handle)"), boot.index("usb_serial_jtag_vfs_use_driver();"))
        self.assertGreater(boot.index("xTaskNotifyGive(s_command_handle)"), boot.index("wake_service_init("))
        # The helper owns each allocation exactly once; app_main cannot silently
        # add a late mandatory allocation after optional radio initialization.
        for operation in ("xTaskCreate(", "xQueueCreate(",
                          "xSemaphoreCreate", "usb_serial_jtag_driver_install(", "esp_timer_create("):
            self.assertNotIn(operation, boot[:boot.index("int64_t period = ")])
        # The optional, fallible prebake task may still be created late.
        self.assertEqual(boot[:boot.index("int64_t period = ")].count("xTaskCreatePinnedToCore("), 1)
        self.assertIn("xTaskCreatePinnedToCore(dotbake_task,", boot)
        self.assertNotIn("hal_wifi_set_enabled(", boot)
        self.assertNotIn("s_wifi_applied", boot)
        harness = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
typedef void (*TaskFunction_t)(void *);
typedef int TaskHandle_t;
typedef int BaseType_t;
typedef struct { int rx_buffer_size,tx_buffer_size; } usb_serial_jtag_driver_config_t;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
typedef struct { int unused[9]; } flush_job;
typedef struct { char bytes[201]; } cmd_line;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define TOUCH_STACK_BYTES 4096
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define TAG "test"
#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() ((usb_serial_jtag_driver_config_t){0})
#define ESP_ERROR_CHECK(x) assert((x)==0)
#define ESP_LOGI(tag,format,...) do { (void)(tag); if(0)printf(format,__VA_ARGS__); } while(0)
static int s_buf_free,s_dma_ready,s_flush_q,s_frame_done,s_command_done,s_command_lock;
static int s_cmd_q,s_host_reply_lock,s_frame_tick,s_frame_timer,s_touch_task_ok;
static int s_flush_handle,s_touch_handle;
static TaskHandle_t s_command_handle;
static int available=32768,objects,usb_installed,timer_created,lcd_ready=1,board_ready,os_ready;
static int ran[4],notified[4],task_count,current;
static TaskFunction_t entries[4];static void *arguments[4];static unsigned stacks[4];
static jmp_buf finished;
static int allocate(unsigned bytes) { if(bytes>(unsigned)available)return 0;available-=(int)bytes;return ++objects; }
static int xSemaphoreCreateCounting(int max,int count) { assert(max==2&&count==2);return allocate(84); }
static int xSemaphoreCreateBinary(void) { return allocate(84); }
static int xSemaphoreCreateMutex(void) { return allocate(84); }
static int xQueueCreate(unsigned count,unsigned item) { return allocate(84+count*item); }
static unsigned heap_caps_get_free_size(int caps) { (void)caps;return (unsigned)available; }
static unsigned heap_caps_get_largest_free_block(int caps) { (void)caps;return (unsigned)available; }
static int usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *c) {
 assert(c->rx_buffer_size==4096&&c->tx_buffer_size==1024);
 assert(allocate((unsigned)(c->rx_buffer_size+c->tx_buffer_size)+256));usb_installed=1;return 0;
}
static void pace_cb(void *p) { (void)p;assert(0); }
static int esp_timer_create(const esp_timer_create_args_t *c,int *timer) {
 assert(c->callback==pace_cb);*timer=allocate(96);assert(*timer);timer_created=1;return 0;
}
static int xTaskCreate(TaskFunction_t entry,const char *name,unsigned bytes,void *arg,int priority,TaskHandle_t *handle) {
 (void)name;assert(priority==4||priority==5);if(!allocate(bytes+340))return 0;
 int id=++task_count;assert(id<=3);entries[id]=entry;arguments[id]=arg;stacks[id]=bytes;*handle=id;return pdPASS;
}
static int xTaskCreatePinnedToCore(TaskFunction_t entry,const char *name,unsigned bytes,void *arg,int priority,TaskHandle_t *handle,int core) {
 assert(core==1);return xTaskCreate(entry,name,bytes,arg,priority,handle);
}
static unsigned ulTaskNotifyTake(int clear,uint32_t wait) {
 assert(clear==pdTRUE&&wait==portMAX_DELAY);assert(current&&notified[current]);notified[current]=0;return 1;
}
static void vTaskDelete(void *task) { assert(!task);longjmp(finished,1); }
static void xTaskNotifyGive(TaskHandle_t task) {
 assert(task>0&&task<=task_count&&!notified[task]);notified[task]=1;current=task;
 if(!setjmp(finished)){entries[task](arguments[task]);}current=0;
}
static void flush_task(void *arg) { assert(!arg&&lcd_ready&&s_flush_q&&s_command_done&&s_dma_ready);ran[1]++; }
static void touch_task(void *arg) { assert(!arg&&board_ready);ran[2]++; }
static void cmd_task(void *arg) { assert(!arg&&usb_installed&&os_ready&&s_cmd_q&&s_host_reply_lock);ran[3]++; }
'''
        harness += actual + r'''
int main(void) {
 boot_resources_init();assert(task_count==3&&usb_installed&&timer_created);
 assert(stacks[s_flush_handle]==3072&&stacks[s_touch_handle]==4096&&stacks[s_command_handle]==6144);
 assert(ran[1]==1&&!ran[2]&&!ran[3]);
 /* A radio consuming the remaining heap cannot steal any mandatory resource.
  * This exact pressure would reject the former late6KiB command allocation. */
 assert(available>1024);assert(allocate((unsigned)available-512));
 TaskHandle_t rejected=0;assert(!xTaskCreate(cmd_task,"late",6144,NULL,4,&rejected));
 assert(!rejected&&available==512);
 board_ready=1;xTaskNotifyGive(s_touch_handle);assert(ran[2]==1&&!ran[3]);
 os_ready=1;xTaskNotifyGive(s_command_handle);assert(ran[3]==1);
 puts("PASS actual mandatory boot reservations and dependency-gated workers under optional heap pressure");return 0;
}
'''
        parent = ROOT / ".worktrees/_audit_tmp"
        parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="boot-resources-", dir=parent) as directory:
            directory = Path(directory)
            test = directory / "boot.c"
            test.write_text(harness)
            binary = directory / ("boot.exe" if os.name == "nt" else "boot")
            result = subprocess.run(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                     "-Wno-unused-function", str(test), "-o", str(binary)],
                                    cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS actual mandatory boot", result.stdout)


if __name__ == "__main__":
    unittest.main()
