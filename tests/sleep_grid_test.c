#include "sleep_grid.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    /* Every result lands on a 50 ms grid point and never waits less than period - slack. */
    for (uint32_t period = 1; period <= 1000; period++)
        for (uint32_t now = 0; now < 400; now++) {
            uint32_t d = sleep_grid_delay_ms(now, period), at = now + d;
            uint32_t slack = period > SLEEP_GRID_SLACK_MS ? SLEEP_GRID_SLACK_MS : 0;
            assert(at % SLEEP_GRID_MS == 0 && d >= period - slack && d < period + SLEEP_GRID_MS);
        }
    /* A task that runs up to 10 ms late keeps the 50 ms cadence instead of dropping to 100 ms. */
    for (uint32_t late = 0; late <= SLEEP_GRID_SLACK_MS; late++)
        assert(sleep_grid_delay_ms(1000 + late, 50) == 50 - late);
    assert(sleep_grid_delay_ms(1011, 50) == 89); /* later than the slack: next grid point after that */
    /* Tasks with different periods wake together at the common grid points. */
    assert(1000 + sleep_grid_delay_ms(1000, 1000) == 2000);
    assert(1003 + sleep_grid_delay_ms(1003, 1000) == 2000);
    /* Tick-counter wrap: unsigned arithmetic stays bounded. */
    uint32_t d = sleep_grid_delay_ms(0xffffffffu - 5, 50);
    assert(d >= 40 && d < 100);
    puts("sleep grid passed");
    return 0;
}
