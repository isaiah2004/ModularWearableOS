#ifndef NOS_BLUR_CACHE_H
#define NOS_BLUR_CACHE_H
#include <stdint.h>
#include <stddef.h>
/* Fixed ceiling, one allocation, retained across owners. No allocator calls per frame. */
#define BC_BYTES (2600u * 1024u)
#define BC_DEFAULT_BYTES (1024u * 1024u)
#define BC_BLOCK 4096u
#define BC_HEADER (32u * 1024u) /* metadata/record/coverage reserved inside ceiling */
#define BC_BLOCKS ((BC_BYTES - BC_HEADER) / BC_BLOCK)
#define BC_SLOTS 48
#define BC_SLICE 8

typedef struct {
 const void *owner;
 int item,w,h,context,x0,horiz;
 unsigned sig, generation, checked, touched;
 unsigned capture_wait; /* stable observations before retrying a rejected source */
 int pinned;
 int first, blocks, sharp_rows, filter_row, level, pending;
 int blur_first,blur_blocks;
 uint16_t *sharp,*blur,*staging;
} blur_item;
typedef struct {
 uint8_t *arena;
 uint16_t *stage;
 int stage_first,stage_blocks;
 blur_item *stage_item;
 int block_count;
 uint8_t used[BC_BLOCKS];
 blur_item slots[BC_SLOTS];
 unsigned clock,hits,misses,evictions,work_pixels;
 blur_item *sum_item;
 int sum_next,sum_packed;
 union {
  unsigned short sums[466][3];
  uint32_t packed_sums[466];
  uint32_t wide_sums[466][2]; /* independent BR16 and G lanes for 40px */
 };
} blur_cache;
/* Exact A/B kernel switch. A partial filter reinitializes rolling sums when
 * representation changes; completed pixels and publication stay unchanged. */
extern int blur_cache_packed;
void blur_cache_init(blur_cache *c, void *arena);
void blur_cache_init_sized(blur_cache *c,void *arena,size_t bytes);
blur_item *blur_cache_find(blur_cache *c,const void *owner,int item,int w,int h,int context,int x0,int horiz);
blur_item *blur_cache_get(blur_cache *c,const void *owner,int item,int w,int h,int context,int x0,int horiz);
void blur_cache_invalidate(blur_cache *c,const void *owner,int first,int last);
/* One bounded slice; input must be fully baked. Returns 1 when selected level is ready. */
int blur_cache_filter(blur_cache *c,blur_item *s,int level);
int blur_cache_length(int level);
#endif
