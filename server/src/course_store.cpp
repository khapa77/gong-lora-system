#include "course_store.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace course_store {
namespace {

const char* kCourseDir = "/course";
const char* kNamespace = "gong";
const char* kCalendarKey = "course_state";
const char* kBootstrapKey = "course_bootstrap";
const char* kLastFireKey = "last_fire";
const char* kMetaBase = "meta";

std::string toStd(const String& value) {
    return std::string(value.c_str(), value.length());
}

String toArduino(const std::string& value) {
    return String(value.c_str());
}

std::string hex32(std::uint32_t value) {
    char buffer[9];
    std::snprintf(buffer, sizeof(buffer), "%08lx",
                  static_cast<unsigned long>(value));
    return std::string(buffer);
}

bool parseU32(const std::string& text, std::uint32_t& out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    if (value > 0xFFFFFFFFul) return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool parseHex32(const std::string& text, std::uint32_t& out) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 16);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    if (value > 0xFFFFFFFFul) return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

std::vector<std::string> splitSemicolon(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ';') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(text[i]);
        }
    }
    parts.push_back(current);
    return parts;
}

std::string makeEnvelope(const std::string& json) {
    return "CRC32 " + hex32(CourseStore::crc32(json)) + "\n" + json;
}

bool readEnvelope(const std::string& raw, std::string& json, std::string& error) {
    const std::size_t newline = raw.find('\n');
    if (newline == std::string::npos) {
        error = "missing CRC header";
        return false;
    }
    std::string header = raw.substr(0, newline);
    if (!header.empty() && header[header.size() - 1] == '\r') {
        header.erase(header.size() - 1);
    }
    if (header.size() < 7 || header.compare(0, 6, "CRC32 ") != 0) {
        error = "invalid CRC header";
        return false;
    }
    std::uint32_t expected = 0;
    if (!parseHex32(header.substr(6), expected)) {
        error = "invalid CRC value";
        return false;
    }
    json = raw.substr(newline + 1);
    const std::uint32_t actual = CourseStore::crc32(json);
    if (actual != expected) {
        error = "CRC mismatch";
        return false;
    }
    return true;
}

bool readFileWithCrc(const std::string& path, std::string& json,
                     std::string& error) {
    error.clear();
    File file = LittleFS.open(path.c_str(), "r");
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    const String raw = file.readString();
    file.close();
    return readEnvelope(toStd(raw), json, error);
}

bool writeFileWithCrc(const std::string& path, const std::string& json,
                      std::string& error) {
    error.clear();
    File file = LittleFS.open(path.c_str(), "w");
    if (!file) {
        error = "cannot create " + path;
        return false;
    }
    const std::string envelope = makeEnvelope(json);
    const size_t written = file.write(
        reinterpret_cast<const uint8_t*>(envelope.data()), envelope.size());
    file.close();
    if (written != envelope.size()) {
        LittleFS.remove(path.c_str());
        error = "short write to " + path;
        return false;
    }
    std::string check;
    if (!readFileWithCrc(path, check, error)) {
        LittleFS.remove(path.c_str());
        return false;
    }
    if (check != json) {
        LittleFS.remove(path.c_str());
        error = "verification mismatch for " + path;
        return false;
    }
    return true;
}

std::string generationDir(std::uint32_t generation) {
    return std::string(kCourseDir) + "/g" + std::to_string(generation);
}

std::string basePath(std::uint32_t generation, const std::string& name) {
    return generationDir(generation) + "/" + name;
}

std::string dayBase(std::uint32_t generation, std::uint8_t day) {
    char name[8];
    std::snprintf(name, sizeof(name), "d%02u", static_cast<unsigned>(day));
    return basePath(generation, name);
}

std::string metaBase(std::uint32_t generation) {
    return basePath(generation, kMetaBase);
}

bool ensureGenerationDir(std::uint32_t generation) {
    LittleFS.mkdir(kCourseDir);
    LittleFS.mkdir(generationDir(generation).c_str());
    return true;
}

