#include "webhandler.h"
#include "config.h"
#include "schedule.h"
#include "mp3handler.h"
#include "rtchandler.h"
#include "lorahandler.h"
#include <SPIFFS.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

static WebServer server(80);
// Time source is chosen explicitly by the user (RTC / Manual tab) — no automatic priority.
enum class TimeSrc { RTC, MANUAL };
static TimeSrc timeSrc = TimeSrc::RTC;

// -------------------------------------------------------
// Auth
// -------------------------------------------------------
static bool   authEnabled  = false;
static String authPassword = "";

static void loadAuth() {
    if (!SPIFFS.exists(AUTH_CONFIG_FILE)) return;
    File f = SPIFFS.open(AUTH_CONFIG_FILE, "r");
    if (!f) return;
    DynamicJsonDocument doc(256);
    if (!deserializeJson(doc, f)) {
        authEnabled  = doc["enabled"]  | false;
        authPassword = doc["password"] | String("");
    }
    f.close();
    logPrintf("[AUTH] %s\n", authEnabled ? "enabled" : "disabled");
}

static void saveAuth() {
    File f = SPIFFS.open(AUTH_CONFIG_FILE, "w");
    if (!f) return;
    DynamicJsonDocument doc(256);
    doc["enabled"]  = authEnabled;
    doc["password"] = authPassword;
    serializeJson(doc, f);
    f.close();
}

static bool checkAuth() {
    if (!authEnabled || authPassword.length() == 0) return true;
    if (server.authenticate("admin", authPassword.c_str())) return true;

    server.sendHeader("WWW-Authenticate", String("Basic realm=\"") + AUTH_REALM + "\"");
    server.send(401, "text/plain", "Unauthorized");
    return false;
}

// -------------------------------------------------------
// Helpers
// -------------------------------------------------------
static void cors(WebServer& s) {
    s.sendHeader("Access-Control-Allow-Origin", "*");
    s.sendHeader("Access-Control-Allow-Methods", "GET,POST,PUT,DELETE,OPTIONS");
    s.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

static void sendJSON(int code, const String& body) {
    cors(server);
    server.send(code, "application/json", body);
}

static void sendOK()   { sendJSON(200, "{\"ok\":true}"); }
static void sendErr(const char* msg) {
    String s = "{\"ok\":false,\"err\":\"";
    s += msg;
    s += "\"}";
    sendJSON(400, s);
}

// -------------------------------------------------------
// CORS preflight
// -------------------------------------------------------
static void handleOptions() {
    cors(server);
    server.send(200, "text/plain", "OK");
}

// -------------------------------------------------------
// Static files
// -------------------------------------------------------
static void handleRoot() {
    if (!checkAuth()) return;
    if (SPIFFS.exists("/index.html")) {
        File f = SPIFFS.open("/index.html", "r");
        server.streamFile(f, "text/html");
        f.close();
    } else {
        server.send(200, "text/html",
            "<h1>Gong Server</h1>"
            "<p>Upload SPIFFS data to get the full web interface.</p>");
    }
}

// -------------------------------------------------------
// /api/schedule
// -------------------------------------------------------
static void handleScheduleGET() {
    if (!checkAuth()) return;
    String json = sched_toJSON();
    if (json.length() == 0) { sendErr("schedule JSON overflow — see logs"); return; }
    sendJSON(200, json);
}

static void handleSchedulePOST() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, server.arg("plain"))) { sendErr("bad json"); return; }
    uint8_t h     = doc["hour"]  | 0;
    uint8_t m     = doc["min"]   | 0;
    uint8_t track = doc["track"] | 1;
    uint8_t loop  = doc["loop"]  | 1;
    String  desc  = doc["desc"]  | "";
    if (sched_add(h, m, desc, track, loop)) sendOK();
    else sendErr("failed (schedule full, bad time/track, or that time slot is already used)");
}

static void handleSchedulePUT() {
    if (!checkAuth()) return;
    uint32_t id = server.arg("id").toInt();
    if (!id) { sendErr("missing id"); return; }
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, server.arg("plain"))) { sendErr("bad json"); return; }
    uint8_t h     = doc["hour"]  | 0;
    uint8_t m     = doc["min"]   | 0;
    uint8_t track = doc["track"] | 1;
    uint8_t loop  = doc["loop"]  | 1;
    bool    en    = doc["en"]    | true;
    String  desc  = doc["desc"]  | "";
    if (sched_edit(id, h, m, desc, track, loop, en)) sendOK();
    else sendErr("not found, bad time/track, or that time slot is already used");
}

static void handleScheduleDELETE() {
    if (!checkAuth()) return;
    uint32_t id = server.arg("id").toInt();
    if (!id) { sendErr("missing id"); return; }
    if (sched_del(id)) sendOK();
    else sendErr("not found");
}

