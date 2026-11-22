// Host-only tests for course_editor: draft generations, structural edits,
// presets, import/export and Apply. No device or real NVS/LittleFS access.
#include "course_editor.h"
#include "course_store.h"

#include "LittleFS.h"
#include "Preferences.h"
#include "allocation.h"
#include "nvs.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" { bool gong_allocation_forbidden = false; }

// Heap metering for this suite. The device has no PSRAM, so the memory tests
// cap the C++ heap and expect the code to fail with an error, never to abort.
// A sanitizer runtime provides its own operator new, so the accounting is only
// compiled into the regular build.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
#define HOST_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define HOST_SANITIZED 1
#endif
#endif
#ifndef HOST_SANITIZED
namespace {
const std::size_t kAllocHeader = 16;
}
void* operator new(std::size_t n) {
    if (host::forbidAllocation()) std::abort();
    const std::size_t total = n + kAllocHeader;
    if (!host::heapBudgetCharge(total)) throw std::bad_alloc();
    void* raw = std::malloc(total ? total : kAllocHeader);
    if (!raw) {
        host::heapBudgetRelease(total);
        throw std::bad_alloc();
    }
    *static_cast<std::size_t*>(raw) = n;
    return static_cast<char*>(raw) + kAllocHeader;
}
void operator delete(void* p) noexcept {
    if (!p) return;
    char* raw = static_cast<char*>(p) - kAllocHeader;
    host::heapBudgetRelease(*reinterpret_cast<std::size_t*>(raw) + kAllocHeader);
    std::free(raw);
}
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif

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

course_store::CalendarState calendar() {
    course_store::CourseStore store;
    course_store::CalendarState state;
    std::string error;
    if (!store.loadCalendar(state, error)) {
        throw std::runtime_error("calendar unreadable: " + error);
    }
    return state;
}

course::Day loadDay(std::uint32_t generation, std::uint8_t day) {
    course_store::CourseStore store;
    course::Day out;
    std::string error;
    if (!store.loadDay(generation, day, out, error)) {
        throw std::runtime_error("day unreadable: " + error);
    }
    return out;
}

const char* kOneEventDay =
    "[{\"id\":1,\"hour\":4,\"min\":0,\"track\":1,\"loop\":1,\"vol\":5,"
    "\"en\":true,\"desc\":\"Wake-up\"}]";

void testEmptyStore() {
    resetHost();
    course_editor::Info info;
    std::string error;
    check(course_editor::info(info, error), "info on empty store succeeds");
    check(!info.hasCourse, "no course before ensureInitialCourse");
    check(!course_editor::dayJson(0, error, error), "dayJson fails without a course");
    check(!course_editor::saveDay(0, kOneEventDay, error), "saveDay fails without a course");
    check(!course_editor::applyDraft(error), "apply fails without a course");
}

void testInitialReferenceCourse() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "reference course published");
    check(!course_editor::ensureInitialCourse(error), "second ensureInitialCourse is refused");

    course_editor::Info info;
    check(course_editor::info(info, error), "info after initial course");
    check(info.hasCourse, "course present");
    check(info.length == 14, "reference course has 14 days");
    check(info.activeGeneration != 0, "active generation set");
    check(info.activeGeneration == info.draftGeneration, "draft starts as active");
    check(!info.draftDirty, "fresh course is not dirty");
    check(info.flags == course_store::kApplied, "course is APPLIED, not bound");

    const course_store::CalendarState cal = calendar();
    check(cal.activation_date == 0, "no activation date after publish");
    check(cal.day_index == 0, "day index 0 after publish");

    std::string day0;
    check(course_editor::dayJson(0, day0, error), "day 0 readable");
    check(day0.find("Arrival") != std::string::npos, "day 0 is arrival");
    std::string day11;
    check(course_editor::dayJson(11, day11, error), "day 11 readable");
    check(day11.find("Departure") != std::string::npos, "day 11 is departure");
    std::string day12;
    std::string day13;
    check(course_editor::dayJson(12, day12, error), "day 12 readable");
    check(course_editor::dayJson(13, day13, error), "day 13 readable");
    check(day12.find("Service breakfast") != std::string::npos, "day 12 is service");
    check(day13.find("Service breakfast") != std::string::npos, "day 13 is service");
}

void testEventEditing() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "course published");

    check(course_editor::saveDay(12, kOneEventDay, error), "day 12 replaced");
    std::string day12;
    std::string day13;
    check(course_editor::dayJson(12, day12, error), "day 12 readable");
    check(course_editor::dayJson(13, day13, error), "day 13 readable");
    check(day12.find("Wake-up") != std::string::npos, "day 12 edited");
    check(day13.find("Service breakfast") != std::string::npos, "day 13 untouched");

    check(course_editor::clearDay(12, error), "day 12 cleared");
    check(course_editor::dayJson(12, day12, error), "day 12 readable after clear");
    check(day12 == "[]", "cleared day is empty");

    check(!course_editor::saveDay(12, "{not json", error), "broken JSON rejected");
    check(course_editor::dayJson(12, day12, error) && day12 == "[]",
          "failed save leaves the day unchanged");
    check(!course_editor::dayJson(14, day12, error), "day index out of range rejected");
}

void testInsertAndDelete() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "course published");
    const course_store::CalendarState before = calendar();

    check(course_editor::insertDay(1, false, error), "insert empty day before 1");
    course_editor::Info info;
    check(course_editor::info(info, error), "info after insert");
    check(info.length == 15, "length grew to 15");
    check(info.activeGeneration == before.active_generation, "active generation untouched");
    check(info.draftGeneration != before.active_generation, "draft generation differs");
    check(info.draftDirty, "draft marked dirty");

    std::string inserted;
    check(course_editor::dayJson(1, inserted, error), "inserted day readable");
    check(inserted == "[]", "inserted day is empty");
    std::string shifted;
    check(course_editor::dayJson(2, shifted, error), "old day 1 moved to 2");
    check(shifted.find("Wake-up") != std::string::npos, "old day 1 kept its events");

    check(course_editor::deleteDay(0, error), "delete day 0");
    check(course_editor::info(info, error), "info after delete");
    check(info.length == 14, "length back to 14");
    std::string day0;
    check(course_editor::dayJson(0, day0, error), "day 0 readable");
    check(day0 == "[]", "inserted empty day became day 0");
    std::string day1;
    check(course_editor::dayJson(1, day1, error), "day 1 readable");
    check(day1.find("Wake-up") != std::string::npos, "old day 1 became day 1");

    check(course_editor::insertDay(5, true, error), "copy insert before 5");
    std::string copy;
    std::string original;
    check(course_editor::dayJson(5, copy, error), "copy day readable");
    check(course_editor::dayJson(6, original, error), "source day readable");
    check(copy == original, "copy duplicates the source day");
}

void testLastDayAndPresets() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "course published");

    check(course_editor::setPreset(1, error), "preset 1 day");
    course_editor::Info info;
    check(course_editor::info(info, error), "info after preset 1");
    check(info.length == 1, "length 1");
    check(!course_editor::deleteDay(0, error), "last day cannot be deleted");

    check(course_editor::setPreset(14, error), "preset 14 days");
    check(course_editor::info(info, error), "info after preset 14");
    check(info.length == 14, "length 14");
    std::string day12;
    std::string day13;
    check(course_editor::dayJson(12, day12, error), "preset day 12 readable");
    check(course_editor::dayJson(13, day13, error), "preset day 13 readable");
    check(day12.find("Service breakfast") != std::string::npos, "preset day 12 service");
    check(day13.find("Service breakfast") != std::string::npos, "preset day 13 service");
    check(course_editor::saveDay(12, kOneEventDay, error), "edit preset day 12");
    check(course_editor::dayJson(13, day13, error), "preset day 13 still readable");
    check(day13.find("Service breakfast") != std::string::npos, "day 13 independent");

    check(course_editor::setPreset(47, error), "preset 47 days");
    check(course_editor::info(info, error), "info after preset 47");
    check(info.length == 47, "length 47");
    check(!course_editor::insertDay(0, false, error), "cannot exceed 47 days");

    check(course_editor::setPreset(3, error), "preset 3 days");
    check(course_editor::info(info, error), "info after preset 3");
    check(info.length == 3, "length 3");
}

void testApplyAndDiscard() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "course published");
    const course_store::CalendarState before = calendar();

    check(course_editor::setPreset(3, error), "draft preset 3 days");
    course_editor::Info info;
    check(course_editor::info(info, error), "info after draft");
    check(info.activeGeneration == before.active_generation, "active untouched by draft");
    check(info.length == 3, "info length follows the draft being edited");

    check(course_editor::applyDraft(error), "apply draft");
    const course_store::CalendarState after = calendar();
    check(after.active_generation == info.draftGeneration, "active switched to draft");
    check(after.draft_generation == after.active_generation, "draft equals active after apply");
    check(after.course_length == 3, "applied length 3");
    check(after.flags == course_store::kApplied, "apply clears RUNNING");
    check(after.activation_date == 0, "apply clears activation date");
    check(after.day_index == 0, "apply resets day index");

    check(course_editor::info(info, error), "info after apply");
    check(!info.draftDirty, "draft clean after apply");
    check(info.length == 3, "active length 3");

    check(course_editor::saveDay(0, kOneEventDay, error), "edit after apply");
    const course_store::CalendarState edited = calendar();
    check(edited.active_generation == after.active_generation, "active generation kept");
    check(edited.draft_generation != edited.active_generation, "copy-on-write draft created");
    check(loadDay(edited.active_generation, 0).events.empty(),
          "active day 0 still empty after draft edit");
    check(loadDay(edited.draft_generation, 0).events.size() == 1,
          "draft day 0 has the edit");

    check(course_editor::discardDraft(error), "discard draft");
    const course_store::CalendarState discarded = calendar();
    check(discarded.draft_generation == discarded.active_generation,
          "draft returned to active");
    std::string day0;
    check(course_editor::dayJson(0, day0, error), "day 0 readable after discard");
    check(day0 == "[]", "discard restores the active day");
}