bool writeAtomic(const std::string& base, const std::string& json,
                 std::string& error) {
    const std::string path = base + ".json";
    const std::string tmp = base + ".tmp";
    const std::string bak = base + ".bak";

    if (!writeFileWithCrc(tmp, json, error)) return false;

    if (LittleFS.exists(path.c_str())) {
        if (LittleFS.exists(bak.c_str())) {
            if (!LittleFS.remove(bak.c_str())) {
                LittleFS.remove(tmp.c_str());
                error = "cannot remove old backup " + bak;
                return false;
            }
        }
        if (!LittleFS.rename(path.c_str(), bak.c_str())) {
            LittleFS.remove(tmp.c_str());
            error = "cannot back up " + path;
            return false;
        }
    }

    if (!LittleFS.rename(tmp.c_str(), path.c_str())) {
        if (LittleFS.exists(bak.c_str())) {
            LittleFS.rename(bak.c_str(), path.c_str());
        }
        error = "cannot publish " + path;
        return false;
    }
    LittleFS.remove(bak.c_str());
    return true;
}

bool recoverBase(const std::string& base, bool allowTmpRestore,
                 std::string& error) {
    const std::string path = base + ".json";
    const std::string tmp = base + ".tmp";
    const std::string bak = base + ".bak";

    std::string json;
    if (LittleFS.exists(path.c_str())) {
        if (readFileWithCrc(path, json, error)) {
            LittleFS.remove(tmp.c_str());
            LittleFS.remove(bak.c_str());
            return true;
        }
        if (LittleFS.exists(bak.c_str())) {
            std::string backup;
            if (readFileWithCrc(bak, backup, error)) {
                if (!LittleFS.remove(path.c_str())) {
                    error = "cannot remove damaged " + path;
                    return false;
                }
                if (!LittleFS.rename(bak.c_str(), path.c_str())) {
                    error = "cannot restore " + bak;
                    return false;
                }
                LittleFS.remove(tmp.c_str());
                return true;
            }
        }
        if (allowTmpRestore && LittleFS.exists(tmp.c_str())) {
            std::string pending;
            if (readFileWithCrc(tmp, pending, error)) {
                if (!LittleFS.remove(path.c_str())) {
                    error = "cannot remove damaged " + path;
                    return false;
                }
                if (!LittleFS.rename(tmp.c_str(), path.c_str())) {
                    error = "cannot publish " + tmp;
                    return false;
                }
                return true;
            }
        }
        error = "damaged file without valid backup: " + path;
        return false;
    }

    if (LittleFS.exists(bak.c_str())) {
        std::string backup;
        if (readFileWithCrc(bak, backup, error)) {
            if (!LittleFS.rename(bak.c_str(), path.c_str())) {
                error = "cannot restore " + bak;
                return false;
            }
            LittleFS.remove(tmp.c_str());
            return true;
        }
    }

    if (allowTmpRestore && LittleFS.exists(tmp.c_str())) {
        std::string pending;
        if (readFileWithCrc(tmp, pending, error)) {
            if (!LittleFS.rename(tmp.c_str(), path.c_str())) {
                error = "cannot publish " + tmp;
                return false;
            }
            return true;
        }
    }

    error = "missing file: " + path;
    return false;
}

bool parseMetaJson(const std::string& json, CourseMeta& out, std::string& error) {
    DynamicJsonDocument doc(2048);
    const DeserializationError result = deserializeJson(doc, json);
    if (result) {
        error = "meta JSON parse error";
        return false;
    }
    if (doc.overflowed()) {
        error = "meta JSON too large";
        return false;
    }
    if (!doc["format"].is<const char*>() ||
        std::string(doc["format"].as<const char*>()) != "gong-course-store") {
        error = "invalid meta format";
        return false;
    }
    if (!doc["version"].is<std::uint32_t>() ||
        doc["version"].as<std::uint32_t>() != 1) {
        error = "invalid meta version";
        return false;
    }
    if (!doc["generation"].is<std::uint32_t>() ||
        !doc["course_length"].is<std::uint32_t>()) {
        error = "invalid meta fields";
        return false;
    }
    const std::uint32_t generation = doc["generation"].as<std::uint32_t>();
    const std::uint32_t length = doc["course_length"].as<std::uint32_t>();
    if (generation == 0 || length < 1 || length > course::kMaxDays) {
        error = "meta values out of range";
        return false;
    }
    if (!doc["name"].is<const char*>()) {
        error = "missing meta name";
        return false;
    }
    out.generation = generation;
    out.course_length = static_cast<std::uint8_t>(length);
    out.name = doc["name"].as<const char*>();
    return true;
}

