#include "schedule.h"
#include "config.h"
#include "mp3handler.h"
#include "active_day_state.h"
#include "course_store.h"
#include "course_editor.h"
#include "emergency.h"
#include "sysstate.h"
#include <LittleFS.h>
#include <time.h>
#include <new>
#include <memory>
#include <utility>
#include <ArduinoJson.h>
#include <Preferences.h>

// ── JSON pool capacity for a full day of schedule entries ─────────────────
// A real 16-entry day with Cyrillic descriptions is ~3.4 KB of JSON text;
// deserializing from a File copies ALL keys and strings into the pool, so 16
// entries already consume ~3 KB. At MAX_SCHEDULES = 32 the old 4096-byte pool
// overflowed: deserializeJson returned NoMemory (whole day silently loaded as
// EMPTY) and createNestedObject started returning null (entries silently
// DROPPED on save → data loss). With desc capped at DESC_MAX_BYTES, 32
// entries need ~7.5 KB worst case; 12 KB leaves real headroom. Overflow is
// additionally checked at every save point below. Upper bound only — pools
// are sized by the file (dayPoolSize) so they fit a fragmented heap.
#define SCHED_JSON_CAPACITY 12288

void (*onScheduleTrigger)(uint8_t track, uint8_t loop, uint8_t vol) = nullptr;

static ScheduleEntry entries[MAX_SCHEDULES];
static uint8_t       count   = 0;
static uint32_t      nextId  = 1;

// Anti-double-trigger: remember which minute we last fired
static int           lastFiredKey    = -1;
static unsigned long lastFiredMillis = 0;
static unsigned long lastTimeLog     = 0;

// The pair is encoded in one NVS record; legacy fields are read-only inputs.
static int    activeDay  = -1;
static String activeDate = "";
static active_day::Store activeStore(DAY_COUNT);
static bool executionRunning = false;
static uint64_t publishedActive = 0;
enum class ScheduleFileState { Unprepared, Ready, Blocked };
static ScheduleFileState scheduleFileState = ScheduleFileState::Unprepared;
// Retain a validated boot input if its file cannot yet be prepared.
static active_day::Record loadedRecord;
static active_day::Source loadedSource = active_day::Source::Error;

// ── Cyclic course state (stage 3) ─────────────────────────────────────────
// When a course_state record exists in NVS the scheduler runs from the
// generation-based course store instead of the legacy dayNN.conf path. The
// legacy path is kept unchanged for existing installations and host tests.
static bool courseMode = false;
static bool courseRunning = false;
// No published course and no legacy NVS binding: the device is waiting for a
// JSON course and must not ring anything (fresh JSON-driven installation).
static bool waitingForCourse = false;
static bool courseRecoveryCheck();
static bool courseSuspended = false;
static bool courseDayValid = false;
static uint8_t courseDay = 0;
static uint8_t courseLength = 0;
static uint32_t courseGeneration = 0;
static course_store::CalendarState courseCal;
static course_store::LastFire courseLastFire;
static bool courseLastFireValid = false;
static course_store::CourseStore courseStore;

static bool schedulerReady() {
    if (courseMode) {
        return courseRunning && !courseSuspended && courseDayValid &&
            scheduleFileState == ScheduleFileState::Ready;
    }
    return activeStore.status() == active_day::Status::Ready && executionRunning &&
        scheduleFileState == ScheduleFileState::Ready;
}

static void blockScheduleFile() {
    scheduleFileState = ScheduleFileState::Blocked;
    executionRunning = false;
}

// M-16: don't write to SPIFFS while a track is playing — a write landing
// exactly then competes with the audio task for the same flash and causes
// audible stutter. The in-memory `entries[]` is already correct by the time
// this is set (callers update it before calling sched_save()), so API
// responses stay instant; only the disk write is delayed.
static volatile bool schedPendingSave = false;

// M-14: persisted timestamp of the last CONFIRMED gong (real epoch time, so
// it's only ever compared while a valid time source is set), used to catch
// up a gong that should have fired during a brief reboot window.
static Preferences firePrefs;
static uint32_t    lastFireTs = 0;

// -------------------------------------------------------
// True if [h,m] already occupied by another entry in the currently loaded
// (active) day. `excludeId` lets sched_edit() ignore the entry being edited.
// -------------------------------------------------------
static bool sched_timeTaken(uint8_t h, uint8_t m, uint32_t excludeId) {
    for (uint8_t i = 0; i < count; i++) {
        if (entries[i].id == excludeId) continue;
        if (entries[i].hour == h && entries[i].minute == m) return true;
    }
    return false;
}

static String currentDateStr() {
    struct tm ti;
    if (!localNow(ti) || ti.tm_year < 124) return "";
    char buf[11];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
    return String(buf);
}

// M-15: calendar days between two "YYYY-MM-DD" strings (to - from). Used so
// a device left off for N days advances the course by N days on next boot,
// instead of always advancing by exactly 1 and drifting the course.
static int daysBetween(const String& from, const String& to) {
    struct tm a = {}, b = {};
    if (sscanf(from.c_str(), "%d-%d-%d", &a.tm_year, &a.tm_mon, &a.tm_mday) != 3) return 0;
    if (sscanf(to.c_str(),   "%d-%d-%d", &b.tm_year, &b.tm_mon, &b.tm_mday) != 3) return 0;
    a.tm_year -= 1900; a.tm_mon -= 1; a.tm_hour = 12;   // noon avoids DST edge cases
    b.tm_year -= 1900; b.tm_mon -= 1; b.tm_hour = 12;
    time_t ta = mktime(&a), tb = mktime(&b);
    return (int)((tb - ta) / 86400);
}

static bool sched_parseFromPath(const char* path);
static bool sched_saveNow();

// ── Cyclic course helpers ─────────────────────────────────────────────────
static bool courseDateFromClock(course::Date& out) {
    struct tm ti;
    if (!localNow(ti) || ti.tm_year < 124) return false;
    out.year = ti.tm_year + 1900;
    out.month = ti.tm_mon + 1;
    out.day = ti.tm_mday;
    return true;
}

static uint32_t courseDateNumber(const course::Date& d) {
    return (uint32_t)d.year * 10000u + (uint32_t)d.month * 100u + (uint32_t)d.day;
}

static course::Date courseDateFromClockNow() {
    course::Date d;
    struct tm ti;
    if (localNow(ti)) {
        d.year = ti.tm_year + 1900;
        d.month = ti.tm_mon + 1;
        d.day = ti.tm_mday;
    }
    return d;
}

static void courseEventFromEntry(const ScheduleEntry& s, course::Event& e) {
    e.id = s.id;
    e.hour = s.hour;
    e.minute = s.minute;
    e.track = s.track;
    e.loop = s.loop;
    e.volume = s.vol;
    e.enabled = s.enabled;
    e.description = std::string(s.description.c_str());
}

static bool courseEntryFromEvent(const course::Event& e, ScheduleEntry& s) {
    if (e.hour > 23 || e.minute > 59) return false;
    s.id = e.id;
    s.hour = e.hour;
    s.minute = e.minute;
    s.track = e.track;
    s.loop = e.loop;
    s.vol = e.volume;
    s.enabled = e.enabled;
    s.description = String(e.description.c_str());
    return true;
}

// Editing a day of a template generation needs that generation's template set
// in RAM. Only the editing entry points below call this: boot, Apply, Activate
// and the midnight advance stay untouched (stage E wires the cache there).
static bool courseEnsureTemplates() {
    if (courseGeneration == 0) return false;
    std::string err;
    if (courseStore.prepareTemplates(courseGeneration, err)) return true;
    logPrintf("[COURSE] templates unavailable: %s\n", err.c_str());
    return false;
}

static bool courseLoadDay(uint8_t day, course::Day& out) {
    if (courseGeneration == 0 || day >= courseLength) return false;
    std::string err;
    // Boot, Activate, the midnight advance and recovery all end up here: the
    // templates of the generation being executed must be in the active slot,
    // never a draft's set.
    if (!courseStore.prepareActiveTemplates(courseGeneration, err)) {
        logPrintf("[COURSE] templates unavailable: %s\n", err.c_str());
        return false;
    }
    if (!courseStore.loadDay(courseGeneration, day, out, err)) {
        logPrintf("[COURSE] load day %u failed: %s\n", (unsigned)day, err.c_str());
        return false;
    }
    return true;
}

// Replaces the live RAM schedule with one course day. Filesystem I/O happens
// before the lock; only the RAM swap is locked.
static bool coursePublishDay(uint8_t day, bool allowCalls) {
    course::Day d;
    if (!courseLoadDay(day, d)) return false;
    if (d.events.size() > MAX_SCHEDULES) return false;
    uint32_t maxId = 0;
    sys_lock();
    for (size_t i = 0; i < d.events.size(); ++i) {
        ScheduleEntry s{};
        if (!courseEntryFromEvent(d.events[i], s)) {
            sys_unlock();
            return false;
        }
        entries[i] = s;
        if (s.id > maxId) maxId = s.id;
    }
    for (size_t i = d.events.size(); i < MAX_SCHEDULES; ++i) entries[i] = ScheduleEntry{};
    count = (uint8_t)d.events.size();
    nextId = maxId + 1;
    activeDay = (int)day;
    activeDate = currentDateStr();
    executionRunning = allowCalls;
    scheduleFileState = ScheduleFileState::Ready;
    sys_unlock();
    courseDay = day;
    courseDayValid = true;
    return true;
}

