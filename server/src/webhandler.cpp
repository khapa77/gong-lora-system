#include "webhandler.h"
#include "config.h"
#include "sysstate.h"
#include "schedule.h"
#include "mp3handler.h"
#include "timesync.h"
#include "relayhandler.h"
#include "wifihandler.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/base64.h"
#include <esp_system.h>

static WebServer server(80);

// The UI is compiled INTO the firmware (platformio.ini: board_build.embed_txtfiles).
// It used to live on LittleFS, so every UI change needed `uploadfs` — which
// rewrites the whole partition and wiped settings, schedule edits and course
// progress with it. embed_txtfiles appends a NUL, hence the -1.
extern const uint8_t index_html_start[] asm("_binary_web_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_web_index_html_end");
extern const uint8_t info_html_start[]  asm("_binary_web_info_html_start");
extern const uint8_t info_html_end[]    asm("_binary_web_info_html_end");
extern const uint8_t guide_html_start[] asm("_binary_web_guide_html_start");
extern const uint8_t guide_html_end[]   asm("_binary_web_guide_html_end");

// -------------------------------------------------------
// Response plumbing. Handlers run under sys_lock() (they touch the schedule,
// relay, WiFi state that controlTask also uses) but must NOT send while
// holding it: a slow client would then stall the scheduler for as long as
// the TCP write blocks. So handlers only stage the response here, and
// dispatch() sends it after unlocking.
// -------------------------------------------------------
static int         respCode = 0;
static const char* respType = "text/plain";
static String      respBody;
static const uint8_t* respPage    = nullptr;   // встроенная страница (index/info/guide) или nullptr
static const uint8_t* respPageEnd = nullptr;

static void reply(int code, const char* type, const String& body) {
    respCode = code;
    respType = type;
    respBody = body;
}
static void sendJSON(int code, const String& body) { reply(code, "application/json", body); }
static void sendOK() { sendJSON(200, "{\"ok\":true}"); }
static void sendErr(const char* msg) {
    String s = "{\"ok\":false,\"err\":\"";
    s += msg;
    s += "\"}";
    sendJSON(400, s);
}

// -------------------------------------------------------
// Auth (H-10) — PBKDF2-SHA256, salted, constant-time compare. Stored in NVS
// (namespace "auth"; was /auth.conf) so uploadfs doesn't drop it.
// -------------------------------------------------------
static bool     authEnabled  = false;
static String   authSaltHex  = "";
static String   authHashHex  = "";
static uint32_t authIter     = 20000;

// 20000 PBKDF2 rounds cost the ESP32 on the order of a second — and Basic
// auth re-sends the password on EVERY request, so with the UI polling every
// 5 s the loop spent much of its time hashing. A successful check now caches
// SHA-256 of the exact Authorization header; later requests compare against
// that (one fast hash). Failed attempts are throttled so a flood of wrong
// passwords can't keep the CPU busy either.
static uint8_t  authCache[32];
static bool     authCacheValid = false;
static uint32_t lastAuthFailMs = 0;
#define AUTH_FAIL_GAP_MS 3000UL

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

static void sha256(const String& s, uint8_t out[32]) {
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               (const uint8_t*)s.c_str(), s.length(), out);
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
    Preferences p;
    p.begin("auth", false);
    p.putBool("enabled", authEnabled);
    p.putString("salt", authSaltHex);
    p.putString("hash", authHashHex);
    p.putUInt("iter", authIter);
    p.end();
    authCacheValid = false;   // password changed/disabled — old header no longer proves anything
}

// One-time import of the pre-6.2 /auth.conf (hashed or even older plaintext).
static void importLegacyAuthFile() {
    File f = LittleFS.open(AUTH_CONFIG_FILE, "r");
    if (!f) return;
    DynamicJsonDocument doc(384);
    bool ok = !deserializeJson(doc, f);
    f.close();
    if (!ok) { logPrintf("[AUTH] %s corrupt — not imported\n", AUTH_CONFIG_FILE); return; }

    authEnabled = doc["enabled"] | false;
    if (doc.containsKey("hash")) {
        authSaltHex = String((const char*)(doc["salt"] | ""));
        authHashHex = String((const char*)(doc["hash"] | ""));
        authIter    = doc["iter"] | 20000;
    } else if (doc.containsKey("password")) {
        setPassword(String((const char*)(doc["password"] | "")));
    }
    saveAuth();
    LittleFS.remove(AUTH_CONFIG_FILE);
    logPrintf("[AUTH] Imported %s into NVS\n", AUTH_CONFIG_FILE);
}

