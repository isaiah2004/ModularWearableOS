/* gb.h — Gadgetbridge "Bangle.js" protocol helpers: line framing, a small JSON
 * parser, and text clean-up (Latin-1 / \xNN / \uNNNN -> UTF-8 -> ASCII-safe).
 *
 * Portable C99, no allocation, no statics: the caller owns every buffer (svc.c keeps
 * its gb_json in PSRAM). Used by nos/svc.c and tools/gb_test.c.
 *
 * Phone -> watch lines look like
 *     \x10GB({"t":"notify","id":1,"src":"WhatsApp","title":"Maya","body":"hi"})
 *     \x10setTime(1700556601);E.setTimeZone(1.0);(s=>s&&(...))(...)
 * gb_frame_line() finds the GB(...) JSON payload (brace balanced, strings respected)
 * and the setTime / E.setTimeZone arguments. gb_parse() tokenises the JSON. */
#ifndef NOS_GB_H
#define NOS_GB_H
#include <stdint.h>

#define GB_LINE_MAX 2048          /* longest line the transports keep (longer = dropped) */

/* ---- framing ---------------------------------------------------------------- */
typedef struct {
    const char *json;             /* start of the GB(...) object inside the line, 0 = none */
    int         json_len;
    int         has_time;         /* setTime(<utc seconds>) seen */
    int64_t     epoch;
    int         has_tz;           /* E.setTimeZone(<hours>) seen */
    float       tz_hours;
} gb_frame;
/* Returns 1 if the line carried anything recognised (JSON payload and/or time). */
int gb_frame_line(const char *line, int len, gb_frame *out);

/* ---- JSON -------------------------------------------------------------------- */
enum { GBJ_NONE = 0, GBJ_OBJ, GBJ_ARR, GBJ_STR, GBJ_NUM, GBJ_TRUE, GBJ_FALSE, GBJ_NULL };
typedef struct {
    uint8_t  type;                /* GBJ_* */
    uint8_t  depth;
    uint16_t skip;                /* index of the first token after this subtree */
    int      start, len;          /* byte span in src (strings: inside the quotes) */
    int      count;               /* OBJ: pairs, ARR: items */
} gb_tok;
#define GB_TOK_MAX 192
typedef struct {
    const char *src;
    int         ntok;
    gb_tok      tok[GB_TOK_MAX];  /* pre-order; tok[0] is the root; object children
                                     alternate key (STR), value */
} gb_json;
/* Tolerant: accepts unquoted keys, single-quoted strings, trailing commas and \xNN
 * escapes (Espruino/Gadgetbridge quirks). Returns 0 on success, -1 on error. */
int gb_parse(gb_json *j, const char *src, int len);

int    gb_get(const gb_json *j, int obj, const char *key);   /* value token or -1 */
int    gb_item(const gb_json *j, int arr, int i);            /* array item token or -1 */
/* Value as a number: numbers, numeric strings ("6", "30:" -> 30), true = 1. */
double gb_num(const gb_json *j, int tok, double def);
int    gb_int(const gb_json *j, int obj, const char *key, int def);
double gb_numk(const gb_json *j, int obj, const char *key, double def);
int    gb_bool(const gb_json *j, int obj, const char *key, int def);
int    gb_has(const gb_json *j, int obj, const char *key);
/* Decoded, ASCII-safe copy of a string value (numbers are copied as written).
 * Returns the output length; out is always NUL-terminated ("" if missing). */
int    gb_str(const gb_json *j, int tok, char *out, int n);
int    gb_strk(const gb_json *j, int obj, const char *key, char *out, int n);

/* ---- text ---------------------------------------------------------------------- */
/* Clean an arbitrary byte string for the ASCII dot fonts: UTF-8 is decoded, invalid
 * bytes are taken as Latin-1, accents are folded (e -> e, ss for sharp s), smart quotes,
 * dashes and ellipses become ASCII, emoji / zero-width marks are dropped, anything
 * else becomes '?'. Control characters and runs of spaces collapse to one space;
 * the result is trimmed. Returns the output length. */
int gb_ascii(const char *in, int inlen, char *out, int n);
/* Append one Unicode code point to out (ASCII fold), returns bytes written. */
int gb_fold_cp(uint32_t cp, char *out, int n);
#endif