// -------------------------------------------------------
// /api/time  — set system time manually
// -------------------------------------------------------
// The calendar day of the schedule (see sched_check()'s date-based auto
// advance) depends on the ESP32's system DATE, not just its clock. Previous
// versions of this handler always pinned the date to a fixed 2024-01-01
// stub, on the assumption that "only HH:MM matters for schedule" — that
// stopped being true once day-switching started comparing calendar dates.
// So: keep whatever valid date the system already has (RTC or an earlier
// manual set) unless the caller explicitly supplies year/month/day.
// -------------------------------------------------------
static void handleTimeSet() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(128);
    if (deserializeJson(doc, server.arg("plain"))) { sendErr("bad json"); return; }
    int h = doc["hour"] | -1;
    int m = doc["min"]  | -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) { sendErr("invalid time"); return; }

    struct tm cur;
    bool haveCur = getLocalTime(&cur) && cur.tm_year >= 124;
    int y  = doc["year"]  | (haveCur ? cur.tm_year + 1900 : 2024);
    int mo = doc["month"] | (haveCur ? cur.tm_mon + 1     : 1);
    int d  = doc["day"]   | (haveCur ? cur.tm_mday        : 1);
    if (y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31) { sendErr("invalid date"); return; }

    struct tm ti = {};
    ti.tm_year = y - 1900;
    ti.tm_mon  = mo - 1;
    ti.tm_mday = d;
    ti.tm_hour = h;
    ti.tm_min  = m;
    ti.tm_sec  = 0;
    time_t t = mktime(&ti);
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    timeSrc = TimeSrc::MANUAL;
    rtc_syncFromSystem();   // persist to DS3231 so it survives next reboot

    sendOK();
    logPrintf("[TIME] Manual time set: %04d-%02d-%02d %02d:%02d\n", y, mo, d, h, m);
}

// POST /api/time/source?s=rtc|manual
static void handleTimeSource() {
    if (!checkAuth()) return;
    String src = server.arg("s");
    if (src == "manual") {
        timeSrc = TimeSrc::MANUAL;
        logPrintf("[TIME] Manual mode active\n");
    } else {
        timeSrc = TimeSrc::RTC;
        if (!rtc_loadToSystem()) {
            logPrintf("[TIME] RTC selected but no valid time on module\n");
        } else {
            logPrintf("[TIME] Switched to RTC time source\n");
        }
    }
    sendOK();
}

// -------------------------------------------------------
// /api/status
// -------------------------------------------------------
static void handleStatus() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(512);
    doc["mode"]   = "AP";
    doc["ip"]     = WiFi.softAPIP().toString();
    doc["ssid"]   = AP_SSID;
    doc["heap"]   = (int)ESP.getFreeHeap();
    doc["uptime"] = (uint32_t)(millis() / 1000);

    struct tm ti;
    if (getLocalTime(&ti) && ti.tm_year >= 124) {
        char tbuf[9];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d",
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
        doc["time"] = tbuf;
        char dbuf[11];
        snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
        doc["date"] = dbuf;
    }
    doc["time_source"] = (timeSrc == TimeSrc::MANUAL) ? "manual" : "rtc";
    doc["active_day"]  = sched_getActiveDay();
    doc["day_count"]   = DAY_COUNT;
    doc["lora_ready"]  = lora_isReady();
    doc["clients"]     = lora_clientCount();

    String s;
    serializeJson(doc, s);
    sendJSON(200, s);
}

// -------------------------------------------------------
// /api/auth/*
// -------------------------------------------------------
static void handleAuthStatus() {
    // No auth check — needed to show lock state in UI before login
    DynamicJsonDocument doc(64);
    doc["enabled"] = authEnabled;
    String s;
    serializeJson(doc, s);
    sendJSON(200, s);
}

static void handleAuthSave() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, server.arg("plain"))) { sendErr("bad json"); return; }
    String pwd = doc["password"] | "";
    if (pwd.length() < 4) { sendErr("password too short (min 4)"); return; }
    authPassword = pwd;
    authEnabled  = true;
    saveAuth();
    sendOK();
    logPrintf("[AUTH] Password updated, auth enabled\n");
}

static void handleAuthDisable() {
    if (!checkAuth()) return;
    authEnabled  = false;
    authPassword = "";
    saveAuth();
    sendOK();
    logPrintf("[AUTH] Auth disabled\n");
}

// -------------------------------------------------------
// /api/days  /api/day  /api/day/activate
// -------------------------------------------------------
static void handleDaysStatus() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(64);
    doc["active"] = sched_getActiveDay();
    doc["count"]  = DAY_COUNT;
    doc["ended"]  = sched_courseEnded();
    String s; serializeJson(doc, s);
    sendJSON(200, s);
}