void testRenameAndImport() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "course published");
    check(course_editor::renameCourse("Renamed", error), "rename draft");
    course_editor::Info info;
    check(course_editor::info(info, error), "info after rename");
    check(info.name == "Renamed", "name updated");

    const std::string v1 =
        "{\"format\":\"gong-course\",\"version\":1,\"name\":\"v1 course\","
        "\"days\":[[{\"id\":1,\"hour\":4,\"min\":0,\"track\":1,\"loop\":1,"
        "\"vol\":5,\"en\":true,\"desc\":\"V1\"}]]}";
    check(course_editor::importCourse(v1, error), "v1 import into draft");
    check(course_editor::info(info, error), "info after v1 import");
    check(info.length == 1, "import stays in draft");
    check(course_editor::applyDraft(error), "apply v1 import");
    check(course_editor::info(info, error), "info after applying v1");
    check(info.length == 1, "v1 course length 1");
    check(info.name == "v1 course", "v1 name imported");

    const std::string v2 =
        "{\"format\":\"gong-course\",\"version\":2,\"name\":\"v2 course\","
        "\"course_length\":2,\"days\":[{\"events\":[{\"id\":1,\"hour\":4,"
        "\"min\":0,\"track\":1,\"loop\":1,\"vol\":5,\"en\":true,"
        "\"desc\":\"V2\"}]},{\"events\":[]}]}";
    check(course_editor::importCourse(v2, error), "v2 import into draft");
    check(course_editor::applyDraft(error), "apply v2 import");
    check(course_editor::info(info, error), "info after applying v2");
    check(info.length == 2, "v2 course length 2");
    check(info.name == "v2 course", "v2 name imported");

    const std::string bad = "{\"format\":\"gong-course\",\"version\":2,\"days\":[]}";
    check(!course_editor::importCourse(bad, error), "empty import rejected");
}

void testExport() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "course published");
    check(course_editor::beginExport(course_editor::kExportDraft, error), "export begins");
    std::string document;
    bool done = false;
    int chunks = 0;
    while (!done) {
        std::string chunk;
        if (!course_editor::nextExportChunk(chunk, done, error)) {
            check(false, "export chunk failed: " + error);
            break;
        }
        document += chunk;
        ++chunks;
        if (chunks > 200) {
            check(false, "export did not terminate");
            break;
        }
    }
    check(chunks > 1, "export streams in more than one chunk");
    check(document.find("\"version\":2") != std::string::npos, "export is v2");
    check(document.find("\"course_length\":14") != std::string::npos, "export length 14");
    check(document.find("Arrival") != std::string::npos, "export contains arrival");

    course::Course parsed;
    const bool parsedOk = course::fromJson(document, parsed, error);
    check(parsedOk, "exported document parses: " + error);
    if (!parsedOk) std::cout << "  document: " << document.substr(0, 300) << "\n";
    check(parsed.days.size() == 14, "exported document has 14 days");
}

std::string generationPath(std::uint32_t generation, const char* name) {
    return "/course/g" + std::to_string(generation) + "/" + name;
}

const char* kOtherDay =
    "[{\"id\":2,\"hour\":5,\"min\":30,\"track\":1,\"loop\":1,\"vol\":10,"
    "\"en\":true,\"desc\":\"Other\"}]";

void testIndexGuards() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "course published");
    course_editor::Info info;
    check(course_editor::info(info, error), "info after publish");

    check(!course_editor::insertDay(15, false, error), "insert past the end rejected");
    check(!course_editor::deleteDay(14, error), "delete past the end rejected");
    check(!course_editor::clearDay(14, error), "clear past the end rejected");
    check(!course_editor::saveDay(14, kOneEventDay, error), "save past the end rejected");

    check(course_editor::insertDay(14, false, error), "insert at the end allowed");
    check(course_editor::info(info, error), "info after append");
    check(info.length == 15, "appended day grows the course");
    std::string appended;
    check(course_editor::dayJson(14, appended, error), "appended day readable");
    check(appended == "[]", "appended day is empty");
}

// A LittleFS write failure inside a structural edit must not move the draft
// pointer, must not touch the active generation and must leave the course
// readable.
void testStructuralWriteFailure() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "course published");
    check(course_editor::saveDay(12, kOneEventDay, error), "draft created by a day edit");
    const course_store::CalendarState before = calendar();
    std::string dayBefore;
    check(course_editor::dayJson(0, dayBefore, error), "day 0 readable before fault");

    host::fs().failWrite = true;
    host::fs().path = generationPath(before.active_generation + 2, "meta.tmp");
    const bool inserted = course_editor::insertDay(0, false, error);
    host::fs().failWrite = false;
    host::fs().path.clear();
    check(!inserted, "insert reports the failed meta write");
    check(!error.empty(), "insert keeps the storage error text");

    const course_store::CalendarState after = calendar();
    check(after.active_generation == before.active_generation, "active generation untouched");
    check(after.draft_generation == before.draft_generation, "draft pointer untouched");
    check(after.course_length == before.course_length, "calendar length untouched");
    std::string dayAfter;
    check(course_editor::dayJson(0, dayAfter, error), "day 0 still readable");
    check(dayAfter == dayBefore, "day 0 unchanged by the failed insert");
}

// A short write while replacing one draft day leaves the previous day intact.
void testShortWriteKeepsDay() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "course published");
    check(course_editor::saveDay(5, kOneEventDay, error), "draft day 5 written");
    const course_store::CalendarState before = calendar();
    check(before.draft_generation != before.active_generation, "draft generation exists");

    host::fs().shortWrite = true;
    host::fs().path = generationPath(before.draft_generation, "d05.tmp");
    const bool saved = course_editor::saveDay(5, kOtherDay, error);
    host::fs().shortWrite = false;
    host::fs().path.clear();
    check(!saved, "short write rejected");
    check(!error.empty(), "short write reports an error");

    std::string day5;
    check(course_editor::dayJson(5, day5, error), "day 5 readable after the fault");
    check(day5.find("Wake-up") != std::string::npos, "day 5 keeps its previous events");
    check(day5.find("Other") == std::string::npos, "partial write did not reach the day");
    const course_store::CalendarState after = calendar();
    check(after.active_generation == before.active_generation, "active generation untouched");
    check(after.draft_generation == before.draft_generation, "draft pointer untouched");
}

// Apply must fail closed when the calendar write fails: no partial publication.
void testApplyNvsFailure() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "course published");
    const course_store::CalendarState before = calendar();
    check(course_editor::setPreset(3, error), "draft preset 3 days");
    course_editor::Info draft;
    check(course_editor::info(draft, error), "info after draft");

    host::nvs().inject("set", "course_state", host::Action::ErrorBefore);
    const bool applied = course_editor::applyDraft(error);
    host::nvs().fault.armed = false;
    check(!applied, "apply fails when the calendar write fails");
    check(!error.empty(), "apply reports the NVS error");

    const course_store::CalendarState after = calendar();
    check(after.active_generation == before.active_generation, "active generation not switched");
    check(after.flags == course_store::kApplied, "binding stays APPLIED");
    check(after.course_length == before.course_length, "active length unchanged");
    course_editor::Info kept;
    check(course_editor::info(kept, error), "draft still readable after the failure");
    check(kept.draftGeneration == draft.draftGeneration, "draft generation preserved");
    check(kept.length == 3, "draft keeps its 3 days");
}

// A damaged draft generation must block editing and Apply instead of being
// silently replaced by the active generation.
void testCorruptDraftGeneration() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "course published");
    check(course_editor::saveDay(0, kOneEventDay, error), "draft created");
    const course_store::CalendarState before = calendar();
    check(before.draft_generation != before.active_generation, "draft generation exists");

    const std::string meta = generationPath(before.draft_generation, "meta.json");
    check(host::files().count(meta) == 1, "draft meta file exists");
    host::files()[meta] = "{\"corrupt\":true}";

    course_editor::Info info;
    check(!course_editor::info(info, error), "info fails on a corrupt draft meta");
    check(!error.empty(), "info reports the read error");
    check(!course_editor::applyDraft(error), "apply refuses a corrupt draft");
    std::string day;
    check(!course_editor::dayJson(0, day, error), "day read refused on a corrupt draft");

    const course_store::CalendarState after = calendar();
    check(after.active_generation == before.active_generation, "active generation untouched");
    check(after.draft_generation == before.draft_generation, "draft pointer untouched");
}

// A day disappearing mid-export must surface an error, not loop forever and
// not modify the course.
void testExportFault() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "course published");
    course_editor::Info info;
    check(course_editor::info(info, error), "info after publish");
    check(course_editor::beginExport(course_editor::kExportDraft, error), "export begins");

    std::string chunk;
    bool done = false;
    check(course_editor::nextExportChunk(chunk, done, error), "first export chunk");
    host::files().erase(generationPath(info.activeGeneration, "d13.json"));

    bool failed = false;
    int chunks = 0;
    while (!done && !failed && chunks < 60) {
        if (!course_editor::nextExportChunk(chunk, done, error)) failed = true;
        ++chunks;
    }
    check(failed, "missing day aborts the export");
    check(!error.empty(), "export reports the missing day");
    check(!done, "aborted export is not reported as complete");

    host::files()[generationPath(info.activeGeneration, "d13.json")] = "[]";
    course_editor::Info after;
    check(course_editor::info(after, error), "course still readable after the aborted export");
    check(after.activeGeneration == info.activeGeneration, "export did not change the course");
    check(course_editor::beginExport(course_editor::kExportDraft, error), "a new export can start after an abort");
}

