#include "rtchandler.h"
#include "config.h"
#include <Wire.h>
#include <RTClib.h>
#include <Preferences.h>

static RTC_DS3231 rtc;
static bool rtcPresent   = false;
static bool rtcValidTime = false;

// До версии 6.1 DS3231 хранил ЛОКАЛЬНОЕ время, а система жила без TZ.
// Теперь в DS3231 лежит UTC (как отдаёт NTP), а локальное время считает TZ.
// Однократный перевод при первой загрузке новой прошивки — иначе часы
// уехали бы на смещение пояса.
static void migrateToUtc() {
    Preferences p;
    p.begin("gongtime", false);
    if (!p.getBool("rtc_utc", false)) {
        if (!rtc.lostPower()) {
            DateTime d = rtc.now();
            if (d.isValid() && d.year() >= 2024 && d.year() <= 2099) {
                struct tm lt = {};
                lt.tm_year  = d.year() - 1900;
                lt.tm_mon   = d.month() - 1;
                lt.tm_mday  = d.day();
                lt.tm_hour  = d.hour();
                lt.tm_min   = d.minute();
                lt.tm_sec   = d.second();
                lt.tm_isdst = -1;
                rtc.adjust(DateTime((uint32_t)mktime(&lt)));
                logPrintf("[RTC] Migrated DS3231 from local time to UTC\n");
            }
        }
        p.putBool("rtc_utc", true);
    }
    p.end();
}

void rtc_setup() {
    Wire.begin();   // SDA=GPIO21, SCL=GPIO22 (ESP32 hardware defaults)

    if (!rtc.begin()) {
        logPrintf("[RTC] DS3231 not found — running without hardware RTC\n");
        return;
    }
    rtcPresent = true;
    migrateToUtc();

    rtcValidTime = !rtc.lostPower();
    if (!rtcValidTime)
        logPrintf("[RTC] DS3231 found but lost power — time not valid until NTP or manual set\n");
}

// Сбой на I2C даёт мусор вроде 2165-165-165 — раньше он уходил прямо в
// системные часы, проходил проверку tm_year >= 124, и гонги звонили в
// случайное время. Теперь: разумный год и два чтения подряд согласованы.
bool rtc_read(time_t& out) {
    if (!rtcPresent) return false;
    if (rtc.lostPower()) { rtcValidTime = false; return false; }

    DateTime a = rtc.now();
    DateTime b = rtc.now();
    int32_t  d = (int32_t)(b.unixtime() - a.unixtime());
    if (!a.isValid() || a.year() < 2024 || a.year() > 2099 || d < 0 || d > 2) {
        logPrintf("[RTC] Implausible DS3231 read (%04d-%02d-%02d, delta %ds) — ignored\n",
                  a.year(), a.month(), a.day(), (int)d);
        return false;
    }
    rtcValidTime = true;
    out = (time_t)b.unixtime();
    return true;
}

bool rtc_isPresent()    { return rtcPresent; }
bool rtc_hasValidTime() { return rtcPresent && rtcValidTime; }

void rtc_syncFromSystem() {
    if (!rtcPresent) return;
    if (!timeIsSet()) {
        logPrintf("[RTC] Sync skipped — system time not valid yet\n");
        return;
    }
    rtc.adjust(DateTime((uint32_t)time(nullptr)));
    rtcValidTime = true;
    logPrintf("[RTC] DS3231 updated from system time\n");
}
