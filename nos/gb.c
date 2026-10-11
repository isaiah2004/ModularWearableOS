/* gb.c — see gb.h. Gadgetbridge Bangle.js line framing, JSON, text clean-up. */
#include "gb.h"
#include <string.h>

/* ------------------------------------------------------------------ numbers */
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* Parse a decimal number at s (sign, digits, fraction, exponent). Returns the bytes
 * consumed, 0 if there is no number. */
static int parse_num(const char *s, const char *e, double *out)
{
    const char *p = s;
    double v = 0, sign = 1;
    int digits = 0;
    if (p < e && (*p == '-' || *p == '+')) { if (*p == '-') sign = -1; p++; }
    while (p < e && is_digit(*p)) { v = v * 10 + (*p - '0'); p++; digits++; }
    if (p < e && *p == '.') {
        double f = 0.1;
        p++;
        while (p < e && is_digit(*p)) { v += (*p - '0') * f; f *= 0.1; p++; digits++; }
    }
    if (!digits) return 0;
    if (p < e && (*p == 'e' || *p == 'E')) {
        const char *q = p + 1;
        int es = 1, ev = 0, ed = 0;
        if (q < e && (*q == '-' || *q == '+')) { if (*q == '-') es = -1; q++; }
        while (q < e && is_digit(*q)) { if (ev < 400) ev = ev * 10 + (*q - '0'); q++; ed++; }
        if (ed) {
            p = q;
            while (ev-- > 0) v = es > 0 ? v * 10 : v / 10;
        }
    }
    *out = v * sign;
    return (int)(p - s);
}

/* ------------------------------------------------------------------ framing */
static const char *find(const char *s, const char *e, const char *pat)
{
    int m = (int)strlen(pat);
    for (const char *p = s; p + m <= e; p++)
        if (*p == pat[0] && !memcmp(p, pat, (size_t)m)) return p;
    return 0;
}

int gb_frame_line(const char *line, int len, gb_frame *out)
{
    memset(out, 0, sizeof *out);
    if (!line || len <= 0) return 0;
    const char *e = line + len;
    int any = 0;

    const char *g = find(line, e, "GB(");
    if (g) {
        const char *p = g + 3;
        while (p < e && is_space(*p)) p++;
        if (p < e && *p == '{') {
            const char *s = p;
            int depth = 0;
            char q = 0;
            for (; p < e; p++) {
                char c = *p;
                if (q) {
                    if (c == '\\') { p++; continue; }
                    if (c == q) q = 0;
                    continue;
                }
                if (c == '"' || c == '\'') q = c;
                else if (c == '{' || c == '[') depth++;
                else if (c == '}' || c == ']') {
                    if (--depth == 0) { out->json = s; out->json_len = (int)(p + 1 - s); any = 1; break; }
                }
            }
        }
    } else {
        /* A bare JSON line (some bridges skip the GB() wrapper). */
        const char *p = line;
        while (p < e && (is_space(*p) || *p == 0x10 || *p == 0x03)) p++;
        if (p < e && *p == '{' && find(p, e, "\"t\"")) {
            out->json = p;
            out->json_len = (int)(e - p);
            while (out->json_len > 0 && (is_space(p[out->json_len - 1]) || p[out->json_len - 1] == ';'))
                out->json_len--;
            any = 1;
        }
    }

    const char *t = find(line, e, "setTime(");
    if (t) {
        double v;
        if (parse_num(t + 8, e, &v) && v > 0) { out->has_time = 1; out->epoch = (int64_t)v; any = 1; }
    }
    const char *z = find(line, e, "setTimeZone(");
    if (z) {
        double v;
        if (parse_num(z + 12, e, &v) && v >= -14 && v <= 14) { out->has_tz = 1; out->tz_hours = (float)v; any = 1; }
    }
    return any;
}

/* --------------------------------------------------------------------- JSON */
typedef struct { const char *s; int len, pos; gb_json *j; } jp;

static void jws(jp *p) { while (p->pos < p->len && is_space(p->s[p->pos])) p->pos++; }

