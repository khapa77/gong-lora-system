#include "webhandler.h"
#include "config.h"
#include "schedule.h"
#include "lorahandler.h"
#include "mp3handler.h"
#include "rtchandler.h"
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ArduinoJson.h>
#include <esp_sntp.h>

bool       apMode = false;
static WebServer server(80);
static WiFiUDP ntpUDP;
static NTPClient ntp(ntpUDP, NTP_SERVER, NTP_UTC_OFFSET);
static bool ntpDisabled = false;  // when true: SNTP stopped, manual time active

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
    Serial.printf("[AUTH] %s\n", authEnabled ? "enabled" : "disabled");
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
    server.requestAuthentication(BASIC_AUTH, AUTH_REALM, "Login required");
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
    // 200 + body избегает предупреждения "content length is zero" в WebServer
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
    sendJSON(200, sched_toJSON());
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
    else sendErr("failed (full or bad time)");
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
    else sendErr("not found");
}

static void handleScheduleDELETE() {
    if (!checkAuth()) return;
    uint32_t id = server.arg("id").toInt();
    if (!id) { sendErr("missing id"); return; }
    if (sched_del(id)) sendOK();
    else sendErr("not found");
}

// -------------------------------------------------------
// /api/play  /api/play/lora  /api/play/all
// -------------------------------------------------------
static void handlePlayLocal() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(128);
    deserializeJson(doc, server.arg("plain"));
    uint8_t track = doc["track"] | DEFAULT_TRACK;
    uint8_t vol   = doc["vol"]   | DEFAULT_VOLUME;
    mp3_setVolume(vol);
    mp3_play(track);
    sendOK();
}

static void handlePlayLoRa() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(128);
    deserializeJson(doc, server.arg("plain"));
    uint8_t track = doc["track"] | DEFAULT_TRACK;
    uint8_t vol   = doc["vol"]   | DEFAULT_VOLUME;
    lora_sendGong(track, vol);
    sendOK();
}

static void handlePlayAll() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(128);
    deserializeJson(doc, server.arg("plain"));
    uint8_t track = doc["track"] | DEFAULT_TRACK;
    uint8_t vol   = doc["vol"]   | DEFAULT_VOLUME;
    // LoRa TX first (blocking), then local — both start audio in sync
    lora_sendGong(track, vol);
    mp3_setVolume(vol);
    mp3_play(track);
    sendOK();
}

// -------------------------------------------------------
// /api/time  — set system time manually (for AP / no-NTP mode)
// -------------------------------------------------------
static void handleTimeSet() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(128);
    if (deserializeJson(doc, server.arg("plain"))) { sendErr("bad json"); return; }
    int h = doc["hour"] | -1;
    int m = doc["min"]  | -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) { sendErr("invalid time"); return; }

    // Set ESP32 system clock; use 2024-01-01 as date (only HH:MM matters for schedule)
    struct tm ti = {};
    ti.tm_year = 124;   // 2024
    ti.tm_mon  = 0;
    ti.tm_mday = 1;
    ti.tm_hour = h;
    ti.tm_min  = m;
    ti.tm_sec  = 0;
    time_t t = mktime(&ti);
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    rtc_syncFromSystem();   // persist to DS3231 so it survives next reboot

    sendOK();
    Serial.printf("[TIME] Manual time set: %02d:%02d\n", h, m);
}

// POST /api/time/source?s=ntp|manual
static void handleTimeSource() {
    if (!checkAuth()) return;
    String src = server.arg("s");
    if (src == "manual") {
        ntpDisabled = true;
        esp_sntp_stop();
        Serial.println("[TIME] NTP disabled, manual mode active");
    } else {
        ntpDisabled = false;
        configTime(NTP_UTC_OFFSET, 0, NTP_SERVER);
        ntp.begin();
        ntp.forceUpdate();
        Serial.println("[TIME] NTP re-enabled");
    }
    sendOK();
}

// -------------------------------------------------------
// /api/stop  — stop playback locally and on all LoRa clients
// -------------------------------------------------------
static void handleStop() {
    if (!checkAuth()) return;
    mp3_stop();
    lora_sendStop();
    sendOK();
}

// -------------------------------------------------------
// /api/sync  — push schedule to all LoRa clients
// -------------------------------------------------------
static void handleSync() {
    if (!checkAuth()) return;
    lora_sendSchedule(sched_toJSON());
    sendOK();
}