static void loadAuth() {
    Preferences p;
    p.begin("auth", true);
    bool have = p.isKey("enabled");
    if (have) {
        authEnabled = p.getBool("enabled", false);
        authSaltHex = p.getString("salt", "");
        authHashHex = p.getString("hash", "");
        authIter    = p.getUInt("iter", 20000);
    }
    p.end();
    if (!have) importLegacyAuthFile();
    logPrintf("[AUTH] %s\n", authEnabled ? "enabled" : "disabled");
}

void web_resetAuth() {
    Preferences p;
    p.begin("auth", false);
    p.clear();
    p.end();
    LittleFS.remove(AUTH_CONFIG_FILE);
    authEnabled = false;
    authSaltHex = "";
    authHashHex = "";
    authCacheValid = false;
    logPrintf("[AUTH] Admin password RESET by button hold at boot — web UI is open\n");
}

static bool constTimeEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

static bool verifyBasic(const String& authHeader) {
    String b64 = authHeader.substring(6);
    uint8_t decoded[128];
    size_t  outLen = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &outLen,
                              (const uint8_t*)b64.c_str(), b64.length()) != 0) return false;
    decoded[outLen] = 0;
    String userPass((const char*)decoded);
    int colon = userPass.indexOf(':');
    if (colon < 0 || userPass.substring(0, colon) != "admin") return false;
    uint8_t salt[16], want[32], got[32];
    if (!hexToBytes(authSaltHex, salt, sizeof(salt))) return false;
    if (!hexToBytes(authHashHex, want, sizeof(want))) return false;
    if (!pbkdf2(userPass.substring(colon + 1), salt, sizeof(salt), authIter, got)) return false;
    return constTimeEqual(got, want, sizeof(want));
}

static bool checkAuth() {
    if (!authEnabled || authHashHex.length() == 0) return true;

    String hdr = server.header("Authorization");
    if (hdr.startsWith("Basic ")) {
        uint8_t h[32];
        sha256(hdr, h);
        if (authCacheValid && constTimeEqual(h, authCache, sizeof(h))) return true;
        bool throttled = lastAuthFailMs && millis() - lastAuthFailMs < AUTH_FAIL_GAP_MS;
        if (!throttled) {
            if (verifyBasic(hdr)) {
                memcpy(authCache, h, sizeof(h));
                authCacheValid = true;
                return true;
            }
            lastAuthFailMs = millis();
            logPrintf("[AUTH] Wrong password from %s\n", server.client().remoteIP().toString().c_str());
        }
    }

    server.sendHeader("WWW-Authenticate", String("Basic realm=\"") + AUTH_REALM + "\"");
    reply(401, "text/plain", "Unauthorized");
    return false;
}

// H-9: this device's whole API is same-origin (the web UI is served BY this
// same ESP32) — CORS was never needed. Cheap CSRF guard: state-changing
// requests must carry a custom header, which a plain cross-origin form/fetch
// (without a CORS preflight, since we don't answer one) cannot set.
static bool checkOrigin() {
    if (server.header("X-Gong-Request") != "1") {
        reply(403, "text/plain", "Forbidden");
        return false;
    }
    return true;
}

// DNS rebinding: an internet page can re-point its own hostname at this
// device's IP and then talk to it as "same origin" — X-Gong-Request doesn't
// help there. The Host header still carries the attacker's name, so only
// names that are actually this device are accepted: the IPs, "gong", and
// "gong.<one label>" (gong.local via mDNS, gong.lan / gong.home via a
// router's DHCP DNS). Registering a whole top-level domain is out of reach.
static bool hostAllowed() {
    String h = server.hostHeader();
    h.toLowerCase();
    int colon = h.indexOf(':');
    if (colon >= 0) h.remove(colon);
    if (h.endsWith(".")) h.remove(h.length() - 1);
    if (h.isEmpty()) return true;   // HTTP/1.0 client without Host — no name to rebind
    if (h == WiFi.softAPIP().toString()) return true;
    if (WiFi.status() == WL_CONNECTED && h == WiFi.localIP().toString()) return true;
    const String name = MDNS_NAME;
    if (h == name) return true;
    if (h.startsWith(name + ".")) return h.indexOf('.', name.length() + 1) < 0;
    return false;
}

enum : uint8_t { R_OPEN = 0, R_AUTH = 1, R_CSRF = 2, R_WRITE = R_AUTH | R_CSRF };

