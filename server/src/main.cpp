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
#include "wifihandler.h"
#include "device.h"
#include "buttonhandler.h"
#include "statusled.h"
#include "relayhandler.h"

// -------------------------------------------------------
// Physical button and status LED ports.
//
// The button never calls into mp3_*/web_*/schedule_* itself: these three
// functions are the whole surface it needs, so the button logic stays
// host-testable and the manual-playback rules live in exactly one place
// (mp3_manualPlay, shared with /api/play).
// -------------------------------------------------------
static bool buttonAudioPlaying() { return mp3_isPlaying(); }

// "Busy" для кнопки — звук ИЛИ ожидание прогрева реле: в обоих случаях
// нажатие означает STOP, а не новое действие (Q7).
static bool buttonBusy() { return relay_isBusy(); }

static void buttonStopAudio() { relay_stop(); }

// Переключение реле без звука (удержание >= 8 с). false — режим не AUTO.
static bool buttonRelayToggle() { return relay_setManual(!relay_isManual()); }

static void buttonPlayManual(uint8_t track, uint8_t vol, uint8_t loop) {
    // Тот же путь, что /api/play: никаких собственных проверок поверх
    // mp3_manualPlay(), иначе кнопка и веб разойдутся (например, когда файла
    // запрошенного трека нет, а _startPlay() играет DEFAULT_TRACK).
    if (!mp3_manualPlay(track, vol, loop))
        logPrintf("[BTN] Manual gong: request rejected (track %u)\n", (unsigned)track);
}

// What "the schedule cannot run" means on this architecture. Deliberately does
// NOT include "no Wi-Fi": the device rings from DS3231/NTP-corrected time and
// LittleFS alone. `waiting` (no course published) and `APPLIED` (published but
// not activated) both mean nothing will ring, and sched_isRunning() is the one
// predicate that covers them together with file errors and emergency.
static bool ledProblem() {
    return !timeIsSet() || !sys_fsOk() || sched_hasError() || !sched_isRunning();
}

// -------------------------------------------------------
// Called when a schedule entry fires (normal course or emergency). Goes through
// the relay module: in AUTO it powers the external amplifier first and starts
// the track once the relay has had its warm-up. MAX98357A is powered all the
// time, so the gong itself never depends on the relay.
// -------------------------------------------------------
static void onGongFire(uint8_t track, uint8_t loop, uint8_t vol) {
    logPrintf("[MAIN] Schedule fired: track=%d loop=%d vol=%d\n", track, loop, vol);
    relay_play(track, vol, loop);
}

// -------------------------------------------------------
// controlTask — everything that has to happen on time: schedule, clock
// sync, WiFi reconnects. It used to share
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
#define CONTROL_PERIOD_MS     10

