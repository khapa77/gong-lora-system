#include <Arduino.h>
#include <LittleFS.h>
#include <esp_core_dump.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "mp3handler.h"
#include "schedule.h"
#include "webhandler.h"
#include "rtchandler.h"
#include "lorahandler.h"

static unsigned long lastSchedCheck     = 0;
static unsigned long lastHeartbeat      = 0;
static unsigned long lastSchedBroadcast = 0;

// -------------------------------------------------------
// Called when a schedule entry fires — broadcasts over LoRa (non-blocking,
// H-1) with playLocal=true, so the local speaker plays once TX actually
// completes (see lora_pollLocalPlay() in loop() below). Previously this
// blocked Core 1 for ~140ms-4s waiting for airtime before starting local
// playback; both sides now start together because of when the TX-done
// interrupt fires, not because anyone waited for it.
// -------------------------------------------------------
static void onGongFire(uint8_t track, uint8_t loop, uint8_t vol) {
    logPrintf("[MAIN] Schedule fired: track=%d loop=%d vol=%d\n", track, loop, vol);
    lora_sendGong(track, vol, loop, /*playLocal=*/true);
}

void setup() {
    Serial.begin(115200);
    delay(500);

    logbuffer_init();  // capture logs for web debug (must precede logPrintf below)

    // If the previous run crashed, a core dump sits in the coredump partition.
    // Log its presence BEFORE erasing — the old code erased it as the very
    // first action, destroying the only evidence of the crash and making the
    // 128 KB coredump partition useless for diagnostics.
    {
        size_t cdAddr = 0, cdSize = 0;
        if (esp_core_dump_image_get(&cdAddr, &cdSize) == ESP_OK && cdSize > 0) {
            logPrintf("[MAIN] Previous run CRASHED — core dump found (%u bytes @0x%X), erasing. "
                      "To keep dumps for offline analysis, remove the erase below.\n",
                      (unsigned)cdSize, (unsigned)cdAddr);
        }
    }
    esp_core_dump_image_erase();

    logPrintf("\n==============================\n");
    logPrintf("  Gong Server v5.0 (WiFi AP + LoRa)\n");
    logPrintf("==============================\n");

    if (!LittleFS.begin(true)) {
        logPrintf("[MAIN] LittleFS init failed — halting\n");
        while (true) delay(1000);
    }

    onScheduleTrigger = onGongFire;

    rtc_setup();     // probe DS3231; if found, load time into system clock
    mp3_setup();
    mp3_startAudioTask();
    lora_setup();    // starts Core-0 radio task; degrades gracefully if module absent
    if (lora_usesDefaultKey())
        logPrintf("[SEC] !!! LORA_HMAC_KEY IS THE REPO DEFAULT — ANYONE WITH AN Ra-02 AND "
                  "THIS REPO CAN RING THE GONG. CHANGE IT IN config.h BEFORE DEPLOYMENT !!!\n");
    sched_setup();
    web_setup();     // starts local AP + HTTP server

    // M-9: a wedged loop() (e.g. stuck inside a web handler or audio.loop())
    // used to mean a dead server until someone found it — for hardware that
    // has to wake a building at 04:00 unattended, that silence is the whole
    // point of everything else in this file.
    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);

    logPrintf("[MAIN] All modules ready. Entering main loop.\n");
}

void loop() {
    esp_task_wdt_reset();
    web_loop();
    // Аудио — в отдельном таске (Core 1, приоритет 10)

    // H-1: local playback queued by loraTask once a GONG's TX actually
    // completes (see server/src/lorahandler.cpp).
    uint8_t lpTrack, lpVol, lpLoop;
    if (lora_pollLocalPlay(lpTrack, lpVol, lpLoop)) {
        mp3_setVolume(lpVol);
        mp3_play(lpTrack, lpLoop);
    }

    unsigned long now = millis();
    if (now - lastSchedCheck >= 1000) {
        sched_check();
        lastSchedCheck = now;
    }

    // M-7: lora_sendHeartbeat() now reports whether it actually got queued.
    // On failure, retry in 2s instead of waiting a full HEARTBEAT_INTERVAL_MS
    // — during which the CLIENT-side registry would otherwise time this
    // server out of every client's view for nothing.
    if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
        if (lora_sendHeartbeat()) lastHeartbeat = now;
        else                      lastHeartbeat = now - HEARTBEAT_INTERVAL_MS + 2000;
    }

    // H-5: broadcast the active day's schedule promptly after it changes,
    // and otherwise once an hour as a safety net (e.g. a client that missed
    // the original broadcast while out of range).
    if (lora_isReady() && (sched_consumeChanged() || now - lastSchedBroadcast >= 3600000UL)) {
        int day = sched_getActiveDay();
        if (day >= 0) {
            SchedBin bin[SCHED_BIN_MAX];
            uint8_t  n = sched_activeBinSnapshot(bin, SCHED_BIN_MAX);
            lora_broadcastSchedule((uint8_t)day, bin, n);
        }
        lastSchedBroadcast = now;
    }

    // M10: this loop() never blocks on its own — web_loop() returns
    // immediately when no client is connected, and everything else above is
    // a millis() check. Arduino-ESP32's loopTask carries no automatic yield,
    // so with nothing here ever ceding the CPU, the Core-1 IDLE task (and
    // its watchdog check) could starve for as long as the device sits with
    // no HTTP traffic — which for hardware meant to ring unattended at 04:00
    // is most of the night. audio itself is unaffected: it runs on its own
    // higher-priority task (audioFeederTask), not inside this loop.
    vTaskDelay(pdMS_TO_TICKS(1));
}