static int jadd(jp *p, int type, int depth, int start)
{
    if (p->j->ntok >= GB_TOK_MAX) return -1;
    int i = p->j->ntok++;
    gb_tok *t = &p->j->tok[i];
    t->type = (uint8_t)type;
    t->depth = (uint8_t)depth;
    t->skip = (uint16_t)(i + 1);
    t->start = start;
    t->len = 0;
    t->count = 0;
    return i;
}

static int jstring(jp *p, int depth)
{
    char q = p->s[p->pos++];
    int start = p->pos;
    while (p->pos < p->len && p->s[p->pos] != q) {
        if (p->s[p->pos] == '\\') p->pos++;
        p->pos++;
    }
    if (p->pos >= p->len) return -1;
    int i = jadd(p, GBJ_STR, depth, start);
    if (i < 0) return -1;
    p->j->tok[i].len = p->pos - start;
    p->pos++;                              /* closing quote */
    return i;
}

static int is_ident(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) || c == '_' || c == '$';
}

static int jvalue(jp *p, int depth);

static int jcontainer(jp *p, int depth, int is_obj)
{
    char close = is_obj ? '}' : ']';
    int i = jadd(p, is_obj ? GBJ_OBJ : GBJ_ARR, depth, p->pos);
    if (i < 0) return -1;
    p->pos++;
    int count = 0;
    for (;;) {
        jws(p);
        if (p->pos >= p->len) return -1;
        if (p->s[p->pos] == close) { p->pos++; break; }
        if (is_obj) {
            char c = p->s[p->pos];
            if (c == '"' || c == '\'') { if (jstring(p, depth + 1) < 0) return -1; }
            else if (is_ident(c)) {                 /* unquoted key */
                int start = p->pos;
                while (p->pos < p->len && is_ident(p->s[p->pos])) p->pos++;
                int k = jadd(p, GBJ_STR, depth + 1, start);
                if (k < 0) return -1;
                p->j->tok[k].len = p->pos - start;
            } else return -1;
            jws(p);
            if (p->pos >= p->len || p->s[p->pos] != ':') return -1;
            p->pos++;
        }
        if (jvalue(p, depth + 1) < 0) return -1;
        count++;
        jws(p);
        if (p->pos >= p->len) return -1;
        if (p->s[p->pos] == ',') { p->pos++; continue; }
        if (p->s[p->pos] == close) { p->pos++; break; }
        return -1;
    }
    gb_tok *t = &p->j->tok[i];
    t->count = count;
    t->skip = (uint16_t)p->j->ntok;
    t->len = p->pos - t->start;
    return i;
}

static int jword(jp *p, const char *w)
{
    int n = (int)strlen(w);
    if (p->pos + n <= p->len && !memcmp(p->s + p->pos, w, (size_t)n) &&
        (p->pos + n == p->len || !is_ident(p->s[p->pos + n]))) { p->pos += n; return 1; }
    return 0;
}

static int jvalue(jp *p, int depth)
{
    if (depth > 24) return -1;
    jws(p);
    if (p->pos >= p->len) return -1;
    char c = p->s[p->pos];
    if (c == '{') return jcontainer(p, depth, 1);
    if (c == '[') return jcontainer(p, depth, 0);
    if (c == '"' || c == '\'') return jstring(p, depth);
    if (c == '-' || c == '+' || c == '.' || is_digit(c)) {
        double v;
        int n = parse_num(p->s + p->pos, p->s + p->len, &v);
        if (!n) return -1;
        int i = jadd(p, GBJ_NUM, depth, p->pos);
        if (i < 0) return -1;
        p->j->tok[i].len = n;
        p->pos += n;
        return i;
    }
    int start = p->pos, type;
    if (jword(p, "true")) type = GBJ_TRUE;
    else if (jword(p, "false")) type = GBJ_FALSE;
    else if (jword(p, "null") || jword(p, "undefined")) type = GBJ_NULL;
    else return -1;
    int i = jadd(p, type, depth, start);
    if (i >= 0) p->j->tok[i].len = p->pos - start;
    return i;
}

