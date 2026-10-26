/* Private retained arc setup. Raster equations match core/gfx.c; only
 * preparation moves to tick. Keep the equivalence tests when changing either. */
static int orr_angle_in(float theta,float a0,float a1)
{ float n=ceilf((a0-theta)*(1.0f/6.28318531f)); return theta+n*6.28318531f<=a1+1e-4f; }
static uint8_t orr_coverage(float d)
{ float c=d+0.5f; return c<=0?0:c>=1?255:(uint8_t)(c*255); }
static void orr_arc_prepare(orr_arc_geom *g,float cx,float cy,float r_mid,float thickness,float a0,float a1)
{
    float half = thickness * 0.5f;
    /* end-cap centers on the midline */
    float e0x = cx + gfx_fast_sin(a0) * r_mid, e0y = cy - gfx_fast_cos(a0) * r_mid;
    float e1x = cx + gfx_fast_sin(a1) * r_mid, e1y = cy - gfx_fast_cos(a1) * r_mid;
    /* Tight bbox of just the swept wedge instead of the whole ring: take the
     * sin/cos extent over [a0,a1] (endpoints, plus any cardinal angle that
     * falls inside the sweep) and pad for thickness + one pixel of AA. This
     * skips the expensive per-pixel atan2f over the entire ring interior. */
    float s0 = gfx_fast_sin(a0), s1 = gfx_fast_sin(a1), c0 = gfx_fast_cos(a0), c1 = gfx_fast_cos(a1);
    float smin = s0 < s1 ? s0 : s1, smax = s0 > s1 ? s0 : s1;
    float cmin = c0 < c1 ? c0 : c1, cmax = c0 > c1 ? c0 : c1;
    if (orr_angle_in( 1.57079633f, a0, a1)) smax =  1.0f;  /* sin peaks +1 */
    if (orr_angle_in(-1.57079633f, a0, a1)) smin = -1.0f;  /* sin dips  -1 */
    if (orr_angle_in( 0.0f,        a0, a1)) cmax =  1.0f;  /* cos peaks +1 */
    if (orr_angle_in( 3.14159265f, a0, a1)) cmin = -1.0f;  /* cos dips  -1 */
    float pad = half + 1.0f;
    /* Angular sector test via dot with the sector bisector — avoids per-pixel
     * atan2f. A pixel is inside [a0,a1] iff cos(angle to bisector) >=
     * cos(halfwidth); multiply both sides by dist>=0 to drop the divide. */
    float bis = 0.5f * (a0 + a1);
    float bx = gfx_fast_sin(bis), by = -gfx_fast_cos(bis);
    float cos_hw = gfx_fast_cos(0.5f * (a1 - a0));
    float cos_hw2 = cos_hw * cos_hw;   /* for the squared (sqrt-free) angular test */
    /* Scanline the annular band inside the wedge bbox (O(band) not O(bbox)).
     * y = cy - cos*r, so the row range uses the cos extent. */
    float r_out = r_mid + pad, r_in = r_mid - pad;
    if (r_in < 0) r_in = 0;
    float r_out2 = r_out * r_out, r_in2 = r_in * r_in;
    /* solid core of the band (no AA) — squared-distance test skips the sqrt */
    float rs_lo = r_mid - (half - 1.0f), rs_hi = r_mid + (half - 1.0f);
    float rs_lo2 = rs_lo > 0 ? rs_lo * rs_lo : 0.0f, rs_hi2 = rs_hi * rs_hi;
    float wx0 = cx + smin * r_mid - pad, wx1 = cx + smax * r_mid + pad;
    float y0=cy-cmax*r_mid-pad, y1=cy-cmin*r_mid+pad;
    g->cx=cx;
    g->cy=cy;
    g->r_mid=r_mid;
    g->half=half;
    g->e0x=e0x;
    g->e0y=e0y;
    g->e1x=e1x;
    g->e1y=e1y;
    g->bx=bx;
    g->by=by;
    g->cos_hw=cos_hw;
    g->cos_hw2=cos_hw2;
    g->r_out2=r_out2;
    g->r_in2=r_in2;
    g->rs_lo2=rs_lo2;
    g->rs_hi2=rs_hi2;
    g->wx0=wx0;
    g->wx1=wx1;
    g->y0=y0;
    g->y1=y1;
}
static void orr_arc_replay(gfx_fb *fb,const orr_arc_geom *g,uint16_t color)
{
    color=gfx_ink(fb,color);
    float cx=g->cx;
    float cy=g->cy;
    float r_mid=g->r_mid;
    float half=g->half;
    float e0x=g->e0x;
    float e0y=g->e0y;
    float e1x=g->e1x;
    float e1y=g->e1y;
    float bx=g->bx;
    float by=g->by;
    float cos_hw=g->cos_hw;
    float cos_hw2=g->cos_hw2;
    float r_out2=g->r_out2;
    float r_in2=g->r_in2;
    float rs_lo2=g->rs_lo2;
    float rs_hi2=g->rs_hi2;
    float wx0=g->wx0;
    float wx1=g->wx1;
    float y0=g->y0;
    float y1=g->y1;
    /* 2D dirty-rect x-window (buffer columns, sentinel cx1<=cx0 => full width);
     * px is logical, so the span clamp is [cc0-ox, cc1-1-ox]. */
    int cc0 = fb->cx0, cc1 = fb->cx1;
    if (cc1 <= cc0) { cc0 = 0; cc1 = GFX_W; }
    if (cc0 < 0) cc0 = 0;
    if (cc1 > GFX_W) cc1 = GFX_W;
    int pxlo = cc0 - fb->ox, pxhi = cc1 - 1 - fb->ox;
    if (orr_ceil(wx1) < pxlo || orr_floor(wx0) > pxhi) { GFX_COST(GFX_COST_ARC, 0); return; }
    int py0 = orr_floor(y0);
    int py1 = orr_ceil(y1);
    int lo=fb->y_base-fb->oy,hi=lo+fb->h-1;
    if(py0<lo)py0=lo;
    if(py1>hi)py1=hi;
    if(py0>py1)return;
    unsigned n = 0;   /* pixels written (cost model only) */
    for (int py = py0; py <= py1; py++) {
        int by_ = py + fb->oy - fb->y_base;
        float dy = (py + 0.5f) - cy;
        float ao = r_out2 - dy * dy;
        if (ao <= 0) continue;
        float xo = gfx_fast_sqrt(ao);
        int hole = (dy * dy < r_in2);
        float xi = hole ? gfx_fast_sqrt(r_in2 - dy * dy) : 0.0f;
        uint16_t *row = gfx_row(fb, by_);
        for (int seg = 0; seg < (hole ? 2 : 1); seg++) {
            float lx = !hole ? cx - xo : (seg == 0 ? cx - xo : cx + xi);
            float rx = !hole ? cx + xo : (seg == 0 ? cx - xi : cx + xo);
            if (lx < wx0) lx = wx0;
            if (rx > wx1) rx = wx1;
            int px0 = orr_floor(lx), px1 = orr_ceil(rx);
            if (px0 < pxlo) px0 = pxlo;
            if (px1 > pxhi) px1 = pxhi;
            for (int px = px0; px <= px1; px++) {
                int bxc = px + fb->ox;
                if ((unsigned)bxc >= GFX_W) continue;
                float dx = (px + 0.5f) - cx;
                float d2 = dx * dx + dy * dy;
                float dot = dx * bx + dy * by;
                /* Angular sector test with squared values — no sqrt. Inside iff
                 * dot >= dist*cos_hw; square it (guarding signs) to drop sqrt. */
                int inside = cos_hw >= 0.0f
                    ? (dot >= 0.0f && dot * dot >= d2 * cos_hw2)
                    : (dot >= 0.0f || dot * dot <= d2 * cos_hw2);
                uint8_t a;
                if (inside) {
                    if (d2 >= rs_lo2 && d2 <= rs_hi2) a = 255;  /* solid core, no sqrt */
                    else a = orr_coverage(half - fabsf(gfx_fast_sqrt(d2) - r_mid));  /* AA rim */
                } else {
                    /* outside the sweep: only the rounded end caps contribute */
                    float d0x = (px + 0.5f) - e0x, d0y = dy + cy - e0y;
                    float d1x = (px + 0.5f) - e1x, d1y = dy + cy - e1y;
                    float dcap = gfx_fast_sqrt(d0x * d0x + d0y * d0y);
                    float dcap1 = gfx_fast_sqrt(d1x * d1x + d1y * d1y);
                    if (dcap1 < dcap) dcap = dcap1;
                    a = orr_coverage(half - dcap);
                }
                if (a == 255) row[bxc] = color;
                else if (a) row[bxc] = gfx_blend565(row[bxc], color, a);
                n += a != 0;
            }
        }
    }
    GFX_COST(GFX_COST_ARC, n);
}