static void flushResponse() {
    if (respPage) {
        server.sendHeader("Cache-Control", "no-cache");
        server.send_P(200, "text/html; charset=utf-8", (PGM_P)respPage,
                      (size_t)(respPageEnd - respPage) - 1);
    } else if (respCode) {
        server.send(respCode, respType, respBody);
    }
    respBody = String();
}

static void dispatch(void (*fn)(), uint8_t flags) {
    respCode = 0;
    respPage = nullptr;
    if (!hostAllowed()) { server.send(403, "text/plain", "Forbidden host"); return; }
    // Auth runs OUTSIDE sys_lock: a PBKDF2 check takes ~1 s and auth state is
    // only ever touched from this (loopTask) side.
    if ((flags & R_AUTH) && !checkAuth())   { flushResponse(); return; }
    if ((flags & R_CSRF) && !checkOrigin()) { flushResponse(); return; }
    sys_lock();
    fn();
    sys_unlock();
    flushResponse();
}

static void route(const char* uri, HTTPMethod method, void (*fn)(), uint8_t flags) {
    server.on(uri, method, [fn, flags]() { dispatch(fn, flags); });
}

// -------------------------------------------------------
// Input helpers
// -------------------------------------------------------
// Cut to at most maxBytes without splitting a UTF-8 sequence.
static void truncUtf8(String& s, size_t maxBytes) {
    if (s.length() <= maxBytes) return;
    size_t n = maxBytes;
    while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;
    s.remove(n);
}

// Parse the request body; the pool is sized to the body so a long field is
// "too large", not a silent NoMemory.
static bool parseBody(DynamicJsonDocument& doc) {
    const String& body = server.arg("plain");
    if (body.length() > 2048) { sendErr("request too large"); return false; }
    if (deserializeJson(doc, body)) { sendErr("bad json"); return false; }
    return true;
}

struct EntryArgs {
    int    hour, min, track, loop, vol;
    bool   en;
    String desc;
};

// Fields used to be read straight into uint8_t with `| 0` defaults: a request
// without "hour" silently created a 00:00 gong, and hour=256 wrapped to 0.
static bool parseEntry(EntryArgs& e) {
    DynamicJsonDocument doc(1024 + server.arg("plain").length());
    if (!parseBody(doc)) return false;
    if (!doc["hour"].is<int>() || !doc["min"].is<int>()) { sendErr("hour and min are required"); return false; }
    e.hour  = doc["hour"];
    e.min   = doc["min"];
    e.track = doc["track"] | DEFAULT_TRACK;
    e.loop  = doc["loop"]  | 1;
    e.vol   = doc["vol"]   | DEFAULT_VOLUME;
    e.en    = doc["en"]    | true;
    e.desc  = String((const char*)(doc["desc"] | ""));
    if (e.hour < 0 || e.hour > 23 || e.min < 0 || e.min > 59) { sendErr("hour 0-23, min 0-59"); return false; }
    if (e.track < 1 || e.track > 99) { sendErr("track 1-99"); return false; }
    if (!mp3_trackExists((uint8_t)e.track)) { sendErr("track file not found on device"); return false; }
    e.loop = constrain(e.loop, 1, LOOP_MAX);
    e.vol  = constrain(e.vol, 0, 30);
    e.desc.trim();
    truncUtf8(e.desc, DESC_MAX_BYTES);
    return true;
}

static int daysInMonth(int y, int mo) {
    static const uint8_t dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return (mo == 2 && leap) ? 29 : dm[mo - 1];
}

// -------------------------------------------------------
// Handlers — auth/CSRF/host are checked by dispatch() (see route flags in
// web_setup), so a handler only does its own job.
// -------------------------------------------------------
static void handleRoot() { respPage = index_html_start; respPageEnd = index_html_end; }
// /info — схема подключения (server/web/info.html)
static void handleInfo() { respPage = info_html_start;  respPageEnd = info_html_end; }
// /guide — инструкция: светодиод, кнопка, реле, предупреждения (server/web/guide.html)
static void handleGuide() { respPage = guide_html_start; respPageEnd = guide_html_end; }

// /api/schedule — GET only. The UI uses the day-scoped /api/day/entry.
static void handleScheduleGET() {
    String json = sched_toJSON();
    if (json.length() == 0) { sendErr("schedule JSON overflow or out of memory — see logs"); return; }
    sendJSON(200, json);
}