int gb_parse(gb_json *j, const char *src, int len)
{
    j->src = src;
    j->ntok = 0;
    if (!src || len <= 0) return -1;
    jp p = { src, len, 0, j };
    if (jvalue(&p, 0) < 0) { j->ntok = 0; return -1; }
    return 0;
}

int gb_get(const gb_json *j, int obj, const char *key)
{
    if (!j || obj < 0 || obj >= j->ntok || j->tok[obj].type != GBJ_OBJ) return -1;
    int klen = (int)strlen(key);
    int i = obj + 1;
    for (int k = 0; k < j->tok[obj].count && i + 1 < j->ntok; k++) {
        const gb_tok *kt = &j->tok[i];
        if (kt->len == klen && !memcmp(j->src + kt->start, key, (size_t)klen)) return i + 1;
        i = j->tok[i + 1].skip;
    }
    return -1;
}

int gb_item(const gb_json *j, int arr, int i)
{
    if (!j || arr < 0 || arr >= j->ntok || j->tok[arr].type != GBJ_ARR) return -1;
    if (i < 0 || i >= j->tok[arr].count) return -1;
    int t = arr + 1;
    while (i-- > 0 && t < j->ntok) t = j->tok[t].skip;
    return t < j->ntok ? t : -1;
}

double gb_num(const gb_json *j, int tok, double def)
{
    if (!j || tok < 0 || tok >= j->ntok) return def;
    const gb_tok *t = &j->tok[tok];
    double v;
    switch (t->type) {
    case GBJ_NUM:
        return parse_num(j->src + t->start, j->src + t->start + t->len, &v) ? v : def;
    case GBJ_STR: {
        const char *s = j->src + t->start, *e = s + t->len;
        while (s < e && is_space(*s)) s++;
        return parse_num(s, e, &v) ? v : def;
    }
    case GBJ_TRUE: return 1;
    case GBJ_FALSE: return 0;
    }
    return def;
}
int gb_int(const gb_json *j, int obj, const char *key, int def)
{
    double v = gb_num(j, gb_get(j, obj, key), (double)def);
    if (v > 2147483647.0) v = 2147483647.0;
    if (v < -2147483648.0) v = -2147483648.0;
    return (int)(v < 0 ? v - 0.5 : v + 0.5);
}
double gb_numk(const gb_json *j, int obj, const char *key, double def)
{
    return gb_num(j, gb_get(j, obj, key), def);
}
int gb_bool(const gb_json *j, int obj, const char *key, int def)
{
    int t = gb_get(j, obj, key);
    if (t < 0) return def;
    switch (j->tok[t].type) {
    case GBJ_TRUE: return 1;
    case GBJ_FALSE: case GBJ_NULL: return 0;
    case GBJ_NUM: return gb_num(j, t, 0) != 0;
    case GBJ_STR: {
        const char *s = j->src + j->tok[t].start;
        int n = j->tok[t].len;
        if ((n == 4 && !memcmp(s, "true", 4)) || (n == 1 && s[0] == '1')) return 1;
        if ((n == 5 && !memcmp(s, "false", 5)) || (n == 1 && s[0] == '0')) return 0;
        return def;
    }
    }
    return def;
}
int gb_has(const gb_json *j, int obj, const char *key) { return gb_get(j, obj, key) >= 0; }

/* --------------------------------------------------------------------- text */
/* Byte source: un-escapes JSON (\n, \xNN, \uNNNN as UTF-8) when esc is set. */
typedef struct { const char *s; int pos, end, esc; uint8_t pend[4]; int np, ip; } bsrc;

