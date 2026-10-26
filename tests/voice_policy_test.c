#include "voice_chat.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    assert(!voice_profile_get(-1) && !voice_profile_get(3));
    const unsigned targets[]={1,5,10};
    for(int i=0;i<3;i++){
        const voice_profile *p=voice_profile_get(i);assert(p->cents_per_minute==targets[i]);
        double reserve=voice_reserve_usd(i,p->history_bytes+384,20000);
        assert(reserve>0 && reserve<p->cents_per_minute*.01);
        assert(voice_budget_allows(i,0,reserve,0));
        assert(!voice_budget_allows(i,p->cents_per_minute*.01,reserve,0));
        assert(!voice_budget_allows(i,NAN,reserve,1000));
        assert(!voice_budget_allows(i,0,-1,1000));
        assert(voice_budget_allows(i,p->cents_per_minute*.01,reserve,120000));
    }
    assert(isinf(voice_reserve_usd(4,0,0)));
    char out[8];assert(voice_text_copy(out,4,"a\xe2\x82\xacz")==1 && !strcmp(out,"a"));
    assert(voice_text_copy(out,5,"a\xe2\x82\xacz")==4 && strlen(out)==4);
    assert(voice_text_copy(out,sizeof out,"hello")==5 && !strcmp(out,"hello"));
    assert(!voice_text_copy(out,1,"hi") && !out[0]);
    puts("PASS voice budgets, invalid values and UTF-8 boundaries");return 0;
}
