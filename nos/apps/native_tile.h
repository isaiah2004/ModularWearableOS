/* Bounded, allocation-free vertical exposure for compact neutral-ink tiles.
 * Integrates constant source pixels over a fractional box. Integer/rest poses
 * keep the original opaque blit; moving poses use a rolling column sum. */
#ifndef NOS_NATIVE_TILE_H
#define NOS_NATIVE_TILE_H
#include "sprite.h"
#include "ui.h"
#include "ui_blur_observe.h"
#include "gfx_cost.h"
#include <stdint.h>
#include <string.h>
#include <limits.h>
/* Caller initializes this bounded owner before use. Source generation must
 * change on any same-pointer rewrite within a frame. INTERNAL requires internal
 * RAM; EXTERNAL copies continuations to the existing stack for hot row updates. */
typedef struct {
    const uint8_t *source;
    uint32_t generation,frame;
    int32_t next;
    int16_t pose,width;
    uint16_t sw,sh,k0,k1,offset,capacity;
} native_carry_slot;
typedef struct {
    uint32_t frame,used;
    native_carry_slot slots[8];
    unsigned values[940];
} native_carry_pool;
_Static_assert(sizeof(native_carry_pool)<=4096,"bounded carry pool");
typedef enum { NATIVE_CARRY_INTERNAL, NATIVE_CARRY_EXTERNAL } native_carry_location;
typedef struct {uint64_t calls,hits,saved_reads,copy_bytes,full_misses;} native_carry_stats;
static void native_carry_reset(native_carry_pool *p,uint32_t frame) {
    if(p){memset(p->slots,0,sizeof p->slots);p->frame=frame;p->used=0;}
}
static void native_carry_invalidate(native_carry_pool *p,const sprite8 *s){
    if(p && s)for(int i=0;i<8;i++)if(p->slots[i].source==s->px)p->slots[i].next=INT32_MIN;
}
static const uint8_t native_zero[GFX_W]={0};
static int native_floor256(int q) { return q >= 0 ? q / 256 : -((-q + 255) / 256); }
static unsigned native_tile_sample(const sprite8 *s, const sprite8 *other,
                                   unsigned mix, int x, int y)
{
    if ((unsigned)y >= (unsigned)s->h) return 0;
    unsigned a = s->px[y * s->w + x];
    if (other && mix) a = (a * (64 - mix) + other->px[y * s->w + x] * mix + 32) >> 6;
    return a;
}
/* The control atlas has a single source. Keep its hot row in a leaf so
 * column sums and source bounds never spill through the clipping routine. */
