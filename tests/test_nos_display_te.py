"""Exercise the actual TE ISR/wait implementation with deterministic GPIO/RTOS I/O."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('gcc'), 'host GCC required')
class DisplayTETests(unittest.TestCase):
    def compile_and_run(self, board_c, init_error=0):
        actual = (ROOT / 'firmware/main/nos_display_te.c').read_text()
        actual = re.sub(r'^#include .*$', '', actual, flags=re.M)
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "nos_display_te.h"
#if BOARD_C
#define PIN_LCD_TE 8
#define IRAM_ATTR
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define portENTER_CRITICAL_ISR(p) ((void)(p))
#define portEXIT_CRITICAL_ISR(p) ((void)(p))
#define portYIELD_FROM_ISR() (yields++)
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef int portMUX_TYPE;
typedef int esp_err_t;
enum {ESP_OK=0,ESP_ERR_INVALID_STATE=0x103,GPIO_MODE_INPUT=1,
      GPIO_PULLDOWN_ENABLE=1,GPIO_INTR_POSEDGE=1};
typedef struct {int value;} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef struct {uint64_t pin_bit_mask;int mode,pull_down_en,intr_type;} gpio_config_t;
static int irq,configured,yields,stale_wake;
static int64_t now_us=1000000,next_edge=1005000;
static void (*handler)(void *);
static int64_t esp_timer_get_time(void) {return now_us;}
static SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s) {s->value=0;return s;}
static int xSemaphoreGiveFromISR(SemaphoreHandle_t s,BaseType_t *woken)
{s->value=1;*woken=1;return pdTRUE;}
static int xSemaphoreTake(SemaphoreHandle_t s,TickType_t ticks) {
    if(s->value){s->value=0;return pdTRUE;}
    if(!ticks)return pdFALSE;
    assert(ticks<=25);
    /* A delayed give from the previous wait must not pass the sequence check. */
    if(stale_wake){stale_wake=0;now_us+=10;return pdTRUE;}
    int64_t deadline=now_us+(int64_t)ticks*1000;
    if(irq && next_edge && next_edge<=deadline) {
        assert(next_edge>now_us);now_us=next_edge;next_edge+=16667;handler(NULL);
        assert(s->value);s->value=0;now_us+=75;return pdTRUE;
    }
    now_us=deadline;return pdFALSE;
}
static int gpio_config(const gpio_config_t *c) {
    assert(c->pin_bit_mask==(1ULL<<8) && c->mode==GPIO_MODE_INPUT);
    assert(c->pull_down_en==GPIO_PULLDOWN_ENABLE && c->intr_type==GPIO_INTR_POSEDGE);
    configured++;return INIT_ERROR;
}
static int gpio_intr_enable(int pin) {assert(pin==8);irq=1;return ESP_OK;}
static int gpio_intr_disable(int pin) {assert(pin==8);irq=0;return ESP_OK;}
static int gpio_install_isr_service(int flags) {assert(!flags);return ESP_ERR_INVALID_STATE;}
static int gpio_isr_handler_add(int pin,void (*fn)(void *),void *arg)
{assert(pin==8 && !arg);handler=fn;return ESP_OK;}
#endif
'''
        assertions = r'''
int main(void) {
    nos_display_te_init();
    nos_display_te_stats s=nos_display_te_snapshot();
#if BOARD_C
    assert(configured==1 && s.gpio==8);
    if(INIT_ERROR) {
        assert(!s.ready && s.error==INIT_ERROR && !irq);
        int64_t before=now_us;nos_display_te_wait();assert(now_us==before);
        puts("PASS checked TE init failure falls back without a wait");return 0;
    }
    assert(s.ready && !s.enabled && s.awake && !irq);
    int64_t disabled_at=now_us;nos_display_te_wait();
    assert(now_us==disabled_at && !nos_display_te_snapshot().waits);
    nos_display_te_set_enabled(1);assert(irq);
    nos_display_te_wait();s=nos_display_te_snapshot();
    assert(s.edges==1 && s.waits==1 && s.synced==1 && !s.timeouts);
    assert(s.last_wait_us==5075 && s.last_phase_us==75 && yields==1);
    /* A previously latched pulse is drained, then the next fresh edge is used. */
    now_us=next_edge;next_edge+=16667;handler(NULL);s_edge->value=1;
    nos_display_te_wait();s=nos_display_te_snapshot();
    assert(s.edges==3 && s.waits==2 && s.synced==2 && s.period_us==16667);
    assert(s.last_wait_us==16742 && s.max_phase_us==75);
    /* A notification without a fresh edge cannot start a transfer. */
    stale_wake=1;nos_display_te_wait();s=nos_display_te_snapshot();
    assert(s.synced==3 && !s.timeouts && s.edges==4 && s.last_phase_us==75);
    /* Missing edges are bounded; the display owner can still submit its DMA. */
    next_edge=0;int64_t before=now_us;nos_display_te_wait();s=nos_display_te_snapshot();
    assert(now_us-before==25000 && s.timeouts==1 && s.waits==4 && s.synced==3);
    assert(s.max_wait_us==25000 && s.wait_total_us>25000);
    nos_display_te_set_awake(0);assert(!irq);
    before=now_us;nos_display_te_wait();s=nos_display_te_snapshot();
    assert(!s.awake && now_us==before && s.waits==4 && !s.period_us);
    assert(s.edge_age_us==UINT32_MAX);
    nos_display_te_set_awake(1);assert(irq);
    next_edge=now_us+1000;nos_display_te_wait();s=nos_display_te_snapshot();
    assert(s.synced==4 && s.timeouts==1 && s.last_wait_us==1075 && !s.period_us);
    nos_display_te_set_enabled(0);assert(!irq);
    before=now_us;nos_display_te_wait();assert(now_us==before);
    nos_display_te_set_enabled(1);assert(irq);
    nos_display_te_wait();s=nos_display_te_snapshot();
    assert(s.synced==5 && s.timeouts==1 && s.waits==6);
    puts("PASS actual TE: fresh edge gating, stale notifications, bounded timeout, sleep/wake and enable");
#else
    assert(s.gpio==-1 && !s.ready && !s.enabled);
    nos_display_te_wait();nos_display_te_set_awake(0);nos_display_te_set_enabled(1);
    s=nos_display_te_snapshot();assert(!s.waits && !s.enabled);
    puts("PASS B board: no GPIO8 use or TE waits");
#endif
}
'''
        with tempfile.TemporaryDirectory(prefix='nos-te-') as tmp:
            source=Path(tmp)/'te.c'
            binary=Path(tmp)/('te.exe' if os.name=='nt' else 'te')
            source.write_text(harness+actual+assertions)
            result=subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                f'-DBOARD_C={int(board_c)}',f'-DINIT_ERROR={init_error}',
                '-Ifirmware/main',str(source),'-o',str(binary)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(binary)],cwd=ROOT,capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('PASS',result.stdout)

    def test_c_fresh_edges_timeout_and_sleep(self):
        self.compile_and_run(True)

    def test_c_init_failure(self):
        self.compile_and_run(True,0x102)

    def test_b_has_no_te_pin(self):
        self.compile_and_run(False)
