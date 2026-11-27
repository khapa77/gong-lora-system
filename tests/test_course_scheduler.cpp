// Host tests for the course-mode scheduler integration in schedule.cpp.
// LittleFS, NVS and audio are replaced by the host fakes; no hardware is used.
#include "host/Arduino.h"
#include "host/LittleFS.h"
#include "host/Preferences.h"
#include <ArduinoJson.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

extern "C" { bool gong_allocation_forbidden = false; }
// ASan provides its own operator new/delete interceptors; overriding them here
// would cause a duplicate-symbol link error. This test does not use the
// allocation-failure injection helpers, so the override is skipped when a
// sanitizer runtime is active.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
#define HOST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define HOST_SANITIZED 1
#endif
#endif
#ifndef HOST_SANITIZED
void* operator new(size_t n) {
    if (host::forbidAllocation()) std::abort();
    if (host::failNewAfter() == 0) { host::failNewAfter() = -1; throw std::bad_alloc(); }
    if (host::failNewAfter() > 0) --host::failNewAfter();
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
#endif

void convertToJson(const String& src, ArduinoJson::JsonVariant dst) {
    dst.set(static_cast<const std::string&>(src));
}

#define time host_time
#include "../server/src/schedule.cpp"
#undef time

// ---- audio / system fakes ------------------------------------------------
static unsigned int audioCalls = 0;
static bool audioPlaying = false;

// Relay stubs: the scheduler must commit a gong only after the audio really
// started. gRelayStarts simulates relay_startCount(), gRelayPending the queue.
static uint32_t gTagSeq = 0;
static uint32_t gRelayPendingTag = 0;
static uint32_t gRelayLastTag = 0;
static bool     gDeferCallback = false;

static bool audioCallback(uint8_t, uint8_t, uint8_t, uint32_t* outTag) {
    ++audioCalls;
    const uint32_t tag = ++gTagSeq;
    if (outTag) *outTag = tag;
    if (gDeferCallback) { gRelayPendingTag = tag; return false; }
    gRelayPendingTag = 0;
    gRelayLastTag = tag;
    return true;
}

void logPrintf(const char*, ...) {}
bool mp3_isPlaying() { return audioPlaying; }
void mp3_stop() { audioPlaying = false; }
// schedule.cpp routes "stop everything" through relay_stop() (it also cancels a
// pending warm-up); the relay itself is exercised by run_relay_tests.py.
void relay_stop() { audioPlaying = false; }
// Schedule asks the relay whether a queued gong actually started.
uint32_t relay_pendingTag() { return gRelayPendingTag; }
uint32_t relay_lastStartedTag() { return gRelayLastTag; }
bool mp3_trackExists(uint8_t) { return true; }
bool sys_fsOk() { return true; }
void sys_lock() {}
void sys_unlock() {}

// ---- helpers -------------------------------------------------------------
static int failures = 0;
static int checks = 0;

static void check(bool condition, const char* what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cout << "  FAIL: " << what << "\n";
    }
}

static time_t atDate(int year, int month, int day, int hour = 3, int minute = 0) {
    std::tm t;
    std::memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_isdst = 0;
    return std::mktime(&t);
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
    courseStore.invalidateTemplates();
    emergency::resetRuntime();
    gRelayPendingTag = 0;
    gRelayLastTag = 0;
    gDeferCallback = false;
    onScheduleTrigger = audioCallback;
    audioCalls = 0;
    audioPlaying = false;
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
    std::string err;
    uint32_t gen = 0;
    course_store::CalendarState st;
    if (!store.applyCourse(c, gen, st, err)) {
        std::cout << "applyCourse failed: " << err << "\n";
        return false;
    }
    return true;
}

static bool readCalendar(course_store::CalendarState& out) {
    course_store::CourseStore store;
    std::string err;
    return store.loadCalendar(out, err);
}

// Simulates a reboot: keep NVS and LittleFS, reset RAM and run sched_setup().
static void reboot() {
    uint32_t gen = courseGeneration;
    uint8_t len = courseLength;
    (void)gen; (void)len;
    resetRuntime();
    sched_setup();
}

// ---- tests ---------------------------------------------------------------
static void test_boot_applied_no_calls() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(courseMode, "course mode entered");
    check(!courseRunning, "APPLIED course does not run");
    check(activeDay == -1, "no active day before activation");
    check(audioCalls == 0, "no calls before activation");
}

static void test_activate_day0_running() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    check(courseRunning, "course running after activation");
    check(activeDay == 0, "active day 0");
    check(count == 1, "one event loaded");
    check(entries[0].description == "day0", "day 0 event published");
    check(audioCalls == 0, "activation does not ring");
}

static void test_midnight_advance_day1() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    host::clock() = atDate(2026, 11, 23, 3, 0);
    sched_check();
    check(activeDay == 1, "advanced to day 1");
    check(entries[0].description == "day1", "day 1 event published");
}

