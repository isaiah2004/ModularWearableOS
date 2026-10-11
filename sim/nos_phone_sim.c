/* nos_phone_sim.c — the phone side of the NOS simulator: hal_phone_* (svc.h) plus a
 * scripted fake Gadgetbridge phone.
 *
 * Timeline (on the active clock, so scripted runs are deterministic):
 *   3.0 s   connects, sends setTime + E.setTimeZone (mirroring the sim clock, so the
 *           time never jumps), then weather, musicinfo and musicstate (playing)
 *   14 s, 33 s, 55 s   three notifications (WhatsApp, Messages with accents, Calendar)
 * In scripted runs (--script) the three notifications are held back so their banners
 * don't land in other screens' screenshots; `phone sim notify` releases them.
 *
 * The fake phone answers what the watch sends: music play/pause/next/previous update
 * the fake player and come back as musicinfo/musicstate, like Gadgetbridge does.
 *
 * Script commands (via the sim's `phone LINE`, which calls svc_phone_inject):
 *   phone sim on | off      force the link up / down (auto = up after 3 s)
 *   phone sim auto
 *   phone sim notify        deliver the held notifications now
 *   phone sim call [NAME]   incoming call;  phone sim callend
 *   phone sim find          the phone rings the watch;  phone sim findstop
 * Any other `phone ...` line is fed to the services as if the phone had sent it. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "hal.h"
#include "svc.h"
#include "gb.h"
#include "nos_sim.h"

#define CONNECT_MS 3000u
#define QN   16
#define QLEN 512

static int      s_force = -1;           /* -1 auto, 0 down, 1 up */
static int      s_up;                   /* link state as last reported */
static uint32_t s_up_ms;                /* when the link came up */
static int      s_step;                 /* next connect-sequence step */
static int      s_notif_next;           /* next timeline notification */
static int      s_notif_release;        /* scripted runs: notifications released */
static char     s_q[QN][QLEN];          /* lines waiting to be read by the watch */
static int      s_qh, s_qn;

/* ---- fake player --------------------------------------------------------------- */
static const struct { const char *track, *artist, *album; int dur; } TRACKS[] = {
    { "Kerala", "Bonobo", "Migration", 245 },
    { "Midnight City", "M83", "Hurry Up, We're Dreaming", 243 },
    { "Breathe", "T\\xe9l\\xe9popmusik", "Genetic World", 276 },
};
#define NTRACKS ((int)(sizeof TRACKS / sizeof TRACKS[0]))
static int      s_track;
static int      s_playing = 1;
static uint32_t s_pos_ms = 73000, s_pos_t0;

