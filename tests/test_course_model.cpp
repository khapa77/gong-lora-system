// Host-only tests for the Arduino-independent course model.
#include "course_model.h"

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
        std::cout << "  FAIL " << g_group << ": exception: " << e.what() << "\n";
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
    c.name = "test";
    c.generation = 1;
    for (int i = 0; i < days; ++i) {
        course::Day d;
        d.hasOwnEvents = true;
        d.events.push_back(event(4, 0, "day " + std::to_string(i), 1));
        c.days.push_back(d);
    }
    return c;
}

bool sameEvent(const course::Event& a, const course::Event& b) {
    return a.id == b.id && a.hour == b.hour && a.minute == b.minute &&
           a.track == b.track && a.loop == b.loop && a.volume == b.volume &&
           a.enabled == b.enabled && a.description == b.description;
}

void reference_structure() {
    course::Course c = course::makeReferenceCourse();
    std::string error;
    check(course::validate(c, error), "reference course validates: " + error);
    check(c.days.size() == 14, "reference has 14 days");
    check(c.days[0].templateName == "arrival", "day 0 uses arrival");
    check(c.days[11].templateName == "departure", "day 11 uses departure");
    check(c.days[12].hasOwnEvents, "day 12 owns service events");
    check(c.days[13].hasOwnEvents, "day 13 owns service events");
    check(c.days[12].events.size() == 1, "day 12 has one service event");
    check(c.days[13].events.size() == 1, "day 13 has one service event");
    check(sameEvent(c.days[12].events[0], c.days[13].events[0]),
          "day 13 starts as a copy of day 12");
}

void reference_independence() {
    course::Course c = course::makeReferenceCourse();
    std::string error;
    course::Event changed = c.days[12].events[0];
    changed.description = "changed 12";
    check(course::updateEvent(c, 12, changed.id, changed, error), "update day 12");
    check(c.days[13].events[0].description == "Service breakfast",
          "day 13 is not changed by day 12");
    changed = c.days[13].events[0];
    changed.description = "changed 13";
    check(course::updateEvent(c, 13, changed.id, changed, error), "update day 13");
    check(c.days[12].events[0].description == "changed 12",
          "day 12 is not changed by day 13");
}

void date_leap_years() {
    course::Date a, b;
    std::string error;
    check(course::parseDate("2024-02-28", a, error), "parse 2024-02-28");
    check(course::parseDate("2024-02-29", b, error), "parse 2024-02-29");
    check(course::civilDaysBetween(a, b) == 1, "2024 leap day adds one day");
    check(course::parseDate("2024-03-01", b, error), "parse 2024-03-01");
    check(course::civilDaysBetween(a, b) == 2, "2024-02-28 to 2024-03-01 is 2");
    check(course::parseDate("2023-02-28", a, error), "parse 2023-02-28");
    check(course::parseDate("2023-03-01", b, error), "parse 2023-03-01");
    check(course::civilDaysBetween(a, b) == 1, "2023-02-28 to 2023-03-01 is 1");
    check(course::parseDate("2024-12-31", a, error), "parse 2024-12-31");
    check(course::parseDate("2025-01-01", b, error), "parse 2025-01-01");
    check(course::civilDaysBetween(a, b) == 1, "year boundary is one day");
    check(course::parseDate("2024-01-01", a, error), "parse 2024-01-01");
    check(course::parseDate("2025-01-01", b, error), "parse 2025-01-01");
    check(course::civilDaysBetween(a, b) == 366, "2024 is a leap year");
    check(course::parseDate("2023-01-01", a, error), "parse 2023-01-01");
    check(course::parseDate("2024-01-01", b, error), "parse 2024-01-01");
    check(course::civilDaysBetween(a, b) == 365, "2023 is a normal year");
}

void date_add_and_format() {
    course::Date d;
    std::string error;
    check(course::parseDate("2024-02-28", d, error), "parse start");
    check(course::formatDate(course::addCivilDays(d, 1)) == "2024-02-29",
          "add one day over leap");
    check(course::formatDate(course::addCivilDays(d, 2)) == "2024-03-01",
          "add two days over month");
    check(course::formatDate(course::addCivilDays(d, -1)) == "2024-02-27",
          "subtract one day");
    check(course::formatDate(course::addCivilDays(d, 366)) == "2025-02-28",
          "add a leap year");
}

