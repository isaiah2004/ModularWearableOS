/* Full-resolution PHOTO Sun: identical UV/QZ/lighting, with unused view/normal math omitted.
 * Template is instantiated for existing QZ/coefficient A/B modes; no mode test per pixel. */
static void CARD_SUN_NAME(sprite*s,const body_state*b,float spin,int textured,const uint32_t*multipliers)
{
    (void)multipliers;
    int r=C.lut_r,n=r*2;
    s->w=n;s->h=n;
    if(!card_window.active)memset(s->px,0,(size_t)n*n*2);
    int step=card_window.active?card_window.detail:1;
    int offset=(int)((spin+b->olon+180)*1024/360);
    if(!card_window.active || card_window.pass==CW_SHADER)
    for(int y=card_window.active?card_window.first:0;y<n && (!card_window.active || y<card_window.end);y+=step)for(int x=0;x<n;x+=step) {
        const globe_lut*q=&C.lut[y*n+x];
        if(q->z==65535)continue;
        float nz=CARD_SUN_QZ(q->z);
        int u=(offset+(int)q->lon*512/32767)&1023;
        unsigned idx=q->v*1024u+(unsigned)u;
        uint16_t color=textured&&C.tex_ok?texture_pixel(C.texture,idx):ob_tint[OB_SUN];
        unsigned level=256u+(unsigned)clamp(nz*255,0,255);
#if CARD_SUN_Q16
        uint32_t m=multipliers[level];
        if(m<=65536u)color=shade_q16(color,m);
        else color=shade(color,C.lighting_coeff->k[level],0,0);
#elif CARD_SUN_CACHED
        color=shade(color,C.lighting_coeff->k[level],0,0);
#else
        float k=.55f+.45f*C.lighting[level]/255.f;
        color=shade(color,k,0,0);
#endif
        for(int yy=y;yy<y+step && yy<n;yy++)for(int xx=x;xx<x+step && xx<n;xx++)s->px[yy*n+xx]=color;
    }
    GFX_COST_PX(GFX_COST_BLEND,(unsigned)(card_window.active?(card_window.end-card_window.first)*n/(step*step):r*r*PI));
}
#undef CARD_SUN_NAME
#undef CARD_SUN_QZ
#undef CARD_SUN_CACHED
#undef CARD_SUN_Q16
