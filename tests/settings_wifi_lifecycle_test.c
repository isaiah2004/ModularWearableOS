/* Fixed assertions replacing wifi_keyboard_cover.c; no credential text is logged. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nos/apps/set_connectivity.c"

nos_prefs_t nos_prefs;
static int covered, connects, pops;
static size_t connected_length;
int nos_leaving_covered(void) { return covered; }
void prefs_save(void) {}
void hal_wifi_connect(const char *ssid, const char *pass)
{
    assert(!strcmp(ssid, "test-network"));
    assert(strlen(pass) == 12);
    connected_length = strlen(pass); connects++;
}
void nos_pop(void) { pops++; keyboard_leave(); }

static void draft(int password_field)
{
    N.field = password_field;
    snprintf(N.ssid, sizeof N.ssid, "test-network");
    memset(N.pass, 'x', 12); N.pass[12] = 0;
    N.show = 1; N.symbols = 2; N.upper = 1;
}
static void assert_scrubbed(void)
{
    for (unsigned i = 0; i < sizeof N.pass; i++) assert(N.pass[i] == 0);
    assert(!N.show);
}
int main(void)
{
    network_state = calloc(1, sizeof *network_state); assert(network_state);
    for (int field = 0; field < 2; field++) {
        draft(field); covered = 1;
        for (int cover = 0; cover < 3; cover++) {
            keyboard_leave();
            assert(strlen(N.pass) == 12 && !N.show && N.field == field);
            assert(N.symbols == 2 && N.upper == 1 && !strcmp(N.ssid, "test-network"));
        }
        covered = 0; keyboard_leave(); assert_scrubbed();
    }
    draft(1);
    nos_input connect = {.type=IN_TAP,.x=300,.y=411};
    assert(keyboard_input(&connect));
    assert(connects == 1 && connected_length == 12 && pops == 1);
    assert_scrubbed();
    draft(0);
    nos_input cancel = {.type=IN_TAP,.x=158,.y=411};
    assert(keyboard_input(&cancel) && pops == 2); assert_scrubbed();
    draft(1); N.pass[4] = 0;
    assert(keyboard_input(&connect) && connects == 1 && pops == 2);
    assert(strlen(N.pass) == 4 && N.error[0]);
    covered = 1; keyboard_leave(); assert(strlen(N.pass) == 4 && N.error[0]);
    covered = 0; keyboard_leave(); assert_scrubbed();
    free(network_state); network_state = NULL;
    puts("PASS DEV-004: repeated cover preserves Wi-Fi/hidden-network drafts while masking text; exit/cancel/connect scrub the entire password; validation retains draft");
    return 0;
}
