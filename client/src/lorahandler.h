#pragma once
#include <Arduino.h>

void lora_setup();
void lora_poll();   // call from Core 1 loop() — drains RX queue, handles audio