/* The zoom segment has only nine quantised lengths. Retain its coverage, not
 * coloured pixels, so the off/on passes still blend over the live sky exactly.
 * Green over black identifies gfx_blend565's 33 effective alpha levels. */
#define ORR_ZMASK 24
static struct orr_zoom_masks {
    uint8_t alpha[20][9][ORR_ZMASK*ORR_ZMASK],ready[20][9];
    int16_t x[20],y[20];
    uint8_t lo[20][9][ORR_ZMASK],hi[20][9][ORR_ZMASK];
    uint16_t scratch[ORR_ZMASK*ORR_ZMASK];
} *orr_zmask;
static int orr_zoom_prepare(int i,int step)
{
    if(!step)return 1;
    if(!orr_zmask) {
        orr_zmask=hal_big_alloc(sizeof *orr_zmask);
        if(!orr_zmask)return 0;
        memset(orr_zmask,0,sizeof *orr_zmask);
    }
    if(orr_zmask->ready[i][step])return 1;
    float a=ZB_P1*D2RF+(ZB_P0*D2RF-ZB_P1*D2RF)*((float)i+0.5f)/20;
    int x=orr_floor(CX+(ZOOM_R+1)*gfx_fast_sin(a))-ORR_ZMASK/2;
    int y=orr_floor(CY-(ZOOM_R+1)*gfx_fast_cos(a))-ORR_ZMASK/2;
    orr_zmask->x[i]=(int16_t)x; orr_zmask->y[i]=(int16_t)y;
    memset(orr_zmask->scratch,0,sizeof orr_zmask->scratch);
    gfx_fb f={0}; f.px=orr_zmask->scratch; f.stride=ORR_ZMASK;
    f.x_base=x; f.cx0=x; f.cx1=x+ORR_ZMASK; f.y_base=y; f.h=ORR_ZMASK;
    prog_arc_segment(&f,PROG_SEGMENTS,CX,CY,ZOOM_R+1,9,ZB_P1*D2RF,ZB_P0*D2RF,
                     20,((float)i+(float)step/8)/20,0xffff,0,i);
    for(int j=0;j<ORR_ZMASK*ORR_ZMASK;j++) {
        int a=(((orr_zmask->scratch[j]>>5)&63)+1)/2;
        orr_zmask->alpha[i][step][j]=(uint8_t)(a==32?255:a*8);
    }
    for(int yy=0;yy<ORR_ZMASK;yy++) {
        int a=ORR_ZMASK,b=0;
        for(int xx=0;xx<ORR_ZMASK;xx++)if(orr_zmask->alpha[i][step][yy*ORR_ZMASK+xx]) { if(xx<a)a=xx; b=xx+1; }
        orr_zmask->lo[i][step][yy]=(uint8_t)a; orr_zmask->hi[i][step][yy]=(uint8_t)b;
    }
    orr_zmask->ready[i][step]=1;
    GFX_COST_PX(GFX_COST_ORR_BUILD,ORR_ZMASK*ORR_ZMASK/8);
    return 1;
}
static void orr_zoom_mask(gfx_fb *fb,int i,int step,uint16_t ink)
{
    if(!step)return;
    int x=orr_zmask->x[i],y=orr_zmask->y[i];
    int lo=fb->y_base-fb->oy-y,hi=lo+fb->h;
    if(lo<0)lo=0;
    if(hi>ORR_ZMASK)hi=ORR_ZMASK;
    int x0=fb->cx0,x1=fb->cx1; if(x1<=x0){x0=0;x1=GFX_W;}
    int a=x0-fb->ox-x,b=x1-fb->ox-x;
    if(a<0)a=0;
    if(b>ORR_ZMASK)b=ORR_ZMASK;
    ink=gfx_ink(fb,ink);
    unsigned n=0;
    for(int yy=lo;yy<hi;yy++) {
        const uint8_t *mask=orr_zmask->alpha[i][step]+yy*ORR_ZMASK;
        uint16_t *dst=gfx_row(fb,y+yy+fb->oy-fb->y_base);
        int left=orr_zmask->lo[i][step][yy],right=orr_zmask->hi[i][step][yy];
        if(left<a)left=a;
        if(right>b)right=b;
        for(int xx=left;xx<right;xx++) {
            unsigned alpha=mask[xx]; if(!alpha)continue;
            int bx=x+xx+fb->ox;
            dst[bx]=alpha==255?ink:gfx_blend565(dst[bx],ink,(uint8_t)alpha); n++;
        }
    }
    GFX_COST_PX(GFX_COST_ORR_RUN,n);
}
