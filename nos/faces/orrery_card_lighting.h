/* Optional exact original lighting coefficients, owned by card buffers. */
#ifndef ORRERY_CARD_LIGHTING_H
#define ORRERY_CARD_LIGHTING_H
typedef struct { float k[512]; } card_lighting;
static unsigned card_lighting_mismatches(const card_lighting *table,const uint8_t *lighting)
{
    unsigned mismatches=0;
    for(int i=0;i<256;i++) {
        float planet=.05f + .95f * lighting[i] / 255.f;
        float sun=.55f + .45f * lighting[256+i] / 255.f;
        uint32_t a,b;memcpy(&a,&planet,4);memcpy(&b,&table->k[i],4);mismatches+=a!=b;
        memcpy(&a,&sun,4);memcpy(&b,&table->k[256+i],4);mismatches+=a!=b;
    }
    return mismatches;
}
static card_lighting *card_lighting_new(const uint8_t *lighting)
{
    if(!lighting)return 0;
#ifdef NOS_PSRAM_TRACKING
    if(nos_mem_available()<sizeof(card_lighting)+16384u)return 0;
#endif
    card_lighting *table=hal_big_alloc(sizeof *table);
    if(!table)return 0;
    for(int i=0;i<256;i++) {
        table->k[i]=.05f + .95f * lighting[i] / 255.f;
        table->k[256+i]=.55f + .45f * lighting[256+i] / 255.f;
    }
    /* Complete table is published by the caller only after bitwise validation. */
    if(card_lighting_mismatches(table,lighting)) {hal_big_free(table);return 0;}
    return table;
}
#endif
