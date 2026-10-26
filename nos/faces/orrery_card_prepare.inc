#ifndef ORRERY_CARD_PREPARE_INC
#define ORRERY_CARD_PREPARE_INC
/* UI-only cooperative exact body preparation. Caller exclusively owns output
 * until step returns1. No allocation; original ordering and candidate tie breaks
 * are retained. One step is one bounded table item/candidate or fixed setup call. */
enum { PP_IDLE,PP_CLEAR,PP_EARTH,PP_HELIO,PP_GEOMETRY,PP_OBSERVE,PP_TEXT,
       PP_HEAT,PP_RANGE,PP_GRAPH,PP_DIRECTIONS,PP_COUNT,PP_EARTH_MOON,PP_MOON,
       PP_CRAFT,PP_COUNT_TEXT,PP_MAXR,PP_SCALE,PP_POSITION,PP_SORT,PP_WIDTH,
       PP_DIR_WIDTH,PP_ROWS,PP_LABEL_BEGIN,PP_LABEL_CANDIDATE,PP_EVENT,
       PP_EXTRA,PP_MARK };
static struct {
    body_state *out;
    int phase,ob,ep,i,nm,nc,planet,index,xx,yy,found,width,nmo;
    const od_moon *mo; /* the open planet's block of moons.bin, NULL until loaded */
    int order[MAX_ITEMS],item_width[MAX_ITEMS],direction_width[2];
    int row_left[15],row_right[15];
    unsigned cleared;
    double jd;
    ep_vec earth,h;
    ep_body sky;
    float maxr,k,wantx,wanty,best;
} P;
static void card_prepare_job_cancel(void) { P.phase=PP_IDLE;P.out=NULL; }
static int card_prepare_job_active(void) { return P.phase!=PP_IDLE; }
/* Scheduling hint only: these steps perform no ephemeris/formatting work.
 * Caller still checks elapsed time after EACH step, including label candidates. */