bool hasCalendar() {
    course_store::CourseStore store;
    return store.hasCalendar();
}

std::uint32_t bootstrapGeneration() {
    course_store::CourseStore store;
    return store.loadBootstrapGeneration();
}

bool calendarKeyPresent() {
    return host::nvs().visible.count("gong/course_state") != 0;
}

// Clean install: no course_state record at all (only legacy dayNN.conf may be
// present on the device). The editor must still be usable.
void testBootstrapWithoutCourse() {
    resetHost();
    std::string error;
    course_editor::Info info;
    check(course_editor::info(info, error), "info succeeds without a course");
    check(!info.hasCourse, "no published course yet");
    check(info.draftGeneration == 0, "no draft generation yet");
    check(info.length == 0, "no length yet");

    std::string day;
    check(!course_editor::dayJson(0, day, error), "day read needs a draft or course");
    check(!course_editor::saveDay(0, kOneEventDay, error), "day write needs a course");
    check(!course_editor::insertDay(0, false, error), "insert needs a course");
    check(!course_editor::deleteDay(0, error), "delete needs a course");
    check(!course_editor::applyDraft(error), "apply needs a draft");
    check(!course_editor::discardDraft(error), "discard needs a draft");
    check(!hasCalendar(), "failed actions created no calendar");
    check(!calendarKeyPresent(), "failed actions wrote no course_state key");

    check(course_editor::setPreset(14, error), "preset 14 works without a course");
    check(!hasCalendar(), "preset does not publish a calendar");
    check(!calendarKeyPresent(), "preset writes no course_state key");
    check(bootstrapGeneration() != 0, "preset records the bootstrap draft");

    check(course_editor::info(info, error), "info after bootstrap preset");
    check(!info.hasCourse, "bootstrap draft is not a published course");
    check(info.draftGeneration == bootstrapGeneration(), "draft generation reported");
    check(info.length == 14, "bootstrap preset has 14 days");
    check(info.draftDirty, "bootstrap draft counts as dirty");

    std::string day0;
    std::string day13;
    check(course_editor::dayJson(0, day0, error), "bootstrap day 0 readable");
    check(day0.find("Arrival") != std::string::npos, "bootstrap day 0 is arrival");
    check(course_editor::dayJson(13, day13, error), "bootstrap day 13 readable");
    check(day13.find("Service breakfast") != std::string::npos,
          "bootstrap day 13 is the service copy");

    check(course_editor::saveDay(0, kOneEventDay, error), "bootstrap day editable");
    check(course_editor::dayJson(0, day0, error) && day0.find("Wake-up") != std::string::npos,
          "bootstrap day 0 edit stored");
    check(course_editor::insertDay(1, false, error), "bootstrap insert works");
    check(course_editor::info(info, error) && info.length == 15, "bootstrap insert grew the draft");
    check(course_editor::deleteDay(1, error), "bootstrap delete works");
    check(course_editor::info(info, error) && info.length == 14, "bootstrap delete shrank the draft");
    check(course_editor::renameCourse("Bootstrap", error), "bootstrap rename works");
    check(course_editor::info(info, error) && info.name == "Bootstrap", "bootstrap name stored");
    check(!hasCalendar(), "editing the bootstrap draft never publishes a calendar");
}

// The first Apply publishes the bootstrap generation as the active course with
// an APPLIED binding, and the draft pointer stays consistent across a reboot.
void testBootstrapApply() {
    resetHost();
    std::string error;
    check(course_editor::setPreset(3, error), "bootstrap preset 3");
    const std::uint32_t draft = bootstrapGeneration();
    check(draft != 0, "bootstrap generation recorded");

    check(course_editor::applyDraft(error), "first apply publishes the draft");
    check(hasCalendar(), "apply creates the calendar");
    check(calendarKeyPresent(), "apply writes course_state");
    check(bootstrapGeneration() == 0, "bootstrap pointer cleared after apply");

    const course_store::CalendarState after = calendar();
    check(after.active_generation == draft, "published generation is the bootstrap draft");
    check(after.draft_generation == draft, "draft equals active after apply");
    check(after.course_length == 3, "published length 3");
    check(after.flags == course_store::kApplied, "binding is APPLIED, not RUNNING");
    check(after.activation_date == 0, "no activation date after publish");
    check(after.day_index == 0, "day index reset after publish");

    course_editor::Info info;
    check(course_editor::info(info, error), "info after first apply");
    check(info.hasCourse, "course present after first apply");
    check(!info.draftDirty, "draft clean after first apply");
    check(info.length == 3, "active length 3");

    // Reboot: only the NVS durable copy and LittleFS survive.
    host::nvs().reboot();
    check(course_editor::info(info, error), "info survives a reboot");
    check(info.hasCourse && info.length == 3, "course survives a reboot");
    std::string day0;
    check(course_editor::dayJson(0, day0, error), "day readable after reboot");
    check(hasCalendar(), "calendar still present after reboot");

    // A second apply of the already published course keeps the binding APPLIED.
    check(course_editor::applyDraft(error), "second apply is accepted");
    const course_store::CalendarState second = calendar();
    check(second.active_generation == draft, "second apply keeps the generation");
    check(second.flags == course_store::kApplied, "second apply keeps APPLIED");
}

// A clean install must be able to import a course before the first publish.
void testBootstrapImport() {
    resetHost();
    std::string error;
    const std::string v1 =
        "{\"format\":\"gong-course\",\"version\":1,\"name\":\"clean import\","
        "\"days\":[[{\"id\":1,\"hour\":4,\"min\":0,\"track\":1,\"loop\":1,"
        "\"vol\":5,\"en\":true,\"desc\":\"V1\"}]]}";
    check(course_editor::importCourse(v1, error), "import works without a course");
    check(!hasCalendar(), "import alone does not publish");
    course_editor::Info info;
    check(course_editor::info(info, error), "info after import");
    check(!info.hasCourse && info.length == 1, "imported draft is unpublished, 1 day");
    check(info.name == "clean import", "imported name kept");

    check(course_editor::applyDraft(error), "apply imported draft");
    check(course_editor::info(info, error), "info after apply");
    check(info.hasCourse && info.length == 1, "imported course published");
    check(info.name == "clean import", "published name kept");
    check(calendar().flags == course_store::kApplied, "imported course is APPLIED");
}

// Failures on a clean install must leave no calendar and keep the draft.
void testBootstrapFailures() {
    resetHost();
    std::string error;
    check(course_editor::setPreset(3, error), "bootstrap preset 3");
    const std::uint32_t draft = bootstrapGeneration();

    host::nvs().inject("set", "course_state", host::Action::ErrorBefore);
    check(!course_editor::applyDraft(error), "apply reports a failed calendar write");
    host::nvs().fault.armed = false;
    check(!error.empty(), "apply keeps the NVS error text");
    check(!hasCalendar(), "failed apply created no calendar");
    check(bootstrapGeneration() == draft, "failed apply keeps the bootstrap draft");
    course_editor::Info info;
    check(course_editor::info(info, error), "info after failed apply");
    check(info.length == 3 && !info.hasCourse, "draft still editable after failed apply");
    check(course_editor::applyDraft(error), "apply succeeds once NVS works again");
    check(hasCalendar(), "calendar created on the retry");

    resetHost();
    host::fs().failWrite = true;
    host::fs().path = "/course/g1/meta.tmp";
    check(!course_editor::setPreset(3, error), "preset reports a failed meta write");
    host::fs().failWrite = false;
    host::fs().path.clear();
    check(!hasCalendar(), "failed preset created no calendar");
    check(bootstrapGeneration() == 0, "failed preset recorded no draft");
    check(course_editor::info(info, error) && !info.hasCourse && info.length == 0,
          "failed preset leaves the editor empty");

    resetHost();
    check(course_editor::setPreset(3, error), "bootstrap preset for the corrupt case");
    const std::uint32_t corrupt = bootstrapGeneration();
    host::files()[generationPath(corrupt, "meta.json")] = "{\"corrupt\":true}";
    check(!course_editor::info(info, error), "info fails on a corrupt bootstrap meta");
    check(!course_editor::applyDraft(error), "apply refuses a corrupt bootstrap draft");
    check(!hasCalendar(), "corrupt draft published nothing");

    resetHost();
    check(course_editor::setPreset(3, error), "bootstrap preset for the missing case");
    course_store::CourseStore store;
    check(store.saveBootstrapGeneration(99, error), "pointer moved to a missing generation");
    check(course_editor::info(info, error), "info survives a dangling pointer");
    check(!info.hasCourse && info.draftGeneration == 0, "dangling pointer reports no draft");
    check(!course_editor::applyDraft(error), "apply refuses a dangling pointer");
}

void testBootstrapDiscardAndSeedGuard() {
    resetHost();
    std::string error;
    check(course_editor::setPreset(3, error), "bootstrap preset 3");
    const std::uint32_t draft = bootstrapGeneration();
    check(host::files().count(generationPath(draft, "meta.json")) == 1,
          "bootstrap generation written");

    check(!course_editor::ensureInitialCourse(error), "auto-seed refuses a pending draft");
    check(!hasCalendar(), "auto-seed did not publish over the draft");
    course_editor::Info info;
    check(course_editor::info(info, error), "draft survives the refused auto-seed");
    check(info.draftGeneration == draft && info.length == 3, "draft unchanged");

    check(course_editor::discardDraft(error), "bootstrap draft discarded");
    check(!hasCalendar(), "discard created no calendar");
    check(bootstrapGeneration() == 0, "discard cleared the pointer");
    check(host::files().count(generationPath(draft, "meta.json")) == 0,
          "discard removed the generation files");
    check(course_editor::info(info, error) && !info.hasCourse && info.draftGeneration == 0,
          "editor empty after discard");
    check(!course_editor::discardDraft(error), "second discard reports nothing to drop");
}

