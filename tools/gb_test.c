/* gb_test.c — feeds Gadgetbridge Bangle.js sample lines (from https://www.espruino.com/Gadgetbridge
 * and the Gadgetbridge sources) through nos/gb.c and prints what was parsed.
 *   gcc -std=c99 -Wall -Wextra -Inos tools/gb_test.c nos/gb.c -o build/gb_test.exe
 * Exit code = number of failed checks. */
#include <stdio.h>
#include <string.h>
#include "gb.h"

static int fails, checks;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { fails++; printf("  FAIL: %s\n", what); }
}
static void check_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (strcmp(got, want)) { fails++; printf("  FAIL: %s: got \"%s\" want \"%s\"\n", what, got, want); }
}

static gb_json J;

/* Print every key/value of the root object (one level, arrays summarised). */
static void dump(const gb_json *j, int obj, int indent)
{
    int i = obj + 1;
    for (int k = 0; k < j->tok[obj].count; k++) {
        char key[32], val[200];
        gb_str(j, i, key, sizeof key);
        int v = i + 1;
        const gb_tok *t = &j->tok[v];
        printf("%*s%-9s ", indent, "", key);
        if (t->type == GBJ_OBJ) { printf("{\n"); dump(j, v, indent + 2); printf("%*s}\n", indent, ""); }
        else if (t->type == GBJ_ARR) {
            printf("[%d items]\n", t->count);
            for (int a = 0; a < t->count; a++) {
                int it = gb_item(j, v, a);
                if (j->tok[it].type == GBJ_OBJ) { printf("%*s[%d] {\n", indent + 2, "", a); dump(j, it, indent + 4); printf("%*s}\n", indent + 2, ""); }
                else { gb_str(j, it, val, sizeof val); printf("%*s[%d] %s\n", indent + 2, "", a, val); }
            }
        } else if (t->type == GBJ_STR) { gb_str(j, v, val, sizeof val); printf("\"%s\"\n", val); }
        else if (t->type == GBJ_NUM) printf("%g\n", gb_num(j, v, 0));
        else printf("%s\n", t->type == GBJ_TRUE ? "true" : t->type == GBJ_FALSE ? "false" : "null");
        i = j->tok[v].skip;
    }
}

/* Frame + parse one raw line as the transport delivers it. Returns 1 if JSON parsed. */
static int feed(const char *line)
{
    gb_frame f;
    printf("\n> %s\n", line);
    int any = gb_frame_line(line, (int)strlen(line), &f);
    if (!any) { printf("  (nothing recognised)\n"); return 0; }
    if (f.has_time) printf("  setTime  %lld\n", (long long)f.epoch);
    if (f.has_tz) printf("  timezone %+.2f h\n", f.tz_hours);
    if (!f.json) return 0;
    if (gb_parse(&J, f.json, f.json_len)) { printf("  JSON PARSE ERROR (%d bytes)\n", f.json_len); return 0; }
    printf("  %d tokens\n", J.ntok);
    dump(&J, 0, 2);
    return 1;
}

