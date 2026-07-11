#include <Arduino.h>
#include <SPIFFS.h>
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

void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println("\n==============================");
    Serial.printf("  Gong LoRa CLIENT [%s]\n", CLIENT_ID);
    Serial.println("==============================");

    if (STATUS_LED >= 0) {
        pinMode(STATUS_LED, OUTPUT);
        digitalWrite(STATUS_LED, LOW);
    }

    if (!SPIFFS.begin(true)) {
        Serial.println("[MAIN] SPIFFS init failed — halting");
        while (true) delay(1000);
    }

    mp3_setup();
    mp3_startAudioTask();
    lora_setup();

    blinkReady();
    Serial.println("[MAIN] Listening for server commands...");
}

void loop() {
    lora_poll();   // drains Core 0 → Core 1 command queue
    // Audio fed by dedicated FreeRTOS task (mp3_startAudioTask)
}
