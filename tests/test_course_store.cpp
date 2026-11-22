// Host-only tests for course_store: LittleFS generations, NVS calendar state,
// atomic replacement and recovery. No device or real NVS/LittleFS access.
#include "course_store.h"

#include "LittleFS.h"
#include "Preferences.h"
#include "nvs.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
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

// NVS limits key and namespace names to 15 characters. The device fails a
// 16-character key with KEY_TOO_LONG (this is what broke
// "course_bootstrap"), so the host model must fail it too instead of hiding it.
void nvs_key_length_limit() {
    resetHost();
    check(std::strlen("course_bootstrap") == 16, "fixture key is 16 characters");
    check(std::strlen("course_bootstra") == 15, "fixture limit key is 15 characters");

    Preferences prefs;
    check(prefs.begin("gong", false), "namespace opens");
    check(prefs.putUInt("course_bootstrap", 7) == 0, "16-character key write is rejected");
    check(prefs.putString("course_bootstrap", "x") == 0, "16-character key string write is rejected");
    check(host::nvs().visible.empty(), "rejected key wrote nothing");

    check(prefs.putUInt("course_bootstra", 7) == static_cast<size_t>(sizeof(std::uint32_t)),
          "15-character key write is accepted");
    check(prefs.getUInt("course_bootstra", 0) == 7, "15-character key reads back");
    check(host::nvs().visible.count(key("course_bootstra")) == 1, "15-character key stored");

    uint32_t value = 0;
    Preferences tooLongNamespace;
    check(!tooLongNamespace.begin("namespace_is_long", false),
          "namespace longer than 15 characters is rejected");
    check(nvs_get_u32(1, "course_bootstrap", &value) == ESP_ERR_NVS_KEY_TOO_LONG,
          "16-character key read is rejected with KEY_TOO_LONG");
}

// The bootstrap draft pointer must use a key NVS accepts and report failures.
void bootstrap_pointer_roundtrip() {
    resetHost();
    course_store::CourseStore store;
    std::string error;

    check(store.loadBootstrapGeneration() == 0, "no pointer before any draft");
    check(store.saveBootstrapGeneration(5, error), "bootstrap pointer write accepted");
    check(error.empty(), "successful pointer write reports no error");
    check(store.loadBootstrapGeneration() == 5, "bootstrap pointer reads back");
    check(host::nvs().visible.count(key("crs_boot")) == 1, "pointer stored under crs_boot");
    check(store.saveBootstrapGeneration(0, error), "pointer can be cleared");
    check(store.loadBootstrapGeneration() == 0, "cleared pointer reads as zero");
    check(store.saveBootstrapGeneration(0, error), "clearing an absent pointer is not an error");

    host::nvs().inject("set", "crs_boot", host::Action::ErrorBefore);
    check(!store.saveBootstrapGeneration(9, error), "device NVS error is reported, not hidden");
    check(!error.empty(), "failed pointer write keeps an error text");
    host::nvs().fault.armed = false;
    check(store.loadBootstrapGeneration() == 0, "failed pointer write stored nothing");
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage A: templates.json, the generation-tagged cache and day resolution.
// ─────────────────────────────────────────────────────────────────────────────

void writeRawFile(const std::string& path, const std::string& json) {
    char header[32];
    std::snprintf(header, sizeof(header), "CRC32 %08lx\n",
                  static_cast<unsigned long>(course_store::CourseStore::crc32(json)));
    host::files()[path] = std::string(header) + json;
}

std::string dayRefJson(std::uint32_t generation, const std::string& templateName) {
    return "{\"format\":\"gong-course\",\"version\":2,\"name\":\"day\",\"generation\":" +
           std::to_string(generation) + ",\"course_length\":1,\"days\":[{\"template\":\"" +
           templateName + "\"}]}";
}

std::map<std::string, std::vector<course::Event> > templateSet(
    const std::string& name, int events) {
    std::map<std::string, std::vector<course::Event> > set;
    std::vector<course::Event> list;
    for (int i = 0; i < events; ++i) {
        list.push_back(event(5 + i, 0, "tpl " + std::to_string(i),
                             static_cast<std::uint32_t>(i + 1)));
    }
    set[name] = list;
    return set;
}

// Builds a generation exactly as stage B will: templates.json plus reference
// day files and a meta marker.
bool seedTemplateGeneration(std::uint32_t generation, int days,
                            const std::string& templateName, int events,
                            std::string& error) {
    course_store::CourseStore store;
    if (!store.writeTemplates(generation, templateSet(templateName, events), error)) {
        return false;
    }
    course_store::CourseMeta meta;
    meta.generation = generation;
    meta.course_length = static_cast<std::uint8_t>(days);
    meta.name = "template course";
    meta.layout = course_store::kLayoutTemplates;
    if (!store.saveMeta(meta, error)) return false;
    for (int day = 0; day < days; ++day) {
        char name[8];
        std::snprintf(name, sizeof(name), "d%02u", static_cast<unsigned>(day));
        writeRawFile(std::string("/course/g") + std::to_string(generation) + "/" + name +
                         ".json",
                     dayRefJson(generation, templateName));
    }
    error.clear();
    return true;
}

std::string templatesPath(std::uint32_t generation) {
    return std::string("/course/g") + std::to_string(generation) + "/templates.json";
}

void templates_expanded_generation_unaffected() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(makeCourse(3), generation, state, error),
          "apply expanded course failed: " + error);
    course_store::CourseMeta meta;
    check(store.loadMeta(generation, meta, error), "loadMeta failed: " + error);
    check(meta.layout == course_store::kLayoutExpanded,
          "expanded generation must not report the tpl1 layout");
    check(host::files().find(templatesPath(generation)) == host::files().end(),
          "expanded generation wrote a templates file");
    check(store.prepareTemplates(generation, error),
          "prepareTemplates on an expanded generation failed: " + error);
    check(store.cachedTemplateCount() == 0,
          "expanded generation exposed template entries");
    check(store.templatesReady(generation), "expanded generation is not marked ready");
    course::Day day;
    check(store.loadDay(generation, 1, day, error), "loadDay failed: " + error);
    check(day.hasOwnEvents && day.events.size() == 1 &&
              day.events[0].description == "day 1",
          "expanded day changed shape");
}