int main(void)
{
    char s[200];

    /* ---- phone -> watch samples, verbatim from the Espruino Gadgetbridge page ---- */
    check(feed("\x10GB({\"t\":\"notify\",\"id\":1575479849,\"src\":\"Hangouts\",\"title\":\"A Name\",\"body\":\"message contents\"})"), "notify parses");
    check(gb_int(&J, 0, "id", 0) == 1575479849, "notify id");
    gb_strk(&J, 0, "src", s, sizeof s); check_str(s, "Hangouts", "notify src");
    gb_strk(&J, 0, "body", s, sizeof s); check_str(s, "message contents", "notify body");

    check(feed("\x10GB({\"t\":\"notify~\",\"id\":1575479849,\"body\":\"this changed\"})"), "notify~ parses");
    gb_strk(&J, 0, "t", s, sizeof s); check_str(s, "notify~", "notify~ type");

    check(feed("\x10GB({\"t\":\"notify-\",\"id\":1575479849});"), "notify- parses (trailing ;)");
    check(gb_int(&J, 0, "id", 0) == 1575479849, "notify- id");

    check(feed("\x10GB({\"t\":\"musicstate\",\"state\":\"play\",\"position\":0,\"shuffle\":1,\"repeat\":1})"), "musicstate");
    gb_strk(&J, 0, "state", s, sizeof s); check_str(s, "play", "musicstate state");

    check(feed("\x10GB({\"t\":\"musicinfo\",\"artist\":\"My Artist\",\"album\":\"My Album\",\"track\":\"Track One\",\"dur\":241,\"c\":2,\"n\":2})"), "musicinfo");
    check(gb_int(&J, 0, "dur", 0) == 241, "musicinfo dur");

    check(feed("\x10GB({\"t\":\"call\",\"cmd\":\"incoming\",\"name\":\"name\",\"number\":\"+441234123123\"})"), "call");
    gb_strk(&J, 0, "number", s, sizeof s); check_str(s, "+441234123123", "call number");

    /* the docs' alarm sample really has string values and a stray ':' — must still work */
    check(feed("\x10GB({\"t\":\"alarm\", \"d\":[{\"h\":\"6\",\"m\":\"30:\",\"rep\":127}]})"), "alarm (docs sample)");
    {
        int d = gb_get(&J, 0, "d"), a0 = gb_item(&J, d, 0);
        check(gb_int(&J, a0, "h", -1) == 6 && gb_int(&J, a0, "m", -1) == 30 && gb_int(&J, a0, "rep", -1) == 127, "alarm h/m/rep");
    }
    check(feed("\x10GB({\"t\":\"alarm\",\"d\":[{\"h\":7,\"m\":15,\"rep\":31,\"on\":true},{\"h\":9,\"m\":0,\"rep\":96,\"on\":false}]})"), "alarm (Gadgetbridge form)");
    {
        int d = gb_get(&J, 0, "d");
        check(J.tok[d].count == 2 && gb_bool(&J, gb_item(&J, d, 1), "on", 1) == 0, "alarm 2 items, second off");
    }

    check(feed("\x10GB({\"t\":\"nav\",\"instr\":\"High St towards Tollgate Rd\",\"distance\":\"966yd\",\"action\":\"continue\",\"eta\":\"08:39\"})"), "nav");
    check(feed("\x10GB({\"t\":\"gps\",\"lat\":51.509865,\"lon\":-0.118092,\"alt\":42,\"speed\":0,\"course\":12.5,\"time\":1700000000000,\"satellites\":7,\"hdop\":1.2,\"externalSource\":true})"), "gps");
    check(gb_numk(&J, 0, "lon", 0) < -0.118 && gb_numk(&J, 0, "lon", 0) > -0.119, "gps lon");

    /* weather as Gadgetbridge sends it (Kelvin, OWM code) */
    check(feed("\x10GB({\"t\":\"weather\",\"temp\":291,\"hi\":294,\"lo\":285,\"hum\":72,\"rain\":40,\"uv\":3,\"code\":500,\"txt\":\"light rain\",\"wind\":14.4,\"wdir\":225,\"loc\":\"Bengaluru\"})"), "weather");
    check(gb_int(&J, 0, "code", 0) == 500 && gb_numk(&J, 0, "wind", 0) > 14.3, "weather code/wind");

    check(feed("\x10GB({\"t\":\"find\",\"n\":true})"), "find");
    check(gb_bool(&J, 0, "n", 0) == 1, "find n");
    check(feed("\x10GB({\"t\":\"act\",\"hrm\":false,\"stp\":true,\"int\":10})"), "act");
    check(gb_bool(&J, 0, "stp", 0) == 1 && gb_int(&J, 0, "int", 0) == 10, "act fields");
    check(feed("\x10GB({\"t\":\"vibrate\",\"n\":2})"), "vibrate");
    check(feed("\x10GB({\"t\":\"is_gps_active\"})"), "is_gps_active");
    check(feed("\x10GB({\"t\": \"http\", \"id\": \"89875941444\", \"resp\": \"Hello World!\\n\"})"), "http resp with spaces");
    gb_strk(&J, 0, "resp", s, sizeof s); check_str(s, "Hello World!", "http resp unescaped + trimmed");

    /* time sync line exactly as BangleJSDeviceSupport.setTime() builds it */
    {
        const char *l = "\x10setTime(1700556601);E.setTimeZone(5.5);(s=>s&&(s.timezone=5.5,require('Storage').write('setting.json',s)))(require('Storage').readJSON('setting.json',1))";
        gb_frame f;
        feed(l);
        gb_frame_line(l, (int)strlen(l), &f);
        check(f.has_time && f.epoch == 1700556601 && f.has_tz && f.tz_hours == 5.5f && !f.json, "setTime + tz");
        const char *l2 = "\x10setTime(1700556601);E.setTimeZone(-3.0);";
        gb_frame_line(l2, (int)strlen(l2), &f);
        check(f.has_tz && f.tz_hours == -3.0f, "negative tz");
    }

    /* ---- non-ASCII: raw Latin-1, \xNN, \uNNNN, raw UTF-8, surrogate emoji ---- */
    check(feed("\x10GB({\"t\":\"notify\",\"id\":7,\"src\":\"WhatsApp\",\"title\":\"J\xf6rg M\xfcller\",\"body\":\"Gr\xfc\xdf" "e aus K\xf6ln\"})"), "raw Latin-1 bytes");
    gb_strk(&J, 0, "title", s, sizeof s); check_str(s, "Jorg Muller", "Latin-1 title folded");
    gb_strk(&J, 0, "body", s, sizeof s); check_str(s, "Grusse aus Koln", "Latin-1 sharp s");

    check(feed("\x10GB({\"t\":\"notify\",\"id\":8,\"src\":\"Messages\",\"title\":\"Zo\\xeb\",\"body\":\"Caf\\xe9 at 5? \\u2014 \\u201cyes\\u201d\"})"), "\\xNN and \\u escapes");
    gb_strk(&J, 0, "title", s, sizeof s); check_str(s, "Zoe", "\\xNN title");
    gb_strk(&J, 0, "body", s, sizeof s); check_str(s, "Cafe at 5? - \"yes\"", "\\u dash and quotes");

    check(feed("\x10GB({\"t\":\"notify\",\"id\":9,\"src\":\"Telegram\",\"title\":\"Ren\xc3\xa9\",\"body\":\"On my way \xf0\x9f\x9a\x97\xf0\x9f\x92\xa8 see you\xe2\x80\xa6\"})"), "raw UTF-8 + emoji");
    gb_strk(&J, 0, "title", s, sizeof s); check_str(s, "Rene", "UTF-8 accent");
    gb_strk(&J, 0, "body", s, sizeof s); check_str(s, "On my way see you...", "emoji dropped, ellipsis");

    check(feed("\x10GB({\"t\":\"notify\",\"id\":10,\"src\":\"Signal\",\"body\":\"ok \\ud83d\\udc4d\\n\\nthanks\\tagain\"})"), "surrogate pair + controls");
    gb_strk(&J, 0, "body", s, sizeof s); check_str(s, "ok thanks again", "surrogate dropped, whitespace collapsed");

    check(feed("\x10GB({\"t\":\"notify\",\"id\":11,\"src\":\"Gmail\",\"title\":\"\\xc3\\xa9t\\xc3\\xa9\",\"body\":\"\\u0410\\u0411 \\u00b7 \\u20ac5\"})"), "escaped UTF-8 bytes, Cyrillic");
    gb_strk(&J, 0, "title", s, sizeof s); check_str(s, "ete", "\\x-escaped UTF-8 decoded as UTF-8");
    gb_strk(&J, 0, "body", s, sizeof s); check_str(s, "?? - EUR5", "Cyrillic -> ?, middle dot, euro");

    /* ---- robustness ---- */
    check(!feed("\x10GB({\"t\":\"notify\",\"id\":12,\"body\":\"cut off"), "truncated line rejected");
    check(feed("GB({t:'notify', id:13, src:'Espruino', title:'unquoted keys', body:'single quotes',})"), "JS-style object");
    gb_strk(&J, 0, "title", s, sizeof s); check_str(s, "unquoted keys", "unquoted key lookup");
    check(feed("{\"t\":\"notify\",\"id\":14,\"title\":\"bare json line\"}"), "bare JSON line");
    check(!feed("\x03"), "Ctrl-C clear line ignored");
    check(!feed("   "), "blank line ignored");
    check(feed("\x10GB({\"t\":\"notify\",\"id\":15,\"title\":\"brace } in \\\"string\\\" {\",\"body\":\"x\"})"), "braces inside strings");
    gb_strk(&J, 0, "title", s, sizeof s); check_str(s, "brace } in \"string\" {", "string braces");
    {   /* deep nesting must not crash */
        char deep[400] = "\x10GB(";
        for (int i = 0; i < 60; i++) strcat(deep, "[");
        for (int i = 0; i < 60; i++) strcat(deep, "]");
        strcat(deep, ")");
        gb_frame f;
        gb_frame_line(deep, (int)strlen(deep), &f);
        check(!f.json || gb_parse(&J, f.json, f.json_len) != 0, "deep nesting rejected");
    }
    {   /* output buffer smaller than the string */
        feed("\x10GB({\"t\":\"notify\",\"id\":16,\"body\":\"A long message that will not fit\"})");
        char small[8];
        int n = gb_strk(&J, 0, "body", small, sizeof small);
        check(n == 6 && !strcmp(small, "A long"), "truncation to buffer (trailing space trimmed)");
        gb_strk(&J, 0, "missing", s, sizeof s); check_str(s, "", "missing key -> empty");
        check(gb_int(&J, 0, "missing", -5) == -5, "missing int -> default");
    }
    {   /* gb_ascii on plain C strings (used for local and companion text) */
        gb_ascii("Today 15:30 \xc2\xb7 Studio 2", -1, s, sizeof s);
        check_str(s, "Today 15:30 - Studio 2", "gb_ascii middle dot");
        gb_ascii("Isaiah shared \xe2\x80\x98Watch OS v3\xe2\x80\x99 with you", -1, s, sizeof s);
        check_str(s, "Isaiah shared 'Watch OS v3' with you", "gb_ascii smart quotes");
    }

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails;
}
