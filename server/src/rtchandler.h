#pragma once
#include <Arduino.h>
#include <time.h>

// DS3231 RTC — I2C, SDA=GPIO21, SCL=GPIO22 (ESP32 defaults, no extra config needed)
// Хранит UTC. Какое время попадает в системные часы, решает timesync.cpp.
//
//   rtc_setup()          — probe DS3231 (+ однократная миграция local → UTC)
//   rtc_read(t)          — прочитать UTC; false если модуля нет, батарейка садилась
//                          или чтение выглядит как мусор
//   rtc_syncFromSystem() — записать системное время в DS3231 (после NTP / ручной установки)

void rtc_setup();
bool rtc_read(time_t& out);
bool rtc_isPresent();
bool rtc_hasValidTime();
void rtc_syncFromSystem();
