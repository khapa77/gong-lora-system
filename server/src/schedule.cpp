#include "schedule.h"
#include "config.h"
#include <SPIFFS.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <WiFi.h>

void (*onScheduleTrigger)(uint8_t track) = nullptr;

static ScheduleEntry entries[MAX_SCHEDULES];
static uint8_t       count   = 0;
static uint32_t      nextId  = 1;

static WiFiUDP    ntpUDP;
static NTPClient  ntp(ntpUDP, NTP_SERVER, NTP_UTC_OFFSET, NTP_INTERVAL_MS);

// Anti-double-trigger: remember which minute we last fired
static int           lastFiredKey   = -1;
static unsigned long lastFiredMillis = 0;

// -------------------------------------------------------
void sched_setup() {
    sched_load();
    ntp.begin();
    Serial.printf("[SCHED] Loaded %d entries. NTP started.\n", count);
}

// -------------------------------------------------------
// Called every second from main loop
// -------------------------------------------------------
void sched_check() {
    if (WiFi.status() != WL_CONNECTED) return;
    ntp.update();
    if (!ntp.isTimeSet()) return;

    int h   = ntp.getHours();
    int m   = ntp.getMinutes();
    int key = h * 60 + m;

    // Reset fire-lock after this minute passes (>61 s guard)
    if (lastFiredKey == key && millis() - lastFiredMillis < 61000) return;

    for (uint8_t i = 0; i < count; i++) {
        if (entries[i].enabled &&
            entries[i].hour   == (uint8_t)h &&
            entries[i].minute == (uint8_t)m) {

            Serial.printf("[SCHED] Trigger %02d:%02d '%s' track=%d\n",
                          h, m, entries[i].description.c_str(), entries[i].track);

            if (onScheduleTrigger) onScheduleTrigger(entries[i].track);

            lastFiredKey    = key;
            lastFiredMillis = millis();
            break; // one trigger per minute is enough
        }
    }
}

// -------------------------------------------------------
bool sched_add(uint8_t h, uint8_t m, const String& desc, uint8_t track) {
    if (count >= MAX_SCHEDULES || h > 23 || m > 59) return false;
    entries[count++] = { nextId++, h, m, track, true, desc };
    sched_save();
    return true;
}

bool sched_edit(uint32_t id, uint8_t h, uint8_t m,
                const String& desc, uint8_t track, bool enabled) {
    if (h > 23 || m > 59) return false;
    for (uint8_t i = 0; i < count; i++) {
        if (entries[i].id == id) {
            entries[i] = { id, h, m, track, enabled, desc };
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
String sched_toJSON() {
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.to<JsonArray>();
    for (uint8_t i = 0; i < count; i++) {
        JsonObject o = arr.createNestedObject();
        o["id"]    = entries[i].id;
        o["hour"]  = entries[i].hour;
        o["min"]   = entries[i].minute;
        o["track"] = entries[i].track;
        o["en"]    = entries[i].enabled;
        o["desc"]  = entries[i].description;
    }
    String s;
    serializeJson(doc, s);
    return s;
}

void sched_save() {
    File f = SPIFFS.open(SCHEDULE_FILE, "w");
    if (!f) { Serial.println("[SCHED] Save failed"); return; }
    f.print(sched_toJSON());
    f.close();
    Serial.printf("[SCHED] Saved %d entries\n", count);
}

void sched_load() {
    if (!SPIFFS.exists(SCHEDULE_FILE)) {
        Serial.println("[SCHED] No file, starting empty");
        return;
    }
    File f = SPIFFS.open(SCHEDULE_FILE, "r");
    if (!f) return;

    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, f)) {
        Serial.println("[SCHED] Parse error");
        f.close();
        return;
    }
    f.close();

    count = 0;
    for (JsonObject o : doc.as<JsonArray>()) {
        if (count >= MAX_SCHEDULES) break;
        uint32_t id = o["id"] | nextId;
        entries[count++] = {
            id,
            (uint8_t)(o["hour"]  | 0),
            (uint8_t)(o["min"]   | 0),
            (uint8_t)(o["track"] | 1),
            (bool)   (o["en"]    | true),
            String((const char*)(o["desc"] | ""))
        };
        if (id >= nextId) nextId = id + 1;
    }
}