void date_invalid() {
    course::Date d;
    std::string error;
    check(!course::parseDate("2024-02-30", d, error), "reject Feb 30");
    check(!course::parseDate("2023-02-29", d, error), "reject non-leap Feb 29");
    check(!course::parseDate("2024-13-01", d, error), "reject month 13");
    check(!course::parseDate("2024-00-01", d, error), "reject month 0");
    check(!course::parseDate("2024-01-00", d, error), "reject day 0");
    check(!course::parseDate("2024-1-1", d, error), "reject short format");
    check(!course::parseDate("abcd-01-01", d, error), "reject non-numeric");
    check(course::parseDate("2024-01-02", d, error), "accept YYYY-MM-DD");
    check(course::formatDate(d) == "2024-01-02", "format date");
}

void cycle_formula() {
    course::Date activation, today;
    std::string error;
    check(course::parseDate("2026-11-22", activation, error), "parse activation");
    check(course::parseDate("2026-11-22", today, error), "parse same day");
    int day = -1;
    check(course::computeDayIndex(13, activation, today, 14, day, error),
          "compute same day");
    check(day == 13, "same day stays 13");
    today = course::addCivilDays(activation, 1);
    check(course::computeDayIndex(13, activation, today, 14, day, error), "compute next day");
    check(day == 0, "13 -> 0");
    today = course::addCivilDays(activation, 13);
    check(course::computeDayIndex(13, activation, today, 14, day, error), "compute 13 days later");
    check(day == 12, "13 + 13 mod 14 = 12");
    today = course::addCivilDays(activation, 14);
    check(course::computeDayIndex(13, activation, today, 14, day, error), "compute 14 days later");
    check(day == 13, "full cycle returns to 13");
}

void cycle_lengths() {
    course::Date activation, today;
    std::string error;
    check(course::parseDate("2026-01-01", activation, error), "parse activation");
    for (int length = 1; length <= 47; ++length) {
        today = activation;
        int day = -1;
        check(course::computeDayIndex(length - 1, activation, today, length, day, error),
              "compute last day for length " + std::to_string(length));
        check(day == length - 1, "last day stays last for length " + std::to_string(length));
        today = course::addCivilDays(activation, 1);
        check(course::computeDayIndex(length - 1, activation, today, length, day, error),
              "compute wrap for length " + std::to_string(length));
        check(day == 0, "wrap to 0 for length " + std::to_string(length));
    }
    today = course::addCivilDays(activation, -1);
    int day = -1;
    check(!course::computeDayIndex(0, activation, today, 14, day, error),
          "reject date before activation");
}

void cycle_boundaries() {
    course::Date activation, today;
    std::string error;
    check(course::parseDate("2026-12-31", activation, error), "parse year end");
    check(course::parseDate("2027-01-01", today, error), "parse next year");
    int day = -1;
    check(course::computeDayIndex(13, activation, today, 14, day, error), "year boundary");
    check(day == 0, "year boundary wraps 13 -> 0");
    check(course::nextDayIndex(13, 14) == 0, "next 13 -> 0");
    check(course::nextDayIndex(46, 47) == 0, "next 46 -> 0");
    check(course::nextDayIndex(10, 11) == 0, "next 10 -> 0");
    check(course::nextDayIndex(0, 14) == 1, "next 0 -> 1");
    check(course::nextDayIndex(0, 0) == -1, "invalid length");
}

void insert_and_copy() {
    course::Course c = course::makeReferenceCourse();
    std::string error;
    const std::string first = c.days[0].events.empty() ? "template" : "events";
    check(course::insertBefore(c, 0, true, error), "insert copy before day 0");
    check(c.days.size() == 15, "size after insert");
    check(c.days[0].hasOwnEvents, "copy is materialised");
    check(c.days[0].events.size() == 1, "copy has source events");
    check(c.days[0].events[0].description == "Arrival", "copy content");
    check(c.days[1].templateName == "arrival", "old day 0 shifted to 1");
    check(course::insertAfter(c, 14, false, error), "insert empty after last");
    check(c.days.size() == 16, "size after second insert");
    check(c.days[15].hasOwnEvents && c.days[15].events.empty(), "empty day inserted");
    check(course::validate(c, error), "course still validates: " + error);
    (void)first;
}