// Every preset the GUI offers must work on a clean install.
void testBootstrapPresets() {
    const int lengths[] = {3, 10, 14, 20, 30, 45};
    for (int length : lengths) {
        resetHost();
        std::string error;
        check(course_editor::setPreset(static_cast<std::uint8_t>(length), error),
              "bootstrap preset " + std::to_string(length) + " failed: " + error);
        course_editor::Info info;
        check(course_editor::info(info, error), "info after bootstrap preset");
        check(info.length == length, "bootstrap preset length " + std::to_string(length));
        check(!info.hasCourse, "bootstrap preset did not publish");
        check(!hasCalendar(), "bootstrap preset wrote a calendar");
        check(course_editor::applyDraft(error), "apply bootstrap preset " + std::to_string(length));
        check(course_editor::info(info, error) && info.hasCourse && info.length == length,
              "published preset length " + std::to_string(length));
        check(calendar().flags == course_store::kApplied, "published preset is APPLIED");
    }
}

// The bootstrap pointer must live under a key NVS accepts (<= 15 characters)
// and a device write error must surface instead of being swallowed.
void testBootstrapNvsKey() {
    resetHost();
    std::string error;
    host::nvs().inject("set", "crs_boot", host::Action::ErrorBefore);
    check(!course_editor::setPreset(14, error), "preset reports a pointer write failure");
    check(!error.empty(), "preset keeps the NVS error text");
    check(bootstrapGeneration() == 0, "failed pointer write stored nothing");
    check(!hasCalendar(), "failed preset published no calendar");

    resetHost();
    check(course_editor::setPreset(14, error), "preset succeeds with the short key");
    check(bootstrapGeneration() != 0, "pointer recorded");
    check(host::nvs().visible.count("gong/crs_boot") == 1, "pointer stored under crs_boot");
    check(host::nvs().visible.count("gong/course_bootstrap") == 0,
          "the rejected 16-character key is never used");
    check(course_editor::applyDraft(error), "apply publishes the preset");
    check(hasCalendar(), "published after apply");
    check(host::nvs().visible.count("gong/crs_boot") == 0, "pointer cleared after apply");
}

// Minimal v1 course JSON with `days` calendar days, one event each.
std::string daysJsonV1(int days, const std::string& name) {
    std::string json = "{\"format\":\"gong-course\",\"version\":1,\"name\":\"" + name + "\",\"days\":[";
    for (int i = 0; i < days; ++i) {
        if (i) json += ',';
        json += "[{\"id\":1,\"hour\":4,\"min\":" + std::to_string(i % 60) +
                ",\"track\":1,\"loop\":1,\"vol\":5,\"en\":true,\"desc\":\"d" +
                std::to_string(i) + "\"}]";
    }
    json += "]}";
    return json;
}

std::string exportToString(course_editor::ExportSource source, std::string& error,
                           bool& ok) {
    ok = course_editor::beginExport(source, error);
    std::string document;
    bool done = false;
    int guard = 0;
    while (ok && !done && guard++ < 80) {
        std::string chunk;
        if (!course_editor::nextExportChunk(chunk, done, error)) { ok = false; break; }
        document += chunk;
    }
    return document;
}

// Validation is the only thing the GUI does before the operator presses Apply,
// so it must accept v1/v2, report the shape and never touch storage.
void testInspectCourse() {
    resetHost();
    course_editor::Inspection inspection;
    std::string error;

    const std::string v1 = daysJsonV1(10, "v1 ten");
    check(course_editor::inspectCourse(v1, inspection, error), "v1 JSON accepted");
    check(inspection.version == 1, "v1 version reported");
    check(inspection.length == 10, "v1 length comes from the days array");
    check(inspection.events == 10, "v1 events counted");
    check(inspection.name == "v1 ten", "v1 name reported");

    const std::string v2 =
        "{\"format\":\"gong-course\",\"version\":2,\"name\":\"v2 three\",\"course_length\":3,"
        "\"templates\":{\"t\":[{\"id\":1,\"hour\":4,\"min\":0,\"track\":1,\"loop\":1,"
        "\"vol\":5,\"en\":true,\"desc\":\"t\"}]},"
        "\"days\":[{\"template\":\"t\"},{\"template\":\"t\"},{\"events\":[]}]}";
    check(course_editor::inspectCourse(v2, inspection, error), "v2 JSON accepted");
    check(inspection.version == 2, "v2 version reported");
    check(inspection.length == 3, "v2 length from the days array");
    check(inspection.events == 2, "v2 template events resolved and counted");
    check(inspection.name == "v2 three", "v2 name reported");

    check(!course_editor::inspectCourse("not json", inspection, error), "broken JSON rejected");
    check(!error.empty(), "broken JSON reports an error");
    check(!course_editor::inspectCourse("{\"format\":\"other\",\"version\":1,\"days\":[[]]}",
                                        inspection, error),
          "foreign format rejected");
    check(!course_editor::inspectCourse(
              "{\"format\":\"gong-course\",\"version\":3,\"days\":[[]]}", inspection, error),
          "unknown version rejected");
    check(!course_editor::inspectCourse(
              "{\"format\":\"gong-course\",\"version\":1,\"days\":[]}", inspection, error),
          "empty course rejected");

    std::string many = "{\"format\":\"gong-course\",\"version\":1,\"days\":[";
    for (int i = 0; i < 48; ++i) { if (i) many += ','; many += "[]"; }
    many += "]}";
    check(!course_editor::inspectCourse(many, inspection, error), "48 days rejected");

    check(!hasCalendar(), "validation wrote no calendar");
    check(bootstrapGeneration() == 0, "validation wrote no draft");
    check(host::files().empty(), "validation wrote no files");
}

// The imported JSON decides the course length: 1, 10, 14, 20 and 47 days, with
// a different length replacing the previous course.
void testImportReplacesCourse() {
    const int lengths[] = {1, 10, 14, 20, 47};
    for (int length : lengths) {
        resetHost();
        std::string error;
        check(course_editor::ensureInitialCourse(error), "reference course published");

        check(course_editor::importCourse(daysJsonV1(length, "len" + std::to_string(length)), error),
              "import " + std::to_string(length) + " days");
        course_editor::Info info;
        check(course_editor::info(info, error), "info after import");
        check(info.length == length, "draft length " + std::to_string(length));
        const course_store::CalendarState before = calendar();
        check(before.course_length == 14, "active course unchanged before Apply");

        check(course_editor::applyDraft(error), "apply " + std::to_string(length) + " days");
        const course_store::CalendarState after = calendar();
        check(after.course_length == length, "published length " + std::to_string(length));
        check(after.flags == course_store::kApplied, "replacement is APPLIED");
        check(after.activation_date == 0, "replacement resets the activation date");
        check(after.day_index == 0, "replacement resets the day index");
        std::string day;
        check(course_editor::dayJson(0, day, error), "day 0 readable after apply");
        check(day.find("\"d0\"") != std::string::npos, "day 0 content from the file");
        std::string last;
        check(course_editor::dayJson(static_cast<std::uint8_t>(length - 1), last, error),
              "last day readable");
    }

    // v2 files with templates import the same way.
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "reference course for v2");
    std::string jsonError;
    const std::string v2 = course::toJsonV2(course::makeReferenceCourse(), false, jsonError);
    check(!v2.empty(), "v2 reference document built");
    check(course_editor::importCourse(v2, error), "v2 import with templates");
    check(course_editor::applyDraft(error), "v2 apply");
    check(calendar().course_length == 14, "v2 course published with 14 days");

    // v1 with a shared template name per day (the original operator format).
    course_editor::Inspection inspection;
    const std::string v1t =
        "{\"format\":\"gong-course\",\"version\":1,\"name\":\"v1 templates\","
        "\"templates\":{\"p\":[{\"id\":1,\"hour\":5,\"min\":0,\"track\":1,\"loop\":1,"
        "\"vol\":10,\"en\":true,\"desc\":\"Практика\"}]},"
        "\"days\":[\"p\",\"p\",[]]}";
    check(course_editor::inspectCourse(v1t, inspection, error), "v1 template JSON accepted");
    check(inspection.version == 1 && inspection.length == 3, "v1 template shape");
    check(inspection.events == 2, "v1 template events counted per reference");
    check(course_editor::importCourse(v1t, error), "v1 template import works");
    check(course_editor::applyDraft(error), "v1 template apply works");
    std::string tplDay;
    check(course_editor::dayJson(0, tplDay, error), "v1 template day readable");
    check(tplDay.find("Практика") != std::string::npos, "v1 template day keeps its events");

    // A second import replaces the pending draft instead of stacking drafts.
    course_editor::Info info;
    check(course_editor::importCourse(daysJsonV1(5, "second"), error), "second import");
    check(course_editor::info(info, error), "info after second import");
    check(info.length == 5, "second import replaced the draft");
}

// Export: the operator gets the published course by default, and can ask for
// the pending draft explicitly.
void testExportSource() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "reference course published");
    check(course_editor::importCourse(daysJsonV1(3, "draft3"), error), "3-day draft imported");

    bool ok = false;
    const std::string active = exportToString(course_editor::kExportActive, error, ok);
    check(ok, "active export completes");
    check(active.find("\"course_length\":14") != std::string::npos,
          "active export contains the published 14-day course");
    check(active.find("\"draft3\"") == std::string::npos,
          "active export is not the pending draft");

    const std::string draft = exportToString(course_editor::kExportDraft, error, ok);
    check(ok, "draft export completes");
    check(draft.find("\"course_length\":3") != std::string::npos,
          "draft export contains the 3-day draft");
}