static void courseSuspend(const char* why, uint8_t reason = emergency::kNone) {
    logPrintf("[COURSE] suspended: %s\n", why);
    courseSuspended = true;
    courseRunning = false;
    executionRunning = false;
    if (reason != emergency::kNone) {
        // A day load failed. Persist the failure counter and either arm a
        // delayed reboot or enter EMERGENCY after the third failure.
        emergency::recordFailure(reason);
    }
    if (courseGeneration != 0) {
        course_store::CalendarState next = courseCal;
        // APPLIED stays set: the course is still published, just not running.
        next.flags = static_cast<uint8_t>(
            (next.flags & course_store::kApplied) | course_store::kSuspended);
        if (emergency::isActive()) next.flags |= course_store::kEmergency;
        next.failure_count = emergency::attempts();
        std::string err;
        if (!courseStore.saveCalendar(next, err)) {
            logPrintf("[COURSE] save suspended state failed: %s\n", err.c_str());
        } else {
            courseCal = next;
        }
    }
}

static bool courseComputeToday(course::Date& today, uint8_t& day) {
    if (!courseDateFromClock(today)) return false;
    const uint32_t act = courseCal.activation_date;
    if (act < 19700101u) return false;
    course::Date start;
    start.year = (int)(act / 10000u);
    start.month = (int)((act / 100u) % 100u);
    start.day = (int)(act % 100u);
    const long long diff = course::civilDaysBetween(start, today);
    if (diff < 0) return false;
    long long idx = ((long long)courseCal.day_index + diff) % (long long)courseLength;
    if (idx < 0) idx += courseLength;
    day = (uint8_t)idx;
    return true;
}

// Saves the live RAM entries as the current day's file (atomic tmp+rename).
static bool courseSaveActiveEntries() {
    if (!courseMode || courseGeneration == 0 || !courseDayValid) return false;
    course::Day d;
    d.hasOwnEvents = true;
    d.events.resize(count);
    for (uint8_t i = 0; i < count; ++i) courseEventFromEntry(entries[i], d.events[i]);
    std::string err;
    if (!courseStore.saveDayAtomic(courseGeneration, courseDay, d, err)) {
        logPrintf("[COURSE] save day %u failed: %s\n", (unsigned)courseDay, err.c_str());
        return false;
    }
    schedPendingSave = false;
    return true;
}

static bool courseTransitionPending = false;

static void courseAdvance() {
    if (!courseMode || !courseRunning || courseSuspended) return;
    if (courseCal.flags & course_store::kSuspended) return;
    course::Date today;
    uint8_t day = 0;
    if (!courseComputeToday(today, day)) return;
    if (courseDayValid && day == courseDay) {
        courseTransitionPending = false;
        return;
    }
    if (mp3_isPlaying()) {
        // The calendar day has changed, but the old day's schedule must not
        // ring anymore: wait for the track to end.
        courseTransitionPending = true;
        return;
    }
    courseTransitionPending = false;
    if (!coursePublishDay(day, true)) courseSuspend("cannot load next day", emergency::kMidnightFailed);
}

// Drops the generation replaced by the last Apply once the new active day has
// loaded. Until then it stays on flash as a fallback.
static void courseReclaimPending() {
    std::string err;
    if (!courseStore.reclaimPendingGeneration(err)) {
        logPrintf("[COURSE] reclaim pending generation failed: %s\n", err.c_str());
    }
}

static void dayPath(int day, char (&path)[16]);

// True only when the filesystem is mounted and every legacy schedule file
// (dayNN.conf, gong.conf) is confirmed absent. A failed or ambiguous probe is
// not absence: seeding a course over unknown state would hide a storage fault.
static bool courseLegacyConfirmedAbsent() {
    if (!sys_fsOk()) return false;
    if (active_day::probeFile(SCHEDULE_FILE) != active_day::FilePresence::ConfirmedAbsent) return false;
    char path[16];
    for (int d = 0; d < DAY_COUNT; d++) {
        dayPath(d, path);
        if (active_day::probeFile(path) != active_day::FilePresence::ConfirmedAbsent) return false;
    }
    return true;
}

static bool courseTryBoot() {
    if (!courseStore.hasCalendar()) {
        // Fresh device: publish the reference course so the editor has a
        // course to show. Installations that still have any legacy schedule
        // file, or whose filesystem cannot confirm their absence, keep the
        // legacy path untouched.
        if (courseLegacyConfirmedAbsent()) {
            std::string err;
            if (course_editor::ensureInitialCourse(err)) {
                logPrintf("[COURSE] initial reference course published\n");
            } else {
                logPrintf("[COURSE] initial course failed: %s\n", err.c_str());
            }
        }
        if (!courseStore.hasCalendar()) return false;
    }
    std::string err;
    course_store::CalendarState cal;
    if (!courseStore.loadCalendar(cal, err)) {
        courseMode = true;
        courseSuspended = true;
        courseRunning = false;
        logPrintf("[COURSE] calendar read failed: %s\n", err.c_str());
        emergency::recordFailure(emergency::kCalendarCorrupt);
        return true;
    }
    courseMode = true;
    courseCal = cal;
    courseGeneration = cal.active_generation;
    courseLength = cal.course_length;
    courseSuspended = (cal.flags & course_store::kSuspended) != 0;
    courseRunning = false;
    if (courseGeneration == 0 || courseLength == 0) {
        courseSuspended = true;
        emergency::recordFailure(emergency::kGenerationInvalid);
        return true;
    }
    course_store::LastFire lf;
    if (courseStore.loadLastFire(lf, err) && lf.generation == courseGeneration) {
        courseLastFire = lf;
        courseLastFireValid = true;
    }
    if (cal.flags & course_store::kRunning) {
        course::Date today;
        uint8_t day = 0;
        if (!courseComputeToday(today, day)) {
            courseSuspend("cannot compute current day", emergency::kBootFailed);
            return true;
        }
        if (!coursePublishDay(day, true)) {
            courseSuspend("cannot load active day", emergency::kBootFailed);
            return true;
        }
        courseRunning = true;
        emergency::clearFailure();
        courseReclaimPending();
    } else if (cal.flags & course_store::kSuspended) {
        // The previous activation/midnight load failed. Retry the interrupted
        // binding once at boot; a repeated failure counts as the next attempt.
        if (courseRecoveryCheck()) {
            courseRunning = true;
        } else {
            courseSuspend("boot retry failed", emergency::kBootFailed);
        }
    } else {
        // Published but not bound: nothing may ring until an explicit Activate.
        // The legacy loader may have left its own day in RAM on a device that
        // was upgraded from the old scheduler; drop it so no stale entry can be
        // executed or written back by the legacy paths.
        count = 0;
        for (unsigned i = 0; i < MAX_SCHEDULES; ++i) entries[i] = ScheduleEntry{};
        activeDay = -1;
        activeDate = "";
    }
    return true;
}

static bool courseActivate(uint8_t day) {
    if (!courseMode) return false;
    if (day >= courseLength) return false;
    if (!sys_fsOk()) return false;
    mp3_stop();
    if (schedPendingSave) courseSaveActiveEntries();
    std::string err;
    // The generation must resolve completely through its OWN template set
    // before any day is bound.
    if (!courseStore.prepareActiveTemplates(courseGeneration, err)) {
        courseSuspend("templates unavailable", emergency::kActivateFailed);
        return false;
    }
    if (!courseStore.validateGeneration(courseGeneration, courseLength, err)) {
        courseSuspend("generation validation failed", emergency::kGenerationInvalid);
        return false;
    }
    course::Day d;
    if (!courseLoadDay(day, d)) {
        courseSuspend("selected day missing", emergency::kActivateFailed);
        return false;
    }
    course::Date today;
    if (!courseDateFromClock(today)) return false;
    course_store::CalendarState next = courseCal;
    next.day_index = day;
    next.activation_date = courseDateNumber(today);
    next.course_length = courseLength;
    next.flags = static_cast<uint8_t>(course_store::kApplied | course_store::kSuspended);
    if (!courseStore.saveCalendar(next, err)) {
        courseSuspended = true;
        courseRunning = false;
        executionRunning = false;
        return false;
    }
    courseCal = next;
    if (!coursePublishDay(day, false)) {
        courseSuspend("cannot publish selected day", emergency::kActivateFailed);
        return false;
    }
    next.flags = static_cast<uint8_t>(course_store::kApplied | course_store::kRunning);
    next.failure_count = 0;
    if (!courseStore.saveCalendar(next, err)) {
        courseSuspended = true;
        courseRunning = false;
        executionRunning = false;
        return false;
    }
    courseCal = next;
    courseSuspended = false;
    courseRunning = true;
    executionRunning = true;
    courseTransitionPending = false;
    emergency::clearFailure();
    courseReclaimPending();
    logPrintf("[COURSE] activated day %u\n", (unsigned)day);
    return true;
}