std::string makeMetaJson(const CourseMeta& meta, std::string& error) {
    DynamicJsonDocument doc(2048);
    doc["format"] = "gong-course-store";
    doc["version"] = 1;
    doc["generation"] = meta.generation;
    doc["course_length"] = meta.course_length;
    doc["name"] = meta.name;
    if (doc.overflowed()) {
        error = "meta JSON too large";
        return std::string();
    }
    String output;
    serializeJson(doc, output);
    return toStd(output);
}

bool readCalendarBlob(const std::string& blob, CalendarState& out,
                      std::string& error) {
    const std::vector<std::string> parts = splitSemicolon(blob);
    if (parts.size() != 9) {
        error = "calendar field count mismatch";
        return false;
    }
    if (parts[0] != "v1") {
        error = "unsupported calendar version";
        return false;
    }
    std::uint32_t active = 0;
    std::uint32_t draft = 0;
    std::uint32_t date = 0;
    std::uint32_t day = 0;
    std::uint32_t length = 0;
    std::uint32_t flags = 0;
    std::uint32_t failures = 0;
    std::uint32_t expectedCrc = 0;
    if (!parseU32(parts[1], active) || !parseU32(parts[2], draft) ||
        !parseU32(parts[3], date) || !parseU32(parts[4], day) ||
        !parseU32(parts[5], length) || !parseU32(parts[6], flags) ||
        !parseU32(parts[7], failures) || !parseHex32(parts[8], expectedCrc)) {
        error = "calendar numeric parse error";
        return false;
    }
    std::string payload;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (i) payload += ';';
        payload += parts[i];
    }
    if (CourseStore::crc32(payload) != expectedCrc) {
        error = "calendar CRC mismatch";
        return false;
    }
    if (day > 255 || length > 255 || flags > 255 || failures > 255) {
        error = "calendar value out of range";
        return false;
    }
    out.active_generation = active;
    out.draft_generation = draft;
    out.activation_date = date;
    out.day_index = static_cast<std::uint8_t>(day);
    out.course_length = static_cast<std::uint8_t>(length);
    out.flags = static_cast<std::uint8_t>(flags);
    out.failure_count = static_cast<std::uint8_t>(failures);
    return true;
}

std::string makeCalendarBlob(const CalendarState& state) {
    std::string payload = "v1;";
    payload += std::to_string(state.active_generation);
    payload += ';';
    payload += std::to_string(state.draft_generation);
    payload += ';';
    payload += std::to_string(state.activation_date);
    payload += ';';
    payload += std::to_string(static_cast<unsigned>(state.day_index));
    payload += ';';
    payload += std::to_string(static_cast<unsigned>(state.course_length));
    payload += ';';
    payload += std::to_string(static_cast<unsigned>(state.flags));
    payload += ';';
    payload += std::to_string(static_cast<unsigned>(state.failure_count));
    return payload + ";" + hex32(CourseStore::crc32(payload));
}

bool readLastFireBlob(const std::string& blob, LastFire& out,
                      std::string& error) {
    const std::vector<std::string> parts = splitSemicolon(blob);
    if (parts.size() != 8) {
        error = "last_fire field count mismatch";
        return false;
    }
    if (parts[0] != "v1") {
        error = "unsupported last_fire version";
        return false;
    }
    std::uint32_t generation = 0;
    std::uint32_t day = 0;
    std::uint32_t minute = 0;
    std::uint32_t event = 0;
    std::uint32_t date = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t expectedCrc = 0;
    if (!parseU32(parts[1], generation) || !parseU32(parts[2], day) ||
        !parseU32(parts[3], minute) || !parseU32(parts[4], event) ||
        !parseU32(parts[5], date) || !parseU32(parts[6], timestamp) ||
        !parseHex32(parts[7], expectedCrc)) {
        error = "last_fire numeric parse error";
        return false;
    }
    std::string payload;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (i) payload += ';';
        payload += parts[i];
    }
    if (CourseStore::crc32(payload) != expectedCrc) {
        error = "last_fire CRC mismatch";
        return false;
    }
    if (day > 255 || minute > 1439) {
        error = "last_fire value out of range";
        return false;
    }
    out.generation = generation;
    out.day_index = static_cast<std::uint8_t>(day);
    out.minute = static_cast<std::uint16_t>(minute);
    out.event_id = event;
    out.date = date;
    out.timestamp = timestamp;
    return true;
}