static __attribute__((noinline)) void native_rolling_row(uint16_t *restrict dst,
 unsigned *restrict sums,const uint8_t *a,const uint8_t *b,const uint8_t *sub,
 int count,const uint16_t *lut,unsigned wa,unsigned wb,unsigned reciprocal) {
    for(int x=0;x<count;x++) {
        unsigned incoming=b[x],sum=sums[x];
        unsigned total=(sum<<8)+a[x]*wa+incoming*wb;
        unsigned ink=(total*reciprocal+0x800000u)>>24;
        dst[x]=lut[ink>255?255:ink];
        sums[x]=sum+incoming-sub[x];
    }
}
static inline void native_tile_blit_carry(gfx_fb *fb, const sprite8 *s, const sprite8 *other,
                            unsigned mix, int x, int y, const uint16_t *lut,
                            unsigned key, int pose, int exposure, int top, int bottom,
 native_carry_pool *pool,uint32_t frame,uint32_t generation,native_carry_location location,native_carry_stats *stats)
{
#if NOS_RENDER_SHARP
    (void)pose;(void)exposure;(void)pool;(void)frame;(void)generation;(void)location;(void)stats;
    if(other && mix>=32)s=other;
    if(!s || !s->px || !lut)return;
    int sharp_first=top>y?top-y:0,sharp_last=bottom<y+s->h?bottom-y:s->h;
    if(sharp_last<=sharp_first)return;
    sprite8 sharp=*s;sharp.px+=(size_t)sharp_first*s->w;sharp.h=sharp_last-sharp_first;
    sprite8_blit(fb,&sharp,x,y+sharp_first,lut,key);return;
#endif

    if(pool && pool->frame!=frame)native_carry_reset(pool,frame);
    if(pool && fb && fb->rec && pool->used)native_carry_reset(pool,frame);
    if(other && mix)native_carry_invalidate(pool,s);
    if (!s || !s->px || !lut || s->w <= 0 || s->h <= 0 || s->w > GFX_W || bottom<=top) {
        native_carry_invalidate(pool,s);
        return;
    }
    if(other && (!other->px || other->w!=s->w || other->h!=s->h || mix>64)){native_carry_invalidate(pool,s);return;}
    if(pose < -256 || pose > 256){native_carry_invalidate(pool,s);return;}
    if(exposure < -48*256)exposure=-48*256;
    if (exposure < 0) exposure = -exposure;
    if (exposure > 48 * 256) exposure = 48 * 256;
    if (!pose && !exposure && (!other || !mix) && top<=y && bottom>=y+s->h) {
        sprite8_blit(fb,s,x,y,lut,key);
        native_carry_invalidate(pool,s);
        return;
    }
    int width = exposure < 256 ? 256 : exposure;
    int halo = (width + 511) / 512 + 1;
    int first = -halo, last = s->h + halo;
    if (first < top-y) first = top-y;
    if (last > bottom-y) last = bottom-y;
    key=gfx_hash_u(gfx_hash_u(gfx_hash_u(key,(unsigned)pose),(unsigned)width),mix);
    key=gfx_hash_u(gfx_hash_u(key,(unsigned)top),(unsigned)bottom);
    key=gfx_hash_u(gfx_hash_u(key,(unsigned)x),(unsigned)y);
    if (gfx_rec_cmd(fb,x,y+first,x+s->w,y+last,key)) {
        native_carry_invalidate(pool,s);
        return;
    }
    int c0=fb->cx0,c1=fb->cx1;
    if(c1<=c0){c0=0;c1=GFX_W;}
    if(c0<fb->x_base)c0=fb->x_base;
    int right=fb->x_base+(fb->stride?fb->stride:GFX_W);
    if(c1>right)c1=right;
    if(c0<0)c0=0;
    if(c1>GFX_W)c1=GFX_W;
    int bx=x+fb->ox,k0=c0>bx?c0-bx:0,k1=c1<bx+s->w?c1-bx:s->w;
    if(first<fb->y_base-y-fb->oy)first=fb->y_base-y-fb->oy;
    if(last>fb->y_base+fb->h-y-fb->oy)last=fb->y_base+fb->h-y-fb->oy;
    if(k0>=k1||first>=last){native_carry_invalidate(pool,s);return;}
    uint16_t dimmed[256];
    if(fb->dim){for(int i=0;i<256;i++)dimmed[i]=gfx_ink(fb,lut[i]);lut=dimmed;}
    unsigned local_sums[GFX_W];
    unsigned *sums=local_sums;
    int start=first*256-pose-width/2+128,end=start+width;
    int a=native_floor256(start),b=native_floor256(end);
    unsigned wa=(unsigned)((a+1)*256-start),wb=(unsigned)(end-b*256);
    native_carry_slot *carry=NULL;
    int hit=0;
    if(stats)stats->calls++;
    if(pool && (!other || !mix) && s->w<=UINT16_MAX && s->h<=UINT16_MAX) {
        for(int i=0;i<8;i++)if(pool->slots[i].source==s->px){carry=&pool->slots[i];break;}
        if(carry && carry->capacity<(unsigned)s->w) {
            /* Retire the old key before allocating a wider region. Its space
             * remains bounded until the next frame; never duplicate a key. */
            carry->source=NULL;
            carry->next=INT32_MIN;
            carry=NULL;
        }
        if(!carry && pool->used+(unsigned)s->w<=940u)for(int i=0;i<8;i++)if(!pool->slots[i].source){
            carry=&pool->slots[i];carry->source=s->px;carry->offset=(uint16_t)pool->used;
            carry->capacity=(uint16_t)s->w;pool->used+=(unsigned)s->w;carry->next=INT32_MIN;break;
        }
        if(carry){
            hit=carry->frame==frame && carry->generation==generation && carry->sw==s->w && carry->sh==s->h &&
                carry->pose==pose && carry->width==width && carry->k0==k0 && carry->k1==k1 && carry->next==first;
            if(location==NATIVE_CARRY_INTERNAL)sums=pool->values+carry->offset;
            else if(hit){memcpy(sums+k0,pool->values+carry->offset+k0,(size_t)(k1-k0)*sizeof *sums);if(stats)stats->copy_bytes+=(uint64_t)(k1-k0)*sizeof *sums;}
            if(hit && stats){int lo=a+1>0?a+1:0,hi=b<s->h?b:s->h;stats->hits++;stats->saved_reads+=(uint64_t)(hi>lo?hi-lo:0)*(unsigned)(k1-k0);}
            carry->next=INT32_MIN;
        }else if(stats)stats->full_misses++;
    }
    if(hit) { /* Exact post-row sums from the preceding contiguous stripe. */ }
    else if(!other || !mix) {
        for(int col=k0;col<k1;col++)sums[col]=0;
        /* Walk contiguous source rows instead of striding PSRAM by column. */
        for(int r=a+1;r<b;r++)if((unsigned)r<(unsigned)s->h) {
            const uint8_t *row=s->px+r*s->w;
            for(int col=k0;col<k1;col++)sums[col]+=row[col];
        }
    } else {
    for(int col=k0;col<k1;col++){
        unsigned sum=0;
        for(int r=a+1;r<b;r++)sum+=native_tile_sample(s,other,mix,col,r);
        sums[col]=sum;
    }
    }
    /* Reciprocal error is < 0.094 ink codes, before final rounding. */
    unsigned reciprocal=(0x1000000u+(unsigned)width/2)/(unsigned)width;
#ifdef GFX_COST_MODEL
    gfx_cost_target(fb->cost_psram);
#endif
    if(exposure)ui_fblur_coverage_claim(fb,x+k0,y+first,x+k1,y+last);
    unsigned pixels=(unsigned)(last-first)*(unsigned)(k1-k0);
    ui_fblur_observe_native(fb,x+k0,y+first,x+k1,y+last,exposure!=0);
    GFX_COST(GFX_COST_BLIT,pixels); GFX_COST_PX(GFX_COST_BLEND,pixels);
    for(int r=first;r<last;r++,a++,b++){
        uint16_t *dst=gfx_row(fb,y+r+fb->oy-fb->y_base)+bx+k0;
        if(!other || !mix) {
            const uint8_t *pa=(unsigned)a<(unsigned)s->h?s->px+a*s->w+k0:native_zero;
            const uint8_t *pb=(unsigned)b<(unsigned)s->h?s->px+b*s->w+k0:native_zero;
            const uint8_t *ps=(unsigned)(a+1)<(unsigned)s->h?s->px+(a+1)*s->w+k0:native_zero;
            native_rolling_row(dst,sums+k0,pa,pb,ps,k1-k0,lut,wa,wb,reciprocal);
            continue;
        }
        for(int col=k0;col<k1;col++){
            unsigned total=(sums[col]<<8)+native_tile_sample(s,other,mix,col,a)*wa+
                           native_tile_sample(s,other,mix,col,b)*wb;
            unsigned ink=(total*reciprocal+0x800000u)>>24;
            *dst++=lut[ink>255?255:ink];
            sums[col]+=native_tile_sample(s,other,mix,col,b);
            sums[col]-=native_tile_sample(s,other,mix,col,a+1);
        }
    }
    if(carry){
        int authored_last=s->h+halo;if(authored_last>bottom-y)authored_last=bottom-y;
        if(last<authored_last && y+last+fb->oy<GFX_H){
            if(location==NATIVE_CARRY_EXTERNAL){memcpy(pool->values+carry->offset+k0,sums+k0,(size_t)(k1-k0)*sizeof *sums);if(stats)stats->copy_bytes+=(uint64_t)(k1-k0)*sizeof *sums;}
            carry->frame=frame;carry->generation=generation;carry->sw=(uint16_t)s->w;carry->sh=(uint16_t)s->h;
            carry->pose=(int16_t)pose;carry->width=(int16_t)width;carry->k0=(uint16_t)k0;carry->k1=(uint16_t)k1;carry->next=last;
        }
    }

}

