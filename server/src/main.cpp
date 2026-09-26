#include <Arduino.h>
#include <LittleFS.h>
#include <esp_core_dump.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "sysstate.h"
#include "mp3handler.h"
#include "schedule.h"
#include "webhandler.h"
#include "timesync.h"
#include "relayhandler.h"
#include "wifihandler.h"
#include "buttonhandler.h"
#include "statusled.h"

// -------------------------------------------------------
// Called when a schedule entry fires. Goes through the relay module: in AUTO
// mode it powers the external amplifier first and starts the track once the
// amp has had RELAY pre-delay to settle.
// -------------------------------------------------------
static void onGongFire(uint8_t track, uint8_t loop, uint8_t vol) {
    logPrintf("[MAIN] Schedule fired: track=%d loop=%d vol=%d\n", track, loop, vol);
    relay_play(track, vol, loop);
}

// -------------------------------------------------------
// controlTask — everything that has to happen on time: schedule, button,
// relay timing, clock sync, WiFi reconnects, status LED. It used to share
// loop() with the web server, and WebServer::handleClient() waits up to 5 s
// for a slow or silent client — per request — during which none of this ran.
// Now HTTP stays in loopTask; state shared with web handlers is guarded by
// sys_lock() (see sysstate.h), which handlers hold only while they work.
//
// Priority 2: above loopTask (1, web) so a busy web server can't delay it,
// well below audio_feed (10). Stack 8 KB: sched_activateDay()/sched_toJSON()
// build Strings and logPrintf keeps a 256-byte buffer on the stack; the big
// JSON pools are on the heap.
// -------------------------------------------------------
#define CONTROL_TASK_PRIORITY 2
#define CONTROL_TASK_STACK    8192
#define CONTROL_PERIOD_MS     10     // button debounce is 50 ms, relay poll 50 ms

static void controlTask(void*) {
    esp_task_wdt_add(nullptr);   // M-9: a wedged schedule loop must reboot, not sit silent
    uint32_t lastSchedCheck = 0;
    for (;;) {
        esp_task_wdt_reset();
        sys_lock();
        wifi_loop();
        time_loop();
        button_loop();
        relay_loop();
        led_loop();
        uint32_t now = millis();
        if (now - lastSchedCheck >= 1000) {
            sched_check();
            lastSchedCheck = now;
        }
        sys_unlock();
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    logbuffer_init();  // capture logs for web debug (must precede logPrintf below)
    sys_setup();       // state mutex, reset reason, crash counter

    // If the previous run crashed, a core dump sits in the coredump partition.
    // Log its presence BEFORE erasing (the reset reason above is kept in
    // /api/status too).
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

    // LittleFS.begin(true) used to FORMAT the partition on any mount failure —
    // wiping the MP3s and every schedule, after which the device booted fine
    // and simply never rang. It also halted forever (no watchdog yet) if even
    // that failed. Now: a few retries, never format, and keep running without
    // a filesystem so the AP + web UI (compiled into the firmware) can show
    // the problem; the status LED blinks fast. `pio run -t uploadfs` restores.
    bool fsOk = false;
    for (int i = 1; i <= 3 && !fsOk; i++) {
        fsOk = LittleFS.begin(false);
        if (!fsOk) {
            logPrintf("[MAIN] LittleFS mount failed (attempt %d/3)\n", i);
            delay(500);
        }
    }
    sys_setFsOk(fsOk);
    if (!fsOk)
        logPrintf("[MAIN] ERROR: no filesystem — NO SOUND, NO SCHEDULE. Not formatting "
                  "(that would erase whatever is left). Re-upload with: pio run -t uploadfs\n");

    led_setup();
    button_setup();
    // Admin password recovery without reflashing (see AUTH_RESET_HOLD_MS).
    if (button_resetHeldAtBoot(AUTH_RESET_HOLD_MS)) web_resetAuth();

    onScheduleTrigger = onGongFire;

    time_setup();    // TZ + DS3231 → system clock (NTP starts once STA is up)
    mp3_setup();
    mp3_startAudioTask();
    relay_setup();   // before sched_setup(): a catch-up gong may fire from it
    sched_setup();
    wifi_setup();    // own AP always + STA if a network is saved
    web_setup();     // HTTP server

    // M-9: both tasks are watched — a web handler stuck in loopTask and a
    // wedged controlTask each reboot the board after 30 s.
    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);
    xTaskCreatePinnedToCore(controlTask, "control", CONTROL_TASK_STACK, nullptr,
                            CONTROL_TASK_PRIORITY, nullptr, 1);

    logPrintf("[MAIN] All modules ready. Entering main loop.\n");
}

void loop() {
    esp_task_wdt_reset();
    web_loop();
    // M10: Arduino-ESP32's loopTask carries no automatic yield — without this
    // the Core-1 IDLE task (and its watchdog check) could starve while no
    // HTTP client is connected.
    vTaskDelay(pdMS_TO_TICKS(1));
}
