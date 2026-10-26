/* knock_test.c - synthetic traces for core/knock.c at the IMU's 125 Hz.
 * gcc -O2 -std=c99 -Icore tests/knock_test.c core/knock.c -lm -o knock_test */
#include "knock.h"
#include <math.h>
#include <stdio.h>

static knock_state K;
static uint32_t T;
static int fired;
#define DT 8   /* ms: 125 Hz */

/* one sample: gravity face-up plus extra acceleration, and a gyro speed */
static void s(float ex, float ey, float ez, float gyro)
{
    T += DT;
    fired += knock_update(&K, 0.02f + ex, -0.01f + ey, -1.0f + ez, gyro, T);
}
static void still(int ms) { for (int t = 0; t < ms; t += DT) s(0, 0, 0, 2); }
/* a real knock (knockrec2.txt): a 2.5 g spike into the glass, then the wrist's reaction,
 * ~200 deg/s and ~0.3 g for ~100 ms */
static void knock(void)
{
    s(0, 0, 2.5f, 10); s(0.2f, 0.1f, -1.0f, 150);
    for (int t = 0; t < 100; t += DT) s(0.2f, 0.1f, 0.2f, 200);
}
static void side_knock(void)   /* on the case's side: mostly sideways */
{
    s(2.0f, 0.4f, 0.5f, 20); s(-0.8f, 0, -0.2f, 160);
    for (int t = 0; t < 100; t += DT) s(0.2f, 0.1f, 0.1f, 220);
}
static void reset(void) { knock_init(&K); T = 1000; fired = 0; still(600); }

static int fails;
static void check(const char *name, int want)
{
    int ok = (want ? fired == 1 : fired == 0);
    printf("%s %-40s fired=%d\n", ok ? "ok  " : "FAIL", name, fired);
    if (!ok) fails++;
}

int main(void)
{
    reset(); knock(); still(220); knock(); still(400);                  check("double knock", 1);
    reset(); knock(); knock(); still(400);                              check("quick double knock (~120 ms)", 1);
    reset(); side_knock(); still(150); side_knock(); still(400);        check("double knock on the case side", 1);
    reset(); knock(); still(330); knock(); still(400);                  check("slow double knock (~450 ms)", 1);
    reset(); knock(); still(800);                                       check("single knock", 0);
    reset(); knock(); still(220); knock(); still(150); knock(); still(400); check("triple knock", 0);
    reset(); knock(); still(700); knock(); still(400);                  check("two knocks too far apart", 0);
    /* knocks while the arm swings: rotation and broad acceleration between them */
    reset();
    for (int t = 0; t < 600; t += DT) s(0.15f * sinf(t * 0.02f), 0.1f, 0.1f * sinf(t * 0.03f), 150);
    knock(); for (int t = 0; t < 200; t += DT) s(0.2f, 0.1f, 0.1f, 160); knock();
    for (int t = 0; t < 400; t += DT) s(0.15f, 0.1f, 0.05f, 150);
    check("knocks while swinging the arm", 0);
    /* running: a footstrike every 330 ms, each a sharp Z spike, arm swinging between */
    reset();
    for (int k = 0; k < 12; k++) {
        s(0.3f, 0.1f, 1.4f, 140); s(0.2f, 0, -0.6f, 150);
        for (int t = 0; t < 314; t += DT) s(0.25f * sinf(t * 0.02f), 0.1f, 0.15f * sinf(t * 0.02f), 130);
    }
    still(400);                                                         check("running footstrikes", 0);
    /* walking with a still-ish wrist: steps every 550 ms, softer */
    reset();
    for (int k = 0; k < 8; k++) { s(0.05f, 0, 0.45f, 40); for (int t = 0; t < 542; t += DT) s(0.06f, 0.02f, 0.05f, 45); }
    still(400);                                                         check("walking steps", 0);
    /* a slow wrist raise: broad, sideways, no spike */
    reset();
    for (int t = 0; t < 600; t += DT) s(0.3f * sinf(t * 0.005f), 0.2f, 0.1f, 90);
    still(400);                                                         check("slow wrist raise", 0);
    printf(fails ? "FAIL knock: %d case(s)\n" : "PASS knock: double/quick/slow pairs; single, triple, far apart, swinging, running, walking, raise rejected\n", fails);
    return fails != 0;
}