std::string makeLastFireBlob(const LastFire& value) {
    std::string payload = "v1;";
    payload += std::to_string(value.generation);
    payload += ';';
    payload += std::to_string(static_cast<unsigned>(value.day_index));
    payload += ';';
    payload += std::to_string(static_cast<unsigned>(value.minute));
    payload += ';';
    payload += std::to_string(value.event_id);
    payload += ';';
    payload += std::to_string(value.date);
    payload += ';';
    payload += std::to_string(value.timestamp);
    return payload + ";" + hex32(CourseStore::crc32(payload));
}

}  // namespace

CalendarState::CalendarState()
    : active_generation(0),
      draft_generation(0),
      activation_date(0),
      day_index(0),
      course_length(0),
      flags(0),
      failure_count(0) {}

LastFire::LastFire()
    : generation(0),
      day_index(0),
      minute(0),
      event_id(0),
      date(0),
      timestamp(0) {}

CourseMeta::CourseMeta() : generation(0), course_length(0), name() {}

CourseStore::CourseStore() {}

std::uint32_t CourseStore::crc32(const std::string& data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < data.size(); ++i) {
        crc ^= static_cast<std::uint8_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                static_cast<std::uint32_t>(-(static_cast<int>(crc & 1u)));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

namespace {
bool readCalendarIfPresent(CalendarState& state, bool& present,
                           std::string& error) {
    present = false;
    error.clear();
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "cannot open NVS";
        return false;
    }
    if (!prefs.isKey(kCalendarKey)) {
        prefs.end();
        return true;
    }
    present = true;
    String blob = prefs.getString(kCalendarKey, "");
    prefs.end();
    return readCalendarBlob(toStd(blob), state, error);
}

}  // namespace

bool CourseStore::begin(std::string& error) {
    error.clear();
    if (!LittleFS.mkdir(kCourseDir)) {
        // mkdir returns false when the directory already exists on some
        // Arduino cores; that is not an error.
    }
    return true;
}

bool CourseStore::hasCalendar() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return false;
    const bool present = prefs.isKey(kCalendarKey);
    prefs.end();
    return present;
}

std::uint32_t CourseStore::loadBootstrapGeneration() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return 0;
    const std::uint32_t generation = prefs.getUInt(kBootstrapKey, 0);
    prefs.end();
    return generation;
}

bool CourseStore::saveBootstrapGeneration(std::uint32_t generation,
                                          std::string& error) {
    error.clear();
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "cannot open NVS";
        return false;
    }
    bool ok = true;
    if (generation == 0) {
        // remove() reports false for an absent key; clearing nothing is fine.
        if (prefs.isKey(kBootstrapKey)) ok = prefs.remove(kBootstrapKey);
    } else {
        ok = prefs.putUInt(kBootstrapKey, generation) == sizeof(generation);
    }
    prefs.end();
    if (!ok) {
        error = "bootstrap pointer write failed";
        return false;
    }
    return true;
}

bool CourseStore::loadCalendar(CalendarState& out, std::string& error) {
    error.clear();
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
        error = "cannot open NVS";
        return false;
    }
    if (!prefs.isKey(kCalendarKey)) {
        prefs.end();
        error = "calendar not found";
        return false;
    }
    const String raw = prefs.getString(kCalendarKey, "");
    prefs.end();
    return readCalendarBlob(toStd(raw), out, error);
}

bool CourseStore::saveCalendar(const CalendarState& state, std::string& error) {
    error.clear();
    const std::string blob = makeCalendarBlob(state);
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "cannot open NVS";
        return false;
    }
    const size_t written = prefs.putString(kCalendarKey, toArduino(blob));
    prefs.end();
    if (written != blob.size()) {
        error = "calendar NVS write failed";
        return false;
    }
    return true;
}

