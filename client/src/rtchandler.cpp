#include "rtchandler.h"
#include <Wire.h>
#include <RTClib.h>

static RTC_DS3231 rtc;
static bool rtcPresent   = false;
static bool rtcValidTime = false;

// -------------------------------------------------------
// Called once in setup(). Probes DS3231 on I2C (SDA=21, SCL=22).
// A dead/missing RTC is not fatal — the client just falls back to the
// heartbeat-derived virtual clock, same as before this file existed.
// -------------------------------------------------------
void rtc_setup() {
    Wire.begin();   // SDA=GPIO21, SCL=GPIO22 (ESP32 hardware defaults)

    if (!rtc.begin()) {
        Serial.println("[RTC] DS3231 not found — falling back to heartbeat virtual clock");
        return;
    }
    rtcPresent = true;

    if (rtc.lostPower()) {
        // Battery ran out or first boot — time is garbage until the next heartbeat.
        rtcValidTime = false;
        Serial.println("[RTC] DS3231 found but lost power — waiting for a heartbeat to set it");
        return;
    }

    rtcValidTime = true;
    DateTime now = rtc.now();
    Serial.printf("[RTC] DS3231 time-of-day at boot: %02d:%02d:%02d\n",
                  now.hour(), now.minute(), now.second());
}

bool rtc_isPresent()    { return rtcPresent; }
bool rtc_hasValidTime() { return rtcPresent && rtcValidTime; }

// -------------------------------------------------------
// Overwrite only the time-of-day, keeping whatever date is currently stored
// — the client never needs a real calendar date (see rtchandler.h). Called
// on every heartbeat while the server is reachable, so the RTC tracks the
// server's authoritative clock and free-runs accurately on its own crystal
// once contact is lost.
// -------------------------------------------------------
void rtc_setTimeOfDay(uint8_t hh, uint8_t mm, uint8_t ss) {
    if (!rtcPresent) return;

    // First-ever set (lostPower / never valid) has no usable date — pin it to
    // an arbitrary fixed date. Its value is never read back by anyone.
    int y = 2024, mo = 1, d = 1;
    if (rtcValidTime) {
        DateTime cur = rtc.now();
        y = cur.year(); mo = cur.month(); d = cur.day();
    }

    rtc.adjust(DateTime(y, mo, d, hh, mm, ss));
    rtcValidTime = true;
}

int32_t rtc_getTimeOfDaySec() {
    if (!rtcPresent || !rtcValidTime) return -1;
    DateTime now = rtc.now();
    return (int32_t)now.hour() * 3600 + (int32_t)now.minute() * 60 + (int32_t)now.second();
}
