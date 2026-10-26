static int entry_store_ref(entry_image *im, const uint8_t *px, int h)
{
    int x0 = ROW_W, x1 = -1, y0 = h, y1 = -1;
    for (int y = 0; y < h; y++) for (int x = 0; x < ROW_W; x++) if (px[y * ROW_W + x]) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    if (x1 < x0) return 1;
    int w = x1 - x0 + 1, ht = y1 - y0 + 1;
    uint8_t *dst = row_alloc((unsigned)(w * ht));
    if (!dst) return 0;
    im->s = (sprite8){ dst, w, ht }; im->x = x0; im->y = y0;
    for (int y = 0; y < ht; y++) memcpy(dst + y * w, px + (y0 + y) * ROW_W + x0, (size_t)w);
    return 1;
}
static int entry_bake_ref(int e)
{
    if (!rows_init()) return 0;
    entry_cache *r = &RC->entry[e];
    unsigned sig = entry_sig(e);
    if (r->ready && r->sig == sig) return 1;
    entry_free(r);                         /* invalid old pixels never double the peak */
    int h = units_of(E[e].kind) * UNIT, red = E[e].kind == K_DANGER;
    sprite cv = { RC->canvas, ROW_W, h };
    gfx_fb fb = sprite_fb(&cv);
    fb.ox = -UI_ROW_X0;
    memset(RC->canvas, 0, (size_t)ROW_W * h * sizeof(uint16_t));
    row_baking = 1; draw_entry(&fb, e, 0, (float)h, 0); row_baking = 0;
    /* Reset is a single accent: preserve its colour through a second intensity LUT.
     * Use the strongest channel so the light-theme accent retains full precision. */
    int shift = 5, mask = 63, peak = 63;
    if (red) {
        int rr = C_RED >> 11, gg = (C_RED >> 5) & 63, bb = C_RED & 31;
        if (rr * 2 >= gg && rr >= bb) { shift = 11; mask = 31; peak = rr; }
        else if (bb * 2 >= gg) { shift = 0; mask = 31; peak = bb; }
        else peak = gg;
    }
    for (int i = 0; i < ROW_W * h; i++)
        RC->grey[i] = (uint8_t)((((RC->canvas[i] >> shift) & mask) * 255 + peak / 2) / peak);
    if (!entry_store_ref(&r->image[0], RC->grey, h)) goto fail;
    for (int k = 0; k < 8; k++) {
        int support = row_blur_len[k%4]/2, direction=k<4?1:-1;
        for (int x = 0; x < ROW_W; x++) {
            for (int y = 0; y < h + 2 * ROW_HALO; y++) {
                uint64_t result=0;
                for(int age=-2;age<=support;age++) {
                    double t=4.0*age/support;
                    uint64_t percent=(uint64_t)((age==-2?20:age==-1?40:t<=1?100-50*t:t<=2?60-10*t:t<=3?80-20*t:50-10*t)+0.5);
                    int sy=y-ROW_HALO+direction*age;
                    if(sy>=0 && sy<h) {
                        uint64_t candidate=(percent*RC->grey[sy*ROW_W+x]+50)/100;
                        if(candidate>result)result=candidate;
                    }
                }
                RC->blur[y * ROW_W + x] = (uint8_t)result;
            }
        }
        if (!entry_store_ref(&r->image[k + 1], RC->blur, h + 2 * ROW_HALO)) goto fail;
        r->image[k+1].y-=ROW_HALO;
    }
    r->sig = sig; r->ready = 1; row_bakes++;
    return 1;
fail:
    entry_free(r); return 0;
}
