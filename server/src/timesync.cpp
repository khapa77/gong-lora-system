#include "timesync.h"
#include "config.h"
#include "rtchandler.h"
#include <esp_sntp.h>
#include <sys/time.h>

enum class TimeSrc : uint8_t { NONE, RTC, NTP, MANUAL };
static TimeSrc src = TimeSrc::NONE;   // кто последним выставил системные часы

// Колбэк SNTP вызывается из задачи lwIP — там только флаг. Запись в DS3231
// (I2C) и логирование делает time_loop() на loopTask, которому принадлежит Wire.
static volatile bool ntpSyncPending = false;
static bool          ntpEverSynced  = false;
static uint32_t      lastNtpMs      = 0;
static uint32_t      lastRtcCheckMs = 0;

static void onNtpSync(struct timeval*) { ntpSyncPending = true; }

static void applyFromRtc(bool atBoot) {
    time_t rt;
    if (!rtc_read(rt)) return;
    time_t sys = time(nullptr);
    long drift = (long)(rt - sys);
    struct timeval tv = { .tv_sec = rt, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    src = TimeSrc::RTC;
    if (atBoot || drift > 1 || drift < -1) {
        struct tm lt;
        localtime_r(&rt, &lt);
        logPrintf("[TIME] From DS3231: %04d-%02d-%02d %02d:%02d:%02d%s\n",
                  lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                  lt.tm_hour, lt.tm_min, lt.tm_sec,
                  atBoot ? "" : " (drift corrected)");
    }
}

void time_setup() {
    // TZ — до rtc_setup(): миграция DS3231 на UTC переводит его старое
    // локальное время через mktime(), которому нужен часовой пояс.
    setenv("TZ", TIME_TZ, 1);
    tzset();
    sntp_set_time_sync_notification_cb(onNtpSync);

    rtc_setup();
    applyFromRtc(true);
    if (src == TimeSrc::NONE)
        logPrintf("[TIME] No valid time — waiting for NTP (WiFi) or manual set in the web UI\n");
    lastRtcCheckMs = millis();
}

void time_startNtp() {
    // По умолчанию SNTP в этом ядре опрашивает раз в 3 часа — чаще, чтобы
    // пропажа интернета замечалась через NTP_STALE_MS, а не через полдня.
    sntp_set_sync_interval(NTP_SYNC_INTERVAL_MS);
    configTzTime(TIME_TZ, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
    logPrintf("[TIME] NTP started (%s, %s, %s)\n", NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
}

void time_loop() {
    uint32_t now = millis();

    if (ntpSyncPending) {
        ntpSyncPending = false;
        if (timeIsSet()) {
            ntpEverSynced = true;
            lastNtpMs     = now;
            src           = TimeSrc::NTP;
            rtc_syncFromSystem();   // DS3231 держит NTP-время на случай пропажи интернета
            struct tm lt;
            localNow(lt);
            logPrintf("[TIME] NTP sync: %04d-%02d-%02d %02d:%02d:%02d\n",
                      lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                      lt.tm_hour, lt.tm_min, lt.tm_sec);
        }
    }

    if (now - lastRtcCheckMs >= RTC_RELOAD_MS) {
        lastRtcCheckMs = now;
        bool ntpFresh = ntpEverSynced && now - lastNtpMs < NTP_STALE_MS;
        if (!ntpFresh) applyFromRtc(false);
    }
}

bool time_setManual(int year, int month, int day, int hour, int min) {
    struct tm ti = {};
    ti.tm_year  = year - 1900;
    ti.tm_mon   = month - 1;
    ti.tm_mday  = day;
    ti.tm_hour  = hour;
    ti.tm_min   = min;
    ti.tm_isdst = -1;   // пусть TZ решает сам
    time_t t = mktime(&ti);
    if (t <= (time_t)TIME_VALID_EPOCH) return false;
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    src = TimeSrc::MANUAL;
    rtc_syncFromSystem();   // переживёт перезагрузку
    return true;
}

const char* time_sourceName() {
    if (!timeIsSet()) return "none";
    switch (src) {
        case TimeSrc::NTP:    return "ntp";
        case TimeSrc::RTC:    return "rtc";
        case TimeSrc::MANUAL: return "manual";
        default:              return "none";
    }
}

int32_t time_ntpAgeSec() {
    if (!ntpEverSynced) return -1;
    return (int32_t)((millis() - lastNtpMs) / 1000);
}
