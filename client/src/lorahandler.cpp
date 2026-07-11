#include "lorahandler.h"
#include "config.h"
#include "mp3handler.h"
#include <SPI.h>
#include <RadioLib.h>
#include <ArduinoJson.h>
#include "mbedtls/md.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#define LORA_PAYLOAD_MAX 256

static Module mod(LORA_SS, LORA_DIO0, LORA_RST, RADIOLIB_NC);
static SX1278 radio(&mod);

static volatile bool dioFlag = false;
static void IRAM_ATTR onDio0() { dioFlag = true; }

static volatile bool loraReady = false;

// ── HMAC-SHA256 (mirrors server) ─────────────────────────────────────────
static String computeHMAC(uint8_t msgType, const String& payload) {
    uint8_t hmac[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const uint8_t*)LORA_HMAC_KEY, strlen(LORA_HMAC_KEY));
    mbedtls_md_hmac_update(&ctx, &msgType, 1);
    mbedtls_md_hmac_update(&ctx, (const uint8_t*)payload.c_str(), payload.length());
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02x", hmac[i]);
    hex[16] = '\0';
    return String(hex);
}

// ── Replay protection ─────────────────────────────────────────────────────
static uint32_t lastServerTs  = 0;
static String   lastServerSig = "";

// A backward jump this large means the server rebooted without RTC/NTP and
// restarted its clock near zero — treat it as a resync, not an attack. Only
// a small backward/equal jump is flagged as a real replay.
static const uint32_t REPLAY_RESYNC_GAP_S = 3600;

static bool verifyMsg(uint8_t type, DynamicJsonDocument& doc) {
    String sig = doc["sig"] | "";
    if (sig.length() == 0) {
        Serial.printf("[LORA] No sig on 0x%02X — rejected\n", type);
        return false;
    }
    doc.remove("sig");
    String payload;
    serializeJson(doc, payload);
    if (sig != computeHMAC(type, payload)) {
        Serial.printf("[LORA] Bad sig on 0x%02X — rejected\n", type);
        return false;
    }
    uint32_t ts = doc["ts"] | 0;
    if (ts > 0) {
        if (ts < lastServerTs) {
            uint32_t drop = lastServerTs - ts;
            if (drop < REPLAY_RESYNC_GAP_S) {
                Serial.printf("[LORA] Replay ts=%u last=%u — rejected\n", ts, lastServerTs);
                return false;
            }
            Serial.printf("[LORA] Server clock reset (ts=%u < last=%u) — resyncing\n", ts, lastServerTs);
        } else if (ts == lastServerTs && sig == lastServerSig) {
            Serial.printf("[LORA] Duplicate packet ts=%u — rejected\n", ts);
            return false;
        }
        lastServerTs  = ts;
        lastServerSig = sig;
    }
    return true;
}

// ── Command queue: Core 0 → Core 1 (audio calls stay off the radio task) ──
struct RxCmd {
    uint8_t type;
    uint8_t track, vol, loop;
    int     rssi;
};
static QueueHandle_t rxQueue = nullptr;

// ── ACK — always sent synchronously from Core 0, inside loraTask ─────────
static void sendAck(int rxRssi) {
    DynamicJsonDocument doc(128);
    doc["id"]   = CLIENT_ID;
    doc["rssi"] = rxRssi;
    String payload;
    serializeJson(doc, payload);

    vTaskDelay(pdMS_TO_TICKS(10 + random(0, 70)));  // jitter to reduce collisions

    uint8_t buf[LORA_PAYLOAD_MAX + 1];
    buf[0] = MSG_ACK;
    size_t plen = payload.length();
    if (plen > LORA_PAYLOAD_MAX) plen = LORA_PAYLOAD_MAX;
    memcpy(buf + 1, payload.c_str(), plen);

    // Non-blocking split TX — radio.transmit() busy-waits internally with no
    // yield, which at long-airtime SF starves IDLE0 long enough to trip the
    // task watchdog. startTransmit()/finishTransmit() let us yield every tick.
    dioFlag = false;
    int state = radio.startTransmit(buf, 1 + plen);
    if (state == RADIOLIB_ERR_NONE) {
        uint32_t waitStart = millis();
        const uint32_t txTimeoutMs = 5000;
        while (!dioFlag && millis() - waitStart < txTimeoutMs) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (dioFlag) {
            dioFlag = false;
            state = radio.finishTransmit();
            if (state != RADIOLIB_ERR_NONE)
                Serial.printf("[LORA] ACK TX failed: %d\n", state);
        } else {
            Serial.println("[LORA] ACK TX timed out");
            radio.finishTransmit();
        }
    } else {
        Serial.printf("[LORA] ACK TX start failed: %d\n", state);
    }

    radio.startReceive();
}

