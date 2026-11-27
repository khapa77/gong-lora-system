// Host tests for the stage-4 emergency module and its scheduler integration.
// The production translation units are compiled as-is; only the Arduino,
// LittleFS, Preferences and time layers are replaced by host fakes.
#include "host/Arduino.h"
#include "host/LittleFS.h"
#include "host/Preferences.h"
#include <ArduinoJson.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

extern "C" { bool gong_allocation_forbidden = false; }

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
#define HOST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define HOST_SANITIZED 1
#endif
#endif
#ifndef HOST_SANITIZED
void* operator new(size_t n) {
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
#endif

void convertToJson(const String& src, ArduinoJson::JsonVariant dst) {
    dst.set(src.c_str());
}

#define time host_time
#include "../server/src/schedule.cpp"
#undef time

#include "emergency.h"
#include "course_store.h"
#include "course_model.h"

// ---- audio / reboot capture ------------------------------------------------
static int audioCalls = 0;
static int lastTrack = 0;
static int lastPlays = 0;
static int lastVolume = 0;
static int rebootCalls = 0;
static bool audioPlaying = false;

static bool audioCallback(uint8_t track, uint8_t plays, uint8_t volume, uint32_t*) {
    ++audioCalls;
    lastTrack = track;
    lastPlays = plays;
    lastVolume = volume;
    return true;
}

static void rebootCallback() { ++rebootCalls; }

void logPrintf(const char*, ...) {}
bool mp3_isPlaying() { return audioPlaying; }
void mp3_stop() { audioPlaying = false; }
// schedule.cpp routes "stop everything" through relay_stop() (it also cancels a
// pending warm-up); the relay itself is exercised by run_relay_tests.py.
void relay_stop() { audioPlaying = false; }
// Schedule asks the relay whether a queued gong actually started.
uint32_t relay_pendingTag() { return 0; }
uint32_t relay_lastStartedTag() { return 0; }
bool mp3_trackExists(uint8_t) { return true; }
bool sys_fsOk() { return true; }
void sys_lock() {}
void sys_unlock() {}

static int checks = 0;
static int failures = 0;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::cout << "  FAIL: " << what << "\n";
    }
}

static time_t atDate(int y, int mo, int d, int h = 3, int mi = 0) {
    struct tm t;
    std::memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_isdst = 0;
    return mktime(&t);
}

static bool hostNow(struct tm& out) {
    const time_t now = host::clock();
    const struct tm* local = localtime(&now);
    if (!local) return false;
    out = *local;
    return true;
}

static void resetRuntime() {
    for (int i = 0; i < MAX_SCHEDULES; ++i) entries[i] = ScheduleEntry();
    count = 0;
    nextId = 1;
    lastFiredKey = -1;
    lastFiredMillis = 0;
    lastTimeLog = 0;
    activeDay = -1;
    activeDate = "";
    activeStore = active_day::Store(DAY_COUNT);
    executionRunning = false;
    publishedActive = 0;
    scheduleFileState = ScheduleFileState::Unprepared;
    loadedRecord = active_day::Record();
    loadedSource = active_day::Source::Error;
    schedPendingSave = false;
    firePrefs = Preferences();
    lastFireTs = 0;
    courseMode = false;
    courseRunning = false;
    courseSuspended = false;
    courseDayValid = false;
    courseDay = 0;
    courseLength = 0;
    courseGeneration = 0;
    courseCal = course_store::CalendarState();
    courseLastFire = course_store::LastFire();
    courseLastFireValid = false;
    courseTransitionPending = false;
    courseStore = course_store::CourseStore();
    emergency::resetRuntime();
    emergency::onLocalNow = hostNow;
    emergency::onRecoveryCheck = nullptr;
    emergency::onReboot = rebootCallback;
    onScheduleTrigger = audioCallback;
    audioCalls = 0;
    audioPlaying = false;
    rebootCalls = 0;
}

static void freshEnvironment() {
    setenv("TZ", "UTC", 1);
    tzset();
    host::nvs() = host::NvsModel();
    host::files().clear();
    host::fs() = host::FsFaults();
    resetRuntime();
}