static void push(const char *line)
{
    if (s_qn >= QN) return;
    snprintf(s_q[(s_qh + s_qn) % QN], QLEN, "%s", line);
    s_qn++;
}
static void pushf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void pushf(const char *fmt, ...)
{
    char b[QLEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    push(b);
}
static uint32_t player_pos(void)
{
    uint32_t p = s_pos_ms + (s_playing ? hal_millis() - s_pos_t0 : 0);
    uint32_t d = (uint32_t)TRACKS[s_track].dur * 1000u;
    return p > d ? d : p;
}
static void send_musicinfo(void)
{
    pushf("\x10GB({\"t\":\"musicinfo\",\"artist\":\"%s\",\"album\":\"%s\",\"track\":\"%s\",\"dur\":%d,\"c\":%d,\"n\":%d})",
          TRACKS[s_track].artist, TRACKS[s_track].album, TRACKS[s_track].track, TRACKS[s_track].dur,
          NTRACKS, s_track + 1);
}
static void send_musicstate(void)
{
    pushf("\x10GB({\"t\":\"musicstate\",\"state\":\"%s\",\"position\":%u,\"shuffle\":0,\"repeat\":0})",
          s_playing ? "play" : "pause", (unsigned)(player_pos() / 1000));
}

/* ---- connect sequence and timeline ----------------------------------------------- */
static void connect_step(uint32_t since)
{
    while (s_step < 4) {
        static const uint32_t at[4] = { 0, 200, 400, 500 };
        if (since < at[s_step]) return;
        switch (s_step) {
        case 0: {                       /* exactly what BangleJSDeviceSupport.setTime() sends */
            float tz = sim_tz_min / 60.0f;
            pushf("\x10setTime(%llu);E.setTimeZone(%.1f);(s=>s&&(s.timezone=%.1f,require('Storage').write('setting.json',s)))(require('Storage').readJSON('setting.json',1))",
                  (unsigned long long)hal_epoch_seconds(), tz, tz);
            break;
        }
        case 1:
            push("\x10GB({\"t\":\"weather\",\"temp\":291.15,\"hi\":294.15,\"lo\":286.15,\"hum\":78,\"rain\":60,"
                 "\"uv\":2,\"code\":500,\"txt\":\"light rain\",\"wind\":14.4,\"wdir\":225,\"loc\":\"Bengaluru\"})");
            break;
        case 2: s_pos_t0 = hal_millis(); send_musicinfo(); break;
        case 3: send_musicstate(); break;
        }
        s_step++;
    }
}
static const struct { uint32_t at_ms; const char *line; } NOTIFS[] = {
    { 14000, "\x10GB({\"t\":\"notify\",\"id\":7001,\"src\":\"WhatsApp\",\"title\":\"Sam Park\","
             "\"body\":\"Are we still on for dinner tonight? I booked 8:00\",\"sender\":\"Sam Park\",\"tel\":\"+919800000001\"})" },
    { 33000, "\x10GB({\"t\":\"notify\",\"id\":7002,\"src\":\"Messages\",\"title\":\"Zo\\xeb\","
             "\"body\":\"Lunch at the caf\\xe9 near Studio 2? \\u2014 my treat \\ud83d\\ude0a\"})" },
    { 55000, "\x10GB({\"t\":\"notify\",\"id\":7003,\"src\":\"Calendar\",\"title\":\"Standup\","
             "\"body\":\"Starts in 5 min \\u00b7 Room 4\"})" },
};
#define NNOTIFS ((int)(sizeof NOTIFS / sizeof NOTIFS[0]))

static void update(void)
{
    uint32_t now = hal_millis();
    int up = s_force >= 0 ? s_force : now >= CONNECT_MS;
    if (up != s_up) {
        s_up = up;
        s_qn = 0;
        if (up) { s_up_ms = now; s_step = 0; hal_log("phone: Gadgetbridge connected (sim)"); }
        else hal_log("phone: disconnected (sim)");
    }
    if (!s_up) return;
    connect_step(now - s_up_ms);
    int release = !sim_virtual || s_notif_release;
    while (release && s_notif_next < NNOTIFS && now >= NOTIFS[s_notif_next].at_ms)
        push(NOTIFS[s_notif_next++].line);
}

/* ---- hal_phone_* ----------------------------------------------------------------- */
int hal_phone_connected(void) { update(); return s_up; }

int hal_phone_read_line(char *buf, int n)
{
    update();
    if (!s_up || !s_qn || n <= 0) return 0;
    snprintf(buf, (size_t)n, "%s", s_q[s_qh]);
    s_qh = (s_qh + 1) % QN;
    s_qn--;
    return (int)strlen(buf);
}

void hal_phone_write_line(const char *line)
{
    update();
    if (!s_up || !line) return;
    static gb_json j;
    if (gb_parse(&j, line, (int)strlen(line)) || j.tok[0].type != GBJ_OBJ) return;
    char t[16], v[24];
    gb_strk(&j, 0, "t", t, sizeof t);
    gb_strk(&j, 0, "n", v, sizeof v);
    if (strcmp(t, "status") && strcmp(t, "act")) {
        char lg[96];
        snprintf(lg, sizeof lg, "phone <- %.80s", line);
        hal_log(lg);
    }
    if (!strcmp(t, "music")) {
        if (!strcmp(v, "play") || !strcmp(v, "pause")) {
            s_pos_ms = player_pos();
            s_pos_t0 = hal_millis();
            s_playing = !strcmp(v, "play");
            send_musicstate();
        } else if (!strcmp(v, "next") || !strcmp(v, "previous")) {
            s_track = (s_track + (v[0] == 'n' ? 1 : NTRACKS - 1)) % NTRACKS;
            s_pos_ms = 0;
            s_pos_t0 = hal_millis();
            s_playing = 1;
            send_musicinfo();
            send_musicstate();
        }
    }
}

/* Display timezone (minutes east of UTC); the sim's epoch follows it. */
void hal_set_tz_min(int tz_min) { sim_tz_min = tz_min; }

void nos_phone_sim_set(int connected) { s_force = connected < 0 ? -1 : connected ? 1 : 0; update(); }

int nos_phone_sim_cmd(const char *s)
{
    if (!s || strncmp(s, "sim", 3) || (s[3] && s[3] != ' ')) return 0;
    const char *a = s + 3;
    while (*a == ' ') a++;
    if (!strcmp(a, "on")) nos_phone_sim_set(1);
    else if (!strcmp(a, "off")) nos_phone_sim_set(0);
    else if (!strcmp(a, "auto")) nos_phone_sim_set(-1);
    else if (!strcmp(a, "notify")) {
        s_notif_release = 1;
        update();
        while (s_notif_next < NNOTIFS) push(NOTIFS[s_notif_next++].line);
    } else if (!strncmp(a, "call", 4) && (a[4] == 0 || a[4] == ' ')) {
        const char *name = a[4] ? a + 5 : "Maya Chen";
        pushf("\x10GB({\"t\":\"call\",\"cmd\":\"incoming\",\"name\":\"%s\",\"number\":\"+91 98450 12345\"})", name);
    } else if (!strcmp(a, "callend")) push("\x10GB({\"t\":\"call\",\"cmd\":\"end\",\"name\":\"\",\"number\":\"\"})");
    else if (!strcmp(a, "find")) push("\x10GB({\"t\":\"find\",\"n\":true})");
    else if (!strcmp(a, "findstop")) push("\x10GB({\"t\":\"find\",\"n\":false})");
    else return 0;
    return 1;
}
