// Host-only tests for course_store: LittleFS generations, NVS calendar state,
// atomic replacement and recovery. No device or real NVS/LittleFS access.
#include "course_store.h"

#include "LittleFS.h"
#include "Preferences.h"
#include "nvs.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;
std::string g_group;

void check(bool condition, const std::string& message) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::cout << "  FAIL " << g_group << ": " << message << "\n";
    }
}

void runGroup(const char* name, void (*fn)()) {
    g_group = name;
    const int before = g_fail;
    try {
        fn();
    } catch (const std::exception& e) {
        ++g_fail;
        std::cout << "  FAIL " << g_group << ": exception " << e.what() << "\n";
    } catch (...) {
        ++g_fail;
        std::cout << "  FAIL " << g_group << ": unknown exception\n";
    }
    if (g_fail == before) {
        std::cout << "PASS " << name << "\n";
    } else {
        std::cout << "FAIL " << name << "\n";
    }
}

void resetHost() {
    host::files().clear();
    host::fs() = host::FsFaults();
    host::nvs().visible.clear();
    host::nvs().durable.clear();
    host::nvs().handles.clear();
    host::nvs().nextHandle = 1;
    host::nvs().lastKey.clear();
    host::nvs().fault = host::Fault();
    host::nvs().trace.clear();
    host::nvs().beforeWrite = nullptr;
    host::nvs().beforeOperation = nullptr;
    host::nvs().afterU64Read = nullptr;
    host::nvs().namespacePresent = false;
    host::nvs().writes = 0;
    host::nvs().commits = 0;
    host::nvs().reads = 0;
}

course::Event event(int hour, int minute, const std::string& desc,
                    std::uint32_t id = 1) {
    course::Event e;
    e.id = id;
    e.hour = static_cast<std::uint8_t>(hour);
    e.minute = static_cast<std::uint8_t>(minute);
    e.description = desc;
    return e;
}

course::Course makeCourse(int days) {
    course::Course c;
    c.name = "test course";
    for (int i = 0; i < days; ++i) {
        course::Day d;
        d.hasOwnEvents = true;
        d.events.push_back(event(4, 0, "day " + std::to_string(i),
                                 static_cast<std::uint32_t>(i + 1)));
        c.days.push_back(d);
    }
    return c;
}

std::string key(const char* name) {
    return std::string("gong/") + name;
}

void calendar_roundtrip() {
    resetHost();
    course_store::CourseStore store;
    course_store::CalendarState state;
    state.active_generation = 3;
    state.draft_generation = 4;
    state.activation_date = 20261122;
    state.day_index = 5;
    state.course_length = 14;
    state.flags = course_store::kRunning;
    state.failure_count = 2;
    std::string error;
    check(store.saveCalendar(state, error), "save calendar: " + error);
    check(store.hasCalendar(), "calendar present");
    course_store::CalendarState loaded;
    check(store.loadCalendar(loaded, error), "load calendar: " + error);
    check(loaded.active_generation == 3, "active generation");
    check(loaded.draft_generation == 4, "draft generation");
    check(loaded.activation_date == 20261122, "activation date");
    check(loaded.day_index == 5, "day index");
    check(loaded.course_length == 14, "course length");
    check(loaded.flags == course_store::kRunning, "flags");
    check(loaded.failure_count == 2, "failure count");
}

void calendar_crc_corruption() {
    resetHost();
    course_store::CourseStore store;
    course_store::CalendarState state;
    state.active_generation = 1;
    state.course_length = 14;
    std::string error;
    check(store.saveCalendar(state, error), "save calendar");
    host::nvs().visible[key("course_state")].text = "garbage";
    course_store::CalendarState loaded;
    check(!store.loadCalendar(loaded, error), "corrupt calendar rejected");
    check(error.find("calendar") != std::string::npos ||
              error.find("CRC") != std::string::npos ||
              error.find("parse") != std::string::npos,
          "calendar error reported");
}

