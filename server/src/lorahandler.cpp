#include "lorahandler.h"
#include "config.h"
#include <SPI.h>
#include <LoRa.h>
#include <ArduinoJson.h>
#include <time.h>

// -------------------------------------------------------
// Track known clients (by their ACK messages)
// -------------------------------------------------------
#define MAX_CLIENTS 16

struct ClientInfo {
    String   id;
    int      rssi;
    uint32_t lastSeenMs;   // millis() of last ACK
};

static ClientInfo clients[MAX_CLIENTS];
static uint8_t    cliCount = 0;

static void upsertClient(const String& id, int rssi) {
    for (uint8_t i = 0; i < cliCount; i++) {
        if (clients[i].id == id) {
            clients[i].rssi       = rssi;
            clients[i].lastSeenMs = millis();
            return;
        }
    }
    if (cliCount < MAX_CLIENTS) {
        clients[cliCount++] = { id, rssi, millis() };
        Serial.printf("[LORA] New client registered: %s\n", id.c_str());
    }
}

// -------------------------------------------------------
// Low-level send
// -------------------------------------------------------
static void loraSend(uint8_t type, const String& payload) {
    // First byte = message type, rest = payload
    LoRa.beginPacket();
    LoRa.write(type);
    LoRa.print(payload);
    LoRa.endPacket();
    Serial.printf("[LORA] TX type=0x%02X payload_len=%d\n", type, payload.length());
}

// -------------------------------------------------------
// Incoming message handlers
// -------------------------------------------------------
static void handleACK(const String& payload, int rssi) {
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, payload)) return;
    String id = doc["id"] | "unknown";
    upsertClient(id, rssi);
    Serial.printf("[LORA] ACK from '%s' RSSI=%d dBm\n", id.c_str(), rssi);
}

// -------------------------------------------------------
void lora_setup() {
    // Use VSPI bus: SCK=18 MISO=19 MOSI=23
    SPI.begin(18, 19, 23, LORA_SS);
    LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

    if (!LoRa.begin(LORA_FREQ)) {
        Serial.println("[LORA] Init FAILED — check module wiring!");
        return;
    }
    LoRa.setSyncWord(LORA_SYNC_WORD);
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setTxPower(LORA_TX_POWER, PA_OUTPUT_PA_BOOST_PIN);

    Serial.printf("[LORA] Server ready @ %.0f MHz  SF=%d BW=%.0fk\n",
                  LORA_FREQ / 1e6, LORA_SF, LORA_BW / 1e3);
}

void lora_loop() {
    int pktSize = LoRa.parsePacket();
    if (pktSize == 0) return;

    uint8_t type = LoRa.read();   // first byte = type
    String  payload = "";
    while (LoRa.available()) payload += (char)LoRa.read();
    int rssi = LoRa.packetRssi();

    Serial.printf("[LORA] RX type=0x%02X len=%d RSSI=%d\n",
                  type, payload.length(), rssi);

    switch (type) {
        case MSG_ACK: handleACK(payload, rssi); break;
        default:
            Serial.printf("[LORA] Unhandled type 0x%02X\n", type);
    }
}

// -------------------------------------------------------
void lora_sendGong(uint8_t track, uint8_t vol) {
    DynamicJsonDocument doc(128);
    doc["track"] = track;
    doc["vol"]   = vol;
    doc["ts"]    = (uint32_t)millis();
    String s;
    serializeJson(doc, s);
    loraSend(MSG_GONG, s);
    Serial.printf("[LORA] GONG broadcast — track=%d vol=%d clients=%d\n",
                  track, vol, cliCount);
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
        JsonObject o = arr.createNestedObject();
        o["id"]      = clients[i].id;
        o["rssi"]    = clients[i].rssi;
        o["seen_ms"] = now - clients[i].lastSeenMs;
    }
    String s;
    serializeJson(doc, s);
    return s;
}

int lora_clientCount() { return cliCount; }
