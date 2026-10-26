#include "band_damage.h"
#include <string.h>
void band_damage_clear(band_damage *m) {
  if (m)
    memset(m, 0, sizeof *m);
}
void band_damage_add(band_damage *m, int x0, int y0, int x1, int y1) {
  if (!m)
    return;
  if (x0 < 0)
    x0 = 0;
  if (x1 > BD_WIDTH)
    x1 = BD_WIDTH;
  if (y0 < 0)
    y0 = 0;
  if (y1 > BD_HEIGHT)
    y1 = BD_HEIGHT;
  if (x1 <= x0 || y1 <= y0)
    return;
  x0 /= 2;
  x1 = (x1 + 1) / 2;
  int b0 = y0 / BD_ROWS, b1 = (y1 + BD_ROWS - 1) / BD_ROWS;
  for (int b = b0; b < b1; b++)
    for (int w = x0 / 32; w <= (x1 - 1) / 32; w++) {
      int left = x0 - w * 32, right = x1 - w * 32;
      if (left < 0)
        left = 0;
      if (right > 32)
        right = 32;
      uint32_t mask = ~UINT32_C(0);
      mask <<= left;
      if (right < 32)
        mask &= (UINT32_C(1) << right) - 1u;
      m->bits[b][w] |= mask;
    }
}
static int marked(const band_damage *c, const band_damage *p, int tail, int b,
                  int x) {
  return ((c->bits[b][x / 32] | (tail && p ? p->bits[b][x / 32] : 0)) >>
          (x & 31)) &
         1u;
}
int band_damage_spans(const band_damage *c, const band_damage *p, int tail,
                      int band, int gap, int spans[BD_MAX_SPANS][2]) {
  if (!c || !spans || band < 0 || band >= BD_BANDS)
    return 0;
  if (gap < 0)
    gap = 0;
  if (gap > BD_WIDTH)
    gap = BD_WIDTH;
  int count = 0, overflow = 0, first = 0, last = 0;
  for (int x = 0; x < BD_WIDTH / 2;) {
    if (!marked(c, p, tail, band, x)) {
      x++;
      continue;
    }
    int start = x;
    while (x < BD_WIDTH / 2 && marked(c, p, tail, band, x))
      x++;
    while (x < BD_WIDTH / 2) {
      int next = x;
      while (next < BD_WIDTH / 2 && !marked(c, p, tail, band, next))
        next++;
      if (next == BD_WIDTH / 2 || (next - x) * 2 > gap)
        break;
      x = next;
      while (x < BD_WIDTH / 2 && marked(c, p, tail, band, x))
        x++;
    }
    if (!count)
      first = start * 2;
    last = x * 2;
    if (count < BD_MAX_SPANS) {
      spans[count][0] = start * 2;
      spans[count][1] = x * 2;
    } else
      overflow = 1;
    count++;
  }
  if (overflow) {
    spans[0][0] = first;
    spans[0][1] = last;
    return 1;
  }
  return count;
}
