#ifndef WATCH_BOARD_ROTATION_COLUMNS_H
#define WATCH_BOARD_ROTATION_COLUMNS_H
/* Read two adjacent source columns per aligned word. Four source pixels become
 * two destination words; no scratch allocation and no speculative edge loads.
 * The paired/scalar paths retain all odd-height/unaligned/other-endian cases. */
#if defined(__GNUC__) && (defined(_WIN32) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__))
static inline uint32_t board_rotation_swap_word(uint32_t v) {
    uint32_t lo=v&0x00ff00ffu,hi=v>>8;
    __asm__ __volatile__("" : "+r"(lo), "+r"(hi));
    return (lo<<8)|(hi&0x00ff00ffu);
}
#define BOARD_COLUMN_PAIR(index,sw,half,advance) do { \
    uint32_t a=*(const board_rotation_word *)col; \
    uint32_t b=*(const board_rotation_word *)(col+stride); \
    if(sw){a=board_rotation_swap_word(a);b=board_rotation_swap_word(b);} \
    dst[index]=(a>>16)|(b&0xffff0000u); \
    dst[half+(index)]=(a&0xffffu)|(b<<16); \
    advance; \
}while(0)
#define BOARD_COLUMN_ADVANCE do {col+=2*stride;__asm__ __volatile__("" : "+r"(col));}while(0)
#define BOARD_COLUMN_FOUR(sw,half) \
    BOARD_COLUMN_PAIR(0,sw,half,BOARD_COLUMN_ADVANCE); \
    BOARD_COLUMN_PAIR(1,sw,half,BOARD_COLUMN_ADVANCE); \
    BOARD_COLUMN_PAIR(2,sw,half,BOARD_COLUMN_ADVANCE)
#define BOARD_COLUMN_MORE(sw) \
    BOARD_COLUMN_PAIR(3,sw,8,BOARD_COLUMN_ADVANCE); \
    BOARD_COLUMN_PAIR(4,sw,8,BOARD_COLUMN_ADVANCE); \
    BOARD_COLUMN_PAIR(5,sw,8,BOARD_COLUMN_ADVANCE); \
    BOARD_COLUMN_PAIR(6,sw,8,BOARD_COLUMN_ADVANCE)
/* The cursor fence prevents GCC hoisting fifteen row addresses into spills.
 * Keep fixed-height leaves out of the large generic dispatcher's registers. */
#define BOARD_COLUMN_KERNEL(name,sw,half,last,more) \
static __attribute__((noinline)) void name(const uint16_t *src,int stride,board_rotation_word *dst,int width) { \
    for(int x=width-2;x>=0;x-=2) { \
        const uint16_t *col=src+x; \
        BOARD_COLUMN_FOUR(sw,half);more; \
        BOARD_COLUMN_PAIR(last,sw,half,(void)0); \
        dst+=2*half; \
    } \
}
BOARD_COLUMN_KERNEL(board_rotation_columns_swap8,1,4,3,(void)0)
BOARD_COLUMN_KERNEL(board_rotation_columns_copy8,0,4,3,(void)0)
BOARD_COLUMN_KERNEL(board_rotation_columns_swap16,1,8,7,BOARD_COLUMN_MORE(1))
BOARD_COLUMN_KERNEL(board_rotation_columns_copy16,0,8,7,BOARD_COLUMN_MORE(0))
#undef BOARD_COLUMN_KERNEL
#undef BOARD_COLUMN_MORE
#undef BOARD_COLUMN_FOUR
#undef BOARD_COLUMN_ADVANCE
#undef BOARD_COLUMN_PAIR
static int board_rotation_columns(const uint16_t *src,int stride,uint16_t *out,int width,int height,int swap) {
    if((height!=8 && height!=16) || ((uintptr_t)src&3u) || ((uintptr_t)out&3u) || (stride&1))return 0;
    if(width&1) {
        if(!board_rotation_pairs(src+width-1,stride,out,1,height,swap))return 0;
        out+=height;width--;
    }
    if(height==8) {
        if(swap)board_rotation_columns_swap8(src,stride,(board_rotation_word *)out,width);
        else board_rotation_columns_copy8(src,stride,(board_rotation_word *)out,width);
    } else {
        if(swap)board_rotation_columns_swap16(src,stride,(board_rotation_word *)out,width);
        else board_rotation_columns_copy16(src,stride,(board_rotation_word *)out,width);
    }
    return 1;
}
#else
static int board_rotation_columns(const uint16_t *src,int stride,uint16_t *out,int width,int height,int swap) {
    (void)src;(void)stride;(void)out;(void)width;(void)height;(void)swap;return 0;
}
#endif
#endif
