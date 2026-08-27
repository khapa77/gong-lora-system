#include "webhandler.h"
#include "config.h"
#include "schedule.h"
#include "mp3handler.h"
#include "rtchandler.h"
#include "lorahandler.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/base64.h"
#include <esp_system.h>

static WebServer server(80);
// Time source is chosen explicitly by the user (RTC / Manual tab) — no automatic priority.
enum class TimeSrc { RTC, MANUAL };
static TimeSrc timeSrc = TimeSrc::RTC;

// -------------------------------------------------------
// Auth (H-10) — PBKDF2-SHA256, salted, constant-time compare.
// /auth.conf: {"enabled":true,"salt":"<32 hex>","hash":"<64 hex>","iter":20000}
// A dump of the flash chip used to hand over the admin password in plain
// text; it now only hands over a salted, 20000-round hash, same cost as any
// other PBKDF2-protected login.
// -------------------------------------------------------
static bool     authEnabled  = false;
static String   authSaltHex  = "";
static String   authHashHex  = "";
static uint32_t authIter     = 20000;

static void bytesToHex(const uint8_t* b, size_t n, String& out) {
    out = "";
    char h[3];
    for (size_t i = 0; i < n; i++) { snprintf(h, sizeof(h), "%02x", b[i]); out += h; }
}

static bool hexToBytes(const String& hex, uint8_t* out, size_t outLen) {
    if (hex.length() != outLen * 2) return false;
    for (size_t i = 0; i < outLen; i++) {
        char byteStr[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        out[i] = (uint8_t)strtoul(byteStr, nullptr, 16);
    }
    return true;
}

static bool pbkdf2(const String& pwd, const uint8_t* salt, size_t saltLen, uint32_t iter, uint8_t out[32]) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) != 0) {
        mbedtls_md_free(&ctx);
        return false;
    }
    int rc = mbedtls_pkcs5_pbkdf2_hmac(&ctx, (const uint8_t*)pwd.c_str(), pwd.length(),
                                        salt, saltLen, iter, 32, out);
    mbedtls_md_free(&ctx);
    return rc == 0;
}

static void setPassword(const String& pwd) {
    uint8_t salt[16];
    esp_fill_random(salt, sizeof(salt));
    uint8_t hash[32];
    pbkdf2(pwd, salt, sizeof(salt), authIter, hash);
    bytesToHex(salt, sizeof(salt), authSaltHex);
    bytesToHex(hash, sizeof(hash), authHashHex);
}

static void saveAuth() {
    File f = LittleFS.open(AUTH_CONFIG_FILE, "w");
    if (!f) return;
    DynamicJsonDocument doc(384);
    doc["enabled"] = authEnabled;
    doc["salt"]    = authSaltHex;
    doc["hash"]    = authHashHex;
    doc["iter"]    = authIter;
    serializeJson(doc, f);
    f.close();
}

static void loadAuth() {
    if (!LittleFS.exists(AUTH_CONFIG_FILE)) return;
    File f = LittleFS.open(AUTH_CONFIG_FILE, "r");
    if (!f) return;
    DynamicJsonDocument doc(384);
    bool ok = !deserializeJson(doc, f);
    f.close();
    if (!ok) return;

    authEnabled = doc["enabled"] | false;
    if (doc.containsKey("hash")) {
        authSaltHex = String((const char*)(doc["salt"] | ""));
        authHashHex = String((const char*)(doc["hash"] | ""));
        authIter    = doc["iter"] | 20000;
    } else if (doc.containsKey("password")) {
        // One-time migration from the pre-H-10 plaintext format — an
        // existing deployment's password keeps working, just gets upgraded
        // to a hash on the next boot instead of needing to be re-entered.
        String legacy = doc["password"] | "";
        setPassword(legacy);
        saveAuth();
        logPrintf("[AUTH] Migrated legacy plaintext password to PBKDF2\n");
    }
    logPrintf("[AUTH] %s\n", authEnabled ? "enabled" : "disabled");
}

