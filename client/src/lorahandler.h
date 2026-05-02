#pragma once
#include <Arduino.h>

void lora_setup();
void lora_loop();
void lora_sendACK(int rxRssi);
