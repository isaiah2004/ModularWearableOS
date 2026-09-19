/* app_2048.c — a fully playable 2048 for the round watch.
 *
 * =====================  PLAIN-ENGLISH ALGORITHM  =====================
 *
 * BOARD MODEL
 *   The grid is a flat static int[16], row-major: index = row*4 + col.
 *   A cell holds 0 (empty) or a power of two (2,4,8,...). All game state
 *   (board, score, over/won flags) is file-scope static — no malloc.
 *
 * THE ONE PRIMITIVE: move_left
 *   Every move is expressed as "slide/merge every row to the LEFT". For a
 *   single row we:
 *     1. COMPACT: copy the non-zero values to the front (drop the gaps).
 *     2. MERGE: walk left-to-right; if two neighbours are equal, fuse them
 *        into one tile of double the value, add that value to the score, and
 *        skip the consumed partner so each tile merges at most once per move
 *        (classic rule: [2,2,2,2] -> [4,4], never [8]).
 *     3. RE-COMPACT into a fresh 4-wide row, zero-padded on the right.
 *   move_left runs this over all four rows and reports whether ANY cell
 *   changed (needed to decide if a new tile should spawn).
 *
 * DERIVING THE OTHER THREE DIRECTIONS (reuse move_left via reflections)
 *   - LEFT : board as-is.
 *   - RIGHT: reverse each row, move_left, reverse each row back.
 *   - UP   : transpose, move_left, transpose back.
 *   - DOWN : transpose + reverse-rows, move_left, reverse-rows + transpose.
 *   transpose() swaps rows<->cols; reverse_rows() mirrors each row L<->R.
 *   Composing a mirror/transpose, sliding left, then undoing the transform
 *   gives an exact slide in the requested direction — one tested primitive
 *   powers all four moves.
 *
 * SPAWNING
 *   After any move that changed the board, spawn_tile() picks a uniformly
 *   random empty cell and drops a 2 (90%) or a 4 (10%). A new game seeds two
 *   tiles.
 *
 * GAME OVER / WIN
 *   has_moves() is true if any cell is empty OR any orthogonal neighbours are
 *   equal (a merge is still possible). Game over = !has_moves after a spawn.
 *   Reaching a 2048 tile latches the "won" banner but play continues.
 *
 * PRNG
 *   A tiny 32-bit xorshift, seeded from hal_millis() at new-game time (never
 *   from a wall-clock that might be unavailable). Deterministic, cheap, and
 *   good enough for tile placement.
 *
 * The move/merge core (slide_merge_row, move_left, move_dir, has_moves,
 * transpose, reverse_rows) is pure and header-only, so the SELFTEST main()
 * below exercises it directly with -DSELFTEST, independent of any UI/HAL.
 * ====================================================================== */

#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include <stdio.h>
#include <string.h>

/* --------------------------- move/merge core -------------------------- */

/* Slide+merge one 4-cell row to the left, in place. Adds merged values to
 * *score. Returns 1 if the row changed, 0 otherwise. */
static int slide_merge_row(int row[4], long *score)
{
    int tmp[4];
    int n = 0;
    for (int i = 0; i < 4; i++)          /* 1. compact non-zeros forward */
        if (row[i]) tmp[n++] = row[i];

    int out[4] = { 0, 0, 0, 0 };
    int oi = 0;
    for (int i = 0; i < n; i++) {        /* 2. merge equal adjacent pair */
        if (i + 1 < n && tmp[i] == tmp[i + 1]) {
            int merged = tmp[i] * 2;
            out[oi++] = merged;
            if (score) *score += merged;
            i++;                         /*    consume the partner once   */
        } else {
            out[oi++] = tmp[i];
        }
    }

    int changed = 0;                     /* 3. write back, detect change  */
    for (int i = 0; i < 4; i++) {
        if (row[i] != out[i]) changed = 1;
        row[i] = out[i];
    }
    return changed;
}

/* Slide the whole board left. Returns 1 if anything moved/merged. */
static int move_left(int b[16], long *score)
{
    int changed = 0;
    for (int r = 0; r < 4; r++) {
        int row[4] = { b[r*4], b[r*4+1], b[r*4+2], b[r*4+3] };
        if (slide_merge_row(row, score)) changed = 1;
        b[r*4] = row[0]; b[r*4+1] = row[1]; b[r*4+2] = row[2]; b[r*4+3] = row[3];
    }
    return changed;
}

