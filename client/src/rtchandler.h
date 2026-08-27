#pragma once
#include <Arduino.h>

// DS3231 RTC — I2C, SDA=GPIO21, SCL=GPIO22 (ESP32 defaults, no extra config
// needed). Optional on the client: without it, autonomous mode still works
// exactly as before (see syncVirtualClock() in lorahandler.cpp), just tied to
// millis() since the last heartbeat and reset on every reboot.
//
// With it: the client keeps its own battery-backed time-of-day, refreshed
// from every heartbeat while the server is alive, so the autonomous fallback
// schedule (H-5) can resume correctly even if the CLIENT itself reboots while
// the server is already silent — a case the millis()-only virtual clock can
// never recover from (see 01_AUDIT_REPORT.md H-5 / lora-ds-autonomy branch).
//
// The client has no concept of calendar date (its schedule format is
// hour/minute only — see SchedBin in lora_shared.h), so only the
// time-of-day component of the RTC is ever read or written; whatever date it
// holds is irrelevant and never inspected.
//
// Usage:
//   rtc_setup()                     — call once in setup(), before lora_setup()
//   rtc_isPresent()                 — true if DS3231 was found on I2C bus
//   rtc_hasValidTime()              — true if DS3231 has not lost power since last set
//   rtc_setTimeOfDay(hh, mm, ss)    — write time-of-day (keeps whatever date is stored)
//   rtc_getTimeOfDaySec()           — seconds since midnight, or -1 if not present/valid

void    rtc_setup();
bool    rtc_isPresent();
bool    rtc_hasValidTime();
void    rtc_setTimeOfDay(uint8_t hh, uint8_t mm, uint8_t ss);
int32_t rtc_getTimeOfDaySec();