void insert_after_and_invalid() {
    course::Course c = makeCourse(3);
    std::string error;
    check(course::insertBefore(c, 0, false, error), "insert empty before day 0");
    check(c.days.size() == 4, "size after empty insert");
    check(c.days[0].hasOwnEvents && c.days[0].events.empty(), "empty day has no events");
    check(c.days[1].events[0].description == "day 0", "old day 0 shifted");
    check(course::insertAfter(c, 2, true, error), "insert copy after day 2");
    check(c.days.size() == 5, "size after after-insert");
    check(c.days[3].events[0].description == "day 1", "copy after uses source content");
    check(course::validate(c, error), "course after inserts validates: " + error);
    check(!course::insertBefore(c, 99, false, error), "reject invalid before index");
    check(!course::insertAfter(c, 99, false, error), "reject invalid after index");
    check(!course::deleteDay(c, 99, error), "reject invalid delete index");
    check(!course::clearDay(c, 99, error), "reject invalid clear index");
}

void insert_max_and_invalid() {
    course::Course c = makeCourse(47);
    std::string error;
    check(!course::insertBefore(c, 0, false, error), "cannot exceed 47 days");
    check(!course::insertBefore(c, 47, false, error), "invalid before index");
    check(!course::insertAfter(c, 47, false, error), "invalid after index");
    course::Course d = makeCourse(2);
    check(course::insertBefore(d, 0, true, error), "copy from own day");
    check(d.days[0].events[0].description == "day 0", "copy event content");
}

void delete_and_clear() {
    course::Course c = course::makeReferenceCourse();
    std::string error;
    check(course::deleteDay(c, 0, error), "delete day 0");
    check(c.days.size() == 13, "size after delete");
    check(c.days[0].templateName == "practice", "old day 1 becomes day 0");
    check(course::deleteDay(c, 12, error), "delete last day");
    check(c.days.size() == 12, "size after last delete");
    course::Course one = makeCourse(1);
    check(!course::deleteDay(one, 0, error), "cannot delete last remaining day");
    check(!course::deleteDay(c, 99, error), "reject invalid delete");
    course::Course t = course::makeReferenceCourse();
    check(course::clearDay(t, 1, error), "clear template day");
    check(t.days[1].hasOwnEvents, "cleared day is materialised");
    check(t.days[1].events.empty(), "cleared day has no events");
    check(t.days[2].templateName == "practice", "other day unchanged");
    check(course::validate(t, error), "cleared course validates");
}

void event_editing() {
    course::Course c = course::makeReferenceCourse();
    std::string error;
    course::Event e = event(8, 30, "new event", 0);
    check(course::addEvent(c, 0, e, error), "add event to template day");
    check(c.days[0].hasOwnEvents, "day materialised on add");
    check(c.days[0].events.size() == 2, "event added");
    check(c.templates["arrival"].size() == 1, "template unchanged");
    check(c.days[0].events[1].id != 0, "auto id assigned");
    check(!course::addEvent(c, 0, event(19, 45, "duplicate"), error),
          "duplicate time rejected");
    check(!course::addEvent(c, 0, event(9, 0, "duplicate id", 2), error),
          "duplicate id rejected");
    check(!course::addEvent(c, 0, event(24, 0, "bad hour"), error),
          "bad hour rejected");
    check(!course::addEvent(c, 0, event(9, 60, "bad minute"), error),
          "bad minute rejected");
    course::Event changed = c.days[0].events[1];
    changed.description = "edited";
    check(course::updateEvent(c, 0, changed.id, changed, error), "update event");
    check(c.days[0].events[1].description == "edited", "event updated");
    check(course::setEventEnabled(c, 0, changed.id, false, error), "disable event");
    check(!c.days[0].events[1].enabled, "event disabled");
    check(course::removeEvent(c, 0, changed.id, error), "remove event");
    check(c.days[0].events.size() == 1, "event removed");
    check(!course::removeEvent(c, 0, changed.id, error), "remove missing event");
    check(!course::updateEvent(c, 0, 9999, changed, error), "update missing event");
}

void event_limits() {
    course::Course c = makeCourse(1);
    std::string error;
    for (int i = 0; i < 31; ++i) {
        check(course::addEvent(c, 0, event(5, i, "fill", 0), error),
              "fill event " + std::to_string(i));
    }
    check(c.days[0].events.size() == 32, "day has 32 events");
    check(!course::addEvent(c, 0, event(6, 0, "overflow", 0), error),
          "33rd event rejected");
    course::Event e = event(7, 0, "desc");
    e.track = 99;
    e.loop = 7;
    e.volume = 30;
    e.description = std::string(96, 'x');
    check(course::validateEvent(e, error), "boundary event validates");
    e.description = std::string(97, 'x');
    check(!course::validateEvent(e, error), "97-byte description rejected");
    e.description = "x";
    e.track = 0;
    check(!course::validateEvent(e, error), "track 0 rejected");
    e.track = 100;
    check(!course::validateEvent(e, error), "track 100 rejected");
    e.track = 1;
    e.loop = 0;
    check(!course::validateEvent(e, error), "loop 0 rejected");
    e.loop = 8;
    check(!course::validateEvent(e, error), "loop 8 rejected");
    e.loop = 1;
    e.volume = 31;
    check(!course::validateEvent(e, error), "volume 31 rejected");
    e.volume = 30;
    e.id = 0;
    check(!course::validateEvent(e, error), "id 0 rejected");
}

