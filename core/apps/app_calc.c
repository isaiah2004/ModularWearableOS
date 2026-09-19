/* app_calc.c — a working touch calculator for the round watch.
 *
 * =====================  PLAIN-ENGLISH ALGORITHM  =====================
 *
 * UI STATE
 *   - s_expr[]  : the expression the user is typing, e.g. "12+3*4".
 *   - s_result[]: the last valid evaluation of s_expr, shown big below.
 *   - s_error   : set when '=' hit a syntax error / divide-by-zero -> "ERR".
 *   - s_evaluated: set right after '=' so the next digit starts a fresh
 *                  expression while the next operator chains off the result.
 *   Buttons are a 4x4 round-button grid, laid out on the 240 design grid
 *   and scaled to 466 with GFX_S(). A TAP is hit-tested against each
 *   button's circle (dx*dx+dy*dy <= r*r).
 *     [7 8 9 /]
 *     [4 5 6 *]
 *     [1 2 3 -]
 *     [C 0 = +]
 *   Digits/'.'/operators append to s_expr; 'C' clears; '=' evaluates.
 *
 * EVALUATOR (recursive-descent, correct operator precedence)
 *   A cursor walks the string. The grammar encodes precedence directly:
 *     expr   := term  (('+'|'-') term)*     <- lowest precedence, left-assoc
 *     term   := factor(('*'|'/') factor)*   <- higher precedence, left-assoc
 *     factor := ['+'|'-'] factor            <- unary sign
 *             | '(' expr ')'                <- grouping
 *             | number
 *     number := digit+ ['.' digit+]         <- decimals supported
 *   Because '*'/'/' live one level deeper than '+'/'-', "2+3*4" naturally
 *   evaluates the multiply first (=14). Left-recursion in the loops gives
 *   left-associativity, so "7-2-1" == 4. A shared s_eval_err flag is raised
 *   on: divide-by-zero, a missing number, an unbalanced ')', or trailing
 *   junk after a complete parse. eval() returns the value and the flag.
 *
 * The evaluator is pure and header-free, so it is exercised directly by the
 * SELFTEST main() below (compile with -DSELFTEST) independent of any UI.
 * ====================================================================== */

#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include <stdio.h>
#include <string.h>

/* ----------------------------- evaluator ----------------------------- */

static int s_eval_err;   /* raised by the parse routines below */

static void skip_ws(const char **p) { while (**p == ' ') (*p)++; }

static double parse_expr(const char **p);   /* forward decl */

static double parse_number(const char **p)
{
    double v = 0.0;
    int digits = 0;
    while (**p >= '0' && **p <= '9') { v = v * 10.0 + (**p - '0'); (*p)++; digits++; }
    if (**p == '.') {
        (*p)++;
        double frac = 0.1;
        while (**p >= '0' && **p <= '9') { v += (**p - '0') * frac; frac *= 0.1; (*p)++; digits++; }
    }
    if (!digits) s_eval_err = 1;   /* '.' or nothing where a number was due */
    return v;
}

static double parse_factor(const char **p)
{
    skip_ws(p);
    if (**p == '+') { (*p)++; return  parse_factor(p); }  /* unary plus  */
    if (**p == '-') { (*p)++; return -parse_factor(p); }  /* unary minus */
    if (**p == '(') {
        (*p)++;
        double v = parse_expr(p);
        skip_ws(p);
        if (**p == ')') (*p)++; else s_eval_err = 1;
        return v;
    }
    return parse_number(p);
}

static double parse_term(const char **p)
{
    double v = parse_factor(p);
    for (;;) {
        skip_ws(p);
        char c = **p;
        if (c == '*') { (*p)++; v *= parse_factor(p); }
        else if (c == '/') {
            (*p)++;
            double r = parse_factor(p);
            if (r == 0.0) { s_eval_err = 1; return 0.0; }
            v /= r;
        } else break;
    }
    return v;
}

static double parse_expr(const char **p)
{
    double v = parse_term(p);
    for (;;) {
        skip_ws(p);
        char c = **p;
        if (c == '+') { (*p)++; v += parse_term(p); }
        else if (c == '-') { (*p)++; v -= parse_term(p); }
        else break;
    }
    return v;
}

