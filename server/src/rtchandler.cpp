#include "rtchandler.h"
#include <Wire.h>
#include <RTClib.h>
#include <sys/time.h>

static RTC_DS3231 rtc;
static bool rtcPresent   = false;
static bool rtcValidTime = false;

// -------------------------------------------------------
// Called once in setup().
// Probes DS3231 on I2C (SDA=21, SCL=22).
// If found and battery has not died, loads time into ESP32 system clock.
// -------------------------------------------------------
void rtc_setup() {
    Wire.begin();   // SDA=GPIO21, SCL=GPIO22 (ESP32 hardware defaults)

    if (!rtc.begin()) {
        Serial.println("[RTC] DS3231 not found — running without hardware RTC");
        return;
    }
    rtcPresent = true;

    if (rtc.lostPower()) {
        // Battery ran out or first boot — time is garbage, don't use it
        Serial.println("[RTC] DS3231 found but lost power — time not valid, set via NTP or manually");
        return;
    }

    // DS3231 has a valid timestamp — push it into the ESP32 system clock
    DateTime now = rtc.now();
    struct timeval tv = { .tv_sec = (time_t)now.unixtime(), .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    rtcValidTime = true;

    Serial.printf("[RTC] Time loaded from DS3231: %04d-%02d-%02d %02d:%02d:%02d\n",
                  now.year(), now.month(), now.day(),
                  now.hour(), now.minute(), now.second());
}

// -------------------------------------------------------
bool rtc_isPresent()   { return rtcPresent; }
bool rtc_hasValidTime(){ return rtcPresent && rtcValidTime; }

// -------------------------------------------------------
// Write current ESP32 system time to DS3231.
// Call after NTP sync or after manual time set so RTC stays up-to-date
// and can survive the next power cycle without NTP.
// -------------------------------------------------------
void rtc_syncFromSystem() {
    if (!rtcPresent) return;

    time_t t = time(nullptr);
    if (t < 1000000UL) {
        Serial.println("[RTC] Sync skipped — system time not valid yet");
        return;
    }

    rtc.adjust(DateTime((uint32_t)t));
    rtcValidTime = true;
    Serial.println("[RTC] DS3231 updated from system time");
}
