// Host-testable cyclic course model. No Arduino headers and no global state.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace course {

const std::size_t kMaxDays = 47;
const std::size_t kMaxEventsPerDay = 32;
const std::size_t kMaxDescBytes = 96;
const std::uint8_t kMaxTrack = 99;
const std::uint8_t kMaxLoop = 7;
const std::uint8_t kMaxVolume = 30;
const std::uint8_t kDefaultTrack = 1;
const std::uint8_t kDefaultLoop = 1;
const std::uint8_t kDefaultVolume = 30;

struct Event {
    std::uint32_t id;
    std::uint8_t hour;
    std::uint8_t minute;
    std::uint8_t track;
    std::uint8_t loop;
    std::uint8_t volume;
    bool enabled;
    std::string description;

    Event();
};

struct Day {
    // A day either references a named template or owns its event list.
    // hasOwnEvents == true means events is authoritative. Editing a day that
    // references a template materialises a private copy (copy-on-write).
    std::string templateName;
    bool hasOwnEvents;
    std::vector<Event> events;

    Day();
};

struct Course {
    std::string name;
    std::uint32_t generation;
    std::map<std::string, std::vector<Event> > templates;
    std::vector<Day> days;

    Course();
};

// Civil date arithmetic. Dates are independent of time_t, DST and 86400.
struct Date {
    int year;
    int month;
    int day;

    Date();
};

bool isLeapYear(int year);
int daysInMonth(int year, int month);
bool parseDate(const std::string& text, Date& out, std::string& error);
std::string formatDate(const Date& date);
long long civilDaysBetween(const Date& from, const Date& to);
Date addCivilDays(const Date& date, long long days);

// Returns false when the course length is invalid or today is before the
// activation date. The result is always in [0, length).
bool computeDayIndex(int activatedDay, const Date& activationDate,
                     const Date& today, int courseLength, int& out,
                     std::string& error);
int nextDayIndex(int currentDay, int courseLength);

bool validate(const Course& course, std::string& error);
bool validateEvent(const Event& event, std::string& error);

// Resolves a day to its effective event list without modifying the course.
bool resolvedEvents(const Course& course, std::size_t dayIndex,
                    std::vector<Event>& out, std::string& error);

// Structural editing. Insert positions are explicit; the resulting days are
// renumbered by their vector position.
bool insertBefore(Course& course, std::size_t index, bool copySource,
                  std::string& error);
bool insertAfter(Course& course, std::size_t index, bool copySource,
                 std::string& error);
bool deleteDay(Course& course, std::size_t index, std::string& error);
bool clearDay(Course& course, std::size_t index, std::string& error);

// Ordinary event editing. All functions materialise the day first, so edits
// never mutate a shared template or another day.
bool addEvent(Course& course, std::size_t dayIndex, Event event,
              std::string& error);
bool updateEvent(Course& course, std::size_t dayIndex, std::uint32_t id,
                 const Event& event, std::string& error);
bool removeEvent(Course& course, std::size_t dayIndex, std::uint32_t id,
                 std::string& error);
bool setEventEnabled(Course& course, std::size_t dayIndex, std::uint32_t id,
                     bool enabled, std::string& error);

// JSON. Import accepts v1 and v2. Export always writes v2. expandTemplates
// writes every day as an inline event list; otherwise template references are
// preserved where the day does not own its events.
bool fromJson(const std::string& json, Course& out, std::string& error);
std::string toJsonV2(const Course& course, bool expandTemplates,
                     std::string& error);

// Reference 14-day course: day 0 arrival, days 1-10 practice, day 11
// departure, day 12 service, day 13 an independent copy of day 12.
Course makeReferenceCourse();

}  // namespace course
