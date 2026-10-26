#include "../firmware/main/setup_protocol.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    assert(setup_credentials_valid("Home + Guest","space & + % \" password"));
    assert(setup_credentials_valid("Open network",""));
    assert(!setup_credentials_valid("","password"));
    assert(!setup_credentials_valid("Home","1234567"));
    char ssid[34],pass[65];
    memset(ssid,'x',32);ssid[32]=0;
    memset(pass,'y',63);pass[63]=0;
    assert(setup_credentials_valid(ssid,pass));
    ssid[32]='x';ssid[33]=0;assert(!setup_credentials_valid(ssid,pass));
    ssid[32]=0;pass[63]='y';pass[64]=0;assert(!setup_credentials_valid(ssid,pass));
    puts("PASS Wi-Fi credential boundaries, open networks, spaces and symbols");
    return 0;
}
