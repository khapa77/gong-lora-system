#include "mp3handler.h"
#include "config.h"
#include <SPIFFS.h>
#include <Audio.h>

static Audio    audio;
static uint8_t  curVol    = DEFAULT_VOLUME;
static bool     ramping   = false;
static uint32_t rampStart = 0;
static uint8_t  loopTrack  = 0;
static uint8_t  loopRemain = 0;

// ESP32-audioI2S volume 0–21; we use 0–30 externally
static void applyVolume() {
    uint8_t v = (curVol <= 30) ? (curVol * 21 / 30) : 21;
    audio.setVolume(v);
}

static void _startPlay(uint8_t track) {
    if (audio.isRunning()) audio.stopSong();
    ramping = false;

    char path[16];
    snprintf(path, sizeof(path), "/%04d.mp3", track);

    if (!SPIFFS.exists(path)) {
        Serial.printf("[MP3] File not found: %s\n", path);
        loopRemain = 0;
        return;
    }

    audio.setVolume(0);
    if (audio.connecttoFS(SPIFFS, path)) {
        ramping   = true;
        rampStart = millis();
        Serial.printf("[MP3] Playing: %s\n", path);
    } else {
        Serial.printf("[MP3] Failed to start: %s\n", path);
        loopRemain = 0;
    }
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
    if (!ramping) applyVolume();
    Serial.printf("[MP3] Volume = %d/30\n", curVol);
}

uint8_t mp3_getVolume() {
    return curVol;
}

void mp3_play(uint8_t track, uint8_t loops) {
    if (loops < 1) loops = 1;
    loopTrack  = track;
    loopRemain = loops - 1;  // first play counts as one
    Serial.printf("[MP3] mp3_play track=%d loops=%d\n", track, loops);
    _startPlay(track);
}

void mp3_stop() {
    if (audio.isRunning()) {
        audio.stopSong();
    }
    ramping    = false;
    loopRemain = 0;
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
        // Track finished — check if we need to repeat
        if (loopRemain > 0) {
            loopRemain--;
            idleCount = 0;
            Serial.printf("[MP3] Loop repeat — remaining=%d\n", loopRemain);
            _startPlay(loopTrack);
        } else {
            audio.setVolume(0);
        }
    }
}

bool mp3_isPlaying() {
    return audio.isRunning();
}