static void controlTask(void*) {
    esp_task_wdt_add(nullptr);   // M-9: a wedged schedule loop must reboot, not sit silent
    uint32_t lastSchedCheck = 0;
    for (;;) {
        esp_task_wdt_reset();
        sys_lock();
        wifi_loop();
        time_loop();
        // Polled before sched_check(): a press at an event minute must not be
        // starved by the scheduler's own work, and the LED hint has to follow
        // the button within one 10 ms tick.
        button_loop();
        uint32_t now = millis();
        if (now - lastSchedCheck >= 1000) {
            sched_check();
            // Pre-warm: the relay closes preMs before the next event so the
            // external amplifier is ready and the sound starts exactly on time.
            // +1 s covers the once-per-second clock granularity. The answer is
            // recomputed from the live entries, so day transitions, cancelled
            // events, Apply/Activate, emergency and clock corrections are all
            // handled by sched_msToNext() itself.
            const long toGong = sched_msToNext();
            if (toGong > 0 && (uint32_t)toGong <= relay_preMs() + 1000UL)
                relay_prewarm((uint32_t)toGong);
            lastSchedCheck = now;
        }
        relay_loop();
        led_loop();
        sys_unlock();
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

void setup() {
    // The physical button is sampled FIRST, before Serial, the core-dump check
    // and the LittleFS mount: the 10 s recovery hold is counted from power-on,
    // not from the end of initialisation, and the pin must be configured
    // (INPUT_PULLUP) before the first digitalRead().
    button_bootProbe();

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
    logPrintf("  Gong Server v" FW_VERSION " (standalone: WiFi + MAX98357A)\n");
    logPrintf("==============================\n");

    // LittleFS.begin(true) used to FORMAT the partition on any mount failure —
    // wiping the MP3s and every schedule, after which the device booted fine
    // and simply never rang. It also halted forever (no watchdog yet) if even
    // that failed. Now: a few retries, never format, and keep running without
    // a filesystem so the AP + web UI (compiled into the firmware) can show
    // the problem (red banner). `pio run -t uploadfs` restores.
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

    // ── Status LED + power-on recovery hold ────────────────────────────────
    // The pin was already sampled in the first line of setup(); the gate runs
    // here because the reset deletes /auth.conf and needs a mounted LittleFS.
    led_setup();
    if (button_bootHeldAtPowerOn()) {
        bool resetDone = false;
        const uint32_t gateStart = millis();
        led_set(true);                 // AUTH_RESET indication: steady light
        for (;;) {
            const BootGate gate = button_bootTick();
            if (gate == BootGate::ResetGrant && !resetDone) {
                // true only if the credentials really went away; the result is
                // reported instead of assuming success.
                resetDone = web_resetAuth();
            }
            if (gate == BootGate::Settled) break;
            // A contact stuck closed must not hold the device in setup()
            // forever: after BUTTON_STUCK_MS the boot continues and
            // button_setup() arms the button "already fired" until it is
            // released, so the stuck press cannot ring a gong.
            if ((uint32_t)(millis() - gateStart) >= BUTTON_STUCK_MS) {
                logPrintf("[BTN] Still held after %lus — booting anyway, button waits for release\n",
                          (unsigned long)(BUTTON_STUCK_MS / 1000));
                break;
            }
            // The task watchdog is armed later in setup(); feeding it before
            // that is an error return, not a feed. Only feed when registered.
            if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(CONTROL_PERIOD_MS));
        }
        led_set(false);
        logPrintf("[BTN] Power-on hold finished: %s\n",
                  resetDone ? "admin password reset, web UI is open"
                            : "credentials NOT reset (released early or erase failed)");
    }

    const ButtonHooks buttonHooks = { buttonAudioPlaying, buttonBusy,
                                      buttonPlayManual, buttonStopAudio,
                                      buttonRelayToggle };
    button_setup(buttonHooks);
    led_setProblemSource(ledProblem);


    onScheduleTrigger = onGongFire;

    time_setup();    // TZ + DS3231 → system clock (NTP starts once STA is up)
    mp3_setup();
    mp3_startAudioTask();
    relay_setup();   // before sched_setup(): a catch-up gong may fire from it
    sched_setup();
    device_setup();  // number + label → AP name / mDNS host (before wifi_setup)
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

// Admin password recovery over USB (AUTH_RESET_CMD in config.h). Runs in
// loopTask, the same task as the web handlers that use the auth state.
static void serialCommands() {
    static char buf[24];
    static uint8_t len = 0;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            buf[len] = '\0';
            if (len && strcmp(buf, AUTH_RESET_CMD) == 0) {
                if (!web_resetAuth())
                    logPrintf("[MAIN] Serial reset-auth FAILED — credentials may survive\n");
            }
            else if (len) logPrintf("[MAIN] Unknown serial command '%s' (known: " AUTH_RESET_CMD ")\n", buf);
            len = 0;
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}

void loop() {
    esp_task_wdt_reset();
    web_loop();
    serialCommands();
    device_loop();   // restart after a device-number change, once the reply is out
    // M10: Arduino-ESP32's loopTask carries no automatic yield — without this
    // the Core-1 IDLE task (and its watchdog check) could starve while no
    // HTTP client is connected.
    vTaskDelay(pdMS_TO_TICKS(1));
}