void templates_reference_day_resolves() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 3, "practice", 4, error), "seed failed: " + error);
    course_store::CourseStore store;
    check(store.prepareTemplates(1, error), "prepareTemplates failed: " + error);
    check(store.cachedTemplateCount() == 1, "template count");
    check(store.cachedTemplateEvents() == 4, "template event count");
    check(store.cachedTemplateGeneration() == 1, "cache generation tag");
    course::Day day;
    check(store.loadDay(1, 2, day, error), "loadDay failed: " + error);
    check(day.hasOwnEvents, "resolved day is still a reference");
    check(day.templateName.empty(), "resolved day kept its template name");
    check(day.events.size() == 4, "resolved event count");
    check(day.events[0].description == "tpl 0", "resolved payload");
    const std::string raw = host::files()["/course/g1/d02.json"];
    check(raw.size() < 160, "a reference day file should stay tiny");
}

void templates_mixed_generation() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 3, "practice", 4, error), "seed failed: " + error);
    course_store::CourseStore store;
    // Day 1 carries its own events (an individually edited day).
    course::Day own;
    own.hasOwnEvents = true;
    own.events.push_back(event(9, 30, "own day", 7));
    check(store.saveDayAtomic(1, 1, own, error), "own day write failed: " + error);
    check(store.prepareTemplates(1, error), "prepareTemplates failed: " + error);
    course::Day ref;
    course::Day edited;
    check(store.loadDay(1, 0, ref, error), "reference day failed: " + error);
    check(store.loadDay(1, 1, edited, error), "own day failed: " + error);
    check(ref.events.size() == 4, "reference day lost its template events");
    check(edited.events.size() == 1 && edited.events[0].description == "own day",
          "own day was overwritten by the template");
    // Editing the own day again must not leak into the shared template.
    own.events[0].description = "own day v2";
    check(store.saveDayAtomic(1, 1, own, error), "own day rewrite failed: " + error);
    check(store.loadDay(1, 0, ref, error), "reference day reload failed: " + error);
    check(ref.events.size() == 4 && ref.events[0].description == "tpl 0",
          "editing one day changed a day that shares the template");
}

void templates_missing_template() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 2, "practice", 3, error), "seed failed: " + error);
    writeRawFile("/course/g1/d00.json", dayRefJson(1, "ghost"));
    course_store::CourseStore store;
    check(store.prepareTemplates(1, error), "prepareTemplates failed: " + error);
    course::Day day;
    error.clear();
    check(!store.loadDay(1, 0, day, error), "unknown template must not resolve");
    check(error.find("unknown template") != std::string::npos,
          "missing template error is unclear: " + error);
    error.clear();
    check(!store.validateGeneration(1, 2, error),
          "generation with a missing template must not validate");
}

void templates_corrupt_body() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 2, "practice", 3, error), "seed failed: " + error);
    // Valid CRC, broken JSON: the file must be rejected, not half-parsed.
    writeRawFile(templatesPath(1), "{\"format\":\"gong-course-templates\",\"version\":1,");
    course_store::CourseStore store;
    check(!store.prepareTemplates(1, error), "corrupt templates.json was accepted");
    check(store.cachedTemplateGeneration() == 0, "failed load left a generation tag");
    check(store.cachedTemplateCount() == 0, "failed load left templates cached");
    course::Day day;
    check(!store.loadDay(1, 0, day, error),
          "reference day loaded without a usable template set");
    check(day.events.empty(), "a failed load must not expose events");
}

void templates_crc_error() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 2, "practice", 3, error), "seed failed: " + error);
    std::string& raw = host::files()[templatesPath(1)];
    raw[raw.size() - 2] = (raw[raw.size() - 2] == 'x') ? 'y' : 'x';
    course_store::CourseStore store;
    error.clear();
    check(!store.prepareTemplates(1, error), "CRC error was not detected");
    check(error.find("CRC") != std::string::npos, "CRC error text: " + error);
    check(store.cachedTemplateCount() == 0, "CRC failure left templates cached");
}

