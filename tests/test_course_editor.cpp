// Host-only tests for course_editor: draft generations, structural edits,
// presets, import/export and Apply. No device or real NVS/LittleFS access.
#include "course_editor.h"
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
    check(course_editor::beginExport(error), "export begins");
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
    check(course_editor::beginExport(error), "export begins");

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
    check(course_editor::beginExport(error), "a new export can start after an abort");
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
    runGroup("published course ignores bootstrap", testPublishedCourseIgnoresBootstrap);

    std::cout << "\n" << g_pass << " checks passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