static bool constTimeEqual(const String& a, const String& b) {
    if (a.length() != b.length()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.length(); i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static bool checkAuth() {
    if (!authEnabled || authHashHex.length() == 0) return true;

    String authHeader = server.header("Authorization");
    if (authHeader.startsWith("Basic ")) {
        String b64 = authHeader.substring(6);
        uint8_t decoded[128];
        size_t  outLen = 0;
        if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &outLen,
                                   (const uint8_t*)b64.c_str(), b64.length()) == 0) {
            decoded[outLen] = 0;
            String userPass((const char*)decoded);
            int colon = userPass.indexOf(':');
            if (colon >= 0 && userPass.substring(0, colon) == "admin") {
                String pass = userPass.substring(colon + 1);
                uint8_t salt[16];
                if (hexToBytes(authSaltHex, salt, sizeof(salt))) {
                    uint8_t hash[32];
                    if (pbkdf2(pass, salt, sizeof(salt), authIter, hash)) {
                        String hashHex;
                        bytesToHex(hash, sizeof(hash), hashHex);
                        if (constTimeEqual(hashHex, authHashHex)) return true;
                    }
                }
            }
        }
    }

    server.sendHeader("WWW-Authenticate", String("Basic realm=\"") + AUTH_REALM + "\"");
    server.send(401, "text/plain", "Unauthorized");
    return false;
}

// H-9: this device's whole API is same-origin (the web UI is served BY this
// same ESP32) — CORS was never needed, and a wildcard
// `Access-Control-Allow-Origin: *` on unauthenticated-by-default admin
// endpoints meant any page open on a device connected to the AP could
// silently POST /api/play/all. Cheap CSRF guard instead: state-changing
// requests must carry a custom header, which a plain cross-origin form/fetch
// (without a CORS preflight, since we no longer answer one) cannot set.
static bool checkOrigin() {
    if (server.header("X-Gong-Request") != "1") {
        server.send(403, "text/plain", "Forbidden");
        return false;
    }
    return true;
}

// -------------------------------------------------------
// Helpers
// -------------------------------------------------------
static void sendJSON(int code, const String& body) {
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
// Static files — M-20: serve the pre-gzipped UI when present (41KB -> ~9KB,
// matters a lot over the AP's own link). /index.html.gz is built via
// `gzip -9` alongside data/index.html (see README "Обновление веб-интерфейса").
// -------------------------------------------------------
static void handleRoot() {
    if (!checkAuth()) return;
    if (LittleFS.exists("/index.html.gz")) {
        File f = LittleFS.open("/index.html.gz", "r");
        server.sendHeader("Content-Encoding", "gzip");
        server.streamFile(f, "text/html");
        f.close();
    } else if (LittleFS.exists("/index.html")) {
        File f = LittleFS.open("/index.html", "r");
        server.streamFile(f, "text/html");
        f.close();
    } else {
        server.send(200, "text/html",
            "<h1>Gong Server</h1>"
            "<p>Upload filesystem data to get the full web interface.</p>");
    }
}

// -------------------------------------------------------
// /api/schedule — GET only. POST/PUT/DELETE here were dead code: the UI has
// always used the day-scoped /api/day/entry (see handleDayEntry* below).
// -------------------------------------------------------
static void handleScheduleGET() {
    if (!checkAuth()) return;
    String json = sched_toJSON();
    if (json.length() == 0) { sendErr("schedule JSON overflow — see logs"); return; }
    sendJSON(200, json);
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
    if (!checkAuth() || !checkOrigin()) return;
    DynamicJsonDocument doc(128);
    if (deserializeJson(doc, server.arg("plain"))) { sendErr("bad json"); return; }
    int h = doc["hour"] | -1;
    int m = doc["min"]  | -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) { sendErr("invalid time"); return; }

    struct tm cur;
    bool haveCur = localNow(cur) && cur.tm_year >= 124;
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
    if (!checkAuth() || !checkOrigin()) return;
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
// /api/status  — see also /api/state (M-10), which bundles this with
// schedule/days/clients/auth into one response for the UI's poll loop.
// -------------------------------------------------------
static String statusJSON() {
    DynamicJsonDocument doc(512);
    doc["mode"]   = "AP";
    doc["ip"]     = WiFi.softAPIP().toString();
    doc["ssid"]   = AP_SSID;
    doc["heap"]   = (int)ESP.getFreeHeap();
    doc["uptime"] = (uint32_t)(millis() / 1000);
    doc["fw"]     = FW_VERSION;   // Low: lets an operator confirm every device runs the same build

    struct tm ti;
    if (localNow(ti) && ti.tm_year >= 124) {
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
    doc["default_key"] = lora_usesDefaultKey();

    String s;
    serializeJson(doc, s);
    return s;
}

static void handleStatus() {
    if (!checkAuth()) return;
    sendJSON(200, statusJSON());
}

// -------------------------------------------------------
// /api/auth/*
// -------------------------------------------------------
static String authJSON() {
    DynamicJsonDocument doc(64);
    doc["enabled"] = authEnabled;
    String s;
    serializeJson(doc, s);
    return s;
}

static void handleAuthStatus() {
    // No auth check — needed to show lock state in UI before login
    sendJSON(200, authJSON());
}

static void handleAuthSave() {
    if (!checkAuth() || !checkOrigin()) return;
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, server.arg("plain"))) { sendErr("bad json"); return; }
    String pwd = doc["password"] | "";
    if (pwd.length() < 4) { sendErr("password too short (min 4)"); return; }
    setPassword(pwd);
    authEnabled = true;
    saveAuth();
    sendOK();
    logPrintf("[AUTH] Password updated, auth enabled\n");
}

