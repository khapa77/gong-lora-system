#pragma once

#include <Arduino.h>

void     lora_setup();
bool     lora_isReady();     // true once radio.begin() succeeded and loraTask is running

void     lora_sendGong(uint8_t track, uint8_t vol, uint8_t loop = 1);
void     lora_sendStop();
void     lora_sendHeartbeat();

String   lora_clientsJSON();
int      lora_clientCount();
uint32_t lora_getAvgOneWayMs();
