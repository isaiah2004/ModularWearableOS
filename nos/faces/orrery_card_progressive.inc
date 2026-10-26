/* Cooperative renderer: UI thread only. Shared LUT/ring memory belongs to this job,
 * while complete front pixels remain immutable. Neighbour is never borrowed from
 * a pending/live preview. No new pixel allocation and no worker references. */
#include "orrery_card_rows.inc"
enum { CR_IDLE,CR_LUT,CR_RING_CLEAR,CR_RING,CR_CLEAR,CR_SHADER,CR_OVERLAY,CR_DOTMAX,CR_DOT,CR_PREVIEW,CR_READY };
static struct {
    int stage,row,scan,detail,front_detail,lut_detail,ring_detail;
    int mode,free,tex,night,ring,preview,r,w,h,was_moving,restore_pending;
    int lut_valid,ring_valid,lr,rr,front_r,front_mode,front_free;
    float spin,tilt,lat,mx,llat,rlat,front_spin,front_lat;
    unsigned asset,generation,published,cancelled,quanta,work_us,max_work_us,max_quantum_us;
    uint32_t started_ms,front_ms,front_pose_ms,rest_started_ms,restore_ms,pose_ms;
    float prior_spin,prior_tilt;
    unsigned preview_completed;
} R;
static void card_render_cancel(void) {
    if(R.stage!=CR_IDLE)R.cancelled++;
    if(R.stage!=CR_IDLE && R.stage!=CR_PREVIEW)C.dirty=1;
    if(R.stage==CR_LUT)R.lut_valid=0;
    if(R.stage==CR_RING_CLEAR||R.stage==CR_RING)R.ring_valid=0;
    R.stage=CR_IDLE;R.generation++;card_window.active=0;
}
static void card_render_invalidate(void) {
    card_render_cancel();R.lut_valid=R.ring_valid=0;R.front_detail=0;
    C.lut_r=C.lut_row=C.lut_publish=C.ring_lat_valid=0;
    R.pose_ms=hal_millis();R.prior_spin=C.spin;R.prior_tilt=C.tilt;R.rest_started_ms=R.restore_ms=0;R.was_moving=R.restore_pending=0;
}
void orrery_card_render_report(void) {
    unsigned now=hal_millis();
    printf("CARDWORK enabled=%d budget_us=%u cap=%u stage=%d row=%d detail=%d front_detail=%d age_ms=%u front_age_ms=%u backlog=%d published=%u cancelled=%u work_us=%u max_us=%u quantum_max_us=%u restore_ms=%u preview_done=%u\n",
           orrery_card_progressive,orrery_card_render_budget_us,orrery_card_render_quanta,R.stage,R.row,R.detail,R.front_detail,
           (unsigned)(R.stage?now-R.started_ms:0),(unsigned)(R.published?now-R.front_ms:0),
           R.stage&&(C.spin!=R.spin||C.tilt!=R.tilt||C.mode!=R.mode||C.free!=R.free||C.dirty),
           R.published,R.cancelled,R.work_us,R.max_work_us,R.max_quantum_us,(unsigned)R.restore_ms,R.preview_completed);
}
static void card_after_lut(void) {
    R.row=0;
    R.stage=C.nb->ob==OB_SATURN && !(R.ring_valid&&R.rr==R.r&&R.rlat==R.lat&&R.ring_detail==R.detail)
        ?CR_RING_CLEAR:CR_CLEAR;
}
static int card_render_start(float lat,int detail) {
    if(C.preview||C.pending||C.anim_n||C.peek||!C.nb||!C.neighbour.px)return 0;
    card_render_cancel();
    *C.nb=*C.b; /* body/epoch snapshot; never used as a live preview during this job */
    R.spin=C.spin;R.tilt=C.tilt;R.lat=lat;R.mode=C.mode;R.free=C.free;
    R.tex=C.tex_ok;R.night=C.night_ok;R.ring=C.ring_ok;R.asset=asset_generation;
    R.detail=detail;R.r=globe_radius(C.ob,lat);
    R.w=2*(C.ob==OB_SATURN?(int)ceilf(2.33f*R.r)+2:R.r);
    R.h=2*(C.ob==OB_SATURN?ring_extent(R.r,lat):R.r);
    C.neighbour.w=R.w;C.neighbour.h=R.h;
    R.row=R.scan=0;R.started_ms=hal_millis();R.preview=0;
    if(R.lut_valid&&R.lr==R.r&&R.llat==lat&&R.lut_detail==detail) {
        C.lut_r=R.r;C.lut_lat=lat;C.lut_row=2*R.r;card_after_lut();
    } else {R.lut_valid=0;R.ring_valid=0;C.lut_r=0;R.stage=CR_LUT;}
    C.dirty=0; /* subsequent asset/epoch invalidations remain pending during work */
    return 1;
}
static void card_preview_start(void) {
    card_render_cancel();R.stage=CR_PREVIEW;R.row=0;R.preview=1;
    R.started_ms=hal_millis();R.free=C.free;R.asset=asset_generation;
}
/* Defined below with the legacy preview, to keep the exact original shader. */
static void card_preview_rows(int first,int end);
static void card_render_quantum(void) {
    int rows=R.stage==CR_DOT?4:2;
    /* Clears have no per-pixel geometry. Larger bounded spans avoid spending
     * the deterministic work cap on hundreds of tiny memset calls. */
    if(R.stage==CR_RING_CLEAR||R.stage==CR_CLEAR)rows=16;
    if(R.detail==4 && (R.stage==CR_LUT||R.stage==CR_RING||R.stage==CR_SHADER||R.stage==CR_OVERLAY))rows=4;
    int end=R.row+rows;
    int mode=C.mode,free_mode=C.free,tex=C.tex_ok,night=C.night_ok,ring=C.ring_ok;
    C.mode=R.mode;C.free=R.free;C.tex_ok=R.tex;C.night_ok=R.night;C.ring_ok=R.ring;
    card_profile_token token=card_profile_begin();
    switch(R.stage) {
    case CR_LUT:
        card_lut_rows(R.r,R.lat,rows,R.detail);
        if(C.lut_row>=2*R.r){R.lut_valid=1;R.lr=R.r;R.llat=R.lat;R.lut_detail=R.detail;card_after_lut();}
        card_profile_end(CP_LUT,token);break;
    case CR_RING_CLEAR:
        if(end>236)end=236;
        memset(C.rings+R.row*416,0,(size_t)(end-R.row)*416*2);R.row=end;
        if(end==236){R.row=0;R.stage=CR_RING;}
        card_profile_end(CP_RINGS,token);break;
    case CR_RING:
        if(end>R.h)end=R.h;
        card_ring_rows(R.lat,R.row,end,R.detail);R.row=end;
        if(end==R.h){R.ring_valid=1;R.rr=R.r;R.rlat=R.lat;R.ring_detail=R.detail;R.row=0;R.stage=CR_CLEAR;}
        card_profile_end(CP_RINGS,token);break;
    case CR_CLEAR:
        if(end>R.h)end=R.h;
        memset(C.neighbour.px+R.row*R.w,0,(size_t)(end-R.row)*R.w*2);R.row=end;
        if(end==R.h){R.row=0;R.stage=CR_SHADER;}
        card_profile_end(CP_GLOBE,token);break;
    case CR_SHADER:case CR_OVERLAY:
        card_window.active=1;card_window.pass=R.stage==CR_SHADER?CW_SHADER:CW_OVERLAY;
        card_window.first=R.row;card_window.end=end;card_window.detail=R.detail;
        render_globe(&C.neighbour,C.nb,R.spin,R.lat,1);card_window.active=0;
        R.row=end;
        if(R.row>=(R.stage==CR_SHADER?2*R.r:R.h)) {
            R.row=0;
            if(R.stage==CR_SHADER && C.nb->ob==OB_SATURN)R.stage=CR_OVERLAY;
            else{R.stage=R.mode==2?CR_DOTMAX:CR_READY;R.scan=0;R.mx=.05f;}
        }
        break;
    case CR_DOTMAX: {
        int limit=R.scan+rows*R.w;if(limit>R.w*R.h)limit=R.w*R.h;
        for(;R.scan<limit;R.scan+=7) {
            uint16_t c=C.neighbour.px[R.scan];
            float l=(c>>11)*.3f/31+((c>>5)&63)*.59f/63+(c&31)*.11f/31;
            if(l>R.mx)R.mx=l;
        }
        if(R.scan>=R.w*R.h){R.row=0;R.stage=CR_DOT;}
        card_profile_end(CP_GLOBE,token);break;
    }
    case CR_DOT:
        card_halftone_rows(&C.neighbour,R.mx,R.row,end);R.row=end;
        if(R.row>=R.h)R.stage=CR_READY;
        card_profile_end(CP_GLOBE,token);break;
    case CR_PREVIEW:
        card_preview_rows(R.row,end);R.row=end;
        if(R.row>=2*radius(C.nb->ob)){C.preview_key=card_image_key();R.preview_completed++;R.stage=CR_IDLE;}
        card_profile_end(CP_PREVIEW,token);break;
    default:break;
    }
    C.mode=mode;C.free=free_mode;C.tex_ok=tex;C.night_ok=night;C.ring_ok=ring;
}
static void card_render_publish(void) {
    if(R.stage!=CR_READY||C.preview||C.pending||C.anim_n||C.peek)return;
    sprite old=C.globe;C.globe=C.neighbour;C.neighbour=old;
    C.key=card_image_key();C.last_spin=R.spin;C.last_tilt=R.tilt;C.last_mode=R.mode;C.last_free=R.free;
    C.lut_publish=0;R.front_detail=R.detail;R.front_r=R.r;R.front_spin=R.spin;R.front_lat=R.lat;
    R.front_mode=R.mode;R.front_free=R.free;R.front_ms=hal_millis();R.front_pose_ms=R.started_ms;R.published++;
    if(R.detail==1&&R.restore_pending){R.restore_ms=R.front_ms-R.rest_started_ms;R.restore_pending=0;}
    R.stage=CR_IDLE;
}
static int card_work_budget_hit(void) {
    unsigned cap=orrery_card_render_quanta;
    if(!cap)cap=1;
    if(cap>512)cap=512;
    if(card_frame_work_quanta>=cap)return 1;
#ifdef ESP_PLATFORM
    unsigned budget=orrery_card_render_budget_us;
    if(budget<250)budget=250;
    if(budget>8000)budget=8000;
    if((unsigned)(hal_micros()-card_frame_work_start)>=budget)return 1;
#endif
    return 0;
}
static void card_render_pump(void) {
    if(R.stage==CR_IDLE)return;
    if(card_frame_work_active && card_work_budget_hit())return;
    if(R.asset!=asset_generation || release_pending || (R.preview&&!C.preview)) {card_render_cancel();return;}
    uint32_t start=hal_micros();unsigned count=0;
    unsigned budget=orrery_card_render_budget_us,cap=orrery_card_render_quanta;
    if(budget<250)budget=250;
    if(budget>8000)budget=8000;
    if(!cap)cap=1;
    if(cap>512)cap=512;
    do {
        uint32_t q=hal_micros();card_render_quantum();unsigned elapsed=hal_micros()-q;
        if(elapsed>R.max_quantum_us)R.max_quantum_us=elapsed;
        R.quanta++;count++;
        if(card_frame_work_active){card_frame_work_quanta++;if(card_work_budget_hit())break;}
        if(R.stage==CR_IDLE||R.stage==CR_READY)break;
#ifdef ESP_PLATFORM
        if((unsigned)(hal_micros()-start)>=budget)break;
#endif
        /* Host replay uses a deterministic quantum cap, not process scheduling. */
    } while(count<cap);
    R.work_us=hal_micros()-start;if(R.work_us>R.max_work_us)R.max_work_us=R.work_us;
    card_render_publish();
}
