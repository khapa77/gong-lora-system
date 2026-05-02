#include "lorahandler.h"
#include "config.h"
#include <SPI.h>
#include <RadioLib.h>
#include <ArduinoJson.h>
#include <time.h>
#include "mbedtls/md.h"

#define LORA_PAYLOAD_MAX 256

static Module mod(LORA_SS, LORA_DIO0, LORA_RST, RADIOLIB_NC);
static SX1278 radio(&mod);

// -------------------------------------------------------
// Non-blocking TX queue (single slot)
// -------------------------------------------------------
static uint8_t  txBuf[LORA_PAYLOAD_MAX + 1];
static size_t   txLen     = 0;
static bool     txPending = false;
static bool     txBusy    = false;
static uint8_t  txType    = 0;

static void txEnqueue(uint8_t type, const uint8_t* buf, size_t len) {
    if (len > LORA_PAYLOAD_MAX + 1) len = LORA_PAYLOAD_MAX + 1;
    memcpy(txBuf, buf, len);
    txLen     = len;
    txType    = type;
    txPending = true;
}

// -------------------------------------------------------
// Track known clients
// -------------------------------------------------------
#define MAX_CLIENTS 16

struct ClientInfo {
    String   id;
    int      rssi;
    uint32_t lastSeenMs;
    uint32_t rttMs;
    uint32_t oneWayMs;
};

static ClientInfo clients[MAX_CLIENTS];
static uint8_t    cliCount = 0;

static uint32_t lastHeartbeatSentMs = 0;
static const uint32_t ACK_RANDOM_DELAY_AVG_MS = 45;

static void upsertClient(const String& id, int rssi, uint32_t rtt) {
    unsigned long now = millis();

    for (uint8_t i = 0; i < cliCount; i++) {
        if (clients[i].id == id) {
            clients[i].rssi       = rssi;
            clients[i].lastSeenMs = now;
            if (rtt > 0) {
                uint32_t trueRtt = (rtt > ACK_RANDOM_DELAY_AVG_MS)
                                   ? rtt - ACK_RANDOM_DELAY_AVG_MS : 0;
                clients[i].rttMs    = (clients[i].rttMs == 0)
                                      ? trueRtt
                                      : (clients[i].rttMs * 7 + trueRtt * 3) / 10;
                clients[i].oneWayMs = clients[i].rttMs / 2;
                Serial.printf("[LORA] RTT '%s': raw=%ums true=%ums one-way=%ums\n",
                              id.c_str(), rtt, trueRtt, clients[i].oneWayMs);
            }
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
            clients[oldest] = { id, rssi, now, 0, 0 };
            return;
        }
        Serial.println("[LORA] MAX_CLIENTS reached, new client ignored");
        return;
    }

    clients[cliCount++] = { id, rssi, now, 0, 0 };
    Serial.printf("[LORA] New client registered: %s\n", id.c_str());
}

// -------------------------------------------------------
// HMAC-SHA256: sign [type_byte | payload], return 8-byte hex (16 chars)
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

static uint32_t nowTs() {
    time_t t = time(nullptr);
    return (t > 100000UL) ? (uint32_t)t : (uint32_t)(millis() / 1000);
}

// -------------------------------------------------------
// Prepare packet and enqueue (non-blocking)
// -------------------------------------------------------
static void loraSend(uint8_t type, const String& payload) {
    String finalPayload = payload;

    if (type == MSG_GONG || type == MSG_HEARTBEAT || type == MSG_STOP) {
        String sig = computeHMAC(type, payload);
        finalPayload = payload.substring(0, payload.length() - 1)
                       + ",\"sig\":\"" + sig + "\"}";
    }

    size_t plen = finalPayload.length();
    if (plen > LORA_PAYLOAD_MAX) plen = LORA_PAYLOAD_MAX;

    uint8_t buf[LORA_PAYLOAD_MAX + 1];
    buf[0] = type;
    memcpy(buf + 1, finalPayload.c_str(), plen);

    txEnqueue(type, buf, 1 + plen);

    Serial.printf("[LORA] TX queued type=0x%02X payload_len=%u\n",
                  type, (unsigned)plen);
}