static inline void native_tile_blit(gfx_fb *fb,const sprite8 *s,const sprite8 *other,unsigned mix,
 int x,int y,const uint16_t *lut,unsigned key,int pose,int exposure,int top,int bottom) {
    native_tile_blit_carry(fb,s,other,mix,x,y,lut,key,pose,exposure,top,bottom,NULL,0,0,NATIVE_CARRY_INTERNAL,NULL);
}
/* Continuous temporal density: 10/20/40/50/100/40/20 at path positions
 * -tail,-3tail/4,-tail/2,-tail/4,0,1,2; tail=floor(length/2).
 * Integrate linearly interpolated source poses every quarter pixel using the
 * trapezoid rule, then fold their bilinear weights into integer source rows.
 * This is the dense preview's density, NOT seven copies or a max envelope.
 * Build once per cache level; rendering still only interpolates prepared tiles.
 * Keep the full denominator at source edges (outside the tile is black).
 * The menu's existing row crop, palette and curved-layout approximation remain.
 */
#define NATIVE_TRAIL_MAX_LENGTH 90
#define NATIVE_TRAIL_MAX_TAIL (NATIVE_TRAIL_MAX_LENGTH/2)
typedef struct {
    uint32_t weight[NATIVE_TRAIL_MAX_TAIL+3]; /* ages -2 through tail */
    uint32_t total;
    int tail;
} native_trail_kernel;

