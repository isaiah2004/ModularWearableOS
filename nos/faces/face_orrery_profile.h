/* Private ORRERY stage/category accounting. Board timers / simulator operation costs.
 * Close a frame at the next tick: draw can run for many dirty/translated stripes. */
static int orr_category = OP_OTHER;
static int orr_model_stage = -1;
#ifdef GFX_COST_MODEL
extern void gfx_cost_orr_scope(int stage, int category);
extern uint32_t gfx_cost_orr_micros(void);
#define orr_prof_now() gfx_cost_orr_micros()
#define ORR_SCOPE(stage) gfx_cost_orr_scope(stage,orr_category)
#else
#define orr_prof_now() hal_micros()
#define ORR_SCOPE(stage) ((void)0)
#endif
#define orr_prof (S.profile)
void orrery_profile(int enabled)
{
    if (!orr_ok()) return;
    memset(&orr_prof, 0, sizeof orr_prof);
    orr_prof.enabled = enabled != 0;
    orr_prof.frame = nos_frame;
    orr_prof.last_build[0]=S.dot_builds;
    orr_prof.last_build[1]=S.orbit_pixels?S.orbit_pixels->builds:0;
    orr_prof.last_build[2]=S.label_builds;
    orr_prof.last_build[3]=S.ui_cache?S.ui_cache->builds:0;
}
static void orr_prof_stop(void)
{
    ORR_SCOPE(-1);
    orr_model_stage=-1;
    if (!orr_prof.active) return;
    uint32_t elapsed=orr_prof_now()-orr_prof.stamp;
    orr_prof.us[orr_prof.stage][orr_category] += elapsed;
    if(orr_prof.stage==OP_RASTER) { orr_prof.frame_us[4]+=elapsed; orr_prof.frame_cat[orr_category]+=elapsed; }
    orr_prof.active = 0;
}
static void orr_cat(int category)
{
    if (category == orr_category) return;
    if (orr_prof.active) {
        uint32_t now = orr_prof_now();
        orr_prof.us[orr_prof.stage][orr_category] += now - orr_prof.stamp;
        if(orr_prof.stage==OP_RASTER) { orr_prof.frame_us[4]+=now-orr_prof.stamp; orr_prof.frame_cat[orr_category]+=now-orr_prof.stamp; }
        orr_prof.stamp = now;
    }
    orr_category = category;
    ORR_SCOPE(orr_model_stage);
}
static void orr_prof_start(int stage)
{
    orr_model_stage=stage;
    ORR_SCOPE(stage);
    if (!orr_prof.enabled) return;
    orr_prof.stage = stage; orr_prof.active = 1;
    orr_prof.stamp = orr_prof_now();
    if (stage == OP_RASTER) orr_prof.drew = 1;
}
static void orr_prof_report(void)
{
    if(!orr_prof.enabled || !orr_prof.frames)return;
    char line[512];
    int used=snprintf(line,sizeof line,"ORRP window=%s n=%u r=%u b=%u cmd=%d ref=%lu hot=%d ob=%u op=%d db=%u lb=%u du=%u rb=%u ub=%u uh=%u us",
        orr_prof.scrub?"scrub":"general",orr_prof.frames,orr_prof.raster_frames,orr_prof.rebuilds,S.list?S.list->n:0,
        (unsigned long)(S.list?S.list->start[ORR_STRIPES]:0),
        0, /* stripe references now stay in PSRAM */
        S.orbit_pixels?S.orbit_pixels->builds:0,S.orbit_pixels?S.orbit_pixels->n:0,
        S.dot_builds,S.label_builds,S.list?S.list->dot_updates:0,S.ring_builds,
        S.ui_cache?S.ui_cache->builds:0,S.ui_cache?S.ui_cache->hits:0);
    static const char *const name[OP_N] = {"dots","orbits","planets","text","controls","trails","other"};
    for (int k=0;k<OP_N;k++)
        used+=snprintf(line+used,sizeof line-(size_t)used," %s=%lu/%lu/%lu",name[k],(unsigned long)(orr_prof.us[0][k]/orr_prof.frames),
               (unsigned long)(orr_prof.us[1][k]/orr_prof.frames),(unsigned long)(orr_prof.us[2][k]/orr_prof.frames));
    printf("%s\n",line);
    printf("ORRMEAN window=%s n=%u us=%lu/%lu/%lu/%lu/%lu math=%lu/%lu/%lu/%lu/%lu slots=sky/small/belt/events/anomaly\n",
        orr_prof.scrub?"scrub":"general",orr_prof.frames,
        (unsigned long)(orr_prof.sum_us[0]/orr_prof.frames),(unsigned long)(orr_prof.sum_us[1]/orr_prof.frames),
        (unsigned long)(orr_prof.sum_us[2]/orr_prof.frames),(unsigned long)(orr_prof.sum_us[3]/orr_prof.frames),
        (unsigned long)(orr_prof.sum_us[4]/orr_prof.frames),
        (unsigned long)(orr_prof.sum_detail[0]/orr_prof.frames),(unsigned long)(orr_prof.sum_detail[1]/orr_prof.frames),
        (unsigned long)(orr_prof.sum_detail[2]/orr_prof.frames),(unsigned long)(orr_prof.sum_detail[3]/orr_prof.frames),
        (unsigned long)(orr_prof.sum_detail[4]/orr_prof.frames));
    /* Worst face CPU frame, not a sum of independently occurring maxima.
     * us = astronomy/layout/capture/bucket/raster; builds = dot/orbit/label/UI.
     * Engine, transfer, other screens and contention are outside these scopes. */
    printf("ORRPEAK window=%s f=%lu us=%lu/%lu/%lu/%lu/%lu builds=%u/%u/%u/%u\n",
        orr_prof.scrub?"scrub":"general",(unsigned long)orr_prof.peak_frame,
        (unsigned long)orr_prof.peak_us[0],(unsigned long)orr_prof.peak_us[1],
        (unsigned long)orr_prof.peak_us[2],(unsigned long)orr_prof.peak_us[3],
        (unsigned long)orr_prof.peak_us[4],orr_prof.peak_build[0],orr_prof.peak_build[1],
        orr_prof.peak_build[2],orr_prof.peak_build[3]);
    printf("ORRPEAK_MATH us=%lu/%lu/%lu/%lu/%lu slots=sky/small/belt/events/anomaly\n",
        (unsigned long)orr_prof.peak_detail[0],(unsigned long)orr_prof.peak_detail[1],
        (unsigned long)orr_prof.peak_detail[2],(unsigned long)orr_prof.peak_detail[3],
        (unsigned long)orr_prof.peak_detail[4]);
    printf("ORRPEAK_DETAIL f=%lu ctx=%d/%d/%d/%d view=%d dial=%d raster=%lu/%lu/%lu/%lu/%lu/%lu/%lu source=%s\n",
        (unsigned long)orr_prof.peak_frame,
        orr_prof.peak_context[0],orr_prof.peak_context[1],orr_prof.peak_context[2],orr_prof.peak_context[3],
        orr_prof.peak_context[6],orr_prof.peak_context[7],
        (unsigned long)orr_prof.peak_cat[0],(unsigned long)orr_prof.peak_cat[1],
        (unsigned long)orr_prof.peak_cat[2],(unsigned long)orr_prof.peak_cat[3],
        (unsigned long)orr_prof.peak_cat[4],(unsigned long)orr_prof.peak_cat[5],
        (unsigned long)orr_prof.peak_cat[6],
#ifdef GFX_COST_MODEL
        "model"
#elif defined(_WIN32)
        "host"
#else
        "board"
#endif
    );
    /* Slots: projection / orbit coverage / dot bins / labels / ring calendar / menu.
     * Disambiguate ring-calendar work from orbit/dot builds nested in layout.
     * Ring opening and scene shrink are in thousandths, captured at the peak. */
    printf("ORRPEAK_LAYOUT us=%lu/%lu/%lu/%lu/%lu/%lu ring=%d shrink=%d slots=projection/orbits/dots/labels/calendar/menu\n",
        (unsigned long)orr_prof.peak_layout_us[0],(unsigned long)orr_prof.peak_layout_us[1],
        (unsigned long)orr_prof.peak_layout_us[2],(unsigned long)orr_prof.peak_layout_us[3],
        (unsigned long)orr_prof.peak_layout_us[4],(unsigned long)orr_prof.peak_layout_us[5],
        orr_prof.peak_context[4],orr_prof.peak_context[5]);
    printf("ORRMEAN_LAYOUT window=%s us=%lu/%lu/%lu/%lu/%lu/%lu arc_prepare=%lu arc_retain=%lu slots=projection/orbits/dots/labels/calendar/menu\n",
        orr_prof.scrub?"scrub":"general",
        (unsigned long)(orr_prof.sum_layout[0]/orr_prof.frames),(unsigned long)(orr_prof.sum_layout[1]/orr_prof.frames),
        (unsigned long)(orr_prof.sum_layout[2]/orr_prof.frames),(unsigned long)(orr_prof.sum_layout[3]/orr_prof.frames),
        (unsigned long)(orr_prof.sum_layout[4]/orr_prof.frames),(unsigned long)(orr_prof.sum_layout[5]/orr_prof.frames),
        (unsigned long)(orr_prof.sum_arc[0]/orr_prof.frames),(unsigned long)(orr_prof.sum_arc[1]/orr_prof.frames));
    /* These are real engine timers, never simulated operation prices. */
    printf("ORRMEAN_ENGINE window=%s record=%lu compare=%lu source=%s\n",orr_prof.scrub?"scrub":"general",
        (unsigned long)(orr_prof.sum_record/orr_prof.frames),(unsigned long)(orr_prof.sum_compare/orr_prof.frames),
#ifdef _WIN32
        "host"
#else
        "board"
#endif
    );
    orr_prof.peak_total=0;
    memset(orr_prof.us,0,sizeof orr_prof.us);
    memset(orr_prof.sum_us,0,sizeof orr_prof.sum_us);
    memset(orr_prof.sum_detail,0,sizeof orr_prof.sum_detail);
    memset(orr_prof.sum_layout,0,sizeof orr_prof.sum_layout);
    memset(orr_prof.sum_arc,0,sizeof orr_prof.sum_arc);
    orr_prof.sum_record=orr_prof.sum_compare=0;
    orr_prof.frames=orr_prof.rebuilds=orr_prof.raster_frames=0;
}
static void orr_prof_frame(void)
{
    if (!orr_prof.enabled || orr_prof.frame == nos_frame) return;
    for(int i=0;i<6;i++)orr_prof.sum_layout[i]+=orr_prof.layout_us[i];
    for(int i=0;i<2;i++) { orr_prof.sum_arc[i]+=orr_prof.arc_us[i];orr_prof.arc_us[i]=0; }
    orr_prof.sum_record+=nos_last.record_us;orr_prof.sum_compare+=nos_last.compare_us;
    unsigned builds[4]={S.dot_builds,S.orbit_pixels?S.orbit_pixels->builds:0,
                        S.label_builds,S.ui_cache?S.ui_cache->builds:0};
    uint32_t total=0;
    for(int i=0;i<5;i++) total+=orr_prof.frame_us[i];
    for(int i=0;i<5;i++) { orr_prof.sum_us[i]+=orr_prof.frame_us[i];orr_prof.sum_detail[i]+=orr_prof.detail_us[i]; }
    if(total>orr_prof.peak_total || !orr_prof.frames) {
        orr_prof.peak_total=total;orr_prof.peak_frame=orr_prof.frame;
        memcpy(orr_prof.peak_us,orr_prof.frame_us,sizeof orr_prof.peak_us);
        memcpy(orr_prof.peak_detail,orr_prof.detail_us,sizeof orr_prof.peak_detail);
        memcpy(orr_prof.peak_cat,orr_prof.frame_cat,sizeof orr_prof.peak_cat);
        memcpy(orr_prof.peak_context,orr_prof.context,sizeof orr_prof.peak_context);
        memcpy(orr_prof.peak_layout_us,orr_prof.layout_us,sizeof orr_prof.layout_us);
        for(int i=0;i<4;i++)orr_prof.peak_build[i]=builds[i]-orr_prof.last_build[i];
    }
    memcpy(orr_prof.last_build,builds,sizeof builds);
    memset(orr_prof.frame_us,0,sizeof orr_prof.frame_us);
    memset(orr_prof.detail_us,0,sizeof orr_prof.detail_us);
    memset(orr_prof.frame_cat,0,sizeof orr_prof.frame_cat);
    memset(orr_prof.layout_us,0,sizeof orr_prof.layout_us);
    orr_prof.frame = nos_frame;
    orr_prof.raster_frames += orr_prof.drew; orr_prof.drew = 0;
    if (++orr_prof.frames < 30) return;
    /* Average us per tick, including idle ticks; r/b disclose the denominator.
     * Each tuple is raster/capture/bucket. Printing is outside measured scopes. */
    orr_prof_report();
}