bool CourseStore::loadMeta(std::uint32_t generation, CourseMeta& out,
                           std::string& error) {
    error.clear();
    if (generation == 0) {
        error = "invalid generation";
        return false;
    }
    std::string json;
    if (!readFileWithCrc(metaBase(generation) + ".json", json, error)) {
        return false;
    }
    if (!parseMetaJson(json, out, error)) return false;
    if (out.generation != generation) {
        error = "meta generation mismatch";
        return false;
    }
    return true;
}

bool CourseStore::loadDay(std::uint32_t generation, std::uint8_t day,
                          course::Day& out, std::string& error) {
    error.clear();
    if (generation == 0 || day >= course::kMaxDays) {
        error = "day index out of range";
        return false;
    }
    std::string json;
    if (!readFileWithCrc(dayBase(generation, day) + ".json", json, error)) {
        return false;
    }
    course::Course one;
    if (!course::fromJson(json, one, error)) return false;
    if (one.generation != generation) {
        error = "day generation mismatch";
        return false;
    }
    if (one.days.size() != 1) {
        error = "day file must contain one day";
        return false;
    }
    out = one.days[0];
    return true;
}

bool CourseStore::saveDayAtomic(std::uint32_t generation, std::uint8_t day,
                                const course::Day& dayData,
                                std::string& error) {
    error.clear();
    if (generation == 0 || day >= course::kMaxDays) {
        error = "day index out of range";
        return false;
    }
    if (!dayData.hasOwnEvents) {
        error = "day must be materialised before storage";
        return false;
    }
    course::Day stored = dayData;
    stored.templateName.clear();
    course::Course one;
    one.name = "day";
    one.generation = generation;
    one.days.push_back(stored);
    if (!course::validate(one, error)) return false;
    const std::string json = course::toJsonV2(one, true, error);
    if (json.empty()) return false;
    return writeAtomic(dayBase(generation, day), json, error);
}

bool CourseStore::applyCourse(const course::Course& course,
                              std::uint32_t& newGeneration,
                              CalendarState& outState, std::string& error) {
    error.clear();
    if (!course::validate(course, error)) return false;
    if (course.days.empty() || course.days.size() > course::kMaxDays) {
        error = "course length out of range";
        return false;
    }

    CalendarState oldState;
    bool hasOld = false;
    if (!readCalendarIfPresent(oldState, hasOld, error)) return false;

    std::uint32_t generation = 1;
    if (hasOld && oldState.active_generation > 0) {
        generation = oldState.active_generation + 1;
    }
    for (unsigned guard = 0; guard < 10000; ++guard) {
        if (!LittleFS.exists((metaBase(generation) + ".json").c_str())) break;
        ++generation;
    }
    if (generation == 0) generation = 1;
    if (!ensureGenerationDir(generation)) {
        error = "cannot create generation directory";
        return false;
    }

    for (std::size_t i = 0; i < course.days.size(); ++i) {
        std::vector<course::Event> events;
        if (!course::resolvedEvents(course, i, events, error)) return false;
        course::Day flat;
        flat.hasOwnEvents = true;
        flat.events = events;
        if (!saveDayAtomic(generation, static_cast<std::uint8_t>(i), flat,
                           error)) {
            return false;
        }
    }

    CourseMeta meta;
    meta.generation = generation;
    meta.course_length = static_cast<std::uint8_t>(course.days.size());
    meta.name = course.name;
    std::string metaJson = makeMetaJson(meta, error);
    if (metaJson.empty()) return false;
    if (!writeAtomic(metaBase(generation), metaJson, error)) return false;

    if (!validateGeneration(generation, meta.course_length, error)) return false;

    CalendarState next;
    next.active_generation = generation;
    next.draft_generation = generation;
    next.activation_date = 0;
    next.day_index = 0;
    next.course_length = meta.course_length;
    next.flags = kApplied;
    next.failure_count = 0;
    if (!saveCalendar(next, error)) return false;

    if (hasOld) {
        if (oldState.active_generation > 0 &&
            oldState.active_generation != generation) {
            deleteGeneration(oldState.active_generation, error);
        }
        if (oldState.draft_generation > 0 &&
            oldState.draft_generation != generation &&
            oldState.draft_generation != oldState.active_generation) {
            deleteGeneration(oldState.draft_generation, error);
        }
    }
    error.clear();
    newGeneration = generation;
    outState = next;
    return true;
}