// Separate recovery path used by the emergency module. It validates the
// persisted course, restores the calendar binding and publishes the current
// day. It never fires a missed event: the normal scheduler starts from "now".
static bool courseRecoveryCheck() {
    if (!courseStore.hasCalendar()) return false;
    std::string err;
    course_store::CalendarState cal;
    if (!courseStore.loadCalendar(cal, err)) {
        logPrintf("[COURSE] recovery: calendar unreadable: %s\n", err.c_str());
        return false;
    }
    if (cal.active_generation == 0 || cal.course_length == 0) return false;
    if (!(cal.flags & course_store::kApplied)) return false;
    if (cal.activation_date < 19700101u) return false;
    if (!courseStore.prepareActiveTemplates(cal.active_generation, err)) {
        logPrintf("[COURSE] recovery: templates unavailable: %s\n", err.c_str());
        return false;
    }
    if (!courseStore.validateGeneration(cal.active_generation, cal.course_length, err)) {
        logPrintf("[COURSE] recovery: generation invalid: %s\n", err.c_str());
        return false;
    }
    courseMode = true;
    courseCal = cal;
    courseGeneration = cal.active_generation;
    courseLength = cal.course_length;
    course::Date today;
    uint8_t day = 0;
    if (!courseComputeToday(today, day)) {
        logPrintf("[COURSE] recovery: cannot compute day\n");
        return false;
    }
    if (!coursePublishDay(day, true)) {
        logPrintf("[COURSE] recovery: cannot load day %u\n", (unsigned)day);
        return false;
    }
    course_store::CalendarState next = cal;
    next.flags = static_cast<uint8_t>(course_store::kApplied | course_store::kRunning);
    next.failure_count = 0;
    next.day_index = day;
    if (!courseStore.saveCalendar(next, err)) {
        logPrintf("[COURSE] recovery: cannot save RUNNING: %s\n", err.c_str());
        return false;
    }
    courseCal = next;
    courseSuspended = false;
    courseRunning = true;
    executionRunning = true;
    courseTransitionPending = false;
    emergency::clearFailure();
    courseReclaimPending();
    logPrintf("[COURSE] recovered to RUNNING on day %u\n", (unsigned)day);
    return true;
}

// Ordinary event editing for non-active days. The active day is edited in RAM
// and flushed through sched_saveNow()/courseSaveActiveEntries().
static bool courseDayAddEvent(uint8_t day, uint8_t h, uint8_t m, const String& desc,
                              uint8_t track, uint8_t loop, uint8_t vol) {
    if (!courseEnsureTemplates()) return false;
    course::Day d;
    if (!courseLoadDay(day, d)) return false;
    if (d.events.size() >= MAX_SCHEDULES) return false;
    for (size_t i = 0; i < d.events.size(); ++i)
        if (d.events[i].hour == h && d.events[i].minute == m) return false;
    uint32_t maxId = 0;
    for (size_t i = 0; i < d.events.size(); ++i)
        if (d.events[i].id > maxId) maxId = d.events[i].id;
    course::Event e;
    e.id = maxId + 1;
    e.hour = h;
    e.minute = m;
    e.track = track;
    e.loop = loop;
    e.volume = vol;
    e.enabled = true;
    e.description = std::string(desc.c_str());
    d.events.push_back(e);
    std::string err;
    return courseStore.saveDayAtomic(courseGeneration, day, d, err);
}

static bool courseDayEditEvent(uint8_t day, uint32_t id, uint8_t h, uint8_t m,
                               const String& desc, uint8_t track, uint8_t loop, uint8_t vol,
                               bool enabled) {
    if (!courseEnsureTemplates()) return false;
    course::Day d;
    if (!courseLoadDay(day, d)) return false;
    for (size_t i = 0; i < d.events.size(); ++i) {
        if (d.events[i].id != id) continue;
        for (size_t j = 0; j < d.events.size(); ++j)
            if (j != i && d.events[j].hour == h && d.events[j].minute == m) return false;
        d.events[i].hour = h;
        d.events[i].minute = m;
        d.events[i].track = track;
        d.events[i].loop = loop;
        d.events[i].volume = vol;
        d.events[i].enabled = enabled;
        d.events[i].description = std::string(desc.c_str());
        std::string err;
        return courseStore.saveDayAtomic(courseGeneration, day, d, err);
    }
    return false;
}

static bool courseDayDeleteEvent(uint8_t day, uint32_t id) {
    if (!courseEnsureTemplates()) return false;
    course::Day d;
    if (!courseLoadDay(day, d)) return false;
    for (size_t i = 0; i < d.events.size(); ++i) {
        if (d.events[i].id != id) continue;
        d.events.erase(d.events.begin() + i);
        std::string err;
        return courseStore.saveDayAtomic(courseGeneration, day, d, err);
    }
    return false;
}

static String courseDayJson(uint8_t day) {
    if (!courseEnsureTemplates()) return String();
    course::Day d;
    if (!courseLoadDay(day, d)) return String();
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();
    for (size_t i = 0; i < d.events.size(); ++i) {
        JsonObject o = arr.createNestedObject();
        o["id"] = d.events[i].id;
        o["hour"] = d.events[i].hour;
        o["min"] = d.events[i].minute;
        o["track"] = d.events[i].track;
        o["loop"] = d.events[i].loop;
        o["vol"] = d.events[i].volume;
        o["en"] = d.events[i].enabled;
        o["desc"] = d.events[i].description.c_str();
    }
    if (doc.overflowed()) return String();
    String out;
    serializeJson(doc, out);
    return out;
}

// JSON pool for a day file of `fileSize` bytes, plus room for one more entry
// (template add). Each entry is 9 pool slots (16 B on ESP32) plus its strings
// — up to ~2.4x a file with empty descriptions; 3x + 1 KB covers it (checked
// against data/*.conf and a 32-entry worst case). A fixed 12 KB block per
// call used to fail on a fragmented heap — the midnight day switch included.
static size_t dayPoolSize(size_t fileSize) {
    size_t n = 1024 + 3 * fileSize;
    return n < SCHED_JSON_CAPACITY ? n : SCHED_JSON_CAPACITY;
}

static void dayPath(int day, char (&path)[16]) {
    snprintf(path, sizeof(path), "/day%02d.conf", day);
}

// Course import staging (see sched_stageDay).
static void stagePath(int day, char (&path)[16]) {
    snprintf(path, sizeof(path), "/day%02d.new", day);
}

static void dropStaged() {
    char path[16];
    for (int d = 0; d < DAY_COUNT; d++) {
        stagePath(d, path);
        if (LittleFS.exists(path)) LittleFS.remove(path);
    }
}

struct PreparedDay {
    ScheduleEntry items[MAX_SCHEDULES]{};
    uint8_t size = 0;
    uint32_t next = 1;
    active_day::Record record;
    uint64_t encoded = 0;
    String date;
};

static bool prepareRecord(PreparedDay& candidate, const active_day::Record& record) {
    if (!active_day::encode(record, DAY_COUNT, candidate.encoded)) return false;
    char text[11];
    active_day::formatDate(record.date, text);
    candidate.date = text;
    if (candidate.date.length() != strlen(text)) return false;
    candidate.record = record;
    return true;
}

static bool prepareFromPath(const char* path, PreparedDay& candidate);

// Fully allocated candidate; ESP32 Arduino 2.0.16 String moves never allocate.
static void publishPrepared(PreparedDay& candidate, bool running, bool pending) noexcept {
    sys_lock();
    for (unsigned i = 0; i < MAX_SCHEDULES; ++i) entries[i] = std::move(candidate.items[i]);
    count = candidate.size;
    nextId = candidate.next;
    activeDay = candidate.record.day;
    publishedActive = candidate.encoded;
    activeDate = std::move(candidate.date);
    schedPendingSave = pending;
    scheduleFileState = ScheduleFileState::Ready;
    activeStore.published();
    executionRunning = running;
    sys_unlock();
}

// Date-only transactions retain live edits and their pending-save ownership.
static bool setActiveDay(int day, const String& date) {
    if (!schedulerReady() || day != activeDay) return false;
    active_day::Record record;
    record.day = uint8_t(day);
    if (!active_day::parseDate(date.c_str(), record.date)) return false;
    std::unique_ptr<PreparedDay> candidate(new (std::nothrow) PreparedDay);
    if (!candidate || !prepareRecord(*candidate, record)) return false;
    candidate->size = count;
    candidate->next = nextId;
    for (unsigned i = 0; i < count; ++i) {
        candidate->items[i] = entries[i];
        if (candidate->items[i].description.length() != entries[i].description.length()) return false;
    }
    if (!activeStore.persist(record)) return false;
    publishPrepared(*candidate, true, schedPendingSave);
    return true;
}

static void loadActiveDay() {
    active_day::Record record;
    const active_day::Source source = activeStore.load(sys_fsOk(), record);
    if (source == active_day::Source::Empty || source == active_day::Source::Error) return;
    loadedRecord = record;
    loadedSource = source;
    std::unique_ptr<PreparedDay> candidate(new (std::nothrow) PreparedDay);
    char path[16];
    dayPath(record.day, path);
    if (!sys_fsOk() || !candidate || !prepareRecord(*candidate, record) || !prepareFromPath(path, *candidate)) {
        blockScheduleFile();  // The validated NVS classification is unchanged.
        return;
    }
    if (source == active_day::Source::Legacy && !activeStore.persist(record)) return;
    publishPrepared(*candidate, true, false);
}