// -------------------------------------------------------
static void handleACK(const String& payload, int rssi) {
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, payload)) return;
    String id = doc["id"] | "unknown";
    uint32_t rtt = (lastHeartbeatSentMs > 0)
                   ? (uint32_t)(millis() - lastHeartbeatSentMs) : 0;
    upsertClient(id, rssi, rtt);
    Serial.printf("[LORA] ACK from '%s' RSSI=%d dBm\n", id.c_str(), rssi);
}

// -------------------------------------------------------
void lora_setup() {
    SPI.begin(18, 19, 23, LORA_SS);

    float freqMHz = (float)(LORA_FREQ / 1e6);
    float bwKHz   = (float)(LORA_BW / 1e3);
    int state = radio.begin(freqMHz, bwKHz, LORA_SF, LORA_CR,
                            LORA_SYNC_WORD, LORA_TX_POWER, 8, 0);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] Init FAILED: %d — check module wiring!\n", state);
        return;
    }

    Serial.printf("[LORA] Server ready @ %.0f MHz  SF=%d BW=%.0fk\n",
                  freqMHz, LORA_SF, bwKHz);

    radio.startReceive();
}

// -------------------------------------------------------
// lora_loop — non-blocking TX state machine + RX
// -------------------------------------------------------
void lora_loop() {

    // ── TX ──────────────────────────────────────────────
    if (txBusy) {
        // DIO0 HIGH = TX done
        if (digitalRead(LORA_DIO0)) {
            int state = radio.finishTransmit();
            txBusy = false;
            if (state != RADIOLIB_ERR_NONE) {
                Serial.printf("[LORA] TX finish error: %d\n", state);
            } else {
                Serial.printf("[LORA] TX done type=0x%02X\n", txType);
            }
            radio.startReceive();
        }
        return;
    }

    if (txPending) {
        txPending = false;
        txBusy    = true;
        int state = radio.startTransmit(txBuf, txLen);
        if (state != RADIOLIB_ERR_NONE) {
            Serial.printf("[LORA] TX start error: %d\n", state);
            txBusy = false;
            radio.startReceive();
        }
        return;
    }

    // ── RX ──────────────────────────────────────────────
    // DIO0 HIGH = packet received (startReceive mode)
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
    doc["ts"]    = nowTs();
    String s;
    serializeJson(doc, s);
    loraSend(MSG_GONG, s);
    Serial.printf("[LORA] GONG TX — track=%d vol=%d loop=%d clients=%d\n",
                  track, vol, loop, cliCount);

    // Block until TX completes so mp3_play() starts after client receives the packet.
    // This preserves audio sync: client begins playback at TX-end, same as server.
    while (txPending || txBusy) lora_loop();
}

void lora_sendStop() {
    DynamicJsonDocument doc(64);
    doc["ts"] = nowTs();
    String s;
    serializeJson(doc, s);
    loraSend(MSG_STOP, s);
    Serial.println("[LORA] STOP queued");
}

void lora_sendHeartbeat() {
    // Skip if TX busy — don't preempt a gong or stop command
    if (txBusy || txPending) return;

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
    doc["ts"]      = nowTs();
    String s;
    serializeJson(doc, s);
    lastHeartbeatSentMs = millis();
    loraSend(MSG_HEARTBEAT, s);
}

void lora_sendSchedule(const String& scheduleJson) {
    loraSend(MSG_SCHEDULE, scheduleJson);
    Serial.println("[LORA] Schedule sync queued");
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
        o["id"]         = clients[i].id;
        o["rssi"]       = clients[i].rssi;
        o["seen_ms"]    = age;
        o["rtt_ms"]     = clients[i].rttMs;
        o["one_way_ms"] = clients[i].oneWayMs;
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

uint32_t lora_getAvgOneWayMs() {
    unsigned long now = millis();
    uint32_t sum = 0;
    uint8_t  n   = 0;
    for (uint8_t i = 0; i < cliCount; i++) {
        if (now - clients[i].lastSeenMs <= CLIENT_TIMEOUT_MS
            && clients[i].oneWayMs > 0) {
            sum += clients[i].oneWayMs;
            n++;
        }
    }
    return n ? sum / n : 0;
}
