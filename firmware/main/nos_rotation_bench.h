#ifndef NOS_ROTATION_BENCH_H
#define NOS_ROTATION_BENCH_H
/* Caller owns the already-fenced raster/DMA scratch, and invalidates afterwards.
 * Both modes see identical SRAM pixels/geometry; hashing is outside the timer. */
static void nos_rotation_bench_run(uint16_t *raw,uint16_t *out) {
    const int widths[]={466,320,232,231}, offsets[]={0,0,2,2}, order[]={1,2,2,1};
    int saved=board_pixels_rotation_mode_get();
    for(unsigned i=0;i<GFX_W*NOS_STRIPE_ROWS;i++)raw[i]=(uint16_t)((i*2654435761u)^(i>>3));
    for(unsigned c=0;c<4;c++) {
        int width=widths[c],offset=offsets[c];uint32_t expected=2166136261u;
        for(int x=width-1;x>=0;x--)for(int y=0;y<NOS_DMA_ROWS;y++) {
            uint16_t p=raw[y*GFX_W+x+offset];p=(uint16_t)((p>>8)|(p<<8));
            expected=(expected^p)*16777619u;
        }
        for(unsigned arm=0;arm<4;arm++) {
            board_pixels_rotation_mode_set(order[arm]);
            board_pixels_ccw_swap(raw+offset,GFX_W,out,width,NOS_DMA_ROWS,1);
            uint32_t start=hal_micros();
            for(unsigned rep=0;rep<128;rep++)board_pixels_ccw_swap(raw+offset,GFX_W,out,width,NOS_DMA_ROWS,1);
            uint32_t elapsed=hal_micros()-start,actual=2166136261u;
            for(int i=0;i<width*NOS_DMA_ROWS;i++)actual=(actual^out[i])*16777619u;
            printf("NOSROTBENCH v=1 case=%u arm=%u mode=%d width=%d height=%d source_x=%d repeats=128 pixels=%u elapsed_us=%u checksum=%08x mismatches=%u\n",
                c,arm,order[arm],width,NOS_DMA_ROWS,offset,(unsigned)(width*NOS_DMA_ROWS*128),(unsigned)elapsed,(unsigned)actual,actual!=expected);
        }
    }
    board_pixels_rotation_mode_set(saved);nos_invalidate();
}
#endif
