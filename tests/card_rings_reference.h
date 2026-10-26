/* Immutable iteration13 original ring geometry oracle. */
static void build_rings_reference(float lat)
{
    float sl = fk_sin(lat * RAD), cl = fk_cos(lat * RAD);
    int r = C.lut_r, ext = (int)ceilf(2.33f * r) + 2, ey = ring_extent(r, lat), w = ext * 2;
    int band = (int)ceilf(2.33f * r * fabsf(sl));
    memset(C.rings, 0, 416u * 236u * 2);
    if (fabsf(sl) > .001f)
        for (int y = ey - band; y < ey + band; y++)
            for (int x = 0; x < w; x++) {
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