bool CourseStore::saveDraftCourse(const course::Course& course,
                                  std::uint32_t& draftGeneration,
                                  CalendarState& outState,
                                  std::string& error) {
    error.clear();
    if (!course::validate(course, error)) return false;
    if (course.days.empty() || course.days.size() > course::kMaxDays) {
        error = "course length out of range";
        return false;
    }

    CalendarState oldState;
    bool hasOld = false;
    if (!readCalendarIfPresent(oldState, hasOld, error)) return false;

    std::uint32_t generation = 1;
    if (hasOld) {
        generation = oldState.active_generation;
        if (oldState.draft_generation > generation) {
            generation = oldState.draft_generation;
        }
        ++generation;
    }
    for (unsigned guard = 0; guard < 10000; ++guard) {
        if (!LittleFS.exists((metaBase(generation) + ".json").c_str())) break;
        ++generation;
    }
    if (!ensureGenerationDir(generation)) {
        error = "cannot create generation directory";
        return false;
    }

    for (std::size_t i = 0; i < course.days.size(); ++i) {
        std::vector<course::Event> events;
        if (!course::resolvedEvents(course, i, events, error)) return false;
        course::Day flat;
        flat.hasOwnEvents = true;
        flat.events = events;
        if (!saveDayAtomic(generation, static_cast<std::uint8_t>(i), flat,
                           error)) {
            return false;
        }
    }

    CourseMeta meta;
    meta.generation = generation;
    meta.course_length = static_cast<std::uint8_t>(course.days.size());
    meta.name = course.name;
    std::string metaJson = makeMetaJson(meta, error);
    if (metaJson.empty()) return false;
    if (!writeAtomic(metaBase(generation), metaJson, error)) return false;
    if (!validateGeneration(generation, meta.course_length, error)) return false;

    CalendarState next = oldState;
    next.draft_generation = generation;
    if (!hasOld) {
        next.active_generation = 0;
        next.activation_date = 0;
        next.day_index = 0;
        next.course_length = meta.course_length;
        next.flags = 0;
        next.failure_count = 0;
    }
    if (!saveCalendar(next, error)) return false;

    if (hasOld && oldState.draft_generation > 0 &&
        oldState.draft_generation != generation &&
        oldState.draft_generation != oldState.active_generation) {
        deleteGeneration(oldState.draft_generation, error);
    }
    error.clear();
    draftGeneration = generation;
    outState = next;
    return true;
}

bool CourseStore::applyDraft(std::uint32_t draftGeneration,
                             CalendarState& outState, std::string& error) {
    error.clear();
    if (draftGeneration == 0) {
        error = "invalid draft generation";
        return false;
    }
    CourseMeta meta;
    if (!loadMeta(draftGeneration, meta, error)) return false;
    if (!validateGeneration(draftGeneration, meta.course_length, error)) {
        return false;
    }

    CalendarState oldState;
    bool hasOld = false;
    if (!readCalendarIfPresent(oldState, hasOld, error)) return false;

    CalendarState next;
    next.active_generation = draftGeneration;
    next.draft_generation = draftGeneration;
    next.activation_date = 0;
    next.day_index = 0;
    next.course_length = meta.course_length;
    next.flags = kApplied;
    next.failure_count = 0;
    if (!saveCalendar(next, error)) return false;

    if (hasOld && oldState.active_generation > 0 &&
        oldState.active_generation != draftGeneration) {
        deleteGeneration(oldState.active_generation, error);
    }
    if (hasOld && oldState.draft_generation > 0 &&
        oldState.draft_generation != draftGeneration &&
        oldState.draft_generation != oldState.active_generation) {
        deleteGeneration(oldState.draft_generation, error);
    }
    error.clear();
    outState = next;
    return true;
}

