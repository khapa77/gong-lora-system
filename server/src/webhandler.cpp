#include "webhandler.h"
#include "config.h"
#include "schedule.h"
#include "lorahandler.h"
#include "mp3handler.h"
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ArduinoJson.h>

bool       apMode = false;
static WebServer server(80);
static WiFiUDP ntpUDP;
static NTPClient ntp(ntpUDP, NTP_SERVER, NTP_UTC_OFFSET);

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
    sendJSON(200, sched_toJSON());
}

static void handleSchedulePOST() {
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, server.arg("plain"))) { sendErr("bad json"); return; }
    uint8_t h     = doc["hour"]  | 0;
    uint8_t m     = doc["min"]   | 0;
    uint8_t track = doc["track"] | 1;
    String  desc  = doc["desc"]  | "";
    if (sched_add(h, m, desc, track)) sendOK();
    else sendErr("failed (full or bad time)");
}

static void handleSchedulePUT() {
    uint32_t id = server.arg("id").toInt();
    if (!id) { sendErr("missing id"); return; }
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, server.arg("plain"))) { sendErr("bad json"); return; }
    uint8_t h     = doc["hour"]  | 0;
    uint8_t m     = doc["min"]   | 0;
    uint8_t track = doc["track"] | 1;
    bool    en    = doc["en"]    | true;
    String  desc  = doc["desc"]  | "";
    if (sched_edit(id, h, m, desc, track, en)) sendOK();
    else sendErr("not found");
}

static void handleScheduleDELETE() {
    uint32_t id = server.arg("id").toInt();
    if (!id) { sendErr("missing id"); return; }
    if (sched_del(id)) sendOK();
    else sendErr("not found");
}

// -------------------------------------------------------
// /api/play  /api/play/lora  /api/play/all
// -------------------------------------------------------
static void handlePlayLocal() {
    DynamicJsonDocument doc(128);
    deserializeJson(doc, server.arg("plain"));
    uint8_t track = doc["track"] | DEFAULT_TRACK;
    uint8_t vol   = doc["vol"]   | DEFAULT_VOLUME;
    mp3_setVolume(vol);
    mp3_play(track);
    sendOK();
}

static void handlePlayLoRa() {
    DynamicJsonDocument doc(128);
    deserializeJson(doc, server.arg("plain"));
    uint8_t track = doc["track"] | DEFAULT_TRACK;
    uint8_t vol   = doc["vol"]   | DEFAULT_VOLUME;
    lora_sendGong(track, vol);
    sendOK();
}

static void handlePlayAll() {
    DynamicJsonDocument doc(128);
    deserializeJson(doc, server.arg("plain"));
    uint8_t track = doc["track"] | DEFAULT_TRACK;
    uint8_t vol   = doc["vol"]   | DEFAULT_VOLUME;
    mp3_setVolume(vol);
    mp3_play(track);
    lora_sendGong(track, vol);
    sendOK();
}

// -------------------------------------------------------
// /api/sync  — push schedule to all LoRa clients
// -------------------------------------------------------
static void handleSync() {
    lora_sendSchedule(sched_toJSON());
    sendOK();
}

// -------------------------------------------------------
// /api/clients  /api/status
// -------------------------------------------------------
static void handleClients() {
    sendJSON(200, lora_clientsJSON());
}

static void handleStatus() {
    DynamicJsonDocument doc(256);
    doc["mode"]    = apMode ? "AP" : "STA";
    doc["ip"]      = apMode ? WiFi.softAPIP().toString()
                            : WiFi.localIP().toString();
    doc["ssid"]    = apMode ? AP_SSID : WiFi.SSID();
    doc["clients"] = lora_clientCount();
    doc["heap"]    = (int)ESP.getFreeHeap();
    doc["uptime"]  = (uint32_t)(millis() / 1000);
    if (WiFi.status() == WL_CONNECTED) {
        ntp.update();
        doc["ntp_time"] = ntp.getFormattedTime();
    }
    String s;
    serializeJson(doc, s);
    sendJSON(200, s);
}

// -------------------------------------------------------
// /api/wifi/*
// -------------------------------------------------------
static void handleWiFiStatus() {
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
    delay(500);
    ESP.restart();
}

static void handleWiFiReset() {
    SPIFFS.remove(WIFI_CONFIG_FILE);
    sendOK();
    delay(500);
    ESP.restart();
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

    // OPTIONS preflight for CORS
    server.on("/api/schedule",    HTTP_OPTIONS, handleOptions);
    server.on("/api/play",        HTTP_OPTIONS, handleOptions);
    server.on("/api/play/lora",   HTTP_OPTIONS, handleOptions);
    server.on("/api/play/all",    HTTP_OPTIONS, handleOptions);
    server.on("/api/sync",        HTTP_OPTIONS, handleOptions);

    // Routes
    server.on("/",                HTTP_GET,    handleRoot);
    server.on("/api/schedule",    HTTP_GET,    handleScheduleGET);
    server.on("/api/schedule",    HTTP_POST,   handleSchedulePOST);
    server.on("/api/schedule",    HTTP_PUT,    handleSchedulePUT);    // ?id=
    server.on("/api/schedule",    HTTP_DELETE, handleScheduleDELETE); // ?id=

    server.on("/api/play",        HTTP_POST,   handlePlayLocal);
    server.on("/api/play/lora",   HTTP_POST,   handlePlayLoRa);
    server.on("/api/play/all",    HTTP_POST,   handlePlayAll);

    server.on("/api/sync",        HTTP_POST,   handleSync);
    server.on("/api/clients",     HTTP_GET,    handleClients);
    server.on("/api/status",      HTTP_GET,    handleStatus);
    server.on("/api/wifi/status", HTTP_GET,    handleWiFiStatus);
    server.on("/api/wifi/save",   HTTP_POST,   handleWiFiSave);
    server.on("/api/wifi/reset",  HTTP_POST,   handleWiFiReset);

    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("[WEB] HTTP server listening on port 80");
}

void web_loop() {
    server.handleClient();
}

