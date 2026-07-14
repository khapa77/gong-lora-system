#include "mp3handler.h"
#include "config.h"
#include <SPIFFS.h>
#include <Audio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static Audio    audio;
static uint8_t  curVol     = DEFAULT_VOLUME;
static bool     ramping    = false;
static uint32_t rampStart  = 0;
static uint8_t  loopTrack  = 0;
static uint8_t  loopRemain = 0;

// ── The Audio object is NOT thread-safe ───────────────────────────────────
// It is touched from two Core-1 tasks: audioFeederTask (priority 24, calls
// mp3_loop() every ms) and loopTask (web handlers / scheduler call mp3_play,
// mp3_stop, mp3_setVolume). The priority-24 task can preempt loopTask in the
// middle of connecttoFS()/stopSong() and then run audio.loop() on
// half-initialised decoder state — a classic source of rare heap crashes.
// Every public entry point therefore serialises access through audioMtx.
// FreeRTOS mutexes use priority inheritance, so the feeder blocking here
// briefly boosts loopTask instead of stalling audio.
static SemaphoreHandle_t audioMtx = nullptr;

static inline bool audioLock() {
    return audioMtx && xSemaphoreTake(audioMtx, portMAX_DELAY) == pdTRUE;
}
static inline void audioUnlock() { xSemaphoreGive(audioMtx); }

// ESP32-audioI2S volume 0–21; we use 0–30 externally
static void applyVolume() {
    uint8_t v = (curVol <= 30) ? (curVol * 21 / 30) : 21;
    audio.setVolume(v);
}

// Caller must hold audioMtx.
static void _startPlay(uint8_t track) {
    if (audio.isRunning()) audio.stopSong();
    ramping = false;

    char path[16];
    snprintf(path, sizeof(path), "/%04d.mp3", track);

    if (!SPIFFS.exists(path)) {
        logPrintf("[MP3] File not found: %s\n", path);
        loopRemain = 0;
        return;
    }

    audio.setVolume(0);
    if (audio.connecttoFS(SPIFFS, path)) {
        ramping   = true;
        rampStart = millis();
        logPrintf("[MP3] Playing: %s\n", path);
    } else {
        logPrintf("[MP3] Failed to start: %s\n", path);
        loopRemain = 0;
    }
}

void mp3_setup() {
    logPrintf("[MP3] Initializing I2S (ESP32-audioI2S, MAX98357A)...\n");

    // Mutex is created before any task that could call into this module.
    audioMtx = xSemaphoreCreateMutex();

    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    applyVolume();

    logPrintf("[MP3] Ready — BCLK=%d LRC=%d DOUT=%d volume=%d/30\n",
                  I2S_BCLK, I2S_LRC, I2S_DOUT, curVol);
}

void mp3_setVolume(uint8_t vol) {
    if (vol > 30) vol = 30;
    if (!audioLock()) return;
    curVol = vol;
    if (!ramping) applyVolume();
    audioUnlock();
    logPrintf("[MP3] Volume = %d/30\n", vol);
}

uint8_t mp3_getVolume() {
    return curVol;
}

void mp3_play(uint8_t track, uint8_t loops) {
    if (loops < 1) loops = 1;
    logPrintf("[MP3] mp3_play track=%d loops=%d\n", track, loops);
    if (!audioLock()) return;
    loopTrack  = track;
    loopRemain = loops - 1;  // first play counts as one
    _startPlay(track);
    audioUnlock();
}

void mp3_stop() {
    if (!audioLock()) return;
    if (audio.isRunning()) {
        audio.stopSong();
    }
    ramping    = false;
    loopRemain = 0;
    audioUnlock();
}

void mp3_loop() {
    if (!audioLock()) return;
    audio.loop();

    if (ramping) {
        if (millis() - rampStart >= 40) {
            applyVolume();
            ramping = false;
        }
    } else {
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
                logPrintf("[MP3] Loop repeat — remaining=%d\n", loopRemain);
                _startPlay(loopTrack);
            } else {
                audio.setVolume(0);
            }
        }
    }
    audioUnlock();
}

bool mp3_isPlaying() {
    if (!audioLock()) return false;
    bool r = audio.isRunning();
    audioUnlock();
    return r;
}

String mp3_listTracksJSON() {
    String s = "[";
    bool first = true;
    File root = SPIFFS.open("/");
    File f = root.openNextFile();
    while (f) {
        String name = f.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (name.length() == 8 && name.endsWith(".mp3")) {
            bool numeric = true;
            for (int i = 0; i < 4; i++) if (!isDigit(name[i])) { numeric = false; break; }
            if (numeric) {
                if (!first) s += ",";
                s += String(name.substring(0, 4).toInt());
                first = false;
            }
        }
        f = root.openNextFile();
    }
    s += "]";
    return s;
}

// ────────────────────────────────────────────────────────────────
// Dedicated audio feeder task (Core 1, high priority)
// ────────────────────────────────────────────────────────────────
static TaskHandle_t audioTaskHandle = nullptr;

static void audioFeederTask(void*) {
    for (;;) {
        mp3_loop();
        vTaskDelay(pdMS_TO_TICKS(1));  // ~1 kHz feed rate
    }
}

void mp3_startAudioTask() {
    if (audioTaskHandle) return;
    // Stack 8192: MP3 decoding happens INSIDE audio.loop() in this library
    // and 4096 was borderline (typical working sizes are 5–8 KB). Priority
    // configMAX_PRIORITIES-1 (24) is safe here: Wi-Fi tasks live on Core 0,
    // and this task yields every 1 ms tick, so nothing on Core 1 starves.
    xTaskCreatePinnedToCore(audioFeederTask, "audio_feed", 8192, nullptr,
                            configMAX_PRIORITIES - 1, &audioTaskHandle, 1);
    Serial.println("[MP3] Audio feeder task started on Core 1 (priority 24)");
}