void json_v1_import() {
    const std::string json = R"json({
      "format":"gong-course","version":1,"name":"v1","generation":2,
      "templates":{"A":[{"id":1,"hour":7,"min":0,"track":1,"loop":1,"vol":30,"en":true,"desc":"A"}],
                   "B":[{"id":1,"hour":8,"min":0,"track":2,"loop":2,"vol":20,"en":false,"desc":"B"}]},
      "days":["A",[{"id":5,"hour":9,"min":30,"track":3,"loop":3,"vol":10,"en":true,"desc":"inline"}],"B"]
    })json";
    course::Course c;
    std::string error;
    check(course::fromJson(json, c, error), "v1 import: " + error);
    check(c.days.size() == 3, "v1 days count");
    check(c.days[0].templateName == "A", "v1 template day");
    check(c.days[1].hasOwnEvents, "v1 inline day");
    check(c.days[1].events[0].description == "inline", "v1 inline event");
    check(c.days[2].templateName == "B", "v1 second template");
    check(c.generation == 2, "v1 generation");
    check(c.name == "v1", "v1 name");
    std::string out = course::toJsonV2(c, false, error);
    check(!out.empty(), "v1 to v2 export");
    course::Course round;
    check(course::fromJson(out, round, error), "v2 roundtrip: " + error);
    check(round.days.size() == 3, "roundtrip days");
    check(round.days[0].templateName == "A", "roundtrip template");
    check(round.days[1].events[0].description == "inline", "roundtrip inline");
}

void json_v2_import_and_both() {
    const std::string json = R"json({
      "format":"gong-course","version":2,"name":"v2","generation":3,"course_length":2,
      "templates":{"A":[{"id":1,"hour":7,"min":0,"desc":"A"}]},
      "days":[{"template":"A"},{"template":"A","events":[{"id":2,"hour":9,"min":0,"desc":"own"}]}]
    })json";
    course::Course c;
    std::string error;
    check(course::fromJson(json, c, error), "v2 import: " + error);
    check(c.days.size() == 2, "v2 days count");
    check(c.days[0].templateName == "A", "v2 template day");
    check(c.days[1].hasOwnEvents, "v2 both uses own events");
    check(c.days[1].events[0].description == "own", "v2 own event wins");
    check(c.days[1].templateName == "A", "v2 retains template reference");
    std::vector<course::Event> resolved;
    check(course::resolvedEvents(c, 1, resolved, error), "resolve both");
    check(resolved.size() == 1 && resolved[0].description == "own", "resolved uses own");
    check(course::validate(c, error), "v2 both validates");
}

void json_roundtrip_and_expand() {
    course::Course c = course::makeReferenceCourse();
    std::string error;
    std::string json = course::toJsonV2(c, false, error);
    check(!json.empty(), "reference export");
    course::Course round;
    check(course::fromJson(json, round, error), "reference roundtrip: " + error);
    check(round.days.size() == 14, "roundtrip size");
    check(round.days[0].templateName == "arrival", "roundtrip template preserved");
    check(round.days[12].hasOwnEvents, "roundtrip own day preserved");
    std::string expanded = course::toJsonV2(c, true, error);
    check(!expanded.empty(), "expanded export");
    course::Course flat;
    check(course::fromJson(expanded, flat, error), "expanded import: " + error);
    check(flat.days[0].hasOwnEvents, "expanded day owns events");
    check(flat.days[0].events.size() == 1, "expanded event count");
    check(flat.templates.empty(), "expanded export has no templates");
}

