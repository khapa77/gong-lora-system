#include "mp3handler.h"
#include "config.h"
#include <SPIFFS.h>
#include <Audio.h>

static Audio audio;
static uint8_t curVol = DEFAULT_VOLUME;

// ESP32-audioI2S volume 0–21; мы используем 0–30 снаружи
static void applyVolume() {
    uint8_t v = (curVol <= 30) ? (curVol * 21 / 30) : 21;
    audio.setVolume(v);
}

void mp3_setup() {
    Serial.println("[MP3] Initializing I2S (ESP32-audioI2S, MAX98357A)...");

    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    applyVolume();

    Serial.printf("[MP3] Ready — BCLK=%d LRC=%d DOUT=%d volume=%d/30\n",
                  I2S_BCLK, I2S_LRC, I2S_DOUT, curVol);
}

void mp3_setVolume(uint8_t vol) {
    if (vol > 30) vol = 30;
    curVol = vol;
    applyVolume();
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

    applyVolume();  // восстановить громкость перед воспроизведением (после молчания в простое)
    if (audio.connecttoFS(SPIFFS, path)) {
        Serial.printf("[MP3] Playing: %s\n", path);
    } else {
        Serial.printf("[MP3] Failed to start: %s\n", path);
    }
}

void mp3_stop() {
    if (audio.isRunning()) {
        audio.stopSong();
    }
}

void mp3_loop() {
    audio.loop();
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
