#include "lorahandler.h"
#include "config.h"
#include "mp3handler.h"
#include <SPI.h>
#include <RadioLib.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_system.h>
#include <Preferences.h>

static Module mod(LORA_SS, LORA_DIO0, LORA_RST, RADIOLIB_NC);
static SX1278 radio(&mod);

static volatile bool dioFlag = false;
static void IRAM_ATTR onDio0() { dioFlag = true; }

static volatile bool loraReady = false;

// ── Client ID — H-6: default used to be a hardcoded string ("client_twoX")
// that was easy to forget to change on a second device, silently merging two
// clients into one entry on the server. Derive a unique ID from the MAC
// address unless the operator opted into a human-readable name.
static String g_clientId;

static String resolveClientId() {
#ifdef CLIENT_ID_OVERRIDE
    return String(CLIENT_ID_OVERRIDE);
#else
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char b[16];
    snprintf(b, sizeof(b), "cli_%02X%02X%02X", mac[3], mac[4], mac[5]);
    return String(b);
#endif
}

const char* lora_clientId() { return g_clientId.c_str(); }

static uint16_t ackSlot() {
    // FNV-1a over the client ID — deterministic, so every client picks a
    // different slot without ever having to coordinate with the others.
    uint32_t h = 2166136261u;
    for (const char* p = g_clientId.c_str(); *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
    return (uint16_t)(h % ACK_SLOT_COUNT);
}

// ── Replay protection (C-2) ───────────────────────────────────────────────
// The server's nowTs() is guaranteed monotonic across reboots (NVS
// water-mark, see server lorahandler.cpp ts_setup()/nowTs()), so a strict
// `ts <= lastServerTs` rejection is both correct and simple. lastServerTs is
// persisted in NVS so a client reboot doesn't reset protection to "accept
// anything".
static Preferences rpPrefs;
static uint32_t    lastServerTs = 0;
// code_review.md C2: sub-second tie-breaker within the same "ts". Not
// persisted — after a reboot "ts" alone is already guaranteed to jump
// strictly forward past anything seen before (server-side tsBase water-mark),
// so the very first post-reboot frame passes on "ts" regardless of "n"
// starting back at 0. See lora_shared.h / server lorahandler.cpp nowTs().
static uint32_t lastServerN  = 0;

static void replay_setup() {
    rpPrefs.begin("gong", false);
    lastServerTs = rpPrefs.getUInt("lastts", 0);
}

static bool checkReplay(uint32_t ts, uint32_t n) {
    if (ts == 0) { Serial.println("[LORA] No ts — rejected"); return false; }
    bool newer = (ts > lastServerTs) || (ts == lastServerTs && n > lastServerN);
    if (!newer) {
        Serial.printf("[LORA] Replay/dup ts=%u n=%u last=%u/%u — rejected\n", ts, n, lastServerTs, lastServerN);
        return false;
    }
    lastServerTs = ts;
    lastServerN  = n;
    static uint32_t lastWrite = 0;
    if (ts > lastWrite + 60) { lastWrite = ts; rpPrefs.putUInt("lastts", ts); }
    return true;
}

// H-3/M-5/M-6: frame is [type][8-byte HMAC tag][payload], tag at a fixed
// offset, computed directly over the raw payload bytes — no JSON
// re-serialization involved, so nothing can make the check drift from what
// was actually signed. `buf`/`len` are the full received frame (type byte
// included); on success `payload`/`plen` point at the bytes after the tag.
static bool verifyFrame(uint8_t type, const uint8_t* buf, size_t len,
                         const uint8_t*& payload, size_t& plen) {
    if (len < 1 + LORA_TAG_LEN) {
        Serial.printf("[LORA] Short frame type=0x%02X len=%u\n", type, (unsigned)len);
        return false;
    }
    payload = buf + 1 + LORA_TAG_LEN;
    plen    = len - 1 - LORA_TAG_LEN;
    uint8_t expected[LORA_TAG_LEN];
    lora_hmacTag(LORA_HMAC_KEY, type, payload, plen, expected);
    if (!lora_tagEqual(buf + 1, expected)) {
        Serial.printf("[LORA] Bad sig on 0x%02X — rejected\n", type);
        return false;
    }
    return true;
}

// ── H-5: virtual clock, kept in sync from the "time" field of every
// heartbeat. The client has no RTC at all — this is only ever as accurate as
// the last heartbeat it saw, which is exactly why it's only trusted for the
// autonomous fallback schedule, not presented as real time anywhere.
static uint32_t vclockAnchorMs  = 0;
static int32_t  vclockAnchorSec = -1;   // seconds-of-day at the anchor; -1 = never synced

static void syncVirtualClock(const String& hhmmss) {
    if (hhmmss.length() != 8 || hhmmss == "--:--:--") return;   // server itself has no valid time
    int hh = hhmmss.substring(0, 2).toInt();
    int mm = hhmmss.substring(3, 5).toInt();
    int ss = hhmmss.substring(6, 8).toInt();
    vclockAnchorSec = hh * 3600 + mm * 60 + ss;
    vclockAnchorMs  = millis();
}

static int32_t virtualSecOfDay() {
    if (vclockAnchorSec < 0) return -1;
    uint32_t elapsedS = (millis() - vclockAnchorMs) / 1000;
    return (int32_t)((vclockAnchorSec + elapsedS) % 86400);
}

// ── H-5: autonomous fallback schedule, received via MSG_SCHEDULE and kept in
// NVS. A dead server no longer means a silent gong for the rest of the day —
// see 01_AUDIT_REPORT.md H-5, the single worst operational risk flagged.
static Preferences schedPrefs;
static uint8_t     g_schedDay   = 0xFF;
static uint8_t     g_schedCount = 0;
static SchedBin     g_sched[SCHED_BIN_MAX];

static void loadStoredSchedule() {
    schedPrefs.begin("gong", true);
    g_schedDay   = schedPrefs.getUChar("schday", 0xFF);
    g_schedCount = schedPrefs.getUChar("schcnt", 0);
    if (g_schedCount > SCHED_BIN_MAX) g_schedCount = SCHED_BIN_MAX;
    if (g_schedCount) schedPrefs.getBytes("schdata", g_sched, (size_t)g_schedCount * sizeof(SchedBin));
    schedPrefs.end();
    if (g_schedCount)
        Serial.printf("[LORA] Loaded stored schedule: day=%02d entries=%u\n", (int)g_schedDay, (unsigned)g_schedCount);
}

static void handleScheduleFrame(const uint8_t* p, size_t plen) {
    if (plen < sizeof(SchedBinHeader)) return;
    SchedBinHeader hdr;
    memcpy(&hdr, p, sizeof(hdr));
    uint8_t cnt = hdr.count;
    if (cnt > SCHED_BIN_MAX) cnt = SCHED_BIN_MAX;
    size_t need = sizeof(hdr) + (size_t)cnt * sizeof(SchedBin);
    if (plen < need) return;

    g_schedDay   = hdr.day;
    g_schedCount = cnt;
    if (cnt) memcpy(g_sched, p + sizeof(hdr), (size_t)cnt * sizeof(SchedBin));

    schedPrefs.begin("gong", false);
    schedPrefs.putUChar("schday", g_schedDay);
    schedPrefs.putUChar("schcnt", g_schedCount);
    if (g_schedCount) schedPrefs.putBytes("schdata", g_sched, (size_t)g_schedCount * sizeof(SchedBin));
    schedPrefs.end();

    Serial.printf("[LORA] Schedule stored: day=%02d entries=%u\n", (int)g_schedDay, (unsigned)g_schedCount);
}

static volatile uint32_t lastHeartbeatMs = 0;

bool lora_heartbeatLost()      { return millis() - lastHeartbeatMs >= HEARTBEAT_LOST_MS; }
uint32_t lora_msSinceHeartbeat() { return millis() - lastHeartbeatMs; }

// H-5: called once a second from loop(). Mirrors the server's sched_check()
// anti-double-fire pattern (one trigger per minute).
static int      autoLastFiredKey = -1;
static uint32_t autoLastFiredMs  = 0;

void lora_autonomousTick() {
    if (!lora_heartbeatLost()) return;               // server alive — stay slave, do nothing
    if (g_schedCount == 0 || g_schedDay == 0xFF) return;  // nothing to fall back to
    int32_t sec = virtualSecOfDay();
    if (sec < 0) return;                              // never got a valid time from the server

    int key = sec / 60;
    if (autoLastFiredKey == key && millis() - autoLastFiredMs < 65000UL) return;

    int hh = sec / 3600, mm = (sec % 3600) / 60;
    for (uint8_t i = 0; i < g_schedCount; i++) {
        if (!schedbin_enabled(g_sched[i].loopEn)) continue;
        if (g_sched[i].hour != hh || g_sched[i].minute != mm) continue;

        uint8_t loopCount = schedbin_loop(g_sched[i].loopEn);
        Serial.printf("[LORA] AUTONOMOUS gong (server silent %lus): track=%d vol=%d loop=%d\n",
                      (unsigned long)(lora_msSinceHeartbeat() / 1000),
                      g_sched[i].track, g_sched[i].vol, loopCount);
        if (STATUS_LED >= 0) digitalWrite(STATUS_LED, HIGH);
        mp3_setVolume(g_sched[i].vol);
        mp3_play(g_sched[i].track, loopCount);
        autoLastFiredKey = key;
        autoLastFiredMs  = millis();
        break;
    }
}

// ── Command queue: Core 0 → Core 1 (audio calls stay off the radio task) ──
struct RxCmd {
    uint8_t type;
    uint8_t track, vol, loop;
    int     rssi;
};
static QueueHandle_t rxQueue = nullptr;

// ── ACK — always sent synchronously from Core 0, inside loraTask. Only ever
// sent for MSG_HEARTBEAT now (C-3 measure 2: GONG never gets one — the
// server already ignores it for stats, and holding the radio for ~140ms
// right when a STOP might follow is a window we can't afford to be deaf in).
static void sendAck(int rxRssi, uint32_t hbSeq) {
    DynamicJsonDocument doc(128);
    doc["id"]   = g_clientId;
    doc["rssi"] = rxRssi;
    if (hbSeq != 0) doc["hb"] = hbSeq;
    String payload;
    serializeJson(doc, payload);

    // C-3 measure 3: deterministic per-client slot instead of a 0-70ms random
    // jitter — that window was 4x shorter than one ACK's airtime at the old
    // SF9, guaranteeing collisions with 2+ clients. At SF7 + a slot per
    // client, two clients' ACKs can no longer land on top of each other.
    vTaskDelay(pdMS_TO_TICKS(ACK_GUARD_MS + ackSlot() * ACK_SLOT_MS));

    // H-2: a packet may have arrived while this client waited for its slot
    // (e.g. STOP right after a GONG). Blindly clearing dioFlag and
    // transmitting would silently destroy it. Skip the ACK and let loraTask's
    // next iteration handle the pending packet — the server sees this client
    // again on the next heartbeat regardless.
    if (dioFlag) {
        Serial.println("[LORA] RX pending during ACK slot — skipping ACK, handling packet first");
        return;
    }

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

// ── M-8: radio init / self-heal ───────────────────────────────────────────
static uint32_t lastRadioOk = 0;

static bool radioInit() {
    float freqMHz = (float)LORA_FREQ;   // already MHz — do NOT divide
    float bwKHz   = (float)LORA_BW;     // already kHz — do NOT divide
    int state = radio.begin(freqMHz, bwKHz, LORA_SF, LORA_CR,
                            LORA_SYNC_WORD, LORA_TX_POWER, 8, 0);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA] Init FAILED: %d (freq=%.3fMHz bw=%.2fkHz) — check module wiring!\n",
                      state, freqMHz, bwKHz);
        loraReady = false;
        return false;
    }
    dioFlag = false;
    radio.startReceive();
    lastRadioOk = millis();
    loraReady   = true;
    Serial.printf("[LORA] Client '%s' ready @ %.0f MHz  SF=%d BW=%.0fk  (Core 0)\n",
                  g_clientId.c_str(), freqMHz, LORA_SF, bwKHz);
    return true;
}

