#include "nos_button_pulse.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    nos_button_pulse p = {0};
    assert(!nos_button_pulse_sample(&p, 0));
    nos_button_pulse_start(&p, 100);
    assert(nos_button_pulse_sample(&p, 100));
    assert(nos_button_pulse_sample(&p, 125));
    assert(nos_button_pulse_sample(&p, 179));
    assert(!nos_button_pulse_sample(&p, 180));
    assert(!nos_button_pulse_sample(&p, 200));
    nos_button_pulse_start(&p, 300);
    assert(nos_button_pulse_sample(&p, 500)); /* delayed first frame still sees down */
    assert(!nos_button_pulse_sample(&p, 525));
    nos_button_pulse_start(&p, UINT32_MAX - 39);
    assert(nos_button_pulse_sample(&p, UINT32_MAX - 20));
    assert(nos_button_pulse_sample(&p, 39));
    assert(!nos_button_pulse_sample(&p, 40)); /* wrap-safe 80ms */
    nos_button_pulse_start(&p, 1000);
    assert(nos_button_pulse_sample(&p, 1000));
    nos_button_pulse_start(&p, 1070); /* overlapping requests extend the current press */
    assert(nos_button_pulse_sample(&p, 1090));
    assert(!nos_button_pulse_sample(&p, 1150));
    int physical = 1;
    assert(physical | nos_button_pulse_sample(&p, 1200)); /* no synthetic physical release */
    puts("NOS serial button pulse passed");
    return 0;
}
