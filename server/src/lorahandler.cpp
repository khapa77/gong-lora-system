#include "lorahandler.h"
#include "config.h"
#include <SPI.h>
#include <RadioLib.h>
#include <ArduinoJson.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <Preferences.h>

static Module mod(LORA_SS, LORA_DIO0, LORA_RST, RADIOLIB_NC);
static SX1278 radio(&mod);

// ── DIO0 interrupt — fires on both TX-done and RX-done (RISING) ──────────
static volatile bool dioFlag = false;
static void IRAM_ATTR onDio0() { dioFlag = true; }

// ── TX queue (Core 1 → Core 0) ────────────────────────────────────────────
// H-1: playLocal/track/vol/loop let the TX-done handler queue a local play
// once airtime is actually spent, so lora_sendGong() itself never blocks.
struct TxReq {
    uint8_t  type;
    uint8_t  buf[1 + LORA_PAYLOAD_MAX];
    size_t   len;
    bool     playLocal;
    uint8_t  track, vol, loop;
};
struct LocalPlay { uint8_t track, vol, loop; };

static QueueHandle_t     txQueue        = nullptr;
static QueueHandle_t     localPlayQueue = nullptr;   // Core 0 → Core 1 (H-1)
static volatile bool     loraReady      = false;     // true once radio is initialised and loraTask can TX/RX

// ── Client registry + mutex ────────────────────────────────────────────────
#define MAX_CLIENTS 16

struct ClientInfo {
    String   id;
    int      rssi;
    float    snr;
    uint32_t lastSeenMs;
    uint32_t respMs;   // time from last heartbeat's TX-done to this ACK — diagnostic only, NOT distance (see C-4)
};

static ClientInfo        clients[MAX_CLIENTS];
static uint8_t           cliCount   = 0;
static SemaphoreHandle_t clientsMtx = nullptr;

// ── Heartbeat bookkeeping ─────────────────────────────────────────────────
// respMs is valid ONLY for an ACK that echoes the seq of the LAST heartbeat
// ("hb" field), measured from the moment that heartbeat's TX actually
// finished (DIO0 TX-done), not from when it was queued. ACKs to GONG carry
// no "hb" field and never touch this figure.
static volatile uint32_t hbSeq        = 0;   // seq of the last heartbeat sent
static volatile uint32_t hbTxDoneMs   = 0;   // millis() at that heartbeat's TX-done
static volatile uint32_t ackWindowUntil = 0; // C-3: server stays silent (except GONG/STOP) while clients' ACK slots play out

static void upsertClient(const String& id, int rssi, float snr, uint32_t respMs) {
    unsigned long now = millis();

    for (uint8_t i = 0; i < cliCount; i++) {
        if (clients[i].id == id) {
            clients[i].rssi       = rssi;
            clients[i].snr        = snr;
            clients[i].lastSeenMs = now;
            if (respMs > 0) clients[i].respMs = respMs;
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
            logPrintf("[LORA] Evicting stale client: %s\n", clients[oldest].id.c_str());
            clients[oldest] = { id, rssi, snr, now, respMs };
            return;
        }
        logPrintf("[LORA] MAX_CLIENTS reached, new client ignored\n");
        return;
    }

    clients[cliCount++] = { id, rssi, snr, now, respMs };
    logPrintf("[LORA] New client registered: %s\n", id.c_str());
}

// ── C-2: monotonic ts ──────────────────────────────────────────────────────
// The old nowTs() fell back to millis()/1000 whenever system time wasn't set
// — which is NOT monotonic across a reboot: uptime restarts at 0, jumps
// backwards relative to whatever the client last saw, and the client's
// anti-replay guard then rejects every packet until uptime climbs back past
// the old value (up to tens of minutes of total silence after every server
// reboot). tsBase, persisted in NVS with a safety margin, guarantees nowTs()
// never goes backwards — regardless of reboots or RTC/NTP availability.
// See 01_AUDIT_REPORT.md C-2.
static Preferences tsPrefs;
static uint32_t    tsBase      = 0;   // NVS-persisted floor for the uptime-based fallback
static uint32_t    tsPersisted = 0;   // last value actually written to NVS

