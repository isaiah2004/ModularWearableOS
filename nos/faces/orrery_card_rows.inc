/* Bounded geometry/halftone rows. Original arithmetic/order retained; full-detail
 * geometry exploits exact horizontal symmetry. Coarse uses the original 2x2 lattice. */
static int card_lut_rows(int r,float lat,int rows,int detail)
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
    int end = C.lut_row + rows;
    if (end > n) end = n;
    for (int y = C.lut_row; y < end; y+=detail)
        for (int x = 0; x < (detail==1?r:n); x+=detail) {
            globe_lut *q = &C.lut[y * n + x];
            float nx = axis ? axis[x] : (x + .5f - r) / r,
                  ny = axis ? -axis[y] : -(y + .5f - r) / r, rr = nx * nx + ny * ny;
            globe_lut *mirror=&C.lut[y*n+n-1-x];
            q->z = 65535;
            if(detail==1)mirror->z=65535;
            if (rr > 1) continue;
            float z = root(1 - rr), bx = -ny * sl + z * cl, bz = ny * cl + z * sl,
                  la = atan_fast(bz, root(1 - bz * bz)) / RAD;
            q->lon = (int16_t)(atan_fast(nx, bx) / PI * 32767);
            q->lat = (int16_t)(la * 100);
            q->v = (uint16_t)clamp((90 - la) * 512 / 180, 0, 511);
            q->z = (uint16_t)(z * 32767);
            if(detail==1){*mirror=*q;mirror->lon=(int16_t)-q->lon;}
        }
    GFX_COST_PX(GFX_COST_BLEND, (unsigned)((end - C.lut_row) * n / (detail==1?2:detail*detail)) * 2u);
    C.lut_row = end;
    C.lut_publish = end == n;
    return 0;
}
static void card_ring_rows(float lat,int first,int end,int detail)
{
    float sl = fk_sin(lat * RAD), cl = fk_cos(lat * RAD);
    int r = C.lut_r, ext = (int)ceilf(2.33f * r) + 2, ey = ring_extent(r, lat), w = ext * 2;
    int band = (int)ceilf(2.33f * r * fabsf(sl));

    if (fabsf(sl) > .001f)
        for (int y=first>ey-band?first-((first-(ey-band))%detail):ey-band; y<end && y<ey+band; y+=detail) {
            float py=-(y+.5f-ey)/r,qx=-py/sl,t=-py*cl/sl;
            for (int x=0; x<(detail==1?ext:w); x+=detail) {
                float px = (x + .5f - ext) / r,
                      rho2 = qx * qx + px * px;
                if (rho2 <= 1.11f * 1.11f || rho2 >= 2.33f * 2.33f) continue;
                float rr = px * px + py * py;
                int front = rr > 1 || t > root(1 - rr),
                    ri = (int)clamp((root(rho2) - 1.11f) / 1.22f * 511, 0, 511);
                C.rings[y*w+x]=(int16_t)((ri+1)*(front?1:-1));
                if(detail==1)C.rings[y*w+w-1-x]=C.rings[y*w+x];
            }
        }
    C.ring_lat_valid = 1;
    GFX_COST_PX(GFX_COST_BLIT, (unsigned)(w * (end-first) / (detail==1?2:detail*detail)) * 4u);
}
static void card_halftone_rows(sprite*s,float mx,int first,int end)
{
    for (int y=first;y<end && y<s->h;y+=4)
        for (int x = 0; x < s->w; x += 4) {
            float r = 0, g = 0, b = 0;
            int n = 0;
            for (int j = y; j < y + 4 && j < s->h; j++)
                for (int i = x; i < x + 4 && i < s->w; i++) {
                    uint16_t c = s->px[j * s->w + i];
                    if (c) {
                        r += (c >> 11) * 255.f / 31;
                        g += ((c >> 5) & 63) * 255.f / 63;
                        b += (c & 31) * 255.f / 31;
                        n++;
                    }
                    s->px[j * s->w + i] = 0;
                }
            if (!n) continue;
            r /= n;
            g /= n;
            b /= n;
            float l = clamp((r * .3f + g * .59f + b * .11f) / 255 / mx, 0, 1);
            if (l < .04f) continue;
            float m = fmaxf(r, fmaxf(g, b)), k = (.6f + .4f * l) * 255 / fmaxf(m, 1),
                  rr = 5.0176f * l * n / 16;
            uint16_t c =
                GFX_RGB((int)clamp(r * k, 0, 255), (int)clamp(g * k, 0, 255), (int)clamp(b * k, 0, 255));
            for (int j = 0; j < 4 && y + j < s->h; j++)
                for (int i = 0; i < 4 && x + i < s->w; i++) {
                    float d = (i - 1.5f) * (i - 1.5f) + (j - 1.5f) * (j - 1.5f),
                          a = clamp(rr - d + .5f, 0, 1);
                    s->px[(y + j) * s->w + x + i] = shade(c, a, 0, 0);
                }
        }
}
