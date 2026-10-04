#include "schedule.h"
#include "config.h"
#include "mp3handler.h"
#include <LittleFS.h>
#include <time.h>
#include <new>
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
// additionally checked at every save point below.
#define SCHED_JSON_CAPACITY 12288

void (*onScheduleTrigger)(uint8_t track, uint8_t loop, uint8_t vol) = nullptr;

static ScheduleEntry entries[MAX_SCHEDULES];
static uint8_t       count   = 0;
static uint32_t      nextId  = 1;

// Anti-double-trigger: remember which minute we last fired
static int           lastFiredKey    = -1;
static unsigned long lastFiredMillis = 0;
static unsigned long lastTimeLog     = 0;

// Active course day + the calendar date it was stamped with. Lives in NVS
// (was /activeday.conf): NVS writes are atomic, and uploadfs — which rewrites
// the whole LittleFS partition — no longer resets the course to Day 00.
// Cached in RAM, so the per-second date check costs no flash reads.
static int    activeDay  = -1;
static String activeDate = "";

// Active day's file missing/unreadable at boot — schedule is empty and no
// gong will ring. Surfaced in /api/status (red banner in the UI).
static bool   schedError = false;

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
static void sched_saveNow();

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

// Set active day + date stamp (does not touch entries).
static void setActiveDay(int day, const String& date) {
    activeDay  = day;
    activeDate = date;
    firePrefs.putInt("aday", day);
    firePrefs.putString("adate", date);
}

static void loadActiveDay() {
    if (firePrefs.isKey("aday")) {
        activeDay  = firePrefs.getInt("aday", -1);
        activeDate = firePrefs.getString("adate", "");
        if (activeDay >= DAY_COUNT) activeDay = -1;
        return;
    }
    // One-time import of the pre-6.2 /activeday.conf.
    File f = LittleFS.open("/activeday.conf", "r");
    if (!f) return;
    DynamicJsonDocument doc(96);
    bool ok = !deserializeJson(doc, f);
    f.close();
    int day = ok ? (int)(doc["day"] | -1) : -1;
    if (day < 0 || day >= DAY_COUNT) {
        logPrintf("[SCHED] /activeday.conf unreadable — not imported\n");
        return;
    }
    setActiveDay(day, String((const char*)(doc["date"] | "")));
    LittleFS.remove("/activeday.conf");
    logPrintf("[SCHED] Imported active day %02d from /activeday.conf into NVS\n", day);
}