static inline void native_trail_kernel_make(native_trail_kernel *k,int length) {
    memset(k,0,sizeof *k);
    if(length>NATIVE_TRAIL_MAX_LENGTH)length=NATIVE_TRAIL_MAX_LENGTH;
    k->tail=length>0?length/2:0;
    if(!k->tail){k->weight[2]=k->total=1;return;}
    int tail=k->tail;
    for(int q=-8;q<=4*tail;q++) {
        /* density*tail is integral at all quarter-pixel samples, including
         * quarter-tail knots when the nominal length is odd. */
        unsigned density;
        if(q< -4)density=(unsigned)((60+5*q)*tail);
        else if(q<0)density=(unsigned)((100+15*q)*tail);
        else if(q<=tail)density=(unsigned)(100*tail-50*q);
        else if(q<=2*tail)density=(unsigned)(60*tail-10*q);
        else if(q<=3*tail)density=(unsigned)(80*tail-20*q);
        else density=(unsigned)(50*tail-10*q);
        unsigned area=density*((q==-8 || q==4*tail)?1u:2u);
        int row=q>=0?q/4:-((-q+3)/4),fraction=q-row*4;
        k->weight[row+2]+=area*(unsigned)(4-fraction);
        if(fraction)k->weight[row+3]+=area*(unsigned)fraction;
        k->total+=area*4u;
    }
}
static inline unsigned native_trail_sample(const native_trail_kernel *k,
 const uint8_t *px,int w,int h,int x,int y,int direction) {
    if(!px || x<0 || x>=w || h<=0)return 0;
    uint32_t sum=0;
    for(int age=-2;age<=k->tail;age++) {
        int sy=y+direction*age;
        if(sy>=0 && sy<h)sum+=k->weight[age+2]*px[sy*w+x];
    }
    /* Max total=2,817,000; even 255*total fits in uint32_t. Round once. */
    return (sum+k->total/2)/k->total;
}
static inline unsigned native_trail_pixel(const uint8_t *px,int w,int h,int x,int y,int len,int direction) {
    native_trail_kernel k;native_trail_kernel_make(&k,len);
    return native_trail_sample(&k,px,w,h,x,y,direction);
}
/* 75 px/s reaches the first authored level; 150/300/450 reach 30/60/90.
 * Continuous onset avoids a discontinuity when crossing the threshold. */
