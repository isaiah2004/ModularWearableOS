#ifndef WATCH_WEATHER_PARSE_H
#define WATCH_WEATHER_PARSE_H
#include "weather.h"
/* All-or-nothing parse: malformed responses leave the previous value alone. */
int weather_parse(const char *json, hal_weather *out);
#endif