void templates_cache_generation_mismatch() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 2, "one", 4, error), "seed 1 failed: " + error);
    check(seedTemplateGeneration(2, 2, "two", 2, error), "seed 2 failed: " + error);
    course_store::CourseStore store;
    check(store.prepareTemplates(1, error), "prepare(1) failed: " + error);
    course::Day day;
    error.clear();
    check(!store.loadDay(2, 0, day, error),
          "generation 2 was served from generation 1's cache");
    check(error.find("not loaded") != std::string::npos,
          "cross-generation error text: " + error);
    check(store.prepareTemplates(2, error), "prepare(2) failed: " + error);
    check(store.loadDay(2, 0, day, error), "loadDay(2) failed: " + error);
    check(day.events.size() == 2, "stale template set survived the reload");
    check(day.events[0].description == "tpl 0", "wrong template payload");
}

void templates_failed_reload_invalidates() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 2, "practice", 3, error), "seed failed: " + error);
    course_store::CourseStore store;
    check(store.prepareTemplates(1, error), "prepare failed: " + error);
    writeRawFile(templatesPath(1), "{\"format\":\"gong-course-templates\",\"version\":1,"
                                   "\"templates\":{\"practice\":[{\"id\":1,\"hour\":99}]}}");
    // A fresh boot reads the file again; the already-loaded set is dropped.
    store.invalidateTemplates();
    error.clear();
    check(!store.prepareTemplates(1, error), "invalid template body was accepted");
    check(store.cachedTemplateGeneration() == 0, "failed reload kept the old cache");
    course::Day day;
    check(!store.loadDay(1, 0, day, error),
          "stale cache was used after a failed reload");
}

void templates_recover_interrupted_write() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 2, "practice", 3, error), "seed failed: " + error);
    course_store::CourseStore store;
    course_store::CalendarState state;
    state.active_generation = 1;
    state.course_length = 2;
    state.flags = course_store::kApplied;
    check(store.saveCalendar(state, error), "saveCalendar failed: " + error);

    // Interrupted replacement: the new file sits in .tmp, the published one is
    // damaged beyond CRC.
    const std::string good = host::files()[templatesPath(1)];
    host::files()["/course/g1/templates.tmp"] = good;
    host::files()[templatesPath(1)] = "CRC32 00000000\n{\"format\":\"gong-course-templates\"}";

    check(store.recover(error), "recover failed: " + error);
    check(host::files().find("/course/g1/templates.tmp") == host::files().end(),
          "recover left the tmp file behind");
    course::Day day;
    check(store.loadDay(1, 0, day, error), "day load after recover failed: " + error);
    check(day.events.size() == 3, "recover lost the template events");
}

void templates_recover_backup() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 2, "practice", 3, error), "seed failed: " + error);
    course_store::CourseStore store;
    course_store::CalendarState state;
    state.active_generation = 1;
    state.course_length = 2;
    state.flags = course_store::kApplied;
    check(store.saveCalendar(state, error), "saveCalendar failed: " + error);

    const std::string good = host::files()[templatesPath(1)];
    host::files()["/course/g1/templates.bak"] = good;
    host::files()[templatesPath(1)] = "CRC32 00000000\n{";
    check(store.recover(error), "recover from backup failed: " + error);
    check(host::files().find("/course/g1/templates.bak") == host::files().end(),
          "recover left the bak file behind");
    course::Day day;
    check(store.loadDay(1, 0, day, error), "day load after backup restore failed: " + error);
    check(day.events.size() == 3, "backup restore lost the template events");
}

void templates_delete_generation() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 2, "practice", 3, error), "seed failed: " + error);
    course_store::CourseStore store;
    check(store.prepareTemplates(1, error), "prepare failed: " + error);
    check(store.cachedTemplateGeneration() == 1, "cache tag before delete");
    check(store.deleteGeneration(1, error), "deleteGeneration failed: " + error);
    check(host::files().find(templatesPath(1)) == host::files().end(),
          "deleteGeneration kept templates.json");
    check(store.cachedTemplateGeneration() == 0,
          "deleteGeneration kept the deleted generation in the cache");
}

void templates_limits() {
    resetHost();
    course_store::CourseStore store;
    std::string error;

    std::map<std::string, std::vector<course::Event> > many;
    for (int i = 0; i < 33; ++i) {
        many["t" + std::to_string(i)] = templateSet("x", 1)["x"];
    }
    check(!store.writeTemplates(1, many, error), "33 templates must be refused");

    std::map<std::string, std::vector<course::Event> > tooManyEvents;
    for (int i = 0; i < 9; ++i) {
        std::vector<course::Event> list;
        for (int e = 0; e < 32; ++e) {
            list.push_back(event(5 + e, 0, "e", static_cast<std::uint32_t>(e + 1)));
        }
        tooManyEvents["t" + std::to_string(i)] = list;
    }
    check(!store.writeTemplates(1, tooManyEvents, error),
          "288 template events must be refused");

    std::vector<course::Event> oversized;
    for (int e = 0; e < 33; ++e) {
        oversized.push_back(event(5, e, "e", static_cast<std::uint32_t>(e + 1)));
    }
    std::map<std::string, std::vector<course::Event> > perTemplate;
    perTemplate["big"] = oversized;
    check(!store.writeTemplates(1, perTemplate, error),
          "33 events in one template must be refused");
    check(host::files().find(templatesPath(1)) == host::files().end(),
          "a refused template set must not be written");
}