// -------------------------------------------------------
// Auto-advance the active day when the calendar date changes. Compares dates
// (not a live "hour==0 && min==0" tick) so it still fires correctly even if
// the device was powered off/rebooting exactly at midnight, or if valid time
// only became available after midnight had already passed.
// -------------------------------------------------------
static void checkDateAdvance() {
    if (courseMode) {
        courseAdvance();
        return;
    }
    if (!schedulerReady() || activeDay < 0) return;
    String today = currentDateStr();
    if (!today.length()) return;
    if (!activeDate.length()) {
        // No date stamp yet (legacy / activated before time was set) — just
        // stamp today, don't advance (avoids a spurious jump).
        setActiveDay(activeDay, today);
        return;
    }
    if (today == activeDate) return;

    // M-16: a day-switch writes flash — if a gong is playing, wait for it to
    // finish instead of competing with audio for flash.
    static bool deferLogged = false;
    if (mp3_isPlaying()) {
        if (!deferLogged) logPrintf("[SCHED] Day switch deferred — audio is playing\n");
        deferLogged = true;
        return;
    }
    deferLogged = false;

    // M-15: advance by the ACTUAL number of calendar days elapsed, not always
    // +1 — a device left off for 3 days must not make the course drift by 2.
    int diff    = daysBetween(activeDate, today);
    int nextDay = activeDay + diff;
    if (nextDay >= DAY_COUNT) nextDay = DAY_COUNT - 1;
    char path[16];
    dayPath(nextDay, path);
    if (diff <= 0) {
        // Clock moved BACKWARDS (operator corrected a wrong date) or the
        // stored date is unparsable — that is not a day passing. Re-stamp.
        logPrintf("[SCHED] Date changed (%s -> %s) but not forward — staying on day %02d\n",
                  activeDate.c_str(), today.c_str(), activeDay);
        setActiveDay(activeDay, today);
    } else if (diff > DAY_COUNT) {
        // More days than a whole course — a clock correction, not days passing.
        logPrintf("[SCHED] Date jumped %d days (%s -> %s) — looks like a clock correction, "
                  "staying on day %02d (activate the right day in the UI)\n",
                  diff, activeDate.c_str(), today.c_str(), activeDay);
        setActiveDay(activeDay, today);
    } else if (nextDay != activeDay && LittleFS.exists(path)) {
        if (diff > 1) logPrintf("[SCHED] %d calendar day(s) elapsed while off\n", diff);
        logPrintf("[SCHED] Date changed (%s -> %s): day %02d -> %02d\n",
                  activeDate.c_str(), today.c_str(), activeDay, nextDay);
        sched_activateDay((uint8_t)nextDay);
    } else {
        logPrintf("[SCHED] Date changed (%s -> %s) but day %02d is the last day (or next day "
                  "file missing) — course ended, staying on day %02d\n",
                  activeDate.c_str(), today.c_str(), activeDay, activeDay);
        setActiveDay(activeDay, today);
    }
}

// -------------------------------------------------------
// Fire one entry: callback + anti-double-trigger bookkeeping + M-14 persisted
// watermark, shared by the normal per-second check and the boot catch-up.
// -------------------------------------------------------
static void doFire(const ScheduleEntry& e, const char* why) {
    logPrintf("[SCHED] %s %02d:%02d '%s' track=%d loop=%d vol=%d\n",
              why, e.hour, e.minute, e.description.c_str(), e.track, e.loop, e.vol);
    if (onScheduleTrigger) onScheduleTrigger(e.track, e.loop, e.vol);
    lastFiredKey    = e.hour * 60 + e.minute;
    lastFiredMillis = millis();
    const time_t now = time(nullptr);
    if (courseMode) {
        // Sound first, then the persisted identity. A failed write is logged
        // and the sound is not repeated (see spec 7).
        courseLastFire.generation = courseGeneration;
        courseLastFire.day_index = courseDay;
        courseLastFire.minute = (uint16_t)(e.hour * 60 + e.minute);
        courseLastFire.event_id = e.id;
        courseLastFire.date = courseDateNumber(courseDateFromClockNow());
        courseLastFire.timestamp = (uint32_t)now;
        courseLastFireValid = true;
        std::string err;
        if (!courseStore.saveLastFire(courseLastFire, err))
            logPrintf("[COURSE] last_fire save failed: %s\n", err.c_str());
    } else if ((uint32_t)now > TIME_VALID_EPOCH) {
        lastFireTs = (uint32_t)now;
        firePrefs.putUInt("lastfire", lastFireTs);
    }
}

// M-14: if the device rebooted in a narrow window around a gong's scheduled
// time, that gong must not be lost — but also must not double-fire if it
// already ran before the reboot. Only looks at TODAY's entries, and only
// within CATCHUP_WINDOW_S of "now" (a stale multi-hour-old miss is reported,
// not silently replayed).
// There used to be a `gap > 3600` bail-out here: catch-up only worked if the
// PREVIOUS gong was less than an hour before the reboot — a blip at 06:30:10
// after a 04:20 gong lost the 06:30 one. `fireT > lastFireTs` alone already
// rules out a double fire, and CATCHUP_WINDOW_S bounds how stale a replay can be.
static void sched_catchup() {
    if (!schedulerReady()) return;
    if (!timeIsSet() || lastFireTs == 0) return;
    time_t now = time(nullptr);
    if ((uint32_t)now <= lastFireTs) return;   // clock behind the watermark — don't guess

    struct tm tiNow;
    localtime_r(&now, &tiNow);
    for (uint8_t i = 0; i < count; i++) {
        if (!entries[i].enabled) continue;
        struct tm tiFire = tiNow;
        tiFire.tm_hour = entries[i].hour;
        tiFire.tm_min  = entries[i].minute;
        tiFire.tm_sec  = 0;
        time_t fireT = mktime(&tiFire);
        if (fireT > (time_t)lastFireTs && fireT <= now && (uint32_t)(now - fireT) <= CATCHUP_WINDOW_S) {
            doFire(entries[i], "Catch-up (missed during reboot):");
            break;   // one per boot is enough — same one-trigger-per-minute spirit as sched_check()
        }
    }
}

// -------------------------------------------------------
void sched_setup() {
    emergency::onLocalNow = localNow;
    emergency::onRecoveryCheck = courseRecoveryCheck;
#ifdef ESP32
    emergency::onReboot = []() { ESP.restart(); };
#endif
    emergency::begin();
    if (emergency::isActive()) {
        logPrintf("[SCHED] EMERGENCY active, skipping normal course load\n");
        return;
    }
    waitingForCourse = false;
    if (!firePrefs.begin("gong", false)) { activeStore.unknown(); return; }
    lastFireTs = firePrefs.getUInt("lastfire", 0);

    loadActiveDay();
    // A published course takes precedence over the legacy active-day key: once
    // an operator has applied a course, the legacy schedule must never play
    // again after a reboot — including while the course is APPLIED and not yet
    // activated. Devices without a course_state record keep the legacy path,
    // so existing installations are unaffected. courseTryBoot() also seeds the
    // reference course, which is only allowed when the legacy state is empty
    // and the filesystem confirms that no legacy schedule exists.
    const bool coursePublished = courseStore.hasCalendar();
    if ((coursePublished || activeStore.status() == active_day::Status::Empty) &&
        courseTryBoot()) {
        logPrintf("[COURSE] mode active, day %d, %d entries, running=%d\n",
                  activeDay, count, courseRunning ? 1 : 0);
        return;
    }
    if (activeStore.status() == active_day::Status::Empty) {
        // No published course and no legacy binding: this is a fresh JSON-driven
        // installation (the LittleFS image ships dayNN.conf, but nothing has
        // ever been activated). Nothing may ring — the device waits for the
        // operator to import and apply a JSON course. Auto-activating Day 00
        // here is what made a new device play the legacy schedule by itself.
        waitingForCourse = true;
        logPrintf("[SCHED] no published course and no legacy binding — waiting for JSON\n");
        return;
    }
    if (schedulerReady()) dropStaged();
    logPrintf("[SCHED] Day %02d, %d entries.\n", activeDay, count);

    // Date first: after a reboot across midnight, catch-up must look at
    // TODAY's schedule, not yesterday's.
    if (timeIsSet()) checkDateAdvance();
    if (!courseMode) sched_catchup();
}