static void test_wrap_last_to_zero() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(13), "activate day 13");
    host::clock() = atDate(2026, 11, 23, 3, 0);
    sched_check();
    check(activeDay == 0, "wrapped to day 0");
}

static void test_wrap_11_day_course() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(11)), "apply course");
    sched_setup();
    check(sched_activateDay(10), "activate day 10");
    host::clock() = atDate(2026, 11, 23, 3, 0);
    sched_check();
    check(activeDay == 0, "11-day course wrapped to day 0");
    host::clock() = atDate(2026, 11, 24, 3, 0);
    sched_check();
    check(activeDay == 1, "11-day course advanced to day 1");
}

static void test_month_and_year_boundary() {
    freshEnvironment();
    host::clock() = atDate(2026, 12, 31);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    host::clock() = atDate(2027, 1, 1, 3, 0);
    sched_check();
    check(activeDay == 1, "crossed month/year to day 1");
}

static void test_leap_day() {
    freshEnvironment();
    host::clock() = atDate(2024, 2, 28);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    host::clock() = atDate(2024, 2, 29, 3, 0);
    sched_check();
    check(activeDay == 1, "leap day day 1");
    host::clock() = atDate(2024, 3, 1, 3, 0);
    sched_check();
    check(activeDay == 2, "after leap day day 2");
}

static void test_length_one_always_zero() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(1)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    host::clock() = atDate(2026, 11, 25, 3, 0);
    sched_check();
    check(activeDay == 0, "length 1 stays on day 0");
}

static void test_length_47_wrap() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(47)), "apply course");
    sched_setup();
    check(sched_activateDay(46), "activate day 46");
    host::clock() = atDate(2026, 11, 23, 3, 0);
    sched_check();
    check(activeDay == 0, "length 47 wraps to day 0");
}

static void test_manual_activate_arbitrary() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(7), "activate day 7");
    check(activeDay == 7, "active day 7");
    check(entries[0].description == "day7", "day 7 event published");
    course_store::CalendarState cal;
    check(readCalendar(cal), "calendar readable");
    check(cal.day_index == 7, "calendar stores activated day");
    check(cal.activation_date == 20261122u, "calendar stores activation date");
    check((cal.flags & course_store::kRunning) != 0, "calendar flags running");
}

static void test_audio_defers_advance() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    audioPlaying = true;
    host::clock() = atDate(2026, 11, 23, 3, 0);
    sched_check();
    check(activeDay == 0, "advance deferred while audio plays");
    audioPlaying = false;
    sched_check();
    check(activeDay == 1, "advance after audio ends");
}

static void test_midnight_event_skipped_after_audio() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    course::Course c = makeCourse(14, 0, 0);   // event at 00:00 every day
    check(applyCourse(c), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    audioPlaying = true;
    host::clock() = atDate(2026, 11, 23, 0, 0);
    sched_check();
    check(activeDay == 0, "00:00 event not fired while deferring");
    audioPlaying = false;
    host::clock() = atDate(2026, 11, 23, 0, 1);
    sched_check();
    check(activeDay == 1, "advanced after audio");
    check(audioCalls == 0, "missed 00:00 event not replayed");
}

static void test_no_catchup_after_reboot() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 12, 0);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    // Reboot at 12:00: the 04:00 event already passed and must not replay.
    audioCalls = 0;
    reboot();
    sched_check();
    check(audioCalls == 0, "no catch-up replay after reboot");
}

static void test_ordinary_edit_active_day() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    check(sched_addToDay(0, 5, 30, String("added"), 1, 1, 10), "add event to active day");
    check(count == 2, "RAM updated immediately");
    course_store::CourseStore store;
    std::string err;
    course_store::CourseMeta meta;
    check(store.loadMeta(courseGeneration, meta, err), "meta readable");
    course::Day d;
    check(store.loadDay(courseGeneration, 0, d, err), "day readable");
    check(d.events.size() == 2, "store updated for active day");
}

static void test_ordinary_edit_future_day() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    check(sched_addToDay(5, 6, 15, String("future"), 1, 1, 10), "add event to future day");
    check(count == 1, "RAM for active day unchanged");
    course_store::CourseStore store;
    std::string err;
    course::Day d;
    check(store.loadDay(courseGeneration, 5, d, err), "future day readable");
    check(d.events.size() == 2, "future day store updated");
}

static void test_last_fire_suppresses_repeat() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 4, 0);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    sched_check();
    check(audioCalls == 1, "event fired once");
    audioCalls = 0;
    reboot();
    sched_check();
    check(audioCalls == 0, "same event not repeated after reboot");
}

static void test_calendar_not_written_at_midnight() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    course_store::CalendarState before;
    check(readCalendar(before), "calendar before");
    host::clock() = atDate(2026, 11, 23, 3, 0);
    sched_check();
    course_store::CalendarState after;
    check(readCalendar(after), "calendar after");
    check(after.day_index == before.day_index, "day_index unchanged at midnight");
    check(after.activation_date == before.activation_date, "activation date unchanged");
}

