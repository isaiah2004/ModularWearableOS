#include "render_exposure.h"
#include "render_history.h"
#include "ui.h"
#include <string.h>
/* Only called for nonzero words. Keep the worker portable without requiring
 * a particular target bit-scan instruction. */
static unsigned first_bit(uint32_t value) {
#if defined(__GNUC__)
    return (unsigned)__builtin_ctz(value);
#else
    unsigned bit = 0;
    while (!(value & 1u)) {
        value >>= 1;
        ++bit;
    }
    return bit;
#endif
}
int nos_exposure_prepare(nos_exposure_job *j, const gfx_fb *fb, int x0, int y0, int x1, int y1,
                         int temporal, int aod) {
    if (!j || !fb || !fb->px || fb->rec || fb->h <= 0)
        return 0;
    int stride = fb->stride ? fb->stride : GFX_W, c0 = fb->cx0, c1 = fb->cx1;
    if (stride <= 0)
        return 0;
    if (c1 <= c0) {
        c0 = 0;
        c1 = GFX_W;
    }
    if (x0 < 0)
        x0 = 0;
    if (x0 < c0)
        x0 = c0;
    if (x0 < fb->x_base)
        x0 = fb->x_base;
    if (x1 > GFX_W)
        x1 = GFX_W;
    if (x1 > c1)
        x1 = c1;
    int64_t right = (int64_t)fb->x_base + stride;
    if (x1 > right)
        x1 = (int)right;
    if (y0 < 0)
        y0 = 0;
    if (y0 < fb->y_base)
        y0 = fb->y_base;
    if (y1 > GFX_H)
        y1 = GFX_H;
    int64_t bottom = (int64_t)fb->y_base + fb->h;
    if (y1 > bottom)
        y1 = (int)bottom;
    if (y1 - y0 > NOS_EXPOSURE_ROWS)
        return 0;
    j->fb = *fb;
    j->x0 = x0;
    j->x1 = x1;
    j->y0 = y0;
    j->y1 = y1;
    j->enabled = temporal && !aod;
    j->mode = j->enabled ? 1 : 0;
    if (x1 <= x0 || y1 <= y0) {
        j->x1 = x0;
        j->y1 = y0;
        return 1;
    }
    if (!ui_fblur_any_active())
        return 1;
    memset(j->directional, 0, sizeof j->directional);
    int any = 0, all = 1;
    for (int y = y0; y < y1; y++) {
        int spans[32][2], n = ui_fblur_row_spans(y, spans, 32), covered = 0;
        for (int i = 0; i < n; i++) {
            int a = spans[i][0], b = spans[i][1];
            if (a < x0)
                a = x0;
            if (b > x1)
                b = x1;
            if (b <= a)
                continue;
            covered += b - a;
            for (int w = a / 32; w <= (b - 1) / 32; w++) {
                int lo = a > w * 32 ? a - w * 32 : 0, hi = b < (w + 1) * 32 ? b - w * 32 : 32;
                uint32_t mask = (0xffffffffu << lo) & (0xffffffffu >> (32 - hi));
                j->directional[y - y0][w] |= mask;
            }
        }
        any |= covered != 0;
        all &= covered == x1 - x0;
    }
    /* Copy/expose fast modes avoid worker mask scans. Mixed retains boundaries
     * even when disabled so history instrumentation matches original partition. */
    if (all)
        j->mode = 0;
    else if (any)
        j->mode = 2;
    return 1;
}
void nos_exposure_apply(const nos_exposure_job *j) {
    if (!j)
        return;
    if (j->x1 <= j->x0 || j->y1 <= j->y0)
        return;
    gfx_fb fb = j->fb;
    if (j->mode != 2) {
        nos_history_apply_disjoint(&fb, j->x0, j->y0, j->x1, j->y1, j->mode == 1);
        return;
    }
    /* Group only identical masks. Each row retains the same X-span order
     * and every covered pixel still updates sharp history without reblur. */
    for (int y = j->y0; y < j->y1;) {
        const uint32_t *mask = j->directional[y - j->y0];
        int end_y=y+1;
        const int word0=j->x0/32, words=(j->x1-1)/32-word0+1;
        while(end_y<j->y1 && !memcmp(mask+word0,
              j->directional[end_y-j->y0]+word0,(size_t)words*sizeof *mask))end_y++;
        int x = j->x0;
        while (x < j->x1) {
            int start = x, covered = (mask[x / 32] >> (x & 31)) & 1u;
            /* Scan words, invoking history only at actual coverage transitions. */
            while (x < j->x1) {
                int bit = x & 31, end = (x & ~31) + 32;
                if (end > j->x1)
                    end = j->x1;
                uint32_t opposite = (covered ? ~mask[x / 32] : mask[x / 32]) &
                                    (0xffffffffu << bit) &
                                    (0xffffffffu >> (32 - (end - (x & ~31))));
                if (opposite) {
                    x = (x & ~31) + first_bit(opposite);
                    break;
                }
                x = end;
            }
            nos_history_apply_disjoint(&fb, start, y, x, end_y, covered ? 0 : j->enabled);
        }
        y=end_y;
    }
}