static void handleDayGet() {
    if (!checkAuth()) return;
    int n = server.arg("n").toInt();
    if (n < 0 || n >= DAY_COUNT) { sendErr("invalid day"); return; }
    sendJSON(200, sched_dayJSON((uint8_t)n));
}

static void handleDayActivate() {
    if (!checkAuth()) return;
    int n = server.arg("n").toInt();
    if (n < 0 || n >= DAY_COUNT) { sendErr("invalid day"); return; }
    if (sched_activateDay((uint8_t)n)) sendOK();
    else sendErr("day file not found on SPIFFS");
}

static void handleTracksGet() {
    if (!checkAuth()) return;
    sendJSON(200, mp3_listTracksJSON());
}

// -------------------------------------------------------
// /api/day/entry — add/edit/delete a single entry in a specific day's
// template (?day=N), without activating it. If N is the active day, these
// transparently behave like /api/schedule (see sched_addToDay & co).
// -------------------------------------------------------
static void handleDayEntryPOST() {
    if (!checkAuth()) return;
    int day = server.arg("day").toInt();
    if (day < 0 || day >= DAY_COUNT) { sendErr("invalid day"); return; }
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, server.arg("plain"))) { sendErr("bad json"); return; }
    uint8_t h     = doc["hour"]  | 0;
    uint8_t m     = doc["min"]   | 0;
    uint8_t track = doc["track"] | 1;
    uint8_t loop  = doc["loop"]  | 1;
    String  desc  = doc["desc"]  | "";
    if (sched_addToDay((uint8_t)day, h, m, desc, track, loop)) sendOK();
    else sendErr("failed (full, bad time/track, or that time slot is already used)");
}

static void handleDayEntryPUT() {
    if (!checkAuth()) return;
    int      day = server.arg("day").toInt();
    uint32_t id  = server.arg("id").toInt();
    if (day < 0 || day >= DAY_COUNT) { sendErr("invalid day"); return; }
    if (!id) { sendErr("missing id"); return; }
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, server.arg("plain"))) { sendErr("bad json"); return; }
    uint8_t h     = doc["hour"]  | 0;
    uint8_t m     = doc["min"]   | 0;
    uint8_t track = doc["track"] | 1;
    uint8_t loop  = doc["loop"]  | 1;
    bool    en    = doc["en"]    | true;
    String  desc  = doc["desc"]  | "";
    if (sched_editInDay((uint8_t)day, id, h, m, desc, track, loop, en)) sendOK();
    else sendErr("not found, bad time/track, or that time slot is already used");
}

static void handleDayEntryDELETE() {
    if (!checkAuth()) return;
    int      day = server.arg("day").toInt();
    uint32_t id  = server.arg("id").toInt();
    if (day < 0 || day >= DAY_COUNT) { sendErr("invalid day"); return; }
    if (!id) { sendErr("missing id"); return; }
    if (sched_delFromDay((uint8_t)day, id)) sendOK();
    else sendErr("not found");
}

// -------------------------------------------------------
// /api/play*  /api/stop  /api/sync  /api/clients — manual + LoRa control
// -------------------------------------------------------
static bool parsePlayArgs(uint8_t& track, uint8_t& vol, uint8_t& loop) {
    DynamicJsonDocument doc(128);
    if (deserializeJson(doc, server.arg("plain"))) return false;
    track = doc["track"] | DEFAULT_TRACK;
    vol   = doc["vol"]   | DEFAULT_VOLUME;
    loop  = doc["loop"]  | 1;
    return true;
}

static void handlePlayLocal() {
    if (!checkAuth()) return;
    uint8_t track, vol, loop;
    if (!parsePlayArgs(track, vol, loop)) { sendErr("bad json"); return; }
    mp3_setVolume(vol);
    mp3_play(track, loop);
    sendOK();
}

static void handlePlayLoRa() {
    if (!checkAuth()) return;
    uint8_t track, vol, loop;
    if (!parsePlayArgs(track, vol, loop)) { sendErr("bad json"); return; }
    lora_sendGong(track, vol, loop);
    sendOK();
}

static void handlePlayAll() {
    if (!checkAuth()) return;
    uint8_t track, vol, loop;
    if (!parsePlayArgs(track, vol, loop)) { sendErr("bad json"); return; }
    lora_sendGong(track, vol, loop);   // TX first, then local — stays in sync
    mp3_setVolume(vol);
    mp3_play(track, loop);
    sendOK();
}

static void handleStop() {
    if (!checkAuth()) return;
    mp3_stop();
    lora_sendStop();
    sendOK();
}

static void handleSync() {
    if (!checkAuth()) return;
    // Schedule sync over LoRa was never functional: ~3.5 KB of JSON cannot
    // fit a 255-byte frame and clients discard MSG_SCHEDULE anyway. The
    // schedule lives on the server; clients only need GONG/STOP. Answer
    // honestly instead of returning a fake "ok".
    sendErr("schedule sync over LoRa is not supported (clients don't store schedules)");
}