static void test_last_fire_separate_key() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 4, 0);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    sched_check();
    course_store::CourseStore store;
    course_store::LastFire lf;
    std::string err;
    check(store.loadLastFire(lf, err), "last_fire stored separately");
    check(lf.generation == courseGeneration, "last_fire generation");
    check(lf.date == 20261122u, "last_fire date");
}

static void test_47_day_simulation() {
    freshEnvironment();
    const int baseDay = 1;
    host::clock() = atDate(2026, 11, baseDay);
    check(applyCourse(makeCourse(47)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    for (int i = 0; i < 47; ++i) {
        host::clock() = atDate(2026, 11, baseDay + i, 3, 0);
        sched_check();
        if (activeDay != i) {
            std::cout << "  day " << i << " got " << activeDay << "\n";
            check(false, "47-day sequence");
        }
    }
    host::clock() = atDate(2026, 11, baseDay + 47, 3, 0);
    sched_check();
    check(activeDay == 0, "47-day cycle wraps to 0");
}

static void test_event_fires_at_time() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 4, 0);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    sched_check();
    check(audioCalls == 1, "event fired at 04:00");
}

// The course length comes only from the JSON: 10 and 20 days must wrap to 00
// with no special rules for the last day.
static void test_wrap_10_day_course() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(10)), "apply 10-day course");
    sched_setup();
    check(sched_activateDay(9), "activate day 9 of 10");
    host::clock() = atDate(2026, 11, 23, 3, 0);
    sched_check();
    check(activeDay == 0, "10-day course wraps 09 -> 00");
    check(entries[0].description == "day0", "day 0 published after the wrap");
}

static void test_wrap_20_day_course() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(20)), "apply 20-day course");
    sched_setup();
    check(sched_activateDay(19), "activate day 19 of 20");
    host::clock() = atDate(2026, 11, 23, 3, 0);
    sched_check();
    check(activeDay == 0, "20-day course wraps 19 -> 00");
}

// A reboot in the middle of a course keeps the bound day, and the cyclic index
// still follows the activation date afterwards.
static void test_reboot_midcourse_keeps_day() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 12, 0);
    check(applyCourse(makeCourse(20)), "apply 20-day course");
    sched_setup();
    check(sched_activateDay(7), "activate day 7");
    reboot();
    check(courseMode && courseRunning, "course resumed after the reboot");
    check(activeDay == 7, "mid-course day kept across the reboot");
    host::clock() = atDate(2026, 11, 25, 3, 0);
    sched_check();
    check(activeDay == 10, "7 + 3 civil days = day 10");
}

static std::string courseJsonV2(int days) {
    std::string error;
    return course::toJsonV2(makeCourse(days), true, error);
}

// Replacing the course through the GUI path (validate -> import -> apply) must
// not auto-activate, and the new length drives the day cycle.
static void test_replace_course_different_length() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 3, 0);
    check(applyCourse(makeCourse(14)), "apply initial 14-day course");
    sched_setup();
    check(sched_activateDay(5), "activate day 5 of 14");
    check(audioCalls == 0, "activation itself does not ring");

    const std::string json = courseJsonV2(20);
    course_editor::Inspection inspection;
    std::string err;
    check(course_editor::inspectCourse(json, inspection, err), "validate 20-day JSON");
    check(inspection.length == 20, "validation reports 20 days");
    check(course_editor::importCourse(json, err), "import 20-day course");
    check(course_editor::applyDraft(err), "apply 20-day course");
    check(sched_courseApply(), "scheduler republished the draft");
    check(courseMode && !courseRunning, "replaced course is APPLIED, not running");
    check(activeDay == -1, "no active day after the replacement");
    host::clock() = atDate(2026, 11, 23, 4, 0);
    sched_check();
    check(audioCalls == 0, "replaced course did not ring before Activate");

    check(sched_activateDay(19), "activate day 19 of the new 20-day course");
    check(courseLength == 20, "new course length is 20");
    check(activeDay == 19, "day 19 bound");
    host::clock() = atDate(2026, 11, 24, 3, 0);
    sched_check();
    check(activeDay == 0, "new 20-day course wraps 19 -> 00");
}

// A fresh JSON-driven installation ships legacy day files but has no course and
// no legacy binding: it must wait silently instead of playing Day 00.
static void test_no_course_waits() {
    freshEnvironment();
    host::files()["/day00.conf"] = "[]";   // part of the flashed LittleFS image
    host::clock() = atDate(2026, 11, 22, 4, 0);
    sched_setup();
    check(std::string(sched_runMode()) == "waiting", "run mode is not waiting");
    check(!sched_courseEnabled(), "no published course is reported as enabled");
    check(!sched_isRunning(), "waiting device reports running");
    check(activeDay == -1, "waiting device has an active day");
    sched_check();
    check(audioCalls == 0, "waiting device rang the legacy schedule");
}