// -------------------------------------------------------
// Called every second from main loop.
// Time comes from NTP when STA has internet, else DS3231, else a manual set
// in the web UI — see timesync.h.
// -------------------------------------------------------
void sched_check() {
    // A failed day load either arms a delayed reboot or enters EMERGENCY.
    // The reboot timer must run even while the normal scheduler is not ready.
    emergency::tickReboot();
    if (emergency::isActive()) {
        emergency::tick();
        return;
    }
    if (!schedulerReady()) return;
    // M-16: flush a save that was deferred while a gong was playing.
    if (schedPendingSave && !mp3_isPlaying()) {
        sched_saveNow();  // failure retains pending; the consistent live day can still ring
    }

    struct tm ti;
    if (!localNow(ti)) {
        static unsigned long lastWarn = 0;
        if (millis() - lastWarn >= 60000UL) {
            logPrintf("[SCHED] Skip: time not available.\n");
            lastWarn = millis();
        }
        return;
    }

    // Accept time only once it's actually a real date (from DS3231 or a
    // manual set) — the RTC being present does NOT mean it holds a valid time.
    bool timeValid = (ti.tm_year >= 124);
    if (!timeValid) {
        static unsigned long lastWarn = 0;
        if (millis() - lastWarn >= 60000UL) {
            logPrintf("[SCHED] Skip: no valid RTC/manual time set.\n");
            lastWarn = millis();
        }
        return;
    }

    int h   = ti.tm_hour;
    int m   = ti.tm_min;
    int key = h * 60 + m;

    if (millis() - lastTimeLog >= 60000UL) {
        logPrintf("[SCHED] Time %02d:%02d (entries=%d)\n", h, m, count);
        lastTimeLog = millis();
    }

    // Every second: a RAM-only compare now (was a 30 s poll reading
    // /activeday.conf), so a gong at 00:00 already uses the new day's schedule.
    checkDateAdvance();
    if (!schedulerReady()) return;
    if (courseMode && courseTransitionPending) return;

    // The RAM guard is lost on reboot; the persisted timestamp still protects
    // that absolute minute. Only use it before this boot's first firing:
    // catch-up records playback time, not necessarily the scheduled minute.
    if (lastFiredKey == -1 && lastFireTs != 0) {
        const time_t now = mktime(&ti);
        if (now / 60 == (time_t)lastFireTs / 60) return;
    }

    // Guard: prevent re-triggering within the same minute.
    // Use 65 s window (5 s margin) to handle NTP clock jitter.
    if (lastFiredKey == key && millis() - lastFiredMillis < 65000UL) return;

    if (courseMode) {
        const time_t now = mktime(&ti);
        if (courseLastFireValid) {
            // Clock moved backwards: do not replay a gong that may already
            // have fired before the correction.
            if ((uint32_t)now < courseLastFire.timestamp) return;
            const uint32_t today = (uint32_t)(ti.tm_year + 1900) * 10000u +
                (uint32_t)(ti.tm_mon + 1) * 100u + (uint32_t)ti.tm_mday;
            if (courseLastFire.generation == courseGeneration &&
                courseLastFire.day_index == courseDay &&
                courseLastFire.minute == (uint16_t)key &&
                courseLastFire.date == today) {
                return;   // this minute already fired for this day/generation
            }
        }
    }

    for (uint8_t i = 0; i < count; i++) {
        if (entries[i].enabled &&
            entries[i].hour   == (uint8_t)h &&
            entries[i].minute == (uint8_t)m) {
            doFire(entries[i], "Trigger");
            break; // one trigger per minute
        }
    }
}

// -------------------------------------------------------
bool sched_add(uint8_t h, uint8_t m, const String& desc, uint8_t track, uint8_t loop, uint8_t vol) {
    if (!schedulerReady()) return false;
    if (count >= MAX_SCHEDULES || h > 23 || m > 59) return false;
    if (track < 1 || track > 99) return false;
    if (sched_timeTaken(h, m, 0)) return false;   // slot already used this day
    if (loop < 1) loop = 1;
    if (loop > 7) loop = 7;
    if (vol > 30) vol = 30;
    entries[count++] = { nextId++, h, m, track, loop, vol, true, desc };
    sched_save();
    return true;
}

bool sched_edit(uint32_t id, uint8_t h, uint8_t m,
                const String& desc, uint8_t track, uint8_t loop, bool enabled, uint8_t vol) {
    if (!schedulerReady()) return false;
    if (h > 23 || m > 59) return false;
    if (track < 1 || track > 99) return false;
    if (sched_timeTaken(h, m, id)) return false;  // slot already used by another entry
    if (loop < 1) loop = 1;
    if (loop > 7) loop = 7;
    if (vol > 30) vol = 30;
    for (uint8_t i = 0; i < count; i++) {
        if (entries[i].id == id) {
            entries[i] = { id, h, m, track, loop, vol, enabled, desc };
            sched_save();
            return true;
        }
    }
    return false;
}

bool sched_del(uint32_t id) {
    if (!schedulerReady()) return false;
    for (uint8_t i = 0; i < count; i++) {
        if (entries[i].id == id) {
            for (uint8_t j = i; j < count - 1; j++) entries[j] = entries[j + 1];
            count--;
            sched_save();
            return true;
        }
    }
    return false;
}

// -------------------------------------------------------
// Serialized one entry at a time through a small pool. It used to build the
// whole day in one 12 KB DynamicJsonDocument — on every 5 s UI poll. Once WiFi
// and the audio task had fragmented the heap there was often no 12 KB block
// left even with 60+ KB free: /api/state then sent "schedule":[] and the page
// showed an empty table while the gongs still rang. The only big buffer now
// is the output String itself (~3.5 KB for a 16-entry day).
// -------------------------------------------------------
String sched_toJSON() {
    // Out of memory used to return "[]" — a NON-empty string, so
    // sched_saveNow() happily wrote an empty schedule over gong.conf AND the
    // active day's file. Every caller treats String() as "error, don't persist".
    String s;
    if (!s.reserve(64 + count * 200)) {
        logPrintf("[SCHED] ERROR: out of memory in sched_toJSON\n");
        return String();
    }
    s = "[";
    StaticJsonDocument<384> doc;   // 8 members + a desc of up to DESC_MAX_BYTES
    for (uint8_t i = 0; i < count; i++) {
        doc.clear();
        doc["id"]    = entries[i].id;
        doc["hour"]  = entries[i].hour;
        doc["min"]   = entries[i].minute;
        doc["track"] = entries[i].track;
        doc["loop"]  = entries[i].loop;
        doc["vol"]   = entries[i].vol;
        doc["en"]    = entries[i].enabled;
        doc["desc"]  = entries[i].description;
        // An overflowed entry would be written without its fields — that is
        // permanent data loss once saved; sched_save() refuses String().
        if (doc.overflowed()) {
            logPrintf("[SCHED] ERROR: JSON pool overflow in sched_toJSON (entry id=%u)\n",
                      (unsigned)entries[i].id);
            return String();
        }
        if (i) s += ',';
        serializeJson(doc, s);
    }
    s += ']';
    return s;
}

// M-16: the actual disk write, always synchronous. sched_save() (below) is
// the public entry point and defers to this when it's safe to write.
static bool sched_saveNow() {
    if (courseMode) return courseSaveActiveEntries();
    if (!schedulerReady()) return false;
    String json = sched_toJSON();
    if (json.length() == 0) {   // overflow marker from sched_toJSON
        logPrintf("[SCHED] Save ABORTED — JSON overflow, on-disk data left untouched\n");
        return false;
    }

    // /gong.conf is only a first-boot seed. A full byte count below is not
    // an atomic replacement or a close/flush durability acknowledgement (A04).
    char path[16];
    if (activeDay >= 0) dayPath(activeDay, path);
    else                snprintf(path, sizeof(path), "%s", SCHEDULE_FILE);
    File f = LittleFS.open(path, "w");
    if (!f) { logPrintf("[SCHED] Save failed (%s)\n", path); return false; }
    const size_t written = f.print(json);
    f.close();
    if (written != json.length()) return false;
    schedPendingSave = false;

    logPrintf("[SCHED] Saved %d entries to %s\n", count, path);
    return true;
}

void sched_save() {
    if (!schedulerReady()) return;
    schedPendingSave = true;
    if (mp3_isPlaying()) {
        // In-memory `entries[]` is already up to date at this point (every
        // caller mutates it before calling sched_save()), so the API/UI see
        // the change instantly — only the SPIFFS write waits a few seconds
        // for sched_check() to flush it once playback stops.
        schedPendingSave = true;
        logPrintf("[SCHED] Save deferred — audio is playing\n");
        return;
    }
    sched_saveNow();
}

static bool prepareFromPath(const char* path, PreparedDay& candidate) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    DynamicJsonDocument *doc = new (std::nothrow) DynamicJsonDocument(dayPoolSize(f.size()));
    if (!doc || doc->capacity() == 0) { delete doc; f.close(); return false; }
    if (deserializeJson(*doc, f) || !doc->is<JsonArray>()) {
        f.close(); delete doc; return false;
    }
    f.close();
    candidate.size = 0;
    candidate.next = 1;
    for (JsonObject o : doc->as<JsonArray>()) {
        if (candidate.size >= MAX_SCHEDULES) break;
        uint8_t track = o["track"] | 1;
        if (track < 1)  track = 1;
        if (track > 99) track = 99;
        uint8_t loop = o["loop"] | 1;
        if (loop < 1) loop = 1;
        if (loop > 7) loop = 7;
        uint8_t vol = o["vol"] | DEFAULT_VOLUME;   // M-12: legacy files without "vol" keep the old default
        if (vol > 30) vol = 30;
        uint32_t id = o["id"] | candidate.next;
        const char* description = o["desc"] | "";
        ScheduleEntry& entry = candidate.items[candidate.size++];
        entry = {
            id,
            (uint8_t)(o["hour"] | 0),
            (uint8_t)(o["min"]  | 0),
            track, loop, vol,
            (bool)(o["en"] | true),
            String(description)
        };
        if (entry.description.length() != strlen(description)) { delete doc; return false; }
        if (id >= candidate.next) candidate.next = id + 1;
    }
    delete doc;
    return true;
}