static void ts_setup() {
    tsPrefs.begin("gong", false);
    tsBase      = tsPrefs.getUInt("tsbase", 0);
    tsPersisted = tsBase;
}

static uint32_t nowTs() {
    uint32_t real = (uint32_t)time(nullptr);
    uint32_t up   = tsBase + (uint32_t)(millis() / 1000);
    uint32_t v    = (real > TIME_VALID_EPOCH && real > up) ? real : up;

    // Persist a "water mark" 120 s ahead of the highest value seen so far, at
    // most once a minute, so an unrecorded window between writes still can't
    // cause a backward jump on the next boot.
    if (v > tsPersisted + 60) {
        tsPersisted = v;
        tsPrefs.putUInt("tsbase", v + 120);
    }
    return v;
}

// ── loraSend — enqueue a TX request from Core 1. Returns false if it never
// made it onto the queue (radio not ready / queue full).
// H-3/M-5/M-6: the frame is [type][8-byte HMAC tag][payload] — the tag sits
// at a fixed offset and is computed directly over the raw payload bytes that
// go on the air, so there's no string surgery to append a "sig" field and no
// re-serialization for the receiver to depend on.
static bool loraSendRaw(uint8_t type, const uint8_t* payload, size_t plen,
                         bool playLocal, uint8_t track, uint8_t vol, uint8_t loop) {
    if (plen > (size_t)(LORA_PAYLOAD_MAX - LORA_TAG_LEN)) {
        logPrintf("[LORA] TX rejected: payload %u > %u bytes type=0x%02X\n",
                  (unsigned)plen, (unsigned)(LORA_PAYLOAD_MAX - LORA_TAG_LEN), type);
        return false;
    }

    TxReq req{};
    req.type   = type;
    req.buf[0] = type;
    uint8_t tag[LORA_TAG_LEN];
    lora_hmacTag(LORA_HMAC_KEY, type, payload, plen, tag);
    memcpy(req.buf + 1, tag, LORA_TAG_LEN);
    if (plen) memcpy(req.buf + 1 + LORA_TAG_LEN, payload, plen);
    req.len       = 1 + LORA_TAG_LEN + plen;
    req.playLocal = playLocal;
    req.track     = track;
    req.vol       = vol;
    req.loop      = loop;

    if (!loraReady || xQueueSend(txQueue, &req, pdMS_TO_TICKS(200)) != pdTRUE) {
        logPrintf("[LORA] TX not sent (not ready or queue full) type=0x%02X\n", type);
        return false;
    }
    return true;
}

static bool loraSend(uint8_t type, const String& payload,
                      bool playLocal = false, uint8_t track = 0, uint8_t vol = 0, uint8_t loop = 0) {
    return loraSendRaw(type, (const uint8_t*)payload.c_str(), payload.length(), playLocal, track, vol, loop);
}

// ── M-8: radio init / self-heal ───────────────────────────────────────────
static uint32_t lastRadioOk = 0;

static bool radioInit() {
    float freqMHz = (float)LORA_FREQ;   // already MHz — do NOT divide
    float bwKHz   = (float)LORA_BW;     // already kHz — do NOT divide
    int state = radio.begin(freqMHz, bwKHz, LORA_SF, LORA_CR,
                            LORA_SYNC_WORD, LORA_TX_POWER, 8, 0);
    if (state != RADIOLIB_ERR_NONE) {
        logPrintf("[LORA] Init FAILED: %d (freq=%.3fMHz bw=%.2fkHz) — check module wiring!\n",
                  state, freqMHz, bwKHz);
        loraReady = false;
        return false;
    }
    dioFlag = false;
    radio.startReceive();
    lastRadioOk = millis();
    loraReady   = true;
    logPrintf("[LORA] Server ready @ %.0f MHz  SF=%d BW=%.0fk  (Core 0)\n", freqMHz, LORA_SF, bwKHz);
    return true;
}