void templates_clone_keeps_layout() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 3, "practice", 3, error), "seed failed: " + error);
    course_store::CourseStore store;
    std::uint32_t clone = 0;
    check(store.cloneGeneration(1, clone, error), "clone failed: " + error);
    course_store::CourseMeta meta;
    check(store.loadMeta(clone, meta, error), "clone meta failed: " + error);
    check(meta.layout == course_store::kLayoutTemplates,
          "clone lost the tpl1 layout marker");
    check(host::files().find(templatesPath(clone)) != host::files().end(),
          "clone did not copy templates.json");
    const std::string dayFile = "/course/g" + std::to_string(clone) + "/d00.json";
    check(host::files()[dayFile].size() < 160,
          "clone materialised a reference day");
    check(store.prepareTemplates(clone, error), "prepare(clone) failed: " + error);
    course::Day day;
    check(store.loadDay(clone, 0, day, error), "clone day load failed: " + error);
    check(day.events.size() == 3, "clone lost the template events");
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage B: compact (template) generation writing. Not part of the operational
// path; exercised directly here.
// ─────────────────────────────────────────────────────────────────────────────

std::map<std::string, std::vector<course::Event> > templateEvents(int count,
                                                                  const std::string& tag) {
    std::map<std::string, std::vector<course::Event> > set;
    std::vector<course::Event> list;
    for (int i = 0; i < count; ++i) {
        list.push_back(event(5 + i % 18, (i * 3) % 60, tag + " " + std::to_string(i),
                             static_cast<std::uint32_t>(i + 1)));
    }
    set["x"] = list;
    return set;
}

course::Course makeTemplateCourse(int days, int perTemplate, int templateCount) {
    course::Course course;
    course.name = "compact course";
    for (int t = 0; t < templateCount; ++t) {
        const std::string name = "t" + std::to_string(t);
        course.templates[name] = templateEvents(perTemplate, name)["x"];
    }
    for (int d = 0; d < days; ++d) {
        course::Day day;
        day.templateName = "t" + std::to_string(d % templateCount);
        day.hasOwnEvents = false;
        course.days.push_back(day);
    }
    return course;
}

std::size_t generationBytes(std::uint32_t generation) {
    const std::string prefix = "/course/g" + std::to_string(generation) + "/";
    std::size_t total = 0;
    for (std::map<std::string, std::string>::const_iterator it = host::files().begin();
         it != host::files().end(); ++it) {
        if (it->first.compare(0, prefix.size(), prefix) == 0) total += it->second.size();
    }
    return total;
}

void compact_write_links_repeated_days() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    std::string error;
    const course::Course course = makeTemplateCourse(14, 10, 3);
    check(store.writeCompactGeneration(course, generation, error),
          "compact write failed: " + error);
    course_store::CourseMeta meta;
    check(store.loadMeta(generation, meta, error), "meta: " + error);
    check(meta.layout == course_store::kLayoutTemplates, "layout marker missing");
    check(host::files().find(templatesPath(generation)) != host::files().end(),
          "templates.json missing");
    // Every day is a reference: no day file may carry an event array.
    for (int d = 0; d < 14; ++d) {
        char name[8];
        std::snprintf(name, sizeof(name), "d%02u", static_cast<unsigned>(d));
        const std::string path =
            "/course/g" + std::to_string(generation) + "/" + name + ".json";
        check(host::files().find(path) != host::files().end(),
              std::string("missing day file ") + name);
        check(host::files()[path].find("\"events\"") == std::string::npos,
              std::string("day ") + name + " was materialised");
        check(host::files()[path].size() < 200,
              std::string("day ") + name + " is larger than a reference");
    }
    check(store.prepareTemplates(generation, error), "prepare: " + error);
    check(store.cachedTemplateCount() == 3, "unique template count");
    check(store.cachedTemplateEvents() == 30, "unique event count");
    course::Day day;
    check(store.loadDay(generation, 13, day, error), "loadDay: " + error);
    check(day.events.size() == 10, "resolved event count");
    check(day.events[0].description == "t1 0", "wrong template for day 13");
}

void compact_apply_states_and_sizes() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t expanded = 0;
    course_store::CalendarState cal;
    std::string error;
    // A realistic 14-day operator course: 4 templates, 136 occurrences.
    course::Course expandedCourse = makeTemplateCourse(14, 10, 4);
    expandedCourse.days[5].hasOwnEvents = true;
    expandedCourse.days[5].templateName.clear();
    expandedCourse.days[5].events.push_back(event(21, 15, "own", 99));
    check(store.applyCourse(expandedCourse, expanded, cal, error),
          "expanded apply failed: " + error);
    const std::size_t expandedBytes = generationBytes(expanded);
    check(store.prepareTemplates(expanded, error), "prepare expanded: " + error);
    course::Day check0;
    check(store.loadDay(expanded, 0, check0, error), "expanded day: " + error);
    check(check0.events.size() == 10, "expanded day event count");

    std::uint32_t compact = 0;
    course_store::CalendarState state;
    check(store.applyCompactCourse(expandedCourse, compact, state, error),
          "compact apply failed: " + error);
    const std::size_t compactBytes = generationBytes(compact);
    std::cout << "  expanded=" << expandedBytes << "B compact=" << compactBytes
              << "B days=" << expandedCourse.days.size() << "\n";
    check(compactBytes < expandedBytes, "compact storage is not smaller");

    check(state.flags == course_store::kApplied, "compact apply must be APPLIED");
    check((state.flags & course_store::kRunning) == 0,
          "compact apply must not be RUNNING");
    check(state.activation_date == 0, "compact apply must not set activation_date");
    check(state.active_generation == compact, "active generation not switched");
    check(state.course_length == 14, "course length");

    // The old generation is reclaimed and the new one resolves through its own
    // cache only.
    check(!store.generationExists(expanded), "old generation was kept");
    check(store.prepareTemplates(compact, error), "prepare compact: " + error);
    check(store.cachedTemplateGeneration() == compact, "cache generation");
    course::Day day;
    check(store.loadDay(compact, 0, day, error), "compact day: " + error);
    check(day.events.size() == 10, "compact day events");
    check(store.loadDay(compact, 5, day, error), "compact own day: " + error);
    check(day.events.size() == 1 && day.events[0].description == "own",
          "individually edited day was replaced by the template");
}

