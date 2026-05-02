#include "schedule.h"
#include "config.h"
#include <SPIFFS.h>
#include <WiFi.h>
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

// -------------------------------------------------------
void sched_setup() {
    sched_load();
    Serial.printf("[SCHED] Loaded %d entries.\n", count);
}

// -------------------------------------------------------
// Called every second from main loop
// Uses system time configured via configTime() in wifi_connect()
// -------------------------------------------------------
void sched_check() {
    struct tm ti;
    if (!getLocalTime(&ti)) {
        static unsigned long lastWarn = 0;
        if (millis() - lastWarn >= 60000UL) {
            Serial.println("[SCHED] Skip: time not available.");
            lastWarn = millis();
        }
        return;
    }

    // Accept time from NTP (WiFi connected) or manually set by user (year >= 2024)
    bool timeValid = (WiFi.status() == WL_CONNECTED) || (ti.tm_year >= 124);
    if (!timeValid) {
        static unsigned long lastWarn = 0;
        if (millis() - lastWarn >= 60000UL) {
            Serial.println("[SCHED] Skip: no NTP and no manual time set.");
            lastWarn = millis();
        }
        return;
    }

    int h   = ti.tm_hour;
    int m   = ti.tm_min;
    int key = h * 60 + m;

    if (millis() - lastTimeLog >= 60000UL) {
        Serial.printf("[SCHED] Time %02d:%02d (entries=%d)\n", h, m, count);
        lastTimeLog = millis();
    }

    // Guard: prevent re-triggering within the same minute.
    // Use 65 s window (5 s margin) to handle NTP clock jitter.
    if (lastFiredKey == key && millis() - lastFiredMillis < 65000UL) return;

    for (uint8_t i = 0; i < count; i++) {
        if (entries[i].enabled &&
            entries[i].hour   == (uint8_t)h &&
            entries[i].minute == (uint8_t)m) {

            Serial.printf("[SCHED] Trigger %02d:%02d '%s' track=%d loop=%d\n",
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
    if (!f) { Serial.println("[SCHED] Save failed"); return; }
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

    Serial.printf("[SCHED] Saved %d entries\n", count);
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
        Serial.println("[SCHED] No file, starting empty");
        return;
    }
    if (!sched_parseFromPath(SCHEDULE_FILE))
        Serial.println("[SCHED] Parse error");
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
        Serial.printf("[SCHED] Day %02d not found\n", (int)day);
        return false;
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

    // Now update active day tracker
    File af = SPIFFS.open("/activeday.conf", "w");
    if (af) { af.printf("{\"day\":%d}", (int)day); af.close(); }

    Serial.printf("[SCHED] Activated day %02d (%d entries)\n", (int)day, count);
    return true;
}

int sched_getActiveDay() {
    if (!SPIFFS.exists("/activeday.conf")) return -1;
    File f = SPIFFS.open("/activeday.conf", "r");
    if (!f) return -1;
    DynamicJsonDocument doc(64);
    if (deserializeJson(doc, f)) { f.close(); return -1; }
    f.close();
    return doc["day"] | -1;
}

