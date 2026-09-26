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
#include "relayhandler.h"
#include "wifihandler.h"

static unsigned long lastSchedCheck = 0;

// -------------------------------------------------------
// Called when a schedule entry fires. Goes through the relay module: in AUTO
// mode it powers the external amplifier first and starts the track once the
// amp has had RELAY pre-delay to settle.
// -------------------------------------------------------
static void onGongFire(uint8_t track, uint8_t loop, uint8_t vol) {
    logPrintf("[MAIN] Schedule fired: track=%d loop=%d vol=%d\n", track, loop, vol);
    relay_play(track, vol, loop);
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
    logPrintf("  Gong Server v" FW_VERSION " (standalone: WiFi + relay)\n");
    logPrintf("==============================\n");

    if (!LittleFS.begin(true)) {
        logPrintf("[MAIN] LittleFS init failed — halting\n");
        while (true) delay(1000);
    }

    onScheduleTrigger = onGongFire;

    rtc_setup();     // probe DS3231; if found, load time into system clock
    mp3_setup();
    mp3_startAudioTask();
    relay_setup();   // before sched_setup(): a catch-up gong may fire from it
    sched_setup();
    wifi_setup();    // own AP always + STA if a network is saved
    web_setup();     // HTTP server

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

    wifi_loop();
    relay_loop();

    unsigned long now = millis();
    if (now - lastSchedCheck >= 1000) {
        sched_check();
        lastSchedCheck = now;
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