static void handleClients() {
    if (!checkAuth()) return;
    sendJSON(200, lora_clientsJSON());
}

static void handleFavicon() {
    server.send(204);
}

// -------------------------------------------------------
// /api/logs — live debug log from ring buffer
// -------------------------------------------------------
static void handleLogs() {
    if (!checkAuth()) return;
    int n = server.arg("n").toInt();
    if (n < 1 || n > 64) n = 40;
    sendJSON(200, logbuffer_toJSON(n));
}

static void handleNotFound() {
    server.send(404, "text/plain", "Not found");
}

// -------------------------------------------------------
// WiFi — standalone AP only, never joins another network
// -------------------------------------------------------
static void wifi_startAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    logPrintf("[WIFI] AP '%s' started — IP: %s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());
    MDNS.begin(MDNS_NAME);
    logPrintf("[MDNS] http://%s.local\n", MDNS_NAME);
}

// -------------------------------------------------------
// Public setup / loop
// -------------------------------------------------------
void web_setup() {
    wifi_startAP();

    loadAuth();

    // OPTIONS preflight for CORS
    server.on("/api/schedule",    HTTP_OPTIONS, handleOptions);
    server.on("/api/time",        HTTP_OPTIONS, handleOptions);
    server.on("/api/time/source", HTTP_OPTIONS, handleOptions);
    server.on("/api/auth/save",   HTTP_OPTIONS, handleOptions);
    server.on("/api/auth/disable",HTTP_OPTIONS, handleOptions);
    server.on("/api/days",        HTTP_OPTIONS, handleOptions);
    server.on("/api/day",         HTTP_OPTIONS, handleOptions);
    server.on("/api/day/activate",HTTP_OPTIONS, handleOptions);
    server.on("/api/day/entry",   HTTP_OPTIONS, handleOptions);
    server.on("/api/logs",        HTTP_OPTIONS, handleOptions);
    server.on("/api/status",      HTTP_OPTIONS, handleOptions);
    server.on("/api/tracks",      HTTP_OPTIONS, handleOptions);
    server.on("/api/play",        HTTP_OPTIONS, handleOptions);
    server.on("/api/play/lora",   HTTP_OPTIONS, handleOptions);
    server.on("/api/play/all",    HTTP_OPTIONS, handleOptions);
    server.on("/api/stop",        HTTP_OPTIONS, handleOptions);
    server.on("/api/sync",        HTTP_OPTIONS, handleOptions);
    server.on("/api/clients",     HTTP_OPTIONS, handleOptions);

    // Routes
    server.on("/favicon.ico",     HTTP_GET,    handleFavicon);
    server.on("/",                HTTP_GET,    handleRoot);
    server.on("/api/schedule",    HTTP_GET,    handleScheduleGET);
    server.on("/api/schedule",    HTTP_POST,   handleSchedulePOST);
    server.on("/api/schedule",    HTTP_PUT,    handleSchedulePUT);    // ?id=
    server.on("/api/schedule",    HTTP_DELETE, handleScheduleDELETE); // ?id=

    server.on("/api/time",        HTTP_POST,   handleTimeSet);
    server.on("/api/time/source", HTTP_POST,   handleTimeSource);
    server.on("/api/logs",        HTTP_GET,    handleLogs);
    server.on("/api/status",      HTTP_GET,    handleStatus);

    server.on("/api/auth/status", HTTP_GET,    handleAuthStatus);
    server.on("/api/auth/save",   HTTP_POST,   handleAuthSave);
    server.on("/api/auth/disable",HTTP_POST,   handleAuthDisable);

    server.on("/api/days",           HTTP_GET,  handleDaysStatus);
    server.on("/api/day",            HTTP_GET,  handleDayGet);
    server.on("/api/day/activate",   HTTP_POST, handleDayActivate);
    server.on("/api/day/entry",      HTTP_POST,   handleDayEntryPOST);
    server.on("/api/day/entry",      HTTP_PUT,    handleDayEntryPUT);
    server.on("/api/day/entry",      HTTP_DELETE, handleDayEntryDELETE);
    server.on("/api/tracks",         HTTP_GET,  handleTracksGet);

    server.on("/api/play",        HTTP_POST, handlePlayLocal);
    server.on("/api/play/lora",   HTTP_POST, handlePlayLoRa);
    server.on("/api/play/all",    HTTP_POST, handlePlayAll);
    server.on("/api/stop",        HTTP_POST, handleStop);
    server.on("/api/sync",        HTTP_POST, handleSync);
    server.on("/api/clients",     HTTP_GET,  handleClients);

    server.onNotFound(handleNotFound);
    server.begin();
    logPrintf("[WEB] HTTP server listening on port 80\n");
}

void web_loop() {
    server.handleClient();
}
