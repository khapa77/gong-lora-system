#pragma once

#include <Arduino.h>

void lora_setup();
bool lora_isReady();
void lora_poll();   // call every loop() from Core 1 — drains the GONG/STOP command queue
