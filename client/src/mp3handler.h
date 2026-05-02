#pragma once
#include <Arduino.h>

// ESP32-audioI2S (schreibfaul1) — I2S output, MP3 from SPIFFS
// I2S pins for MAX98357A in config.h

void    mp3_setup();
void    mp3_loop();

// track=1 → /0001.mp3, track=2 → /0002.mp3
void    mp3_play(uint8_t track, uint8_t loops = 1);  // loops=1 → play once

void    mp3_stop();
void    mp3_setVolume(uint8_t vol);   // 0–30 (mapped to 0–21 internally)
uint8_t mp3_getVolume();
bool    mp3_isPlaying();
