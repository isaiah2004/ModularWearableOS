#ifndef NOS_COST_PROFILE_H
#define NOS_COST_PROFILE_H
#include <stdint.h>
#ifndef NOS_RENDER_PROFILING
#define NOS_RENDER_PROFILING 0
#endif
enum { NOS_COST_SCENE, NOS_COST_CLEAR, NOS_COST_SPRITE8, NOS_COST_SPRITE565,
       NOS_COST_TEXT, NOS_COST_SHAPE, NOS_COST_COUNT };
typedef struct {
    uint32_t inclusive[NOS_COST_COUNT], exclusive[NOS_COST_COUNT], calls[NOS_COST_COUNT];
} nos_cost_frame;
#if NOS_RENDER_PROFILING
typedef struct nos_cost_scope {
    struct nos_cost_scope *parent;
    uint32_t start, children;
    unsigned bucket, live;
} nos_cost_scope;
extern int nos_cost_active;
void nos_cost_enter(nos_cost_scope *scope, unsigned bucket);
void nos_cost_leave(nos_cost_scope *scope);
void nos_cost_set(int enabled);
int nos_cost_enabled(void);
void nos_cost_begin(void);
void nos_cost_end(void);
nos_cost_frame nos_cost_last(void);
void nos_cost_capture_reset(void);
void nos_cost_capture_add(int painted);
void nos_cost_report(void);
void nos_cost_calibrate(void);
#define NOS_COST_JOIN_(a,b) a##b
#define NOS_COST_JOIN(a,b) NOS_COST_JOIN_(a,b)
#define NOS_COST_SCOPE_(bucket,condition,line) \
    nos_cost_scope NOS_COST_JOIN(cost_scope_,line) __attribute__((cleanup(nos_cost_leave))) = {0}; \
    if (nos_cost_active && (condition)) nos_cost_enter(&NOS_COST_JOIN(cost_scope_,line),bucket)
#define NOS_COST_SCOPE(bucket,condition) NOS_COST_SCOPE_(bucket,condition,__LINE__)
#else
#define NOS_COST_SCOPE(bucket,condition) ((void)0)
static inline void nos_cost_begin(void) {}
static inline void nos_cost_end(void) {}
#endif
#endif