// ── Core 0: LoRa task — non-blocking TX state machine + RX dispatch ──────
// If DIO0 never fires (bad wiring, module fault, RF issue), a TX must not
// wedge the radio task forever — that would silently kill ALL future TX/RX
// (heartbeats, gongs, client replies) with no further symptom in the logs.
static const uint32_t TX_TIMEOUT_MS       = 4000;     // generous margin over SF7 airtime (~140ms)
static const uint32_t RADIO_SILENCE_MS    = 300000UL; // M-8: 5 min with no successful TX/RX → reinit
static const uint32_t RADIO_RETRY_MS      = 30000UL;  // M-8: retry a failed init this often

static void loraTask(void*) {
    bool     txBusy      = false;
    uint8_t  txType       = 0;
    uint32_t txStart      = 0;
    bool     txPlayLocal  = false;
    uint8_t  txTrack = 0, txVol = 0, txLoop = 0;
    uint32_t lastInitAttempt = 0;

    for (;;) {
        // ── Not initialised (or lost) — retry periodically, do nothing else ──
        if (!loraReady) {
            if (millis() - lastInitAttempt >= RADIO_RETRY_MS) {
                lastInitAttempt = millis();
                radioInit();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // ── TX in flight: wait for DIO0 (TX-done), or time out ──────────
        if (txBusy) {
            if (dioFlag) {
                dioFlag = false;
                lastRadioOk = millis();   // DIO0 fired — chip is alive regardless of finishTransmit's result
                int st = radio.finishTransmit();
                txBusy = false;
                if (st != RADIOLIB_ERR_NONE)
                    logPrintf("[LORA] TX finish error: %d\n", st);
                else
                    logPrintf("[LORA] TX done type=0x%02X\n", txType);
                if (st == RADIOLIB_ERR_NONE && txType == MSG_HEARTBEAT) {
                    hbTxDoneMs     = millis();   // resp_ms reference point (see above)
                    ackWindowUntil = millis() + ACK_WINDOW_MS;
                }
                if (txType == MSG_GONG && txPlayLocal) {
                    LocalPlay lp = { txTrack, txVol, txLoop };
                    xQueueSend(localPlayQueue, &lp, 0);
                }
                radio.startReceive();
            } else if (millis() - txStart > TX_TIMEOUT_MS) {
                logPrintf("[LORA] TX timeout (no DIO0) type=0x%02X — check DIO0 wiring! Recovering.\n", txType);
                txBusy = false;
                if (txType == MSG_GONG && txPlayLocal) {
                    LocalPlay lp = { txTrack, txVol, txLoop };
                    xQueueSend(localPlayQueue, &lp, 0);
                }
                radio.standby();
                radio.startReceive();
            }
            vTaskDelay(1);
            continue;
        }

        // ── M-8: no successful TX/RX in a long time — reinit the module ──
        if (millis() - lastRadioOk > RADIO_SILENCE_MS) {
            logPrintf("[LORA] No activity %lu s — reinitialising radio\n", (unsigned long)(RADIO_SILENCE_MS / 1000));
            radio.reset();
            radioInit();   // sets loraReady accordingly; on failure the top-of-loop retry takes over
            continue;
        }

        // ── C-3: stay off the air (except GONG/STOP) while clients' ACK
        // slots are still playing out after the last heartbeat ────────────
        TxReq req;
        if (xQueuePeek(txQueue, &req, 0) == pdTRUE) {
            bool mustWait = (millis() < ackWindowUntil) && req.type != MSG_GONG && req.type != MSG_STOP;
            if (!mustWait) {
                xQueueReceive(txQueue, &req, 0);
                dioFlag = false;  // clear any stale RX flag before TX
                txType  = req.type;
                int st  = radio.startTransmit(req.buf, req.len);
                if (st != RADIOLIB_ERR_NONE) {
                    logPrintf("[LORA] TX start error: %d\n", st);
                    if (req.type == MSG_GONG && req.playLocal) {
                        LocalPlay lp = { req.track, req.vol, req.loop };
                        xQueueSend(localPlayQueue, &lp, 0);
                    }
                    radio.startReceive();
                } else {
                    txBusy      = true;
                    txStart     = millis();
                    txPlayLocal = req.playLocal;
                    txTrack = req.track; txVol = req.vol; txLoop = req.loop;
                    logPrintf("[LORA] TX started type=0x%02X len=%u\n", req.type, (unsigned)req.len);
                }
            }
            vTaskDelay(1);
            continue;
        }

        // ── RX: check for a received packet ────────────────────────────
        if (!dioFlag) {
            vTaskDelay(1);
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

        uint8_t msgType = buf[0];
        int     rssi    = (int)radio.getRSSI();

        if (msgType == MSG_ACK) {
            // ACK is not signed (server doesn't act on it beyond bookkeeping) —
            // still framed as [type][json], just no HMAC tag prefix.
            buf[len] = '\0';   // M-17: one conversion instead of 256 String reallocations
            String payload((const char*)(buf + 1));
            DynamicJsonDocument doc(256);
            if (!deserializeJson(doc, payload)) {
                String id  = doc["id"] | "unknown";
                // resp_ms only when the ACK echoes the seq of the LAST heartbeat;
                // ACKs to GONG (no "hb") pass 0 → registry keeps old value.
                uint32_t hb   = doc["hb"] | 0;
                uint32_t resp = (hb != 0 && hb == hbSeq && hbTxDoneMs != 0)
                               ? (uint32_t)(millis() - hbTxDoneMs) : 0;
                float snr = radio.getSNR();
                xSemaphoreTake(clientsMtx, portMAX_DELAY);
                upsertClient(id, rssi, snr, resp);
                xSemaphoreGive(clientsMtx);
                logPrintf("[LORA] ACK from '%s' RSSI=%d SNR=%.1f\n", id.c_str(), rssi, snr);
            }
        } else {
            logPrintf("[LORA] RX unhandled type=0x%02X len=%u RSSI=%d\n",
                      msgType, (unsigned)(len - 1), rssi);
        }

        radio.startReceive();
    }
}

// ────────────────────────────────────────────────────────────────────────
void lora_setup() {
    ts_setup();

    // RTOS primitives FIRST — must exist even if the radio isn't wired up.
    txQueue        = xQueueCreate(8, sizeof(TxReq));
    localPlayQueue = xQueueCreate(4, sizeof(LocalPlay));
    clientsMtx     = xSemaphoreCreateMutex();

    SPI.begin(LORA_SPI_SCK, LORA_SPI_MISO, LORA_SPI_MOSI, LORA_SS);
    attachInterrupt(digitalPinToInterrupt(LORA_DIO0), onDio0, RISING);

    radioInit();   // failure is not fatal — loraTask retries every 30s (M-8)
    xTaskCreatePinnedToCore(loraTask, "lora", 6144, nullptr, 2, nullptr, 0);
}

bool lora_isReady() { return loraReady; }

// H-7: the repo ships LORA_HMAC_KEY with a placeholder value so the project
// builds out of the box — but if nobody changes it before deployment, anyone
// with an Ra-02 and this repo can sign valid GONG/HEARTBEAT/STOP frames.
// Can now be overridden at build time (see config.h); this is the runtime
// "at least warn loudly" floor for anyone who didn't.
bool lora_usesDefaultKey() {
    return strcmp(LORA_HMAC_KEY, "change_me_before_deploy_32chars!") == 0;
}

// ────────────────────────────────────────────────────────────────────────
// H-1: non-blocking. Local playback (if requested) happens from
// lora_pollLocalPlay() once the TX-done handler in loraTask queues it —
// there is no more waiting here for airtime (~140ms at SF7, up to 4s on a
// TX timeout) before this function returns.
void lora_sendGong(uint8_t track, uint8_t vol, uint8_t loop, bool playLocal) {
    DynamicJsonDocument doc(128);
    doc["track"] = track;
    doc["vol"]   = vol;
    doc["loop"]  = loop;
    doc["ts"]    = nowTs();
    String s;
    serializeJson(doc, s);

    if (!loraSend(MSG_GONG, s, playLocal, track, vol, loop)) {
        logPrintf("[LORA] GONG not broadcast\n");
        if (playLocal) {
            // Radio path failed outright (not ready / queue full) — the
            // TX-done handler that would normally queue this never runs.
            LocalPlay lp = { track, vol, loop };
            xQueueSend(localPlayQueue, &lp, 0);
        }
    }
}

void lora_sendStop() {
    DynamicJsonDocument doc(64);
    doc["ts"] = nowTs();
    String s;
    serializeJson(doc, s);
    loraSend(MSG_STOP, s);
}

// M-7: returns false if the heartbeat couldn't even be queued, so the caller
// can retry soon instead of silently waiting a full HEARTBEAT_INTERVAL_MS —
// during which the client-side registry can time a client out.
bool lora_sendHeartbeat() {
    if (uxQueueMessagesWaiting(txQueue) > 0) return false;  // higher-priority TX pending

    DynamicJsonDocument doc(128);
    struct tm ti;
    if (localNow(ti)) {
        char buf[9];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
        doc["time"] = buf;
    } else {
        doc["time"] = "--:--:--";
    }
    doc["clients"] = lora_clientCount();
    doc["ts"]      = nowTs();
    doc["seq"]     = ++hbSeq;   // clients echo this back as "hb" in their ACK
    hbTxDoneMs     = 0;         // invalid until THIS heartbeat's TX-done fires;
                                // otherwise, after a TX timeout, an ACK to the
                                // new seq would be measured from the PREVIOUS
                                // heartbeat's timestamp
    String s;
    serializeJson(doc, s);
    return loraSend(MSG_HEARTBEAT, s);
}

// H-5: binary, signed broadcast of the active day's schedule — clients store
// it in NVS and fall back to it if the server goes silent for too long (see
// client/src/lorahandler.cpp). Wire payload: [4B ts][SchedBinHeader][SchedBin...].
void lora_broadcastSchedule(uint8_t day, const SchedBin* entries, uint8_t count) {
    if (count > SCHED_BIN_MAX) count = SCHED_BIN_MAX;
    uint8_t payload[4 + sizeof(SchedBinHeader) + SCHED_BIN_MAX * sizeof(SchedBin)];
    size_t  off = 0;
    uint32_t ts = nowTs();
    memcpy(payload + off, &ts, sizeof(ts)); off += sizeof(ts);
    SchedBinHeader hdr = { day, count };
    memcpy(payload + off, &hdr, sizeof(hdr)); off += sizeof(hdr);
    if (count) { memcpy(payload + off, entries, (size_t)count * sizeof(SchedBin)); off += (size_t)count * sizeof(SchedBin); }

    if (loraSendRaw(MSG_SCHEDULE, payload, off, false, 0, 0, 0))
        logPrintf("[LORA] Schedule broadcast: day=%02d entries=%u\n", (int)day, (unsigned)count);
    else
        logPrintf("[LORA] Schedule broadcast failed (radio not ready or queue full)\n");
}

// H-1: called every loop() from Core 1 (see main.cpp).
bool lora_pollLocalPlay(uint8_t& track, uint8_t& vol, uint8_t& loop) {
    if (!localPlayQueue) return false;
    LocalPlay lp;
    if (xQueueReceive(localPlayQueue, &lp, 0) != pdTRUE) return false;
    track = lp.track; vol = lp.vol; loop = lp.loop;
    return true;
}

// ────────────────────────────────────────────────────────────────────────
String lora_clientsJSON() {
    xSemaphoreTake(clientsMtx, portMAX_DELAY);
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.to<JsonArray>();
    unsigned long now = millis();
    for (uint8_t i = 0; i < cliCount; i++) {
        unsigned long age = now - clients[i].lastSeenMs;
        if (age > CLIENT_TIMEOUT_MS) continue;
        JsonObject o = arr.createNestedObject();
        o["id"]      = clients[i].id;
        o["rssi"]    = clients[i].rssi;
        o["snr"]     = clients[i].snr;
        o["seen_ms"] = age;
        o["resp_ms"] = clients[i].respMs;
    }
    String s;
    serializeJson(doc, s);
    xSemaphoreGive(clientsMtx);
    return s;
}

int lora_clientCount() {
    xSemaphoreTake(clientsMtx, portMAX_DELAY);
    unsigned long now = millis();
    int active = 0;
    for (uint8_t i = 0; i < cliCount; i++) {
        if (now - clients[i].lastSeenMs <= CLIENT_TIMEOUT_MS) active++;
    }
    xSemaphoreGive(clientsMtx);
    return active;
}