// A published course is APPLIED until the operator activates a day.
static void test_published_course_needs_manual_activation() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCourse(makeCourse(10)), "apply 10-day course");
    sched_setup();
    check(std::string(sched_runMode()) == "course", "run mode is not course");
    check(sched_courseEnabled(), "published course is not enabled");
    check(!courseRunning, "APPLIED course is running");
    host::clock() = atDate(2026, 11, 22, 4, 0);
    sched_check();
    check(audioCalls == 0, "APPLIED course rang before Activate");
    check(sched_activateDay(9), "activate day 9");
    check(std::string(sched_runMode()) == "course", "run mode changed after activation");
    host::clock() = atDate(2026, 11, 23, 4, 0);
    sched_check();
    check(audioCalls == 1, "activated course did not ring on the wrapped day 0");
}

// ---- runner --------------------------------------------------------------
struct TestCase { const char* name; void (*fn)(); };

// Stage C: the scheduler's day editors copy a reference day before changing it.
static void test_cow_scheduler_edits_reference_day() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);

    // Mixed tpl1 generation: day 0 has its own events (so boot and activation
    // work before stage E), days 1..3 reference the shared template.
    course::Course c;
    c.name = "cow-course";
    for (int i = 0; i < 4; ++i) {
        course::Event e;
        e.id = static_cast<std::uint32_t>(i + 1);
        e.hour = static_cast<std::uint8_t>(5 + i);
        e.minute = 0;
        e.track = 1;
        e.loop = 1;
        e.volume = 10;
        e.enabled = true;
        e.description = "tpl" + std::to_string(i);
        c.templates["practice"].push_back(e);
    }
    course::Day day0;
    day0.hasOwnEvents = true;
    {
        course::Event e;
        e.id = 1;
        e.hour = 4;
        e.minute = 0;
        e.track = 1;
        e.loop = 1;
        e.volume = 10;
        e.enabled = true;
        e.description = "day0";
        day0.events.push_back(e);
    }
    c.days.push_back(day0);
    for (int d = 1; d < 4; ++d) {
        course::Day day;
        day.templateName = "practice";
        c.days.push_back(day);
    }

    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string err;
    check(store.applyCompactCourse(c, generation, state, err),
          "compact apply failed");

    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    check(courseRunning && activeDay == 0, "day 0 active");

    // Drop the cache the way a fresh boot would (stage E loads it at boot):
    // editing a reference day must fetch the set itself.
    courseStore.invalidateTemplates();
    check(store.cachedTemplateGeneration() == 0, "cache not cleared for the test");

    // Add to a reference day: only that day may gain the event.
    check(sched_addToDay(2, 21, 30, String("cow"), 1, 1, 10),
          "add to reference day failed");
    course::Day day2;
    check(store.loadDay(generation, 2, day2, err), "day 2 load failed");
    check(day2.events.size() == 5, "added event not stored on day 2");
    course::Day day1;
    check(store.loadDay(generation, 1, day1, err), "day 1 load failed");
    check(day1.events.size() == 4, "adding to day 2 changed day 1");

    // Edit and delete on other reference days.
    check(sched_editInDay(1, 1, 5, 30, String("edited"), 1, 1, true, 10),
          "edit reference day failed");
    check(sched_delFromDay(3, 4), "delete from reference day failed");

    // Reboot at the store level: the individual days and the shared template
    // must survive exactly as written.
    host::nvs().reboot();
    store.invalidateTemplates();
    check(store.prepareTemplates(generation, err), "prepare after reboot failed");

    course::Day reread2;
    check(store.loadDay(generation, 2, reread2, err), "day 2 after reboot failed");
    check(reread2.events.size() == 5, "individual events lost across the reboot");
    course::Day reread1;
    check(store.loadDay(generation, 1, reread1, err), "day 1 after reboot failed");
    check(reread1.events.size() == 4, "edit lost across the reboot");
    check(reread1.events[0].hour == 5 && reread1.events[0].minute == 30,
          "edited time lost across the reboot");
    course::Day reread3;
    check(store.loadDay(generation, 3, reread3, err), "day 3 after reboot failed");
    check(reread3.events.size() == 3, "delete lost across the reboot");
    course::Day reread0;
    check(store.loadDay(generation, 0, reread0, err), "day 0 after reboot failed");
    check(reread0.events.size() == 1, "the active day changed");

    // The shared template is still the source for days nobody touched.
    check(host::files()["/course/g" + std::to_string(generation) + "/d02.json"]
                  .find("\"events\"") != std::string::npos,
          "day 2 was not materialised");
    // And the live schedule did not need a second activation.
    check(courseRunning && activeDay == 0, "active day lost after day edits");
    check(count == 1 && entries[0].description == "day0",
          "the live day changed while editing other days");
}

// ---- stage E: template (layout=tpl1) courses in the scheduler -------------

