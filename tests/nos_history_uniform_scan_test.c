/* Independent scalar oracle for the internal solid-current scan. */
#include "../nos/render_history.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void *hal_big_alloc(uint32_t n){return malloc(n);}
static uint16_t pixels[GFX_W+4];
static unsigned random_state=0x743e;
static unsigned random_word(void){random_state=random_state*1664525u+1013904223u;return random_state;}
static unsigned reference(const uint16_t *p,int n)
{
    uint16_t c=p[0];
    if(c && c!=0xffffu)return 0;
    for(int i=1;i<n;i++)if(p[i]!=c)return 0;
    return c?2u:1u;
}
int main(void)
{
    /* The target-rejected cadence experiment must remain opt-in. */
    assert(nos_history_cadence==0);
    nos_history_begin_interval(1,25000);
    assert(nos_history_previous_weight_q5()==8);
    assert(nos_history_interval_us()==25000);
    unsigned cases=0;
    for(int off=0;off<2;off++)for(int white=0;white<2;white++)
        for(int n=1;n<=GFX_W;n++) {
            uint16_t *p=pixels+off,c=white?0xffffu:0;
            for(unsigned i=0;i<sizeof pixels/sizeof pixels[0];i++)pixels[i]=c;
            assert(uniform_current(p,n)==reference(p,n));cases++;
            /* Every defect position covers both reduction groups and tails. */
            for(int j=0;j<n;j++) {
                p[j]=(uint16_t)(c^(1u<<(j%16)));
                assert(uniform_current(p,n)==reference(p,n));cases++;
                p[j]=c;
            }
        }
    for(int t=0;t<20000;t++) {
        int off=(int)(random_word()&1u),n=1+(int)(random_word()%GFX_W);
        uint16_t *p=pixels+off;
        for(int i=0;i<n;i++)p[i]=(uint16_t)(random_word()%4?0:random_word());
        assert(uniform_current(p,n)==reference(p,n));cases++;
    }
    printf("PASS uniform scan %u independent scalar cases: all lengths, alignments, solid/mixed and defect positions\n",cases);
    return 0;
}
