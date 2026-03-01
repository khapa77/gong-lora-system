#pragma once
#include <Arduino.h>

void mp3_setup();
void mp3_play(uint8_t track);
void mp3_stop();
void mp3_setVolume(uint8_t vol);
bool mp3_isBusy();
void mp3_loop();