// Operator-sized course: `days` days, `perDay` events each, Cyrillic text.
std::string bigCourseJsonV2(int days, int perDay, const std::string& name) {
    std::string json;
    json.reserve(static_cast<std::size_t>(days) * 24 + static_cast<std::size_t>(perDay) * 150 + 512);
    json += "{\"format\":\"gong-course\",\"version\":2,\"name\":\"";
    json += name;
    json += "\",\"days\":[";
    char buf[256];
    for (int d = 0; d < days; ++d) {
        if (d) json += ',';
        json += "{\"events\":[";
        for (int e = 0; e < perDay; ++e) {
            if (e) json += ',';
            std::snprintf(buf, sizeof(buf),
                          "{\"id\":%d,\"hour\":%d,\"min\":%d,\"track\":1,\"loop\":1,\"vol\":20,"
                          "\"en\":true,\"desc\":\"Практика: утренний гонг номер %d — тишина и внимание\"}",
                          e + 1, 5 + (e / 60) % 18, e % 60, e + 1);
            json += buf;
        }
        json += "]}";
    }
    json += "],\"course_length\":";
    json += std::to_string(days);
    json += "}";
    return json;
}

std::string bigCourseJsonV1(int days, int perDay, const std::string& name) {
    std::string json;
    json.reserve(static_cast<std::size_t>(days) * static_cast<std::size_t>(perDay) * 130 + 512);
    json += "{\"format\":\"gong-course\",\"version\":1,\"name\":\"";
    json += name;
    json += "\",\"days\":[";
    char buf[256];
    for (int d = 0; d < days; ++d) {
        if (d) json += ',';
        json += '[';
        for (int e = 0; e < perDay; ++e) {
            if (e) json += ',';
            std::snprintf(buf, sizeof(buf),
                          "{\"id\":%d,\"hour\":%d,\"min\":%d,\"track\":1,\"loop\":1,\"vol\":20,"
                          "\"en\":true,\"desc\":\"День %d, гонг %d — практика\"}",
                          e + 1, 4 + (e / 60) % 18, e % 60, d, e + 1);
            json += buf;
        }
        json += ']';
    }
    json += "]}";
    return json;
}

// A real operator file (hundreds of events, Cyrillic) must validate, import and
// publish through the bounded path, and survive a reboot.
void testOperatorSizedCourse() {
    resetHost();
    std::string error;
    const std::string v2 = bigCourseJsonV2(8, 32, "Курс на 8 дней");
    check(v2.size() > 30000, "operator v2 file is realistically large");
    course_editor::Inspection inspection;
    check(course_editor::inspectCourse(v2, inspection, error), "big v2 validates: " + error);
    check(inspection.length == 8, "big v2 reports 8 days");
    check(inspection.events == 256, "big v2 reports 256 events");
    check(inspection.version == 2, "big v2 reports format 2");
    check(inspection.name == "Курс на 8 дней", "big v2 keeps the Cyrillic name");

    check(course_editor::importCourse(v2, error), "big v2 imports: " + error);
    check(course_editor::applyDraft(error), "big v2 applies: " + error);
    course_editor::Info info;
    check(course_editor::info(info, error), "info after big apply");
    check(info.hasCourse && info.length == 8, "big v2 published with 8 days");
    check(calendar().flags == course_store::kApplied, "big v2 is APPLIED");

    std::string day;
    check(course_editor::dayJson(7, day, error), "last day readable");
    check(day.find("Практика") != std::string::npos, "Cyrillic description stored");
    check(day.find("\\u") == std::string::npos, "descriptions are stored as UTF-8, not escapes");

    // Reboot: only NVS and LittleFS survive.
    host::nvs().reboot();
    check(course_editor::info(info, error), "info survives the reboot");
    check(info.hasCourse && info.length == 8, "course survives the reboot");
    check(calendar().flags == course_store::kApplied, "still APPLIED after reboot");

    resetHost();
    const std::string v1 = bigCourseJsonV1(10, 12, "Курс v1");
    check(course_editor::inspectCourse(v1, inspection, error), "big v1 validates: " + error);
    check(inspection.version == 1 && inspection.length == 10, "big v1 shape");
    check(course_editor::importCourse(v1, error), "big v1 imports");
    check(course_editor::applyDraft(error), "big v1 applies");
    check(course_editor::info(info, error) && info.length == 10, "big v1 published");
}

// The device has no PSRAM: the bounded path must stay well under a small heap,
// while the whole-document tree path (the old crash) cannot.
void testHeapBudget() {
    resetHost();
    std::string error;
    const std::string big = bigCourseJsonV2(8, 32, "Курс на 8 дней");
    course_editor::Inspection inspection;

#ifndef HOST_SANITIZED
    // Control: the pre-fix path needs a whole-document tree and dies under the
    // budget a no-PSRAM ESP32 can offer.
    host::heapBudgetReset(96 * 1024);
    bool treeThrew = false;
    try {
        course::Course parsed;
        std::string treeError;
        (void)course::fromJson(big, parsed, treeError);
    } catch (const std::bad_alloc&) {
        treeThrew = true;
    }
    check(treeThrew, "whole-document parse exceeds a device-sized heap (old crash path)");

    // The bounded path validates the same file without the tree.
    host::heapBudgetReset(96 * 1024);
    const bool inspected =
        course_editor::inspectCourse(big, inspection, error);
    const std::size_t peak = host::heapBudget().peak;
    std::cout << "  heap peak for validation: " << (peak / 1024) << " KB of 96 KB\n";
    check(inspected, "bounded validation fits the device-sized heap: " + error);
    check(inspection.events == 256, "bounded validation still counts every event");
    // One day with 32 events is parsed at a time; that working set (one day's
    // tree plus the wrapper) is what the guard below budgets for.
    check(peak < 96 * 1024, "bounded validation stays inside the device-sized heap");

    // Too little memory: a normal error, never an escaping exception.
    host::heapBudgetReset(6 * 1024);
    bool threw = false;
    bool ok = false;
    try {
        ok = course_editor::inspectCourse(big, inspection, error);
    } catch (...) {
        threw = true;
    }
    check(!threw, "tight heap does not throw out of inspectCourse");
    check(!ok, "tight heap is reported as a validation failure");
    check(error.find("memory") != std::string::npos, "error says memory: " + error);
    host::heapBudgetReset(0);
#else
    // A sanitizer runtime owns operator new, so the budget model is not
    // available here; the bounded path itself is still exercised below.
    check(course_editor::inspectCourse(big, inspection, error), "bounded validation with a sanitizer");
    check(inspection.events == 256, "bounded validation counts every event");
#endif

    // Import and Apply on the same file still work (the host FS model keeps the
    // course in RAM, so it is given a realistic budget rather than a tiny one).
    host::heapBudgetReset(1024 * 1024);
    const bool imported = course_editor::importCourse(big, error);
    const bool applied = imported && course_editor::applyDraft(error);
    check(imported, "big import succeeds: " + error);
    check(applied, "big apply succeeds: " + error);
    check(calendar().course_length == 8, "big course published");
    host::heapBudgetReset(0);
}

// A published course must keep using its own draft pointer, never the bootstrap.
void testPublishedCourseIgnoresBootstrap() {
    resetHost();
    std::string error;
    check(course_editor::ensureInitialCourse(error), "reference course published");
    check(course_editor::setPreset(3, error), "draft preset on a published course");
    check(bootstrapGeneration() == 0, "published course never uses the bootstrap key");
    check(course_editor::applyDraft(error), "apply published draft");
    check(bootstrapGeneration() == 0, "apply keeps the bootstrap key unused");
    check(hasCalendar(), "published course still has its calendar");
}

}  // namespace

// An operator-shaped v1 document: unique templates plus calendar references.
std::string compactCourseJsonV1(int days, int templateCount, int perTemplate) {
    std::string doc = "{\"format\":\"gong-course\",\"version\":1,\"name\":\"Gong #2\",\"templates\":{";
    for (int t = 0; t < templateCount; ++t) {
        if (t) doc += ',';
        doc += "\"t" + std::to_string(t) + "\":[";
        for (int e = 0; e < perTemplate; ++e) {
            if (e) doc += ',';
            doc += "{\"id\":" + std::to_string(e + 1) + ",\"hour\":" +
                   std::to_string(5 + e % 18) + ",\"min\":" + std::to_string(e * 5) +
                   ",\"track\":1,\"loop\":1,\"vol\":20,\"en\":true,\"desc\":\"Практика " +
                   std::to_string(t) + "-" + std::to_string(e) + "\"}";
        }
        doc += "]";
    }
    doc += "},\"days\":[";
    for (int d = 0; d < days; ++d) {
        if (d) doc += ',';
        doc += "\"t" + std::to_string(d % templateCount) + "\"";
    }
    doc += "]}";
    return doc;
}

