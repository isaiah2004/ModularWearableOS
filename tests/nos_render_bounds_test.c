#include "render_bounds.h"
#include "render_config.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    unsigned pixels = 0;
    for (int height = 1; height <= GFX_H; height++) {
        for (int y0 = 0; y0 < GFX_H; y0 += height) {
            int y1 = y0 + height; if (y1 > GFX_H) y1 = GFX_H;
            int x0 = 0, x1 = GFX_W;
            nos_glass_span(y0, y1, &x0, &x1);
            assert(x0 >= 0 && x1 <= GFX_W && !(x0 & 1) && !(x1 & 1));
            for (int y = y0; y < y1; y++) for (int x = 0; x < GFX_W; x++) {
                float dx = x + 0.5f - GFX_CX, dy = y + 0.5f - GFX_CY;
                if (dx * dx + dy * dy < (GFX_CX + 0.5f) * (GFX_CX + 0.5f))
                    assert(x >= x0 && x < x1);
            }
            if (height == NOS_STRIPE_ROWS) pixels += (unsigned)((x1 - x0) * (y1 - y0));
        }
    }
    assert(pixels < GFX_W * GFX_H * 85u / 100u);
    printf("round stripe bounds: every visible/AA pixel retained; %d rows, %u/%u pixels (%.1f%% fewer)\n",
           NOS_STRIPE_ROWS, pixels, GFX_W * GFX_H, 100.0 * (1.0 - (double)pixels / (GFX_W * GFX_H)));
    return 0;
}