#define ORR_REBUILD() do { if(orr_prof.enabled) orr_prof.rebuilds++; } while(0)
/* Snapshot at the end of tick, before raster: AOD/transition/drag/coasting. */
static void orr_prof_context(const face_ctx *c)
{
    if(!orr_prof.enabled) return;
    orr_prof.context[0]=c->aod; orr_prof.context[1]=nos_transitioning();
    orr_prof.context[2]=S.drag;
    orr_prof.context[3]=S.pvx!=0 || S.pvy!=0 || S.home_t!=0;
    orr_prof.context[4]=(int)(S.ringe*1000+.5f);
    orr_prof.context[5]=(int)(S.sc*1000+.5f);
    orr_prof.context[6]=S.view;orr_prof.context[7]=S.d_ring;
}
#define ORR_DETAIL(slot,code) do { \
    uint32_t detail_stamp=orr_prof.enabled?orr_prof_now():0; \
    code; \
    if(orr_prof.enabled)orr_prof.detail_us[slot]+=orr_prof_now()-detail_stamp; \
} while(0)
#define ORR_LAYOUT(slot,code) do { \
    uint32_t orr_layout_stamp=orr_prof.enabled?orr_prof_now():0; \
    code; \
    if(orr_prof.enabled)orr_prof.layout_us[slot]+=orr_prof_now()-orr_layout_stamp; \
} while(0)

/* Called after closing the previous frame, before timing the next tick.
 * Both ends flush partial windows; unlock/coast cannot contaminate scrubs. */
static void orr_prof_scrub(int active)
{
    if(!orr_prof.enabled || active==orr_prof.scrub)return;
    orr_prof_report();
    orr_prof.scrub=active;
}
