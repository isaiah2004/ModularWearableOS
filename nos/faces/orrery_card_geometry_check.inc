/* Diagnostic only: independent original scalar expressions, noallocation.
 * Call outside capture windows after exact geometry has completed. */
unsigned orrery_card_geometry_check(void) {
    if(!orrery_card_progressive || card_prepare_job_active() || C.preview || C.pending || release_pending || R.stage || !R.lut_valid || R.lut_detail!=1 || C.lut_r!=R.lr || C.lut_lat!=R.llat)return ~0u;
    int r=R.lr,n=2*r;float lat=R.llat,cl=fk_cos(lat*RAD),sl=fk_sin(lat*RAD);
    unsigned bad=0;
    for(int y=0;y<n;y++)for(int x=0;x<n;x++) {
        const globe_lut*q=C.lut+y*n+x;
        float nx=(x+.5f-r)/r,ny=-(y+.5f-r)/r,rr=nx*nx+ny*ny;
        if(rr>1){bad+=q->z!=65535;continue;}
        float z=root(1-rr),bx=-ny*sl+z*cl,bz=ny*cl+z*sl,la=atan_fast(bz,root(1-bz*bz))/RAD;
        globe_lut ref;
        ref.lon=(int16_t)(atan_fast(nx,bx)/PI*32767);ref.lat=(int16_t)(la*100);
        ref.v=(uint16_t)clamp((90-la)*512/180,0,511);ref.z=(uint16_t)(z*32767);
        bad+=memcmp(q,&ref,sizeof ref)!=0;
    }
    if(C.ob==OB_SATURN) {
        if(!R.ring_valid||R.ring_detail!=1||R.rr!=r||R.rlat!=lat)return ~0u;
        int ext=(int)ceilf(2.33f*r)+2,ey=ring_extent(r,lat),w=ext*2;
        int band=(int)ceilf(2.33f*r*fabsf(sl));
        for(int y=0;y<ey*2;y++)for(int x=0;x<w;x++) {
            int16_t ref=0;
            if(fabsf(sl)>.001f && y>=ey-band && y<ey+band) {
                float px=(x+.5f-ext)/r,py=-(y+.5f-ey)/r,qx=-py/sl,rho2=qx*qx+px*px;
                if(rho2>1.11f*1.11f && rho2<2.33f*2.33f) {
                    float rr=px*px+py*py,t=-py*cl/sl;
                    int front=rr>1 || t>root(1-rr),ri=(int)clamp((root(rho2)-1.11f)/1.22f*511,0,511);
                    ref=(int16_t)((ri+1)*(front?1:-1));
                }
            }
            bad+=C.rings[y*w+x]!=ref;
        }
    }
    return bad;
}