static course::Course makeTemplateCourse(int days, int perTemplate, int templateCount) {
    course::Course c;
    c.name = "tpl-course";
    for (int t = 0; t < templateCount; ++t) {
        const std::string name = "t" + std::to_string(t);
        for (int e = 0; e < perTemplate; ++e) {
            course::Event ev;
            ev.id = static_cast<std::uint32_t>(e + 1);
            ev.hour = static_cast<std::uint8_t>(5 + e);
            ev.minute = 0;
            ev.track = 1;
            ev.loop = 1;
            ev.volume = 10;
            ev.enabled = true;
            ev.description = name + "-" + std::to_string(e);
            c.templates[name].push_back(ev);
        }
    }
    for (int d = 0; d < days; ++d) {
        course::Day day;
        day.templateName = "t" + std::to_string(d % templateCount);
        c.days.push_back(day);
    }
    return c;
}

static bool applyCompact(const course::Course& c) {
    course_store::CourseStore store;
    std::uint32_t gen = 0;
    course_store::CalendarState st;
    std::string err;
    if (!store.applyCompactCourse(c, gen, st, err)) {
        std::cout << "applyCompactCourse failed: " << err << "\n";
        return false;
    }
    return true;
}

static void test_tpl_apply_activate_and_ring() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22);
    check(applyCompact(makeTemplateCourse(14, 4, 3)), "apply compact course");
    sched_setup();
    check(courseMode, "template course enters course mode");
    check(!courseRunning, "APPLIED template course does not run");
    check(activeDay == -1, "no active day before Activate");
    check(audioCalls == 0, "no calls before Activate");
    check(sched_dayCount() == 14, "course length from the template course");
    check(sched_activateDay(7), "activate day 7");
    check(courseRunning && activeDay == 7, "day 7 running");
    check(count == 4, "template events published");
    check(entries[0].description == "t1-0", "day 7 uses template t1");
    host::clock() = atDate(2026, 11, 22, 5, 0);
    sched_check();
    check(audioCalls == 1, "template event rang");
}

static void test_tpl_reboot_midnight_and_wrap() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 12, 0);
    check(applyCompact(makeTemplateCourse(14, 3, 3)), "apply");
    sched_setup();
    check(sched_activateDay(7), "activate day 7");
    reboot();
    check(courseMode && courseRunning, "running after reboot");
    check(activeDay == 7, "day 7 restored after reboot");
    check(count == 3, "day 7 events restored");
    host::clock() = atDate(2026, 11, 23, 3, 0);
    sched_check();
    check(activeDay == 8, "midnight advanced to day 8");
    host::clock() = atDate(2026, 11, 29, 3, 0);   // 7 + 7 days
    sched_check();
    check(activeDay == 0, "14-day course wrapped 13 -> 00");
}

static void test_tpl_draft_browsing_keeps_running() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 3, 0);
    check(applyCompact(makeTemplateCourse(14, 3, 3)), "apply active");
    sched_setup();
    check(sched_activateDay(2), "activate day 2");
    check(courseRunning && activeDay == 2, "running day 2");
    course_store::CourseStore store;
    const std::uint32_t active = courseGeneration;

    // A different template course lands in the draft generation.
    course::Course other = makeTemplateCourse(10, 5, 2);
    other.name = "draft course";
    std::string draftError;
    const std::string draftJson = course::toJsonV2(other, false, draftError);
    check(!draftJson.empty(), "draft JSON build failed");
    check(course_editor::importCourseAuto(draftJson, draftError),
          "compact draft import failed");

    // Browsing the draft resolves from the draft slot...
    std::string dayJson;
    std::string err;
    check(course_editor::dayJson(0, dayJson, err), "draft day readable");
    check(dayJson.find("t0-0") != std::string::npos, "draft day used the wrong template");

    // ...and the running generation still resolves through its own set.
    check(store.prepareActiveTemplates(active, err), "active templates still loaded");
    check(store.cachedTemplateGeneration() == active, "active slot was evicted");
    course::Day day;
    check(store.loadDay(active, 2, day, err), "active day still loadable");
    check(day.events.size() == 3, "active day events intact");

    // The live schedule was not disturbed.
    check(courseRunning && activeDay == 2 && count == 3, "live day changed");
    host::clock() = atDate(2026, 11, 22, 5, 0);
    sched_check();
    check(audioCalls == 1, "active schedule stopped ringing after draft browsing");
}