static bool sched_parseFromPath(const char* path) {
    if (!schedulerReady() || schedPendingSave) return false;
    std::unique_ptr<PreparedDay> candidate(new (std::nothrow) PreparedDay);
    active_day::Record record;
    record.day = uint8_t(activeDay);
    if (!active_day::parseDate(activeDate.c_str(), record.date) || !candidate ||
        !prepareRecord(*candidate, record) || !prepareFromPath(path, *candidate)) return false;
    publishPrepared(*candidate, true, false);
    return true;
}

bool sched_load() {
    sys_lock();
    struct Unlock { ~Unlock() { sys_unlock(); } } unlock;
    const bool blocked = scheduleFileState == ScheduleFileState::Blocked;
    if (schedPendingSave || (!blocked && !schedulerReady())) return false;
    const active_day::Status status = activeStore.status();
    if (status != active_day::Status::Ready && status != active_day::Status::Loaded) return false;
    active_day::Record record = loadedRecord;
    if (status == active_day::Status::Ready && !active_day::decode(publishedActive, DAY_COUNT, record)) return false;
    std::unique_ptr<PreparedDay> candidate(new (std::nothrow) PreparedDay);
    char path[16];
    dayPath(record.day, path);
    if (!sys_fsOk() || !candidate || !prepareRecord(*candidate, record) || !prepareFromPath(path, *candidate)) return false;
    // Only a previously deferred legacy migration needs an NVS transaction.
    if (status == active_day::Status::Loaded && loadedSource == active_day::Source::Legacy &&
        !activeStore.persist(record)) return false;
    publishPrepared(*candidate, !blocked, false);
    return true;
}

// -------------------------------------------------------
// Multi-day helpers
// -------------------------------------------------------
String sched_dayJSON(uint8_t day) {
    if (courseMode) return courseDayJson(day);
    char path[16];
    dayPath(day, path);
    if (!LittleFS.exists(path)) return "[]";
    File f = LittleFS.open(path, "r");
    if (!f) return "[]";
    String s = f.readString();
    f.close();
    return s;
}

bool sched_activateDay(uint8_t day) {
    if (emergency::isActive()) return false;   // recovery is a separate operation
    if (courseMode) return courseActivate(day);
    if (day >= DAY_COUNT) return false;
    const active_day::Status status = activeStore.status();
    if (scheduleFileState == ScheduleFileState::Blocked ||
        (!schedulerReady() && status != active_day::Status::Empty)) return false;
    if (!sys_fsOk()) return false;
    char path[16];
    dayPath(day, path);
    const active_day::FilePresence presence = active_day::probeFile(path);
    if (presence == active_day::FilePresence::Error) {
        if (status == active_day::Status::Empty) activeStore.unknown();
        return false;
    }
    if (presence == active_day::FilePresence::ConfirmedAbsent) {
        String seed("[]");
        if (status == active_day::Status::Empty) {
            const active_day::FilePresence seedPresence = active_day::probeFile(SCHEDULE_FILE);
            if (seedPresence == active_day::FilePresence::Error) { activeStore.unknown(); return false; }
            if (seedPresence == active_day::FilePresence::Present) {
                File old = LittleFS.open(SCHEDULE_FILE, "r");
                if (!old) { activeStore.unknown(); return false; }
                const size_t expected = old.size();
                seed = old.readString();
                old.close();
                if (!seed.length() || seed.length() != expected) { activeStore.unknown(); return false; }
            }
        }
        File file = LittleFS.open(path, "w");
        if (!file) {
            if (status == active_day::Status::Empty) activeStore.unknown();
            return false;
        }
        const size_t written = file.print(seed);
        file.close();
        if (written != seed.length()) return false;
    }

    // Flush current schedule to its day file before switching (activeDay
    // still points to the old day here). Always synchronous, unlike the
    // public sched_save() — deferring THIS specific write risks the flush
    // landing after entries[] has already been overwritten with the new
    // day's content below. File BLOCKED was rejected before this point, so
    // an activation cannot flush stale RAM over a replaced active-day file.
    if (schedPendingSave && !sched_saveNow()) return false;
    std::unique_ptr<PreparedDay> candidate(new (std::nothrow) PreparedDay);
    active_day::Record record;
    record.day = day;
    const String date = currentDateStr();
    if ((timeIsSet() && !date.length()) || !active_day::parseDate(date.c_str(), record.date) ||
        !candidate || !prepareRecord(*candidate, record) || !prepareFromPath(path, *candidate)) {
        if (status == active_day::Status::Empty) activeStore.unknown();
        return false;
    }
    if (!activeStore.persist(record)) return false;
    publishPrepared(*candidate, true, false);

    logPrintf("[SCHED] Activated day %02d (%d entries)\n", (int)day, count);
    return true;
}

int  sched_getActiveDay() { return activeDay; }
bool sched_hasError() {
    if (emergency::isActive()) return true;
    if (courseMode) return courseSuspended || scheduleFileState != ScheduleFileState::Ready;
    return scheduleFileState != ScheduleFileState::Ready || activeStore.status() != active_day::Status::Ready;
}
uint8_t sched_dayCount() { return courseMode ? courseLength : (uint8_t)DAY_COUNT; }
// True once a published course owns execution. The Course Editor's Activate
// must not fall through to the legacy day files before that.
bool sched_courseEnabled() { return courseMode; }

// Actual execution mode, so the operator never has to guess:
//   "course"  — a published JSON course owns execution (APPLIED or RUNNING);
//   "legacy"  — an existing installation with a legacy NVS binding;
//   "waiting" — no course and no legacy binding: nothing will ring.
const char* sched_runMode() {
    if (courseMode) return "course";
    if (waitingForCourse) return "waiting";
    return "legacy";
}

bool sched_waitingForCourse() { return waitingForCourse; }

active_day::Status sched_persistenceStatus() { return activeStore.status(); }
bool sched_isRunning() { return schedulerReady(); }

bool sched_recover(uint64_t expected, bool discardPending) {
    sys_lock();
    struct Unlock { ~Unlock() { sys_unlock(); } } unlock;
    if (scheduleFileState == ScheduleFileState::Blocked && activeStore.status() != active_day::Status::Unknown) return false;
    if (activeStore.status() == active_day::Status::Ready) return !executionRunning && expected == publishedActive;
    if (activeStore.status() != active_day::Status::Unknown || (schedPendingSave && !discardPending)) return false;
    active_day::Record record;
    uint64_t raw = 0;
    if (!activeStore.readActive(record, raw) || raw != expected) return false;
    std::unique_ptr<PreparedDay> candidate(new (std::nothrow) PreparedDay);
    char path[16];
    dayPath(record.day, path);
    if (!sys_fsOk() || !candidate || !prepareRecord(*candidate, record) ||
        !prepareFromPath(path, *candidate) || !activeStore.persist(record, true)) return false;
    publishPrepared(*candidate, false, false);
    return true;
}

bool sched_resume() {
    sys_lock();
    const bool ready = activeStore.status() == active_day::Status::Ready && scheduleFileState == ScheduleFileState::Ready;
    if (ready) executionRunning = true;
    sys_unlock();
    return ready;
}

bool sched_courseEnded() {
    // A course is cyclic: the last day is followed by day 0, so it never ends.
    if (courseMode) return false;
    return activeDay >= 0 && activeDay == DAY_COUNT - 1;
}

// -------------------------------------------------------
// Template editing (see schedule.h for the "why"): reads/writes /dayNN.conf
// directly via ArduinoJson, entirely independent of the in-memory `entries`
// array that represents the live/active day.
// -------------------------------------------------------
static bool dayTimeTaken(JsonArray arr, uint8_t h, uint8_t m, uint32_t excludeId) {
    for (JsonObject o : arr) {
        uint32_t id = o["id"] | 0;
        if (id == excludeId) continue;
        if ((uint8_t)(o["hour"] | 0) == h && (uint8_t)(o["min"] | 0) == m) return true;
    }
    return false;
}

// Pool for editing day N's template: sized by its file (see dayPoolSize).
static size_t dayEditPool(uint8_t day) {
    char path[16];
    dayPath(day, path);
    File f = LittleFS.open(path, "r");
    size_t n = f ? f.size() : 0;
    if (f) f.close();
    return dayPoolSize(n);
}

static bool isActiveDay(uint8_t day) {
    return activeDay >= 0 && day == (uint8_t)activeDay;
}

// Loads /dayNN.conf into `doc` and hands back its root array. A file that
// doesn't exist yet is an empty day; a file that EXISTS but doesn't parse is
// an error — it used to be treated as empty too, and the next add/edit then
// overwrote whatever was left of that day with a single entry.
static bool loadDayArray(uint8_t day, DynamicJsonDocument& doc, JsonArray& out) {
    if (doc.capacity() == 0) return false;   // allocation failed
    char path[16];
    dayPath(day, path);
    if (!LittleFS.exists(path)) { out = doc.to<JsonArray>(); return true; }
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    out = doc.as<JsonArray>();
    if (err || out.isNull()) {
        logPrintf("[SCHED] Day %02d: %s unreadable (%s) — edit refused\n",
                  (int)day, path, err ? err.c_str() : "not an array");
        return false;
    }
    return true;
}