static const uint32_t RADIO_SILENCE_MS = 300000UL;  // M-8: 5 min with no successful RX → reinit
static const uint32_t RADIO_RETRY_MS   = 30000UL;   // M-8: retry a failed init this often

// ── Core 0: LoRa task — RX + ACK; audio dispatched to Core 1 via queue ───
static void loraTask(void*) {
    uint32_t lastInitAttempt = 0;

    for (;;) {
        if (!loraReady) {
            if (millis() - lastInitAttempt >= RADIO_RETRY_MS) {
                lastInitAttempt = millis();
                radioInit();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (millis() - lastRadioOk > RADIO_SILENCE_MS) {
            Serial.println("[LORA] No activity 5 min — reinitialising radio");
            radio.reset();
            radioInit();
            continue;
        }

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
        lastRadioOk = millis();

        uint8_t type = buf[0];
        int     rssi = (int)radio.getRSSI();

        if (type == MSG_GONG || type == MSG_HEARTBEAT || type == MSG_STOP || type == MSG_SCHEDULE) {
            const uint8_t* payload; size_t plen;
            if (!verifyFrame(type, buf, len, payload, plen)) { radio.startReceive(); continue; }

            if (type == MSG_SCHEDULE) {
                if (plen < 8) { radio.startReceive(); continue; }
                uint32_t ts, n; memcpy(&ts, payload, 4); memcpy(&n, payload + 4, 4);
                if (!checkReplay(ts, n)) { radio.startReceive(); continue; }
                handleScheduleFrame(payload + 8, plen - 8);
                radio.startReceive();
                continue;
            }

            DynamicJsonDocument doc(512);
            if (deserializeJson(doc, payload, plen)) { radio.startReceive(); continue; }
            uint32_t ts = doc["ts"] | 0;
            uint32_t n  = doc["n"]  | 0;
            if (!checkReplay(ts, n)) { radio.startReceive(); continue; }

            if (type == MSG_HEARTBEAT) {
                lastHeartbeatMs = millis();
                syncVirtualClock(doc["time"] | "");
                uint32_t seq = doc["seq"] | 0;
                sendAck(rssi, seq);   // startReceive() called inside sendAck
                continue;
            }

            if (type == MSG_GONG) {
                RxCmd cmd = {};
                cmd.type  = type;
                cmd.rssi  = rssi;
                cmd.track = doc["track"] | 1;
                cmd.vol   = doc["vol"]   | DEFAULT_VOLUME;
                cmd.loop  = doc["loop"]  | 1;
                // Queue the command BEFORE returning to RX: lora_poll() on
                // Core 1 starts the audio immediately, while Core 0 goes
                // straight back to listening — no ACK, no delay (see above).
                xQueueSend(rxQueue, &cmd, 0);
                radio.startReceive();
                continue;
            }

            if (type == MSG_STOP) {
                RxCmd cmd = {};
                cmd.type = type;
                cmd.rssi = rssi;
                radio.startReceive();
                xQueueSend(rxQueue, &cmd, 0);
                continue;
            }
        }

        Serial.printf("[LORA] Unknown type 0x%02X\n", type);
        radio.startReceive();
    }
}

// ────────────────────────────────────────────────────────────────────────
void lora_setup() {
    g_clientId      = resolveClientId();
    lastHeartbeatMs = millis();   // grace period at boot — not instantly "lost"
    replay_setup();
    loadStoredSchedule();

    // RTOS primitives FIRST — must exist even if the radio isn't wired up.
    rxQueue = xQueueCreate(4, sizeof(RxCmd));

    SPI.begin(LORA_SPI_SCK, LORA_SPI_MISO, LORA_SPI_MOSI, LORA_SS);
    attachInterrupt(digitalPinToInterrupt(LORA_DIO0), onDio0, RISING);

    if (strcmp(LORA_HMAC_KEY, "change_me_before_deploy_32chars!") == 0)
        Serial.println("[SEC] !!! LORA_HMAC_KEY IS THE REPO DEFAULT — ANYONE WITH AN Ra-02 AND "
                        "THIS REPO CAN RING THE GONG. CHANGE IT IN config.h BEFORE DEPLOYMENT !!!");

    radioInit();   // failure is not fatal — loraTask retries every 30s (M-8)
    xTaskCreatePinnedToCore(loraTask, "lora_rx", 5120, nullptr, 2, nullptr, 0);
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
