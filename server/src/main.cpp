#include <Arduino.h>
#include <SPIFFS.h>
#include <esp_core_dump.h>
#include "config.h"
#include "mp3handler.h"
#include "schedule.h"
#include "webhandler.h"
#include "rtchandler.h"
#include "lorahandler.h"

static unsigned long lastSchedCheck = 0;
static unsigned long lastHeartbeat  = 0;

// -------------------------------------------------------
// Called when a schedule entry fires — broadcasts over LoRa first (blocks
// briefly until TX completes), then plays locally, so both sides start
// audio together.
// -------------------------------------------------------
static void onGongFire(uint8_t track, uint8_t loop) {
    logPrintf("[MAIN] Schedule fired: track=%d loop=%d\n", track, loop);
    lora_sendGong(track, DEFAULT_VOLUME, loop);
    mp3_setVolume(DEFAULT_VOLUME);
    mp3_play(track, loop);
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
    logPrintf("  Gong Server v4.0 (WiFi + LoRa)\n");
    logPrintf("==============================\n");

    if (!SPIFFS.begin(true)) {
        logPrintf("[MAIN] SPIFFS init failed — halting\n");
        while (true) delay(1000);
    }

    onScheduleTrigger = onGongFire;

    rtc_setup();     // probe DS3231; if found, load time into system clock
    mp3_setup();
    mp3_startAudioTask();
    lora_setup();    // starts Core-0 radio task; degrades gracefully if module absent
    sched_setup();
    web_setup();     // starts local AP + HTTP server

    logPrintf("[MAIN] All modules ready. Entering main loop.\n");
}

void loop() {
    web_loop();
    // Аудио — в отдельном таске (Core 1, приоритет 24)

    unsigned long now = millis();
    if (now - lastSchedCheck >= 1000) {
        sched_check();
        lastSchedCheck = now;
    }
    if (now - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
        lora_sendHeartbeat();
        lastHeartbeat = now;
    }
}
