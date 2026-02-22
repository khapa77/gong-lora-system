#include "lorahandler.h"
#include "config.h"
#include "mp3handler.h"
#include <SPI.h>
#include <LoRa.h>
#include <ArduinoJson.h>

// -------------------------------------------------------
// Incoming message handlers
// -------------------------------------------------------
static void handleGong(const String& payload, int rssi) {
    DynamicJsonDocument doc(128);
    if (deserializeJson(doc, payload)) {
        Serial.println("[LORA] Bad GONG payload");
        return;
    }
    uint8_t track = doc["track"] | 1;
    uint8_t vol   = doc["vol"]   | DEFAULT_VOLUME;

    Serial.printf("[LORA] GONG received! track=%d vol=%d RSSI=%d dBm\n",
                  track, vol, rssi);

    // Flash LED while playing
    if (STATUS_LED >= 0) digitalWrite(STATUS_LED, HIGH);

    mp3_setVolume(vol);
    mp3_play(track);

    // Send ACK back to server
    lora_sendACK(rssi);
}

static void handleHeartbeat(const String& payload) {
    DynamicJsonDocument doc(128);
    if (deserializeJson(doc, payload)) return;
    const char* t = doc["time"] | "--:--:--";
    int clients   = doc["clients"] | 0;
    Serial.printf("[LORA] Heartbeat — server time=%s known_clients=%d\n",
                  t, clients);
}

static void handleSchedule(const String& payload) {
    // Clients receive schedule for informational purposes
    // (could be used for local fallback — not implemented here)
    Serial.printf("[LORA] Schedule received (%d bytes)\n", payload.length());
}

// -------------------------------------------------------
void lora_setup() {
    SPI.begin(18, 19, 23, LORA_SS);   // VSPI: SCK MISO MOSI SS
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

    Serial.printf("[LORA] Client '%s' listening @ %.0f MHz\n",
                  CLIENT_ID, LORA_FREQ / 1e6);
}

void lora_loop() {
    int pktSize = LoRa.parsePacket();
    if (pktSize == 0) return;

    uint8_t type    = LoRa.read();  // first byte = message type
    String  payload = "";
    while (LoRa.available()) payload += (char)LoRa.read();
    int rssi = LoRa.packetRssi();

    switch (type) {
        case MSG_GONG:      handleGong(payload, rssi);      break;
        case MSG_HEARTBEAT: handleHeartbeat(payload);        break;
        case MSG_SCHEDULE:  handleSchedule(payload);         break;
        default:
            Serial.printf("[LORA] Unknown type 0x%02X\n", type);
    }

    // Turn off LED after message processed
    if (STATUS_LED >= 0) digitalWrite(STATUS_LED, LOW);
}

void lora_sendACK(int rxRssi) {
    DynamicJsonDocument doc(128);
    doc["id"]   = CLIENT_ID;
    doc["rssi"] = rxRssi;

    String payload;
    serializeJson(doc, payload);

    delay(random(10, 80));  // small random delay to avoid collision if many clients

    LoRa.beginPacket();
    LoRa.write(MSG_ACK);
    LoRa.print(payload);
    LoRa.endPacket();

    Serial.printf("[LORA] ACK sent as '%s'\n", CLIENT_ID);
}