/* Evaluate `s`. Returns the value; sets *err to nonzero on any error. */
static double eval(const char *s, int *err)
{
    const char *p = s;
    s_eval_err = 0;
    double v = parse_expr(&p);
    skip_ws(&p);
    if (*p != '\0') s_eval_err = 1;   /* leftover chars => malformed input */
    if (err) *err = s_eval_err;
    return v;
}

/* ============================ SELFTEST ============================== */
#ifdef SELFTEST
#include <assert.h>
#include <math.h>

static int approx(double a, double b) { return fabs(a - b) < 1e-9; }

int main(void)
{
    int err;
    double v;
    int pass = 0;

    v = eval("2+3*4", &err);       assert(!err && approx(v, 14.0));   pass++;
    printf("[ok] 2+3*4 = %g\n", v);

    v = eval("10/4", &err);        assert(!err && approx(v, 2.5));    pass++;
    printf("[ok] 10/4 = %g\n", v);

    v = eval("7-2-1", &err);       assert(!err && approx(v, 4.0));    pass++;
    printf("[ok] 7-2-1 = %g\n", v);

    v = eval("2*3+4*5", &err);     assert(!err && approx(v, 26.0));   pass++;
    printf("[ok] 2*3+4*5 = %g\n", v);

    v = eval("1/0", &err);         assert(err);                       pass++;
    printf("[ok] 1/0 -> error\n");

    /* extra coverage: decimals, precedence, parens, unary, chaining, junk */
    v = eval("3.5+1.5", &err);     assert(!err && approx(v, 5.0));    pass++;
    printf("[ok] 3.5+1.5 = %g\n", v);

    v = eval("2+3*4-6/2", &err);   assert(!err && approx(v, 11.0));   pass++;
    printf("[ok] 2+3*4-6/2 = %g\n", v);

    v = eval("(2+3)*4", &err);     assert(!err && approx(v, 20.0));   pass++;
    printf("[ok] (2+3)*4 = %g\n", v);

    v = eval("-3+5", &err);        assert(!err && approx(v, 2.0));    pass++;
    printf("[ok] -3+5 = %g\n", v);

    v = eval("0.1+0.2", &err);     assert(!err && approx(v, 0.3));    pass++;
    printf("[ok] 0.1+0.2 = %g\n", v);

    v = eval("10*0.5", &err);      assert(!err && approx(v, 5.0));    pass++;
    printf("[ok] 10*0.5 = %g\n", v);

    v = eval("2+", &err);          assert(err);                       pass++;
    printf("[ok] 2+ -> error (incomplete)\n");

    v = eval("5/0.0", &err);       assert(err);                       pass++;
    printf("[ok] 5/0.0 -> error\n");

    v = eval("100-10*9", &err);    assert(!err && approx(v, 10.0));   pass++;
    printf("[ok] 100-10*9 = %g\n", v);

    printf("\nALL %d SELFTEST CASES PASSED\n", pass);
    return 0;
}

/* ============================ REAL APP ============================== */
#else

#define BTN_R GFX_S(14)

/* 4x4 grid, row-major. '\0' marks (none). */
static const char  s_lab[16]  = { '7','8','9','/',
                                  '4','5','6','*',
                                  '1','2','3','-',
                                  'C','0','=','+' };

typedef struct { float cx, cy; } cbtn;
static cbtn  s_btn[16];
static int   s_laid;

static char  s_expr[48];
static char  s_result[32] = "0";
static int   s_error;
static int   s_evaluated;

static void layout(void)
{
    /* columns x = 69,103,137,171 ; rows y = 100,132,164,196 (240 grid).
     * Kept in the lower band so the 4-wide grid stays inside r=120 and the
     * top of the screen is free for the display. Column pitch (34) is tuned so
     * the smaller discs leave an even gap in both axes and never touch. */
    const float colx[4] = { 69.0f, 103.0f, 137.0f, 171.0f };
    const float rowy[4] = { 100.0f, 132.0f, 164.0f, 196.0f };
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            s_btn[r * 4 + c].cx = GFX_S(colx[c]);
            s_btn[r * 4 + c].cy = GFX_S(rowy[r]);
        }
    s_laid = 1;
}

static void on_start(void)
{
    if (!s_laid) layout();
    s_expr[0] = '\0';
    strcpy(s_result, "0");
    s_error = 0;
    s_evaluated = 0;
}