void compact_lengths() {
    const int lengths[] = {1, 10, 14, 20, 47};
    for (std::size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        resetHost();
        course_store::CourseStore store;
        std::uint32_t generation = 0;
        course_store::CalendarState state;
        std::string error;
        const course::Course course = makeTemplateCourse(lengths[i], 6, 2);
        check(store.applyCompactCourse(course, generation, state, error),
              "apply " + std::to_string(lengths[i]) + " days: " + error);
        check(state.course_length == lengths[i], "length mismatch");
        check(store.prepareTemplates(generation, error), "prepare failed");
        course::Day last;
        check(store.loadDay(generation, static_cast<std::uint8_t>(lengths[i] - 1), last,
                            error),
              "last day load failed: " + error);
        check(last.events.size() == 6, "last day events");
    }
}

void compact_rejects_full_events() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    std::string error;
    check(!store.writeCompactGeneration(makeCourse(3), generation, error),
          "a course without references must not use the compact writer");
    check(error.find("no template references") != std::string::npos,
          "unclear rejection: " + error);
    check(generation == 0, "a rejected write must not report a generation");
}

void compact_limits() {
    resetHost();
    course_store::CourseStore store;
    std::string error;
    std::uint32_t generation = 0;

    // 33 distinct referenced templates.
    course::Course many;
    many.name = "many";
    for (int t = 0; t < 33; ++t) {
        const std::string name = "t" + std::to_string(t);
        many.templates[name] = templateEvents(1, name)["x"];
        course::Day day;
        day.templateName = name;
        many.days.push_back(day);
    }
    check(!store.writeCompactGeneration(many, generation, error),
          "33 templates must be refused");

    // 288 unique events across templates (more than the 256 budget).
    course::Course heavy;
    heavy.name = "heavy";
    for (int t = 0; t < 9; ++t) {
        const std::string name = "t" + std::to_string(t);
        std::vector<course::Event> list;
        for (int e = 0; e < 32; ++e) {
            list.push_back(event(5 + e, 0, "e", static_cast<std::uint32_t>(e + 1)));
        }
        heavy.templates[name] = list;
        course::Day day;
        day.templateName = name;
        heavy.days.push_back(day);
    }
    check(!store.writeCompactGeneration(heavy, generation, error),
          "288 unique template events must be refused");
}

void compact_failures_keep_published_course() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t published = 0;
    course_store::CalendarState cal;
    std::string error;
    check(store.applyCourse(makeCourse(4), published, cal, error),
          "baseline apply failed: " + error);
    const std::string publishedMeta =
        host::files()["/course/g" + std::to_string(published) + "/meta.json"];
    const auto filesBefore = host::files();

    // 1. LittleFS refuses the template file: nothing may change.
    host::fs().failWrite = true;
    host::fs().path = "/course/g" + std::to_string(published + 1) + "/templates.tmp";
    std::uint32_t candidate = 0;
    check(!store.applyCompactCourse(makeTemplateCourse(14, 10, 3), candidate, cal, error),
          "a failed template write must fail the apply");
    host::fs().failWrite = false;
    host::fs().path.clear();

    // 2. NVS refuses the calendar write after the generation was written.
    host::nvs().inject("set", "course_state", host::Action::ErrorBefore);
    check(!store.applyCompactCourse(makeTemplateCourse(14, 10, 3), candidate, cal, error),
          "a failed NVS write must fail the apply");
    host::nvs().fault = host::Fault();

    check(host::files()["/course/g" + std::to_string(published) + "/meta.json"] ==
              publishedMeta,
          "the published generation was modified");
    course_store::CalendarState after;
    check(store.loadCalendar(after, error), "calendar unreadable: " + error);
    check(after.active_generation == published, "calendar switched on failure");
    check(after.flags == course_store::kApplied, "flags changed on failure");
    check(store.validateGeneration(published, 4, error),
          "the published course no longer validates: " + error);
    // No half-written generation may be referenced.
    check(!store.generationExists(published + 1) ||
              host::files().find(templatesPath(published + 1)) == host::files().end(),
          "a partial compact generation survived the failure");
    (void)filesBefore;
}

