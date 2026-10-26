static void prepare_reference(body_state *b, int ob, double jd)
{
    memset(b, 0, sizeof *b);
    b->ob = ob;
    b->jd = jd;
    ep_vec earth = ep_helio(EP_EARTH, jd), h = {0, 0, 0};
    int ep = ob_ephem(ob);
    if (ob == OB_PLUTO)
        h = orrery_pluto(jd);
    else if (ob != OB_SUN && ob != OB_MOON)
        h = ep_helio(ep, jd);
    ep_geometry(ep, jd, ob == OB_PLUTO ? h : (ep_vec){0, 0, 0}, &b->olat, &b->olon, &b->slat, &b->slon);
    b->sun_dist = length(h);
    ep_body sky = ep_observe(h, earth);
    b->dist = sky.dist;
    b->elong = (float)sky.elong;
    b->lit = (float)sky.lit;
    if (ob == OB_EARTH) {
        b->dist = 0;
        snprintf(b->hero, sizeof b->hero, "%.3f", b->sun_dist);
        snprintf(b->unit, sizeof b->unit, "AU FROM THE SUN");
    } else {
        if (ob == OB_MOON) {
            double lo, la, km;
            int waxing;
            ep_moon(jd, &lo, &la, &km);
            b->dist = km / AU;
            b->sun_dist = length(earth);
            b->lit = (float)ep_moon_lit(jd, &waxing);
            snprintf(b->sub, sizeof b->sub, "%d%% LIT / %s", (int)(b->lit * 100 + .5f),
                     waxing ? "WAXING" : "WANING");
        }
        char lt[24];
        light_time(b->dist, lt, sizeof lt);
        if (b->dist < .01)
            snprintf(b->hero, sizeof b->hero, "%.0f", b->dist * AU);
        else
            snprintf(b->hero, sizeof b->hero, b->dist < 2 ? "%.3f" : "%.2f", b->dist);
        snprintf(b->unit, sizeof b->unit, "%s FROM YOU / %s", b->dist < .01 ? "KM" : "AU", lt);
        if (ob != OB_SUN && ob != OB_MOON) {
            const char *side = sky.elong < 15 ? "LOST IN THE SUN"
                               : sky.evening  ? "EVENING SKY"
                                              : "MORNING SKY";
            if (ob == OB_VENUS)
                snprintf(b->sub, sizeof b->sub, "%d%% LIT / %s", (int)(100 * b->lit + .5f), side);
            else if (ob == OB_SATURN)
                snprintf(b->sub, sizeof b->sub, "RINGS %.1f DEG / %s", fabsf(b->olat), side);
            else
                snprintf(b->sub, sizeof b->sub, "%s / %.0f DEG", side, sky.elong);
        }
    }
    const heat_profile *p = &profiles[ob];
    b->low = 1e20f;
    b->high = 0;
    for (int i = 0; i < p->n; i++) {
        if (p->pts[i][1] < b->low) b->low = p->pts[i][1];
        if (p->pts[i][1] > b->high) b->high = p->pts[i][1];
    }
    if (ob == OB_SUN)
        snprintf(b->range, sizeof b->range, "4K - 15.7M");
    else
        snprintf(b->range, sizeof b->range, "%.0f  %.0f", b->low - 273.15f, b->high - 273.15f);
    b->graph_n = p->vertical ? 33 : 49;
    for (int i = 0; i < b->graph_n; i++) {
        float f = (float)i / (b->graph_n - 1), v, t;
        point *q = &b->graph[i];
        if (p->vertical) {
            v = p->ylog ? p->y0 * powf(p->y1 / p->y0, f) : p->y0 + (p->y1 - p->y0) * f;
            t = interp(p, v);
            q->x = 100 + 280 * (t - p->x0) / (p->x1 - p->x0);
            q->y = graph_y(p, v);
        } else {
            v = p->x0 + (p->x1 - p->x0) * f;
            t = interp(p, v);
            q->x = 100 + 280 * f;
            q->y = graph_y(p, t);
        }
        q->d = 4.8f;
        q->c = hc((t - b->low) / (b->high - b->low));
    }
    if (pages(ob) == 4) {
        int planet = ob == OB_PLUTO ? 10 : ep, nm = od_moon_count[planet < 0 ? 0 : planet], nc = 0, nmo = 0;
        const od_moon *mo = NULL;
        if (ob != OB_EARTH && nm) {
            int st = od_moons_use(planet);
            if (st > 0) mo = od_moons_of(planet, &nmo);
            else b->moons_state = st < 0 ? 2 : 1;
        }
        ep_vec sun = {-h.x, -h.y, -h.z}, you = {earth.x - h.x, earth.y - h.y, earth.z - h.z};
        ep_pole_frame(ep, sun, &b->sx, &b->sy);
        ep_pole_frame(ep, you, &b->ex, &b->ey);
        float sn = root(b->sx * b->sx + b->sy * b->sy), en = root(b->ex * b->ex + b->ey * b->ey);
        if (ob != OB_EARTH && sn > 0 && en > 0 && (b->sx * b->ex + b->sy * b->ey) / (sn * en) > .98f)
            add_direction(b, b->ex, b->ey, "SUN/YOU");
        else {
            add_direction(b, b->sx, b->sy, "SUN");
            if (ob != OB_EARTH) add_direction(b, b->ex, b->ey, "YOU");
        }
        if (ob == OB_EARTH) {
            double lo, la, km;
            ep_moon(jd, &lo, &la, &km);
            ep_vec v = {km * cos(la * RAD) * cos(lo * RAD), km * cos(la * RAD) * sin(lo * RAD),
                        km * sin(la * RAD)};
            char info[80];
            snprintf(info, sizeof info, "MOON / %.0f KM / %.0f%% LIT", km, 100 * ep_moon_lit(jd, 0));
            add_item(b, "MOON", v, 1, 0, info);
        }
        for (int i = 0; i < nmo; i++) {
                const od_moon *m = &mo[i];
                ep_vec v = ep_sat_pos(&m->el, jd);
                if (m->fresh || nm < 6) {
                    char info[96];
                    snprintf(info, sizeof info, "%s / %.0f KM / %.2f D", m->name, m->el.a, m->el.P);
                    add_item(b, m->name, v, m->fresh, 0, info);
                } else if (b->minor_n < 512) {
                    float a = atan_fast((float)v.y, (float)v.x), r = 198 + (m->code % 7);
                    point q = {233 + r * fk_cos(a), 236 - r * fk_sin(a), 1.8f, C_DIS};
                    if (q.y >= 110) b->minor[b->minor_n++] = q;
                }
            }
        for (int i = 0; i < od_crafts_n; i++) {
            const od_craft *c = &od_crafts[i];
            if (c->planet == planet) {
                nc++;
                if (c->has_orbit) {
                    char info[96];
                    snprintf(info, sizeof info, "%s / %s", c->name, c->status);
                    add_item(b, c->name, ep_sat_pos(&c->orbit, jd), 1, 1, info);
                }
            }
            if (ob == OB_EARTH && !strcmp(c->name, "JWST")) {
                ep_vec e = ep_helio(EP_EARTH, od_pack_jd),
                       v = {(c->xyz[0] - e.x) * AU, (c->xyz[1] - e.y) * AU, (c->xyz[2] - e.z) * AU};
                char info[96];
                snprintf(info, sizeof info, "JWST / %.2f M KM / L2 / PACK %s", length(v) / 1e6, od_built);
                add_item(b, "JWST", v, 1, 1, info);
            }
            if (ob == OB_MERCURY && !strcmp(c->name, "BepiColombo")) {
                ep_vec m = ep_helio(EP_MERCURY, od_pack_jd),
                       v = {c->xyz[0] - m.x, c->xyz[1] - m.y, c->xyz[2] - m.z};
                snprintf(b->bepi, sizeof b->bepi, "BEPICOLOMBO %.1f M KM / %d D", length(v) * AU / 1e6,
                         (int)(2461365.5 - jd));
            }
        }
        if (b->moons_state)
            snprintf(b->count, sizeof b->count, "%s / %d ORBITER%s", b->moons_state == 2 ? "MOON DATA MISSING" : "MOONS LOADING", nc, nc == 1 ? "" : "S");
        else
            snprintf(b->count, sizeof b->count, "%d KNOWN / %d ORBITER%s", nm, nc, nc == 1 ? "" : "S");
        float maxr = 4;
        for (int i = 0; i < b->n; i++) {
            float r = root(b->items[i].r / radii[ob]);
            if (r > maxr) maxr = r;
        }
        /* The full named orbit system fits below the title/count block. */
        float k = 128 / maxr;
        b->planet_r = clamp(k, 9, 18);
        for (int i = 0; i < b->n; i++) {
            moon_item *m = &b->items[i];
            float a = atan_fast(m->y, m->x);
            m->r = k * root(m->r / radii[ob]);
            m->x = 233 + m->r * fk_cos(a);
            m->y = 236 - m->r * fk_sin(a);
        }
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
    int ne;
    const ep_event *ev = orrery_events(&ne);
    for (int i = 0; i < ne; i++)
        if (ev[i].jd >= jd - 1 && ((ev[i].kind == EV_LINEUP && ep < EP_N && (ev[i].mask & (1 << ep))) ||
                                   (ev[i].kind != EV_LINEUP && (ev[i].a == ep || ev[i].b == ep)))) {
            char title[40];
            ep_event_title(&ev[i], title, sizeof title);
            snprintf(b->next, sizeof b->next, "%s / %d D", title, (int)(ev[i].jd - jd));
            break;
        }
    b->ready = 1;
}