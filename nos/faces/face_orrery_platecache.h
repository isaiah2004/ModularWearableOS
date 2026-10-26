/* The two resting plate silhouettes and the expanded NOW button. Retain
 * exact blend lanes from the original pill kernel, independent of ink/dim.
 * PSRAM only; allocation failure keeps the ordinary primitive path. */
typedef struct {
    uint8_t *alpha;
    uint16_t lo[112],hi[112],solid_lo[112],solid_hi[112];
    int x,y,w,h;
    float xa,cy,xb,r;
} orr_plate_mask;
static orr_plate_mask *orr_plate_masks;
static void orr_plate_prepare(void)
{
    if(orr_plate_masks)return;
    orr_plate_masks=hal_big_alloc(3*sizeof *orr_plate_masks);
    if(!orr_plate_masks)return;
    memset(orr_plate_masks,0,3*sizeof *orr_plate_masks);
    const float geometry[3][4]={{183,402,283,43},{151,392,315,52},{260,392,320,24}};
    for(int i=0;i<3;i++) {
        orr_plate_mask *m=orr_plate_masks+i;
        m->xa=geometry[i][0];m->cy=geometry[i][1];m->xb=geometry[i][2];m->r=geometry[i][3];
        m->x=orr_floor(m->xa-m->r-1);m->y=orr_floor(m->cy-m->r-1);
        m->w=orr_ceil(m->xb+m->r+1)+1-m->x;m->h=orr_ceil(m->cy+m->r+1)+1-m->y;
        uint16_t *work=hal_big_alloc(m->w*m->h*sizeof *work);
        m->alpha=hal_big_alloc(m->w*m->h);
        if(!work || !m->alpha) { hal_big_free(work);hal_big_free(m->alpha);m->alpha=0;continue; }
        memset(work,0,m->w*m->h*sizeof *work);
        gfx_fb f={0};f.px=work;f.stride=m->w;f.x_base=m->x;f.y_base=m->y;f.h=m->h;f.cx0=m->x;f.cx1=m->x+m->w;
        gfx_fill_pill(&f,m->xa,m->cy,m->xb,m->r,0xffff);
        /* Green has 64 levels: all 33 5-bit blend lanes map uniquely. */
        uint8_t lane[64]={0};
        for(int a=0;a<=32;a++)lane[(gfx_blend565(0,0xffff,a==32?255:a*8)>>5)&63]=(uint8_t)a;
        for(int y=0;y<m->h;y++) {
            int lo=m->w,hi=0,sl=m->w,sh=0;
            for(int x=0;x<m->w;x++) {
                int a=lane[(work[y*m->w+x]>>5)&63];m->alpha[y*m->w+x]=(uint8_t)a;
                if(a) { if(x<lo)lo=x;hi=x+1; }
                if(a==32) { if(x<sl)sl=x;sh=x+1; }
            }
            m->lo[y]=(uint16_t)lo;m->hi[y]=(uint16_t)hi;m->solid_lo[y]=(uint16_t)sl;m->solid_hi[y]=(uint16_t)sh;
        }
        hal_big_free(work);
        GFX_COST_PX(GFX_COST_ORR_PRE_CPU,120*m->w*m->h);
    }
}
static int orr_plate_pill(gfx_fb *fb,float xa,float cy,float xb,float r,uint16_t colour)
{
    if(!orr_plate_masks)return 0;
    orr_plate_mask *m=0;
    for(int i=0;i<3;i++) {
        orr_plate_mask *p=orr_plate_masks+i;
        if(p->alpha && xa==p->xa && cy==p->cy && xb==p->xb && r==p->r) { m=p;break; }
    }
    if(!m)return 0;
    int x0=fb->cx0,x1=fb->cx1;if(x1<=x0){x0=0;x1=GFX_W;}
    if(x0<0)x0=0;
    if(x1>GFX_W)x1=GFX_W;
    int a=fb->y_base-fb->oy-m->y,z=a+fb->h;
    if(a<0)a=0;
    if(z>m->h)z=m->h;
    colour=gfx_ink(fb,colour);unsigned solid=0,edges=0;
    for(int y=a;y<z;y++) {
        int l=m->lo[y],h=m->hi[y],ox=m->x+fb->ox;
        if(l<x0-ox)l=x0-ox;
        if(h>x1-ox)h=x1-ox;
        if(l>=h)continue;
        uint16_t *row=gfx_row(fb,m->y+y+fb->oy-fb->y_base);
        int sl=m->solid_lo[y],sh=m->solid_hi[y];
        if(sl<l)sl=l;
        if(sl>h)sl=h;
        if(sh<sl)sh=sl;
        if(sh>h)sh=h;
        for(int x=l;x<sl;x++) { unsigned alpha=m->alpha[y*m->w+x];if(alpha)row[ox+x]=gfx_blend565(row[ox+x],colour,alpha*8);edges++; }
        uint16_t *dst=row+ox+sl;int n=sh-sl;
        if(n && ((uintptr_t)dst&2)) { *dst++=colour;n--; }
        uint32_t *words=(uint32_t *)dst,packed=colour|((uint32_t)colour<<16);
        while(n>=16) {
            words[0]=words[1]=words[2]=words[3]=packed;
            words[4]=words[5]=words[6]=words[7]=packed;words+=8;n-=16;
        }
        while(n>=2) { *words++=packed;n-=2; }
        if(n)*(uint16_t *)words=colour;
        for(int x=sh;x<h;x++) { unsigned alpha=m->alpha[y*m->w+x];if(alpha)row[ox+x]=gfx_blend565(row[ox+x],colour,alpha*8);edges++; }
        solid+=sh-sl;
    }
    GFX_COST(GFX_COST_FILL,solid);GFX_COST_PX(GFX_COST_BLEND,edges);
    GFX_COST_PX(GFX_COST_ORR_CPU,1500*(z>a?z-a:0));
    return 1;
}
