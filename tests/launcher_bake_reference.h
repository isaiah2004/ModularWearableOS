/* Independent full-item baker for authored pixels and continuous trail density. */
static void reference_to_index(sprite8 *dst, const uint16_t *src)
{
    int n = dst->w * dst->h;
    for (int i = 0; i < n; i++) {
        int g = (src[i] >> 5) & 63;
        dst->px[i] = (uint8_t)((g * 255 + 31) / 63);
    }
}

/* Independent floating quadrature of the authored path. Do not call the
 * prepared-kernel implementation: this oracle must detect changed weights,
 * direction, edge normalization and cropping in the resumable baker. */
static unsigned reference_trail_px(const sprite8 *s,int x,int y,int len,int direction)
{
    int tail=len/2,steps=4*(tail+2);
    const double knot[]={-tail,-.75*tail,-.5*tail,-.25*tail,0,1,2};
    const double density[]={10,20,40,50,100,40,20};
    double sum=0,area=0;
    for(int step=0;step<=steps;step++) {
        double pose=-tail+step*.25;int segment=0;
        while(segment<5 && pose>knot[segment+1])segment++;
        double weight=density[segment]+(density[segment+1]-density[segment])*
            (pose-knot[segment])/(knot[segment+1]-knot[segment]);
        weight*=step==0||step==steps?.125:.25;
        double at=y-direction*pose;int row=(int)floor(at);double fraction=at-row;
        double a=row>=0&&row<s->h?s->px[row*s->w+x]:0;
        double b=row+1>=0&&row+1<s->h?s->px[(row+1)*s->w+x]:0;
        area+=weight;sum+=weight*(a*(1-fraction)+b*fraction);
    }
    return (unsigned)floor(sum/area+.5+1e-9);
}

/* Full continuous trail in one row-pitch band, independently cropped to ink. */
static int reference_bake_blur(blur8 *dst, const sprite8 *src, int top, int len,int direction)
{
    int w = src->w;
    uint8_t *tmp = (uint8_t *)L->scratch;      /* w * BAND <= 320 * 92 < sizeof scratch */
    for(int y=0;y<BAND;y++) for(int x=0;x<w;x++)
        tmp[y*w+x]=(uint8_t)reference_trail_px(src,x,y-top,len,direction);
    int y0 = 0, y1 = BAND - 1;
    while (y0 <= y1) { int any = 0; for (int x = 0; x < w && !any; x++) any = tmp[y0 * w + x]; if (any) break; y0++; }
    while (y1 >= y0) { int any = 0; for (int x = 0; x < w && !any; x++) any = tmp[y1 * w + x]; if (any) break; y1--; }
    if (y1 < y0) { dst->s.px = 0; return 1; }
    if (!dst->s.px && !sprite8_alloc(&dst->s, w, y1 - y0 + 1)) return 0;
    if (dst->s.w != w || dst->s.h != y1 - y0 + 1) return 0;   /* baked once; never resized */
    memcpy(dst->s.px, tmp + y0 * w, (size_t)w * (y1 - y0 + 1));
    dst->y0 = y0 - BAND / 2;
    return 1;
}

static int reference_bake_app(int i)
{
    row_spr *R = &L->rows[i];
    const nos_app *a = &nos_app_list[i];
    sprite cv;
    cv.px = L->scratch;
    for (int v = 0; v < NVAR; v++) {
        if (!R->icon[v].px && !sprite8_alloc(&R->icon[v], ISPR, ISPR)) return 0;
        cv.w = cv.h = ISPR;
        gfx_fb fb = sprite_fb(&cv);
        memset(cv.px, 0, (size_t)ISPR * ISPR * 2);
        float c = ISPR * 0.5f, r = k_var_r[v];
        gfx_fill_circle(&fb, c, c, r, C_SURF2);
        float pitch = r * 0.118f;
        dot_icon(&fb, a->icon, c, c, pitch, pitch * 0.8f, C_WHITE);
        reference_to_index(&R->icon[v], cv.px);
    }
    const font_t *f = UI_F_TITLE;
    int w = ui_text_width(f, a->name, 0) + 6;
    if (w > 320) w = 320;
    int h = f->line_height + 6;
    if (h > 64) h = 64;
    if (!R->name.px && !sprite8_alloc(&R->name, w, h)) return 0;
    cv.w = w; cv.h = h;
    gfx_fb fb = sprite_fb(&cv);
    memset(cv.px, 0, (size_t)w * h * 2);
    R->name_base = f->ascent + 3;
    ui_text(&fb, f, 2, R->name_base, a->name, C_WHITE, UI_LEFT, 0);
    reference_to_index(&R->name, cv.px);
    R->name_w = w; R->name_h = h;
    R->ready = 1;
    /* fling blur: the middle icon and the name, each placed where it sits in its row */
    int name_top = BAND / 2 - R->name_base + f->ascent / 2 - 1;   /* = ny - cy + BAND/2 */
    for (int b = 0; b < NBLUR; b++) {
        if (!reference_bake_blur(&R->bicon[b], &R->icon[1], BAND / 2 - ISPR / 2, k_blur_len[b],b<4?1:-1)) return 1;
        if (!reference_bake_blur(&R->bname[b], &R->name, name_top, k_blur_len[b],b<4?1:-1)) return 1;
    }
    R->blurred = 1;                            /* without it the list just stays sharp */
    return 1;
}