static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int hexn(const bsrc *b, int at, int n)
{
    if (at + n > b->end) return -1;
    int v = 0;
    for (int k = 0; k < n; k++) {
        int h = hexv((unsigned char)b->s[at + k]);
        if (h < 0) return -1;
        v = v * 16 + h;
    }
    return v;
}
static int utf8_put(uint32_t cp, uint8_t *o)
{
    if (cp < 0x80) { o[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) { o[0] = (uint8_t)(0xC0 | (cp >> 6)); o[1] = (uint8_t)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        o[0] = (uint8_t)(0xE0 | (cp >> 12)); o[1] = (uint8_t)(0x80 | ((cp >> 6) & 63));
        o[2] = (uint8_t)(0x80 | (cp & 63)); return 3;
    }
    o[0] = (uint8_t)(0xF0 | (cp >> 18)); o[1] = (uint8_t)(0x80 | ((cp >> 12) & 63));
    o[2] = (uint8_t)(0x80 | ((cp >> 6) & 63)); o[3] = (uint8_t)(0x80 | (cp & 63)); return 4;
}
static int bs_next(bsrc *b)
{
    if (b->ip < b->np) return b->pend[b->ip++];
    if (b->pos >= b->end) return -1;
    int c = (unsigned char)b->s[b->pos++];
    if (!b->esc || c != '\\' || b->pos >= b->end) return c;
    int e = (unsigned char)b->s[b->pos++];
    switch (e) {
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'b': return 8;
    case 'f': return 12;
    case '0': return 0;
    case 'x': {
        int v = hexn(b, b->pos, 2);
        if (v < 0) return 'x';
        b->pos += 2;
        return v;                       /* a raw byte: Latin-1 or part of UTF-8 */
    }
    case 'u': {
        int v = hexn(b, b->pos, 4);
        if (v < 0) return 'u';
        b->pos += 4;
        uint32_t cp = (uint32_t)v;
        if (cp >= 0xD800 && cp <= 0xDBFF && b->pos + 6 <= b->end &&
            b->s[b->pos] == '\\' && b->s[b->pos + 1] == 'u') {
            int lo = hexn(b, b->pos + 2, 4);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (uint32_t)(lo - 0xDC00);
                b->pos += 6;
            }
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        b->np = utf8_put(cp, b->pend);
        b->ip = 1;
        return b->pend[0];
    }
    default: return e;                  /* \" \\ \/ \' and unknown escapes */
    }
}
/* Next code point: UTF-8 when valid, otherwise the byte as Latin-1. -1 at end. */
static int32_t bs_cp(bsrc *b)
{
    int c = bs_next(b);
    if (c < 0x80) return c;
    int need = (c >= 0xC2 && c <= 0xDF) ? 1 : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xF0 && c <= 0xF4) ? 3 : 0;
    if (!need) return c;
    bsrc save = *b;
    uint32_t cp = (uint32_t)c & (need == 1 ? 0x1F : need == 2 ? 0x0F : 0x07);
    for (int k = 0; k < need; k++) {
        int d = bs_next(b);
        if (d < 0 || (d & 0xC0) != 0x80) { *b = save; return c; }
        cp = (cp << 6) | (uint32_t)(d & 0x3F);
    }
    if ((need == 2 && cp < 0x800) || (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
        (cp >= 0xD800 && cp <= 0xDFFF)) { *b = save; return c; }
    return (int32_t)cp;
}

/* Latin-1 supplement 0xA0-0xFF, one entry per code point. */
static const char *const LATIN1[96] = {
    " ", "!", "c", "L", "$", "Y", "|", "S", "\"", "(c)", "a", "<<", "-", "", "(R)", "-",
    "", "+/-", "2", "3", "'", "u", "P", "-", ",", "1", "o", ">>", "1/4", "1/2", "3/4", "?",
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
    "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y",
};
/* Latin Extended-A 0x100-0x17F: base letter per code point. */
static const char EXT_A[129] =
    "AaAaAa" "CcCcCcCc" "DdDd" "EeEeEeEeEe" "GgGgGgGg" "HhHh" "IiIiIiIiIi" "Ii" "Jj" "Kkk"
    "LlLlLlLlLl" "NnNnNnnNn" "OoOoOo" "Oo" "RrRrRr" "SsSsSsSs" "TtTtTt" "UuUuUuUuUuUu" "Ww"
    "YyY" "ZzZzZz" "s";
typedef char ext_a_size_check[sizeof EXT_A == 129 ? 1 : -1];

static const char *fold(uint32_t cp, char *one)
{
    if (cp < 32 || cp == 127) return " ";
    if (cp < 127) { one[0] = (char)cp; one[1] = 0; return one; }
    if (cp < 0xA0) return "";                         /* C1 controls */
    if (cp <= 0xFF) return LATIN1[cp - 0xA0];
    if (cp <= 0x17F) { one[0] = EXT_A[cp - 0x100]; one[1] = 0; return one; }
    if (cp >= 0x300 && cp <= 0x36F) return "";        /* combining accents */
    switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: case 0x02BC: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
    case 0x2026: return "...";
    case 0x2022: case 0x2023: case 0x25CF: return "*";
    case 0x20AC: return "EUR";
    case 0x20B9: return "Rs";
    case 0x2122: return "TM";
    case 0x2190: return "<-";
    case 0x2192: return "->";
    case 0xFFFD: return "?";
    }
    if (cp >= 0x2000 && cp <= 0x200A) return " ";     /* typographic spaces */
    if ((cp >= 0x200B && cp <= 0x200F) || cp == 0x2060 || cp == 0xFEFF) return "";  /* zero width */
    if (cp >= 0xFE00 && cp <= 0xFE0F) return "";      /* variation selectors */
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return "";    /* emoji, pictographs */
    if (cp >= 0x2600 && cp <= 0x27BF) return "";      /* misc symbols, dingbats */
    if (cp >= 0x2B00 && cp <= 0x2BFF) return "";
    if (cp >= 0xE0000 && cp <= 0xE007F) return "";    /* emoji tags */
    return "?";
}