bool CourseStore::cloneGeneration(std::uint32_t from,
                                  std::uint32_t& outGeneration,
                                  std::string& error) {
    error.clear();
    if (from == 0) {
        error = "invalid generation";
        return false;
    }
    CourseMeta meta;
    if (!loadMeta(from, meta, error)) return false;
    if (meta.course_length == 0 || meta.course_length > course::kMaxDays) {
        error = "generation length out of range";
        return false;
    }

    std::uint32_t generation = from + 1;
    for (unsigned guard = 0; guard < 10000; ++guard) {
        if (!LittleFS.exists((metaBase(generation) + ".json").c_str())) break;
        ++generation;
    }
    if (generation == 0) generation = 1;
    if (!ensureGenerationDir(generation)) {
        error = "cannot create generation directory";
        return false;
    }

    std::string scratch;
    for (std::uint8_t day = 0; day < meta.course_length; ++day) {
        course::Day data;
        if (!loadDay(from, day, data, error)) {
            deleteGeneration(generation, scratch);
            return false;
        }
        if (!saveDayAtomic(generation, day, data, error)) {
            deleteGeneration(generation, scratch);
            return false;
        }
    }

    CourseMeta newMeta = meta;
    newMeta.generation = generation;
    const std::string metaJson = makeMetaJson(newMeta, error);
    if (metaJson.empty()) {
        deleteGeneration(generation, scratch);
        return false;
    }
    if (!writeAtomic(metaBase(generation), metaJson, error)) {
        deleteGeneration(generation, scratch);
        return false;
    }
    if (!validateGeneration(generation, meta.course_length, error)) {
        deleteGeneration(generation, scratch);
        return false;
    }
    error.clear();
    outGeneration = generation;
    return true;
}

bool CourseStore::createGeneration(std::uint32_t& outGeneration,
                                   std::string& error) {
    error.clear();
    std::uint32_t generation = 1;
    CalendarState state;
    bool present = false;
    if (!readCalendarIfPresent(state, present, error)) return false;
    if (present) {
        generation = state.active_generation > state.draft_generation
                         ? state.active_generation
                         : state.draft_generation;
        ++generation;
    }
    if (generation == 0) generation = 1;
    for (unsigned guard = 0; guard < 10000; ++guard) {
        const std::string meta = metaBase(generation) + ".json";
        const std::string first = dayBase(generation, 0) + ".json";
        if (!LittleFS.exists(meta.c_str()) &&
            !LittleFS.exists((metaBase(generation) + ".tmp").c_str()) &&
            !LittleFS.exists(first.c_str())) {
            break;
        }
        ++generation;
    }
    if (!ensureGenerationDir(generation)) {
        error = "cannot create generation directory";
        return false;
    }
    error.clear();
    outGeneration = generation;
    return true;
}

bool CourseStore::saveMeta(const CourseMeta& meta, std::string& error) {
    error.clear();
    if (meta.generation == 0) {
        error = "invalid generation";
        return false;
    }
    if (meta.course_length == 0 || meta.course_length > course::kMaxDays) {
        error = "course length out of range";
        return false;
    }
    const std::string json = makeMetaJson(meta, error);
    if (json.empty()) return false;
    return writeAtomic(metaBase(meta.generation), json, error);
}

bool CourseStore::generationExists(std::uint32_t generation) {
    if (generation == 0) return false;
    return LittleFS.exists((metaBase(generation) + ".json").c_str());
}

bool CourseStore::validateGeneration(std::uint32_t generation,
                                     std::uint8_t expectedLength,
                                     std::string& error) {
    error.clear();
    CourseMeta meta;
    if (!loadMeta(generation, meta, error)) return false;
    if (meta.course_length != expectedLength) {
        error = "course length mismatch";
        return false;
    }
    for (std::uint8_t i = 0; i < meta.course_length; ++i) {
        course::Day day;
        if (!loadDay(generation, i, day, error)) return false;
    }
    return true;
}

