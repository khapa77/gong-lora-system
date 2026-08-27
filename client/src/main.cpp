#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include "esp32-hal-bt.h"
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "mp3handler.h"
#include "lorahandler.h"

static void blinkReady() {
    if (STATUS_LED < 0) return;
    for (int i = 0; i < 4; i++) {
        digitalWrite(STATUS_LED, HIGH); delay(150);
        digitalWrite(STATUS_LED, LOW);  delay(150);
    }
}

// H-11: non-blocking LED state machine — without a screen, network, or
// buttons, this is the only way to tell "is this client alive, is it
// hearing the server" without plugging a laptop into it. See
// 01_AUDIT_REPORT.md 1.2.
//   - solid          : playing a track right now
//   - fast blink 5Hz : radio never came up (check wiring)
//   - double blink/3s: heartbeat lost (> HEARTBEAT_LOST_MS) — running autonomous fallback
//   - single blink/3s: normal, heartbeat fresh
static void updateStatusLed() {
    if (STATUS_LED < 0) return;

    if (mp3_isPlaying()) { digitalWrite(STATUS_LED, HIGH); return; }

    if (!lora_isReady()) {
        bool on = (millis() / 100) % 2 == 0;   // 5 Hz
        digitalWrite(STATUS_LED, on ? HIGH : LOW);
        return;
    }

    uint32_t cyclePos = millis() % 3000UL;
    bool on = lora_heartbeatLost()
        ? (cyclePos < 100 || (cyclePos >= 250 && cyclePos < 350))   // double blink
        : (cyclePos < 100);                                        // single blink
    digitalWrite(STATUS_LED, on ? HIGH : LOW);
}

static unsigned long lastAutoTick = 0;

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n==============================");
    Serial.println("  Gong LoRa CLIENT");
    Serial.println("==============================");

    if (STATUS_LED >= 0) {
        pinMode(STATUS_LED, OUTPUT);
        digitalWrite(STATUS_LED, LOW);
    }

    // WiFi/BT are never used on the client — stop them explicitly so they
    // don't add RF noise right next to the 433MHz receiver, or draw power
    // for nothing (Low, 01_AUDIT_REPORT.md §5).
    WiFi.mode(WIFI_OFF);
    btStop();

    if (!LittleFS.begin(true)) {
        Serial.println("[MAIN] LittleFS init failed — halting");
        while (true) delay(1000);
    }

    mp3_setup();
    mp3_startAudioTask();
    lora_setup();

    // M-9: a wedged loop() or audio.loop() used to mean a dead device until
    // someone physically power-cycled it — critical for hardware that has to
    // wake people at 04:00 unattended.
    esp_task_wdt_init(30, true);
    esp_task_wdt_add(NULL);

    blinkReady();
    Serial.println("[MAIN] Listening for server commands...");
}

void loop() {
    esp_task_wdt_reset();
    lora_poll();   // drains Core 0 → Core 1 command queue

    unsigned long now = millis();
    if (now - lastAutoTick >= 1000) {
        lora_autonomousTick();   // H-5: fallback schedule once the server's been silent too long
        lastAutoTick = now;
    }
    updateStatusLed();
    // Audio fed by dedicated FreeRTOS task (mp3_startAudioTask)

    // M10 (code_review.md): nothing above ever blocks, so with no yield here
    // the Core-1 IDLE task could starve indefinitely — same reasoning as the
    // server's loop(), see server/src/main.cpp.
    vTaskDelay(pdMS_TO_TICKS(1));
}