void testCompactImport() {
    resetHost();
    std::string error;
    const std::string doc = compactCourseJsonV1(14, 3, 8);
    check(course_editor::importCourseCompact(doc, error),
          "compact import failed: " + error);

    course_store::CourseStore store;
    const std::uint32_t draft = store.loadBootstrapGeneration();
    check(draft != 0, "bootstrap draft missing");
    course_store::CourseMeta meta;
    check(store.loadMeta(draft, meta, error), "meta: " + error);
    check(meta.layout == course_store::kLayoutTemplates,
          "compact import did not mark the generation as tpl1");
    check(meta.course_length == 14, "compact import length");
    check(host::files().find("/course/g" + std::to_string(draft) + "/templates.json") !=
              host::files().end(),
          "compact import wrote no templates.json");
    for (int d = 0; d < 14; ++d) {
        char name[8];
        std::snprintf(name, sizeof(name), "d%02u", static_cast<unsigned>(d));
        const std::string path =
            "/course/g" + std::to_string(draft) + "/" + name + ".json";
        check(host::files()[path].find("\"events\"") == std::string::npos,
              std::string("day ") + name + " was materialised");
    }

    // Unique vs calendar occurrences: 3 templates x 8 events, 14 references.
    check(store.prepareTemplates(draft, error), "prepare: " + error);
    check(store.cachedTemplateCount() == 3, "unique template count");
    check(store.cachedTemplateEvents() == 24, "unique event count");
    course::Day day;
    check(store.loadDay(draft, 13, day, error), "loadDay: " + error);
    check(day.events.size() == 8, "resolved event count");
    check(day.events[0].description == "Практика 1-0", "wrong template resolved");

    course_store::CalendarState state;
    check(store.applyDraft(draft, state, error), "applyDraft: " + error);
    check(state.flags == course_store::kApplied, "apply must leave the course APPLIED");
    check((state.flags & course_store::kRunning) == 0, "apply must not run the course");
    check(state.activation_date == 0, "apply must not set an activation date");

    // The same document written through the operational import must stay
    // expanded: stage B does not change what the device stores today.
    resetHost();
    check(course_editor::importCourse(doc, error), "operational import failed: " + error);
    course_store::CourseStore store2;
    const std::uint32_t operational = store2.loadBootstrapGeneration();
    check(operational != 0, "operational draft missing");
    course_store::CourseMeta operationalMeta;
    check(store2.loadMeta(operational, operationalMeta, error), "operational meta: " + error);
    check(operationalMeta.layout == course_store::kLayoutExpanded,
          "the operational import started writing tpl1 generations");
    check(host::files().find("/course/g" + std::to_string(operational) +
                             "/templates.json") == host::files().end(),
          "the operational import wrote templates.json");
}

void testCompactImportRejectsExpanded() {
    resetHost();
    std::string error;
    const std::string expanded = bigCourseJsonV2(4, 5, "v2 full events");
    check(!course_editor::importCourseCompact(expanded, error),
          "compact import must refuse a course without references");
    check(error.find("no template references") != std::string::npos,
          "unclear rejection: " + error);
    check(course_store::CourseStore().loadBootstrapGeneration() == 0,
          "a rejected compact import left a generation behind");

    check(course_editor::importCourse(expanded, error), "v2 import failed: " + error);
    course_store::CourseStore store;
    course_store::CourseMeta meta;
    const std::uint32_t draft = store.loadBootstrapGeneration();
    check(store.loadMeta(draft, meta, error), "meta: " + error);
    check(meta.layout == course_store::kLayoutExpanded, "v2 import changed layout");
}

void testCompactImportFailureKeepsCourse() {
    resetHost();
    std::string error;
    // Publish a normal expanded course first.
    check(course_editor::importCourse(bigCourseJsonV2(4, 4, "baseline"), error),
          "baseline import failed: " + error);
    course_store::CourseStore store;
    const std::uint32_t baseline = store.loadBootstrapGeneration();
    check(baseline != 0, "baseline draft missing");
    check(course_editor::applyDraft(error), "apply baseline failed: " + error);
    course_store::CalendarState before;
    check(store.loadCalendar(before, error), "calendar: " + error);

    // The compact import fails while writing the template set.
    // Every write fails from here on, whichever generation the import picks.
    host::fs().path.clear();
    host::fs().failWrite = true;
    const std::string doc = compactCourseJsonV1(6, 2, 4);
    check(!course_editor::importCourseCompact(doc, error),
          "a failed compact import must report an error");
    host::fs().failWrite = false;
    host::fs().path.clear();

    course_store::CalendarState after;
    check(store.loadCalendar(after, error), "calendar after failure: " + error);
    check(after.active_generation == before.active_generation,
          "the published course changed on a failed import");
    check(after.draft_generation == before.draft_generation,
          "the draft pointer changed on a failed import");
    check(after.flags == before.flags, "flags changed on a failed import");
    check(store.validateGeneration(before.active_generation, before.course_length, error),
          "the published course no longer validates: " + error);
}