bool CourseStore::deleteGeneration(std::uint32_t generation,
                                   std::string& error) {
    error.clear();
    if (generation == 0) return true;
    for (std::uint8_t i = 0; i < course::kMaxDays; ++i) {
        const std::string base = dayBase(generation, i);
        LittleFS.remove((base + ".json").c_str());
        LittleFS.remove((base + ".tmp").c_str());
        LittleFS.remove((base + ".bak").c_str());
    }
    const std::string meta = metaBase(generation);
    LittleFS.remove((meta + ".json").c_str());
    LittleFS.remove((meta + ".tmp").c_str());
    LittleFS.remove((meta + ".bak").c_str());
    return true;
}

bool CourseStore::setPendingGeneration(std::uint32_t generation,
                                       std::string& error) {
    error.clear();
    const std::string path = std::string(kCourseDir) + "/pending_gen";
    if (generation == 0) {
        LittleFS.remove(path.c_str());
        return true;
    }
    File file = LittleFS.open(path.c_str(), "w");
    if (!file) {
        error = "cannot write pending generation";
        return false;
    }
    const std::string text = std::to_string(generation) + "\n";
    const size_t written = file.write(
        reinterpret_cast<const uint8_t*>(text.data()), text.size());
    file.close();
    if (written != text.size()) {
        LittleFS.remove(path.c_str());
        error = "short write to pending generation";
        return false;
    }
    return true;
}

bool CourseStore::reclaimPendingGeneration(std::string& error) {
    error.clear();
    const std::string path = std::string(kCourseDir) + "/pending_gen";
    if (!LittleFS.exists(path.c_str())) return true;
    File file = LittleFS.open(path.c_str(), "r");
    if (!file) {
        error = "cannot read pending generation";
        return false;
    }
    const String raw = file.readString();
    file.close();
    LittleFS.remove(path.c_str());
    std::uint32_t generation = 0;
    if (!parseU32(toStd(raw).substr(0, toStd(raw).find('\n')), generation)) {
        // A corrupt marker must not block the scheduler.
        return true;
    }
    return deleteGeneration(generation, error);
}

bool CourseStore::recover(std::string& error) {
    error.clear();
    CalendarState state;
    bool present = false;
    if (!readCalendarIfPresent(state, present, error)) return false;
    if (!present) {
        error.clear();
        return true;
    }
    if (state.active_generation == 0 || state.course_length == 0) {
        error = "calendar has no active generation";
        return false;
    }
    if (!recoverBase(metaBase(state.active_generation), false, error)) {
        return false;
    }
    for (std::uint8_t i = 0; i < state.course_length; ++i) {
        if (!recoverBase(dayBase(state.active_generation, i), true, error)) {
            return false;
        }
    }
    if (state.draft_generation != 0 &&
        state.draft_generation != state.active_generation) {
        // Draft recovery is best-effort: a broken draft must not stop the
        // active course from running.
        std::string ignored;
        recoverBase(metaBase(state.draft_generation), false, ignored);
        for (std::uint8_t i = 0; i < state.course_length; ++i) {
            recoverBase(dayBase(state.draft_generation, i), true, ignored);
        }
    }
    return validateGeneration(state.active_generation, state.course_length,
                              error);
}

bool CourseStore::loadLastFire(LastFire& out, std::string& error) {
    error.clear();
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) {
        error = "cannot open NVS";
        return false;
    }
    if (!prefs.isKey(kLastFireKey)) {
        prefs.end();
        error = "last_fire not found";
        return false;
    }
    const String raw = prefs.getString(kLastFireKey, "");
    prefs.end();
    return readLastFireBlob(toStd(raw), out, error);
}

bool CourseStore::saveLastFire(const LastFire& value, std::string& error) {
    error.clear();
    const std::string blob = makeLastFireBlob(value);
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "cannot open NVS";
        return false;
    }
    const size_t written = prefs.putString(kLastFireKey, toArduino(blob));
    prefs.end();
    if (written != blob.size()) {
        error = "last_fire NVS write failed";
        return false;
    }
    return true;
}

bool CourseStore::clearLastFire(std::string& error) {
    error.clear();
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) {
        error = "cannot open NVS";
        return false;
    }
    if (!prefs.isKey(kLastFireKey)) {
        prefs.end();
        return true;
    }
    const bool removed = prefs.remove(kLastFireKey);
    prefs.end();
    if (!removed) {
        error = "last_fire NVS remove failed";
        return false;
    }
    return true;
}

}  // namespace course_store