static int card_prepare_job_cheap(void) {
    if(P.phase==PP_LABEL_CANDIDATE || P.phase==PP_COUNT)return 1;
    if(P.phase==PP_MOON) {
        return P.i>=P.nmo;
    }
    if(P.phase==PP_CRAFT) {
        if(P.i>=od_crafts_n)return 1;
        const od_craft *c=&od_crafts[P.i];
        if(c->planet==P.planet && c->has_orbit)return 0;
        if(P.ob==OB_EARTH && !strcmp(c->name,"JWST"))return 0;
        if(P.ob==OB_MERCURY && !strcmp(c->name,"BepiColombo"))return 0;
        return 1;
    }
    return 0;
}
static int card_prepare_job_phase(void) { return P.phase; }
static int card_prepare_job_cursor(void) { return P.phase==PP_LABEL_CANDIDATE?P.index*435+(P.yy-116)/20*29+(P.xx-65)/12:P.i; }
static void card_prepare_job_start(body_state *out,int ob,double jd) {
    memset(&P,0,sizeof P);P.out=out;P.ob=ob;P.jd=jd;P.ep=ob_ephem(ob);
    if(out){out->ready=0;P.phase=PP_CLEAR;}
}
static int card_prepare_job_step(void) {
    if(!P.phase||!P.out)return 0;
    body_state *b=P.out;
    int ob=P.ob,ep=P.ep;
    double jd=P.jd;
    const heat_profile *p=&profiles[ob];
    switch(P.phase) {
    case PP_CLEAR: {
        unsigned bytes=(unsigned)sizeof *b-P.cleared;if(bytes>512)bytes=512;
        memset((unsigned char*)b+P.cleared,0,bytes);P.cleared+=bytes;
        if(P.cleared==sizeof *b){b->ob=ob;b->jd=jd;P.phase=PP_EARTH;}
        break;
    }
    case PP_EARTH:P.earth=ep_helio(EP_EARTH,jd);P.phase=PP_HELIO;break;
    case PP_HELIO:
        if(ob==OB_PLUTO)P.h=orrery_pluto(jd);
        else if(ob!=OB_SUN&&ob!=OB_MOON)P.h=ep_helio(ep,jd);
        P.phase=PP_GEOMETRY;break;
    case PP_GEOMETRY:
        ep_geometry(ep,jd,ob==OB_PLUTO?P.h:(ep_vec){0,0,0},&b->olat,&b->olon,&b->slat,&b->slon);
        P.phase=PP_OBSERVE;break;
    case PP_OBSERVE:
        b->sun_dist=length(P.h);P.sky=ep_observe(P.h,P.earth);P.phase=PP_TEXT;break;
    case PP_TEXT: {
        ep_vec earth=P.earth;ep_body sky=P.sky;
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
        b->low=1e20f;b->high=0;P.i=0;P.phase=PP_HEAT;break;
    }
    case PP_HEAT: {
        int i=P.i;
        if (p->pts[i][1] < b->low) b->low = p->pts[i][1];
        if (p->pts[i][1] > b->high) b->high = p->pts[i][1];
        if(++P.i>=p->n)P.phase=PP_RANGE;
        break;
    }
    case PP_RANGE:
    if (ob == OB_SUN)
        snprintf(b->range, sizeof b->range, "4K - 15.7M");
    else
        snprintf(b->range, sizeof b->range, "%.0f  %.0f", b->low - 273.15f, b->high - 273.15f);
    b->graph_n = p->vertical ? 33 : 49;
        P.i=0;P.phase=PP_GRAPH;break;
    case PP_GRAPH: {
        int i=P.i;
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
        if(++P.i>=b->graph_n)P.phase=pages(ob)==4?PP_DIRECTIONS:PP_EVENT;
        if(P.phase==PP_EVENT)P.i=0;
        break;
    }
    case PP_DIRECTIONS: {
        ep_vec earth=P.earth,h=P.h;
        P.planet=ob==OB_PLUTO?10:ep;
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
        P.i=0;P.phase=PP_COUNT;break;
    }
    case PP_COUNT:
        P.nm=od_moons_known(P.planet);
        if(ob!=OB_EARTH && P.nm) {
            int st=od_moons_use(P.planet);
            if(st>0)P.mo=od_moons_of(P.planet,&P.nmo);
            else b->moons_state=st<0?2:1;
        }
        P.phase=PP_EARTH_MOON;
        break;
    case PP_EARTH_MOON:
        if (ob == OB_EARTH) {
            double lo, la, km;
            ep_moon(jd, &lo, &la, &km);
            ep_vec v = {km * cos(la * RAD) * cos(lo * RAD), km * cos(la * RAD) * sin(lo * RAD),
                        km * sin(la * RAD)};
            char info[80];
            snprintf(info, sizeof info, "MOON / %.0f KM / %.0f%% LIT", km, 100 * ep_moon_lit(jd, 0));
            add_item(b, "MOON", v, 1, 0, info);
        }
        P.i=0;P.phase=PP_MOON;break;
    case PP_MOON: {
        if(P.i>=P.nmo){P.i=0;P.phase=PP_CRAFT;break;}
        int nm=P.nm;
            {
                const od_moon *m = &P.mo[P.i++];
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
        break;
    }
    case PP_CRAFT: {
        if(P.i>=od_crafts_n){P.phase=PP_COUNT_TEXT;break;}
        int i=P.i++,planet=P.planet;
            const od_craft *c = &od_crafts[i];
            if (c->planet == planet) {
                P.nc++;
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
        break;
    }
    case PP_COUNT_TEXT:
        card_count_text(b,P.nm,P.nc);
        P.maxr=4;P.i=0;P.phase=PP_MAXR;break;
    case PP_MAXR:
        if(P.i<b->n){float r=root(b->items[P.i++].r/radii[ob]);if(r>P.maxr)P.maxr=r;}
        else P.phase=PP_SCALE;
        break;
    case PP_SCALE:P.k=128/P.maxr;b->planet_r=clamp(P.k,9,18);P.i=0;P.phase=PP_POSITION;break;
    case PP_POSITION: {
        if(P.i>=b->n){P.i=0;P.phase=PP_SORT;break;}
        int i=P.i++;float k=P.k;
            moon_item *m = &b->items[i];
            float a = atan_fast(m->y, m->x);
            m->r = k * root(m->r / radii[ob]);
            m->x = 233 + m->r * fk_cos(a);
            m->y = 236 - m->r * fk_sin(a);
        break;
    }
    case PP_SORT:
        if(P.i<b->n){int i=P.i++,j=i;while(j>0&&b->items[P.order[j-1]].r<b->items[i].r){P.order[j]=P.order[j-1];j--;}P.order[j]=i;}
        else{P.i=0;P.phase=PP_WIDTH;}
        break;
    case PP_WIDTH:
        if(P.i<b->n){P.item_width[P.i]=ui_text_width(UI_F_SMALL,b->items[P.i].name,1);P.i++;}
        else{P.i=0;P.phase=PP_DIR_WIDTH;}
        break;
    case PP_DIR_WIDTH:
        if(P.i<b->direction_n){P.direction_width[P.i]=ui_text_width(UI_F_SMALL,b->directions[P.i].name,1);P.i++;}
        else{P.i=0;P.phase=PP_ROWS;}
        break;
    case PP_ROWS: {
        int yy=116+20*P.i,ascent=UI_F_SMALL->ascent,descent=UI_F_SMALL->line_height-ascent;
        ui_row_span((float)(yy<236?yy-ascent:yy+descent),24,&P.row_left[P.i],&P.row_right[P.i]);
        if(++P.i==15){P.index=0;P.phase=PP_LABEL_BEGIN;}
        break;
    }
    case PP_LABEL_BEGIN: {
        if(P.index>=b->n){P.i=0;P.phase=PP_EVENT;break;}
        int i=P.order[P.index];moon_item *m=&b->items[i];
        m->lx=clamp(m->x,85,381);m->ly=clamp(m->y+(m->y>236?20:-12),110,386);
        P.wantx=m->lx;P.wanty=m->ly;P.best=1e20f;P.found=0;P.width=P.item_width[i];P.xx=65;P.yy=116;
        P.phase=PP_LABEL_CANDIDATE;break;
    }
    case PP_LABEL_CANDIDATE: {
        int index=P.index,i=P.order[index],xx=P.xx,yy=P.yy,width=P.width;
        int ascent=UI_F_SMALL->ascent,descent=UI_F_SMALL->line_height-ascent;
        float wantx=P.wantx,wanty=P.wanty;
        moon_item *m=&b->items[i];
        do {
                    float cost = (xx - wantx) * (xx - wantx) + (yy - wanty) * (yy - wanty);
                    if(cost>=P.best) break;
                    int left=P.row_left[(yy-116)/20],right=P.row_right[(yy-116)/20];
                    if (xx - width / 2 < left || xx + width / 2 > right) break;
                    float dx = fmaxf(0, (float)abs(xx - 233) - width * .5f),
                          dy = fmaxf(0, fmaxf(yy - ascent - 236, 236 - yy - descent));
                    float disc = b->planet_r * (ob == OB_SATURN ? 1.6f : 1) + 8;
                    if (dx * dx + dy * dy < disc * disc) break;
                    int collision = 0;
                    for (int j = 0; j < b->direction_n; j++) {
                        const moon_item *d = &b->directions[j];
                        int dw = P.direction_width[j];
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
                        moon_item *prev = &b->items[P.order[j]];
                        int pw = P.item_width[P.order[j]];
                        if (fabsf(xx - prev->lx) < (width + pw) * .5f + 10 &&
                            fabsf(yy - prev->ly) < UI_F_SMALL->line_height + 6) {
                            collision = 1;
                            break;
                        }
                    }
                    if (!collision && cost < P.best) {
                        P.best = cost;
                        m->lx = (float)xx;
                        m->ly = (float)yy;
                        P.found = 1;
                    }
        } while(0);
        P.xx+=12;if(P.xx>401){P.xx=65;P.yy+=20;}
        if(P.yy>396){if(!P.found){m->lx=P.wantx;m->ly=P.wanty;}P.index++;P.phase=PP_LABEL_BEGIN;}
        break;
    }
    case PP_EVENT: {
        int ne;const ep_event *ev=orrery_events(&ne);
        if(P.i>=ne){P.i=0;P.phase=PP_EXTRA;break;}
        int i=P.i++;
        if (ev[i].jd >= jd - 1 && ((ev[i].kind == EV_LINEUP && ep < EP_N && (ev[i].mask & (1 << ep))) ||
                                   (ev[i].kind != EV_LINEUP && (ev[i].a == ep || ev[i].b == ep)))) {
            char title[40];
            ep_event_title(&ev[i], title, sizeof title);
            snprintf(b->next, sizeof b->next, "%s / %d D", title, (int)(ev[i].jd - jd));
            P.i=ne;
        }        break;
    }
    case PP_EXTRA:
        if(P.i<p->ny){int i=P.i++;b->graph[49+i].y=graph_y(p,p->yt[i].v);}
        else P.phase=PP_MARK;
        break;
    case PP_MARK:
        if(p->mark_text)b->mark=p->vertical?graph_y(p,p->mark):100+280*(p->mark-p->x0)/(p->x1-p->x0);
        b->ready=1;P.phase=PP_IDLE;P.out=NULL;return 1;
    default:card_prepare_job_cancel();break;
    }
    return 0;
}

#endif