typedef struct { char *o; int n, len, sp; } emit;
static void em_str(emit *e, const char *s)
{
    for (; *s; s++) {
        char c = *s;
        if (c == ' ') { if (e->sp) continue; e->sp = 1; }
        else e->sp = 0;
        if (e->len < e->n - 1) e->o[e->len++] = c;
    }
}
static int em_end(emit *e)
{
    if (e->n <= 0) return 0;
    while (e->len > 0 && e->o[e->len - 1] == ' ') e->len--;
    e->o[e->len] = 0;
    return e->len;
}
static int decode(const char *s, int len, int esc, char *out, int n)
{
    if (!out || n <= 0) return 0;
    emit e = { out, n, 0, 1 };
    bsrc b;
    memset(&b, 0, sizeof b);
    b.s = s; b.end = s ? len : 0; b.esc = esc;
    char one[2];
    int32_t cp;
    while ((cp = bs_cp(&b)) >= 0) {
        em_str(&e, fold((uint32_t)cp, one));
        if (e.len >= n - 1) break;
    }
    return em_end(&e);
}

int gb_ascii(const char *in, int inlen, char *out, int n)
{
    if (in && inlen < 0) inlen = (int)strlen(in);
    return decode(in, in ? inlen : 0, 0, out, n);
}

int gb_fold_cp(uint32_t cp, char *out, int n)
{
    char one[2];
    const char *s = fold(cp, one);
    int k = 0;
    while (s[k] && k < n - 1) { out[k] = s[k]; k++; }
    if (n > 0) out[k] = 0;
    return k;
}

int gb_str(const gb_json *j, int tok, char *out, int n)
{
    if (!out || n <= 0) return 0;
    out[0] = 0;
    if (!j || tok < 0 || tok >= j->ntok) return 0;
    const gb_tok *t = &j->tok[tok];
    switch (t->type) {
    case GBJ_STR: return decode(j->src + t->start, t->len, 1, out, n);
    case GBJ_NUM: case GBJ_TRUE: case GBJ_FALSE: return decode(j->src + t->start, t->len, 0, out, n);
    }
    return 0;
}
int gb_strk(const gb_json *j, int obj, const char *key, char *out, int n)
{
    return gb_str(j, gb_get(j, obj, key), out, n);
}
