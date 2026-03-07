#pragma once
#include <Arduino.h>

// DS3231 RTC — I2C, SDA=GPIO21, SCL=GPIO22 (ESP32 defaults, no extra config needed)
// Provides battery-backed timekeeping independent of WiFi/NTP.
//
// Priority:  NTP (most accurate) > RTC (battery-backed) > Manual > None
//
// Usage:
//   rtc_setup()          — call in setup() before web_setup(); reads RTC → system clock
//   rtc_isPresent()      — true if DS3231 was found on I2C bus
//   rtc_hasValidTime()   — true if DS3231 has not lost power since last set
//   rtc_syncFromSystem() — write current system time to DS3231 (call after NTP sync or manual set)

void rtc_setup();
bool rtc_isPresent();
bool rtc_hasValidTime();
void rtc_syncFromSystem();