// -------------------------------------------------------
// Auto-advance the active day when the calendar date changes. Compares dates
// (not a live "hour==0 && min==0" tick) so it still fires correctly even if
// the device was powered off/rebooting exactly at midnight, or if valid time
// only became available after midnight had already passed.
// -------------------------------------------------------
static void checkDateAdvance() {
    if (activeDay < 0) return;
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
    time_t now = time(nullptr);
    if ((uint32_t)now > TIME_VALID_EPOCH) {
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
    firePrefs.begin("gong", false);
    lastFireTs = firePrefs.getUInt("lastfire", 0);

    dropStaged();   // a course upload interrupted by a reboot is never committed
    loadActiveDay();

    if (activeDay >= 0) {
        // The active day's file is the single source of truth (/gong.conf used
        // to be a second copy, and a power cut between the two writes of a day
        // switch could make the next edit overwrite the wrong day's template).
        char path[16];
        dayPath(activeDay, path);
        if (!sched_parseFromPath(path)) {
            schedError = true;
            logPrintf("[SCHED] ERROR: %s missing or unreadable — schedule EMPTY, no gongs "
                      "until a day is activated in the web UI\n", path);
        }
    } else {
        // First-ever boot: auto-activate Day 00 (seeded from /gong.conf if
        // day00.conf doesn't exist yet) so the course machinery and its
        // midnight advance are live without anyone opening the web UI.
        sched_load();
        logPrintf("[SCHED] No active day set — auto-activating Day 00\n");
        sched_activateDay(0);
    }
    logPrintf("[SCHED] Day %02d, %d entries.\n", activeDay, count);

    // Date first: after a reboot across midnight, catch-up must look at
    // TODAY's schedule, not yesterday's.
    if (timeIsSet()) checkDateAdvance();
    sched_catchup();
}

// -------------------------------------------------------
// Called every second from main loop.
// Time comes from NTP when STA has internet, else DS3231, else a manual set
// in the web UI — see timesync.h.
// -------------------------------------------------------
void sched_check() {
    // M-16: flush a save that was deferred while a gong was playing.
    if (schedPendingSave && !mp3_isPlaying()) {
        schedPendingSave = false;
        sched_save();
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

    // Guard: prevent re-triggering within the same minute.
    // Use 65 s window (5 s margin) to handle NTP clock jitter.
    if (lastFiredKey == key && millis() - lastFiredMillis < 65000UL) return;

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
// Use heap for large JSON to avoid stack overflow on ESP32 (was 4KB on stack)
// -------------------------------------------------------
String sched_toJSON() {
    // Out of memory used to return "[]" — a NON-empty string, so
    // sched_saveNow() happily wrote an empty schedule over gong.conf AND the
    // active day's file. Every caller treats String() as "error, don't persist".
    DynamicJsonDocument *doc = new (std::nothrow) DynamicJsonDocument(SCHED_JSON_CAPACITY);
    if (!doc || doc->capacity() == 0) {
        delete doc;
        logPrintf("[SCHED] ERROR: out of memory in sched_toJSON\n");
        return String();
    }
    JsonArray arr = doc->to<JsonArray>();
    for (uint8_t i = 0; i < count; i++) {
        JsonObject o = arr.createNestedObject();
        o["id"]    = entries[i].id;
        o["hour"]  = entries[i].hour;
        o["min"]   = entries[i].minute;
        o["track"] = entries[i].track;
        o["loop"]  = entries[i].loop;
        o["vol"]   = entries[i].vol;
        o["en"]    = entries[i].enabled;
        o["desc"]  = entries[i].description;
    }
    // If the pool overflowed, entries were silently dropped — writing that
    // result to disk would be permanent data loss. Log loudly; sched_save()
    // below refuses to persist an overflowed snapshot.
    if (doc->overflowed())
        logPrintf("[SCHED] ERROR: JSON pool overflow in sched_toJSON — increase SCHED_JSON_CAPACITY\n");
    bool overflowed = doc->overflowed();
    String s;
    serializeJson(*doc, s);
    delete doc;
    if (overflowed) return String();   // empty marker — callers treat as error
    return s;
}

// M-16: the actual disk write, always synchronous. sched_save() (below) is
// the public entry point and defers to this when it's safe to write.
static void sched_saveNow() {
    String json = sched_toJSON();
    if (json.length() == 0) {   // overflow marker from sched_toJSON
        logPrintf("[SCHED] Save ABORTED — JSON overflow, on-disk data left untouched\n");
        return;
    }

    // The active day's file is the only copy (/gong.conf is just the
    // first-boot seed now). LittleFS commits a file atomically on close().
    char path[16];
    if (activeDay >= 0) dayPath(activeDay, path);
    else                snprintf(path, sizeof(path), "%s", SCHEDULE_FILE);
    File f = LittleFS.open(path, "w");
    if (!f) { logPrintf("[SCHED] Save failed (%s)\n", path); return; }
    f.print(json);
    f.close();
    schedError = false;   // an explicit edit replaced the unreadable file

    logPrintf("[SCHED] Saved %d entries to %s\n", count, path);
}

void sched_save() {
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

static bool sched_parseFromPath(const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    DynamicJsonDocument *doc = new (std::nothrow) DynamicJsonDocument(SCHED_JSON_CAPACITY);
    if (!doc || doc->capacity() == 0) { delete doc; f.close(); return false; }
    if (deserializeJson(*doc, f)) {
        f.close(); delete doc; return false;
    }
    f.close();
    count  = 0;
    nextId = 1;
    for (JsonObject o : doc->as<JsonArray>()) {
        if (count >= MAX_SCHEDULES) break;
        uint8_t track = o["track"] | 1;
        if (track < 1)  track = 1;
        if (track > 99) track = 99;
        uint8_t loop = o["loop"] | 1;
        if (loop < 1) loop = 1;
        if (loop > 7) loop = 7;
        uint8_t vol = o["vol"] | DEFAULT_VOLUME;   // M-12: legacy files without "vol" keep the old default
        if (vol > 30) vol = 30;
        uint32_t id = o["id"] | nextId;
        entries[count++] = {
            id,
            (uint8_t)(o["hour"] | 0),
            (uint8_t)(o["min"]  | 0),
            track, loop, vol,
            (bool)(o["en"] | true),
            String((const char*)(o["desc"] | ""))
        };
        if (id >= nextId) nextId = id + 1;
    }
    delete doc;
    return true;
}

void sched_load() {
    if (!LittleFS.exists(SCHEDULE_FILE)) {
        logPrintf("[SCHED] No file, starting empty\n");
        return;
    }
    if (!sched_parseFromPath(SCHEDULE_FILE))
        logPrintf("[SCHED] Parse error\n");
}

// -------------------------------------------------------
// Multi-day helpers
// -------------------------------------------------------
String sched_dayJSON(uint8_t day) {
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
    if (day >= DAY_COUNT) return false;
    char path[16];
    dayPath(day, path);
    if (!LittleFS.exists(path)) {
        File nf = LittleFS.open(path, "w");
        if (!nf) { logPrintf("[SCHED] Day %02d: create failed\n", (int)day); return false; }
        // First-ever activation (no active day yet): seed with current schedule so
        // existing entries are not lost. Subsequent new days start empty.
        bool firstActivation = (activeDay < 0);
        String seed = firstActivation ? sched_toJSON() : String("[]");
        if (seed.length() == 0) seed = "[]";   // overflow marker — never write ""
        nf.print(seed);
        nf.close();
        logPrintf("[SCHED] Day %02d created (%s)\n", (int)day,
                      firstActivation ? "seeded from current" : "empty");
    }

    // Flush current schedule to its day file before switching (activeDay
    // still points to the old day here). Always synchronous, unlike the
    // public sched_save() — deferring THIS specific write risks the flush
    // landing after entries[] has already been overwritten with the new
    // day's content below. Skipped while the old day's file failed to load:
    // entries[] is empty then, and flushing would wipe that file for good.
    if (activeDay >= 0 && !schedError) sched_saveNow();
    schedPendingSave = false;

    // Load new day into memory; on failure entries[] and activeDay are untouched.
    if (!sched_parseFromPath(path)) {
        logPrintf("[SCHED] Day %02d: %s unreadable — not activated\n", (int)day, path);
        return false;
    }

    // Only now switch the tracker (date-stamped so sched_check() can detect
    // day changes even across a missed midnight tick). A power cut before
    // this line simply leaves the old day active — consistent either way.
    setActiveDay(day, currentDateStr());
    schedError = false;

    logPrintf("[SCHED] Activated day %02d (%d entries)\n", (int)day, count);
    return true;
}

int  sched_getActiveDay() { return activeDay; }
bool sched_hasError()     { return schedError; }

bool sched_courseEnded() {
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
    if (isActiveDay(day)) return sched_add(h, m, desc, track, loop, vol);
    if (h > 23 || m > 59 || track < 1 || track > 99) return false;
    if (loop < 1) loop = 1;
    if (loop > 7) loop = 7;
    if (vol > 30) vol = 30;

    DynamicJsonDocument doc(SCHED_JSON_CAPACITY);
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
    if (isActiveDay(day)) return sched_edit(id, h, m, desc, track, loop, enabled, vol);
    if (h > 23 || m > 59 || track < 1 || track > 99) return false;
    if (loop < 1) loop = 1;
    if (loop > 7) loop = 7;
    if (vol > 30) vol = 30;

    DynamicJsonDocument doc(SCHED_JSON_CAPACITY);
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
    if (isActiveDay(day)) return sched_del(id);

    DynamicJsonDocument doc(SCHED_JSON_CAPACITY);
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
    char from[16], to[16];
    for (int d = 0; d < DAY_COUNT; d++) {
        stagePath(d, from);
        if (!LittleFS.exists(from)) {
            err = "day " + String(d) + " was not uploaded";
            return false;
        }
    }
    for (int d = 0; d < DAY_COUNT; d++) {
        stagePath(d, from);
        dayPath(d, to);
        // LittleFS replaces an existing target on rename; the remove is only
        // a fallback for a VFS that refuses to.
        if (!LittleFS.rename(from, to) && !(LittleFS.remove(to) && LittleFS.rename(from, to))) {
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
        schedError = !sched_parseFromPath(to);
    }
    logPrintf("[SCHED] Course import: %d days committed, active day %02d (%d entries)\n",
              DAY_COUNT, activeDay, count);
    return true;
}

String sched_courseJSON() {
    String s;
    if (!s.reserve(DAY_COUNT * 2048)) return String();
    s = "{\"days\":[";
    for (int d = 0; d < DAY_COUNT; d++) {
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