/* Mirror each row left<->right. */
static void reverse_rows(int b[16])
{
    for (int r = 0; r < 4; r++) {
        int t;
        t = b[r*4];   b[r*4]   = b[r*4+3]; b[r*4+3] = t;
        t = b[r*4+1]; b[r*4+1] = b[r*4+2]; b[r*4+2] = t;
    }
}

/* Swap rows and columns. */
static void transpose(int b[16])
{
    for (int r = 0; r < 4; r++)
        for (int c = r + 1; c < 4; c++) {
            int t = b[r*4+c];
            b[r*4+c] = b[c*4+r];
            b[c*4+r] = t;
        }
}

/* dir: 0=left, 1=right, 2=up, 3=down. Returns 1 if the board changed. */
static int move_dir(int b[16], int dir, long *score)
{
    if      (dir == 1) reverse_rows(b);
    else if (dir == 2) transpose(b);
    else if (dir == 3) { transpose(b); reverse_rows(b); }

    int changed = move_left(b, score);

    if      (dir == 1) reverse_rows(b);
    else if (dir == 2) transpose(b);
    else if (dir == 3) { reverse_rows(b); transpose(b); }

    return changed;
}

/* True if any move is still possible (an empty cell or an equal neighbour). */
static int has_moves(const int b[16])
{
    for (int i = 0; i < 16; i++)
        if (b[i] == 0) return 1;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            int v = b[r*4+c];
            if (c < 3 && b[r*4+c+1]   == v) return 1;
            if (r < 3 && b[(r+1)*4+c] == v) return 1;
        }
    return 0;
}

/* ============================ SELFTEST ============================== */
#ifdef SELFTEST
#include <assert.h>

static int row_eq(const int a[4], int x, int y, int z, int w)
{
    return a[0]==x && a[1]==y && a[2]==z && a[3]==w;
}

int main(void)
{
    int pass = 0;
    long score;
    int changed;

    /* ---- single-row primitive (the spec's worked examples) ---- */
    { int r[4]={2,2,0,0}; score=0; changed=slide_merge_row(r,&score);
      assert(row_eq(r,4,0,0,0) && score==4 && changed); pass++;
      printf("[ok] [2,2,0,0] -> [4,0,0,0]  score+4\n"); }

    { int r[4]={2,2,2,2}; score=0; changed=slide_merge_row(r,&score);
      assert(row_eq(r,4,4,0,0) && score==8 && changed); pass++;
      printf("[ok] [2,2,2,2] -> [4,4,0,0]  score+8\n"); }

    { int r[4]={4,4,8,8}; score=0; changed=slide_merge_row(r,&score);
      assert(row_eq(r,8,16,0,0) && score==24 && changed); pass++;
      printf("[ok] [4,4,8,8] -> [8,16,0,0]\n"); }

    { int r[4]={2,0,2,4}; score=0; changed=slide_merge_row(r,&score);
      assert(row_eq(r,4,4,0,0) && score==4 && changed); pass++;
      printf("[ok] [2,0,2,4] -> [4,4,0,0]  score+4\n"); }

    { int r[4]={0,0,0,2}; score=0; changed=slide_merge_row(r,&score);
      assert(row_eq(r,2,0,0,0) && score==0 && changed); pass++;
      printf("[ok] [0,0,0,2] -> [2,0,0,0]  (slide, no merge)\n"); }

    /* a row already left-packed with no merge must report NO change */
    { int r[4]={4,2,0,0}; score=0; changed=slide_merge_row(r,&score);
      assert(row_eq(r,4,2,0,0) && score==0 && !changed); pass++;
      printf("[ok] [4,2,0,0] -> unchanged (no move)\n"); }

    /* triple: only the leftmost pair merges */
    { int r[4]={2,2,2,0}; score=0; changed=slide_merge_row(r,&score);
      assert(row_eq(r,4,2,0,0) && score==4 && changed); pass++;
      printf("[ok] [2,2,2,0] -> [4,2,0,0]  score+4\n"); }

    /* ---- full-board move + direction derivation ---- */
    /* LEFT on a full board */
    { int b[16]={ 2,2,4,4,  0,0,0,0,  8,0,8,0,  2,4,2,4 };
      score=0; changed=move_dir(b,0,&score);
      int e[16]={ 4,8,0,0,  0,0,0,0,  16,0,0,0, 2,4,2,4 };
      assert(changed && memcmp(b,e,sizeof e)==0 && score==(4+8+16)); pass++;
      printf("[ok] board move LEFT correct  score+28\n"); }

    /* RIGHT is the mirror of LEFT */
    { int b[16]={ 2,2,0,0,  0,0,0,0,  0,0,0,0,  0,0,0,0 };
      score=0; changed=move_dir(b,1,&score);
      int e[16]={ 0,0,0,4,  0,0,0,0,  0,0,0,0,  0,0,0,0 };
      assert(changed && memcmp(b,e,sizeof e)==0 && score==4); pass++;
      printf("[ok] board move RIGHT correct score+4\n"); }

    /* UP collapses a column */
    { int b[16]={ 2,0,0,0,  2,0,0,0,  4,0,0,0,  4,0,0,0 };
      score=0; changed=move_dir(b,2,&score);
      int e[16]={ 4,0,0,0,  8,0,0,0,  0,0,0,0,  0,0,0,0 };
      assert(changed && memcmp(b,e,sizeof e)==0 && score==(4+8)); pass++;
      printf("[ok] board move UP correct    score+12\n"); }

    /* DOWN collapses a column downward */
    { int b[16]={ 2,0,0,0,  2,0,0,0,  4,0,0,0,  4,0,0,0 };
      score=0; changed=move_dir(b,3,&score);
      int e[16]={ 0,0,0,0,  0,0,0,0,  4,0,0,0,  8,0,0,0 };
      assert(changed && memcmp(b,e,sizeof e)==0 && score==(4+8)); pass++;
      printf("[ok] board move DOWN correct  score+12\n"); }

    /* a move that changes nothing is detected as "no move" */
    { int b[16]={ 2,4,8,16, 4,8,16,32, 8,16,32,64, 16,32,64,128 };
      score=0; changed=move_dir(b,0,&score);  /* already packed left */
      assert(!changed && score==0); pass++;
      printf("[ok] no-op board move LEFT detected as no move\n"); }

    /* game-over detection: full checkerboard, no equal neighbours */
    { int b[16]={ 2,4,2,4, 4,2,4,2, 2,4,2,4, 4,2,4,2 };
      assert(!has_moves(b)); pass++;
      printf("[ok] checkerboard -> game over (no moves)\n"); }

    /* has_moves true when a merge exists even with no empties */
    { int b[16]={ 2,2,4,8, 4,8,16,32, 8,16,32,64, 16,32,64,128 };
      assert(has_moves(b)); pass++;
      printf("[ok] equal neighbour -> moves remain\n"); }

    printf("\nALL %d SELFTEST CASES PASSED\n", pass);
    return 0;
}

