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
// DROPPED on save → data loss). 8192 covers 32 entries with ~25 % headroom;
// overflow is additionally checked at every save point below.
#define SCHED_JSON_CAPACITY 8192

void (*onScheduleTrigger)(uint8_t track, uint8_t loop, uint8_t vol) = nullptr;

static ScheduleEntry entries[MAX_SCHEDULES];
static uint8_t       count   = 0;
static uint32_t      nextId  = 1;

// Anti-double-trigger: remember which minute we last fired
static int           lastFiredKey    = -1;
static unsigned long lastFiredMillis = 0;
static unsigned long lastTimeLog     = 0;

// Date-based auto-advance: how often we compare "today" against the date
// stamped in /activeday.conf. Cheap check, no need to run every second.
static unsigned long lastDateCheckMs = 0;

// H-5 / lora-ds-autonomy: set whenever the active schedule changes (add/edit/
// del/activate); scheduleChangedAtMs resets on every subsequent change, so
// sched_pendingBroadcastReady() only fires once edits have gone quiet for the
// configured debounce window instead of once per save.
static volatile bool     scheduleDirty      = true;   // force one broadcast after boot
static volatile uint32_t scheduleChangedAtMs = 0;

static void markScheduleDirty() {
    scheduleDirty       = true;
    scheduleChangedAtMs = millis();
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

// Rewrite /activeday.conf with the given day + date (does not touch entries).
static void writeActiveDayFile(uint8_t day, const String& date) {
    File af = LittleFS.open("/activeday.conf", "w");
    if (!af) return;
    af.printf("{\"day\":%d,\"date\":\"%s\"}", (int)day, date.c_str());
    af.close();
}

static String sched_getActiveDate() {
    if (!LittleFS.exists("/activeday.conf")) return "";
    File f = LittleFS.open("/activeday.conf", "r");
    if (!f) return "";
    DynamicJsonDocument doc(96);
    if (deserializeJson(doc, f)) { f.close(); return ""; }
    f.close();
    return String((const char*)(doc["date"] | ""));
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
static void sched_catchup() {
    if (!timeIsSet() || lastFireTs == 0) return;
    time_t now = time(nullptr);
    uint32_t gap = (uint32_t)now - lastFireTs;
    if (gap == 0 || gap > 3600UL) return;   // too long ago, or clock moved backwards — don't guess

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

    sched_load();
    logPrintf("[SCHED] Loaded %d entries.\n", count);

    // First-ever boot (no /activeday.conf): auto-activate Day 00 so the
    // multi-day course machinery (and its midnight advance) is live from the
    // start, instead of silently sitting on whatever /gong.conf happened to
    // contain until someone opens the web UI and clicks a day.
    if (sched_getActiveDay() < 0) {
        logPrintf("[SCHED] No active day set — auto-activating Day 00\n");
        sched_activateDay(0);
    }

    sched_catchup();
}

// -------------------------------------------------------
// Called every second from main loop.
// Time comes from DS3231 (RTC) or manual entry via the web UI — this device
// is a standalone WiFi access point with no internet access, so there is no
// NTP source.
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

    // Auto-advance the active day when the calendar date changes. Compares
    // dates (not a live "hour==0 && min==0" tick) so it still fires correctly
    // even if the device was powered off/rebooting exactly at midnight, or if
    // valid time only became available after midnight had already passed.
    if (millis() - lastDateCheckMs >= 30000UL) {
        lastDateCheckMs = millis();
        int activeDay = sched_getActiveDay();
        if (activeDay >= 0) {
            String today  = currentDateStr();
            String stored = sched_getActiveDate();
            if (today.length() && stored.length() && today != stored) {
                // M-16: a day-switch does several SPIFFS writes — if a gong
                // happens to be playing exactly at this 30s check, wait for
                // the next one instead of competing with audio for flash.
                if (mp3_isPlaying()) {
                    logPrintf("[SCHED] Day switch deferred — audio is playing\n");
                } else {
                    // M-15: advance by the ACTUAL number of calendar days
                    // elapsed, not always +1 — a device left off for 3 days
                    // must not make the course drift by 2.
                    int diff = daysBetween(stored, today);
                    if (diff <= 0) diff = 1;
                    int nextDay = activeDay + diff;
                    if (nextDay >= DAY_COUNT) nextDay = DAY_COUNT - 1;
                    char path[16];
                    snprintf(path, sizeof(path), "/day%02d.conf", nextDay);
                    if (LittleFS.exists(path)) {
                        if (diff > 1)
                            logPrintf("[SCHED] %d calendar day(s) elapsed while off\n", diff);
                        logPrintf("[SCHED] Date changed (%s -> %s): day %02d -> %02d\n",
                                      stored.c_str(), today.c_str(), activeDay, nextDay);
                        sched_activateDay((uint8_t)nextDay);
                    } else {
                        logPrintf("[SCHED] Date changed (%s -> %s) but day %02d has no next "
                                      "day file — course ended, staying on day %02d\n",
                                      stored.c_str(), today.c_str(), activeDay, activeDay);
                        writeActiveDayFile((uint8_t)activeDay, today);
                    }
                }
            } else if (!stored.length() && today.length()) {
                // Legacy/first-run /activeday.conf without a date stamp — just
                // stamp today, don't advance (avoids a spurious jump on upgrade).
                writeActiveDayFile((uint8_t)activeDay, today);
            }
        }
    }

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
    DynamicJsonDocument *doc = new (std::nothrow) DynamicJsonDocument(SCHED_JSON_CAPACITY);
    if (!doc) return "[]";
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

    File f = LittleFS.open(SCHEDULE_FILE, "w");
    if (!f) { logPrintf("[SCHED] Save failed\n"); return; }
    f.print(json);
    f.close();

    // Mirror every edit back into the active day file so changes survive day switches
    int day = sched_getActiveDay();
    if (day >= 0) {
        char path[16];
        snprintf(path, sizeof(path), "/day%02d.conf", day);
        File df = LittleFS.open(path, "w");
        if (df) { df.print(json); df.close(); }
    }

    markScheduleDirty();   // H-5
    logPrintf("[SCHED] Saved %d entries\n", count);
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
    if (!doc) { f.close(); return false; }
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
    snprintf(path, sizeof(path), "/day%02d.conf", (int)day);
    if (!LittleFS.exists(path)) return "[]";
    File f = LittleFS.open(path, "r");
    if (!f) return "[]";
    String s = f.readString();
    f.close();
    return s;
}

bool sched_activateDay(uint8_t day) {
    char path[16];
    snprintf(path, sizeof(path), "/day%02d.conf", (int)day);
    if (!LittleFS.exists(path)) {
        File nf = LittleFS.open(path, "w");
        if (!nf) { logPrintf("[SCHED] Day %02d: create failed\n", (int)day); return false; }
        // First-ever activation (no activeday.conf): seed with current schedule so
        // existing entries are not lost. Subsequent new days start empty.
        bool firstActivation = (sched_getActiveDay() < 0);
        String seed = firstActivation ? sched_toJSON() : String("[]");
        if (seed.length() == 0) seed = "[]";   // overflow marker — never write ""
        nf.print(seed);
        nf.close();
        logPrintf("[SCHED] Day %02d created (%s)\n", (int)day,
                      firstActivation ? "seeded from current" : "empty");
    }

    // Flush current schedule to its day file before switching (activeday.conf
    // still points to the old day here). Always synchronous, unlike the
    // public sched_save() — deferring THIS specific write risks the flush
    // landing after entries[] has already been overwritten with the new
    // day's content below.
    sched_saveNow();

    // Load new day into memory
    if (!sched_parseFromPath(path)) return false;

    // Write new day to /gong.conf directly — do NOT call sched_save() here,
    // because activeday.conf still holds the old day number and would
    // overwrite the old day's file with the new day's content.
    String json = sched_toJSON();
    if (json.length() == 0) return false;   // overflow marker — don't persist
    File gf = LittleFS.open(SCHEDULE_FILE, "w");
    if (!gf) return false;
    gf.print(json);
    gf.close();

    // Now update active day tracker (date-stamped so sched_check() can detect
    // day changes even across a missed midnight tick — see sched_check()).
    writeActiveDayFile(day, currentDateStr());

    markScheduleDirty();   // H-5
    logPrintf("[SCHED] Activated day %02d (%d entries)\n", (int)day, count);
    return true;
}

int sched_getActiveDay() {
    if (!LittleFS.exists("/activeday.conf")) return -1;
    File f = LittleFS.open("/activeday.conf", "r");
    if (!f) return -1;
    DynamicJsonDocument doc(96);
    if (deserializeJson(doc, f)) { f.close(); return -1; }
    f.close();
    return doc["day"] | -1;
}

bool sched_courseEnded() {
    int day = sched_getActiveDay();
    return day >= 0 && day == DAY_COUNT - 1;
}

// H-5: enabled entries of the active (in-memory) day, packed for LoRa.
uint8_t sched_activeBinSnapshot(SchedBin* out, uint8_t maxCount) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < count && n < maxCount; i++) {
        if (!entries[i].enabled) continue;
        out[n].hour   = entries[i].hour;
        out[n].minute = entries[i].minute;
        out[n].track  = entries[i].track;
        out[n].vol    = entries[i].vol;
        out[n].loopEn = schedbin_pack(entries[i].loop, true);
        n++;
    }
    return n;
}