static void handleAuthDisable() {
    if (!checkAuth() || !checkOrigin()) return;
    authEnabled = false;
    authSaltHex = "";
    authHashHex = "";
    saveAuth();
    sendOK();
    logPrintf("[AUTH] Auth disabled\n");
}

// -------------------------------------------------------
// /api/days  /api/day  /api/day/activate
// -------------------------------------------------------
static String daysJSON() {
    DynamicJsonDocument doc(64);
    doc["active"] = sched_getActiveDay();
    doc["count"]  = DAY_COUNT;
    doc["ended"]  = sched_courseEnded();
    String s; serializeJson(doc, s);
    return s;
}

static void handleDaysStatus() {
    if (!checkAuth()) return;
    sendJSON(200, daysJSON());
}

static void handleDayGet() {
    if (!checkAuth()) return;
    int n = server.arg("n").toInt();
    if (n < 0 || n >= DAY_COUNT) { sendErr("invalid day"); return; }
    sendJSON(200, sched_dayJSON((uint8_t)n));
}

static void handleDayActivate() {
    if (!checkAuth() || !checkOrigin()) return;
    int n = server.arg("n").toInt();
    if (n < 0 || n >= DAY_COUNT) { sendErr("invalid day"); return; }
    if (sched_activateDay((uint8_t)n)) sendOK();
    else sendErr("day file not found on filesystem");
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
    if (!checkAuth() || !checkOrigin()) return;
    int day = server.arg("day").toInt();
    if (day < 0 || day >= DAY_COUNT) { sendErr("invalid day"); return; }
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, server.arg("plain"))) { sendErr("bad json"); return; }
    uint8_t h     = doc["hour"]  | 0;
    uint8_t m     = doc["min"]   | 0;
    uint8_t track = doc["track"] | 1;
    uint8_t loop  = doc["loop"]  | 1;
    uint8_t vol   = doc["vol"]   | DEFAULT_VOLUME;
    String  desc  = doc["desc"]  | "";
    if (sched_addToDay((uint8_t)day, h, m, desc, track, loop, vol)) sendOK();
    else sendErr("failed (full, bad time/track, or that time slot is already used)");
}

static void handleDayEntryPUT() {
    if (!checkAuth() || !checkOrigin()) return;
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
    uint8_t vol   = doc["vol"]   | DEFAULT_VOLUME;
    bool    en    = doc["en"]    | true;
    String  desc  = doc["desc"]  | "";
    if (sched_editInDay((uint8_t)day, id, h, m, desc, track, loop, en, vol)) sendOK();
    else sendErr("not found, bad time/track, or that time slot is already used");
}

