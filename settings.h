#ifndef SETTINGS_H
#define SETTINGS_H

// ---------------------------------------------------------------------------
// SET THESE BEFORE FLASHING.
//
// This watchface does not draw weather, but the Watchy library still needs the
// struct below, and it uses the weather response to correct the clock's GMT
// offset.  Leaving a city that is not yours can therefore drift the time by an
// hour, so set CITY_ID and GMT_OFFSET_SEC even though nothing is displayed.
// ---------------------------------------------------------------------------

// Your city's ID from https://openweathermap.org/current#cityid
// (5128581 = New York, the Watchy library's own default.)
#define CITY_ID "5128581"

// Free API key from https://openweathermap.org/api -- only needed if you add a
// weather element; the face works fine with this left as-is.
#define OPENWEATHERMAP_APIKEY "your_api_key_here"

#define OPENWEATHERMAP_URL "http://api.openweathermap.org/data/2.5/weather?id="
#define TEMP_UNIT "metric"  // metric = Celsius, imperial = Fahrenheit
#define TEMP_LANG "en"
#define WEATHER_UPDATE_INTERVAL 30  // minutes, must be greater than 5

#define NTP_SERVER "pool.ntp.org"

// Your UTC offset in seconds.  Examples: UTC+0 -> 3600 * 0,
// UTC+1 -> 3600 * 1, UTC-5 -> 3600 * -5.
#define GMT_OFFSET_SEC 3600 * 0

// Daylight saving. Set to 3600 * 1 while summer time is in effect, else 0.
#define DST_OFFSET_SEC 3600 * 0

watchySettings settings{
    .cityID = CITY_ID,
    .weatherAPIKey = OPENWEATHERMAP_APIKEY,
    .weatherURL = OPENWEATHERMAP_URL,
    .weatherUnit = TEMP_UNIT,
    .weatherLang = TEMP_LANG,
    .weatherUpdateInterval = WEATHER_UPDATE_INTERVAL,
    .ntpServer = NTP_SERVER,
    .gmtOffset = GMT_OFFSET_SEC + DST_OFFSET_SEC,
};

#endif