void apply_refuses_corrupt_calendar() {
    resetHost();
    course_store::CourseStore store;
    course_store::CalendarState state;
    state.active_generation = 1;
    state.course_length = 14;
    std::string error;
    check(store.saveCalendar(state, error), "save calendar");
    host::nvs().visible[key("course_state")].text = "garbage";
    std::uint32_t generation = 0;
    check(!store.applyCourse(makeCourse(3), generation, state, error),
          "apply refuses corrupt calendar");
    check(!store.saveDraftCourse(makeCourse(3), generation, state, error),
          "draft refuses corrupt calendar");
}

void last_fire_separate_and_crc() {
    resetHost();
    course_store::CourseStore store;
    course_store::CalendarState state;
    state.active_generation = 1;
    state.course_length = 14;
    std::string error;
    check(store.saveCalendar(state, error), "save calendar");
    course_store::LastFire fire;
    fire.generation = 1;
    fire.day_index = 3;
    fire.minute = 615;
    fire.event_id = 42;
    fire.date = 20261122;
    fire.timestamp = 1795310000;
    check(store.saveLastFire(fire, error), "save last_fire");
    course_store::LastFire loaded;
    check(store.loadLastFire(loaded, error), "load last_fire");
    check(loaded.generation == 1 && loaded.day_index == 3 &&
              loaded.minute == 615 && loaded.event_id == 42 &&
              loaded.date == 20261122 && loaded.timestamp == 1795310000,
          "last_fire fields");
    check(store.hasCalendar(), "calendar still present");
    check(store.clearLastFire(error), "clear last_fire");
    check(!store.loadLastFire(loaded, error), "last_fire removed");
    course_store::CalendarState loadedCalendar;
    check(store.loadCalendar(loadedCalendar, error),
          "calendar unaffected by last_fire clear");
    host::nvs().visible[key("last_fire")].text = "bad";
    check(!store.loadLastFire(loaded, error), "corrupt last_fire rejected");
}

void apply_course_and_load() {
    resetHost();
    course_store::CourseStore store;
    course::Course course = course::makeReferenceCourse();
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(course, generation, state, error),
          "apply course: " + error);
    check(generation == 1, "first generation");
    check(state.active_generation == 1, "active set");
    check(state.draft_generation == 1, "draft set");
    check(state.flags == course_store::kApplied, "APPLIED flag");
    check(state.activation_date == 0, "no activation date");
    check(state.course_length == 14, "course length 14");
    check(store.validateGeneration(1, 14, error), "generation validates");
    course_store::CourseMeta meta;
    check(store.loadMeta(1, meta, error), "load meta: " + error);
    check(meta.name == course.name, "meta name");
    check(meta.course_length == 14, "meta length");
    course::Day day;
    check(store.loadDay(1, 0, day, error), "load day 0: " + error);
    check(day.hasOwnEvents, "stored day is materialised");
    check(!day.events.empty(), "stored day has events");
}

void second_generation_replaces_old() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(course::makeReferenceCourse(), generation, state,
                            error),
          "apply first");
    check(generation == 1, "first generation");
    check(store.applyCourse(makeCourse(3), generation, state, error),
          "apply second");
    check(generation == 2, "second generation");
    check(state.active_generation == 2, "active second");
    check(!LittleFS.exists("/course/g1/meta.json"), "old generation removed");
    check(LittleFS.exists("/course/g2/meta.json"), "new generation present");
    check(store.validateGeneration(2, 3, error), "new generation validates");
}

void draft_is_independent() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(course::makeReferenceCourse(), generation, state,
                            error),
          "apply active");
    check(generation == 1, "active generation 1");
    std::uint32_t draft = 0;
    check(store.saveDraftCourse(makeCourse(5), draft, state, error),
          "save draft: " + error);
    check(draft == 2, "draft generation 2");
    check(state.active_generation == 1, "active unchanged");
    check(state.draft_generation == 2, "draft set");
    course::Day activeDay;
    check(store.loadDay(1, 0, activeDay, error), "active day still readable");
    course::Day draftDay;
    check(store.loadDay(2, 0, draftDay, error), "draft day readable");
    check(store.validateGeneration(2, 5, error), "draft validates");
    check(store.applyDraft(2, state, error), "apply draft: " + error);
    check(state.active_generation == 2, "draft published");
    check(state.flags == course_store::kApplied, "published is APPLIED");
    check(!LittleFS.exists("/course/g1/meta.json"), "old active removed");
}

