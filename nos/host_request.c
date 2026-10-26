#include "host_request.h"
#include <string.h>
int host_request_parse(const char *text,char id[9],const char **command)
{
    id[0]=0;*command=text;
    if(strncmp(text,"req",3) || (text[3] && text[3]!=' ' && text[3]!='\t')) return 0;
    size_t length=strlen(text);
    if(length<14 || length>191 || text[3]!=' ' || text[12]!=' ') return -1;
    for(int i=0;i<8;i++) {
        unsigned char c=(unsigned char)text[4+i];
        if(!((c>='0'&&c<='9') || (c>='a'&&c<='f'))) return -1;
    }
    for(size_t i=0;i<length;i++) {
        unsigned char c=(unsigned char)text[i];if(c<32 || c==127) return -1;
    }
    if(text[13]==' ') return -1;
    memcpy(id,text+4,8);id[8]=0;*command=text+13;return 1;
}
int host_request_transfer(const char *command)
{
    static const char *const verbs[]={"sdfeatures","sdls","sdget","sdget2","sdput","sdput!","sdcrc","sdrm"};
    size_t n=strcspn(command," \t");
    for(unsigned i=0;i<sizeof verbs/sizeof verbs[0];i++)
        if(strlen(verbs[i])==n && !memcmp(command,verbs[i],n)) return 1;
    return 0;
}