static void test_tpl_edit_draft_and_active_day() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 3, 0);
    check(applyCompact(makeTemplateCourse(14, 3, 3)), "apply");
    sched_setup();
    check(sched_activateDay(2), "activate day 2");

    // Edit a *different* day of the active generation (copy-on-write).
    check(sched_addToDay(5, 21, 0, String("extra"), 1, 1, 10),
          "edit of another day failed");
    check(count == 3, "live day changed by editing another day");
    course::Day other;
    std::string err;
    course_store::CourseStore store;
    check(store.loadDay(courseGeneration, 5, other, err), "day 5 load");
    check(other.events.size() == 4, "day 5 did not get the event");
    course::Day neighbour;
    check(store.loadDay(courseGeneration, 6, neighbour, err), "day 6 load");
    check(neighbour.events.size() == 3, "day 6 changed");

    // Editing the active day needs no second activation.
    check(sched_addToDay(2, 22, 0, String("live extra"), 1, 1, 10),
          "edit of the active day failed");
    check(count == 4, "active day RAM not updated");
    check(courseRunning, "still running after editing the active day");
    check(sched_dayCount() == 14, "length unchanged");
}

static void test_tpl_republish_other_length() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 3, 0);
    check(applyCompact(makeTemplateCourse(14, 3, 3)), "apply 14");
    sched_setup();
    check(sched_activateDay(7), "activate 7");
    check(courseRunning, "running");

    // A new 10-day course is published through the operational path while the
    // old one runs.
    course::Course next = makeTemplateCourse(10, 2, 2);
    std::string buildError;
    const std::string json = course::toJsonV2(next, false, buildError);
    check(!json.empty(), "10-day JSON build");
    check(course_editor::importCourseAuto(json, buildError), "import 10-day course");
    check(sched_courseApply(), "apply 10-day course");
    check(!courseRunning, "republished course does not run");
    check(courseCal.course_length == 10, "new length stored");
    check(courseGeneration != 0, "new generation active");
    check(audioCalls == 0, "no calls after republish");
    check(sched_activateDay(9), "activate day 9 of 10");
    host::clock() = atDate(2026, 11, 23, 3, 0);
    sched_check();
    check(activeDay == 0, "10-day course wraps 9 -> 0");
}

static void test_tpl_missing_templates_suspends() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 3, 0);
    check(applyCompact(makeTemplateCourse(14, 3, 2)), "apply");
    sched_setup();
    check(sched_activateDay(1), "activate day 1");
    const std::uint32_t gen = courseGeneration;
    host::files().erase("/course/g" + std::to_string(gen) + "/templates.json");

    reboot();
    check(courseMode, "still course mode");
    check(!courseRunning, "a course without templates must not run");
    check(!schedulerReady(), "scheduler not ready");
    check(audioCalls == 0, "no events executed without templates");
    course_store::CalendarState cal;
    check(readCalendar(cal), "calendar readable");
    check((cal.flags & course_store::kSuspended) != 0, "state marked SUSPENDED");
    check((cal.flags & course_store::kApplied) != 0, "course still APPLIED");
}

static void test_tpl_corrupt_templates_suspends() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 3, 0);
    check(applyCompact(makeTemplateCourse(14, 3, 2)), "apply");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    const std::uint32_t gen = courseGeneration;
    std::string& raw = host::files()["/course/g" + std::to_string(gen) + "/templates.json"];
    raw[raw.size() - 2] = (raw[raw.size() - 2] == 'x') ? 'y' : 'x';

    reboot();
    check(!courseRunning && !schedulerReady(), "corrupt CRC must not run");
    check(audioCalls == 0, "no events with a corrupt template file");
    course_store::CalendarState cal;
    check(readCalendar(cal), "calendar readable");
    check((cal.flags & course_store::kSuspended) != 0, "SUSPENDED after corrupt CRC");
}

// A day file that cannot be read must suspend instead of running wrong events.
static void test_audit_corrupt_day_suspends() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 3, 0);
    check(applyCompact(makeTemplateCourse(7, 3, 2)), "apply");
    sched_setup();
    check(sched_activateDay(2), "activate day 2");
    const std::uint32_t gen = courseGeneration;
    std::string& raw = host::files()["/course/g" + std::to_string(gen) + "/d02.json"];
    raw = "CRC32 00000000\n{\"format\":\"gong-course\"";

    reboot();
    check(courseMode, "course mode after corrupt day");
    check(!courseRunning && !schedulerReady(), "corrupt day must not run");
    check(audioCalls == 0, "no events from a corrupt day");
    course_store::CalendarState cal;
    check(readCalendar(cal), "calendar readable");
    check((cal.flags & course_store::kSuspended) != 0, "SUSPENDED after a corrupt day");
    check((cal.flags & course_store::kApplied) != 0, "course still APPLIED");
}

// The midnight switch must not publish a half-broken next day.
static void test_audit_midnight_broken_next_day() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 3, 0);
    check(applyCompact(makeTemplateCourse(7, 3, 2)), "apply");
    sched_setup();
    check(sched_activateDay(2), "activate day 2");
    check(courseRunning, "running day 2");
    const std::uint32_t gen = courseGeneration;
    std::string& next = host::files()["/course/g" + std::to_string(gen) + "/d03.json"];
    next = "CRC32 00000000\nnot json";

    host::clock() = atDate(2026, 11, 23, 3, 0);
    sched_check();
    check(!schedulerReady(), "a broken next day must not become active");
    check(audioCalls == 0, "no calls while the next day is broken");
    course_store::CalendarState cal;
    check(readCalendar(cal), "calendar readable");
    check((cal.flags & course_store::kSuspended) != 0, "SUSPENDED at midnight");
}