void day_atomic_replacement() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(makeCourse(3), generation, state, error),
          "apply course");
    course::Day day;
    check(store.loadDay(1, 0, day, error), "load day");
    day.events[0].description = "changed";
    check(store.saveDayAtomic(1, 0, day, error), "replace day: " + error);
    course::Day loaded;
    check(store.loadDay(1, 0, loaded, error), "load replaced day");
    check(loaded.events[0].description == "changed", "replacement visible");
    check(!LittleFS.exists("/course/g1/d00.bak"), "backup removed");
    check(!LittleFS.exists("/course/g1/d00.tmp"), "temp removed");
}

void day_replacement_failure_restores_old() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(makeCourse(3), generation, state, error),
          "apply course");
    course::Day day;
    check(store.loadDay(1, 0, day, error), "load day");
    day.events[0].description = "should not stick";
    host::fs().renameErrorPath = "/course/g1/d00.tmp";
    check(!store.saveDayAtomic(1, 0, day, error), "publish rename fails");
    host::fs().renameErrorPath.clear();
    course::Day loaded;
    check(store.loadDay(1, 0, loaded, error), "old day readable");
    check(loaded.events[0].description != "should not stick",
          "old content restored");
}

void short_write_rejected() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(makeCourse(3), generation, state, error),
          "apply course");
    course::Day day;
    check(store.loadDay(1, 0, day, error), "load day");
    day.events[0].description = "short";
    host::fs().shortWrite = true;
    check(!store.saveDayAtomic(1, 0, day, error), "short write rejected");
    host::fs().shortWrite = false;
    course::Day loaded;
    check(store.loadDay(1, 0, loaded, error), "old day survives");
    check(loaded.events[0].description != "short", "old content survives");
}

void recover_interrupted_replace() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(makeCourse(3), generation, state, error),
          "apply course");
    const std::string path = "/course/g1/d00.json";
    const std::string bak = "/course/g1/d00.bak";
    const std::string tmp = "/course/g1/d00.tmp";
    host::files()[bak] = host::files()[path];
    host::files().erase(path);
    host::files()[tmp] = "CRC32 00000000\n{}";
    check(store.recover(error), "recover succeeds: " + error);
    check(LittleFS.exists(path.c_str()), "day restored from backup");
    check(!LittleFS.exists(tmp.c_str()), "temp removed");
    check(!LittleFS.exists(bak.c_str()), "backup consumed");
}

void recover_corrupt_day_with_backup() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(makeCourse(3), generation, state, error),
          "apply course");
    const std::string path = "/course/g1/d00.json";
    const std::string bak = "/course/g1/d00.bak";
    host::files()[bak] = host::files()[path];
    host::files()[path] = "garbage";
    check(store.recover(error), "recover corrupt day: " + error);
    course::Day day;
    check(store.loadDay(1, 0, day, error), "restored day loads");
}

void recover_corrupt_meta_fails() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(makeCourse(3), generation, state, error),
          "apply course");
    host::files()["/course/g1/meta.json"] = "garbage";
    check(!store.recover(error), "corrupt meta without backup rejected");
}

void apply_power_cut_before_nvs() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(makeCourse(3), generation, state, error),
          "apply first");
    host::nvs().inject("set", "course_state", host::Action::CutBefore);
    bool cut = false;
    try {
        store.applyCourse(makeCourse(4), generation, state, error);
    } catch (const host::PowerCut&) {
        cut = true;
    }
    check(cut, "power cut propagated");
    host::nvs().reboot();
    course_store::CalendarState loaded;
    check(store.loadCalendar(loaded, error), "calendar survives reboot");
    check(loaded.active_generation == 1, "old active remains");
    check(store.recover(error), "recover after power cut: " + error);
}

