#ifndef WATCH_SETUP_PROTOCOL_H
#define WATCH_SETUP_PROTOCOL_H
#include <stddef.h>
#include <string.h>
static inline int setup_credentials_valid(const char *ssid,const char *pass)
{
    if(!ssid || !pass) return 0;
    size_t s=strlen(ssid),p=strlen(pass);
    return s>0 && s<=32 && (p==0 || (p>=8 && p<=63));
}
#endif