static inline int native_smear_exposure(float travel) {
    float length=travel*M_FPS*0.2f;
    if(length>90.0f)length=90.0f;
    return (int)(length*256.0f+0.5f);
}
static inline void native_direction_levels(int direction,int *lo,int *hi) {
    if(direction<0){if(*lo)*lo+=4;if(*hi)*hi+=4;}
}
/* Prepared boxes share their authored origin, but retain independently cropped
 * bounds. Interpolate source ink before one palette lookup, then reconstruct
 * fractional Y. This path has at most four source reads per written pixel. */
static unsigned native_prepared_sample(const sprite8 *s,int x,int y)
{
    return s && s->px && (unsigned)x<(unsigned)s->w && (unsigned)y<(unsigned)s->h ? s->px[y*s->w+x] : 0;
}
/* Separate leaf keeps row state out of the large clipping routine's register
 * allocation. Four bilinear weights preserve the original single rounding. */
static __attribute__((noinline)) void native_prepared_row(uint16_t *restrict dst,
 const uint8_t *al,const uint8_t *ah,const uint8_t *bl,const uint8_t *bh,
 int count,const uint16_t *lut,unsigned mix,unsigned phase) {
    if(!mix || mix==256) {
        if(mix==256){al=bl;ah=bh;}
        if(!phase) {
            for(int i=0;i<count;i++)dst[i]=lut[al[i]];
        } else {
            unsigned inverse=256-phase;
            for(int i=0;i<count;i++)dst[i]=lut[(al[i]*inverse+ah[i]*phase+128)>>8];
        }
        return;
    }
    if(!phase) {
        unsigned inverse=256-mix;
        for(int i=0;i<count;i++)dst[i]=lut[(al[i]*inverse+bl[i]*mix+128)>>8];
        return;
    }
    unsigned w0=(256-mix)*(256-phase),w1=mix*(256-phase);
    unsigned w2=(256-mix)*phase,w3=mix*phase;
    for(int i=0;i<count;i++) {
        unsigned ink=(al[i]*w0+bl[i]*w1+ah[i]*w2+bh[i]*w3+32768)>>16;
        dst[i]=lut[ink];
    }
}
/* Optional exact row staging. Only the UI producer rasterizes these two menus;
 * the flush/history worker never calls native leaves. This shared owner is not
 * reentrant. Each chunk is consumed before the next overwrite; no source bytes
 * survive for reuse across rows or frames. */
#define NATIVE_ROW_STAGE_CHUNK 128
_Static_assert(sizeof nos_native_row_stage_scratch>=4*NATIVE_ROW_STAGE_CHUNK,
               "shared row stage holds four source chunks");
static inline const uint8_t *native_row_stage_plane(uint8_t *scratch,
 const uint8_t *source,int offset,int count) {
    if(source==native_zero)return native_zero;
    memcpy(scratch,source+offset,(size_t)count);
    return scratch;
}
static __attribute__((noinline)) void native_prepared_row_staged(uint16_t *restrict dst,
 const uint8_t *al,const uint8_t *ah,const uint8_t *bl,const uint8_t *bh,
 int count,const uint16_t *lut,unsigned mix,unsigned phase,uint8_t *scratch) {
    /* A single sharp plane cannot benefit from separating multiple streams. */
    if(!scratch || count>GFX_W || ((!mix || mix==256) && !phase)) {
        native_prepared_row(dst,al,ah,bl,bh,count,lut,mix,phase);
        return;
    }
    for(int offset=0;offset<count;) {
        int n=count-offset;
        if(n>NATIVE_ROW_STAGE_CHUNK)n=NATIVE_ROW_STAGE_CHUNK;
        const uint8_t *a0=al,*a1=ah,*b0=bl,*b1=bh;
        if(!mix || mix==256) {
            if(mix==256) {
                b0=native_row_stage_plane(scratch,bl,offset,n);
                b1=native_row_stage_plane(scratch+NATIVE_ROW_STAGE_CHUNK,bh,offset,n);
            } else {
                a0=native_row_stage_plane(scratch,al,offset,n);
                a1=native_row_stage_plane(scratch+NATIVE_ROW_STAGE_CHUNK,ah,offset,n);
            }
        } else {
            a0=native_row_stage_plane(scratch,al,offset,n);
            b0=native_row_stage_plane(scratch+NATIVE_ROW_STAGE_CHUNK,bl,offset,n);
            if(phase) {
                a1=native_row_stage_plane(scratch+2*NATIVE_ROW_STAGE_CHUNK,ah,offset,n);
                b1=native_row_stage_plane(scratch+3*NATIVE_ROW_STAGE_CHUNK,bh,offset,n);
            }
        }
        native_prepared_row(dst+offset,a0,a1,b0,b1,n,lut,mix,phase);
        offset+=n;
    }
}