void apply_power_cut_after_set_before_commit() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(makeCourse(3), generation, state, error),
          "apply first");
    host::nvs().inject("set", "course_state", host::Action::CutAfter);
    bool cut = false;
    try {
        store.applyCourse(makeCourse(4), generation, state, error);
    } catch (const host::PowerCut&) {
        cut = true;
    }
    check(cut, "power cut after NVS set");
    host::nvs().reboot();
    course_store::CalendarState loaded;
    check(store.loadCalendar(loaded, error), "old calendar survives reboot");
    check(loaded.active_generation == 1, "old active generation preserved");
    check(store.recover(error), "recover after NVS cut: " + error);
}

void apply_meta_write_failure_keeps_old() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(makeCourse(3), generation, state, error),
          "apply first");
    host::fs().failWrite = true;
    host::fs().path = "/course/g2/meta.tmp";
    check(!store.applyCourse(makeCourse(4), generation, state, error),
          "meta write failure rejected");
    host::fs().failWrite = false;
    host::fs().path.clear();
    course_store::CalendarState loaded;
    check(store.loadCalendar(loaded, error), "old calendar readable");
    check(loaded.active_generation == 1, "old active kept after meta failure");
}

void apply_nvs_error_keeps_old() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(makeCourse(3), generation, state, error),
          "apply first");
    host::nvs().inject("set", "course_state", host::Action::ErrorBefore);
    check(!store.applyCourse(makeCourse(4), generation, state, error),
          "apply fails on NVS error");
    course_store::CalendarState loaded;
    check(store.loadCalendar(loaded, error), "old calendar readable");
    check(loaded.active_generation == 1, "old active preserved");
}

void validate_generation_detects_corruption() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(makeCourse(3), generation, state, error),
          "apply course");
    host::files()["/course/g1/d01.json"] = "garbage";
    check(!store.validateGeneration(1, 3, error), "corrupt day rejected");
}

void no_calendar_recover_is_ok() {
    resetHost();
    course_store::CourseStore store;
    std::string error;
    check(store.recover(error), "fresh device recover is a no-op");
}

}  // namespace

int main() {
    runGroup("calendar_roundtrip", calendar_roundtrip);
    runGroup("calendar_crc_corruption", calendar_crc_corruption);
    runGroup("apply_refuses_corrupt_calendar", apply_refuses_corrupt_calendar);
    runGroup("last_fire_separate_and_crc", last_fire_separate_and_crc);
    runGroup("apply_course_and_load", apply_course_and_load);
    runGroup("second_generation_replaces_old", second_generation_replaces_old);
    runGroup("draft_is_independent", draft_is_independent);
    runGroup("day_atomic_replacement", day_atomic_replacement);
    runGroup("day_replacement_failure_restores_old",
             day_replacement_failure_restores_old);
    runGroup("short_write_rejected", short_write_rejected);
    runGroup("recover_interrupted_replace", recover_interrupted_replace);
    runGroup("recover_corrupt_day_with_backup", recover_corrupt_day_with_backup);
    runGroup("recover_corrupt_meta_fails", recover_corrupt_meta_fails);
    runGroup("apply_power_cut_before_nvs", apply_power_cut_before_nvs);
    runGroup("apply_power_cut_after_set_before_commit",
             apply_power_cut_after_set_before_commit);
    runGroup("apply_meta_write_failure_keeps_old",
             apply_meta_write_failure_keeps_old);
    runGroup("apply_nvs_error_keeps_old", apply_nvs_error_keeps_old);
    runGroup("validate_generation_detects_corruption",
             validate_generation_detects_corruption);
    runGroup("no_calendar_recover_is_ok", no_calendar_recover_is_ok);

    std::cout << "PASS=" << g_pass << " FAIL=" << g_fail << "\n";
    return g_fail == 0 ? 0 : 1;
}