// -------------------------------------------------------
// /api/clients  /api/status
// -------------------------------------------------------
static void handleClients() {
    if (!checkAuth()) return;
    sendJSON(200, lora_clientsJSON());
}

static void handleStatus() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(256);
    doc["mode"]    = apMode ? "AP" : "STA";
    doc["ip"]      = apMode ? WiFi.softAPIP().toString()
                            : WiFi.localIP().toString();
    doc["ssid"]    = apMode ? AP_SSID : WiFi.SSID();
    doc["clients"] = lora_clientCount();
    doc["heap"]    = (int)ESP.getFreeHeap();
    doc["uptime"]  = (uint32_t)(millis() / 1000);
    if (WiFi.status() == WL_CONNECTED && !ntpDisabled) {
        ntp.update();
        doc["ntp_time"]    = ntp.getFormattedTime();
        doc["time_source"] = "ntp";
    } else {
        struct tm ti;
        if (getLocalTime(&ti) && ti.tm_year >= 124) {
            char tbuf[9];
            snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d",
                     ti.tm_hour, ti.tm_min, ti.tm_sec);
            doc["ntp_time"]    = tbuf;
            // RTC takes priority over manual if module is present and has valid time
            doc["time_source"] = rtc_hasValidTime() ? "rtc" : "manual";
        } else {
            doc["time_source"] = "none";
        }
    }
    String s;
    serializeJson(doc, s);
    sendJSON(200, s);
}

// -------------------------------------------------------
// /api/wifi/*
// -------------------------------------------------------
static void handleWiFiStatus() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(256);
    doc["ap_mode"]   = apMode;
    doc["connected"] = (WiFi.status() == WL_CONNECTED);
    doc["ssid"]      = apMode ? AP_SSID : WiFi.SSID();
    doc["ip"]        = apMode ? WiFi.softAPIP().toString()
                              : WiFi.localIP().toString();
    String s;
    serializeJson(doc, s);
    sendJSON(200, s);
}

static void handleWiFiSave() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, server.arg("plain"))) { sendErr("bad json"); return; }
    String ssid = doc["ssid"] | "";
    String pass = doc["password"] | "";
    if (ssid.length() == 0) { sendErr("ssid empty"); return; }

    File f = SPIFFS.open(WIFI_CONFIG_FILE, "w");
    if (!f) { sendErr("fs error"); return; }
    DynamicJsonDocument cfg(256);
    cfg["ssid"]     = ssid;
    cfg["password"] = pass;
    serializeJson(cfg, f);
    f.close();

    sendOK();
    { unsigned long t0 = millis(); while (millis() - t0 < 500) server.handleClient(); }
    ESP.restart();
}

static void handleWiFiReset() {
    if (!checkAuth()) return;
    SPIFFS.remove(WIFI_CONFIG_FILE);
    sendOK();
    { unsigned long t0 = millis(); while (millis() - t0 < 500) server.handleClient(); }
    ESP.restart();
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
    Serial.println("[AUTH] Password updated, auth enabled");
}

static void handleAuthDisable() {
    if (!checkAuth()) return;
    authEnabled  = false;
    authPassword = "";
    saveAuth();
    sendOK();
    Serial.println("[AUTH] Auth disabled");
}

// -------------------------------------------------------
// /api/days  /api/day  /api/day/activate
// -------------------------------------------------------
static void handleDaysStatus() {
    if (!checkAuth()) return;
    DynamicJsonDocument doc(64);
    doc["active"] = sched_getActiveDay();
    doc["count"]  = 12;
    String s; serializeJson(doc, s);
    sendJSON(200, s);
}

static void handleDayGet() {
    if (!checkAuth()) return;
    int n = server.arg("n").toInt();
    if (n < 0 || n > 11) { sendErr("invalid day (0-11)"); return; }
    sendJSON(200, sched_dayJSON((uint8_t)n));
}

static void handleDayActivate() {
    if (!checkAuth()) return;
    int n = server.arg("n").toInt();
    if (n < 0 || n > 11) { sendErr("invalid day (0-11)"); return; }
    if (sched_activateDay((uint8_t)n)) sendOK();
    else sendErr("day file not found on SPIFFS");
}


static void handleNotFound() {
    server.send(404, "text/plain", "Not found");
}

