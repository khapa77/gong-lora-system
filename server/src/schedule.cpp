#include "schedule.h"
#include "config.h"
#include <SPIFFS.h>
#include <time.h>
#include <new>
#include <ArduinoJson.h>

void (*onScheduleTrigger)(uint8_t track, uint8_t loop) = nullptr;

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
    if (!getLocalTime(&ti) || ti.tm_year < 124) return "";
    char buf[11];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
    return String(buf);
}

// Rewrite /activeday.conf with the given day + date (does not touch entries).
static void writeActiveDayFile(uint8_t day, const String& date) {
    File af = SPIFFS.open("/activeday.conf", "w");
    if (!af) return;
    af.printf("{\"day\":%d,\"date\":\"%s\"}", (int)day, date.c_str());
    af.close();
}

static String sched_getActiveDate() {
    if (!SPIFFS.exists("/activeday.conf")) return "";
    File f = SPIFFS.open("/activeday.conf", "r");
    if (!f) return "";
    DynamicJsonDocument doc(96);
    if (deserializeJson(doc, f)) { f.close(); return ""; }
    f.close();
    return String((const char*)(doc["date"] | ""));
}

// -------------------------------------------------------
void sched_setup() {
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
}

// -------------------------------------------------------
// Called every second from main loop.
// Time comes from DS3231 (RTC) or manual entry via the web UI — this device
// is a standalone WiFi access point with no internet access, so there is no
// NTP source.
// -------------------------------------------------------
void sched_check() {
    struct tm ti;
    if (!getLocalTime(&ti)) {
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
                uint8_t nextDay = (uint8_t)(activeDay + 1);
                char path[16];
                snprintf(path, sizeof(path), "/day%02d.conf", (int)nextDay);
                if (nextDay < DAY_COUNT && SPIFFS.exists(path)) {
                    logPrintf("[SCHED] Date changed (%s -> %s): day %02d -> %02d\n",
                                  stored.c_str(), today.c_str(), activeDay, nextDay);
                    sched_activateDay(nextDay);
                } else {
                    logPrintf("[SCHED] Date changed (%s -> %s) but day %02d has no next "
                                  "day file — course ended, staying on day %02d\n",
                                  stored.c_str(), today.c_str(), activeDay, activeDay);
                    writeActiveDayFile((uint8_t)activeDay, today);
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

            logPrintf("[SCHED] Trigger %02d:%02d '%s' track=%d loop=%d\n",
                          h, m, entries[i].description.c_str(), entries[i].track, entries[i].loop);

            if (onScheduleTrigger) onScheduleTrigger(entries[i].track, entries[i].loop);

            lastFiredKey    = key;
            lastFiredMillis = millis();
            break; // one trigger per minute
        }
    }
}

// -------------------------------------------------------
bool sched_add(uint8_t h, uint8_t m, const String& desc, uint8_t track, uint8_t loop) {
    if (count >= MAX_SCHEDULES || h > 23 || m > 59) return false;
    if (track < 1 || track > 99) return false;
    if (sched_timeTaken(h, m, 0)) return false;   // slot already used this day
    if (loop < 1) loop = 1;
    if (loop > 7) loop = 7;
    entries[count++] = { nextId++, h, m, track, loop, true, desc };
    sched_save();
    return true;
}

