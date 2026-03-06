#include "lorahandler.h"
#include "config.h"
#include "mp3handler.h"
#include <SPI.h>
#include <RadioLib.h>
#include <ArduinoJson.h>
#include "mbedtls/md.h"

#define LORA_PAYLOAD_MAX 256

// -------------------------------------------------------
// RadioLib: Module(cs, irq, rst, gpio)
// -------------------------------------------------------
static Module mod(LORA_SS, LORA_DIO0, LORA_RST, RADIOLIB_NC);
static SX1278 radio(&mod);

// -------------------------------------------------------
// HMAC verification (mirrors server implementation)
// -------------------------------------------------------
static String computeHMAC(uint8_t msgType, const String& payload) {
    uint8_t hmac[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx,
        (const uint8_t*)LORA_HMAC_KEY, strlen(LORA_HMAC_KEY));
    mbedtls_md_hmac_update(&ctx, &msgType, 1);
    mbedtls_md_hmac_update(&ctx,
        (const uint8_t*)payload.c_str(), payload.length());
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02x", hmac[i]);
    hex[16] = '\0';
    return String(hex);
}

// Verify signature and replay protection.
// doc is parsed payload; on success sig field is removed and ts is checked.
// Returns true if packet is authentic and fresh.
static uint32_t lastServerTs = 0;

static bool verifyMsg(uint8_t type, DynamicJsonDocument& doc) {
    String sig = doc["sig"] | "";
    if (sig.length() == 0) {
        Serial.printf("[LORA] No sig on 0x%02X — rejected\n", type);
        return false;
    }

    // Reconstruct payload without sig (same field order as server sent)
    doc.remove("sig");
    String payload;
    serializeJson(doc, payload);

    if (sig != computeHMAC(type, payload)) {
        Serial.printf("[LORA] Bad sig on 0x%02X — rejected\n", type);
        return false;
    }

    // Replay check: ts must strictly increase
    uint32_t ts = doc["ts"] | 0;
    if (ts > 0 && ts <= lastServerTs) {
        Serial.printf("[LORA] Replay ts=%u last=%u — rejected\n", ts, lastServerTs);
        return false;
    }
    if (ts > 0) lastServerTs = ts;

    return true;
}

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
    uint8_t loop  = doc["loop"]  | 1;

    Serial.printf("[LORA] GONG received! track=%d vol=%d loop=%d RSSI=%d dBm\n",
                  track, vol, loop, rssi);

    if (STATUS_LED >= 0) digitalWrite(STATUS_LED, HIGH);

    mp3_setVolume(vol);
    mp3_play(track, loop);

    lora_sendACK(rssi);
}

static void handleHeartbeat(const String& payload, int rssi) {
    DynamicJsonDocument doc(128);
    if (deserializeJson(doc, payload)) return;
    const char* t = doc["time"] | "--:--:--";
    int clients   = doc["clients"] | 0;
    Serial.printf("[LORA] Heartbeat — server time=%s known_clients=%d\n",
                  t, clients);
    lora_sendACK(rssi);
}

static void handleSchedule(const String& payload) {
    Serial.printf("[LORA] Schedule received (%d bytes)\n", payload.length());
}

// -------------------------------------------------------
void lora_setup() {
    SPI.begin(18, 19, 23, LORA_SS);

    float freqMHz = (float)(LORA_FREQ / 1e6);
    float bwKHz   = (float)(LORA_BW / 1e3);
    int state = radio.begin(freqMHz, bwKHz, LORA_SF, LORA_CR, LORA_SYNC_WORD, LORA_TX_POWER, 8, 0);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] Init FAILED: %d — check module wiring!\n", state);
        return;
    }

    Serial.printf("[LORA] Client '%s' listening @ %.0f MHz  SF=%d BW=%.0fk\n",
                  CLIENT_ID, freqMHz, LORA_SF, bwKHz);

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

    uint8_t type    = buf[0];
    String  payload = "";
    for (size_t i = 1; i < len; i++) payload += (char)buf[i];

    int rssi = (int)radio.getRSSI();

    Serial.printf("[LORA] RX type=0x%02X len=%u RSSI=%d\n",
                  type, (unsigned)(len - 1), rssi);

    // Verify HMAC signature on all command packets
    if (type == MSG_GONG || type == MSG_HEARTBEAT || type == MSG_STOP) {
        DynamicJsonDocument doc(512);
        if (deserializeJson(doc, payload) || !verifyMsg(type, doc)) {
            radio.startReceive();
            return;
        }
        // Re-serialize without sig field for downstream handlers
        serializeJson(doc, payload);
    }

    switch (type) {
        case MSG_GONG:      handleGong(payload, rssi);      break;
        case MSG_HEARTBEAT: handleHeartbeat(payload, rssi); break;
        case MSG_SCHEDULE:  handleSchedule(payload);        break;
        case MSG_STOP:
            Serial.println("[LORA] STOP verified — stopping");
            mp3_stop();
            break;
        default:
            Serial.printf("[LORA] Unknown type 0x%02X\n", type);
    }

    if (STATUS_LED >= 0) digitalWrite(STATUS_LED, LOW);

    radio.startReceive();
}

void lora_sendACK(int rxRssi) {
    DynamicJsonDocument doc(128);
    doc["id"]   = CLIENT_ID;
    doc["rssi"] = rxRssi;

    String payload;
    serializeJson(doc, payload);

    delay(random(10, 80));  // avoid collision if many clients

    uint8_t buf[LORA_PAYLOAD_MAX + 1];
    buf[0] = MSG_ACK;
    size_t plen = payload.length();
    if (plen > LORA_PAYLOAD_MAX) plen = LORA_PAYLOAD_MAX;
    memcpy(buf + 1, payload.c_str(), plen);

    int state = radio.transmit(buf, 1 + plen);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] ACK TX failed: %d\n", state);
    } else {
        Serial.printf("[LORA] ACK sent as '%s'\n", CLIENT_ID);
    }

    radio.startReceive();
}
