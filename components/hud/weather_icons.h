#pragma once
#include <cstdint>
#include <cstring>

struct WeatherIcon { uint8_t rows[8]; };

static const WeatherIcon ICON_SUNNY  = {{ 0x24, 0x18, 0x7E, 0xFF, 0xFF, 0x7E, 0x18, 0x24 }};
static const WeatherIcon ICON_CLOUDY = {{ 0x00, 0x3C, 0x7E, 0xFF, 0xFF, 0x7E, 0x00, 0x00 }};
static const WeatherIcon ICON_RAINY  = {{ 0x3C, 0x7E, 0xFF, 0xFF, 0x4A, 0x25, 0x4A, 0x00 }};
static const WeatherIcon ICON_SNOWY  = {{ 0x24, 0x18, 0xFF, 0x7E, 0x7E, 0xFF, 0x18, 0x24 }};
static const WeatherIcon ICON_UNKNWN = {{ 0x3C, 0x42, 0x06, 0x0C, 0x08, 0x00, 0x08, 0x00 }};

inline const WeatherIcon& icon_for_condition(const char* s) {
    if (!s) return ICON_UNKNWN;
    if (strstr(s, "sunny")   || strstr(s, "clear"))    return ICON_SUNNY;
    if (strstr(s, "cloud")   || strstr(s, "overcast")) return ICON_CLOUDY;
    if (strstr(s, "rain")    || strstr(s, "drizzle"))  return ICON_RAINY;
    if (strstr(s, "snow")    || strstr(s, "sleet"))    return ICON_SNOWY;
    return ICON_UNKNWN;
}