bool sched_edit(uint32_t id, uint8_t h, uint8_t m,
                const String& desc, uint8_t track, uint8_t loop, bool enabled) {
    if (h > 23 || m > 59) return false;
    if (track < 1 || track > 99) return false;
    if (sched_timeTaken(h, m, id)) return false;  // slot already used by another entry
    if (loop < 1) loop = 1;
    if (loop > 7) loop = 7;
    for (uint8_t i = 0; i < count; i++) {
        if (entries[i].id == id) {
            entries[i] = { id, h, m, track, loop, enabled, desc };
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
    DynamicJsonDocument *doc = new (std::nothrow) DynamicJsonDocument(4096);
    if (!doc) return "[]";
    JsonArray arr = doc->to<JsonArray>();
    for (uint8_t i = 0; i < count; i++) {
        JsonObject o = arr.createNestedObject();
        o["id"]    = entries[i].id;
        o["hour"]  = entries[i].hour;
        o["min"]   = entries[i].minute;
        o["track"] = entries[i].track;
        o["loop"]  = entries[i].loop;
        o["en"]    = entries[i].enabled;
        o["desc"]  = entries[i].description;
    }
    String s;
    serializeJson(*doc, s);
    delete doc;
    return s;
}

void sched_save() {
    String json = sched_toJSON();

    File f = SPIFFS.open(SCHEDULE_FILE, "w");
    if (!f) { logPrintf("[SCHED] Save failed\n"); return; }
    f.print(json);
    f.close();

    // Mirror every edit back into the active day file so changes survive day switches
    int day = sched_getActiveDay();
    if (day >= 0) {
        char path[16];
        snprintf(path, sizeof(path), "/day%02d.conf", day);
        File df = SPIFFS.open(path, "w");
        if (df) { df.print(json); df.close(); }
    }

    logPrintf("[SCHED] Saved %d entries\n", count);
}

static bool sched_parseFromPath(const char* path) {
    File f = SPIFFS.open(path, "r");
    if (!f) return false;
    DynamicJsonDocument *doc = new (std::nothrow) DynamicJsonDocument(4096);
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
        uint32_t id = o["id"] | nextId;
        entries[count++] = {
            id,
            (uint8_t)(o["hour"] | 0),
            (uint8_t)(o["min"]  | 0),
            track, loop,
            (bool)(o["en"] | true),
            String((const char*)(o["desc"] | ""))
        };
        if (id >= nextId) nextId = id + 1;
    }
    delete doc;
    return true;
}

void sched_load() {
    if (!SPIFFS.exists(SCHEDULE_FILE)) {
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
    if (!SPIFFS.exists(path)) return "[]";
    File f = SPIFFS.open(path, "r");
    if (!f) return "[]";
    String s = f.readString();
    f.close();
    return s;
}

bool sched_activateDay(uint8_t day) {
    char path[16];
    snprintf(path, sizeof(path), "/day%02d.conf", (int)day);
    if (!SPIFFS.exists(path)) {
        File nf = SPIFFS.open(path, "w");
        if (!nf) { logPrintf("[SCHED] Day %02d: create failed\n", (int)day); return false; }
        // First-ever activation (no activeday.conf): seed with current schedule so
        // existing entries are not lost. Subsequent new days start empty.
        bool firstActivation = (sched_getActiveDay() < 0);
        nf.print(firstActivation ? sched_toJSON() : String("[]"));
        nf.close();
        logPrintf("[SCHED] Day %02d created (%s)\n", (int)day,
                      firstActivation ? "seeded from current" : "empty");
    }

    // Flush current schedule to its day file before switching
    // (activeday.conf still points to the old day here)
    sched_save();

    // Load new day into memory
    if (!sched_parseFromPath(path)) return false;

    // Write new day to /gong.conf directly — do NOT call sched_save() here,
    // because activeday.conf still holds the old day number and would
    // overwrite the old day's file with the new day's content.
    String json = sched_toJSON();
    File gf = SPIFFS.open(SCHEDULE_FILE, "w");
    if (!gf) return false;
    gf.print(json);
    gf.close();

    // Now update active day tracker (date-stamped so sched_check() can detect
    // day changes even across a missed midnight tick — see sched_check()).
    writeActiveDayFile(day, currentDateStr());

    logPrintf("[SCHED] Activated day %02d (%d entries)\n", (int)day, count);
    return true;
}

int sched_getActiveDay() {
    if (!SPIFFS.exists("/activeday.conf")) return -1;
    File f = SPIFFS.open("/activeday.conf", "r");
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
    if (SPIFFS.exists(path)) {
        File f = SPIFFS.open(path, "r");
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
    char path[16];
    snprintf(path, sizeof(path), "/day%02d.conf", (int)day);
    File wf = SPIFFS.open(path, "w");
    if (!wf) return false;
    serializeJson(doc, wf);
    wf.close();
    return true;
}

bool sched_addToDay(uint8_t day, uint8_t h, uint8_t m,
                     const String& desc, uint8_t track, uint8_t loop) {
    if (isActiveDay(day)) return sched_add(h, m, desc, track, loop);
    if (h > 23 || m > 59 || track < 1 || track > 99) return false;
    if (loop < 1) loop = 1;
    if (loop > 7) loop = 7;

    DynamicJsonDocument doc(4096);
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
    o["en"]    = true;
    o["desc"]  = desc;

    if (!saveDayArray(day, doc)) return false;
    logPrintf("[SCHED] Day %02d (template): added %02d:%02d '%s'\n",
                  (int)day, h, m, desc.c_str());
    return true;
}

bool sched_editInDay(uint8_t day, uint32_t id, uint8_t h, uint8_t m,
                     const String& desc, uint8_t track, uint8_t loop, bool enabled) {
    if (isActiveDay(day)) return sched_edit(id, h, m, desc, track, loop, enabled);
    if (h > 23 || m > 59 || track < 1 || track > 99) return false;
    if (loop < 1) loop = 1;
    if (loop > 7) loop = 7;

    DynamicJsonDocument doc(4096);
    JsonArray arr = loadDayArray(day, doc);
    if (dayTimeTaken(arr, h, m, id)) return false;

    bool found = false;
    for (JsonObject o : arr) {
        if ((uint32_t)(o["id"] | 0) == id) {
            o["hour"]  = h;
            o["min"]   = m;
            o["track"] = track;
            o["loop"]  = loop;
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

    DynamicJsonDocument doc(4096);
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