// Calendar semantics for the extremes of the supported range.
static void test_audit_length_one_and_47() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 3, 0);
    check(applyCompact(makeTemplateCourse(1, 2, 1)), "apply 1 day");
    sched_setup();
    check(sched_activateDay(0), "activate day 0 of 1");
    host::clock() = atDate(2026, 11, 29, 3, 0);
    sched_check();
    check(activeDay == 0, "a 1-day course stays on day 0");
    check(courseRunning, "1-day course keeps running");

    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 3, 0);
    check(applyCompact(makeTemplateCourse(47, 2, 2)), "apply 47 days");
    sched_setup();
    check(sched_activateDay(46), "activate day 46 of 47");
    host::clock() = atDate(2026, 11, 23, 3, 0);
    sched_check();
    check(activeDay == 0, "47-day course wraps 46 -> 00");
}

// A draft import while a course is running must not disturb the schedule.
static void test_audit_import_during_running() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 3, 0);
    check(applyCompact(makeTemplateCourse(14, 3, 3)), "apply active");
    sched_setup();
    check(sched_activateDay(2), "activate day 2");
    check(courseRunning && count == 3, "running day 2");

    course::Course other = makeTemplateCourse(10, 5, 2);
    other.name = "other";
    std::string buildError;
    const std::string json = course::toJsonV2(other, false, buildError);
    check(!json.empty(), "draft JSON build");
    check(course_editor::importCourseAuto(json, buildError), "draft import");

    // The running generation still resolves, and midnight still advances.
    check(courseRunning && activeDay == 2 && count == 3, "live day disturbed");
    host::clock() = atDate(2026, 11, 23, 3, 0);
    sched_check();
    check(activeDay == 3, "midnight advance after a draft import");
    check(count == 3 && schedulerReady(), "next day published after a draft import");
    host::clock() = atDate(2026, 11, 23, 5, 0);
    sched_check();
    check(audioCalls == 1, "schedule stopped ringing after a draft import");
}

// ── Relay pre-warm helper ──────────────────────────────────────────────────
// sched_msToNext() is the only thing the relay uses; the schedule itself must
// not change because of it.
static void test_msToNext_lifecycle() {
    freshEnvironment();
    check(sched_msToNext() == -1, "no course: nothing to pre-warm");

    host::clock() = atDate(2026, 11, 22, 3, 58);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_msToNext() == -1, "APPLIED (not activated): nothing will ring");

    check(sched_activateDay(0), "activate day 0");
    const long ms = sched_msToNext();
    check(ms == 2L * 60L * 1000L, "03:58 → 120000 ms to 04:00");

    // After the event fired there is no second event today: no pre-warm.
    host::clock() = atDate(2026, 11, 22, 4, 0);
    sched_check();
    check(sched_msToNext() == -1, "nothing else today after 04:00");
}

static void test_msToNext_ignores_disabled_and_handles_emergency() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 3, 0);
    check(applyCourse(makeCourse(2)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    check(sched_msToNext() == 60L * 60L * 1000L, "one hour to 04:00");

    // Disabling the only event removes the pre-warm target.
    check(sched_edit(1, 4, 0, "day0", 1, 1, false, 10), "disable the event");
    check(sched_msToNext() == -1, "disabled event: nothing to pre-warm");

    // Emergency stops normal execution; the relay must not be pre-warmed by a
    // schedule that will not run.
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 3, 0);
    check(applyCourse(makeCourse(2)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");
    emergency::recordFailure(emergency::kBootFailed);
    emergency::recordFailure(emergency::kBootFailed);
    emergency::recordFailure(emergency::kBootFailed);
    check(sched_emergencyActive(), "emergency latched");
    check(sched_msToNext() == -1, "emergency: no pre-warm");
}

// ── Deferred fire (relay warm-up) ──────────────────────────────────────────
static void test_deferred_fire_commits_after_start() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 4, 0);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");

    audioCalls = 0;
    gDeferCallback = true;                 // relay is cold: the gong is queued
    sched_check();
    check(audioCalls == 1, "trigger invoked");
    sched_check();                         // same minute: no repeat while queued
    check(audioCalls == 1, "no repeat while queued");

    gRelayLastTag = gRelayPendingTag;      // the relay really started OUR track
    gRelayPendingTag = 0;
    gDeferCallback = false;
    sched_check();                         // deferred fire resolves + commits

    resetRuntime();                        // reboot in the same minute
    sched_setup();
    sched_check();
    check(audioCalls == 0, "watermark committed: no replay after reboot");
}