// -------------------------------------------------------
// WiFi connection
// -------------------------------------------------------
bool wifi_connect() {
    if (!SPIFFS.exists(WIFI_CONFIG_FILE)) return false;
    File f = SPIFFS.open(WIFI_CONFIG_FILE, "r");
    if (!f) return false;

    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, f)) { f.close(); return false; }
    f.close();

    String ssid = doc["ssid"] | "";
    String pass = doc["password"] | "";
    if (ssid.length() == 0) return false;

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("[WIFI] Connecting to '%s'", ssid.c_str());

    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        ntp.begin();
        ntp.update();
        configTime(NTP_UTC_OFFSET, 0, NTP_SERVER);
        delay(200);           // let configTime propagate
        rtc_syncFromSystem(); // persist NTP time to DS3231 for next power cycle
        return true;
    }
    Serial.println("\n[WIFI] Connection failed");
    return false;
}

void wifi_startAP() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    apMode = true;
    Serial.printf("[WIFI] AP '%s' started — IP: %s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());
}

// -------------------------------------------------------
// Public setup / loop
// -------------------------------------------------------
void web_setup() {
    if (!wifi_connect()) {
        wifi_startAP();
    }

    loadAuth();

    // OPTIONS preflight for CORS
    server.on("/api/schedule",    HTTP_OPTIONS, handleOptions);
    server.on("/api/play",        HTTP_OPTIONS, handleOptions);
    server.on("/api/play/lora",   HTTP_OPTIONS, handleOptions);
    server.on("/api/play/all",    HTTP_OPTIONS, handleOptions);
    server.on("/api/time",        HTTP_OPTIONS, handleOptions);
    server.on("/api/time/source", HTTP_OPTIONS, handleOptions);
    server.on("/api/stop",        HTTP_OPTIONS, handleOptions);
    server.on("/api/sync",        HTTP_OPTIONS, handleOptions);
    server.on("/api/wifi/save",   HTTP_OPTIONS, handleOptions);
    server.on("/api/wifi/reset",  HTTP_OPTIONS, handleOptions);
    server.on("/api/auth/save",   HTTP_OPTIONS, handleOptions);
    server.on("/api/auth/disable",HTTP_OPTIONS, handleOptions);

    // Routes
    server.on("/",                HTTP_GET,    handleRoot);
    server.on("/api/schedule",    HTTP_GET,    handleScheduleGET);
    server.on("/api/schedule",    HTTP_POST,   handleSchedulePOST);
    server.on("/api/schedule",    HTTP_PUT,    handleSchedulePUT);    // ?id=
    server.on("/api/schedule",    HTTP_DELETE, handleScheduleDELETE); // ?id=

    server.on("/api/play",        HTTP_POST,   handlePlayLocal);
    server.on("/api/play/lora",   HTTP_POST,   handlePlayLoRa);
    server.on("/api/play/all",    HTTP_POST,   handlePlayAll);

    server.on("/api/time",        HTTP_POST,   handleTimeSet);
    server.on("/api/time/source", HTTP_POST,   handleTimeSource);
    server.on("/api/stop",        HTTP_POST,   handleStop);
    server.on("/api/sync",        HTTP_POST,   handleSync);
    server.on("/api/clients",     HTTP_GET,    handleClients);
    server.on("/api/status",      HTTP_GET,    handleStatus);
    server.on("/api/wifi/status", HTTP_GET,    handleWiFiStatus);
    server.on("/api/wifi/save",   HTTP_POST,   handleWiFiSave);
    server.on("/api/wifi/reset",  HTTP_POST,   handleWiFiReset);

    server.on("/api/auth/status", HTTP_GET,    handleAuthStatus);
    server.on("/api/auth/save",   HTTP_POST,   handleAuthSave);
    server.on("/api/auth/disable",HTTP_POST,   handleAuthDisable);

    server.on("/api/days",           HTTP_OPTIONS, handleOptions);
    server.on("/api/day",            HTTP_OPTIONS, handleOptions);
    server.on("/api/day/activate",   HTTP_OPTIONS, handleOptions);
    server.on("/api/days",           HTTP_GET,  handleDaysStatus);
    server.on("/api/day",            HTTP_GET,  handleDayGet);
    server.on("/api/day/activate",   HTTP_POST, handleDayActivate);

    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("[WEB] HTTP server listening on port 80");
}

void web_loop() {
    server.handleClient();
}

