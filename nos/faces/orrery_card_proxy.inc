/* Short-lived projected-front response while a coherent pose is refined. This is
 * deliberately a motion approximation: lighting is baked in the source. The
 * narrow angular/time limits avoid presenting an unbounded old hemisphere/ring.
 * Outside those limits the complete front remains visible, diagnosed as stale. */
int orrery_card_motion_proxy=1;
static int card_proxy_sample_step(void) { return orrery_card_motion_step>=4?4:2; }
static struct { int active;float spin,lat;unsigned key,last_key,key_frame,front_published,frames,suppressed,disoccluded,moving_frames,max_source_age,max_error_age,stale_run,max_stale_run;
    float max_spin_error,max_lat_error;uint32_t since,frame; } V;
static void card_proxy_update(void) {
    V.active=0;
    if(V.front_published!=R.published){V.front_published=R.published;V.since=0;}
    if(!orrery_card_progressive||!R.front_detail||!C.key||C.mode!=R.front_mode||C.free!=R.front_free)return;
    if(C.pos>=1 || nos_top()!=&scr_orrery_card || C.preview)return;
    float ds=norm(C.spin-R.front_spin),target=roundf(clamp(C.b->olat+C.tilt,-88,88)*.5f)*2;
    float dt=target-R.front_lat;
    if(fabsf(ds)<.01f && fabsf(dt)<.01f){V.since=0;V.stale_run=0;return;}
    if(!V.since)V.since=hal_millis();
    int fresh=orrery_card_motion_proxy && fabsf(ds)<=5 && fabsf(dt)<=3 && (unsigned)(hal_millis()-V.since)<=200;
    if(V.frame!=(unsigned)nos_frame) {
        V.frame=(unsigned)nos_frame;V.moving_frames++;
        unsigned age=hal_millis()-R.front_pose_ms,error_age=hal_millis()-V.since;
        if(age>V.max_source_age)V.max_source_age=age;
        if(error_age>V.max_error_age)V.max_error_age=error_age;
        if(fabsf(ds)>V.max_spin_error)V.max_spin_error=fabsf(ds);
        if(fabsf(dt)>V.max_lat_error)V.max_lat_error=fabsf(dt);
        if(!fresh){V.suppressed++;V.stale_run++;if(V.stale_run>V.max_stale_run)V.max_stale_run=V.stale_run;}
        else V.stale_run=0;
    }
    if(!fresh)return;
    V.active=1;V.spin=ds;V.lat=target;
    V.key=gfx_hash_u(gfx_hash_f(gfx_hash_f(gfx_hash_u(0xCA14u,C.key),ds),target),
                     (unsigned)card_proxy_sample_step());
    if(V.key!=V.last_key && V.key_frame!=(unsigned)nos_frame){V.frames++;V.last_key=V.key;V.key_frame=(unsigned)nos_frame;}
}
static int card_proxy_nearest(float v){return (int)(v+(v<0?-.5f:.5f));}
static void card_proxy_blit(gfx_fb*fb,const sprite*s,int x,int y) {
    if(gfx_rec_cmd(fb,x,y,x+s->w,y+s->h,V.key))return;
    card_profile_token token=card_profile_begin();
    int c0=fb->cx0,c1=fb->cx1;
    if(c1<=c0){c0=0;c1=GFX_W;}
    if(c0<fb->x_base)c0=fb->x_base;
    int right=fb->x_base+(fb->stride?fb->stride:GFX_W);if(c1>right)c1=right;
    if(c0<0)c0=0;
    if(c1>GFX_W)c1=GFX_W;
    int bx=x+fb->ox,by=y+fb->oy-fb->y_base;
    int first=by<0?-by:0,end=s->h;
    if(end>fb->h-by)end=fb->h-by;
    int left=c0-bx;if(left<0)left=0;
    int last=c1-bx;if(last>s->w)last=s->w;
    if(first>=end||left>=last){card_profile_end(CP_PROXY,token);return;}
    float sn=fk_sin(V.spin*RAD),cs=fk_cos(V.spin*RAD),dt=(V.lat-R.front_lat)*RAD;
    float st=fk_sin(dt),ct=fk_cos(dt),oldsl=fk_sin(R.front_lat*RAD),newsl=fk_sin(V.lat*RAD);
    float ratio=fabsf(newsl)>.02f?oldsl/newsl:1,inv=1.f/R.front_r;
    int ext=s->w/2,ey=s->h/2,step=card_proxy_sample_step();unsigned samples=0;
    /* Match the selected moving detail. Reprojecting a 4x4 snapshot at 2x2
     * repeats geometry work without recovering the missing source detail. */
    for(int yy=first&~(step-1);yy<end;yy+=step)for(int xx=left&~(step-1);xx<last;xx+=step) {
        float nx=(xx+.5f-ext)*inv,ny=-(yy+.5f-ey)*inv,rr=nx*nx+ny*ny;
        uint16_t color=0;
        if(rr<=1) {
            float z=root(1-rr),tx=nx*cs+z*sn,tz=z*cs-nx*sn,ty=ny*ct+tz*st,oz=tz*ct-ny*st;
            int sx=card_proxy_nearest(tx*R.front_r+ext-.5f),sy=card_proxy_nearest(-ty*R.front_r+ey-.5f);
            if(oz>0 && sx>=0&&sx<s->w&&sy>=0&&sy<s->h)color=s->px[sy*s->w+sx];
            else {color=ob_tint[C.ob];V.disoccluded++;}
        } else if(s->w>R.front_r*2) {
            /* Ring pixels cannot reveal old globe pixels beyond the old sphere. */
            int sx=card_proxy_nearest(nx*R.front_r+ext-.5f),sy=card_proxy_nearest((yy+.5f-ey)*ratio+ey-.5f);
            float ox=(sx+.5f-ext)*inv,oy=-(sy+.5f-ey)*inv;
            if(ox*ox+oy*oy>1 && sx>=0&&sx<s->w&&sy>=0&&sy<s->h)color=s->px[sy*s->w+sx];
        }
        color=gfx_ink(fb,color);
        for(int dy=yy;dy<yy+step&&dy<end;dy++)if(dy>=first) {
            uint16_t *dst=gfx_row(fb,by+dy);
            for(int dx=xx;dx<xx+step&&dx<last;dx++)if(dx>=left)dst[bx+dx]=color;
        }
        samples++;
    }
    GFX_COST(GFX_COST_BLIT,(unsigned)(end-first)*(last-left));
    card_profile_end(CP_PROXY,token);
    GFX_COST_PX(GFX_COST_BLEND,samples*3u); /* estimate only; target profiling required */
}

static float card_proxy_lat_error(void) {
    if(!C.b || !C.b->ready)return 0;
    return roundf(clamp(C.b->olat+C.tilt,-88,88)*.5f)*2-R.front_lat;
}
static void card_proxy_report(void) {
    printf("CARDMOTION proxy=%d active=%d unique_proxy=%u suppressed=%u disoccluded_samples=%u age_ms=%u delta_spin=%.3f delta_lat=%.3f moving=%u max_source_age_ms=%u max_error_age_ms=%u max_stale_run=%u max_spin_error=%.3f max_lat_error=%.3f limits_deg=5,3 limit_ms=200\n",
        orrery_card_motion_proxy,V.active,V.frames,V.suppressed,V.disoccluded,(unsigned)(V.since?hal_millis()-V.since:0),
        (double)norm(C.spin-R.front_spin),(double)card_proxy_lat_error(),V.moving_frames,V.max_source_age,V.max_error_age,V.max_stale_run,
        (double)V.max_spin_error,(double)V.max_lat_error);
}