void json_errors() {
    course::Course c;
    std::string error;
    check(!course::fromJson("{}", c, error), "missing format");
    check(!course::fromJson("{\"format\":\"x\",\"version\":2,\"days\":[]}", c, error),
          "wrong format");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":3,\"days\":[]}", c, error),
          "wrong version");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":2,\"days\":[]}", c, error),
          "empty days");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":2,\"days\":[{}],\"course_length\":2}", c, error),
          "course length mismatch");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":2,\"days\":[{}],\"extra\":1}", c, error),
          "unknown top field");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":2,\"days\":[{\"events\":[{\"hour\":1,\"min\":0,\"bad\":1}]}]}", c, error),
          "unknown event field");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":2,\"days\":[{\"events\":[{\"hour\":24,\"min\":0}]}]}", c, error),
          "bad hour");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":2,\"days\":[{\"events\":[{\"hour\":1,\"min\":0},{\"hour\":1,\"min\":0}]}]}", c, error),
          "duplicate time");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":2,\"days\":[{\"events\":[{\"id\":1,\"hour\":1,\"min\":0},{\"id\":1,\"hour\":2,\"min\":0}]}]}", c, error),
          "duplicate id");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":2,\"days\":[{\"template\":\"missing\"}]}", c, error),
          "unknown template");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":2,\"days\":[{\"events\":[{\"hour\":1,\"min\":0,\"desc\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}]}]}", c, error),
          "description too long");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":2,\"days\":[{\"events\":[{\"hour\":1,\"min\":0,\"vol\":31}]}]}", c, error),
          "volume 31");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":2,\"days\":[{\"events\":[{\"hour\":1,\"min\":0,\"track\":0}]}]}", c, error),
          "track 0");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":2,\"days\":[{\"events\":[{\"hour\":1,\"min\":0,\"loop\":8}]}]}", c, error),
          "loop 8");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":2,\"days\":[{\"events\":[{\"hour\":1,\"min\":0}]},{\"events\":[{\"hour\":2,\"min\":0}]}]},", c, error),
          "trailing garbage");
    check(!course::fromJson("{\"format\":\"gong-course\",\"version\":2,\"days\":[{\"events\":[{\"hour\":1,\"min\":0}]}]", c, error),
          "unterminated object");
}

void json_duplicate_key_and_unicode() {
    course::Course c;
    std::string error;
    check(!course::fromJson("{\"format\":\"gong-course\",\"format\":\"gong-course\",\"version\":2,\"days\":[{}]}", c, error),
          "duplicate key rejected");
    const std::string json = R"json({"format":"gong-course","version":2,"name":"caf\u00e9","days":[{"events":[{"id":1,"hour":1,"min":0,"desc":"caf\u00e9 \u2603"}]}]})json";
    check(course::fromJson(json, c, error), "unicode import: " + error);
    check(c.name == "caf\xc3\xa9", "unicode name");
    check(c.days[0].events[0].description == "caf\xc3\xa9 \xe2\x98\x83",
          "unicode description");
    std::string out = course::toJsonV2(c, false, error);
    check(!out.empty(), "unicode export");
    course::Course round;
    check(course::fromJson(out, round, error), "unicode roundtrip");
    check(round.days[0].events[0].description == c.days[0].events[0].description,
          "unicode roundtrip description");
}

void json_auto_ids_and_defaults() {
    const std::string json = R"json({"format":"gong-course","version":2,"days":[{"events":[{"hour":1,"min":2},{"hour":3,"min":4}]}]})json";
    course::Course c;
    std::string error;
    check(course::fromJson(json, c, error), "defaults import: " + error);
    check(c.days[0].events.size() == 2, "two events");
    check(c.days[0].events[0].id != 0 && c.days[0].events[1].id != 0, "auto ids");
    check(c.days[0].events[0].id != c.days[0].events[1].id, "auto ids unique");
    check(c.days[0].events[0].track == 1, "default track");
    check(c.days[0].events[0].loop == 1, "default loop");
    check(c.days[0].events[0].volume == 30, "default volume");
    check(c.days[0].events[0].enabled, "default enabled");
}

void simulate_47_days() {
    course::Course c = makeCourse(47);
    std::string error;
    course::Date activation;
    check(course::parseDate("2024-02-01", activation, error), "parse activation");
    for (int i = 0; i < 47; ++i) {
        const course::Date today = course::addCivilDays(activation, i);
        int day = -1;
        check(course::computeDayIndex(0, activation, today, 47, day, error),
              "simulate compute day " + std::to_string(i));
        check(day == i, "simulate day index " + std::to_string(i));
        check(course::civilDaysBetween(activation, today) == i,
              "simulate civil distance " + std::to_string(i));
    }
    const course::Date after = course::addCivilDays(activation, 47);
    int day = -1;
    check(course::computeDayIndex(0, activation, after, 47, day, error),
          "simulate wrap");
    check(day == 0, "47-day wrap to 0");
    check(course::formatDate(course::addCivilDays(activation, 28)) == "2024-02-29",
          "simulation crosses leap day");
}

