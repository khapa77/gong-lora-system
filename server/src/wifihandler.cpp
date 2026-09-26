#include "wifihandler.h"
#include "config.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

static String   staSsid;
static String   staPass;
static bool     connecting     = false;
static bool     wasConnected   = false;
static uint32_t attemptStart   = 0;
static uint32_t lastAttemptEnd = 0;

static void saveConfig() {
    File f = LittleFS.open(WIFI_CONFIG_FILE, "w");
    if (!f) { logPrintf("[WIFI] Failed to save %s\n", WIFI_CONFIG_FILE); return; }
    StaticJsonDocument<256> doc;
    doc["ssid"] = staSsid;
    doc["pass"] = staPass;
    serializeJson(doc, f);
    f.close();
}

static void loadConfig() {
    File f = LittleFS.open(WIFI_CONFIG_FILE, "r");
    if (!f) return;
    StaticJsonDocument<256> doc;
    bool ok = !deserializeJson(doc, f);
    f.close();
    if (!ok) { logPrintf("[WIFI] %s is corrupt — ignoring\n", WIFI_CONFIG_FILE); return; }
    staSsid = String((const char*)(doc["ssid"] | ""));
    staPass = String((const char*)(doc["pass"] | ""));
}

static void beginConnect() {
    if (staSsid.isEmpty()) return;
    logPrintf("[WIFI] STA connecting to '%s'...\n", staSsid.c_str());
    WiFi.begin(staSsid.c_str(), staPass.isEmpty() ? nullptr : staPass.c_str());
    connecting   = true;
    attemptStart = millis();
}

void wifi_setup() {
    // persistent(false): учётные данные живут в нашем WIFI_CONFIG_FILE, а не
    // в NVS самого WiFi-драйвера — иначе "Forget" не забывал бы сеть
    // полностью, и драйвер подключался бы сам по старым данным.
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false);   // повторные попытки — только в wifi_loop()
    WiFi.setHostname(MDNS_NAME);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    logPrintf("[WIFI] AP '%s' started — IP: %s\n",
              AP_SSID, WiFi.softAPIP().toString().c_str());

    loadConfig();
    if (staSsid.length()) beginConnect();
    else                  logPrintf("[WIFI] STA not configured — AP only\n");

    MDNS.begin(MDNS_NAME);
    // M11: without an advertised service, some resolvers (notably Windows
    // without Bonjour, and some Android NSD-based clients) never resolve the
    // plain hostname — only the fact that the device offers an "http"
    // service actually gets it into their mDNS cache.
    MDNS.addService("http", "tcp", 80);
    logPrintf("[MDNS] http://%s.local\n", MDNS_NAME);
}

void wifi_loop() {
    if (staSsid.isEmpty()) return;
    uint32_t now = millis();
    bool up = WiFi.status() == WL_CONNECTED;

    if (up) {
        if (!wasConnected) {
            wasConnected = true;
            connecting   = false;
            // Внимание: AP переезжает на канал роутера — клиенты нашей AP
            // на секунду отваливаются и переподключаются сами.
            logPrintf("[WIFI] STA connected to '%s' — IP: %s, RSSI %d dBm, ch %d\n",
                      staSsid.c_str(), WiFi.localIP().toString().c_str(),
                      (int)WiFi.RSSI(), (int)WiFi.channel());
        }
        return;
    }

    if (wasConnected) {
        wasConnected   = false;
        connecting     = false;
        lastAttemptEnd = now - WIFI_RETRY_MS + 5000;   // первая повторная попытка через 5 с
        logPrintf("[WIFI] STA connection to '%s' lost\n", staSsid.c_str());
    }

    if (connecting && now - attemptStart >= WIFI_CONNECT_TIMEOUT_MS) {
        // Прекращаем попытку явно: пока драйвер ищет сеть, он перебирает
        // каналы и мешает клиентам собственной AP.
        WiFi.disconnect(false);
        connecting     = false;
        lastAttemptEnd = now;
        logPrintf("[WIFI] STA '%s' not reachable (status %d) — retry in %us\n",
                  staSsid.c_str(), (int)WiFi.status(), (unsigned)(WIFI_RETRY_MS / 1000));
    }

    if (!connecting && now - lastAttemptEnd >= WIFI_RETRY_MS) beginConnect();
}

bool wifi_setCredentials(const String& ssid, const String& pass) {
    if (ssid.isEmpty() || ssid.length() > 32) return false;
    if (pass.length() > 63 || (pass.length() > 0 && pass.length() < 8)) return false;
    staSsid = ssid;
    staPass = pass;
    saveConfig();
    WiFi.disconnect(false);
    wasConnected = false;
    beginConnect();
    return true;
}

void wifi_forget() {
    WiFi.disconnect(false, true);
    staSsid = "";
    staPass = "";
    connecting   = false;
    wasConnected = false;
    LittleFS.remove(WIFI_CONFIG_FILE);
    logPrintf("[WIFI] STA credentials forgotten — AP only\n");
}

bool wifi_startScan() {
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return true;
    // Скан во время попытки подключения драйвер отклоняет — сначала её прерываем.
    if (connecting) { WiFi.disconnect(false); connecting = false; lastAttemptEnd = millis(); }
    return WiFi.scanNetworks(/*async=*/true) == WIFI_SCAN_RUNNING;
}

String wifi_scanJSON() {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return "{\"running\":true}";
    if (n < 0) return "{\"running\":false,\"nets\":[]}";

    DynamicJsonDocument doc(256 + n * 96);
    doc["running"] = false;
    JsonArray arr = doc.createNestedArray("nets");
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) continue;   // скрытые сети
        bool dup = false;               // один SSID с нескольких точек — показываем лучшую
        for (JsonObject o : arr) if (ssid == (const char*)o["ssid"]) { dup = true; break; }
        if (dup) continue;
        JsonObject o = arr.createNestedObject();
        o["ssid"] = ssid;
        o["rssi"] = WiFi.RSSI(i);
        o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    }
    String s;
    serializeJson(doc, s);
    return s;
}

String wifi_statusJSON() {
    StaticJsonDocument<320> doc;
    doc["ap_ssid"]    = AP_SSID;
    doc["ap_ip"]      = WiFi.softAPIP().toString();
    doc["configured"] = !staSsid.isEmpty();
    doc["ssid"]       = staSsid;
    bool up = WiFi.status() == WL_CONNECTED;
    doc["connected"]  = up;
    doc["connecting"] = connecting;
    if (up) {
        doc["ip"]   = WiFi.localIP().toString();
        doc["rssi"] = WiFi.RSSI();
    }
    String s;
    serializeJson(doc, s);
    return s;
}

String wifi_apIP() { return WiFi.softAPIP().toString(); }