void compact_roundtrip_and_clone() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    std::string error;
    const course::Course course = makeTemplateCourse(6, 8, 2);
    check(store.writeCompactGeneration(course, generation, error),
          "write failed: " + error);
    check(store.prepareTemplates(generation, error), "prepare failed: " + error);
    for (int d = 0; d < 6; ++d) {
        course::Day day;
        check(store.loadDay(generation, static_cast<std::uint8_t>(d), day, error),
              "loadDay failed: " + error);
        std::vector<course::Event> expected;
        check(course::resolvedEvents(course, static_cast<std::size_t>(d), expected, error),
              "resolvedEvents: " + error);
        check(day.events.size() == expected.size(), "event count mismatch");
        bool same = day.events.size() == expected.size();
        for (std::size_t i = 0; same && i < expected.size(); ++i) {
            same = day.events[i].hour == expected[i].hour &&
                   day.events[i].minute == expected[i].minute &&
                   day.events[i].description == expected[i].description;
        }
        check(same, "round-trip changed the day");
    }
    std::uint32_t clone = 0;
    check(store.cloneGeneration(generation, clone, error), "clone: " + error);
    check(host::files().find(templatesPath(clone)) != host::files().end(),
          "clone lost templates.json");
    check(store.prepareTemplates(clone, error), "prepare clone: " + error);
    course::Day day;
    check(store.loadDay(clone, 4, day, error), "clone day: " + error);
    check(day.events.size() == 8, "clone day events");
    check(store.cachedTemplateGeneration() == clone, "clone cache generation");
}

void compact_draft_does_not_publish() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t draft = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.saveCompactDraft(makeTemplateCourse(5, 4, 2), draft, state, error),
          "compact draft failed: " + error);
    check(state.active_generation == 0, "a draft must not become active");
    check(state.draft_generation == draft, "draft pointer missing");
    check((state.flags & course_store::kApplied) == 0, "draft must not be APPLIED");
    check(store.hasCalendar(), "draft calendar missing");
    course::Day day;
    check(store.prepareTemplates(draft, error), "prepare draft: " + error);
    check(store.loadDay(draft, 0, day, error), "draft day: " + error);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage C: copy-on-write for days that reference a shared template.
// ─────────────────────────────────────────────────────────────────────────────

// writeAtomic() works on the base name: dNN.tmp / dNN.bak, not dNN.json.tmp.
std::string dayBasePath(std::uint32_t generation, int day) {
    char name[8];
    std::snprintf(name, sizeof(name), "d%02u", static_cast<unsigned>(day));
    return std::string("/course/g") + std::to_string(generation) + "/" + name;
}

std::string dayFile(std::uint32_t generation, int day) {
    char name[8];
    std::snprintf(name, sizeof(name), "d%02u", static_cast<unsigned>(day));
    return std::string("/course/g") + std::to_string(generation) + "/" + name + ".json";
}

void cow_reference_day_becomes_individual() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 3, "practice", 4, error), "seed failed: " + error);
    course_store::CourseStore store;
    check(store.prepareTemplates(1, error), "prepare failed: " + error);
    course::Day day;
    check(store.loadDay(1, 1, day, error), "load day 1 failed: " + error);
    check(day.events.size() == 4, "template events missing");

    const std::string templatesBefore = host::files()[templatesPath(1)];
    const std::string day0Before = host::files()[dayFile(1, 0)];
    const std::string day2Before = host::files()[dayFile(1, 2)];

    // Everything the editor changes happens on its own copy of the day.
    day.events.push_back(event(21, 30, "individual", 9));
    check(store.saveDayAtomic(1, 1, day, error), "save day 1 failed: " + error);

    check(host::files()[templatesPath(1)] == templatesBefore,
          "templates.json was modified by a day edit");
    check(host::files()[dayFile(1, 0)] == day0Before, "day 0 was rewritten");
    check(host::files()[dayFile(1, 2)] == day2Before, "day 2 was rewritten");
    check(host::files()[dayFile(1, 1)].find("\"events\"") != std::string::npos,
          "the edited day is not an own-event file");

    // Reboot: RAM is dropped, flash is kept.
    host::nvs().reboot();
    store.invalidateTemplates();
    check(store.prepareTemplates(1, error), "prepare after reboot failed: " + error);
    course::Day reloaded;
    check(store.loadDay(1, 1, reloaded, error), "reload day 1 failed: " + error);
    check(reloaded.events.size() == 5, "individual event lost across the reboot");
    check(reloaded.events[4].description == "individual", "individual payload changed");
    course::Day neighbour;
    check(store.loadDay(1, 0, neighbour, error), "reload day 0 failed: " + error);
    check(neighbour.events.size() == 4, "a day sharing the template changed");
    check(host::files()[dayFile(1, 0)].find("\"events\"") == std::string::npos,
          "an untouched reference day became individual");
}