// -------------------------------------------------------
// /api/time — manual set: the fallback when there is neither internet for
// NTP nor a DS3231 holding valid time (see timesync.h). The course's day
// auto-advance runs on the calendar DATE, so keep whatever valid date the
// device already has unless the caller supplies year/month/day.
// -------------------------------------------------------
static void handleTimeSet() {
    DynamicJsonDocument doc(256);
    if (!parseBody(doc)) return;
    int h = doc["hour"] | -1;
    int m = doc["min"]  | -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) { sendErr("invalid time"); return; }

    struct tm cur;
    bool haveCur = localNow(cur) && cur.tm_year >= 124;
    // No date in the request and no valid date on the device: refuse instead
    // of inventing 2024-01-01 (that turned into a ~1000-day course "jump"
    // once the real date was set).
    if (!haveCur && !doc.containsKey("year")) { sendErr("date required"); return; }
    int y  = doc["year"]  | (haveCur ? cur.tm_year + 1900 : 2024);
    int mo = doc["month"] | (haveCur ? cur.tm_mon + 1     : 1);
    int d  = doc["day"]   | (haveCur ? cur.tm_mday        : 1);
    // 31.02 used to be accepted and silently normalised by mktime() into 3 March.
    if (y < 2024 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysInMonth(y, mo)) {
        sendErr("invalid date"); return;
    }
    if (!time_setManual(y, mo, d, h, m)) { sendErr("invalid date"); return; }

    sendOK();
    logPrintf("[TIME] Manual time set: %04d-%02d-%02d %02d:%02d\n", y, mo, d, h, m);
}

// -------------------------------------------------------
// /api/status — see also /api/state (M-10), which bundles this with
// schedule/days/relay/wifi/auth into one response for the UI's poll loop.
// -------------------------------------------------------
static String statusJSON() {
    DynamicJsonDocument doc(768);
    doc["mode"]   = WiFi.status() == WL_CONNECTED ? "AP+STA" : "AP";
    doc["ip"]     = wifi_apIP();
    doc["ssid"]   = AP_SSID;
    if (WiFi.status() == WL_CONNECTED) doc["sta_ip"] = WiFi.localIP().toString();
    doc["heap"]   = (int)ESP.getFreeHeap();
    doc["uptime"] = (uint32_t)(millis() / 1000);
    doc["fw"]     = FW_VERSION;

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
    doc["time_source"] = time_sourceName();   // ntp | rtc | manual | none — chosen automatically
    doc["ntp_age"]     = time_ntpAgeSec();    // s since last NTP sync, -1 = never
    doc["active_day"]  = sched_getActiveDay();
    doc["day_count"]   = DAY_COUNT;
    doc["relay_on"]    = relay_isOn();
    // Health — anything here that is false/true-bad means gongs may not ring.
    doc["fs_ok"]        = sys_fsOk();
    doc["sched_error"]  = sched_hasError();
    doc["reset_reason"] = sys_resetReason();
    doc["crash_count"]  = sys_crashCount();
    doc["ap_default_pw"] = strcmp(AP_PASSWORD, "vipassana") == 0;

    String s;
    serializeJson(doc, s);
    return s;
}

static void handleStatus() { sendJSON(200, statusJSON()); }

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

static void handleAuthStatus() { sendJSON(200, authJSON()); }   // open: UI shows lock state before login

static void handleAuthSave() {
    DynamicJsonDocument doc(512);
    if (!parseBody(doc)) return;
    String pwd = doc["password"] | "";
    if (pwd.length() < AUTH_MIN_PASSWORD) { sendErr("password too short (min 8)"); return; }
    if (pwd.length() > 64)                { sendErr("password too long (max 64)"); return; }
    setPassword(pwd);
    authEnabled = true;
    saveAuth();
    sendOK();
    logPrintf("[AUTH] Password updated, auth enabled\n");
}