static void test_deferred_fire_not_credited_to_foreign_start() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 4, 0);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");

    audioCalls = 0;
    gDeferCallback = true;
    sched_check();                         // our gong is queued (tag T)
    check(audioCalls == 1, "trigger invoked");

    // Another request (web/button) replaces the queue and starts instead.
    gRelayPendingTag = 0;
    gRelayLastTag = 999;
    gDeferCallback = false;
    sched_check();                         // must NOT credit our event

    resetRuntime();                        // reboot in the same minute
    sched_setup();
    sched_check();
    check(audioCalls == 1, "foreign start must not mark our event as fired");
}

static void test_deferred_fire_cancelled_is_not_marked() {
    freshEnvironment();
    host::clock() = atDate(2026, 11, 22, 4, 0);
    check(applyCourse(makeCourse(14)), "apply course");
    sched_setup();
    check(sched_activateDay(0), "activate day 0");

    audioCalls = 0;
    gDeferCallback = true;
    sched_check();                         // queued
    check(audioCalls == 1, "trigger invoked");

    gRelayPendingTag = 0;                  // STOP cancelled the warm-up
    gDeferCallback = false;
    sched_check();                         // resolves as cancelled, no watermark

    resetRuntime();                        // reboot in the same minute
    sched_setup();
    sched_check();
    check(audioCalls == 1, "cancelled gong is not marked: it rings after reboot");
}

int main() {
    TestCase tests[] = {
        {"boot_applied_no_calls", test_boot_applied_no_calls},
        {"activate_day0_running", test_activate_day0_running},
        {"midnight_advance_day1", test_midnight_advance_day1},
        {"wrap_last_to_zero", test_wrap_last_to_zero},
        {"wrap_11_day_course", test_wrap_11_day_course},
        {"month_and_year_boundary", test_month_and_year_boundary},
        {"leap_day", test_leap_day},
        {"length_one_always_zero", test_length_one_always_zero},
        {"length_47_wrap", test_length_47_wrap},
        {"manual_activate_arbitrary", test_manual_activate_arbitrary},
        {"audio_defers_advance", test_audio_defers_advance},
        {"midnight_event_skipped_after_audio", test_midnight_event_skipped_after_audio},
        {"no_catchup_after_reboot", test_no_catchup_after_reboot},
        {"ordinary_edit_active_day", test_ordinary_edit_active_day},
        {"ordinary_edit_future_day", test_ordinary_edit_future_day},
        {"last_fire_suppresses_repeat", test_last_fire_suppresses_repeat},
        {"calendar_not_written_at_midnight", test_calendar_not_written_at_midnight},
        {"last_fire_separate_key", test_last_fire_separate_key},
        {"47_day_simulation", test_47_day_simulation},
        {"event_fires_at_time", test_event_fires_at_time},
        {"wrap_10_day_course", test_wrap_10_day_course},
        {"wrap_20_day_course", test_wrap_20_day_course},
        {"reboot_midcourse_keeps_day", test_reboot_midcourse_keeps_day},
        {"replace_course_different_length", test_replace_course_different_length},
        {"no_course_waits", test_no_course_waits},
        {"published_course_needs_manual_activation", test_published_course_needs_manual_activation},
        {"cow_scheduler_edits_reference_day", test_cow_scheduler_edits_reference_day},
        {"tpl_apply_activate_and_ring", test_tpl_apply_activate_and_ring},
        {"tpl_reboot_midnight_and_wrap", test_tpl_reboot_midnight_and_wrap},
        {"tpl_draft_browsing_keeps_running", test_tpl_draft_browsing_keeps_running},
        {"tpl_edit_draft_and_active_day", test_tpl_edit_draft_and_active_day},
        {"tpl_republish_other_length", test_tpl_republish_other_length},
        {"tpl_missing_templates_suspends", test_tpl_missing_templates_suspends},
        {"tpl_corrupt_templates_suspends", test_tpl_corrupt_templates_suspends},
        {"audit_corrupt_day_suspends", test_audit_corrupt_day_suspends},
        {"audit_midnight_broken_next_day", test_audit_midnight_broken_next_day},
        {"audit_length_one_and_47", test_audit_length_one_and_47},
        {"audit_import_during_running", test_audit_import_during_running},
        {"ms_to_next_lifecycle", test_msToNext_lifecycle},
        {"ms_to_next_disabled_emergency", test_msToNext_ignores_disabled_and_handles_emergency},
        {"deferred_fire_commits", test_deferred_fire_commits_after_start},
        {"deferred_fire_cancelled", test_deferred_fire_cancelled_is_not_marked},
        {"deferred_fire_foreign_start", test_deferred_fire_not_credited_to_foreign_start},
    };
    for (const TestCase& t : tests) {
        const int before = failures;
        t.fn();
        std::cout << (failures == before ? "PASS " : "FAIL ") << t.name << "\n";
    }
    std::cout << "SUMMARY checks=" << checks << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}
