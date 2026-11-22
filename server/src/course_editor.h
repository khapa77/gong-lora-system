// Draft course editing for the Web GUI (stage 5). Host-testable: the module
// only talks to CourseStore/CourseModel and never touches the live scheduler.
//
// The active generation is immutable. Every draft write goes to a separate
// generation: the first write after an Apply clones the active generation
// (copy-on-write), structural edits build a brand-new generation day by day
// and switch the NVS draft pointer only after meta.json is published.
#pragma once

#include "course_store.h"

#include <cstdint>
#include <string>

namespace course_editor {

// Result of validating a course JSON without storing anything. The Web GUI
// shows it before the operator publishes the file.
struct Inspection {
    std::string name;
    std::uint8_t length;
    std::uint16_t events;   // events across all days, enabled or not
    std::uint8_t version;   // 1 or 2

    Inspection();
};

// Parses and validates a v1/v2 course JSON. Pure: no files, no NVS, no RAM
// state is touched, so it can be called while a course is running.
//
// The pointer/length form is the one the device uses: the document is scanned
// once for the byte range of every day (and template) and each range is parsed
// on its own through the course model, so a whole-document tree (hundreds of
// KB on a no-PSRAM ESP32) is never built. The std::string overloads do not copy
// the input either.
bool inspectCourse(const char* data, std::size_t size, Inspection& out,
                   std::string& error);
bool inspectCourse(const std::string& json, Inspection& out, std::string& error);

struct Info {
    bool hasCourse;
    std::uint32_t activeGeneration;
    std::uint32_t draftGeneration;
    std::uint32_t activationDate;
    std::uint8_t length;
    std::uint8_t flags;
    std::uint8_t failureCount;
    std::string name;
    bool draftDirty;

    Info();
};

// Reads the persisted calendar + draft meta. False when no course exists.
bool info(Info& out, std::string& error);

// Events of one draft day, as a JSON array in the same shape the event editor
// already uses (id, hour, min, track, vol, en, desc).
bool dayJson(std::uint8_t day, std::string& out, std::string& error);

// Replaces the events of one draft day. `json` is an array of event objects.
bool saveDay(std::uint8_t day, const std::string& json, std::string& error);

// Removes the events of one draft day but keeps the calendar slot.
bool clearDay(std::uint8_t day, std::string& error);

// Inserts a day before `day` (or at the end when day == length). With
// copyEvents the inserted day takes a copy of the source day's events.
bool insertDay(std::uint8_t day, bool copyEvents, std::string& error);

// Removes one calendar slot from the draft. The last remaining day cannot be
// removed.
bool deleteDay(std::uint8_t day, std::string& error);

// Replaces the draft with a preset of `days` calendar days. 14 uses the
// reference course (arrival, practice, departure, two service days).
bool setPreset(std::uint8_t days, std::string& error);

bool renameCourse(const std::string& name, std::string& error);

// Stage B: imports a template course into a compact generation (templates once,
// days as references). Deliberately NOT wired into any HTTP route: the
// scheduler cannot execute a template generation until stage E loads the
// template cache at boot / Apply / Activate, so the operational import keeps
// using the expanded writer above.
bool importCourseCompact(const char* data, std::size_t size, std::string& error);
bool importCourseCompact(const std::string& json, std::string& error);

// Imports a v1/v2 course JSON into the draft. The active course is untouched.
// Same bounded-memory strategy as inspectCourse(): one day is parsed and
// written at a time.
bool importCourse(const char* data, std::size_t size, std::string& error);
bool importCourse(const std::string& json, std::string& error);

// Publishes the draft generation. The active generation is only replaced after
// every day of the draft has been validated. The calendar binding is reset to
// APPLIED (no calls until an explicit Activate).
bool applyDraft(std::string& error);

// Drops the draft and points the draft back at the active generation.
bool discardDraft(std::string& error);

// First-run course: writes the reference course as generation 1 with an
// APPLIED binding. Returns false when a calendar already exists.
bool ensureInitialCourse(std::string& error);

// Which generation an export describes: the published active course, or the
// draft the operator is editing.
enum ExportSource {
    kExportActive = 0,
    kExportDraft = 1
};

// Streaming export. beginExport() snapshots the chosen generation's meta,
// nextExportChunk() yields the v2 JSON in day-sized pieces so no whole-course
// buffer is built. kExportActive falls back to the draft when nothing has been
// published yet, because then the draft is all the device has.
bool beginExport(ExportSource source, std::string& error);
bool nextExportChunk(std::string& chunk, bool& done, std::string& error);

}  // namespace course_editor
