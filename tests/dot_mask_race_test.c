/* Deterministic first-use/prewarmer race injection, without timing-dependent threads. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static void tracked_free(void *p);
#define free tracked_free
#include "../nos/dot.c"
#undef free
static struct { void *ptr;int released; } allocations[512];
static int nalloc,nfree,inject,fail_next;
static int q_inject;
static const dmask *race_winner;
static void tracked_free(void *p) {
 if(!p) return;
 for(int i=0;i<nalloc;i++) if(allocations[i].ptr==p && !allocations[i].released) {
  allocations[i].released=1;nfree++;free(p);return;
 }
 assert(!"double free or unpublished ownership violation");
}
void *hal_big_alloc(uint32_t bytes) {
 if(fail_next) {fail_next=0;return NULL;}
 void *p=calloc(1,bytes);assert(p && nalloc<512);
 allocations[nalloc].ptr=p;allocations[nalloc++].released=0;
 int action=inject;inject=0;
 if(action==1) assert(dot_prebake_begin());
 if(action==2) {int q;race_winner=mask_get(q_inject*0.25f,&q);assert(q==q_inject && race_winner);}
 return p;
}
static unsigned checksum(const dmask *m) {
 unsigned h=0;
 for(uint32_t i=0;i<m->blk*m->nph*m->nph;i++) h=h*16777619u^m->data[i];
 return h;
}
int main(void) {
 /* Concurrent table initialization publishes one zeroed table and frees loser. */
 inject=1;assert(dot_prebake_begin());assert(nalloc==2 && nfree==1);
 int before=nfree,q;
 q_inject=8;inject=2;
 const dmask *m=mask_get(2.0f,&q);
 assert(q==8 && m==race_winner && nfree==before+1);
 unsigned sum=checksum(m);
 assert(publish_mask(&g_masks[8],NULL)==m);
 /* Failed first use preserves the normal live circle fallback (NULL mask). */
 fail_next=1;assert(!mask_get(33.25f,&q));assert(q==133);
 /* Background bake itself loses to an injected foreground first use. */
 before=nfree;q_inject=1;inject=2;dot_prebake_all();
 assert(nfree==before+1 && g_masks[1]==race_winner);
 assert(g_masks[8]==m && checksum(m)==sum);
 for(int i=1;i<=MASK_QMAX;i++) assert(g_masks[i]);
 int baked=nalloc;dot_prebake_all();assert(nalloc==baked); /* prewarm is reusable */
 for(int i=1;i<=MASK_QMAX;i++) tracked_free(g_masks[i]);
 tracked_free(g_masks);
 assert(nfree==nalloc);
 puts("dot masks: table/foreground/background CAS races free only losers; fallback and reuse preserved");
 return 0;
}
