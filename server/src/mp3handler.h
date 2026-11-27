#pragma once

#include <Arduino.h>
#include "config.h"

// ESP32-audioI2S (schreibfaul1) — I2S output, MP3 from SPIFFS
// Пины I2S для MAX98357A в config.h

void    mp3_setup();
void    mp3_loop();
void    mp3_startAudioTask();

// track=1 → /0001.mp3, track=2 → /0002.mp3
void    mp3_play(uint8_t track, uint8_t loops = 1);  // loops=1 → play once

// Громкость + трек одним вызовом — общий вход для расписания, кнопки и веба.
void    mp3_start(uint8_t track, uint8_t vol, uint8_t loops);

// ── Единственный путь ручного воспроизведения ──────────────────────────────
// Его вызывают И /api/play, И физическая кнопка: правила проверки и
// ограничения заданы в одном месте, поэтому «как из веба» и «как кнопкой»
// не могут разойтись. Проверка: track 1..99, vol 0..30, loop 1..LOOP_MAX.
// false — запрос отклонён (трек вне диапазона), воспроизведение не начато.
bool    mp3_manualPlay(int track, int vol, int loop);

// Чистые хелперы того же контракта (host-тестируемые, без побочных эффектов).
inline bool mp3_manualTrackValid(int track) { return track >= 1 && track <= 99; }
inline uint8_t mp3_manualVolume(int vol) { return (uint8_t)constrain(vol, 0, 30); }
inline uint8_t mp3_manualLoops(int loop) { return (uint8_t)constrain(loop, 1, LOOP_MAX); }

void    mp3_stop();
bool    mp3_trackExists(uint8_t track);   // /NNNN.mp3 present on LittleFS
void    mp3_setVolume(uint8_t vol);   // 0–30 (внутри маппится на 0–21)
uint8_t mp3_getVolume();
bool    mp3_isPlaying();

// JSON array of track numbers that actually exist on SPIFFS (e.g. [1,2]),
// scanned from /NNNN.mp3 filenames.
String  mp3_listTracksJSON();
