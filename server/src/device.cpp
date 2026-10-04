#include "device.h"
#include "config.h"
#include <Preferences.h>
#include <esp_mac.h>
#include <ArduinoJson.h>

static uint8_t  num = 0;
static String   name, ssid, host;
static uint32_t rebootAtMs = 0;

static void buildNames() {
    char s[24], h[16];
    if (num) {
        snprintf(s, sizeof(s), "%s-%u", AP_SSID, (unsigned)num);
        snprintf(h, sizeof(h), "%s%u", MDNS_NAME, (unsigned)num);
    } else {
        // Номер не задан: последние 2 байта MAC — уникальны без настройки.
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);   // из eFuse — работает до запуска Wi-Fi
        snprintf(s, sizeof(s), "%s-%02X%02X", AP_SSID, mac[4], mac[5]);
        snprintf(h, sizeof(h), "%s%02x%02x", MDNS_NAME, mac[4], mac[5]);
    }
    ssid = s;
    host = h;
}

void device_setup() {
    Preferences p;
    p.begin("device", true);
    bool have = p.isKey("num");
    num  = p.getUChar("num", DEVICE_NUM);
    name = p.getString("name", "");
    p.end();
    if (num > DEVICE_NUM_MAX) num = 0;
    buildNames();
    logPrintf("[DEV] Device #%u%s%s — AP '%s', http://%s.local\n",
              (unsigned)num, name.length() ? " " : "", name.c_str(),
              ssid.c_str(), host.c_str());
    if (!have && !num)
        logPrintf("[DEV] Number not set — name from MAC; set it in the web UI (Device)\n");
}

void device_loop() {
    if (rebootAtMs && (int32_t)(millis() - rebootAtMs) >= 0) {
        logPrintf("[DEV] Restarting with new device number\n");
        delay(100);
        ESP.restart();
    }
}

uint8_t       device_num()  { return num; }
const String& device_name() { return name; }
const String& device_ssid() { return ssid; }
const String& device_host() { return host; }

bool device_set(uint8_t newNum, const String& newName, bool* reboot) {
    if (newNum > DEVICE_NUM_MAX) return false;
    String n = newName;
    n.trim();
    if (n.length() > DEVICE_NAME_MAX) {   // не резать посреди UTF-8 символа
        size_t k = DEVICE_NAME_MAX;
        while (k > 0 && ((uint8_t)n[k] & 0xC0) == 0x80) k--;
        n.remove(k);
    }
    Preferences p;
    p.begin("device", false);
    p.putUChar("num", newNum);
    p.putString("name", n);
    p.end();
    name = n;
    bool changed = newNum != num;
    *reboot = changed;
    if (changed) {
        logPrintf("[DEV] Number %u -> %u — restart in %u s\n",
                  (unsigned)num, (unsigned)newNum, (unsigned)(DEVICE_REBOOT_DELAY_MS / 1000));
        rebootAtMs = millis() + DEVICE_REBOOT_DELAY_MS;
        if (!rebootAtMs) rebootAtMs = 1;
    }
    return true;
}

String device_toJSON() {
    StaticJsonDocument<256> doc;
    doc["num"]  = num;
    doc["name"] = name;
    doc["ssid"] = ssid;
    doc["host"] = host;
    doc["restarting"] = rebootAtMs != 0;
    String s;
    serializeJson(doc, s);
    return s;
}