static bool saveDayArray(uint8_t day, DynamicJsonDocument& doc) {
    if (doc.overflowed()) {   // entries were silently dropped — never persist
        logPrintf("[SCHED] Day %02d: JSON pool overflow — NOT saved\n", (int)day);
        return false;
    }
    char path[16];
    dayPath(day, path);
    File wf = LittleFS.open(path, "w");
    if (!wf) return false;
    serializeJson(doc, wf);
    wf.close();
    return true;
}

bool sched_addToDay(uint8_t day, uint8_t h, uint8_t m,
                     const String& desc, uint8_t track, uint8_t loop, uint8_t vol) {
    if (courseMode) {
        if (day >= courseLength) return false;
        if (courseDayValid && day == courseDay) return sched_add(h, m, desc, track, loop, vol);
        if (h > 23 || m > 59 || track < 1 || track > 99) return false;
        if (loop < 1) loop = 1;
        if (loop > 7) loop = 7;
        if (vol > 30) vol = 30;
        return courseDayAddEvent(day, h, m, desc, track, loop, vol);
    }
    if (!schedulerReady()) return false;
    if (isActiveDay(day)) return sched_add(h, m, desc, track, loop, vol);
    if (h > 23 || m > 59 || track < 1 || track > 99) return false;
    if (loop < 1) loop = 1;
    if (loop > 7) loop = 7;
    if (vol > 30) vol = 30;

    DynamicJsonDocument doc(dayEditPool(day));
    JsonArray arr;
    if (!loadDayArray(day, doc, arr)) return false;
    if ((int)arr.size() >= MAX_SCHEDULES) return false;
    if (dayTimeTaken(arr, h, m, 0)) return false;

    uint32_t maxId = 0;
    for (JsonObject o : arr) { uint32_t id = o["id"] | 0; if (id > maxId) maxId = id; }

    JsonObject o = arr.createNestedObject();
    o["id"]    = maxId + 1;
    o["hour"]  = h;
    o["min"]   = m;
    o["track"] = track;
    o["loop"]  = loop;
    o["vol"]   = vol;
    o["en"]    = true;
    o["desc"]  = desc;

    if (!saveDayArray(day, doc)) return false;
    logPrintf("[SCHED] Day %02d (template): added %02d:%02d '%s'\n",
                  (int)day, h, m, desc.c_str());
    return true;
}

bool sched_editInDay(uint8_t day, uint32_t id, uint8_t h, uint8_t m,
                     const String& desc, uint8_t track, uint8_t loop, bool enabled, uint8_t vol) {
    if (courseMode) {
        if (day >= courseLength) return false;
        if (courseDayValid && day == courseDay) return sched_edit(id, h, m, desc, track, loop, enabled, vol);
        if (h > 23 || m > 59 || track < 1 || track > 99) return false;
        if (loop < 1) loop = 1;
        if (loop > 7) loop = 7;
        if (vol > 30) vol = 30;
        return courseDayEditEvent(day, id, h, m, desc, track, loop, vol, enabled);
    }
    if (!schedulerReady()) return false;
    if (isActiveDay(day)) return sched_edit(id, h, m, desc, track, loop, enabled, vol);
    if (h > 23 || m > 59 || track < 1 || track > 99) return false;
    if (loop < 1) loop = 1;
    if (loop > 7) loop = 7;
    if (vol > 30) vol = 30;

    DynamicJsonDocument doc(dayEditPool(day));
    JsonArray arr;
    if (!loadDayArray(day, doc, arr)) return false;
    if (dayTimeTaken(arr, h, m, id)) return false;

    bool found = false;
    for (JsonObject o : arr) {
        if ((uint32_t)(o["id"] | 0) == id) {
            o["hour"]  = h;
            o["min"]   = m;
            o["track"] = track;
            o["loop"]  = loop;
            o["vol"]   = vol;
            o["en"]    = enabled;
            o["desc"]  = desc;
            found = true;
            break;
        }
    }
    if (!found) return false;

    if (!saveDayArray(day, doc)) return false;
    logPrintf("[SCHED] Day %02d (template): edited entry id=%u\n", (int)day, (unsigned)id);
    return true;
}

bool sched_delFromDay(uint8_t day, uint32_t id) {
    if (courseMode) {
        if (day >= courseLength) return false;
        if (courseDayValid && day == courseDay) return sched_del(id);
        return courseDayDeleteEvent(day, id);
    }
    if (!schedulerReady()) return false;
    if (isActiveDay(day)) return sched_del(id);

    DynamicJsonDocument doc(dayEditPool(day));
    JsonArray arr;
    if (!loadDayArray(day, doc, arr)) return false;

    int idx = -1, i = 0;
    for (JsonObject o : arr) {
        if ((uint32_t)(o["id"] | 0) == id) { idx = i; break; }
        i++;
    }
    if (idx < 0) return false;
    arr.remove(idx);

    if (!saveDayArray(day, doc)) return false;
    logPrintf("[SCHED] Day %02d (template): deleted entry id=%u\n", (int)day, (unsigned)id);
    return true;
}

// -------------------------------------------------------
// Whole-course import/export (see schedule.h). The browser expands the
// course file's templates and sends one day at a time; each day is checked
// with the same rules as a single-entry edit, renumbered from id 1 and
// written to /dayNN.new. Nothing touches the real day files until commit.
// -------------------------------------------------------
bool sched_stageDay(uint8_t day, JsonArrayConst in, String& err) {
    if (courseMode) { err = "structural import is not available in course mode"; return false; }
    if (!schedulerReady()) { err = "scheduler unavailable or paused"; return false; }
    if (day >= DAY_COUNT) { err = "invalid day"; return false; }
    if (in.isNull())      { err = "entries must be an array"; return false; }
    if (in.size() > MAX_SCHEDULES) { err = "more than " + String(MAX_SCHEDULES) + " entries"; return false; }
    if (day == 0) dropStaged();

    DynamicJsonDocument doc(SCHED_JSON_CAPACITY);
    if (doc.capacity() == 0) { err = "out of memory"; return false; }
    JsonArray out = doc.to<JsonArray>();
    uint32_t id = 1;
    for (JsonVariantConst v : in) {
        JsonObjectConst o = v.as<JsonObjectConst>();
        String where = "entry " + String(id) + ": ";
        if (o.isNull() || !o["hour"].is<int>() || !o["min"].is<int>()) {
            err = where + "hour and min are required"; return false;
        }
        int h = o["hour"], m = o["min"];
        if (h < 0 || h > 23 || m < 0 || m > 59) { err = where + "hour 0-23, min 0-59"; return false; }
        int track = o["track"] | DEFAULT_TRACK;
        if (track < 1 || track > 99) { err = where + "track 1-99"; return false; }
        if (!mp3_trackExists((uint8_t)track)) {
            err = where + "track " + String(track) + " not found on device"; return false;
        }
        if (dayTimeTaken(out, (uint8_t)h, (uint8_t)m, 0)) {
            char t[6];
            snprintf(t, sizeof(t), "%02d:%02d", h, m);
            err = where + t + " used twice"; return false;
        }
        String desc = o["desc"] | "";
        desc.trim();
        if (desc.length() > DESC_MAX_BYTES) {   // don't cut a UTF-8 character in half
            size_t k = DESC_MAX_BYTES;
            while (k > 0 && ((uint8_t)desc[k] & 0xC0) == 0x80) k--;
            desc.remove(k);
        }
        JsonObject e = out.createNestedObject();
        e["id"]    = id++;
        e["hour"]  = h;
        e["min"]   = m;
        e["track"] = track;
        e["loop"]  = constrain((int)(o["loop"] | 1), 1, LOOP_MAX);
        e["vol"]   = constrain((int)(o["vol"] | DEFAULT_VOLUME), 0, 30);
        e["en"]    = (bool)(o["en"] | true);
        e["desc"]  = desc;
    }
    if (doc.overflowed()) { err = "out of memory"; return false; }

    char path[16];
    stagePath(day, path);
    File f = LittleFS.open(path, "w");
    if (!f) { err = "file write failed"; return false; }
    serializeJson(doc, f);
    f.close();
    logPrintf("[SCHED] Course import: day %02d staged (%u entries)\n", (int)day, (unsigned)out.size());
    return true;
}

