#include <Arduino.h>
#include "config.h"
#include "lorahandler.h"
#include "mp3handler.h"

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

    mp3_setup();
    lora_setup();

    blinkReady();
    Serial.println("[MAIN] Listening for server commands...");
}

void loop() {
    lora_loop();
    mp3_loop();
    delay(5);
}