static course::Course makeCourse(int days, int hour = 4, int minute = 0) {
    course::Course c;
    c.name = "test-course";
    c.generation = 1;
    for (int i = 0; i < days; ++i) {
        course::Day d;
        d.hasOwnEvents = true;
        course::Event e;
        e.id = 1;
        e.hour = (uint8_t)hour;
        e.minute = (uint8_t)minute;
        e.track = 1;
        e.loop = 1;
        e.volume = 10;
        e.enabled = true;
        e.description = "day" + std::to_string(i);
        d.events.push_back(e);
        c.days.push_back(d);
    }
    return c;
}

static bool applyCourse(const course::Course& c) {
    course_store::CourseStore store;
    uint32_t gen = 0;
    course_store::CalendarState st;
    std::string err;
    return store.applyCourse(c, gen, st, err);
}

static uint32_t activeGeneration() {
    course_store::CourseStore store;
    course_store::CalendarState st;
    std::string err;
    if (!store.loadCalendar(st, err)) return 0;
    return st.active_generation;
}

static std::string dayPath(uint32_t gen, int day) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/course/g%u/d%02d.json", (unsigned)gen, day);
    return std::string(buf);
}

// ---- tests -----------------------------------------------------------------

static void test_table() {
    size_t count = 0;
    const emergency::Entry* t = emergency::table(count);
    check(count == 4, "four emergency entries");
    check(t[0].hour == 4 && t[0].minute == 0 && t[0].plays == 1 && t[0].volume == 30,
          "04:00 standard gong");
    check(t[1].hour == 4 && t[1].minute == 20 && t[1].plays == 1 && t[1].volume == 30,
          "04:20 standard gong");
    check(t[2].hour == 6 && t[2].minute == 30 && t[2].plays == 1 && t[2].volume == 30,
          "06:30 standard gong");
    check(t[3].hour == 6 && t[3].minute == 31 && t[3].plays == 10 && t[3].volume == 5,
          "06:31 quiet gong, 10 plays, volume 5");
}

static void test_fires_0400() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 4, 0);
    check(emergency::enter(emergency::kBootFailed), "enter emergency");
    check(emergency::tick(), "tick fires 04:00");
    check(audioCalls == 1, "one emergency call");
    check(lastTrack == 1 && lastPlays == 1 && lastVolume == 30, "standard gong params");
}

static void test_fires_0631() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 6, 31);
    check(emergency::enter(emergency::kBootFailed), "enter emergency");
    check(emergency::tick(), "tick fires 06:31");
    check(audioCalls == 1, "one call");
    check(lastPlays == 10, "exactly ten plays");
    check(lastVolume == 5, "volume 5 of 30");
}

static void test_no_duplicate() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 4, 0);
    emergency::enter(emergency::kBootFailed);
    check(emergency::tick(), "first tick fires");
    check(!emergency::tick(), "second tick does not fire");
    check(audioCalls == 1, "still one call");
}

static void test_without_json() {
    freshEnvironment();
    host::files().clear();
    host::clock() = atDate(2026, 11, 22, 6, 31);
    emergency::enter(emergency::kBootFailed);
    check(emergency::tick(), "fires without course files");
    check(audioCalls == 1, "one call without JSON");
}

static void test_counter_persists() {
    freshEnvironment();
    emergency::recordFailure(emergency::kMidnightFailed);
    check(emergency::attempts() == 1, "attempts 1");
    check(emergency::rebootPending(), "reboot armed");
    // Simulated reboot: RAM state is lost, NVS survives.
    emergency::resetRuntime();
    emergency::onReboot = rebootCallback;
    emergency::onLocalNow = hostNow;
    emergency::begin();
    check(emergency::attempts() == 1, "attempts survive reboot");
    check(!emergency::isActive(), "not active after one failure");
}

static void test_third_failure_enters() {
    freshEnvironment();
    emergency::recordFailure(emergency::kMidnightFailed);
    emergency::recordFailure(emergency::kMidnightFailed);
    check(!emergency::isActive(), "not active after two failures");
    check(emergency::rebootPending(), "reboot armed after two failures");
    emergency::recordFailure(emergency::kMidnightFailed);
    check(emergency::isActive(), "active after third failure");
    check(!emergency::rebootPending(), "no reboot after third failure");
}

static void test_no_reboot_in_emergency() {
    freshEnvironment();
    emergency::enter(emergency::kBootFailed);
    emergency::recordFailure(emergency::kMidnightFailed);
    check(emergency::isActive(), "still active");
    check(!emergency::rebootPending(), "no reboot while active");
}

