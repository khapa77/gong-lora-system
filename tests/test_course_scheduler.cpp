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

static void audioCallback(uint8_t, uint8_t, uint8_t) { ++audioCalls; }

void logPrintf(const char*, ...) {}
bool mp3_isPlaying() { return audioPlaying; }
void mp3_stop() { audioPlaying = false; }
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
    };
    for (const TestCase& t : tests) {
        const int before = failures;
        t.fn();
        std::cout << (failures == before ? "PASS " : "FAIL ") << t.name << "\n";
    }
    std::cout << "SUMMARY checks=" << checks << " failures=" << failures << "\n";
    return failures == 0 ? 0 : 1;
}