void cow_add_edit_disable_remove() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 3, "practice", 4, error), "seed failed: " + error);
    course_store::CourseStore store;
    check(store.prepareTemplates(1, error), "prepare failed: " + error);

    course::Day day;
    check(store.loadDay(1, 0, day, error), "load failed: " + error);

    // add
    day.events.push_back(event(22, 45, "added", 7));
    check(store.saveDayAtomic(1, 0, day, error), "add failed: " + error);
    check(store.loadDay(1, 0, day, error), "reload failed: " + error);
    check(day.events.size() == 5, "add did not persist");

    // edit
    day.events[0].hour = 6;
    day.events[0].minute = 10;
    day.events[0].description = "edited";
    check(store.saveDayAtomic(1, 0, day, error), "edit failed: " + error);
    check(store.loadDay(1, 0, day, error), "reload failed: " + error);
    check(day.events[0].description == "edited" && day.events[0].hour == 6,
          "edit did not persist");

    // disable
    day.events[1].enabled = false;
    check(store.saveDayAtomic(1, 0, day, error), "disable failed: " + error);
    check(store.loadDay(1, 0, day, error), "reload failed: " + error);
    check(!day.events[1].enabled, "disable did not persist");

    // remove
    day.events.erase(day.events.begin() + 2);
    check(store.saveDayAtomic(1, 0, day, error), "remove failed: " + error);
    check(store.loadDay(1, 0, day, error), "reload failed: " + error);
    check(day.events.size() == 4, "remove did not persist");

    // The shared template is untouched: another day on it still has 4 events.
    course::Day other;
    check(store.loadDay(1, 2, other, error), "load day 2 failed: " + error);
    check(other.events.size() == 4, "removing from one day changed the template");
    check(other.events[0].description == "tpl 0", "template payload changed");
}

void cow_multiple_individual_days() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 4, "practice", 3, error), "seed failed: " + error);
    course_store::CourseStore store;
    check(store.prepareTemplates(1, error), "prepare failed: " + error);
    for (int d : {0, 3}) {
        course::Day day;
        check(store.loadDay(1, static_cast<std::uint8_t>(d), day, error),
              "load failed: " + error);
        day.events.push_back(event(23, d, "own " + std::to_string(d), 5));
        check(store.saveDayAtomic(1, static_cast<std::uint8_t>(d), day, error),
              "save failed: " + error);
    }
    for (int d : {0, 3}) {
        course::Day own;
        check(store.loadDay(1, static_cast<std::uint8_t>(d), own, error),
              "reload failed: " + error);
        check(own.events.size() == 4, "individual day lost its edit");
    }
    for (int d : {2, 1}) {
        course::Day shared;
        check(store.loadDay(1, static_cast<std::uint8_t>(d), shared, error),
              "reload failed: " + error);
        check(shared.events.size() == 3, "shared day changed");
        check(host::files()[dayFile(1, d)].find("\"events\"") == std::string::npos,
              "shared day became individual");
    }
}

void cow_fs_failure_keeps_day() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 2, "practice", 3, error), "seed failed: " + error);
    course_store::CourseStore store;
    check(store.prepareTemplates(1, error), "prepare failed: " + error);
    course::Day day;
    check(store.loadDay(1, 1, day, error), "load failed: " + error);
    const std::string before = host::files()[dayFile(1, 1)];

    day.events.push_back(event(9, 45, "must not persist", 4));
    host::fs().failWrite = true;
    host::fs().path = dayBasePath(1, 1) + ".tmp";
    check(!store.saveDayAtomic(1, 1, day, error), "a failed write must be reported");
    host::fs().failWrite = false;
    host::fs().path.clear();

    check(host::files()[dayFile(1, 1)] == before, "a failed write changed the day file");
    course::Day reloaded;
    check(store.loadDay(1, 1, reloaded, error), "reload failed: " + error);
    check(reloaded.events.size() == 3, "a failed write left changed events");
}

void cow_tmp_bak_recovery() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 2, "practice", 3, error), "seed failed: " + error);
    course_store::CourseStore store;
    course_store::CalendarState state;
    state.active_generation = 1;
    state.course_length = 2;
    state.flags = course_store::kApplied;
    check(store.saveCalendar(state, error), "calendar failed: " + error);
    check(store.prepareTemplates(1, error), "prepare failed: " + error);

    // An individual day was written, then the replacement was interrupted:
    // the new content is in .tmp and the published file is damaged.
    course::Day individual;
    individual.hasOwnEvents = true;
    individual.events.push_back(event(8, 0, "individual v1", 1));
    check(store.saveDayAtomic(1, 1, individual, error), "initial save failed: " + error);
    individual.events.push_back(event(9, 0, "individual v2", 2));
    check(store.saveDayAtomic(1, 1, individual, error), "second save failed: " + error);

    // Simulate a torn replace: valid new payload in .tmp, damaged .json.
    const std::string good = host::files()[dayFile(1, 1)];
    host::files()[dayBasePath(1, 1) + ".tmp"] = good;
    host::files()[dayFile(1, 1)] = "CRC32 00000000\n{";

    check(store.recover(error), "recover failed: " + error);
    check(host::files().find(dayBasePath(1, 1) + ".tmp") == host::files().end(),
          "recover left the tmp file behind");
    course::Day recovered;
    check(store.loadDay(1, 1, recovered, error), "load after recover failed: " + error);
    check(recovered.events.size() == 2, "recover lost the individual events");
    check(recovered.events.size() > 1 && recovered.events[1].description == "individual v2",
          "recover restored old data");
    course::Day shared;
    check(store.loadDay(1, 0, shared, error), "shared day after recover failed: " + error);
    check(shared.events.size() == 3, "recover broke a shared day");
}