void validation_errors() {
    course::Course c;
    std::string error;
    check(!course::validate(c, error), "empty course rejected");
    c = makeCourse(48);
    check(!course::validate(c, error), "48 days rejected");
    c = makeCourse(1);
    c.days[0].hasOwnEvents = false;
    c.days[0].templateName.clear();
    check(!course::validate(c, error), "day without events or template");
    c = makeCourse(1);
    c.days[0].templateName = "missing";
    check(!course::validate(c, error), "unknown template");
    c = makeCourse(1);
    c.days[0].hasOwnEvents = false;
    c.days[0].templateName = "A";
    c.templates["A"].push_back(event(1, 0, "A"));
    c.days[0].events.push_back(event(2, 0, "stray"));
    check(!course::validate(c, error), "template day with stray events");
}

void template_copy_on_write() {
    course::Course c = course::makeReferenceCourse();
    std::string error;
    course::Event changed = c.templates["practice"][0];
    changed.description = "changed template";
    check(course::updateEvent(c, 1, changed.id, changed, error), "update template day");
    check(c.days[1].hasOwnEvents, "day 1 materialised");
    check(c.templates["practice"][0].description == "Wake-up",
          "template not modified");
    check(c.days[2].templateName == "practice", "day 2 still references template");
    std::vector<course::Event> resolved;
    check(course::resolvedEvents(c, 2, resolved, error), "resolve day 2");
    check(resolved[0].description == "Wake-up", "day 2 sees original template");
}

// formatVersion() is what the Web GUI reports before importing a file: it must
// read v1/v2 and reject anything else without parsing the days.
void json_format_version() {
    int version = 0;
    std::string error;
    check(course::formatVersion("{\"format\":\"gong-course\",\"version\":1,\"days\":[]}",
                                version, error) && version == 1,
          "v1 version detected");
    check(course::formatVersion("{\"format\":\"gong-course\",\"version\":2,\"days\":[]}",
                                version, error) && version == 2,
          "v2 version detected");
    version = 0;
    check(!course::formatVersion("{\"format\":\"gong-course\",\"version\":3,\"days\":[]}",
                                 version, error),
          "unknown version rejected");
    check(!course::formatVersion("{\"format\":\"other\",\"version\":1}", version, error),
          "foreign format rejected");
    check(!course::formatVersion("{\"version\":1}", version, error), "missing format rejected");
    check(!course::formatVersion("{\"format\":\"gong-course\"}", version, error),
          "missing version rejected");
    check(!course::formatVersion("[]", version, error), "array root rejected");
    check(!course::formatVersion("{bad", version, error), "broken JSON rejected");
}

}  // namespace

int main() {
    runGroup("reference_structure", reference_structure);
    runGroup("reference_independence", reference_independence);
    runGroup("date_leap_years", date_leap_years);
    runGroup("date_add_and_format", date_add_and_format);
    runGroup("date_invalid", date_invalid);
    runGroup("cycle_formula", cycle_formula);
    runGroup("cycle_lengths", cycle_lengths);
    runGroup("cycle_boundaries", cycle_boundaries);
    runGroup("insert_and_copy", insert_and_copy);
    runGroup("insert_after_and_invalid", insert_after_and_invalid);
    runGroup("insert_max_and_invalid", insert_max_and_invalid);
    runGroup("delete_and_clear", delete_and_clear);
    runGroup("event_editing", event_editing);
    runGroup("event_limits", event_limits);
    runGroup("json_v1_import", json_v1_import);
    runGroup("json_v2_import_and_both", json_v2_import_and_both);
    runGroup("json_roundtrip_and_expand", json_roundtrip_and_expand);
    runGroup("json_errors", json_errors);
    runGroup("json_duplicate_key_and_unicode", json_duplicate_key_and_unicode);
    runGroup("json_auto_ids_and_defaults", json_auto_ids_and_defaults);
    runGroup("simulate_47_days", simulate_47_days);
    runGroup("validation_errors", validation_errors);
    runGroup("template_copy_on_write", template_copy_on_write);
    runGroup("json_format_version", json_format_version);

    std::cout << "SUMMARY pass=" << g_pass << " fail=" << g_fail << "\n";
    return g_fail == 0 ? 0 : 1;
}