// ── Core 0: LoRa task — RX + ACK; audio dispatched to Core 1 via queue ───
static void loraTask(void*) {
    for (;;) {
        if (!dioFlag) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        dioFlag = false;

        size_t len = radio.getPacketLength();
        if (len == 0 || len > LORA_PAYLOAD_MAX) {
            radio.startReceive();
            continue;
        }

        uint8_t buf[LORA_PAYLOAD_MAX + 1];
        if (radio.readData(buf, len) != RADIOLIB_ERR_NONE) {
            radio.startReceive();
            continue;
        }

        uint8_t type = buf[0];
        String  payload = "";
        for (size_t i = 1; i < len; i++) payload += (char)buf[i];
        int rssi = (int)radio.getRSSI();

        if (type == MSG_GONG || type == MSG_HEARTBEAT || type == MSG_STOP) {
            DynamicJsonDocument doc(512);
            if (deserializeJson(doc, payload) || !verifyMsg(type, doc)) {
                radio.startReceive();
                continue;
            }
            serializeJson(doc, payload);
        }

        if (type == MSG_HEARTBEAT) {
            sendAck(rssi);  // startReceive() called inside sendAck
            continue;
        }

        if (type == MSG_GONG) {
            DynamicJsonDocument doc(128);
            RxCmd cmd = {};
            cmd.type = type;
            cmd.rssi = rssi;
            if (!deserializeJson(doc, payload)) {
                cmd.track = doc["track"] | 1;
                cmd.vol   = doc["vol"]   | DEFAULT_VOLUME;
                cmd.loop  = doc["loop"]  | 1;
            }
            sendAck(rssi);  // ACK on Core 0, audio dispatched to Core 1
            xQueueSend(rxQueue, &cmd, 0);
        } else if (type == MSG_STOP) {
            RxCmd cmd = {};
            cmd.type = type;
            cmd.rssi = rssi;
            radio.startReceive();
            xQueueSend(rxQueue, &cmd, 0);
        } else if (type == MSG_SCHEDULE) {
            Serial.printf("[LORA] Schedule received (%u bytes)\n", (unsigned)(len - 1));
            radio.startReceive();
        } else {
            Serial.printf("[LORA] Unknown type 0x%02X\n", type);
            radio.startReceive();
        }
    }
}

// ────────────────────────────────────────────────────────────────────────
void lora_setup() {
    // RTOS primitives FIRST — must exist even if the radio isn't wired up.
    rxQueue = xQueueCreate(4, sizeof(RxCmd));

    SPI.begin(LORA_SPI_SCK, LORA_SPI_MISO, LORA_SPI_MOSI, LORA_SS);

    float freqMHz = (float)LORA_FREQ;   // already MHz — do NOT divide
    float bwKHz   = (float)LORA_BW;     // already kHz — do NOT divide
    int state = radio.begin(freqMHz, bwKHz, LORA_SF, LORA_CR,
                            LORA_SYNC_WORD, LORA_TX_POWER, 8, 0);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] Init FAILED: %d (freq=%.3fMHz bw=%.2fkHz) — check module wiring! "
                      "(queue created, LoRa disabled, rest of firmware unaffected)\n",
                      state, freqMHz, bwKHz);
        return;
    }
    Serial.printf("[LORA] Client '%s' ready @ %.0f MHz  SF=%d BW=%.0fk  (Core 0)\n",
                  CLIENT_ID, freqMHz, LORA_SF, bwKHz);

    attachInterrupt(digitalPinToInterrupt(LORA_DIO0), onDio0, RISING);
    radio.startReceive();
    xTaskCreatePinnedToCore(loraTask, "lora_rx", 5120, nullptr, 2, nullptr, 0);
    loraReady = true;
}

bool lora_isReady() { return loraReady; }

// Called from Core 1 loop() — drains the GONG/STOP command queue.
void lora_poll() {
    if (!rxQueue) return;
    RxCmd cmd;
    while (xQueueReceive(rxQueue, &cmd, 0) == pdTRUE) {
        if (cmd.type == MSG_GONG) {
            Serial.printf("[LORA] GONG → track=%d vol=%d loop=%d RSSI=%d\n",
                          cmd.track, cmd.vol, cmd.loop, cmd.rssi);
            if (STATUS_LED >= 0) digitalWrite(STATUS_LED, HIGH);
            mp3_setVolume(cmd.vol);
            mp3_play(cmd.track, cmd.loop);
        } else if (cmd.type == MSG_STOP) {
            Serial.println("[LORA] STOP → stopping audio");
            mp3_stop();
            if (STATUS_LED >= 0) digitalWrite(STATUS_LED, LOW);
        }
    }
}
