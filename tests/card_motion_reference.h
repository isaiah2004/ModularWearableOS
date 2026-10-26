/* Standalone whole-image coarse reference from pre-pipeline ignored prototype.
 * Only lattice step parameterized2/4, and original whole DOT pass enabled. */
static int build_lut_motion_reference(int r,float lat,int detail)
{
    int n = r * 2;
    const float *axis = card_coords_axis(C.coords, 0, r);
    if (C.lut_r != r || C.lut_lat != lat) {
        C.lut_r = r;
        C.lut_lat = lat;
        C.lut_row = 0;
        C.lut_publish = 0;
        C.ring_lat_valid = 0;
    }
    if (C.lut_row >= n) return 1;
    float cl = fk_cos(lat * RAD), sl = fk_sin(lat * RAD);
    int end = C.lut_row + n;
    if (end > n) end = n;
    for (int y = C.lut_row; y < end; y+=detail)
        for (int x = 0; x < n; x+=detail) {
            globe_lut *q = &C.lut[y * n + x];
            float nx = axis ? axis[x] : (x + .5f - r) / r,
                  ny = axis ? -axis[y] : -(y + .5f - r) / r, rr = nx * nx + ny * ny;
            q->z = 65535;
            if (rr > 1) continue;
            float z = root(1 - rr), bx = -ny * sl + z * cl, bz = ny * cl + z * sl,
                  la = atan_fast(bz, root(1 - bz * bz)) / RAD;
            q->lon = (int16_t)(atan_fast(nx, bx) / PI * 32767);
            q->lat = (int16_t)(la * 100);
            q->v = (uint16_t)clamp((90 - la) * 512 / 180, 0, 511);
            q->z = (uint16_t)(z * 32767);
        }
    GFX_COST_PX(GFX_COST_BLEND, (unsigned)((end - C.lut_row) * n) * 2u);
    C.lut_row = end;
    C.lut_publish = end == n;
    return 0;
}
static void build_rings_motion_reference(float lat,int detail)
{
    float sl = fk_sin(lat * RAD), cl = fk_cos(lat * RAD);
    int r = C.lut_r, ext = (int)ceilf(2.33f * r) + 2, ey = ring_extent(r, lat), w = ext * 2;
    int band = (int)ceilf(2.33f * r * fabsf(sl));
    memset(C.rings, 0, 416u * 236u * 2);
    if (fabsf(sl) > .001f)
        for (int y = ey - band; y < ey + band; y+=detail)
            for (int x = 0; x < w; x+=detail) {
                float px = (x + .5f - ext) / r, py = -(y + .5f - ey) / r, qx = -py / sl,
                      rho2 = qx * qx + px * px;
                if (rho2 <= 1.11f * 1.11f || rho2 >= 2.33f * 2.33f) continue;
                float rr = px * px + py * py, t = -py * cl / sl;
                int front = rr > 1 || t > root(1 - rr),
                    ri = (int)clamp((root(rho2) - 1.11f) / 1.22f * 511, 0, 511);
                C.rings[y * w + x] = (int16_t)((ri + 1) * (front ? 1 : -1));
            }
    C.ring_lat_valid = 1;
    GFX_COST_PX(GFX_COST_BLIT, (unsigned)(w * band * 2) * 4u);
}
static void render_globe_motion_reference(sprite*s,const body_state*b,float spin,float lat,int textured,int detail)
{
    int r = C.lut_r, n = r * 2, ext = b->ob == OB_SATURN ? (int)ceilf(2.33f * r) + 2 : r;
    int ey = b->ob == OB_SATURN ? ring_extent(r, lat) : r;
    const float *axis = card_coords_axis(C.coords, 0, r);
    s->w = ext * 2;
    s->h = ey * 2;
    memset(s->px, 0, (size_t)s->w * s->h * 2);
    float cl = fk_cos(lat * RAD), sl = fk_sin(lat * RAD), dl = (b->slon - b->olon - spin) * RAD,
          ss = fk_sin(b->slat * RAD), cs = fk_cos(b->slat * RAD);
    float bx = cs * fk_cos(dl), by = cs * fk_sin(dl), sx = by, sy = -bx * sl + ss * cl,
          sz = bx * cl + ss * sl;
    int offset = (int)((spin + b->olon + 180) * 1024 / 360);
    for (int y = 0; y < n; y+=detail)
        for (int x = 0; x < n; x+=detail) {
            const globe_lut *q = &C.lut[y * n + x];
            if (q->z == 65535) continue;
            float nx = axis ? axis[x] : (x + .5f - r) / r,
                  ny = axis ? -axis[y] : -(y + .5f - r) / r, nz = card_qz_exact(q->z),
                  real = nx * sx + ny * sy + nz * sz,
                  l = C.free ? fmaxf(.35f, -.25f * nx + .3f * ny + .92f * nz) : real;
            int u = (offset + (int)q->lon * 512 / 32767) & 1023;
            unsigned idx = q->v * 1024u + (unsigned)u;
            uint16_t color = textured && C.tex_ok ? texture_pixel(C.texture, (unsigned)idx) : ob_tint[b->ob];
            if (C.mode == 1) {
                float t = temperature(b, q->lat * .01f, q->lon * (180.f / 32767) + spin + b->olon, real);
                color = hc((t - ranges[b->ob][0]) / (ranges[b->ob][1] - ranges[b->ob][0]));
            } else {
                float k = b->ob == OB_SUN
                              ? C.lighting_coeff->k[256 + (int)clamp(nz * 255, 0, 255)]
                              : C.lighting_coeff->k[(int)clamp(l * 255, 0, 255)];
                uint16_t night = 0;
                float nk = 0;
                if (textured && C.night_ok && b->ob == OB_EARTH && !C.free && l < .12f) {
                    night = texture_pixel(C.night, (unsigned)idx);
                    nk = clamp((.12f - l) / .15f, 0, 1);
                }
                color = shade(color, k, night, nk);
            }
            int dx=x+ext-r,dy=y+ey-r;
            for(int yy=dy;yy<dy+detail && yy<s->h;yy++)for(int xx=dx;xx<dx+detail && xx<s->w;xx++)s->px[yy*s->w+xx]=color;
        }
    if (b->ob == OB_SATURN && fabsf(sl) > .001f) {
        int band = (int)ceilf(2.33f * r * fabsf(sl));
        for (int y = ey - band; y < ey + band; y+=detail)
            for (int x = 0; x < s->w; x+=detail) {
                float px = (x + .5f - ext) / r, py = -(y + .5f - ey) / r, qx = -py / sl, qy = px;
                int j = y * s->w + x;
                int ri = C.rings[j] - 1;
                if (ri < 0) continue;
                float dot = qx * bx + qy * by, perp = qx * qx + qy * qy - dot * dot;
                float k = C.free ? .95f : (dot < 0 && perp < 1) ? .12f : ss * sl > 0 ? .95f : .4f;
                uint16_t col = ob_tint[OB_SATURN];
                float alpha = .7f;
                if (textured && C.ring_ok) {
                    const uint8_t *rt = C.ring_tex + ri * 4;
                    col = GFX_RGB(rt[0], rt[1], rt[2]);
                    alpha = rt[3] / 255.f;
                }
                uint16_t shaded=shade(col,k,0,0);uint8_t aa=(uint8_t)(alpha*255);
                for(int yy=y;yy<y+detail && yy<s->h;yy++)for(int xx=x;xx<x+detail && xx<s->w;xx++) {int at=yy*s->w+xx;s->px[at]=gfx_blend565(s->px[at],shaded,aa);}
            }
        C.ring_lat_valid = 1;
        GFX_COST_PX(GFX_COST_BLIT, (unsigned)(s->w * band * 2) * 2u);
    }
    if(C.mode==2)halftone(s);
    /* Proxy estimates for tick math omitted by the stock raster model. These
     * are explicit estimates, NOT measured Xtensa timings. */
    GFX_COST_PX(GFX_COST_BLEND, (unsigned)(r * r * PI) * (C.mode == 1 ? 3u : 1u));

    if (C.mode == 2) GFX_COST_PX(GFX_COST_BLIT, (unsigned)(s->w * s->h) * 3u);
}