bool sched_commitCourse(String& err) {
    if (courseMode) { err = "structural import is not available in course mode"; return false; }
    if (!schedulerReady() || schedPendingSave) { err = "scheduler unavailable, paused or has unsaved edits"; return false; }
    char from[16], to[16];
    for (int d = 0; d < DAY_COUNT; d++) {
        stagePath(d, from);
        if (!LittleFS.exists(from)) {
            err = "day " + String(d) + " was not uploaded";
            return false;
        }
    }
    bool activeFileTouched = false;
    for (int d = 0; d < DAY_COUNT; d++) {
        stagePath(d, from);
        dayPath(d, to);
        // A failed rename/remove can also leave the active file indeterminate.
        if (d == activeDay) activeFileTouched = true;
        // LittleFS replaces an existing target on rename; the remove is only
        // a fallback for a VFS that refuses to.
        if (!LittleFS.rename(from, to) && !(LittleFS.remove(to) && LittleFS.rename(from, to))) {
            if (activeFileTouched) blockScheduleFile();
            err = "rename failed at day " + String(d) + " — days before it are already replaced";
            logPrintf("[SCHED] Course import: %s\n", err.c_str());
            return false;
        }
    }
    // The live schedule is the active day's file: reload it. A save deferred
    // while a gong was playing would write the OLD entries back over it.
    if (activeDay >= 0) {
        schedPendingSave = false;
        dayPath(activeDay, to);
        if (!sched_parseFromPath(to)) {
            blockScheduleFile();
            err = "active day file could not be loaded; scheduler BLOCKED";
            return false;
        }
    }
    logPrintf("[SCHED] Course import: %d days committed, active day %02d (%d entries)\n",
              DAY_COUNT, activeDay, count);
    return true;
}

String sched_courseJSON() {
    String s;
    const uint8_t days = sched_dayCount();
    if (!s.reserve((size_t)days * 2048)) return String();
    s = "{\"days\":[";
    for (int d = 0; d < days; d++) {
        if (d) s += ',';
        // The active day's in-memory entries are newer than its file while a
        // save is deferred.
        String day = isActiveDay((uint8_t)d) ? sched_toJSON() : sched_dayJSON((uint8_t)d);
        if (day.length() == 0) return String();
        s += day;
    }
    s += "]}";
    return s;
}

// -------------------------------------------------------
// Emergency status / manual retry (stage 4). These are thin wrappers so the
// Web layer can show the emergency state without including emergency.h.
// -------------------------------------------------------
bool sched_emergencyActive() { return emergency::isActive(); }
uint8_t sched_emergencyReason() { return emergency::reason(); }
const char* sched_emergencyReasonText() { return emergency::reasonText(emergency::reason()); }
uint8_t sched_failureCount() { return emergency::attempts(); }
bool sched_emergencyRetry() { return emergency::checkRecovery(); }

// -------------------------------------------------------
// Course editor facade (stage 5). Draft edits never touch the live scheduler;
// only sched_courseApply() republishes and clears the calendar binding.
// -------------------------------------------------------
static String schedCourseError;

const String& sched_courseError() { return schedCourseError; }

static bool courseFail(const std::string& err) {
    schedCourseError = err.c_str();
    return false;
}

// The draft operations rewrite the NVS calendar's draft_generation. Refresh
// the in-RAM copy so a later suspend/activation cannot write back a stale
// pointer.
static void courseReloadCalendar() {
    if (!courseStore.hasCalendar()) return;
    course_store::CalendarState cal;
    std::string err;
    // A calendar read is one short NVS blob, so it is taken under the lock
    // together with the RAM update: the control task cannot slip a suspend
    // between the read and the assignment and be overwritten here.
    bool loaded = false;
    sys_lock();
    loaded = courseStore.loadCalendar(cal, err);
    if (loaded) courseCal = cal;
    sys_unlock();
    if (!loaded) logPrintf("[COURSE] calendar reload failed: %s\n", err.c_str());
}

bool sched_courseInfoJSON(String& out) {
    schedCourseError = "";
    course_editor::Info info;
    std::string err;
    if (!course_editor::info(info, err)) return courseFail(err);
    // 1.25 KB: state keys, execution mode plus the course name.
    DynamicJsonDocument doc(1280);
    JsonObject root = doc.to<JsonObject>();
    root["has_course"] = info.hasCourse;
    // has_draft is true while an unpublished bootstrap draft exists too, so the
    // GUI can offer Apply before the first publish.
    root["has_draft"] = info.draftGeneration != 0;
    root["bootstrap"] = !info.hasCourse && info.draftGeneration != 0;
    root["active_generation"] = info.activeGeneration;
    root["draft_generation"] = info.draftGeneration;
    root["draft_dirty"] = info.draftDirty;
    root["length"] = info.length;
    root["name"] = info.name.c_str();
    root["flags"] = info.flags;
    root["failure_count"] = info.failureCount;
    root["activation_date"] = info.activationDate;
    root["course_mode"] = courseMode;
    root["running"] = courseRunning && !courseSuspended;
    root["suspended"] = courseSuspended;
    root["emergency"] = emergency::isActive();
    root["day_index"] = activeDay >= 0 ? activeDay : -1;
    root["scheduler_ready"] = schedulerReady();
    // The operator must be able to tell legacy execution from a JSON course.
    root["run_mode"] = sched_runMode();
    root["waiting_for_course"] = waitingForCourse;
    String body;
    serializeJson(doc, body);
    out = body;
    return true;
}

bool sched_courseDraftDayJSON(uint8_t day, String& out) {
    schedCourseError = "";
    std::string json;
    std::string err;
    if (!course_editor::dayJson(day, json, err)) return courseFail(err);
    out = json.c_str();
    return true;
}

bool sched_courseDraftSaveDay(uint8_t day, const String& json) {
    schedCourseError = "";
    std::string err;
    if (!course_editor::saveDay(day, json.c_str(), err)) return courseFail(err);
    courseReloadCalendar();
    return true;
}

bool sched_courseDraftClearDay(uint8_t day) {
    schedCourseError = "";
    std::string err;
    if (!course_editor::clearDay(day, err)) return courseFail(err);
    courseReloadCalendar();
    return true;
}

bool sched_courseInsertDay(uint8_t day, bool copyEvents) {
    schedCourseError = "";
    std::string err;
    if (!course_editor::insertDay(day, copyEvents, err)) return courseFail(err);
    courseReloadCalendar();
    return true;
}

bool sched_courseDeleteDay(uint8_t day) {
    schedCourseError = "";
    std::string err;
    if (!course_editor::deleteDay(day, err)) return courseFail(err);
    courseReloadCalendar();
    return true;
}

bool sched_coursePreset(uint8_t days) {
    schedCourseError = "";
    std::string err;
    if (!course_editor::setPreset(days, err)) return courseFail(err);
    courseReloadCalendar();
    return true;
}

bool sched_courseRename(const String& name) {
    schedCourseError = "";
    std::string err;
    if (!course_editor::renameCourse(name.c_str(), err)) return courseFail(err);
    courseReloadCalendar();
    return true;
}

bool sched_courseImport(const String& json) {
    schedCourseError = "";
    std::string err;
    // Layout-aware: template documents are stored compactly, full-event
    // documents keep the expanded layout.
    if (!course_editor::importCourseAuto(json.c_str(), json.length(), err)) {
        return courseFail(err);
    }
    courseReloadCalendar();
    return true;
}

bool sched_courseDiscardDraft() {
    schedCourseError = "";
    std::string err;
    if (!course_editor::discardDraft(err)) return courseFail(err);
    courseReloadCalendar();
    return true;
}

bool sched_courseApply() {
    schedCourseError = "";
    std::string err;
    if (!course_editor::applyDraft(err)) return courseFail(err);
    // Apply replaces the calendar binding with APPLIED: no day is bound and
    // nothing may ring until an explicit Activate.
    mp3_stop();
    courseReloadCalendar();
    sys_lock();
    courseMode = true;
    waitingForCourse = false;
    courseGeneration = courseCal.active_generation;
    courseLength = courseCal.course_length;
    courseRunning = false;
    courseSuspended = false;
    courseDayValid = false;
    courseTransitionPending = false;
    executionRunning = false;
    scheduleFileState = ScheduleFileState::Ready;
    count = 0;
    for (unsigned i = 0; i < MAX_SCHEDULES; ++i) entries[i] = ScheduleEntry{};
    activeDay = -1;
    activeDate = "";
    sys_unlock();
    // Preload: the operator may Activate immediately. A failure here is not
    // fatal for Apply (the course is published), but Activate and boot will
    // refuse to run until the set can be loaded.
    {
        std::string tplError;
        if (!courseStore.prepareActiveTemplates(courseGeneration, tplError)) {
            logPrintf("[COURSE] applied generation templates unavailable: %s\n",
                      tplError.c_str());
        }
    }
    logPrintf("[COURSE] draft applied: generation %u, %u days\n",
              (unsigned)courseGeneration, (unsigned)courseLength);
    return true;
}

bool sched_courseInspect(const String& json, uint8_t& days, uint16_t& events,
                         uint8_t& version, String& name) {
    schedCourseError = "";
    course_editor::Inspection inspection;
    std::string err;
    if (!course_editor::inspectCourse(json.c_str(), json.length(), inspection, err)) return courseFail(err);
    days = inspection.length;
    events = inspection.events;
    version = inspection.version;
    name = inspection.name.c_str();
    return true;
}

bool sched_courseExportBegin(bool activeOnly) {
    schedCourseError = "";
    std::string err;
    const course_editor::ExportSource source =
        activeOnly ? course_editor::kExportActive : course_editor::kExportDraft;
    if (!course_editor::beginExport(source, err)) return courseFail(err);
    return true;
}

bool sched_courseExportNext(String& chunk, bool& done) {
    schedCourseError = "";
    std::string part;
    std::string err;
    if (!course_editor::nextExportChunk(part, done, err)) return courseFail(err);
    chunk = part.c_str();
    return true;
}