static void handleAuthDisable() {
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

static void handleDaysStatus() { sendJSON(200, daysJSON()); }

static void handleDayGet() {
    int n = server.arg("n").toInt();
    if (n < 0 || n >= DAY_COUNT) { sendErr("invalid day"); return; }
    sendJSON(200, sched_dayJSON((uint8_t)n));
}

static void handleDayActivate() {
    int n = server.arg("n").toInt();
    if (n < 0 || n >= DAY_COUNT) { sendErr("invalid day"); return; }
    if (sched_activateDay((uint8_t)n)) sendOK();
    else sendErr("day file missing or unreadable");
}

static void handleTracksGet() { sendJSON(200, mp3_listTracksJSON()); }

// -------------------------------------------------------
// /api/day/entry — add/edit/delete a single entry in a specific day's
// template (?day=N), without activating it. If N is the active day, these
// transparently edit the live schedule (see sched_addToDay & co).
// -------------------------------------------------------
static void handleDayEntryPOST() {
    int day = server.arg("day").toInt();
    if (day < 0 || day >= DAY_COUNT) { sendErr("invalid day"); return; }
    EntryArgs e;
    if (!parseEntry(e)) return;
    if (sched_addToDay((uint8_t)day, e.hour, e.min, e.desc, e.track, e.loop, e.vol)) sendOK();
    else sendErr("failed (day full, day file unreadable, or that time slot is already used)");
}

static void handleDayEntryPUT() {
    int      day = server.arg("day").toInt();
    uint32_t id  = server.arg("id").toInt();
    if (day < 0 || day >= DAY_COUNT) { sendErr("invalid day"); return; }
    if (!id) { sendErr("missing id"); return; }
    EntryArgs e;
    if (!parseEntry(e)) return;
    if (sched_editInDay((uint8_t)day, id, e.hour, e.min, e.desc, e.track, e.loop, e.en, e.vol)) sendOK();
    else sendErr("not found, day file unreadable, or that time slot is already used");
}

static void handleDayEntryDELETE() {
    int      day = server.arg("day").toInt();
    uint32_t id  = server.arg("id").toInt();
    if (day < 0 || day >= DAY_COUNT) { sendErr("invalid day"); return; }
    if (!id) { sendErr("missing id"); return; }
    if (sched_delFromDay((uint8_t)day, id)) sendOK();
    else sendErr("not found or day file unreadable");
}

// -------------------------------------------------------
// /api/play  /api/stop — manual control. Playback goes through the relay
// module so the external amplifier is powered before the first sample.
// -------------------------------------------------------
static void handlePlay() {
    DynamicJsonDocument doc(256);
    if (!parseBody(doc)) return;
    int track = doc["track"] | DEFAULT_TRACK;
    int vol   = doc["vol"]   | DEFAULT_VOLUME;
    int loop  = doc["loop"]  | 1;
    if (track < 1 || track > 99) { sendErr("track 1-99"); return; }
    // loop wasn't capped here (the schedule caps at 7): 255 repeats = hours of gong.
    relay_play((uint8_t)track, (uint8_t)constrain(vol, 0, 30), (uint8_t)constrain(loop, 1, LOOP_MAX));
    sendOK();
}

static void handleStop() {
    relay_stop();
    sendOK();
}

// -------------------------------------------------------
// /api/relay — GET state; POST {"mode":"auto|on|off"} and/or {"pre":ms,"hold":ms}
// -------------------------------------------------------
static void handleRelayGET() { sendJSON(200, relay_toJSON()); }

static void handleRelayPOST() {
    DynamicJsonDocument doc(256);
    if (!parseBody(doc)) return;
    if (doc.containsKey("pre") || doc.containsKey("hold")) {
        long pre  = doc["pre"]  | (long)relay_preMs();
        long hold = doc["hold"] | (long)relay_holdMs();
        if (pre < 0 || hold < 0 || !relay_setTiming((uint32_t)pre, (uint32_t)hold)) {
            sendErr("warm-up 0-5000 ms, hold 0-20000 ms"); return;
        }
    }
    if (doc.containsKey("mode")) {
        RelayMode m;
        if (!relay_parseMode(String((const char*)(doc["mode"] | "")), m)) { sendErr("mode: auto|on|off"); return; }
        relay_setMode(m);
    }
    sendJSON(200, relay_toJSON());
}

// -------------------------------------------------------
// /api/wifi — STA (connect to an existing network). The password is
// write-only: it is never sent back by any endpoint.
// -------------------------------------------------------
static void handleWifiGET() { sendJSON(200, wifi_statusJSON()); }

static void handleWifiPOST() {
    DynamicJsonDocument doc(512);
    if (!parseBody(doc)) return;
    String ssid = doc["ssid"] | "";
    String pass = doc["pass"] | "";
    ssid.trim();
    if (!wifi_setCredentials(ssid, pass)) { sendErr("SSID 1-32 chars, password empty (open) or 8-63 chars"); return; }
    sendOK();
}

static void handleWifiForget() {
    wifi_forget();
    sendOK();
}

static void handleWifiScanPOST() {
    if (wifi_startScan()) sendOK();
    else sendErr("scan failed to start");
}

static void handleWifiScanGET() { sendJSON(200, wifi_scanJSON()); }

static void handleFavicon() { reply(204, "text/plain", ""); }

// /api/logs — live debug log from ring buffer
static void handleLogs() {
    int n = server.arg("n").toInt();
    if (n < 1 || n > 64) n = 40;
    sendJSON(200, logbuffer_toJSON(n));
}

// -------------------------------------------------------
// M-10: one bundled response for the UI's periodic poll — see index.html's
// refreshAll().
// -------------------------------------------------------
static void handleState() {
    String schedule = sched_toJSON();
    if (schedule.length() == 0) schedule = "[]";   // overflow/OOM marker — never break /api/state
    String s = "{";
    s += "\"status\":";   s += statusJSON();
    s += ",\"schedule\":"; s += schedule;
    s += ",\"days\":";     s += daysJSON();
    s += ",\"relay\":";    s += relay_toJSON();
    s += ",\"wifi\":";     s += wifi_statusJSON();
    s += ",\"auth\":";     s += authJSON();
    s += "}";
    sendJSON(200, s);
}

static void handleNotFound() { reply(404, "text/plain", "Not found"); }

// -------------------------------------------------------
// Public setup / loop
// -------------------------------------------------------
void web_setup() {
    loadAuth();

    // Authorization is always collected once collectHeaders() has been called
    // (WebServer adds it itself); X-Gong-Request backs checkOrigin().
    const char* hdrs[] = { "X-Gong-Request" };
    server.collectHeaders(hdrs, 1);

    route("/favicon.ico",       HTTP_GET,    handleFavicon,        R_OPEN);
    route("/",                  HTTP_GET,    handleRoot,           R_AUTH);
    route("/index.html",        HTTP_GET,    handleRoot,           R_AUTH);
    route("/info",              HTTP_GET,    handleInfo,           R_AUTH);
    route("/guide",             HTTP_GET,    handleGuide,          R_AUTH);
    route("/api/schedule",      HTTP_GET,    handleScheduleGET,    R_AUTH);
    route("/api/state",         HTTP_GET,    handleState,          R_AUTH);

    route("/api/time",          HTTP_POST,   handleTimeSet,        R_WRITE);
    route("/api/logs",          HTTP_GET,    handleLogs,           R_AUTH);
    route("/api/status",        HTTP_GET,    handleStatus,         R_AUTH);

    route("/api/auth/status",   HTTP_GET,    handleAuthStatus,     R_OPEN);
    route("/api/auth/save",     HTTP_POST,   handleAuthSave,       R_WRITE);
    route("/api/auth/disable",  HTTP_POST,   handleAuthDisable,    R_WRITE);

    route("/api/days",          HTTP_GET,    handleDaysStatus,     R_AUTH);
    route("/api/day",           HTTP_GET,    handleDayGet,         R_AUTH);
    route("/api/day/activate",  HTTP_POST,   handleDayActivate,    R_WRITE);
    route("/api/day/entry",     HTTP_POST,   handleDayEntryPOST,   R_WRITE);
    route("/api/day/entry",     HTTP_PUT,    handleDayEntryPUT,    R_WRITE);
    route("/api/day/entry",     HTTP_DELETE, handleDayEntryDELETE, R_WRITE);
    route("/api/tracks",        HTTP_GET,    handleTracksGet,      R_AUTH);

    route("/api/play",          HTTP_POST,   handlePlay,           R_WRITE);
    route("/api/stop",          HTTP_POST,   handleStop,           R_WRITE);

    route("/api/relay",         HTTP_GET,    handleRelayGET,       R_AUTH);
    route("/api/relay",         HTTP_POST,   handleRelayPOST,      R_WRITE);

    route("/api/wifi",          HTTP_GET,    handleWifiGET,        R_AUTH);
    route("/api/wifi",          HTTP_POST,   handleWifiPOST,       R_WRITE);
    route("/api/wifi/forget",   HTTP_POST,   handleWifiForget,     R_WRITE);
    route("/api/wifi/scan",     HTTP_POST,   handleWifiScanPOST,   R_WRITE);
    route("/api/wifi/scan",     HTTP_GET,    handleWifiScanGET,    R_AUTH);

    server.onNotFound([]() { dispatch(handleNotFound, R_OPEN); });
    server.begin();
    logPrintf("[WEB] HTTP server listening on port 80\n");
}

void web_loop() {
    server.handleClient();
}
