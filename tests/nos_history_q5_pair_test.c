/* Paired Q5 arithmetic versus independent, separately extracted RGB channels. */
#include "../nos/render_history.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void *hal_big_alloc(uint32_t n){return malloc(n);}
static uint32_t random_state=41791;
static uint32_t random_word(void)
{
    random_state^=random_state<<13;
    random_state^=random_state>>17;
    random_state^=random_state<<5;
    return random_state;
}
static uint16_t reference(uint16_t c,uint16_t p,unsigned w)
{
    unsigned r=((32-w)*(c>>11)+w*(p>>11)+16)/32;
    unsigned g=((32-w)*((c>>5)&63)+w*((p>>5)&63)+16)/32;
    unsigned b=((32-w)*(c&31)+w*(p&31)+16)/32;
    return (uint16_t)(r<<11|g<<5|b);
}
static unsigned cases;
static void check(uint32_t c,uint32_t p,unsigned w)
{
    uint32_t expected=reference(c,p,w)|((uint32_t)reference(c>>16,p>>16,w)<<16);
    assert(expose_pair_q5(c,p,w)==expected);
    cases++;
}
int main(void)
{
    for(unsigned w=0;w<=32;w++) {
        for(unsigned ch=0;ch<6;ch++) {
            unsigned shift=(ch%3==0?0:ch%3==1?5:11)+16*(ch/3);
            unsigned max=ch%3==1?64:32,mask=(max-1)<<shift;
            /* Exhaust one channel with all neighbours independently zero/max
             * in current and previous: exercise lane boundaries and carries. */
            for(int fc=0;fc<2;fc++)for(int fp=0;fp<2;fp++)
                for(unsigned c=0;c<max;c++)for(unsigned p=0;p<max;p++)
                    check(((fc?~0u:0)&~mask)|(c<<shift),
                          ((fp?~0u:0)&~mask)|(p<<shift),w);
        }
        for(int i=0;i<100000;i++)check(random_word(),random_word(),w);
    }
    assert(cases==4922016);
    printf("PASS Q5 pair %u independent cases, all weights0..32 and channel boundaries\n",cases);
    return 0;
}
