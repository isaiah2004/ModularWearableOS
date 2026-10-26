#ifndef WATCH_BOARD_ROTATION_PAIRS_H
#define WATCH_BOARD_ROTATION_PAIRS_H
#include <stdint.h>

/* Source and destination must not overlap, as for the scalar rotation. An odd
 * source column is fine: input uses halfword loads. Output uses aligned words
 * only on known little-endian GNU targets. No additional pixel storage. */
#if defined(__GNUC__) && (defined(_WIN32) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
     __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__))
typedef uint32_t board_rotation_word __attribute__((__may_alias__));

static inline uint32_t board_rotation_pair(const uint16_t *p, int stride, int swap)
{
    uint32_t v = p[0] | ((uint32_t)p[stride] << 16);
    if (!swap) return v;
    uint32_t lo = v & 0x00ff00ffu, hi = v >> 8;
    /* Keep Xtensa GCC from expanding this permutation into a longer SRC/SSAI
     * sequence. These empty constraints emit no instruction or memory access. */
    __asm__ __volatile__("" : "+r"(lo), "+r"(hi));
    return (lo << 8) | (hi & 0x00ff00ffu);
}

/* One live source cursor avoids address spills in the unrolled target loop.
 * The final pair does not advance past the source array (including reverse
 * strides). Select height and byte order once, outside the pixel loop. */
#define BOARD_ROT_STEP(sw) do { \
    *dst++ = board_rotation_pair(col, stride, sw); \
    col += 2 * stride; \
    __asm__ __volatile__("" : "+r"(col)); \
} while (0)
#define BOARD_ROT_FOUR(sw) BOARD_ROT_STEP(sw); BOARD_ROT_STEP(sw); \
    BOARD_ROT_STEP(sw); BOARD_ROT_STEP(sw)
#define BOARD_ROT_LAST_FOUR(sw) BOARD_ROT_STEP(sw); BOARD_ROT_STEP(sw); \
    BOARD_ROT_STEP(sw); *dst++ = board_rotation_pair(col, stride, sw)
#define BOARD_ROT_KERNEL(name, sw, first) \
static void name(const uint16_t *src, int stride, board_rotation_word *dst, int width) { \
    for (int x = width - 1; x >= 0; x--) { \
        const uint16_t *col = src + x; \
        first; BOARD_ROT_LAST_FOUR(sw); \
    } \
}
BOARD_ROT_KERNEL(board_rotation_swap8, 1, (void)0)
BOARD_ROT_KERNEL(board_rotation_copy8, 0, (void)0)
BOARD_ROT_KERNEL(board_rotation_swap16, 1, BOARD_ROT_FOUR(1))
BOARD_ROT_KERNEL(board_rotation_copy16, 0, BOARD_ROT_FOUR(0))
#undef BOARD_ROT_KERNEL
#undef BOARD_ROT_LAST_FOUR
#undef BOARD_ROT_FOUR
#undef BOARD_ROT_STEP

static int board_rotation_pairs(const uint16_t *src, int stride, uint16_t *out,
                                int width, int height, int swap)
{
    if (width <= 0 || height <= 0 || (height & 1) || ((uintptr_t)out & 3u)) return 0;
    board_rotation_word *dst = (board_rotation_word *)out;
    if (height == 8) {
        if (swap) board_rotation_swap8(src, stride, dst, width);
        else board_rotation_copy8(src, stride, dst, width);
    } else if (height == 16) {
        if (swap) board_rotation_swap16(src, stride, dst, width);
        else board_rotation_copy16(src, stride, dst, width);
    } else {
        for (int x = width - 1; x >= 0; x--) {
            const uint16_t *col = src + x;
            for (int y = 0; y < height; y += 2) {
                *dst++ = board_rotation_pair(col, stride, swap);
                if (y + 2 < height) col += 2 * stride;
            }
        }
    }
    return 1;
}
#else
static int board_rotation_pairs(const uint16_t *src, int stride, uint16_t *out,
                                int width, int height, int swap)
{
    (void)src; (void)stride; (void)out; (void)width; (void)height; (void)swap;
    return 0;
}
#endif
#endif
