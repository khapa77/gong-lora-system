#include "mp3handler.h"
#include "config.h"
#include <SPIFFS.h>
#include <Audio.h>

static Audio    audio;
static uint8_t  curVol    = DEFAULT_VOLUME;
static bool     ramping   = false;
static uint32_t rampStart = 0;

// ESP32-audioI2S volume 0–21; we use 0–30 externally
static void applyVolume() {
    uint8_t v = (curVol <= 30) ? (curVol * 21 / 30) : 21;
    audio.setVolume(v);
}

void mp3_setup() {
    Serial.println("[MP3] Initializing I2S (ESP32-audioI2S, MAX98357A)...");

    if (!SPIFFS.begin(true)) {
        Serial.println("[MP3] SPIFFS mount failed!");
        return;
    }

    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    applyVolume();

    Serial.printf("[MP3] Ready — BCLK=%d LRC=%d DOUT=%d volume=%d/30\n",
                  I2S_BCLK, I2S_LRC, I2S_DOUT, curVol);
}

void mp3_setVolume(uint8_t vol) {
    if (vol > 30) vol = 30;
    curVol = vol;
    if (!ramping) applyVolume();
    Serial.printf("[MP3] Volume = %d/30\n", curVol);
}

uint8_t mp3_getVolume() {
    return curVol;
}

void mp3_play(uint8_t track) {
    mp3_stop();

    char path[16];
    snprintf(path, sizeof(path), "/%04d.mp3", track);

    if (!SPIFFS.exists(path)) {
        Serial.printf("[MP3] File not found: %s\n", path);
        return;
    }

    // Start silent, ramp up after I2S bus has settled (~40 ms)
    audio.setVolume(0);
    if (audio.connecttoFS(SPIFFS, path)) {
        ramping   = true;
        rampStart = millis();
        Serial.printf("[MP3] Playing: %s\n", path);
    } else {
        Serial.printf("[MP3] Failed to start: %s\n", path);
    }
}

void mp3_stop() {
    if (audio.isRunning()) {
        audio.stopSong();
    }
    ramping = false;
}

void mp3_loop() {
    audio.loop();

    if (ramping) {
        if (millis() - rampStart >= 40) {
            applyVolume();
            ramping = false;
        }
        return;
    }

    // Mute only after sustained silence to avoid glitches between DMA chunks
    static uint8_t idleCount = 0;
    if (audio.isRunning()) {
        idleCount = 0;
    } else if (idleCount < 20) {
        idleCount++;
    } else {
        audio.setVolume(0);
    }
}

bool mp3_isPlaying() {
    return audio.isRunning();
}
