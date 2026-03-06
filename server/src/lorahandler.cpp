#include "lorahandler.h"
#include "config.h"
#include <SPI.h>
#include <RadioLib.h>
#include <ArduinoJson.h>
#include <time.h>

#define LORA_PAYLOAD_MAX 256

// -------------------------------------------------------
// RadioLib: Module(cs, irq, rst, gpio)
// -------------------------------------------------------
static Module mod(LORA_SS, LORA_DIO0, LORA_RST, RADIOLIB_NC);
static SX1278 radio(&mod);

// -------------------------------------------------------
// Track known clients (by their ACK messages)
// -------------------------------------------------------
#define MAX_CLIENTS 16

struct ClientInfo {
    String   id;
    int      rssi;
    uint32_t lastSeenMs;
};

static ClientInfo clients[MAX_CLIENTS];
static uint8_t    cliCount = 0;

static void upsertClient(const String& id, int rssi) {
    unsigned long now = millis();

    for (uint8_t i = 0; i < cliCount; i++) {
        if (clients[i].id == id) {
            clients[i].rssi       = rssi;
            clients[i].lastSeenMs = now;
            return;
        }
    }

    if (cliCount >= MAX_CLIENTS) {
        uint8_t oldest = 0;
        for (uint8_t i = 1; i < cliCount; i++) {
            if (now - clients[i].lastSeenMs > now - clients[oldest].lastSeenMs)
                oldest = i;
        }
        if (now - clients[oldest].lastSeenMs > CLIENT_TIMEOUT_MS) {
            Serial.printf("[LORA] Evicting stale client: %s\n",
                          clients[oldest].id.c_str());
            clients[oldest] = { id, rssi, now };
            return;
        }
        Serial.println("[LORA] MAX_CLIENTS reached, new client ignored");
        return;
    }

    clients[cliCount++] = { id, rssi, now };
    Serial.printf("[LORA] New client registered: %s\n", id.c_str());
}

// -------------------------------------------------------
// Low-level send: packet = [type byte][payload string]
// -------------------------------------------------------
static void loraSend(uint8_t type, const String& payload) {
    size_t len = 1 + (payload.length() < LORA_PAYLOAD_MAX ? payload.length() : LORA_PAYLOAD_MAX);
    uint8_t buf[LORA_PAYLOAD_MAX + 1];
    buf[0] = type;
    memcpy(buf + 1, payload.c_str(), len - 1);

    int state = radio.transmit(buf, len);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] TX failed: %d\n", state);
    } else {
        Serial.printf("[LORA] TX type=0x%02X payload_len=%u\n", type, (unsigned)(len - 1));
    }

    radio.startReceive();  // return to RX mode after every TX
}

static void handleACK(const String& payload, int rssi) {
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, payload)) return;
    String id = doc["id"] | "unknown";
    upsertClient(id, rssi);
    Serial.printf("[LORA] ACK from '%s' RSSI=%d dBm\n", id.c_str(), rssi);
}

// -------------------------------------------------------
void lora_setup() {
    SPI.begin(18, 19, 23, LORA_SS);

    // RadioLib begin(freq_MHz, bw_kHz, sf, cr, syncWord, power_dBm, preambleLen, gain)
    float freqMHz = (float)(LORA_FREQ / 1e6);
    float bwKHz   = (float)(LORA_BW / 1e3);
    int state = radio.begin(freqMHz, bwKHz, LORA_SF, LORA_CR, LORA_SYNC_WORD, LORA_TX_POWER, 8, 0);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] Init FAILED: %d — check module wiring!\n", state);
        return;
    }

    Serial.printf("[LORA] Server ready @ %.0f MHz  SF=%d BW=%.0fk\n",
                  freqMHz, LORA_SF, bwKHz);

    // Неблокирующий приём: не вызываем receive() в loop, иначе аудио не успевает
    radio.startReceive();
}

void lora_loop() {
    // DIO0 goes HIGH when packet is received (mapped by startReceive())
    if (!digitalRead(LORA_DIO0)) return;

    size_t len = radio.getPacketLength();
    if (len == 0 || len > LORA_PAYLOAD_MAX) {
        radio.startReceive();
        return;
    }

    uint8_t buf[LORA_PAYLOAD_MAX + 1];
    int state = radio.readData(buf, len);
    if (state != RADIOLIB_ERR_NONE) {
        radio.startReceive();
        return;
    }

    uint8_t type = buf[0];
    String payload;
    for (size_t i = 1; i < len; i++) payload += (char)buf[i];

    int rssi = (int)radio.getRSSI();

    Serial.printf("[LORA] RX type=0x%02X len=%u RSSI=%d\n",
                  type, (unsigned)(len - 1), rssi);

    switch (type) {
        case MSG_ACK: handleACK(payload, rssi); break;
        default:
            Serial.printf("[LORA] Unhandled type 0x%02X\n", type);
    }

    radio.startReceive();
}

// -------------------------------------------------------
void lora_sendGong(uint8_t track, uint8_t vol, uint8_t loop) {
    DynamicJsonDocument doc(128);
    doc["track"] = track;
    doc["vol"]   = vol;
    doc["loop"]  = loop;
    doc["ts"]    = (uint32_t)millis();
    String s;
    serializeJson(doc, s);
    loraSend(MSG_GONG, s);
    Serial.printf("[LORA] GONG broadcast — track=%d vol=%d loop=%d clients=%d\n",
                  track, vol, loop, cliCount);
}

void lora_sendHeartbeat() {
    DynamicJsonDocument doc(128);
    struct tm ti;
    if (getLocalTime(&ti)) {
        char buf[9];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
        doc["time"] = buf;
    } else {
        doc["time"] = "--:--:--";
    }
    doc["clients"] = cliCount;
    String s;
    serializeJson(doc, s);
    loraSend(MSG_HEARTBEAT, s);
}

void lora_sendSchedule(const String& scheduleJson) {
    loraSend(MSG_SCHEDULE, scheduleJson);
    Serial.println("[LORA] Schedule sync broadcast");
}

// -------------------------------------------------------
String lora_clientsJSON() {
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.to<JsonArray>();
    unsigned long now = millis();
    for (uint8_t i = 0; i < cliCount; i++) {
        unsigned long age = now - clients[i].lastSeenMs;
        if (age > CLIENT_TIMEOUT_MS) continue;
        JsonObject o = arr.createNestedObject();
        o["id"]      = clients[i].id;
        o["rssi"]    = clients[i].rssi;
        o["seen_ms"] = age;
    }
    String s;
    serializeJson(doc, s);
    return s;
}

int lora_clientCount() {
    unsigned long now = millis();
    int active = 0;
    for (uint8_t i = 0; i < cliCount; i++) {
        if (now - clients[i].lastSeenMs <= CLIENT_TIMEOUT_MS) active++;
    }
    return active;
}
