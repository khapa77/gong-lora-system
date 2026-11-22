// LittleFS course storage and NVS calendar state. Host-testable.
#pragma once

#include "course_model.h"

#include <cstdint>
#include <string>

namespace course_store {

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
    // binding is reset to APPLIED (no calls until Activate this day).
    bool applyDraft(std::uint32_t draftGeneration, CalendarState& outState,
                    std::string& error);

    bool validateGeneration(std::uint32_t generation, std::uint8_t expectedLength,
                            std::string& error);
    bool deleteGeneration(std::uint32_t generation, std::string& error);

    // Repairs interrupted file replacements and removes stale .tmp/.bak files.
    bool recover(std::string& error);

    bool loadLastFire(LastFire& out, std::string& error);
    bool saveLastFire(const LastFire& value, std::string& error);
    bool clearLastFire(std::string& error);

    static std::uint32_t crc32(const std::string& data);
};

}  // namespace course_store