/* A source level can be absent for many rows of a longer shutter's halo.
 * Keep its contribution zero without loading zero bytes or changing rounding. */
static __attribute__((noinline)) void native_prepared_weighted(uint16_t *restrict dst,
 const uint8_t *a,const uint8_t *b,int count,const uint16_t *lut,unsigned wa,unsigned wb) {
    if((!wa || a==native_zero) && (!wb || b==native_zero)) {
        uint16_t black=lut[0];
        for(int x=0;x<count;x++)dst[x]=black;
        return;
    }
    if(!wa || a==native_zero){a=b;wa=wb;wb=0;}
    if(!wb || b==native_zero) {
        for(int x=0;x<count;x++)dst[x]=lut[(a[x]*wa+32768)>>16];
        return;
    }
    for(int x=0;x<count;x++)dst[x]=lut[(a[x]*wa+b[x]*wb+32768)>>16];
}
/* Preserve the inner 64-step variant rounding before the outer Q16 blend.
 * Combining weights across these stages changes authored grayscale pixels. */
static __attribute__((noinline)) void native_prepared_variant_row(uint16_t *restrict dst,
 const uint8_t *al,const uint8_t *ah,const uint8_t *vl,const uint8_t *vh,
 const uint8_t *bl,const uint8_t *bh,int count,const uint16_t *lut,
 unsigned vmix,unsigned mix,unsigned phase) {
    unsigned vi=64-vmix,w0=(256-mix)*(256-phase),w1=mix*(256-phase);
    unsigned w2=(256-mix)*phase,w3=mix*phase;
    for(int i=0;i<count;i++) {
        unsigned low=(al[i]*vi+vl[i]*vmix+32)>>6;
        unsigned high=(ah[i]*vi+vh[i]*vmix+32)>>6;
        dst[i]=lut[(low*w0+bl[i]*w1+high*w2+bh[i]*w3+32768)>>16];
    }
}
/* Extended menu halos combine ink instead of erasing neighbouring trails.
 * Mode 2 uses a component-wise envelope, also respecting inverted palettes. */
