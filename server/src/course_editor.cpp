#include "course_editor.h"

#include <cstdio>
#include <string>
#include <vector>

namespace course_editor {
namespace {

// Event JSON is emitted with the same keys the event editor and the v2 course
// format already use, so the GUI can feed it straight back into saveDay().
void appendEvent(std::string& out, const course::Event& event) {
    out += "{\"id\":";
    out += std::to_string(event.id);
    out += ",\"hour\":";
    out += std::to_string(static_cast<int>(event.hour));
    out += ",\"min\":";
    out += std::to_string(static_cast<int>(event.minute));
    out += ",\"track\":";
    out += std::to_string(static_cast<int>(event.track));
    out += ",\"loop\":";
    out += std::to_string(static_cast<int>(event.loop));
    out += ",\"vol\":";
    out += std::to_string(static_cast<int>(event.volume));
    out += ",\"en\":";
    out += event.enabled ? "true" : "false";
    out += ",\"desc\":\"";
    // Escape exactly like the course model does for v2 output.
    for (std::size_t i = 0; i < event.description.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(event.description[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out += "\"}";
}

void appendEvents(std::string& out, const std::vector<course::Event>& events) {
    out += '[';
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (i) out += ',';
        appendEvent(out, events[i]);
    }
    out += ']';
}

std::string trim(const std::string& text) {
    std::size_t first = 0;
    while (first < text.size() &&
           (text[first] == ' ' || text[first] == '\t' ||
            text[first] == '\r' || text[first] == '\n')) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first &&
           (text[last - 1] == ' ' || text[last - 1] == '\t' ||
            text[last - 1] == '\r' || text[last - 1] == '\n')) {
        --last;
    }
    return text.substr(first, last - first);
}

bool loadCalendar(course_store::CalendarState& cal, std::string& error) {
    course_store::CourseStore store;
    if (!store.hasCalendar()) {
        error = "no course has been published";
        return false;
    }
    return store.loadCalendar(cal, error);
}

// The generation the operator is editing: the private draft when one exists,
// otherwise the active generation itself (read-only until the first edit).
bool readGeneration(course_store::CalendarState& cal, std::uint32_t& generation,
                    std::string& error) {
    course_store::CourseStore store;
    if (!loadCalendar(cal, error)) return false;
    if (cal.active_generation == 0) {
        error = "no active generation";
        return false;
    }
    if (cal.draft_generation != 0 &&
        cal.draft_generation != cal.active_generation &&
        store.generationExists(cal.draft_generation)) {
        generation = cal.draft_generation;
        return true;
    }
    generation = cal.active_generation;
    return true;
}

// Copy-on-write: the active generation is never edited in place. The first
// draft write clones it and moves the NVS draft pointer to the clone.
bool ensureDraft(course_store::CalendarState& cal, std::uint32_t& generation,
                 std::string& error) {
    course_store::CourseStore store;
    if (!loadCalendar(cal, error)) return false;
    if (cal.active_generation == 0) {
        error = "no active generation";
        return false;
    }
    if (cal.draft_generation != 0 &&
        cal.draft_generation != cal.active_generation &&
        store.generationExists(cal.draft_generation)) {
        generation = cal.draft_generation;
        return true;
    }
    std::uint32_t cloned = 0;
    if (!store.cloneGeneration(cal.active_generation, cloned, error)) return false;
    course_store::CalendarState next = cal;
    next.draft_generation = cloned;
    if (!store.saveCalendar(next, error)) {
        std::string ignored;
        store.deleteGeneration(cloned, ignored);
        return false;
    }
    cal = next;
    generation = cloned;
    return true;
}

// Commits a freshly built generation as the draft. The active generation and
// its calendar binding are untouched.
bool commitDraft(course_store::CalendarState& cal, std::uint32_t built,
                 std::uint32_t previousDraft, std::string& error) {
    course_store::CourseStore store;
    course_store::CalendarState next = cal;
    next.draft_generation = built;
    if (!store.saveCalendar(next, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    cal = next;
    if (previousDraft != 0 && previousDraft != cal.active_generation &&
        previousDraft != built) {
        std::string ignored;
        store.deleteGeneration(previousDraft, ignored);
    }
    return true;
}

bool readMeta(std::uint32_t generation, course_store::CourseMeta& meta,
              std::string& error) {
    course_store::CourseStore store;
    return store.loadMeta(generation, meta, error);
}

// The generation the operator is editing. With a published course that is its
// private draft; before the first Apply it is the bootstrap draft recorded in
// its own NVS key, because course_state does not exist yet.
bool editingGeneration(std::uint32_t& generation, bool& published,
                       std::string& error) {
    error.clear();
    course_store::CourseStore store;
    if (!store.hasCalendar()) {
        published = false;
        const std::uint32_t bootstrap = store.loadBootstrapGeneration();
        if (bootstrap == 0 || !store.generationExists(bootstrap)) {
            error = "no course has been published";
            return false;
        }
        generation = bootstrap;
        return true;
    }
    published = true;
    course_store::CalendarState cal;
    if (!loadCalendar(cal, error)) return false;
    return readGeneration(cal, generation, error);
}

// A generation that may be written to. With a published course that is always
// a private draft generation (copy-on-write on the first edit), so the active
// generation is never modified in place. Before the first Apply the bootstrap
// generation is already private and is used as is.
bool ensureEditableGeneration(course_store::CalendarState& cal,
                              std::uint32_t& generation, bool& published,
                              std::string& error) {
    error.clear();
    course_store::CourseStore store;
    if (store.hasCalendar()) {
        published = true;
        return ensureDraft(cal, generation, error);
    }
    return editingGeneration(generation, published, error);
}

// Publishes a freshly built generation as the bootstrap draft. The NVS pointer
// is written last, so an interrupted build leaves only an unreferenced
// generation and no calendar at all.
bool commitBootstrap(std::uint32_t built, std::uint32_t previous,
                     std::string& error) {
    course_store::CourseStore store;
    if (!store.saveBootstrapGeneration(built, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    if (previous != 0 && previous != built) {
        std::string ignored;
        store.deleteGeneration(previous, ignored);
    }
    return true;
}

// Builds a complete generation from a resolved course and publishes its meta
// last. Used by the presets and by import, both before and after the first
// Apply. Returns false with any partially written generation removed.
bool buildGeneration(const course::Course& source, std::uint32_t& built,
                     std::string& error) {
    course_store::CourseStore store;
    if (!store.createGeneration(built, error)) return false;
    const std::uint8_t length = static_cast<std::uint8_t>(source.days.size());
    for (std::uint8_t i = 0; i < length; ++i) {
        course::Day dayData;
        dayData.hasOwnEvents = true;
        if (!course::resolvedEvents(source, i, dayData.events, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
        if (!store.saveDayAtomic(built, i, dayData, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
    }
    course_store::CourseMeta meta;
    meta.generation = built;
    meta.course_length = length;
    meta.name = source.name;
    if (!store.saveMeta(meta, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    if (!store.validateGeneration(built, length, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    return true;
}

// Either commits the built generation as a published draft or as the bootstrap
// draft, depending on whether a course has been published already.
bool commitEditingGeneration(bool published, std::uint32_t built,
                             course_store::CalendarState& cal,
                             std::uint32_t source, std::string& error) {
    if (published) return commitDraft(cal, built, source, error);
    return commitBootstrap(built, source, error);
}

// Builds an empty day (no events) for presets and "insert without copy".
course::Day emptyDay() {
    course::Day day;
    day.hasOwnEvents = true;
    return day;
}

// Streaming export state. The Web layer pulls one chunk at a time so the whole
// 47-day course never has to exist as one string.
struct ExportState {
    bool active;
    std::uint32_t generation;
    std::uint8_t length;
    std::uint8_t next;
    std::string name;

    ExportState() : active(false), generation(0), length(0), next(0) {}
};

ExportState& exportState() {
    static ExportState state;
    return state;
}

}  // namespace

Inspection::Inspection()
    : length(0), events(0), version(0) {}

bool inspectCourse(const std::string& json, Inspection& out, std::string& error) {
    error.clear();
    out = Inspection();
    course::Course parsed;
    if (!course::fromJson(json, parsed, error)) return false;
    int version = 0;
    if (!course::formatVersion(json, version, error)) return false;
    std::uint32_t events = 0;
    for (std::size_t i = 0; i < parsed.days.size(); ++i) {
        std::vector<course::Event> dayEvents;
        if (!course::resolvedEvents(parsed, i, dayEvents, error)) return false;
        events += static_cast<std::uint32_t>(dayEvents.size());
    }
    if (events > 0xFFFF) {
        error = "course has too many events";
        return false;
    }
    out.name = parsed.name;
    out.length = static_cast<std::uint8_t>(parsed.days.size());
    out.events = static_cast<std::uint16_t>(events);
    out.version = static_cast<std::uint8_t>(version);
    return true;
}

Info::Info()
    : hasCourse(false),
      activeGeneration(0),
      draftGeneration(0),
      activationDate(0),
      length(0),
      flags(0),
      failureCount(0),
      draftDirty(false) {}

bool info(Info& out, std::string& error) {
    error.clear();
    course_store::CourseStore store;
    if (!store.hasCalendar()) {
        // No published course yet. A bootstrap draft may still exist; report
        // it so the editor can be used before the first Apply.
        out = Info();
        const std::uint32_t bootstrap = store.loadBootstrapGeneration();
        if (bootstrap == 0 || !store.generationExists(bootstrap)) return true;
        course_store::CourseMeta meta;
        if (!readMeta(bootstrap, meta, error)) return false;
        out.draftGeneration = bootstrap;
        out.length = meta.course_length;
        out.name = meta.name;
        out.draftDirty = true;
        return true;
    }
    course_store::CalendarState cal;
    if (!store.loadCalendar(cal, error)) return false;
    out.hasCourse = true;
    out.activeGeneration = cal.active_generation;
    out.draftGeneration = cal.draft_generation;
    out.activationDate = cal.activation_date;
    out.length = cal.course_length;
    out.flags = cal.flags;
    out.failureCount = cal.failure_count;
    out.draftDirty = cal.draft_generation != 0 &&
                     cal.draft_generation != cal.active_generation;
    std::uint32_t generation = 0;
    if (!readGeneration(cal, generation, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(generation, meta, error)) return false;
    out.name = meta.name;
    out.length = meta.course_length;
    return true;
}

bool dayJson(std::uint8_t day, std::string& out, std::string& error) {
    error.clear();
    std::uint32_t generation = 0;
    bool published = false;
    if (!editingGeneration(generation, published, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(generation, meta, error)) return false;
    if (day >= meta.course_length) {
        error = "day index out of range";
        return false;
    }
    course::Day dayData;
    course_store::CourseStore store;
    if (!store.loadDay(generation, day, dayData, error)) return false;
    out.clear();
    appendEvents(out, dayData.events);
    return true;
}

bool saveDay(std::uint8_t day, const std::string& json, std::string& error) {
    error.clear();
    course_store::CalendarState cal;
    std::uint32_t generation = 0;
    bool published = false;
    if (!ensureEditableGeneration(cal, generation, published, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(generation, meta, error)) return false;
    if (day >= meta.course_length) {
        error = "day index out of range";
        return false;
    }

    // Accept either a bare event array or a {"events":[...]} day object and
    // validate it through the course model's own v2 parser.
    const std::string trimmed = trim(json);
    if (trimmed.empty()) {
        error = "empty day payload";
        return false;
    }
    std::string eventsJson;
    if (trimmed[0] == '[') {
        eventsJson = trimmed;
    } else if (trimmed[0] == '{') {
        eventsJson = trimmed;
    } else {
        error = "day payload must be an array or object";
        return false;
    }
    std::string wrapped = "{\"format\":\"gong-course\",\"version\":2,";
    wrapped += "\"name\":\"day\",\"course_length\":1,\"days\":[";
    if (trimmed[0] == '[') {
        wrapped += "{\"events\":";
        wrapped += eventsJson;
        wrapped += "}";
    } else {
        wrapped += eventsJson;
    }
    wrapped += "]}";

    course::Course parsed;
    std::string parseError;
    if (!course::fromJson(wrapped, parsed, parseError)) {
        error = "invalid day: " + parseError;
        return false;
    }
    if (parsed.days.size() != 1) {
        error = "invalid day payload";
        return false;
    }
    course::Day dayData = parsed.days[0];
    dayData.hasOwnEvents = true;
    dayData.templateName.clear();
    course_store::CourseStore store;
    if (!store.saveDayAtomic(generation, day, dayData, error)) return false;
    error.clear();
    return true;
}

bool clearDay(std::uint8_t day, std::string& error) {
    course_store::CalendarState cal;
    std::uint32_t generation = 0;
    bool published = false;
    if (!ensureEditableGeneration(cal, generation, published, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(generation, meta, error)) return false;
    if (day >= meta.course_length) {
        error = "day index out of range";
        return false;
    }
    course_store::CourseStore store;
    return store.saveDayAtomic(generation, day, emptyDay(), error);
}

bool insertDay(std::uint8_t day, bool copyEvents, std::string& error) {
    error.clear();
    course_store::CalendarState cal;
    std::uint32_t source = 0;
    bool published = false;
    if (!ensureEditableGeneration(cal, source, published, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(source, meta, error)) return false;
    if (day > meta.course_length) {
        error = "day index out of range";
        return false;
    }
    if (meta.course_length >= course::kMaxDays) {
        error = "course is already at the maximum length";
        return false;
    }

    course_store::CourseStore store;
    std::uint32_t built = 0;
    if (!store.createGeneration(built, error)) return false;

    // Copy the affected days one at a time: only one day is in RAM.
    for (std::uint8_t i = 0; i < meta.course_length; ++i) {
        course::Day dayData;
        if (!store.loadDay(source, i, dayData, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
        const std::uint8_t target = i < day ? i : static_cast<std::uint8_t>(i + 1);
        if (!store.saveDayAtomic(built, target, dayData, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
    }

    course::Day inserted;
    if (copyEvents && day < meta.course_length) {
        if (!store.loadDay(source, day, inserted, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
    } else {
        inserted = emptyDay();
    }
    if (!store.saveDayAtomic(built, day, inserted, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }

    course_store::CourseMeta nextMeta = meta;
    nextMeta.generation = built;
    nextMeta.course_length = static_cast<std::uint8_t>(meta.course_length + 1);
    if (!store.saveMeta(nextMeta, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    if (!store.validateGeneration(built, nextMeta.course_length, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    return commitEditingGeneration(published, built, cal, source, error);
}

bool deleteDay(std::uint8_t day, std::string& error) {
    error.clear();
    course_store::CalendarState cal;
    std::uint32_t source = 0;
    bool published = false;
    if (!ensureEditableGeneration(cal, source, published, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(source, meta, error)) return false;
    if (day >= meta.course_length) {
        error = "day index out of range";
        return false;
    }
    if (meta.course_length <= 1) {
        error = "cannot delete the last remaining day";
        return false;
    }

    course_store::CourseStore store;
    std::uint32_t built = 0;
    if (!store.createGeneration(built, error)) return false;
    for (std::uint8_t i = 0; i < meta.course_length; ++i) {
        if (i == day) continue;
        course::Day dayData;
        if (!store.loadDay(source, i, dayData, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
        const std::uint8_t target = i < day ? i : static_cast<std::uint8_t>(i - 1);
        if (!store.saveDayAtomic(built, target, dayData, error)) {
            std::string ignored;
            store.deleteGeneration(built, ignored);
            return false;
        }
    }
    course_store::CourseMeta nextMeta = meta;
    nextMeta.generation = built;
    nextMeta.course_length = static_cast<std::uint8_t>(meta.course_length - 1);
    if (!store.saveMeta(nextMeta, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    if (!store.validateGeneration(built, nextMeta.course_length, error)) {
        std::string ignored;
        store.deleteGeneration(built, ignored);
        return false;
    }
    return commitEditingGeneration(published, built, cal, source, error);
}

bool setPreset(std::uint8_t days, std::string& error) {
    error.clear();
    if (days < 1 || days > course::kMaxDays) {
        error = "course length out of range";
        return false;
    }
    course_store::CourseStore store;
    const bool published = store.hasCalendar();
    course_store::CalendarState cal;
    std::uint32_t source = 0;
    if (published) {
        if (!ensureDraft(cal, source, error)) return false;
    } else {
        // Nothing to inherit: the preset becomes the bootstrap draft and no
        // calendar is written until Apply.
        source = store.loadBootstrapGeneration();
        if (source != 0 && !store.generationExists(source)) source = 0;
    }

    course::Course preset;
    if (days == 14) {
        preset = course::makeReferenceCourse();
    } else {
        preset.name = "Course " + std::to_string(static_cast<int>(days)) + " days";
        preset.generation = 0;
        for (std::uint8_t i = 0; i < days; ++i) {
            preset.days.push_back(emptyDay());
        }
    }
    if (!course::validate(preset, error)) return false;

    std::uint32_t built = 0;
    if (!buildGeneration(preset, built, error)) return false;
    return commitEditingGeneration(published, built, cal, source, error);
}

bool renameCourse(const std::string& name, std::string& error) {
    error.clear();
    course_store::CalendarState cal;
    std::uint32_t generation = 0;
    bool published = false;
    if (!ensureEditableGeneration(cal, generation, published, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(generation, meta, error)) return false;
    meta.name = name;
    course_store::CourseStore store;
    return store.saveMeta(meta, error);
}

bool importCourse(const std::string& json, std::string& error) {
    error.clear();
    course::Course imported;
    if (!course::fromJson(json, imported, error)) return false;
    if (!course::validate(imported, error)) return false;

    course_store::CourseStore store;
    const bool published = store.hasCalendar();
    course_store::CalendarState cal;
    std::uint32_t source = 0;
    if (published) {
        if (!ensureDraft(cal, source, error)) return false;
    } else {
        // A clean install: the imported course becomes the bootstrap draft.
        source = store.loadBootstrapGeneration();
        if (source != 0 && !store.generationExists(source)) source = 0;
    }

    std::uint32_t built = 0;
    if (!buildGeneration(imported, built, error)) return false;
    return commitEditingGeneration(published, built, cal, source, error);
}

bool applyDraft(std::string& error) {
    error.clear();
    course_store::CourseStore store;
    if (!store.hasCalendar()) {
        // First Apply on a clean install: publish the bootstrap draft as
        // generation N with an APPLIED binding. The calendar is one NVS blob,
        // so the switch is a single atomic record and nothing rings until an
        // explicit Activate.
        const std::uint32_t bootstrap = store.loadBootstrapGeneration();
        if (bootstrap == 0 || !store.generationExists(bootstrap)) {
            error = "no draft course to apply";
            return false;
        }
        course_store::CourseMeta meta;
        if (!readMeta(bootstrap, meta, error)) return false;
        if (!store.validateGeneration(bootstrap, meta.course_length, error)) {
            return false;
        }
        course_store::CalendarState next;
        next.active_generation = bootstrap;
        next.draft_generation = bootstrap;
        next.activation_date = 0;
        next.day_index = 0;
        next.course_length = meta.course_length;
        next.flags = course_store::kApplied;
        next.failure_count = 0;
        if (!store.saveCalendar(next, error)) return false;
        std::string ignored;
        if (!store.saveBootstrapGeneration(0, ignored)) {
            // The pointer is ignored once the calendar exists; a leftover
            // value cannot resurrect the draft.
            error.clear();
        }
        return true;
    }
    course_store::CalendarState cal;
    if (!loadCalendar(cal, error)) return false;
    if (cal.active_generation == 0) {
        error = "no active generation";
        return false;
    }
    std::uint32_t generation = 0;
    if (!readGeneration(cal, generation, error)) return false;
    course_store::CourseMeta meta;
    if (!readMeta(generation, meta, error)) return false;
    if (!store.validateGeneration(generation, meta.course_length, error)) {
        return false;
    }

    // Reclaim whatever the previous Apply left behind before recording a new
    // fallback generation.
    std::string ignored;
    store.reclaimPendingGeneration(ignored);

    course_store::CalendarState next = cal;
    next.active_generation = generation;
    next.draft_generation = generation;
    next.activation_date = 0;
    next.day_index = 0;
    next.course_length = meta.course_length;
    next.flags = course_store::kApplied;
    next.failure_count = 0;
    if (!store.saveCalendar(next, error)) return false;

    if (cal.active_generation != generation) {
        // Keep the replaced generation until the new active day loads.
        if (!store.setPendingGeneration(cal.active_generation, ignored)) {
            // Not fatal: the new generation is already published and valid.
        }
    }
    if (cal.draft_generation != 0 && cal.draft_generation != cal.active_generation &&
        cal.draft_generation != generation) {
        store.deleteGeneration(cal.draft_generation, ignored);
    }
    error.clear();
    return true;
}

bool discardDraft(std::string& error) {
    error.clear();
    course_store::CourseStore store;
    if (!store.hasCalendar()) {
        // Only a bootstrap draft can exist here. Dropping it leaves the device
        // exactly as it was: no calendar, legacy behaviour untouched.
        const std::uint32_t bootstrap = store.loadBootstrapGeneration();
        if (bootstrap == 0) {
            error = "no draft course to discard";
            return false;
        }
        if (!store.saveBootstrapGeneration(0, error)) return false;
        std::string ignored;
        store.deleteGeneration(bootstrap, ignored);
        return true;
    }
    course_store::CalendarState cal;
    if (!loadCalendar(cal, error)) return false;
    if (cal.active_generation == 0) {
        error = "no active generation";
        return false;
    }
    const std::uint32_t previous = cal.draft_generation;
    course_store::CalendarState next = cal;
    next.draft_generation = cal.active_generation;
    if (!store.saveCalendar(next, error)) return false;
    if (previous != 0 && previous != cal.active_generation) {
        std::string ignored;
        store.deleteGeneration(previous, ignored);
    }
    error.clear();
    return true;
}

bool ensureInitialCourse(std::string& error) {
    error.clear();
    course_store::CourseStore store;
    if (store.hasCalendar()) {
        error = "course already present";
        return false;
    }
    if (store.loadBootstrapGeneration() != 0) {
        // The operator is preparing a course on this device. Publishing the
        // reference course over it would silently discard their draft.
        error = "a draft course is already pending";
        return false;
    }
    course::Course reference = course::makeReferenceCourse();
    if (!course::validate(reference, error)) return false;
    std::uint32_t generation = 0;
    course_store::CalendarState state;
    if (!store.applyCourse(reference, generation, state, error)) return false;
    error.clear();
    return true;
}

bool beginExport(ExportSource source, std::string& error) {
    error.clear();
    ExportState& state = exportState();
    state.active = false;
    std::uint32_t generation = 0;
    course_store::CourseStore store;
    if (source == kExportActive && store.hasCalendar()) {
        // The operator asked for what is actually published right now, not for
        // a draft that has been imported but not applied.
        course_store::CalendarState cal;
        if (!loadCalendar(cal, error)) return false;
        if (cal.active_generation == 0 || !store.generationExists(cal.active_generation)) {
            error = "no active generation";
            return false;
        }
        generation = cal.active_generation;
    } else {
        bool published = false;
        if (!editingGeneration(generation, published, error)) return false;
    }
    course_store::CourseMeta meta;
    if (!readMeta(generation, meta, error)) return false;
    state.active = true;
    state.generation = generation;
    state.length = meta.course_length;
    state.next = 0;
    state.name = meta.name;
    return true;
}

bool nextExportChunk(std::string& chunk, bool& done, std::string& error) {
    error.clear();
    done = false;
    ExportState& state = exportState();
    if (!state.active) {
        error = "export not started";
        return false;
    }
    course_store::CourseStore store;
    if (state.next == 0) {
        chunk = "{\"format\":\"gong-course\",\"version\":2,\"name\":\"";
        // Reuse the model's escaping by round-tripping the name through a
        // minimal v2 document is overkill; names are short and already
        // validated, so escape the few characters that matter.
        for (std::size_t i = 0; i < state.name.size(); ++i) {
            const char c = state.name[i];
            if (c == '"' || c == '\\') chunk.push_back('\\');
            if (c == '\n') { chunk += "\\n"; continue; }
            chunk.push_back(c);
        }
        chunk += "\",\"generation\":";
        chunk += std::to_string(state.generation);
        chunk += ",\"course_length\":";
        chunk += std::to_string(static_cast<int>(state.length));
        chunk += ",\"days\":[";
        ++state.next;
        return true;
    }
    const std::uint8_t day = static_cast<std::uint8_t>(state.next - 1);
    if (day < state.length) {
        course::Day dayData;
        if (!store.loadDay(state.generation, day, dayData, error)) return false;
        if (day > 0) chunk += ',';
        chunk += "{\"events\":";
        appendEvents(chunk, dayData.events);
        chunk += "}";
        ++state.next;
        return true;
    }
    chunk = "]}";
    state.active = false;
    done = true;
    return true;
}

}  // namespace course_editor