/* does the current in-progress number already contain a '.'? */
static int cur_number_has_dot(void)
{
    int n = (int)strlen(s_expr);
    for (int i = n - 1; i >= 0; i--) {
        char c = s_expr[i];
        if (c == '.') return 1;
        if (c < '0' || c > '9') return 0;  /* hit an operator: new number */
    }
    return 0;
}

static void append_char(char c)
{
    int n = (int)strlen(s_expr);
    if (n >= (int)sizeof(s_expr) - 1) return;
    s_expr[n] = c;
    s_expr[n + 1] = '\0';
}

static void do_key(char c)
{
    int is_digit = (c >= '0' && c <= '9');

    if (c == 'C') { s_expr[0] = '\0'; strcpy(s_result, "0"); s_error = 0; s_evaluated = 0; return; }

    /* any key after an error clears the error first */
    if (s_error) { s_expr[0] = '\0'; s_error = 0; s_evaluated = 0; }

    if (c == '=') {
        if (s_expr[0] == '\0') return;
        int err;
        double v = eval(s_expr, &err);
        if (err) { s_error = 1; }
        else {
            snprintf(s_result, sizeof s_result, "%g", v);
            strcpy(s_expr, s_result);   /* let the result be chained */
        }
        s_evaluated = 1;
        return;
    }

    /* after '=': a digit/'.' starts fresh; an operator chains off result */
    if (s_evaluated) {
        if (is_digit || c == '.') s_expr[0] = '\0';
        s_evaluated = 0;
    }

    if (c == '.') {
        if (cur_number_has_dot()) return;         /* one dot per number */
        int n = (int)strlen(s_expr);
        if (n == 0 || s_expr[n - 1] < '0' || s_expr[n - 1] > '9')
            append_char('0');                     /* ".5" -> "0.5" */
    }

    append_char(c);
}

static int on_event(const wos_event *ev)
{
    if (ev->type != WOS_EV_TAP) return 0;
    for (int i = 0; i < 16; i++) {
        float dx = ev->x - s_btn[i].cx, dy = ev->y - s_btn[i].cy;
        if (dx * dx + dy * dy <= BTN_R * BTN_R) {
            do_key(s_lab[i]);
            return 1;
        }
    }
    return 0;
}

/* draw text so its right end sits at right_x, clipping to the longest
 * suffix that fits max_w (so a long expression scrolls from the right). */
static void draw_right(gfx_fb *fb, const font_t *f, int right_x, int y,
                       int max_w, const char *s, uint16_t col)
{
    const char *p = s;
    while (*p && font_text_width(f, p) > max_w) p++;
    int w = font_text_width(f, p);
    font_draw(fb, f, right_x - w, y, p, col);
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);

    /* live-evaluate the current expression for the running result */
    if (!s_error) {
        if (s_expr[0] == '\0') strcpy(s_result, "0");
        else {
            int err;
            double v = eval(s_expr, &err);
            if (!err) snprintf(s_result, sizeof s_result, "%g", v);
            /* on transient error (e.g. "2+"), keep the last good result */
        }
    }

    /* expression, right-aligned near the top */
    draw_right(fb, &font_medium, (int)GFX_S(196), (int)GFX_S(46),
               (int)GFX_S(150), s_expr[0] ? s_expr : "0", GFX_GREY);

    /* big running / final result */
    font_draw_centered(fb, &font_large, (int)GFX_CX, (int)GFX_S(78),
                       s_error ? "ERR" : s_result, s_error ? GFX_RED : GFX_WHITE);

    /* button grid: round disc + centered glyph */
    for (int i = 0; i < 16; i++) {
        char c = s_lab[i];
        uint16_t disc, ink = GFX_WHITE;
        if (c == 'C')                       disc = GFX_RED;
        else if (c == '=')                  disc = GFX_GREEN;
        else if (c == '+' || c == '-' ||
                 c == '*' || c == '/')      disc = GFX_ORANGE;
        else                                disc = GFX_DGREY;   /* digits & '.' */

        gfx_fill_circle(fb, s_btn[i].cx, s_btn[i].cy, BTN_R, disc);
        char lbl[2] = { c, '\0' };
        font_draw_centered(fb, &font_medium, (int)s_btn[i].cx,
                           (int)(s_btn[i].cy + GFX_S(7)), lbl, ink);
    }
}

const wos_app app_calc = { "Calc", on_start, on_event, on_draw, GFX_GREY, ICON_CALC };

#endif /* SELFTEST */
