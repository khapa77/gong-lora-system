// LittleFS course storage and NVS calendar state. Host-testable.
#pragma once

#include "course_model.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace course_store {

// Generation layouts.
//   kLayoutExpanded  — every day file carries its own event list (all courses
//                      published before stage A).
//   kLayoutTemplates — meta.json says "layout":"tpl1": the unique templates
//                      live once in templates.json and a day may be a
//                      reference {"template":"name"}.
enum : std::uint8_t {
    kLayoutExpanded = 0,
    kLayoutTemplates = 1,
};

// Longest template name accepted in templates.json.
const std::size_t kMaxTemplateNameBytes = 64;

// Calendar flags. A course may be published (APPLIED) without being bound to a
// calendar date. RUNNING means the activation date/day binding is valid.
enum CalendarFlag : std::uint8_t {
    kApplied = 0x01,
    kRunning = 0x02,
    kSuspended = 0x04,
    kEmergency = 0x08
};

struct CalendarState {
    std::uint32_t active_generation;
    std::uint32_t draft_generation;
    std::uint32_t activation_date;  // YYYYMMDD, 0 when not activated
    std::uint8_t day_index;
    std::uint8_t course_length;
    std::uint8_t flags;
    std::uint8_t failure_count;

    CalendarState();
};

struct LastFire {
    std::uint32_t generation;
    std::uint8_t day_index;
    std::uint16_t minute;  // minute of day, 0..1439
    std::uint32_t event_id;
    std::uint32_t date;       // YYYYMMDD
    std::uint32_t timestamp;  // epoch seconds, used for clock-rollback checks

    LastFire();
};

struct CourseMeta {
    std::uint32_t generation;
    std::uint8_t course_length;
    std::string name;
    // kLayoutExpanded for every generation written before stage A. Only
    // kLayoutTemplates generations may contain day references and a
    // templates.json file.
    std::uint8_t layout;

    CourseMeta();
};

class CourseStore {
public:
    CourseStore();

    // Creates /course if needed. Does not touch existing generations.
    bool begin(std::string& error);

    bool hasCalendar();
    bool loadCalendar(CalendarState& out, std::string& error);
    bool saveCalendar(const CalendarState& state, std::string& error);

    bool loadMeta(std::uint32_t generation, CourseMeta& out, std::string& error);
    bool loadDay(std::uint32_t generation, std::uint8_t day, course::Day& out,
                 std::string& error);

    // Atomic replacement of one day file. The supplied day must be materialised
    // (hasOwnEvents=true); templates are resolved by applyCourse before storage.
    bool saveDayAtomic(std::uint32_t generation, std::uint8_t day,
                       const course::Day& dayData, std::string& error);

    // Publishes a complete new generation. The generation is written and
    // validated first; NVS is updated last. The old generation remains usable
    // until the NVS write succeeds.
    bool applyCourse(const course::Course& course, std::uint32_t& newGeneration,
                     CalendarState& outState, std::string& error);

    // Stores a complete course as a draft generation. The active generation and
    // its calendar binding are not changed.
    bool saveDraftCourse(const course::Course& course,
                         std::uint32_t& draftGeneration,
                         CalendarState& outState, std::string& error);

    // Publishes an already validated draft generation. The active calendar
    // binding is replaced by APPLIED.
    bool applyDraft(std::uint32_t draftGeneration, CalendarState& outState,
                    std::string& error);

    // Copies every day of `from` into a brand-new generation and returns its
    // id. Days are copied one at a time, so only one day is held in RAM.
    bool cloneGeneration(std::uint32_t from, std::uint32_t& outGeneration,
                         std::string& error);

    // Allocates an empty generation directory without writing meta.json. The
    // caller fills the days one at a time and publishes meta last with
    // saveMeta(), so an interrupted build leaves an unreferenced generation
    // rather than a mixed course.
    bool createGeneration(std::uint32_t& outGeneration, std::string& error);

    // Atomically replaces meta.json of an existing generation.
    bool saveMeta(const CourseMeta& meta, std::string& error);

    bool generationExists(std::uint32_t generation);

    bool validateGeneration(std::uint32_t generation, std::uint8_t expectedLength,
                            std::string& error);
    bool deleteGeneration(std::uint32_t generation, std::string& error);

    // A published generation is kept as a fallback until the new active day
    // has actually loaded. The id is recorded here and reclaimed afterwards.
    bool setPendingGeneration(std::uint32_t generation, std::string& error);
    bool reclaimPendingGeneration(std::string& error);

    // Bootstrap draft. Before the first Apply there is no calendar, so the
    // draft generation cannot be referenced from course_state. It is recorded
    // in its own NVS key instead; nothing executes from it and no calendar is
    // created until the operator applies the draft.
    std::uint32_t loadBootstrapGeneration();
    bool saveBootstrapGeneration(std::uint32_t generation, std::string& error);

    // Repairs interrupted file replacements and removes stale .tmp/.bak files.
    bool recover(std::string& error);

    // ── Template set (stage A) ────────────────────────────────────────────
    // The unique templates of one generation, stored once in templates.json.
    // The cache is process-wide, strictly tagged with its generation and
    // fail-closed: a missing file, a CRC error, a broken body or an exhausted
    // budget leaves it empty, so loadDay() can never serve a partial schedule.
    static const std::size_t kMaxTemplates = 32;
    static const std::size_t kMaxTemplateEvents = 256;

    // Writes templates.json for `generation` atomically and invalidates the
    // cache. The caller sets meta.layout = kLayoutTemplates.
    bool writeTemplates(std::uint32_t generation,
                        const std::map<std::string, std::vector<course::Event> >& templates,
                        std::string& error);

    // Loads the template set of `generation` into the cache. Returns true and
    // leaves the cache usable only on complete success. A different generation
    // always invalidates the previous entries first.
    bool prepareTemplates(std::uint32_t generation, std::string& error);
    bool templatesReady(std::uint32_t generation) const;
    void invalidateTemplates();
    std::size_t cachedTemplateCount() const;
    std::size_t cachedTemplateEvents() const;
    std::uint32_t cachedTemplateGeneration() const;

    bool loadLastFire(LastFire& out, std::string& error);
    bool saveLastFire(const LastFire& value, std::string& error);
    bool clearLastFire(std::string& error);

    static std::uint32_t crc32(const std::string& data);
};

}  // namespace course_store