void cow_cache_generation_after_clone() {
    resetHost();
    std::string error;
    check(seedTemplateGeneration(1, 2, "practice", 3, error), "seed failed: " + error);
    course_store::CourseStore store;
    check(store.prepareTemplates(1, error), "prepare failed: " + error);
    std::uint32_t clone = 0;
    check(store.cloneGeneration(1, clone, error), "clone failed: " + error);
    check(clone != 1, "clone reused the source id");

    // Validating the clone loads its own set, never the source generation's.
    check(store.cachedTemplateGeneration() == clone,
          "the clone was not tagged in the cache");
    course::Day day;
    check(store.loadDay(clone, 0, day, error), "clone day load failed: " + error);
    check(day.events.size() == 3, "clone lost the template events");

    // Switch the cache back to the source generation: a load of the clone must
    // then be refused instead of being served from another generation.
    check(store.prepareTemplates(1, error), "prepare source failed: " + error);
    error.clear();
    check(!store.loadDay(clone, 0, day, error),
          "the clone was served from the source generation's cache");
    check(error.find("not loaded") != std::string::npos, "unclear error: " + error);
    check(store.prepareTemplates(clone, error), "prepare clone failed: " + error);
    check(store.loadDay(clone, 0, day, error), "clone day load after reload failed: " + error);
}

void cow_expanded_generation_unchanged() {
    resetHost();
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    std::string error;
    check(store.applyCourse(makeCourse(3), generation, state, error),
          "expanded apply failed: " + error);
    check(store.prepareTemplates(generation, error), "prepare failed: " + error);
    course::Day day;
    check(store.loadDay(generation, 1, day, error), "load failed: " + error);
    day.events.push_back(event(12, 0, "extra", 5));
    check(store.saveDayAtomic(generation, 1, day, error), "save failed: " + error);

    course_store::CourseMeta meta;
    check(store.loadMeta(generation, meta, error), "meta failed: " + error);
    check(meta.layout == course_store::kLayoutExpanded,
          "editing an expanded generation changed its layout");
    check(host::files().find(templatesPath(generation)) == host::files().end(),
          "an expanded generation gained a templates file");
    course::Day reloaded;
    check(store.loadDay(generation, 1, reloaded, error), "reload failed: " + error);
    check(reloaded.events.size() == 2, "expanded day edit did not persist");
}

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
    runGroup("nvs_key_length_limit", nvs_key_length_limit);
    runGroup("bootstrap_pointer_roundtrip", bootstrap_pointer_roundtrip);
    runGroup("validate_generation_detects_corruption",
             validate_generation_detects_corruption);
    runGroup("no_calendar_recover_is_ok", no_calendar_recover_is_ok);

    // Stage A: templates.json, cache and day resolution.
    runGroup("templates_expanded_generation_unaffected",
             templates_expanded_generation_unaffected);
    runGroup("templates_reference_day_resolves", templates_reference_day_resolves);
    runGroup("templates_mixed_generation", templates_mixed_generation);
    runGroup("templates_missing_template", templates_missing_template);
    runGroup("templates_corrupt_body", templates_corrupt_body);
    runGroup("templates_crc_error", templates_crc_error);
    runGroup("templates_cache_generation_mismatch",
             templates_cache_generation_mismatch);
    runGroup("templates_failed_reload_invalidates",
             templates_failed_reload_invalidates);
    runGroup("templates_recover_interrupted_write",
             templates_recover_interrupted_write);
    runGroup("templates_recover_backup", templates_recover_backup);
    runGroup("templates_delete_generation", templates_delete_generation);
    runGroup("templates_limits", templates_limits);
    runGroup("templates_clone_keeps_layout", templates_clone_keeps_layout);

    // Stage B: compact generation writing (not wired into the operational path).
    runGroup("compact_write_links_repeated_days", compact_write_links_repeated_days);
    runGroup("compact_apply_states_and_sizes", compact_apply_states_and_sizes);
    runGroup("compact_lengths", compact_lengths);
    runGroup("compact_rejects_full_events", compact_rejects_full_events);
    runGroup("compact_limits", compact_limits);
    runGroup("compact_failures_keep_published_course",
             compact_failures_keep_published_course);
    runGroup("compact_roundtrip_and_clone", compact_roundtrip_and_clone);
    runGroup("compact_draft_does_not_publish", compact_draft_does_not_publish);

    // Stage C: copy-on-write for days that share a template.
    runGroup("cow_reference_day_becomes_individual",
             cow_reference_day_becomes_individual);
    runGroup("cow_add_edit_disable_remove", cow_add_edit_disable_remove);
    runGroup("cow_multiple_individual_days", cow_multiple_individual_days);
    runGroup("cow_fs_failure_keeps_day", cow_fs_failure_keeps_day);
    runGroup("cow_tmp_bak_recovery", cow_tmp_bak_recovery);
    runGroup("cow_cache_generation_after_clone", cow_cache_generation_after_clone);
    runGroup("cow_expanded_generation_unchanged", cow_expanded_generation_unchanged);

    std::cout << "PASS=" << g_pass << " FAIL=" << g_fail << "\n";
    return g_fail == 0 ? 0 : 1;
}
