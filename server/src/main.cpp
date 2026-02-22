#include <Arduino.h>
#include <SPIFFS.h>
#include "config.h"
#include "mp3handler.h"
#include "schedule.h"
#include "lorahandler.h"
#include "webhandler.h"

static unsigned long lastHeartbeat    = 0;
static unsigned long lastSchedCheck   = 0;

// -------------------------------------------------------
// Called when a schedule entry fires
// Plays locally AND broadcasts to all LoRa clients
// -------------------------------------------------------
static void onGongFire(uint8_t track) {
    Serial.printf("[MAIN] Schedule fired: track=%d\n", track);
    mp3_setVolume(DEFAULT_VOLUME);
    mp3_play(track);
    lora_sendGong(track, DEFAULT_VOLUME);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n==============================");
    Serial.println("  Gong LoRa SERVER v2.0");
    Serial.println("==============================");

    if (!SPIFFS.begin(true)) {
        Serial.println("[MAIN] SPIFFS init failed — halting");
        while (true) delay(1000);
    }

    onScheduleTrigger = onGongFire;

    mp3_setup();
    lora_setup();
    sched_setup();
    web_setup();     // connects WiFi, starts HTTP server

    Serial.println("[MAIN] All modules ready. Entering main loop.");
}

void loop() {
    web_loop();
    lora_loop();
    mp3_loop();

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