static void handleDayEntryDELETE() {
    if (!checkAuth() || !checkOrigin()) return;
    int      day = server.arg("day").toInt();
    uint32_t id  = server.arg("id").toInt();
    if (day < 0 || day >= DAY_COUNT) { sendErr("invalid day"); return; }
    if (!id) { sendErr("missing id"); return; }
    if (sched_delFromDay((uint8_t)day, id)) sendOK();
    else sendErr("not found");
}

// -------------------------------------------------------
// /api/play*  /api/stop  /api/clients — manual + LoRa control
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
    if (!checkAuth() || !checkOrigin()) return;
    uint8_t track, vol, loop;
    if (!parsePlayArgs(track, vol, loop)) { sendErr("bad json"); return; }
    mp3_setVolume(vol);
    mp3_play(track, loop);
    sendOK();
}

static void handlePlayLoRa() {
    if (!checkAuth() || !checkOrigin()) return;
    uint8_t track, vol, loop;
    if (!parsePlayArgs(track, vol, loop)) { sendErr("bad json"); return; }
    lora_sendGong(track, vol, loop, /*playLocal=*/false);
    sendOK();
}

static void handlePlayAll() {
    if (!checkAuth() || !checkOrigin()) return;
    uint8_t track, vol, loop;
    if (!parsePlayArgs(track, vol, loop)) { sendErr("bad json"); return; }
    // H-1: non-blocking — local playback starts from loop()'s
    // lora_pollLocalPlay() once the broadcast's TX actually completes.
    lora_sendGong(track, vol, loop, /*playLocal=*/true);
    sendOK();
}

static void handleStop() {
    if (!checkAuth() || !checkOrigin()) return;
    mp3_stop();
    lora_sendStop();
    sendOK();
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

// -------------------------------------------------------
// M-10: one bundled response for the UI's periodic poll — see index.html's
// refreshAll(). Five separate requests against this device's single-threaded
// WebServer (handleClient() serves one client at a time) is what turned
// C-1's 5s getLocalTime() stall into a multi-second UI freeze on every tick;
// even now that C-1 is fixed, five round trips is still four more than
// necessary.
// -------------------------------------------------------
static void handleState() {
    if (!checkAuth()) return;
    String schedule = sched_toJSON();
    if (schedule.length() == 0) schedule = "[]";   // overflow marker — never break /api/state
    String s = "{";
    s += "\"status\":";   s += statusJSON();
    s += ",\"schedule\":"; s += schedule;
    s += ",\"days\":";     s += daysJSON();
    s += ",\"clients\":";  s += lora_clientsJSON();
    s += ",\"auth\":";     s += authJSON();
    s += "}";
    sendJSON(200, s);
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
    // M11: without an advertised service, some resolvers (notably Windows
    // without Bonjour, and some Android NSD-based clients) never resolve the
    // plain hostname — only the fact that the device offers an "http"
    // service actually gets it into their mDNS cache.
    MDNS.addService("http", "tcp", 80);
    logPrintf("[MDNS] http://%s.local\n", MDNS_NAME);
}

// -------------------------------------------------------
// Public setup / loop
// -------------------------------------------------------
void web_setup() {
    wifi_startAP();

    loadAuth();

    // Authorization is always header index 0 once collectHeaders() has been
    // called at least once — without this call, server.header("Authorization")
    // (and the old server.authenticate()) always returns empty and auth can
    // never succeed, regardless of password. X-Gong-Request backs checkOrigin().
    const char* hdrs[] = { "X-Gong-Request" };
    server.collectHeaders(hdrs, 1);

    // Routes
    server.on("/favicon.ico",     HTTP_GET,    handleFavicon);
    server.on("/",                HTTP_GET,    handleRoot);
    server.on("/index.html",      HTTP_GET,    handleRoot);   // M-20: was a 404 before
    server.on("/api/schedule",    HTTP_GET,    handleScheduleGET);
    server.on("/api/state",       HTTP_GET,    handleState);

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
    server.on("/api/clients",     HTTP_GET,  handleClients);

    server.onNotFound(handleNotFound);
    server.begin();
    logPrintf("[WEB] HTTP server listening on port 80\n");
}

void web_loop() {
    server.handleClient();
}
