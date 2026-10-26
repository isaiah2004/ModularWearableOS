/* Frozen original label search, independent of cooperative candidate state. */
static void card_prepare_labels_reference(body_state *b) {
    int ob=b->ob;
        /* Position every marker before placing labels. Use complete font boxes,
         * not just centres, for the disc, marker and label exclusions. */
        int ascent = UI_F_SMALL->ascent, descent = UI_F_SMALL->line_height - ascent;
        /* Outer markers have less room beside the round edge and header. Place
         * them first so a close-in orbiter cannot displace their names far away. */
        int order[MAX_ITEMS];
        for (int i = 0; i < b->n; i++) {
            int j = i;
            while (j > 0 && b->items[order[j - 1]].r < b->items[i].r) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = i;
        }
        for (int index = 0; index < b->n; index++) {
            int i = order[index];
            moon_item *m = &b->items[i];
            m->lx = clamp(m->x, 85, 381);
            m->ly = clamp(m->y + (m->y > 236 ? 20 : -12), 110, 386);
            float wantx = m->lx, wanty = m->ly, best = 1e20f;
            int found = 0;
            int width = ui_text_width(UI_F_SMALL, m->name, 1);
            for (int yy = 116; yy <= 396; yy += 20)
                for (int xx = 65; xx <= 401; xx += 12) {
                    int left, right;
                    ui_row_span((float)(yy < 236 ? yy - ascent : yy + descent), 24, &left, &right);
                    if (xx - width / 2 < left || xx + width / 2 > right) continue;
                    float dx = fmaxf(0, (float)abs(xx - 233) - width * .5f),
                          dy = fmaxf(0, fmaxf(yy - ascent - 236, 236 - yy - descent));
                    float disc = b->planet_r * (ob == OB_SATURN ? 1.6f : 1) + 8;
                    if (dx * dx + dy * dy < disc * disc) continue;
                    int collision = 0;
                    for (int j = 0; j < b->direction_n; j++) {
                        const moon_item *d = &b->directions[j];
                        int dw = ui_text_width(UI_F_SMALL, d->name, 1);
                        if (fabsf(xx - d->lx) < (width + dw) * .5f + 10 &&
                            fabsf(yy - d->ly) < UI_F_SMALL->line_height + 6) collision = 1;
                    }
                    for (int j = 0; j < b->n; j++) {
                        const moon_item *other = &b->items[j];
                        if (fabsf(xx - other->x) < width * .5f + 9 &&
                            other->y > yy - ascent - 9 && other->y < yy + descent + 9) {
                            collision = 1;
                            break;
                        }
                    }
                    for (int j = 0; j < index; j++) {
                        moon_item *prev = &b->items[order[j]];
                        int pw = ui_text_width(UI_F_SMALL, prev->name, 1);
                        if (fabsf(xx - prev->lx) < (width + pw) * .5f + 10 &&
                            fabsf(yy - prev->ly) < UI_F_SMALL->line_height + 6) {
                            collision = 1;
                            break;
                        }
                    }
                    float cost = (xx - wantx) * (xx - wantx) + (yy - wanty) * (yy - wanty);
                    if (!collision && cost < best) {
                        best = cost;
                        m->lx = (float)xx;
                        m->ly = (float)yy;
                        found = 1;
                    }
                }
            if (!found) {
                m->lx = wantx;
                m->ly = wanty;
            }
        }
}