static inline uint16_t native_halo_merge(uint16_t a,uint16_t b,int light) {
    unsigned out=0;
    const unsigned masks[3]={0xf800,0x07e0,0x001f};
    for(int i=0;i<3;i++){unsigned x=a&masks[i],y=b&masks[i];out|=light?(x>y?x:y):(x<y?x:y);}
    return (uint16_t)out;
}
static inline void native_prepared_blit(gfx_fb *fb,const sprite8 *a,int ax,int ay,
                                const sprite8 *b,int bx,int by,unsigned mix,
                                const uint16_t *lut,unsigned key,int pose,
                                int top,int bottom,int blurred,const sprite8 *variant,unsigned vmix)
{
#if NOS_RENDER_SHARP
    /* Sharp mode selects one immutable source, at integer pixel coordinates.
     * Crop the source itself so authored row ownership remains exact. */
    (void)b;(void)bx;(void)by;(void)mix;(void)pose;(void)blurred;
    if(variant && vmix>=32)a=variant;
    if(!a || !a->px || !lut)return;
    int first=top>ay?top-ay:0,last=bottom<ay+a->h?bottom-ay:a->h;
    if(last<=first)return;
    sprite8 sharp=*a;sharp.px+=(size_t)first*a->w;sharp.h=last-first;
    sprite8_blit(fb,&sharp,ax,ay+first,lut,key);
    return;
#endif
    if(!a || !lut || !a->px || a->w<=0 || a->h<=0 || mix>256 || pose < -256 || pose > 256)return;
    if(variant && (!variant->px || variant->w!=a->w || variant->h!=a->h || vmix>64))return;
    if(!mix || !b || !b->px){b=a;bx=ax;by=ay;mix=0;}
    if(!blurred && !pose && !mix && (!variant || !vmix) && top<=ay && bottom>=ay+a->h){
        sprite8_blit(fb,a,ax,ay,lut,key);
        if(blurred && !fb->rec)ui_fblur_coverage_claim(fb,ax,ay,ax+a->w,ay+a->h);
        return;
    }
    int x0=ax<bx?ax:bx,x1=ax+a->w>bx+b->w?ax+a->w:bx+b->w;
    int y0=(ay<by?ay:by)-1,y1=(ay+a->h>by+b->h?ay+a->h:by+b->h)+1;
    if(y0<top)y0=top;
    if(y1>bottom)y1=bottom;
    if(y1<=y0)return;
    key=gfx_hash_u(gfx_hash_u(gfx_hash_u(key,mix),(unsigned)pose),(unsigned)blurred);
    key=gfx_hash_u(key,vmix);
    key=gfx_hash_u(gfx_hash_u(key,(unsigned)ax),(unsigned)ay);
    key=gfx_hash_u(gfx_hash_u(key,(unsigned)bx),(unsigned)by);
    if(gfx_rec_cmd(fb,x0,y0,x1,y1,key))return;
    int c0=fb->cx0,c1=fb->cx1;if(c1<=c0){c0=0;c1=GFX_W;}
    if(c0<fb->x_base)c0=fb->x_base;
    int right=fb->x_base+(fb->stride?fb->stride:GFX_W);
    if(c1>right)c1=right;
    if(c0<0)c0=0;
    if(c1>GFX_W)c1=GFX_W;
    if(x0+fb->ox<c0)x0=c0-fb->ox;
    if(x1+fb->ox>c1)x1=c1-fb->ox;
    if(y0+fb->oy<fb->y_base)y0=fb->y_base-fb->oy;
    if(y1+fb->oy>fb->y_base+fb->h)y1=fb->y_base+fb->h-fb->oy;
    if(x1<=x0 || y1<=y0)return;
    int stage_mode=nos_native_row_stage_mode;
    uint16_t dimmed[256];
    if(fb->dim){for(int i=0;i<256;i++)dimmed[i]=gfx_ink(fb,lut[i]);lut=dimmed;}
    else if(stage_mode&2){memcpy(dimmed,lut,sizeof dimmed);lut=dimmed;}
    int shift=native_floor256(-pose);unsigned phase=(unsigned)(-pose-shift*256);
    int stage_rows=blurred!=2 && (stage_mode&1);
#ifdef GFX_COST_MODEL
    gfx_cost_target(fb->cost_psram);
#endif
    unsigned pixels=(unsigned)(x1-x0)*(unsigned)(y1-y0);
    ui_fblur_observe_native(fb,x0,y0,x1,y1,blurred!=0);
    GFX_COST(GFX_COST_BLIT,pixels);GFX_COST_PX(GFX_COST_BLEND,pixels);
    /* X ownership is constant across every row in a stripe. Prepare its at most
     * five edge spans once, instead of sorting the same edges on every row. */
    struct {int left,right,ina,inb;} spans[5];
    int nspans=0;
    for(int left=x0;left<x1;) {
        int right=x1;int edges[4]={ax,ax+a->w,bx,bx+b->w};
        for(int edge=0;edge<4;edge++)if(edges[edge]>left && edges[edge]<right)right=edges[edge];
        spans[nspans].left=left;spans[nspans].right=right;
        spans[nspans].ina=left>=ax && left<ax+a->w;
        spans[nspans].inb=left>=bx && left<bx+b->w;
        nspans++;left=right;
    }
    for(int y=y0;y<y1;y++){
        int sy=y+shift;
        uint16_t *dst=gfx_row(fb,y+fb->oy-fb->y_base)+x0+fb->ox;
        uint16_t halo[GFX_W],*target=dst;
        if(blurred==2)dst=halo;
        /* Cropped levels can differ by a single column after rounding. Split
         * their union at source edges so the complete row uses bounded leaves,
         * with zero planes for absent source spans (at most five segments). */
        if(x1-x0<=GFX_W) {
            for(int span=0;span<nspans;span++) {
                int left=spans[span].left,right=spans[span].right;
                int ina=spans[span].ina,inb=spans[span].inb;
                const uint8_t *al=ina && (unsigned)(sy-ay)<(unsigned)a->h ? a->px+(sy-ay)*a->w+left-ax : native_zero;
                const uint8_t *ah=ina && (unsigned)(sy+1-ay)<(unsigned)a->h ? a->px+(sy+1-ay)*a->w+left-ax : native_zero;
                const uint8_t *bl=inb && (unsigned)(sy-by)<(unsigned)b->h ? b->px+(sy-by)*b->w+left-bx : native_zero;
                const uint8_t *bh=inb && (unsigned)(sy+1-by)<(unsigned)b->h ? b->px+(sy+1-by)*b->w+left-bx : native_zero;
                if(variant && vmix) {
                    const uint8_t *vl=ina && (unsigned)(sy-ay)<(unsigned)variant->h ? variant->px+(sy-ay)*variant->w+left-ax : native_zero;
                    const uint8_t *vh=ina && (unsigned)(sy+1-ay)<(unsigned)variant->h ? variant->px+(sy+1-ay)*variant->w+left-ax : native_zero;
                    native_prepared_variant_row(dst+left-x0,al,ah,vl,vh,bl,bh,right-left,lut,vmix,mix,phase);
                } else if(al==native_zero && ah==native_zero) {
                    native_prepared_weighted(dst+left-x0,bl,bh,right-left,lut,
                                             mix*(256-phase),mix*phase);
                } else if(bl==native_zero && bh==native_zero) {
                    native_prepared_weighted(dst+left-x0,al,ah,right-left,lut,
                                             (256-mix)*(256-phase),(256-mix)*phase);
                } else {
                    if(stage_rows)
                        native_prepared_row_staged(dst+left-x0,al,ah,bl,bh,right-left,lut,mix,phase,nos_native_row_stage_scratch);
                    else
                    native_prepared_row(dst+left-x0,al,ah,bl,bh,right-left,lut,mix,phase);
                }
            }
            if(blurred==2)for(int i=0;i<x1-x0;i++)target[i]=native_halo_merge(target[i],halo[i],lut[255]>lut[0]);
            continue;
        }
        for(int x=x0;x<x1;x++){
            unsigned al=native_prepared_sample(a,x-ax,sy-ay),ah=native_prepared_sample(a,x-ax,sy+1-ay);
            if(variant && vmix){
                al=(al*(64-vmix)+native_prepared_sample(variant,x-ax,sy-ay)*vmix+32)>>6;
                ah=(ah*(64-vmix)+native_prepared_sample(variant,x-ax,sy+1-ay)*vmix+32)>>6;
            }
            unsigned lo=al*(256-mix)+native_prepared_sample(b,x-bx,sy-by)*mix;
            unsigned hi=ah*(256-mix)+native_prepared_sample(b,x-bx,sy+1-by)*mix;
            unsigned ink=(lo*(256-phase)+hi*phase+32768)>>16;
            *dst++=lut[ink];
        }
    }
    if(blurred)ui_fblur_coverage_claim(fb,x0,y0,x1,y1);
}
static inline void native_prepared_levels(int exposure,int *lo,int *hi,unsigned *mix)
{
    static const int widths[5]={0,15*256,30*256,60*256,90*256};
    if(exposure<0)exposure=0;
    if(exposure>90*256)exposure=90*256;
    int upper=1;while(upper<4 && exposure>widths[upper])upper++;
    *lo=upper-1;*hi=upper;
    *mix=(unsigned)((exposure-widths[*lo])*256/(widths[*hi]-widths[*lo]));
    if(!*mix)*hi=*lo;
}
#endif