bool sched_pendingBroadcastReady(uint32_t debounceMs) {
    if (!scheduleDirty) return false;
    if (millis() - scheduleChangedAtMs < debounceMs) return false;   // still coalescing
    scheduleDirty = false;
    return true;
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
    int active = sched_getActiveDay();
    return active >= 0 && day == (uint8_t)active;
}

// Loads /dayNN.conf into `doc` and returns its root as a JsonArray (creating
// an empty array in `doc` if the file doesn't exist yet or fails to parse).
static JsonArray loadDayArray(uint8_t day, DynamicJsonDocument& doc) {
    char path[16];
    snprintf(path, sizeof(path), "/day%02d.conf", (int)day);
    if (LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        if (f) {
            bool ok = !deserializeJson(doc, f);
            f.close();
            if (ok) {
                JsonArray arr = doc.as<JsonArray>();
                if (!arr.isNull()) return arr;
            }
        }
    }
    return doc.to<JsonArray>();
}

static bool saveDayArray(uint8_t day, DynamicJsonDocument& doc) {
    if (doc.overflowed()) {   // entries were silently dropped — never persist
        logPrintf("[SCHED] Day %02d: JSON pool overflow — NOT saved\n", (int)day);
        return false;
    }
    char path[16];
    snprintf(path, sizeof(path), "/day%02d.conf", (int)day);
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
    JsonArray arr = loadDayArray(day, doc);
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
    JsonArray arr = loadDayArray(day, doc);
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
    JsonArray arr = loadDayArray(day, doc);

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