static void test_reboot_after_10s() {
    freshEnvironment();
    host::ticks() = 100000;
    emergency::recordFailure(emergency::kMidnightFailed);
    host::ticks() = 109999;
    emergency::tickReboot();
    check(rebootCalls == 0, "no reboot before 10 s");
    host::ticks() = 110000;
    emergency::tickReboot();
    check(rebootCalls == 1, "reboot at 10 s");
}

static void test_persists_across_reboot() {
    freshEnvironment();
    emergency::recordFailure(emergency::kMidnightFailed);
    emergency::recordFailure(emergency::kMidnightFailed);
    emergency::recordFailure(emergency::kMidnightFailed);
    check(emergency::isActive(), "active before reboot");
    emergency::resetRuntime();
    emergency::onReboot = rebootCallback;
    emergency::onLocalNow = hostNow;
    emergency::begin();
    check(emergency::isActive(), "active after reboot");
    check(emergency::reason() == emergency::kMidnightFailed, "reason survives reboot");
}

static void test_recovery() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 5, 0);
    emergency::enter(emergency::kBootFailed);
    check(emergency::isActive(), "active");
    check(!emergency::checkRecovery(), "recovery without a hook fails");
    emergency::onRecoveryCheck = []() { return true; };
    check(emergency::checkRecovery(), "recovery hook succeeds");
    check(!emergency::isActive(), "left emergency");
}

static void test_manual_activate_failure() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 3, 0);
    check(applyCourse(makeCourse(14)), "apply course");
    uint32_t gen = activeGeneration();
    check(gen != 0, "generation saved");
    sched_setup();
    host::files().erase(dayPath(gen, 0));
    check(!sched_activateDay(0), "activation fails without a day file");
    check(emergency::attempts() == 1, "failure counted");
    check(emergency::rebootPending(), "reboot armed");
}

static void test_midnight_failure() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 23, 50);
    check(applyCourse(makeCourse(14)), "apply course");
    uint32_t gen = activeGeneration();
    sched_setup();
    check(sched_activateDay(0), "activation succeeds");
    host::files().erase(dayPath(gen, 1));
    host::clock() = atDate(2026, 11, 23, 0, 1);
    sched_check();
    check(emergency::attempts() == 1, "midnight failure counted");
    check(emergency::rebootPending(), "reboot armed");
}

static void test_auto_recovery() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 23, 50);
    check(applyCourse(makeCourse(14)), "apply course");
    uint32_t gen = activeGeneration();
    sched_setup();
    check(sched_activateDay(0), "activation succeeds");
    std::string saved = host::files()[dayPath(gen, 1)];
    host::files().erase(dayPath(gen, 1));
    host::clock() = atDate(2026, 11, 23, 0, 1);
    sched_check();
    check(emergency::attempts() == 1, "first failure counted");
    // Restore the day file and simulate a reboot.
    host::files()[dayPath(gen, 1)] = saved;
    resetRuntime();
    sched_setup();
    check(courseRunning, "course running after recovery");
    check(emergency::attempts() == 0, "failure counter reset");
    check(!emergency::isActive(), "not in emergency");
}

static void test_recovery_no_past_replay() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 23, 50);
    check(applyCourse(makeCourse(14, 4, 0)), "apply course");
    uint32_t gen = activeGeneration();
    sched_setup();
    check(sched_activateDay(0), "activation succeeds");
    std::string saved = host::files()[dayPath(gen, 1)];
    host::files().erase(dayPath(gen, 1));
    host::clock() = atDate(2026, 11, 23, 5, 0);   // past 04:00 event
    sched_check();
    check(emergency::attempts() == 1, "failure counted");
    host::files()[dayPath(gen, 1)] = saved;
    resetRuntime();
    sched_setup();
    check(courseRunning, "course running");
    sched_check();
    check(audioCalls == 0, "past event not replayed after recovery");
}

int main() {
    test_table();
    test_fires_0400();
    test_fires_0631();
    test_no_duplicate();
    test_without_json();
    test_counter_persists();
    test_third_failure_enters();
    test_no_reboot_in_emergency();
    test_reboot_after_10s();
    test_persists_across_reboot();
    test_recovery();
    test_manual_activate_failure();
    test_midnight_failure();
    test_auto_recovery();
    test_recovery_no_past_replay();
    std::cout << "SUMMARY checks=" << checks << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}