// Stage C: the editor edits a reference day as its own copy.
void testCopyOnWriteDay() {
    resetHost();
    std::string error;
    course::Course course;
    course.name = "cow course";
    for (int i = 0; i < 3; ++i) {
        course::Event e;
        e.id = static_cast<std::uint32_t>(i + 1);
        e.hour = static_cast<std::uint8_t>(5 + i);
        e.minute = 0;
        e.track = 1;
        e.loop = 1;
        e.volume = 10;
        e.enabled = true;
        e.description = "tpl " + std::to_string(i);
        course.templates["practice"].push_back(e);
    }
    for (int d = 0; d < 4; ++d) {
        course::Day day;
        day.templateName = "practice";
        course.days.push_back(day);
    }
    course_store::CourseStore store;
    std::uint32_t published = 0;
    course_store::CalendarState state;
    check(store.applyCompactCourse(course, published, state, error),
          "compact apply failed: " + error);

    // Reading a reference day resolves it through the generation's templates,
    // even when the cache starts empty (fresh boot).
    store.invalidateTemplates();
    std::string json;
    check(course_editor::dayJson(1, json, error), "dayJson failed: " + error);
    check(json.find("\"hour\":6") != std::string::npos,
          "dayJson did not resolve the shared template");

    // Editing writes an individual copy; the active generation stays intact.
    const std::string edited =
        "[{\"id\":9,\"hour\":9,\"min\":15,\"track\":1,\"loop\":1,\"vol\":10,"
        "\"en\":true,\"desc\":\"individual\"}]";
    check(course_editor::saveDay(1, edited, error), "saveDay failed: " + error);

    course_store::CalendarState cal;
    check(store.loadCalendar(cal, error), "calendar: " + error);
    check(cal.active_generation == published, "the published generation changed");
    check(cal.draft_generation != 0 && cal.draft_generation != cal.active_generation,
          "the edit did not use a private draft generation");

    check(store.prepareTemplates(cal.draft_generation, error),
          "prepare draft failed: " + error);
    course::Day day;
    check(store.loadDay(cal.draft_generation, 1, day, error), "load edited: " + error);
    check(day.events.size() == 1 && day.events[0].description == "individual",
          "the edit was not stored as an individual day");
    course::Day neighbour;
    check(store.loadDay(cal.draft_generation, 2, neighbour, error), "load neighbour: " + error);
    check(neighbour.events.size() == 3, "a neighbouring day changed");

    // The published course still has the shared template everywhere.
    check(store.prepareTemplates(published, error), "prepare active failed: " + error);
    course::Day publishedDay;
    check(store.loadDay(published, 1, publishedDay, error), "load published day: " + error);
    check(publishedDay.events.size() == 3, "the published course was edited in place");

    // Clearing a day is also an individual copy (an empty own-event day), and
    // it must work with a cold cache.
    store.invalidateTemplates();
    check(course_editor::clearDay(3, error), "clearDay failed: " + error);
    check(store.prepareTemplates(cal.draft_generation, error), "prepare draft: " + error);
    course::Day cleared;
    check(store.loadDay(cal.draft_generation, 3, cleared, error), "load cleared: " + error);
    check(cleared.events.empty() && cleared.hasOwnEvents,
          "a cleared day did not become an empty individual day");
    course::Day untouched;
    check(store.loadDay(cal.draft_generation, 0, untouched, error), "load day 0: " + error);
    check(untouched.events.size() == 3, "clearing one day emptied another");
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage D: compact JSON export (templates once, days as references).
// ─────────────────────────────────────────────────────────────────────────────

std::string collectExport(course_editor::ExportSource source, std::string& error,
                         std::size_t* maxChunk = nullptr,
                         std::size_t* chunkCount = nullptr) {
    std::string out;
    if (!course_editor::beginExport(source, error)) return std::string();
    for (;;) {
        std::string chunk;
        bool done = false;
        if (!course_editor::nextExportChunk(chunk, done, error)) return std::string();
        out += chunk;
        if (maxChunk != nullptr && chunk.size() > *maxChunk) *maxChunk = chunk.size();
        if (chunkCount != nullptr) ++*chunkCount;
        if (done) break;
    }
    return out;
}

// 14 days, 3 shared templates and one individually edited day.
course::Course compactSource(std::string name) {
    course::Course course;
    course.name = name;
    for (int t = 0; t < 3; ++t) {
        for (int e = 0; e < 4; ++e) {
            course::Event ev;
            ev.id = static_cast<std::uint32_t>(e + 1);
            ev.hour = static_cast<std::uint8_t>(6 + e);
            ev.minute = static_cast<std::uint8_t>(10 * t);
            ev.track = static_cast<std::uint8_t>(1 + t);
            ev.loop = 1;
            ev.volume = 10;
            ev.enabled = true;
            ev.description = "Шаблон " + std::to_string(t) + " событие " + std::to_string(e);
            course.templates["t" + std::to_string(t)].push_back(ev);
        }
    }
    for (int d = 0; d < 14; ++d) {
        course::Day day;
        if (d == 5) {
            day.hasOwnEvents = true;
            course::Event ev;
            ev.id = 1;
            ev.hour = 21;
            ev.minute = 15;
            ev.track = 1;
            ev.loop = 1;
            ev.volume = 10;
            ev.enabled = false;
            ev.description = "Индивидуальный день";
            day.events.push_back(ev);
        } else {
            day.templateName = "t" + std::to_string(d % 3);
        }
        course.days.push_back(day);
    }
    return course;
}

void testCompactExportShape() {
    resetHost();
    std::string error;
    course::Course source = compactSource("Gong #2");
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    check(store.applyCompactCourse(source, generation, state, error),
          "compact apply failed: " + error);

    const std::size_t beforeCache = store.cachedTemplateGeneration();
    const auto filesBefore = host::files();
    course_store::CalendarState calendarBefore;
    check(store.loadCalendar(calendarBefore, error), "calendar before failed");

    const std::string json = collectExport(course_editor::kExportActive, error);
    check(!json.empty(), "export failed: " + error);
    std::cout << "  compact export bytes=" << json.size() << " days="
              << static_cast<int>(source.days.size()) << "\n";
    check(json.find("\"version\":1") != std::string::npos, "export is not version 1");
    check(json.find("\"templates\":") != std::string::npos, "export has no templates");
    check(json.find("\"days\":[") != std::string::npos, "export has no days");
    // Reference days stay references: they appear as quoted template names.
    check(json.find("\"t1\"") != std::string::npos, "a day reference is missing");
    check(json.find("\"Индивидуальный день\"") != std::string::npos,
          "the individual day was lost");
    check(json.find("\"en\":false") != std::string::npos,
          "a disabled event lost its flag");
    check(json.find("Шаблон 0") != std::string::npos, "Cyrillic text was damaged");
    // Each template body must exist exactly once (not expanded per day).
    std::size_t occurrences = 0;
    for (std::size_t pos = json.find("\"t0\":");
         pos != std::string::npos; pos = json.find("\"t0\":", pos + 1)) {
        ++occurrences;
    }
    check(occurrences == 1, "template t0 was expanded more than once");

    // The exported document must be accepted by the strict parser.
    course::Course parsed;
    check(course::fromJson(json, parsed, error), "export does not parse: " + error);
    check(parsed.days.size() == 14, "exported length");
    check(parsed.templates.size() == 3, "exported template count");
    for (std::size_t d = 0; d < source.days.size(); ++d) {
        std::vector<course::Event> expected;
        std::vector<course::Event> actual;
        check(course::resolvedEvents(source, d, expected, error), "source resolve");
        check(course::resolvedEvents(parsed, d, actual, error), "export resolve");
        bool same = expected.size() == actual.size();
        for (std::size_t i = 0; same && i < expected.size(); ++i) {
            same = expected[i].hour == actual[i].hour &&
                   expected[i].minute == actual[i].minute &&
                   expected[i].track == actual[i].track &&
                   expected[i].loop == actual[i].loop &&
                   expected[i].volume == actual[i].volume &&
                   expected[i].enabled == actual[i].enabled &&
                   expected[i].description == actual[i].description;
        }
        check(same, "day " + std::to_string(d) + " changed in the export");
    }

    // Export must not change state of any kind.
    check(store.cachedTemplateGeneration() == beforeCache, "export changed the template cache");
    check(host::files() == filesBefore, "export changed LittleFS");
    course_store::CalendarState calendarAfter;
    check(store.loadCalendar(calendarAfter, error), "calendar after failed");
    check(calendarAfter.active_generation == calendarBefore.active_generation &&
              calendarAfter.flags == calendarBefore.flags &&
              calendarAfter.activation_date == calendarBefore.activation_date,
          "export changed the calendar");
}

void testCompactExportAfterEdit() {
    resetHost();
    std::string error;
    course::Course source = compactSource("edited");
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    check(store.applyCompactCourse(source, generation, state, error),
          "apply failed: " + error);

    // Copy-on-write edit of a reference day (draft generation).
    check(course_editor::saveDay(
              2,
              "[{\"id\":7,\"hour\":12,\"min\":5,\"track\":2,\"loop\":3,\"vol\":20,"
              "\"en\":false,\"desc\":\"Правка дня\"}]",
              error),
          "saveDay failed: " + error);

    const std::string active = collectExport(course_editor::kExportActive, error);
    check(!active.empty(), "active export failed: " + error);
    check(active.find("\"Правка дня\"") == std::string::npos,
          "the published export contains an unapplied draft edit");

    const std::string draft = collectExport(course_editor::kExportDraft, error);
    check(!draft.empty(), "draft export failed: " + error);
    check(draft.find("\"Правка дня\"") != std::string::npos,
          "the draft export lost the individual edit");
    check(draft.find("\"en\":false") != std::string::npos, "the edit lost en=false");

    course::Course parsed;
    check(course::fromJson(draft, parsed, error), "draft export does not parse: " + error);
    std::vector<course::Event> day2;
    check(course::resolvedEvents(parsed, 2, day2, error), "resolve day 2 failed");
    check(day2.size() == 1 && day2[0].description == "Правка дня",
          "the edited day is not an individual day in the export");
    std::vector<course::Event> day1;
    check(course::resolvedEvents(parsed, 1, day1, error), "resolve day 1 failed");
    check(day1.size() == 4, "another day was expanded or lost");
    check(parsed.days[1].templateName == "t1", "day 1 is no longer a reference");
}

void testCompactExportRoundTrip() {
    resetHost();
    std::string error;
    course::Course source = compactSource("round trip");
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    check(store.applyCompactCourse(source, generation, state, error),
          "apply failed: " + error);

    const std::string exported = collectExport(course_editor::kExportActive, error);
    check(!exported.empty(), "export failed: " + error);

    // Re-import through the compact path, apply, export again and compare.
    resetHost();
    course_store::CourseStore store2;
    check(course_editor::importCourseCompact(exported, error),
          "re-import failed: " + error);
    check(course_editor::applyDraft(error), "apply draft failed: " + error);
    const std::string again = collectExport(course_editor::kExportActive, error);
    check(!again.empty(), "second export failed: " + error);

    course::Course first;
    course::Course second;
    check(course::fromJson(exported, first, error), "first parse: " + error);
    check(course::fromJson(again, second, error), "second parse: " + error);
    check(first.days.size() == second.days.size(), "round trip changed the length");
    for (std::size_t d = 0; d < first.days.size(); ++d) {
        std::vector<course::Event> a;
        std::vector<course::Event> b;
        check(course::resolvedEvents(first, d, a, error), "resolve a");
        check(course::resolvedEvents(second, d, b, error), "resolve b");
        bool same = a.size() == b.size();
        for (std::size_t i = 0; same && i < a.size(); ++i) {
            same = a[i].hour == b[i].hour && a[i].minute == b[i].minute &&
                   a[i].track == b[i].track && a[i].loop == b[i].loop &&
                   a[i].volume == b[i].volume && a[i].enabled == b[i].enabled &&
                   a[i].description == b[i].description;
        }
        check(same, "round trip changed day " + std::to_string(d));
    }
    check(store2.generationExists(0) == false, "sanity");
}

void testCompactExportFailures() {
    // Missing template file.
    resetHost();
    std::string error;
    course::Course source = compactSource("broken");
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    check(store.applyCompactCourse(source, generation, state, error),
          "apply failed: " + error);
    host::files().erase("/course/g" + std::to_string(generation) + "/templates.json");
    error.clear();
    std::string json = collectExport(course_editor::kExportActive, error);
    check(json.empty(), "export with a missing templates file must fail");
    check(error.find("templates") != std::string::npos ||
              error.find("cannot open") != std::string::npos,
          "unclear error: " + error);

    // Corrupt template body (valid CRC, broken JSON).
    resetHost();
    std::string error2;
    check(store.applyCompactCourse(source, generation, state, error2),
          "apply failed again: " + error2);
    std::string& raw = host::files()["/course/g" + std::to_string(generation) +
                                     "/templates.json"];
    const std::size_t newline = raw.find('\n');
    std::string body = "{\"format\":\"gong-course-templates\",\"version\":1,"
                       "\"templates\":{\"t0\":[{\"id\":1,\"hour\":99}]}}";
    char header[32];
    std::snprintf(header, sizeof(header), "CRC32 %08lx\n",
                  static_cast<unsigned long>(course_store::CourseStore::crc32(body)));
    raw = std::string(header) + body;
    error2.clear();
    json = collectExport(course_editor::kExportActive, error2);
    check(json.empty(), "export with a broken template body must fail");
    (void)newline;

    // A day that references a template the file does not define.
    resetHost();
    std::string error3;
    check(store.applyCompactCourse(source, generation, state, error3),
          "apply failed once more: " + error3);
    {
        std::string& dayRaw = host::files()["/course/g" + std::to_string(generation) +
                                            "/d00.json"];
        const std::size_t nl = dayRaw.find('\n');
        const std::string dayJson =
            "{\"format\":\"gong-course\",\"version\":2,\"name\":\"day\",\"generation\":" +
            std::to_string(generation) +
            ",\"course_length\":1,\"days\":[{\"template\":\"ghost\"}]}";
        std::snprintf(header, sizeof(header), "CRC32 %08lx\n",
                      static_cast<unsigned long>(course_store::CourseStore::crc32(dayJson)));
        dayRaw = std::string(header) + dayJson;
        (void)nl;
    }
    error3.clear();
    json = collectExport(course_editor::kExportActive, error3);
    check(json.empty(), "export with a missing template reference must fail");
    check(error3.find("missing template") != std::string::npos, "unclear error: " + error3);
}

void testExpandedExportUnchanged() {
    resetHost();
    std::string error;
    course::Course course;
    course.name = "expanded";
    for (int d = 0; d < 3; ++d) {
        course::Day day;
        day.hasOwnEvents = true;
        course::Event ev;
        ev.id = 1;
        ev.hour = 4;
        ev.minute = 0;
        ev.track = 1;
        ev.loop = 1;
        ev.volume = 10;
        ev.enabled = true;
        ev.description = "day " + std::to_string(d);
        day.events.push_back(ev);
        course.days.push_back(day);
    }
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    check(store.applyCourse(course, generation, state, error), "apply failed: " + error);

    const std::string json = collectExport(course_editor::kExportActive, error);
    check(!json.empty(), "export failed: " + error);
    check(json.find("\"version\":2") != std::string::npos, "expanded export changed version");
    check(json.find("\"templates\"") == std::string::npos,
          "expanded export gained a templates section");
    check(json.find("\"days\":[{\"events\":") != std::string::npos,
          "expanded export changed the day shape");
    course::Course parsed;
    check(course::fromJson(json, parsed, error), "expanded export does not parse: " + error);
    check(parsed.days.size() == 3, "expanded export length");
}


// The compact export must stay inside the heap of a device without PSRAM: the
// whole document is never built, and one template is one chunk.
void testCompactExportMemory() {
#ifndef HOST_SANITIZED
    resetHost();
    std::string error;
    course::Course course;
    course.name = "big";
    for (int t = 0; t < 8; ++t) {
        for (int e = 0; e < 32; ++e) {
            course::Event ev;
            ev.id = static_cast<std::uint32_t>(e + 1);
            ev.hour = static_cast<std::uint8_t>(5 + e % 18);
            ev.minute = static_cast<std::uint8_t>((e * 3) % 60);
            ev.track = 1;
            ev.loop = 1;
            ev.volume = 10;
            ev.enabled = true;
            ev.description = "Практика " + std::to_string(t) + "-" + std::to_string(e);
            course.templates["t" + std::to_string(t)].push_back(ev);
        }
    }
    for (int d = 0; d < 47; ++d) {
        course::Day day;
        day.templateName = "t" + std::to_string(d % 8);
        course.days.push_back(day);
    }
    course_store::CourseStore store;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    check(store.applyCompactCourse(course, generation, state, error),
          "apply failed: " + error);

    host::heapBudgetReset(96 * 1024);
    std::size_t maxChunk = 0;
    std::size_t chunks = 0;
    const std::string json =
        collectExport(course_editor::kExportActive, error, &maxChunk, &chunks);
    const std::size_t peak = host::heapBudget().peak;
    host::heapBudgetReset(0);
    check(!json.empty(), "export under a heap budget failed: " + error);
    std::cout << "  export peak=" << peak / 1024 << "KB chunks=" << chunks
              << " maxChunk=" << maxChunk << "B bytes=" << json.size() << "B\n";
    check(peak < 96 * 1024, "the export exceeded the device-sized heap budget");
    check(maxChunk < 16 * 1024, "a single export chunk is too large");
    check(chunks > 47, "the export was not streamed day by day");
#endif
}

void testCompactExportLengths() {
    const int lengths[] = {1, 10, 20, 47};
    for (std::size_t n = 0; n < sizeof(lengths) / sizeof(lengths[0]); ++n) {
        resetHost();
        std::string error;
        course::Course course;
        course.name = "len";
        for (int e = 0; e < 3; ++e) {
            course::Event ev;
            ev.id = static_cast<std::uint32_t>(e + 1);
            ev.hour = static_cast<std::uint8_t>(7 + e);
            ev.minute = 0;
            ev.track = 1;
            ev.loop = 1;
            ev.volume = 10;
            ev.enabled = true;
            ev.description = "общее " + std::to_string(e);
            course.templates["only"].push_back(ev);
        }
        for (int d = 0; d < lengths[n]; ++d) {
            course::Day day;
            day.templateName = "only";
            course.days.push_back(day);
        }
        course_store::CourseStore store;
        std::uint32_t generation = 0;
        course_store::CalendarState state;
        check(store.applyCompactCourse(course, generation, state, error),
              "apply failed: " + error);
        const std::string json = collectExport(course_editor::kExportActive, error);
        check(!json.empty(), "export failed for length " + std::to_string(lengths[n]));
        course::Course parsed;
        check(course::fromJson(json, parsed, error),
              "parse failed for length " + std::to_string(lengths[n]) + ": " + error);
        check(static_cast<int>(parsed.days.size()) == lengths[n], "exported length mismatch");
    }
}

// Stage E: the operational import picks the layout, and validation of a
// maximum-size template course stays inside a device-sized heap.
void testAutoImportLayout() {
    resetHost();
    std::string error;
    // Template document -> compact generation.
    const std::string templated = compactCourseJsonV1(14, 3, 8);
    check(course_editor::importCourseAuto(templated, error),
          "auto import of a template course failed: " + error);
    course_store::CourseStore store;
    const std::uint32_t draft = store.loadBootstrapGeneration();
    course_store::CourseMeta meta;
    check(store.loadMeta(draft, meta, error), "meta: " + error);
    check(meta.layout == course_store::kLayoutTemplates,
          "a template document must be stored compactly");
    check(host::files().find("/course/g" + std::to_string(draft) + "/templates.json") !=
              host::files().end(),
          "compact storage wrote no templates.json");

    // Full-event document -> expanded generation (unchanged behaviour).
    resetHost();
    const std::string expanded = bigCourseJsonV2(4, 5, "full events");
    check(course_editor::importCourseAuto(expanded, error),
          "auto import of a full-event course failed: " + error);
    course_store::CourseStore store2;
    const std::uint32_t draft2 = store2.loadBootstrapGeneration();
    course_store::CourseMeta meta2;
    check(store2.loadMeta(draft2, meta2, error), "meta2: " + error);
    check(meta2.layout == course_store::kLayoutExpanded,
          "a full-event document must keep the expanded layout");
    check(host::files().find("/course/g" + std::to_string(draft2) + "/templates.json") ==
              host::files().end(),
          "expanded storage wrote a templates file");
}

void testTemplateValidationFitsDeviceHeap() {
#ifndef HOST_SANITIZED
    resetHost();
    // 47 days, 8 templates x 32 events = 256 unique events, 1 504 occurrences.
    std::string doc = "{\"format\":\"gong-course\",\"version\":1,\"name\":\"big\",\"templates\":{";
    for (int t = 0; t < 8; ++t) {
        if (t) doc += ',';
        doc += "\"t" + std::to_string(t) + "\":[";
        for (int e = 0; e < 32; ++e) {
            if (e) doc += ',';
            doc += "{\"id\":" + std::to_string(e + 1) + ",\"hour\":" +
                   std::to_string(5 + e % 18) + ",\"min\":" + std::to_string((e * 2) % 60) +
                   ",\"track\":1,\"loop\":1,\"vol\":20,\"en\":true,\"desc\":\"Практика " +
                   std::to_string(t) + "-" + std::to_string(e) + "\"}";
        }
        doc += "]";
    }
    doc += "},\"days\":[";
    for (int d = 0; d < 47; ++d) {
        if (d) doc += ',';
        doc += "\"t" + std::to_string(d % 8) + "\"";
    }
    doc += "]}";

    course_editor::Inspection inspection;
    std::string error;
    host::heapBudgetReset(64 * 1024);
    const bool ok = course_editor::inspectCourse(doc, inspection, error);
    const std::size_t peak = host::heapBudget().peak;
    host::heapBudgetReset(0);
    std::cout << "  template validation peak=" << peak / 1024 << "KB doc=" << doc.size()
              << "B\n";
    check(ok, "validation under a device-sized heap failed: " + error);
    check(inspection.length == 47, "validation length");
    check(inspection.events == 1504, "validation event count");
    check(peak < 32 * 1024, "validation peak is still too high for the device");
#endif
}

int main() {
    runGroup("empty store", testEmptyStore);
    runGroup("initial reference course", testInitialReferenceCourse);
    runGroup("event editing", testEventEditing);
    runGroup("insert and delete", testInsertAndDelete);
    runGroup("last day and presets", testLastDayAndPresets);
    runGroup("apply and discard", testApplyAndDiscard);
    runGroup("rename and import", testRenameAndImport);
    runGroup("export", testExport);
    runGroup("index guards", testIndexGuards);
    runGroup("structural write failure", testStructuralWriteFailure);
    runGroup("short write keeps day", testShortWriteKeepsDay);
    runGroup("apply nvs failure", testApplyNvsFailure);
    runGroup("corrupt draft generation", testCorruptDraftGeneration);
    runGroup("export fault", testExportFault);
    runGroup("bootstrap without course", testBootstrapWithoutCourse);
    runGroup("bootstrap apply", testBootstrapApply);
    runGroup("bootstrap import", testBootstrapImport);
    runGroup("bootstrap failures", testBootstrapFailures);
    runGroup("bootstrap presets", testBootstrapPresets);
    runGroup("bootstrap discard and seed guard", testBootstrapDiscardAndSeedGuard);
    runGroup("bootstrap nvs key", testBootstrapNvsKey);
    runGroup("json inspection", testInspectCourse);
    runGroup("import replaces course", testImportReplacesCourse);
    runGroup("export source", testExportSource);
    runGroup("compact import", testCompactImport);
    runGroup("compact import rejects expanded", testCompactImportRejectsExpanded);
    runGroup("compact import failure keeps course", testCompactImportFailureKeepsCourse);
    runGroup("copy-on-write day", testCopyOnWriteDay);
    runGroup("compact export", testCompactExportShape);
    runGroup("compact export after edit", testCompactExportAfterEdit);
    runGroup("compact export round trip", testCompactExportRoundTrip);
    runGroup("compact export failures", testCompactExportFailures);
    runGroup("expanded export unchanged", testExpandedExportUnchanged);
    runGroup("compact export lengths", testCompactExportLengths);
    runGroup("compact export memory", testCompactExportMemory);
    runGroup("auto import layout", testAutoImportLayout);
    runGroup("template validation fits device heap",
             testTemplateValidationFitsDeviceHeap);
    runGroup("operator sized course", testOperatorSizedCourse);
    runGroup("heap budget", testHeapBudget);
    runGroup("published course ignores bootstrap", testPublishedCourseIgnoresBootstrap);

    std::cout << "\n" << g_pass << " checks passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
