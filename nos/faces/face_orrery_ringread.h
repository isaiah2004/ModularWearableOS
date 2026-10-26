/* The time rings' read-out: a white plate in the bottom sector (the year ring's discontinuity).
 * Two states, eased by S.platee (tick): at now, a compact plate with just the date; travelled,
 * the plate grows (up and out) and shows the offset under the date and a big NOW button on the
 * right that takes you home: easy to hit. Geometry is computed here from S (cheap: a few lerps);
 * text/dot masks and primitives replay through the stripe buckets. */
static void sector_label(gfx_fb *fb,int x,int y,const char *s,uint16_t ink)
{ ui_text(fb,UI_F_SMALL,x,y,s,ink,UI_CENTER,1); }
static void sector_digits(gfx_fb *fb,int x,int y,const char *s,uint16_t ink)
{
    dot_style st={DOT_FONT_5X7,2.5f,2.25f,ink,0,1,1};
    dot_text_a(fb,&st,(float)x,(float)y,s,1);
}
static float plate_mix(float a,float b) { return a+(b-a)*S.platee; }
#define NOWB_X 290                       /* the NOW button's centre, expanded plate */
#define NOWB_Y 392
#define NOWB_R 24
static int ring_sector_hit(int x,int y)
{
    if(fabs(S.off)<=0.02 || S.platee<0.5f)return 0;
    /* the whole right half of the expanded plate is the button */
    return x>=CX-6 && abs(x-NOWB_X)<=64 && abs(y-NOWB_Y)<=50;
}
static void draw_ring_sector(gfx_fb *fb)
{
    if(S.ringe<0.85f || S.lockk>0.5f)return;
    float e=S.platee;
    orr_cat(OP_TEXT);
    /* Static plate/DATE prefix and NOW suffix retain their exact command
     * order around the live date/offset. These optional caches use PSRAM. */
    int cacheable=orr_capture && (e==0 || e==1);
    unsigned key=gfx_hash_u(gfx_hash_u(gfx_hash_u(gfx_hash_f(0xDA7Eu,e),C_PRI),C_BG),C_RED);
    key=gfx_hash_u(gfx_hash_u(key,C_LINE2),S.ring_now_down);
    int n=cacheable?S.list->n:0,nt=cacheable?S.list->nt:0,na=cacheable?S.list->na:0;
    int dx=(int)plate_mix(CX,180);
    if(!cacheable || !commands_restore(S.plate_capture[0],key)) {
        /* compact (cy 402, r 43, caps 183..283) -> expanded (cy 392, r 52, caps 151..315) */
        gfx_fill_pill(fb,plate_mix(183,151),plate_mix(402,392),plate_mix(283,315),plate_mix(43,52),C_PRI);
        sector_label(fb,dx,(int)plate_mix(389,366),"DATE",C_BG);
        if(cacheable)commands_save(&S.plate_capture[0],key,n,nt,na);
    }
    sector_digits(fb,dx,(int)plate_mix(399,376),S.readout,C_BG);
    if(e>0.6f) sector_label(fb,dx,420,S.ring_state,ui_mix(C_PRI,C_BG,(e-0.6f)/0.4f));   /* [+99 D] fades in */
    n=cacheable?S.list->n:0;nt=cacheable?S.list->nt:0;na=cacheable?S.list->na:0;
    if(!cacheable || !commands_restore(S.plate_capture[1],key)) {
        if(e>0.5f) {                         /* the button grows in over the second half */
            float k=(e-0.5f)*2,r=NOWB_R*k,hw=(float)(30*k);
            gfx_fill_pill(fb,NOWB_X-hw,NOWB_Y,NOWB_X+hw,r,S.ring_now_down?C_LINE2:C_BG);
            if(k>0.7f) {
                int tw=ui_text_width(UI_F_BODY,"NOW",1);
                gfx_fill_circle(fb,NOWB_X-tw/2-9,NOWB_Y,3,C_RED);        /* the screen's one red element */
                ui_text(fb,UI_F_BODY,NOWB_X+5,NOWB_Y+7,"NOW",C_PRI,UI_CENTER,1);
            }
        }
        if(cacheable)commands_save(&S.plate_capture[1],key,n,nt,na);
    }
    orr_cat(OP_CONTROLS);
}
