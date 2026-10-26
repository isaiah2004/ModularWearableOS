#include "band_damage.h"
#include "render_bounds.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
static unsigned state = 11;
static unsigned rnd(void) {
  state = state * 1664525u + 1013904223u;
  return state;
}
static unsigned char reference[2][BD_BANDS][BD_WIDTH / 2];
static void reference_box(int which, int x0, int y0, int x1, int y1) {
  if (x1 <= x0 || y1 <= y0)
    return;
  for (int b = 0; b < BD_BANDS; b++) {
    int bottom = (b + 1) * 16;
    if (bottom > 466)
      bottom = 466;
    if (y0 >= bottom || y1 <= b * 16)
      continue;
    for (int x = 0; x < 233; x++)
      if (x0 < (x + 1) * 2 && x1 > x * 2)
        reference[which][b][x] = 1;
  }
}
static int oracle(int b, int tail, int gap, int out[2][2]) {
  int ranges[233][2], n = 0;
  for (int x = 0; x < 233;) {
    if (!(reference[0][b][x] || (tail && reference[1][b][x]))) {
      x++;
      continue;
    }
    int first = x;
    while (x < 233 && (reference[0][b][x] || (tail && reference[1][b][x])))
      x++;
    if (n && first * 2 - ranges[n - 1][1] <= gap)
      ranges[n - 1][1] = x * 2;
    else {
      ranges[n][0] = first * 2;
      ranges[n++][1] = x * 2;
    }
  }
  if (n > 2) {
    out[0][0] = ranges[0][0];
    out[0][1] = ranges[n - 1][1];
    return 1;
  }
  for (int i = 0; i < n; i++)
    memcpy(out[i], ranges[i], sizeof out[i]);
  return n;
}
int main(void) {
  band_damage maps[2];
  unsigned long checked = 0;
  for (int t = 0; t < 2500; t++) {
    memset(reference, 0, sizeof reference);
    band_damage_clear(&maps[0]);
    band_damage_clear(&maps[1]);
    for (int which = 0; which < 2; which++)
      for (int r = 0; r < 20; r++) {
        int x0 = (int)(rnd() % 650) - 100, y0 = (int)(rnd() % 650) - 100,
            x1 = x0 + 1 + rnd() % 130, y1 = y0 + 1 + rnd() % 100;
        band_damage_add(&maps[which], x0, y0, x1, y1);
        reference_box(which, x0, y0, x1, y1);
      }
    for (int b = 0; b < 30; b++)
      for (int tail = 0; tail < 2; tail++)
        for (int gap = 0; gap <= 64; gap += 32) {
          int got[2][2] = {{0}}, want[2][2] = {{0}};
          int n = band_damage_spans(&maps[0], &maps[1], tail, b, gap, got),
              m = oracle(b, tail, gap, want);
          assert(n == m && !memcmp(got, want, n * sizeof got[0]));
          for (int i = 0; i < n; i++) {
            assert(got[i][0] >= 0 && got[i][1] <= 466 &&
                   got[i][0] < got[i][1] && !(got[i][0] & 1) &&
                   !(got[i][1] & 1));
            if (i)
              assert(got[i - 1][1] < got[i][0]);
          }
          int previous_end = -1;
          for (int i = 0; i < n; i++) {
            int x0 = got[i][0], x1 = got[i][1], y1 = (b + 1) * 16;
            if (y1 > 466)
              y1 = 466;
            nos_glass_span(b * 16, y1, &x0, &x1);
            assert(x0 >= got[i][0] && x1 <= got[i][1]);
            if (x1 > x0) {
              assert(previous_end <= x0);
              previous_end = x1;
            }
          }
          for (int x = 0; x < 233; x++)
            if (reference[0][b][x] || (tail && reference[1][b][x])) {
              int covered = 0;
              for (int i = 0; i < n; i++)
                covered |= x * 2 >= got[i][0] && x * 2 < got[i][1];
              assert(covered);
            }
          checked++;
        }
  }
  band_damage_clear(&maps[0]);
  band_damage_add(&maps[0], INT_MIN, INT_MIN, INT_MAX, INT_MAX);
  for (int b = 0; b < 30; b++) {
    int out[2][2];
    assert(band_damage_spans(&maps[0], NULL, 0, b, 64, out) == 1 &&
           out[0][0] == 0 && out[0][1] == 466);
  }
  band_damage_clear(&maps[0]);
  band_damage_add(&maps[0], 0, 0, 2, 1);
  band_damage_add(&maps[0], 200, 0, 202, 1);
  band_damage_add(&maps[0], 464, 0, 466, 1);
  int out[2][2];
  assert(band_damage_spans(&maps[0], NULL, 0, 0, 64, out) == 1 &&
         out[0][0] == 0 && out[0][1] == 466);
  printf("PASS%lu independent interval/coverage cases;max2spanfallback never "
         "loses coverage;even/clip/INTbounds;mapbytes=%u pairbytes=%u\n",
         checked, (unsigned)sizeof maps[0], (unsigned)sizeof maps);
  return 0;
}