/* ============================ REAL APP ============================== */
#else

/* ------------------------------ PRNG -------------------------------- */
static uint32_t s_rng = 1;

static uint32_t xrand(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

/* ---------------------------- game state ---------------------------- */
static int  s_board[16];
static long s_score;
static int  s_over;
static int  s_won;
static int  s_started;

static void spawn_tile(void)
{
    int empty[16], n = 0;
    for (int i = 0; i < 16; i++)
        if (!s_board[i]) empty[n++] = i;
    if (!n) return;
    int idx = empty[xrand() % (uint32_t)n];
    s_board[idx] = (xrand() % 10u == 0) ? 4 : 2;   /* 10% fours */
}

static void new_game(void)
{
    s_rng = hal_millis() | 1u;          /* never zero (xorshift needs it) */
    for (int i = 0; i < 16; i++) s_board[i] = 0;
    s_score = 0;
    s_over  = 0;
    s_won   = 0;
    spawn_tile();
    spawn_tile();
}

static void on_start(void)
{
    if (!s_started) { new_game(); s_started = 1; }
}

/* ------------------------------ colors ------------------------------ */
/* per-value tile colors: warmer/brighter as the value grows. */
static uint16_t tile_color(int v)
{
    switch (v) {
    case 2:    return GFX_RGB(118, 110, 102);
    case 4:    return GFX_RGB(150, 130,  95);
    case 8:    return GFX_RGB(240, 150,  70);
    case 16:   return GFX_RGB(245, 120,  55);
    case 32:   return GFX_RGB(245,  95,  55);
    case 64:   return GFX_RGB(235,  60,  45);
    case 128:  return GFX_RGB(240, 205,  80);
    case 256:  return GFX_RGB(245, 200,  55);
    case 512:  return GFX_RGB(248, 190,  30);
    case 1024: return GFX_RGB(252, 178,  10);
    case 2048: return GFX_RGB(255, 160,   0);
    default:   return GFX_RGB(180, 120, 250);   /* 4096+ */
    }
}

/* dark ink on the two pale low tiles, white elsewhere */
static uint16_t tile_ink(int v)
{
    return (v <= 4) ? GFX_RGB(40, 38, 34) : GFX_WHITE;
}

/* ------------------------- drawing helpers -------------------------- */
/* rounded rectangle: x,y,w,h,r are already in pixels */
static void rrect(gfx_fb *fb, float x, float y, float w, float h,
                  float r, uint16_t col)
{
    gfx_fill_rect(fb, (int)(x + r), (int)y, (int)(w - 2*r), (int)h, col);
    gfx_fill_rect(fb, (int)x, (int)(y + r), (int)w, (int)(h - 2*r), col);
    gfx_fill_circle(fb, x + r,     y + r,     r, col);
    gfx_fill_circle(fb, x + w - r, y + r,     r, col);
    gfx_fill_circle(fb, x + r,     y + h - r, r, col);
    gfx_fill_circle(fb, x + w - r, y + h - r, r, col);
}

/* board geometry on the 240 design grid (148px board, centered).
 * Sized so the rounded corners stay inside the bezel-safe radius. */
#define BRD   148.0f
#define BX0    46.0f
#define BY0    46.0f
#define CELLU  37.0f

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);

    /* --- score, in the top arc above the board --- */
    char buf[24];
    snprintf(buf, sizeof buf, "%ld", s_score);
    font_draw_centered(fb, &font_small,  (int)GFX_CX, (int)GFX_S(16),
                       "SCORE", GFX_GREY);
    font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)GFX_S(34),
                       buf, GFX_WHITE);

    /* --- board backdrop --- */
    rrect(fb, GFX_S(BX0), GFX_S(BY0), GFX_S(BRD), GFX_S(BRD),
          GFX_S(10), GFX_RGB(58, 54, 50));

    /* --- cells --- */
    const float cellpx = GFX_S(CELLU);
    const float pad    = GFX_S(4);
    const float tr     = GFX_S(6);          /* tile corner radius */
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            int v = s_board[r*4 + c];
            float x = GFX_S(BX0) + c * cellpx + pad;
            float y = GFX_S(BY0) + r * cellpx + pad;
            float w = cellpx - 2 * pad;
            uint16_t col = v ? tile_color(v) : GFX_RGB(40, 38, 36);
            rrect(fb, x, y, w, w, tr, col);
            if (v) {
                char t[8];
                snprintf(t, sizeof t, "%d", v);
                int len = (int)strlen(t);
                const font_t *f = (len <= 2) ? &font_large
                                 : (len == 3) ? &font_medium
                                              : &font_small;
                int cx = (int)(x + w / 2.0f);
                int by = (int)(y + w / 2.0f + f->ascent * 0.5f);
                font_draw_centered(fb, f, cx, by, t, tile_ink(v));
            }
        }

    /* --- win latch (keeps playing) --- */
    if (s_won && !s_over)
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(224),
                           "2048! keep going", GFX_YELLOW);
    else if (!s_over)
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(224),
                           "swipe to move", GFX_DGREY);

    /* --- game-over overlay --- */
    if (s_over) {
        float bw = GFX_S(168), bh = GFX_S(80);
        rrect(fb, GFX_CX - bw/2, GFX_CY - bh/2, bw, bh,
              GFX_S(12), GFX_RGB(200, 60, 50));
        font_draw_centered(fb, &font_medium, (int)GFX_CX,
                           (int)(GFX_CY - GFX_S(4)), "Game Over", GFX_WHITE);
        font_draw_centered(fb, &font_small, (int)GFX_CX,
                           (int)(GFX_CY + GFX_S(22)), "tap to retry", GFX_WHITE);
    }
}

static void note_win(void)
{
    for (int i = 0; i < 16; i++)
        if (s_board[i] >= 2048) { s_won = 1; return; }
}

static int on_event(const wos_event *ev)
{
    int dir;
    switch (ev->type) {
    case WOS_EV_SWIPE_LEFT:  dir = 0; break;
    case WOS_EV_SWIPE_RIGHT: dir = 1; break;
    case WOS_EV_SWIPE_UP:    dir = 2; break;
    case WOS_EV_SWIPE_DOWN:  dir = 3; break;
    case WOS_EV_TAP:
        if (s_over) { new_game(); return 1; }
        return 0;                         /* let the OS handle empty taps */
    case WOS_EV_LONG_PRESS:
        new_game();
        return 1;
    default:
        return 0;
    }

    if (s_over) return 1;                  /* consume, but frozen till retry */

    if (move_dir(s_board, dir, &s_score)) {
        spawn_tile();
        if (!s_won) note_win();
        if (!has_moves(s_board)) s_over = 1;
    }
    return 1;
}

const wos_app app_2048 = { "2048", on_start, on_event, on_draw,
                           GFX_ORANGE, ICON_GRID };

#endif /* SELFTEST */
