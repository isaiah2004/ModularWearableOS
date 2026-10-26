#include "card_prepare_label_reference.inc"
/* Frozen prepare_reference is independent of the resumable implementation. */
static unsigned card_prepare_incremental_tests(void) {
    static body_state baseline,candidate;
    unsigned cases=0,steps_total=0,seen=0;
    /* moons.bin is read through the asset worker (sim: synchronous): every body below sees the loaded block */
    for(int i=0;i<64 && od_moons_use(EP_SATURN)<=0;i++)od_sd_pump();
    assert(od_moons_ready());
    for(int ob=0;ob<OB_N;ob++)for(int date=0;date<9;date++) {
        double jd=2451545.0+date*173.125;
        prepare_reference(&baseline,ob,jd);prepare_extra_profile_impl(&baseline);
        memset(&candidate,0xa5,sizeof candidate);
        card_prepare_job_start(&candidate,ob,jd);
        assert(!candidate.ready && card_prepare_job_active());
        unsigned steps=0;
        while(card_prepare_job_active()) {
            int phase=card_prepare_job_phase();seen|=1u<<phase;
            unsigned char state_before[sizeof P];memcpy(state_before,&P,sizeof P);
            int cheap=card_prepare_job_cheap();
            assert(cheap==card_prepare_job_cheap());assert(!memcmp(state_before,&P,sizeof P));
            if(phase==PP_COUNT||phase==PP_LABEL_CANDIDATE)assert(cheap);
            if(phase==PP_GEOMETRY||phase==PP_TEXT||phase==PP_EARTH)assert(!cheap);
            if(phase==PP_MOON)assert(cheap==(P.i>=P.nmo));
            if(phase==PP_CRAFT && P.i<od_crafts_n) {
                const od_craft *c=&od_crafts[P.i];
                if((c->planet==P.planet&&c->has_orbit) ||
                   (ob==OB_EARTH&&!strcmp(c->name,"JWST")) ||
                   (ob==OB_MERCURY&&!strcmp(c->name,"BepiColombo")))assert(!cheap);
            }
            assert(card_prepare_job_cursor()>=0);
            int done=card_prepare_job_step();steps++;
            assert(steps<100000);
            if(done)assert(candidate.ready && !card_prepare_job_active());
            else assert(!candidate.ready);
        }
        assert(!memcmp(&baseline,&candidate,sizeof baseline));
        steps_total+=steps;cases++;
    }
    assert(seen&(1u<<PP_MOON));assert(seen&(1u<<PP_CRAFT));
    assert(seen&(1u<<PP_LABEL_CANDIDATE));assert(seen&(1u<<PP_EXTRA));
    for(int phase=PP_CLEAR;phase<=PP_MARK;phase++) {
        card_prepare_job_start(&candidate,OB_SATURN,2460100.125);
        unsigned steps=0;
        while(card_prepare_job_active()&&card_prepare_job_phase()!=phase){card_prepare_job_step();assert(++steps<100000);}
        assert(card_prepare_job_phase()==phase);
        card_prepare_job_cancel();body_state cancelled=candidate;
        assert(!card_prepare_job_step()&&!card_prepare_job_active());
        assert(!memcmp(&cancelled,&candidate,sizeof candidate));
    }
    /* Saturate label capacity: every candidate remains independently resumable. */
    for(int seed=0;seed<12;seed++) {
        memset(&baseline,0,sizeof baseline);baseline.ob=OB_SATURN;
        baseline.n=MAX_ITEMS;baseline.planet_r=12;baseline.direction_n=2;
        for(int i=0;i<MAX_ITEMS;i++) {
            moon_item *m=&baseline.items[i];float angle=(i*31+seed*7)*RAD;
            m->r=30+(i*13%100);m->x=233+m->r*fk_cos(angle);m->y=236-m->r*fk_sin(angle);
            snprintf(m->name,sizeof m->name,"MOON %d %d",seed,i);
        }
        for(int i=0;i<2;i++) {
            moon_item *m=&baseline.directions[i];m->lx=191+i*84;m->ly=116+i*280;
            snprintf(m->name,sizeof m->name,"DIRECTION %d",i);
        }
        candidate=baseline;card_prepare_labels_reference(&baseline);
        card_prepare_job_start(&candidate,OB_SATURN,2460100.125);
        P.phase=PP_SORT;P.i=0;
        unsigned steps=0;
        while(card_prepare_job_phase()!=PP_EVENT) {
            assert(!card_prepare_job_step());assert(++steps<30000);
        }
        assert(!memcmp(&baseline,&candidate,sizeof baseline));card_prepare_job_cancel();
    }
    card_prepare_job_start(NULL,OB_SUN,0);assert(!card_prepare_job_active());
    printf("PASS incremental prepare %u exact bodies, %u steps, all27 phase cancellations, state=%u bytes\n",cases,steps_total,(unsigned)sizeof P);
    return cases;
}
