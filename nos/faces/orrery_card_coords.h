/* Exact original division results; lifecycle belongs to card buffers. */
#ifndef ORRERY_CARD_COORDS_H
#define ORRERY_CARD_COORDS_H
#define CARD_COORD_SIDE 236
typedef struct {
    int radius[2];
    float axis[2][CARD_COORD_SIDE];
} card_coords;
static card_coords *card_coords_new(void)
{
#ifdef NOS_PSRAM_TRACKING
    if (nos_mem_available() < sizeof(card_coords) + 16384u) return 0;
#endif
    card_coords *c = hal_big_alloc(sizeof *c);
    if (c) c->radius[0] = c->radius[1] = 0;
    return c;
}
static const float *card_coords_axis(card_coords *c, int slot, int r)
{
    if (!c || r <= 0 || r * 2 > CARD_COORD_SIDE) return 0;
    if (c->radius[slot] != r) {
        for (int i = 0; i < r * 2; i++) c->axis[slot][i] = (i + .5f - r) / r;
        c->radius[slot] = r;
    }
    return c->axis[slot];
}
#endif
